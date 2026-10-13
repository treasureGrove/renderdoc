"""RenderDoc MCP bridge extension.

Runs INSIDE qrenderdoc. Drop this file into the extensions directory
(``%APPDATA%/qrenderdoc/extensions`` on Windows, or the extensions folder under
RenderDoc's AppData location on other platforms) and restart / reload
extensions.

It opens a localhost TCP listener (default 127.0.0.1:39944, override with the
``RD_MCP_PORT`` environment variable) and executes tool requests on the UI
thread. Pair it with ``rd_mcp.py`` (same directory), a zero-dependency MCP
stdio server that AI clients (ZCode / Claude Code / Claude Desktop / ...) run
as a subprocess.

Protocol on the TCP socket is newline-delimited JSON:
    request  {"id": 1, "tool": "list_events", "args": {"max_depth": 4}}
    response {"id": 1, "ok": true, "result": {...}}
"""

import json
import os
import queue
import re
import socket
import sys
import tempfile
import threading
import contextlib

import renderdoc
import qrenderdoc

_PKG_DIR = os.path.dirname(os.path.abspath(__file__))
if _PKG_DIR not in sys.path:
    sys.path.insert(0, _PKG_DIR)

import rd_frame_stats

# NOTE: the `pyrenderdoc` CaptureContext object is injected into this module by
# qrenderdoc only after import, as the argument to register(). Use `register()`
# as the entry point - do not touch pyrenderdoc at import time.

HOST = "127.0.0.1"
PORT = int(os.environ.get("RD_MCP_PORT", "39944"))

rd = None  # set by register()

_reqq = queue.Queue()
_pending = {}
_pending_lock = threading.Lock()
_call_ids = [1]
_stop = threading.Event()
_server = None
_timer = None
_hotspot_dialog = None
_pass_timings_panel = None
_panel_cls = None
_timing_viewer = None
_last_flat = []

# GPUCounter name, and the pass_timings column it fills. None means duration.
_SAMPLE_COUNTERS = (
    ("EventGPUDuration", None),
    ("PSInvocations", "ps"),
    ("CSInvocations", "cs"),
    ("VSInvocations", "vs"),
    ("SamplesPassed", "samples"),
    ("IAPrimitives", "ia_primitives"),
)

_timing_cache = {
    "capture_key": None,
    "sampled": False,
    "duration_supported": False,
    "durations": None,
    "counters": None,
    "counter_columns": [],
    "error": None,
}

_SHADER_STAGES = (
    "Vertex",
    "Hull",
    "Domain",
    "Geometry",
    "Pixel",
    "Compute",
    "Task",
    "Mesh",
    "RayGen",
    "Intersection",
    "AnyHit",
    "ClosestHit",
    "Miss",
    "Callable",
)


def _null():
    return renderdoc.ResourceId.Null()


# ---------------------------------------------------------------------------
# tools (executed on the UI thread)
# ---------------------------------------------------------------------------

def _walk_tree(actions, depth, max_depth, lines, budget):
    for a in actions:
        if budget[0] <= 0:
            lines.append("... (truncated)")
            return
        budget[0] -= 1
        flags = int(a.flags)
        is_marker = (flags & int(renderdoc.ActionFlags.PushMarker)) != 0
        is_draw = (flags & int(renderdoc.ActionFlags.Drawcall)) != 0
        is_dispatch = (
            flags
            & (
                int(renderdoc.ActionFlags.Dispatch)
                | int(renderdoc.ActionFlags.MeshDispatch)
                | int(renderdoc.ActionFlags.DispatchRay)
            )
        ) != 0
        is_clear = (flags & int(renderdoc.ActionFlags.Clear)) != 0
        is_copy = (flags & (int(renderdoc.ActionFlags.Copy) | int(renderdoc.ActionFlags.Resolve))) != 0
        is_present = (flags & int(renderdoc.ActionFlags.Present)) != 0

        name = a.customName
        if not name:
            name = "event"

        tag = ""
        if is_draw:
            tag = " [draw %d idx, %d inst]" % (a.numIndices, a.numInstances)
        elif is_dispatch:
            try:
                tag = " [dispatch %dx%dx%d]" % (a.dispatchDimension[0], a.dispatchDimension[1], a.dispatchDimension[2])
            except Exception:
                tag = " [dispatch]"
        elif is_clear:
            tag = " [clear]"
        elif is_copy:
            tag = " [copy]"
        elif is_present:
            tag = " [present]"

        lines.append("%sEID %d: %s%s" % ("  " * depth, a.eventId, name, tag))

        if a.children:
            if depth < max_depth:
                _walk_tree(a.children, depth + 1, max_depth, lines, budget)
            else:
                lines.append("%s... (%d children, deeper events omitted)" % ("  " * (depth + 1), len(a.children)))


def _kind(flags):
    draw = int(renderdoc.ActionFlags.Drawcall)
    dispatch = (
        int(renderdoc.ActionFlags.Dispatch)
        | int(renderdoc.ActionFlags.MeshDispatch)
        | int(renderdoc.ActionFlags.DispatchRay)
    )
    if flags & draw:
        return "draw"
    if flags & dispatch:
        return "dispatch"
    if flags & int(renderdoc.ActionFlags.PushMarker):
        return "marker"
    if flags & int(renderdoc.ActionFlags.Clear):
        return "clear"
    if flags & (int(renderdoc.ActionFlags.Copy) | int(renderdoc.ActionFlags.Resolve)):
        return "copy"
    if flags & int(renderdoc.ActionFlags.Present):
        return "present"
    if flags & int(renderdoc.ActionFlags.MultiAction):
        return "marker"
    return "other"


def _actions_to_tree(actions):
    out = []
    indirect_flag = int(getattr(renderdoc.ActionFlags, "Indirect", 0) or 0)
    for a in actions:
        flags = int(a.flags)
        kind = _kind(flags)
        name = a.customName or kind
        node = {
            "eid": int(a.eventId),
            "name": str(name),
            "kind": kind,
            "num_indices": int(a.numIndices),
            "num_instances": int(a.numInstances),
            "indirect": bool(indirect_flag and (flags & indirect_flag)),
            "children": _actions_to_tree(a.children) if a.children else [],
        }
        if kind == "dispatch":
            try:
                d = a.dispatchDimension
                node["dispatch"] = (int(d[0]), int(d[1]), int(d[2]))
            except Exception:
                node["dispatch"] = (0, 0, 0)
        out.append(node)
    return out


def _require_capture():
    if rd is None or not rd.IsCaptureLoaded():
        return {"error": "no capture loaded"}
    return None


def _flat_actions():
    global _last_flat
    _last_flat = rd_frame_stats.flatten(_actions_to_tree(rd.CurRootActions()))
    return _last_flat


def _pass_events(pass_name, limit=None):
    durations = None
    counters = None
    columns = None
    if _cache_current():
        durations = _timing_cache["durations"]
        counters = _timing_cache["counters"] if _timing_cache["counters"] is not None else {}
        columns = _timing_cache["counter_columns"]
    return rd_frame_stats.pass_events(
        _last_flat,
        pass_name,
        durations=durations,
        counters=counters,
        counter_columns=columns,
        limit=limit,
    )


def _resource_name(rid):
    try:
        name = rd.GetResourceName(rid)
        if name:
            return str(name)
    except Exception:
        pass
    return str(rid)


def _texture_rows():
    rows = []
    for tex in rd.GetTextures():
        fmt = ""
        try:
            fmt = str(tex.format.Name())
        except Exception:
            pass
        rows.append(
            {
                "name": _resource_name(tex.resourceId),
                "width": int(tex.width),
                "height": int(tex.height),
                "depth": int(tex.depth),
                "mips": int(tex.mips),
                "arraysize": int(tex.arraysize),
                "format": fmt,
                "byte_size": int(tex.byteSize),
            }
        )
    return rows


def _buffer_rows():
    rows = []
    for buf in rd.GetBuffers():
        rows.append({"name": _resource_name(buf.resourceId), "length": int(buf.length)})
    return rows


def _limit(args, key, default):
    try:
        return max(1, int(args.get(key, default)))
    except (TypeError, ValueError):
        return default


def tool_capture_info(args):
    out = {"loaded": bool(rd.IsCaptureLoaded()), "bridge_port": PORT}
    if not rd.IsCaptureLoaded():
        return out
    out["filename"] = str(rd.GetCaptureFilename())
    out["current_eid"] = rd.CurEvent()
    try:
        out["api"] = str(rd.APIProps().pipelineType)
    except Exception:
        pass
    counts = {"draws": 0, "dispatches": 0, "markers": 0, "total": 0}

    def count(actions):
        for a in actions:
            counts["total"] += 1
            flags = int(a.flags)
            if flags & int(renderdoc.ActionFlags.Drawcall):
                counts["draws"] += 1
            if flags & (int(renderdoc.ActionFlags.Dispatch) | int(renderdoc.ActionFlags.MeshDispatch) | int(renderdoc.ActionFlags.DispatchRay)):
                counts["dispatches"] += 1
            if flags & int(renderdoc.ActionFlags.PushMarker):
                counts["markers"] += 1
            count(a.children)

    count(rd.CurRootActions())
    out["actions"] = counts
    return out


def tool_list_events(args):
    if not rd.IsCaptureLoaded():
        return {"error": "no capture loaded"}
    max_depth = int(args.get("max_depth", 4))
    budget = [int(args.get("max_lines", 3000))]
    lines = []
    _walk_tree(rd.CurRootActions(), 0, max_depth, lines, budget)
    return {"tree": "\n".join(lines)}


def tool_fetch_eid(args):
    if not rd.IsCaptureLoaded():
        return {"error": "no capture loaded"}
    eid = int(args.get("eid", 0))
    a = rd.GetAction(eid)
    if a is None:
        return {"error": "EID %d not found" % eid}

    # switch the UI to the requested event so everything follows along
    rd.SetEventID(None, eid, eid, False)

    lines = []
    name = a.customName or "event"
    lines.append("EID %d: %s" % (eid, name))
    flags = int(a.flags)
    if flags & int(renderdoc.ActionFlags.Drawcall):
        lines.append("draw: %d indices, %d instances" % (a.numIndices, a.numInstances))
    if flags & (int(renderdoc.ActionFlags.Dispatch) | int(renderdoc.ActionFlags.MeshDispatch)):
        lines.append("dispatch: %dx%dx%d" % (a.dispatchDimension[0], a.dispatchDimension[1], a.dispatchDimension[2]))

    outs = [i for i in range(len(a.outputs)) if a.outputs[i] != _null()]
    lines.append("colour outputs: %d" % len(outs))
    for i in outs:
        tex = rd.GetTexture(a.outputs[i])
        if tex is not None:
            lines.append("  output %d: %s %dx%d %s" % (i, tex.name, tex.width, tex.height, tex.format.Name()))
    if a.depthOut != _null():
        tex = rd.GetTexture(a.depthOut)
        if tex is not None:
            lines.append("  depth: %s %dx%d %s" % (tex.name, tex.width, tex.height, tex.format.Name()))

    lines.append("")
    lines.append("(use get_texture to save the output images, or run_python for arbitrary state)")
    return {"snapshot": "\n".join(lines)}


def tool_get_texture(args):
    if not rd.IsCaptureLoaded():
        return {"error": "no capture loaded"}
    eid = int(args.get("eid", 0)) or rd.CurEvent()
    a = rd.GetAction(eid)
    if a is None:
        return {"error": "EID %d not found" % eid}
    rd.SetEventID(None, eid, eid, False)

    tmpdir = args.get("dest_dir") or tempfile.gettempdir()
    jobs = []
    for i in range(len(a.outputs)):
        if a.outputs[i] != _null():
            jobs.append((a.outputs[i], os.path.join(tmpdir, "rd_mcp_eid%d_output%d.png" % (eid, i))))
    if a.depthOut != _null():
        jobs.append((a.depthOut, os.path.join(tmpdir, "rd_mcp_eid%d_depth.png" % (eid,))))
    if not jobs:
        return {"error": "EID %d has no colour/depth outputs" % eid}

    def grab(controller):
        for rid, path in jobs:
            save = renderdoc.TextureSave()
            save.resourceId = rid
            save.destType = renderdoc.FileType.PNG
            save.mip = 0
            controller.SaveTexture(save, path)

    rd.Replay().BlockInvoke(grab)
    for _, path in jobs:
        if not os.path.exists(path):
            return {"error": "SaveTexture did not produce %s" % path}
    return {"files": [p for _, p in jobs], "eid": eid}


def _as_bool(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "y", "on")
    return bool(value)


def _capture_key():
    if rd is None or not rd.IsCaptureLoaded():
        return None
    try:
        filename = str(rd.GetCaptureFilename())
    except Exception:
        filename = ""
    count = [0]

    def walk(actions):
        for action in actions:
            count[0] += 1
            children = action.children
            if children:
                walk(children)

    try:
        walk(rd.CurRootActions())
    except Exception:
        pass
    return (filename, count[0])


def _reset_timing_cache():
    _timing_cache["capture_key"] = None
    _timing_cache["sampled"] = False
    _timing_cache["duration_supported"] = False
    _timing_cache["durations"] = None
    _timing_cache["counters"] = None
    _timing_cache["counter_columns"] = []
    _timing_cache["error"] = None


def _cache_current():
    """True when the UI-thread sample still belongs to the open capture."""
    key = _capture_key()
    if _timing_cache["capture_key"] != key:
        _reset_timing_cache()
        return False
    return bool(_timing_cache["sampled"])


def _counter_number(result, desc, is_duration):
    """Decode one CounterResult the way the Event Browser and counter viewer do.

    EventGPUDuration is seconds in ``value.d`` (FormatDuration multiplies by
    1000 for milliseconds). Other counters follow DescribeCounter: unsigned
    results use u32/u64, everything else uses f/d based on resultByteWidth.
    """
    if is_duration:
        try:
            return float(result.value.d)
        except Exception:
            pass
    is_uint = False
    width = 8
    if desc is not None:
        try:
            is_uint = int(desc.resultType) == int(renderdoc.CompType.UInt)
        except Exception:
            is_uint = False
        try:
            width = int(desc.resultByteWidth) or 8
        except Exception:
            width = 8
    if is_uint:
        try:
            if width <= 4:
                return int(result.value.u32)
            return int(result.value.u64)
        except Exception:
            return 0
    try:
        if width <= 4:
            return float(result.value.f)
        return float(result.value.d)
    except Exception:
        return 0.0


def _sample_counters():
    """Replay the loaded frame and cache GPU counters on the UI thread.

    FetchCounters is not reading the capture file. It replays the frame on
    the replay thread and can take a while. Callers must already be on the
    UI thread (the panel button, or the MCP pump).
    """
    err = _require_capture()
    if err:
        return err
    key = _capture_key()
    box = {
        "error": None,
        "durations": {},
        "counters": {},
        "columns": [],
        "duration_supported": False,
    }

    def grab(controller):
        try:
            available = [int(counter) for counter in controller.EnumerateCounters()]
        except Exception as exc:
            box["error"] = "EnumerateCounters failed: %s" % exc
            return
        selected = []
        id_to_spec = {}
        for name, column in _SAMPLE_COUNTERS:
            enum = getattr(renderdoc.GPUCounter, name, None)
            if enum is None:
                continue
            cid = int(enum)
            if cid not in available:
                continue
            selected.append(enum)
            id_to_spec[cid] = (name, column)
        descriptions = {}
        for enum in selected:
            try:
                descriptions[int(enum)] = controller.DescribeCounter(enum)
            except Exception:
                descriptions[int(enum)] = None
        if not selected:
            return
        try:
            results = controller.FetchCounters(selected)
        except Exception as exc:
            box["error"] = "FetchCounters failed: %s" % exc
            return
        columns = []
        for name, column in _SAMPLE_COUNTERS:
            if column and any(spec[1] == column for spec in id_to_spec.values()):
                columns.append(column)
        box["columns"] = columns
        box["duration_supported"] = any(spec[0] == "EventGPUDuration" for spec in id_to_spec.values())
        for result in results:
            spec = id_to_spec.get(int(result.counter))
            if spec is None:
                continue
            name, column = spec
            eid = int(result.eventId)
            value = _counter_number(
                result, descriptions.get(int(result.counter)), name == "EventGPUDuration"
            )
            if name == "EventGPUDuration":
                box["durations"][eid] = value
            elif column:
                box["counters"].setdefault(eid, {})[column] = value

    try:
        rd.Replay().BlockInvoke(grab)
    except Exception as exc:
        if _capture_key() != key:
            return {"error": "capture changed while sampling"}
        _timing_cache["capture_key"] = key
        _timing_cache["sampled"] = False
        _timing_cache["error"] = "%s: %s" % (type(exc).__name__, exc)
        return {"error": _timing_cache["error"]}

    if _capture_key() != key:
        return {"error": "capture changed while sampling"}

    if box["error"]:
        _timing_cache["capture_key"] = key
        _timing_cache["sampled"] = False
        _timing_cache["error"] = box["error"]
        return {"error": box["error"]}

    _timing_cache["capture_key"] = key
    _timing_cache["sampled"] = True
    _timing_cache["duration_supported"] = bool(box["duration_supported"])
    _timing_cache["durations"] = box["durations"] if box["duration_supported"] else None
    _timing_cache["counters"] = box["counters"]
    _timing_cache["counter_columns"] = list(box["columns"])
    _timing_cache["error"] = None
    return None


def _timing_report(limit=None):
    durations = None
    counters = None
    columns = None
    duration_supported = None
    if _cache_current():
        durations = _timing_cache["durations"]
        counters = _timing_cache["counters"] if _timing_cache["counters"] is not None else {}
        columns = _timing_cache["counter_columns"]
        duration_supported = _timing_cache["duration_supported"]
    report = rd_frame_stats.pass_timings(
        _flat_actions(),
        durations=durations,
        counters=counters,
        limit=limit,
        counter_columns=columns,
        duration_supported=duration_supported,
    )
    if _timing_cache.get("error") and _timing_cache.get("capture_key") == _capture_key():
        report["error"] = _timing_cache["error"]
    return report


def tool_frame_hotspots(args):
    err = _require_capture()
    if err:
        return err
    # refresh replays the frame. Without it, a previous Pass Timings sample
    # is reused so this call stays cheap.
    if _as_bool(args.get("refresh", False)):
        sample_err = _sample_counters()
        if sample_err:
            return sample_err
    limit = _limit(args, "limit", 20)
    summary = rd_frame_stats.summarize(
        _flat_actions(),
        _texture_rows(),
        _buffer_rows(),
        draw_limit=limit,
        texture_limit=limit,
        buffer_limit=min(limit, 15),
        pass_limit=max(limit, 40),
    )
    timing = _timing_report()
    summary["timings"] = {
        "sampled": timing["sampled"],
        "duration_supported": timing["duration_supported"],
        "note": timing["note"],
        "totals": timing["totals"],
        "passes": timing["passes"],
        "counter_columns": timing["counter_columns"],
        "cached": _cache_current(),
    }
    summary["text"] = summary["text"].rstrip() + "\n\n" + rd_frame_stats.format_pass_timings(timing, level=2)
    return summary


def tool_pass_timings(args):
    """Per-pass GPU milliseconds and invocation counters.

    ``refresh=true`` replays the frame via FetchCounters. Otherwise the last
    sample cached on the UI thread is reused, and the geometric estimate is
    returned alone when nothing has been sampled yet.
    """
    err = _require_capture()
    if err:
        return err
    refresh = _as_bool(args.get("refresh", False))
    if refresh:
        sample_err = _sample_counters()
        if sample_err:
            return sample_err
    limit = None
    if args.get("limit") is not None:
        limit = _limit(args, "limit", 40)
    report = _timing_report(limit=limit)
    report["cached"] = (not refresh) and _cache_current()
    pass_name = str(args.get("pass") or args.get("pass_name") or "").strip()
    if pass_name:
        event_limit = 200
        if args.get("event_limit") is not None:
            event_limit = _limit(args, "event_limit", 200)
        breakdown = _pass_events(pass_name, limit=event_limit)
        report["events"] = breakdown["events"]
        report["event_count"] = breakdown["event_count"]
        report["events_omitted"] = breakdown["omitted"]
        report["events_text"] = rd_frame_stats.format_pass_events(breakdown)
    report["sampling"] = (
        "refresh=true replays the frame with FetchCounters and can take a while. "
        "GPU milliseconds and invocation counters are not stored in the .rdc. "
        "Omit refresh to reuse the last sample for this capture."
    )
    return report


def tool_pass_costs(args):
    err = _require_capture()
    if err:
        return err
    rows = rd_frame_stats.pass_costs(_flat_actions(), limit=_limit(args, "limit", 40))
    return {"passes": rows, "text": rd_frame_stats.format_passes(rows)}


def tool_heaviest_draws(args):
    err = _require_capture()
    if err:
        return err
    rows = rd_frame_stats.heaviest_draws(_flat_actions(), limit=_limit(args, "limit", 20))
    return {"draws": rows, "text": rd_frame_stats.format_draws(rows)}


def tool_search_events(args):
    err = _require_capture()
    if err:
        return err
    query = str(args.get("query") or args.get("name") or "").strip()
    if not query:
        return {"error": "query is required"}
    hits = rd_frame_stats.search_events(_flat_actions(), query, limit=_limit(args, "limit", 50))
    return {"matches": hits, "text": rd_frame_stats.format_search(query, hits)}


def tool_resource_census(args):
    err = _require_capture()
    if err:
        return err
    limit = _limit(args, "limit", 20)
    textures = _texture_rows()
    buffers = _buffer_rows()
    top_tex = rd_frame_stats.top_resources(textures, "byte_size", limit)
    top_buf = rd_frame_stats.top_resources(buffers, "length", limit)
    text = (
        "# Resource census\n\n## Largest textures\n\n%s\n## Largest buffers\n\n%s"
        % (rd_frame_stats.format_textures(top_tex), rd_frame_stats.format_buffers(top_buf))
    )
    return {
        "texture_count": len(textures),
        "buffer_count": len(buffers),
        "texture_bytes": sum(int(t["byte_size"]) for t in textures),
        "buffer_bytes": sum(int(b["length"]) for b in buffers),
        "textures": top_tex,
        "buffers": top_buf,
        "text": text,
    }


def _refl_names(items, label):
    out = []
    for item in items or []:
        try:
            name = str(item.name or label)
            bind = "%s:%s" % (item.fixedBindSetOrSpace, item.fixedBindNumber)
            extra = ""
            if label == "cb":
                extra = " (%d bytes, %d vars)" % (int(item.byteSize), len(item.variables))
            out.append("%s @ %s%s" % (name, bind, extra))
        except Exception:
            continue
    return out


def _collect_shader_stages():
    pipe = rd.CurPipelineState()
    pipe_obj = pipe.GetGraphicsPipelineObject()
    if pipe_obj == _null():
        pipe_obj = pipe.GetComputePipelineObject()
    stages = []
    for name in _SHADER_STAGES:
        stage_enum = getattr(renderdoc.ShaderStage, name, None)
        if stage_enum is None:
            continue
        try:
            refl = pipe.GetShaderReflection(stage_enum)
        except Exception:
            continue
        if not refl:
            continue
        try:
            if refl.resourceId == _null():
                continue
        except Exception:
            continue
        entry = ""
        try:
            entry = str(refl.entryPoint or "")
        except Exception:
            pass
        if not entry:
            try:
                entry = str(pipe.GetShaderEntryPoint(stage_enum) or "")
            except Exception:
                pass
        stages.append(
            {
                "stage": name,
                "entry": entry,
                "resource_id": str(refl.resourceId),
                "constant_blocks": _refl_names(getattr(refl, "constantBlocks", []), "cb"),
                "textures": _refl_names(getattr(refl, "readOnlyResources", []), "tex"),
                "samplers": _refl_names(getattr(refl, "samplers", []), "smp"),
                "disassembly": "",
                "_refl": refl,
                "_pipe": pipe_obj,
            }
        )
    return stages


def tool_get_shader(args):
    err = _require_capture()
    if err:
        return err
    eid = int(args.get("eid", 0) or 0) or rd.CurEvent()
    action = rd.GetAction(eid)
    if action is None:
        return {"error": "EID %d not found" % eid}
    rd.SetEventID(None, eid, eid, False)
    try:
        max_chars = max(500, min(int(args.get("max_chars", 8000)), 40000))
    except (TypeError, ValueError):
        max_chars = 8000
    include = args.get("disassembly", True)
    if isinstance(include, str):
        include = include.lower() not in ("0", "false", "no")
    stage_filter = str(args.get("stage") or "")
    stages = _collect_shader_stages()
    if include and stages:
        def grab(controller):
            for info in stages:
                try:
                    text = controller.DisassembleShader(info["_pipe"], info["_refl"], "")
                    info["disassembly"] = str(text or "")
                except Exception as exc:
                    info["disassembly"] = "(disassembly failed: %s)" % exc

        rd.Replay().BlockInvoke(grab)
    for info in stages:
        info.pop("_refl", None)
        info.pop("_pipe", None)
    text = rd_frame_stats.format_shader_stages(stages, stage_filter=stage_filter, max_chars=max_chars)
    shown = rd_frame_stats.select_stages(stages, stage_filter)
    return {"eid": eid, "stage_count": len(shown), "text": text}


def tool_run_python(args):
    code = args.get("code", "")
    import io

    buf = io.StringIO()
    g = {"pyrenderdoc": rd, "renderdoc": renderdoc, "qrenderdoc": qrenderdoc, "result": None}
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            exec(compile(code, "<rd_mcp>", "exec"), g)
        out = {"output": buf.getvalue()}
        if g.get("result") is not None:
            out["result"] = repr(g["result"])
        return out
    except Exception as e:
        return {"output": buf.getvalue(), "error": "%s: %s" % (type(e).__name__, e)}


def tool_read_texture_b64(args):
    if not rd.IsCaptureLoaded():
        return {"error": "no capture loaded"}
    eid = int(args.get("eid", 0)) or rd.CurEvent()
    a = rd.GetAction(eid)
    if a is None:
        return {"error": "EID %d not found" % eid}
    want_depth = bool(args.get("depth", False))
    idx = int(args.get("output_index", 0))
    rid = _null()
    if want_depth:
        rid = a.depthOut
    elif idx < len(a.outputs):
        rid = a.outputs[idx]
    if rid == _null():
        return {"error": "EID %d has no such output" % eid}

    rd.SetEventID(None, eid, eid, False)
    path = os.path.join(tempfile.gettempdir(), "rd_mcp_b64.png")

    def grab(controller):
        save = renderdoc.TextureSave()
        save.resourceId = rid
        save.destType = renderdoc.FileType.PNG
        save.mip = 0
        controller.SaveTexture(save, path)

    rd.Replay().BlockInvoke(grab)

    import base64
    from PySide2.QtCore import QBuffer, QIODevice, Qt
    from PySide2.QtGui import QImage

    img = QImage(path)
    try:
        os.remove(path)
    except OSError:
        pass
    if img.isNull():
        return {"error": "SaveTexture produced no readable image"}

    maxsize = max(32, int(args.get("max_size", 512)))
    if img.width() > maxsize or img.height() > maxsize:
        img = img.scaled(maxsize, maxsize, Qt.KeepAspectRatio, Qt.SmoothTransformation)

    buf = QBuffer()
    buf.open(QIODevice.WriteOnly)
    img.save(buf, "PNG")
    data = bytes(buf.data())
    if data.count() > 4 * 1024 * 1024:
        return {"error": "image too large to return (%d bytes)" % data.count()}
    return {
        "eid": eid,
        "width": img.width(),
        "height": img.height(),
        "format": "png_base64",
        "data": base64.b64encode(data).decode("ascii"),
    }


def _pv_str(pv):
    try:
        f = pv.floatValue
        return "(%.4f, %.4f, %.4f, %.4f)" % (f[0], f[1], f[2], f[3])
    except Exception:
        return str(pv)


def tool_pixel_history(args):
    if not rd.IsCaptureLoaded():
        return {"error": "no capture loaded"}
    eid = int(args.get("eid", 0)) or rd.CurEvent()
    a = rd.GetAction(eid)
    if a is None:
        return {"error": "EID %d not found" % eid}
    try:
        x = int(args["x"])
        y = int(args["y"])
    except Exception:
        return {"error": "integer x and y are required"}
    rid = _null()
    for i in range(len(a.outputs)):
        if a.outputs[i] != _null():
            rid = a.outputs[i]
            break
    if rid == _null():
        rid = a.depthOut
    if rid == _null():
        return {"error": "EID %d has no outputs to inspect" % eid}

    rd.SetEventID(None, eid, eid, False)
    box = {}

    def grab(controller):
        box["hist"] = controller.PixelHistory(rid, x, y, renderdoc.Subresource(),
                                              renderdoc.CompType.Typeless)

    rd.Replay().BlockInvoke(grab)

    tex = rd.GetTexture(rid)
    lines = ["Pixel history for (%d, %d) on %s (from EID %d):" %
             (x, y, tex.name if tex is not None else "output", eid)]
    for h in box.get("hist", []):
        flags = []
        for attr in ("discard", "backfaceCulled", "depthClipped", "depthFailed", "stencilFailed"):
            try:
                if getattr(h, attr):
                    flags.append(attr)
            except Exception:
                pass
        line = "EID %d: %s -> %s" % (h.eventId, _pv_str(h.preMod), _pv_str(h.postMod))
        if flags:
            line += " [" + ",".join(flags) + "]"
        lines.append(line)
    if len(lines) == 1:
        lines.append("(no history entries at this pixel)")
    return {"history": "\n".join(lines), "count": len(box.get("hist", []))}


def _hotspot_report():
    return rd_frame_stats.format_report(_flat_actions(), _texture_rows(), _buffer_rows())


def _eid_in_line(line):
    return rd_frame_stats.eid_from_report_line(line)


def _open_hotspots(_ctx, _data):
    global _hotspot_dialog
    from PySide2.QtGui import QFont
    from PySide2.QtWidgets import (
        QApplication,
        QDialog,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QMessageBox,
        QPlainTextEdit,
        QPushButton,
        QVBoxLayout,
    )

    if rd is None or not rd.IsCaptureLoaded():
        QMessageBox.information(None, "Frame Hotspots", "Load a capture first.")
        return

    parent = None
    try:
        parent = rd.GetMainWindow().Widget()
    except Exception:
        parent = None

    dlg = QDialog(parent)
    dlg.setWindowTitle("Frame Hotspots")
    dlg.resize(920, 680)
    layout = QVBoxLayout(dlg)
    layout.addWidget(
        QLabel(
            "Pass cost, heaviest draws, and largest textures/buffers. "
            "Select a table row and Jump, or type an EID."
        )
    )
    text = QPlainTextEdit()
    text.setReadOnly(True)
    text.setFont(QFont("Consolas", 10))
    text.setPlainText(_hotspot_report())
    layout.addWidget(text)

    row = QHBoxLayout()
    find_edit = QLineEdit()
    find_edit.setPlaceholderText("search pass / draw name")
    eid_edit = QLineEdit()
    eid_edit.setPlaceholderText("EID")
    eid_edit.setMaximumWidth(120)
    find_btn = QPushButton("Find")
    jump_btn = QPushButton("Jump to EID")
    copy_btn = QPushButton("Copy")
    refresh_btn = QPushButton("Refresh")
    row.addWidget(find_edit, 1)
    row.addWidget(find_btn)
    row.addWidget(eid_edit)
    row.addWidget(jump_btn)
    row.addWidget(copy_btn)
    row.addWidget(refresh_btn)
    layout.addLayout(row)

    def do_refresh():
        if not rd.IsCaptureLoaded():
            text.setPlainText("Load a capture first.")
            return
        text.setPlainText(_hotspot_report())

    def do_find():
        query = find_edit.text().strip()
        if not query:
            do_refresh()
            return
        if not rd.IsCaptureLoaded():
            text.setPlainText("Load a capture first.")
            return
        hits = rd_frame_stats.search_events(_flat_actions(), query, limit=80)
        text.setPlainText(rd_frame_stats.format_search(query, hits))

    def do_jump():
        raw = eid_edit.text().strip()
        eid = None
        if raw:
            match = re.search(r"\d+", raw)
            if match:
                eid = int(match.group(0))
        if eid is None:
            eid = _eid_in_line(text.textCursor().block().text())
        if eid is None:
            return
        rd.SetEventID(None, eid, eid, False)
        eid_edit.setText(str(eid))

    def do_copy():
        QApplication.clipboard().setText(text.toPlainText())

    find_btn.clicked.connect(do_find)
    find_edit.returnPressed.connect(do_find)
    jump_btn.clicked.connect(do_jump)
    eid_edit.returnPressed.connect(do_jump)
    copy_btn.clicked.connect(do_copy)
    refresh_btn.clicked.connect(do_refresh)
    dlg.show()
    _hotspot_dialog = dlg


def _register_hotspot_menu():
    try:
        rd.Extensions().RegisterWindowMenu(
            qrenderdoc.WindowMenu.Tools, ["Frame Hotspots"], _open_hotspots
        )
        print("[rd_mcp_bridge] Tools -> Frame Hotspots registered")
    except Exception as exc:
        print("[rd_mcp_bridge] Frame Hotspots menu failed: %s" % exc)


def _main_window_widget():
    try:
        return rd.GetMainWindow().Widget()
    except Exception:
        return None


def _panel_alive(widget):
    if widget is None:
        return False
    try:
        widget.objectName()
        return True
    except RuntimeError:
        return False


def _ensure_panel_class():
    global _panel_cls
    if _panel_cls is not None:
        return _panel_cls
    from PySide2.QtCore import Qt, QTimer, Signal
    from PySide2.QtWidgets import (
        QApplication,
        QFileDialog,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QPushButton,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )

    class _NumItem(QTableWidgetItem):
        def __lt__(self, other):
            left = self.data(Qt.UserRole)
            right = other.data(Qt.UserRole) if other is not None else None
            if left is None and right is None:
                return False
            if left is None:
                return True
            if right is None:
                return False
            try:
                return float(left) < float(right)
            except (TypeError, ValueError):
                return str(left) < str(right)

    class PassTimingsPanel(QWidget):
        sampleFinished = Signal(str)

        def __init__(self, parent=None):
            super(PassTimingsPanel, self).__init__(parent)
            self.setObjectName("PassTimingsPanel")
            self.setWindowTitle("Pass Timings")
            self.setMinimumSize(780, 420)
            self._markdown = ""
            self._report = None
            self._query = ""
            self._sample_error = ""
            self._sampling = False
            self._sample_started = 0.0
            self._docked = False
            self.sampleFinished.connect(self._on_sample_finished, Qt.QueuedConnection)
            self._tick = QTimer(self)
            self._tick.setInterval(500)
            self._tick.timeout.connect(self._on_sample_tick)

            layout = QVBoxLayout(self)
            self._status = QLabel("Idle — geometric estimate only. Sample GPU counters to measure milliseconds.")
            self._status.setWordWrap(True)
            self._totals = QLabel("")
            self._totals.setWordWrap(True)
            self._note = QLabel("")
            self._note.setWordWrap(True)
            layout.addWidget(self._status)
            layout.addWidget(self._totals)
            layout.addWidget(self._note)
            self._filter = QLineEdit()
            self._filter.setPlaceholderText("Filter pass or event name / EID")
            self._filter.textChanged.connect(self._on_filter)
            layout.addWidget(self._filter)

            self._table = QTableWidget(0, 0)
            self._table.setSelectionBehavior(QTableWidget.SelectRows)
            self._table.setSelectionMode(QTableWidget.SingleSelection)
            self._table.setEditTriggers(QTableWidget.NoEditTriggers)
            self._table.setAlternatingRowColors(True)
            self._table.verticalHeader().setVisible(False)
            self._table.cellDoubleClicked.connect(self._on_double_click)
            self._table.itemSelectionChanged.connect(self._fill_events)
            layout.addWidget(self._table, 3)

            self._events_label = QLabel("Select a pass to list the events that make up its time.")
            self._events_label.setWordWrap(True)
            layout.addWidget(self._events_label)
            self._events = QTableWidget(0, 0)
            self._events.setSelectionBehavior(QTableWidget.SelectRows)
            self._events.setSelectionMode(QTableWidget.SingleSelection)
            self._events.setEditTriggers(QTableWidget.NoEditTriggers)
            self._events.setAlternatingRowColors(True)
            self._events.verticalHeader().setVisible(False)
            self._events.cellDoubleClicked.connect(self._on_event_double_click)
            layout.addWidget(self._events, 2)

            buttons = QHBoxLayout()
            self._sample_btn = QPushButton("Sample GPU counters")
            self._sample_btn.setToolTip(
                "Replay the frame and fetch EventGPUDuration plus invocation counters. "
                "The rest of the UI stays usable. The result is not stored in the capture."
            )
            jump_btn = QPushButton("Jump")
            copy_btn = QPushButton("Copy as markdown")
            export_btn = QPushButton("Export…")
            self._sample_btn.clicked.connect(self._on_sample)
            jump_btn.clicked.connect(self._on_jump)
            copy_btn.clicked.connect(self._on_copy)
            export_btn.clicked.connect(self._on_export)
            buttons.addWidget(self._sample_btn)
            buttons.addWidget(jump_btn)
            buttons.addWidget(copy_btn)
            buttons.addWidget(export_btn)
            buttons.addStretch(1)
            layout.addLayout(buttons)

        def showEvent(self, event):
            super(PassTimingsPanel, self).showEvent(event)
            if not self._sampling:
                self.reload()

        def reload(self):
            if self._sampling:
                return
            if rd is None or not rd.IsCaptureLoaded():
                self._status.setText("Idle — load a capture, then sample GPU counters.")
                self._totals.setText("")
                self._note.setText("Milliseconds appear after sampling. The sample replays the frame and is not stored in the .rdc.")
                self._markdown = ""
                self._report = None
                self._table.setSortingEnabled(False)
                self._table.setRowCount(0)
                self._events.setSortingEnabled(False)
                self._events.setRowCount(0)
                self._events_label.setText("Select a pass to list the events that make up its time.")
                return
            report = _timing_report()
            self._fill(report)

        def clear_for_close(self):
            self._markdown = ""
            self._report = None
            self._status.setText("Idle — load a capture, then sample GPU counters.")
            self._totals.setText("")
            self._note.setText("Milliseconds appear after sampling. The sample replays the frame and is not stored in the .rdc.")
            self._events_label.setText("Select a pass to list the events that make up its time.")
            self._table.setSortingEnabled(False)
            self._table.setRowCount(0)
            self._events.setSortingEnabled(False)
            self._events.setRowCount(0)

        def _on_filter(self, text):
            self._query = text or ""
            if self._report is not None and not self._sampling:
                self._fill(self._report)

        def _fill(self, report):
            error = report.get("error")
            if self._sample_error:
                self._status.setText("Error — %s" % self._sample_error)
            elif error:
                self._status.setText("Error — %s" % error)
            elif not report.get("sampled"):
                self._status.setText(
                    "Idle — geometric estimate only. Sample GPU counters to measure milliseconds (replays the frame)."
                )
            elif not report.get("duration_supported"):
                self._status.setText(
                    "Done — EventGPUDuration is not supported by this driver. Milliseconds are unavailable."
                )
            else:
                self._status.setText(
                    "Done — frame GPU %s ms. Sampled by replaying the frame; not stored in the capture."
                    % rd_frame_stats.format_ms(report["totals"].get("gpu_seconds"))
                )

            totals = report.get("totals") or {}
            bits = []
            if totals.get("gpu_ms") is not None:
                bits.append("Frame %s ms" % rd_frame_stats.format_ms(totals.get("gpu_seconds")))
            bits.append("Triangles %s" % rd_frame_stats._comma(totals.get("triangles") or 0))
            bits.append("Draws %s" % rd_frame_stats._comma(totals.get("draws") or 0))
            bits.append("Dispatches %s" % rd_frame_stats._comma(totals.get("dispatches") or 0))
            counter_totals = totals.get("counters") or {}
            for key in report.get("counter_columns") or []:
                bits.append(
                    "%s %s" % (rd_frame_stats._counter_label(key), rd_frame_stats._comma(round(counter_totals.get(key) or 0)))
                )
            self._totals.setText("  ·  ".join(bits))
            self._note.setText(report.get("note") or "")
            self._markdown = report.get("text") or ""
            self._report = report

            columns = [
                "EID",
                "Pass",
                "GPU ms",
                "% of frame",
                "Draws",
                "Dispatches",
                "Est. triangles",
                "ms / Mtri",
                "Balance",
            ]
            columns.extend(rd_frame_stats._counter_label(key) for key in report.get("counter_columns") or [])
            tips = {
                "GPU ms": "EventGPUDuration replay sample, in milliseconds. A parent is the sum of its children.",
                "% of frame": "Share of frame GPU time after the same child-sum rollup.",
                "Est. triangles": "indices/3 x instances. Strips and other topologies are approximate.",
                "ms / Mtri": "GPU milliseconds per million estimated triangles.",
                "Balance": "time-heavy: time share is more than twice the triangle share. geometry-heavy is the reverse.",
                "PS invocations": "Pixel shader invocations from the replay sample.",
                "CS invocations": "Compute shader invocations from the replay sample.",
                "VS invocations": "Vertex shader invocations from the replay sample.",
                "Samples passed": "Samples that passed the depth/stencil test.",
                "IA primitives": "Primitives read by the input assembler.",
            }

            table = self._table
            keep_eid = self._selected_eid()
            table.blockSignals(True)
            table.setSortingEnabled(False)
            table.clear()
            table.setColumnCount(len(columns))
            table.setHorizontalHeaderLabels(columns)
            for index, name in enumerate(columns):
                item = table.horizontalHeaderItem(index)
                if item is not None and name in tips:
                    item.setToolTip(tips[name])
            passes = rd_frame_stats.filter_passes(report.get("passes") or [], self._query)
            table.setRowCount(len(passes))
            counter_keys = list(report.get("counter_columns") or [])
            for row_index, row in enumerate(passes):
                eid = row.get("eid")
                values = [
                    ("" if eid is None else str(eid), None if eid is None else int(eid)),
                    (rd_frame_stats.pass_label(row), row.get("name") or ""),
                    (
                        rd_frame_stats.format_ms(row.get("gpu_seconds")) if row.get("gpu_ms") is not None else "---",
                        row.get("gpu_ms"),
                    ),
                    (rd_frame_stats.format_share(row.get("pct")), row.get("pct")),
                    (rd_frame_stats._comma(row.get("draws") or 0), int(row.get("draws") or 0)),
                    (rd_frame_stats._comma(row.get("dispatches") or 0), int(row.get("dispatches") or 0)),
                    (rd_frame_stats._comma(row.get("triangles") or 0), int(row.get("triangles") or 0)),
                    (rd_frame_stats.format_rate(row.get("ms_per_mtri")), row.get("ms_per_mtri")),
                    (row.get("balance") or "", row.get("balance") or ""),
                ]
                cmap = row.get("counters") or {}
                for key in counter_keys:
                    amount = cmap.get(key) or 0
                    values.append((rd_frame_stats._comma(round(amount)), float(amount)))
                for col, (text, sort_value) in enumerate(values):
                    if col in (1, 8):
                        item = QTableWidgetItem(text)
                    else:
                        item = _NumItem(text)
                        item.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
                    item.setData(Qt.UserRole, sort_value)
                    table.setItem(row_index, col, item)

            header = table.horizontalHeader()
            header.setSectionResizeMode(QHeaderView.ResizeToContents)
            if table.columnCount() > 1:
                header.setSectionResizeMode(1, QHeaderView.Stretch)
            table.setSortingEnabled(True)
            if report.get("sampled") and report.get("duration_supported"):
                table.sortItems(2, Qt.DescendingOrder)
            elif table.columnCount() > 6:
                table.sortItems(6, Qt.DescendingOrder)
            select_row = 0 if table.rowCount() else -1
            if keep_eid is not None:
                for row_index in range(table.rowCount()):
                    item = table.item(row_index, 0)
                    if item is not None and item.data(Qt.UserRole) == keep_eid:
                        select_row = row_index
                        break
            table.blockSignals(False)
            if select_row >= 0:
                table.selectRow(select_row)
            else:
                self._fill_events()

        def _selected_pass_name(self):
            row = self._table.currentRow()
            if row < 0:
                return None
            item = self._table.item(row, 1)
            if item is None:
                return None
            name = item.data(Qt.UserRole)
            if name is None:
                name = item.text()
            name = str(name or "").strip()
            return name or None

        def _fill_events(self):
            name = self._selected_pass_name()
            table = self._events
            table.setSortingEnabled(False)
            table.clear()
            table.setRowCount(0)
            if not name or rd is None or not rd.IsCaptureLoaded():
                self._events_label.setText("Select a pass to list the events that make up its time.")
                return
            breakdown = _pass_events(name)
            events = list(breakdown.get("events") or [])
            query = (self._query or "").strip().lower()
            if query and query not in name.lower():
                events = [
                    event for event in events
                    if query in (event.get("name") or "").lower() or query in str(event.get("eid") or "")
                ]
            total_events = len(events)
            shown = events[:300]
            omitted = total_events - len(shown)
            if breakdown.get("gpu_seconds") is not None:
                label = "Events in %s — %s ms is the sum of every event in the pass, not an extra parent timestamp." % (
                    name,
                    rd_frame_stats.format_ms(breakdown.get("gpu_seconds")),
                )
            else:
                label = "Events in %s — GPU milliseconds appear after sampling. Triangles are the geometric estimate." % name
            if omitted:
                label += " Showing the first %d of %d." % (len(shown), total_events)
            self._events_label.setText(label)
            events = shown
            columns = ["EID", "Event", "Kind", "GPU ms", "% of pass", "Est. triangles", "ms / Mtri", "Thread groups"]
            counter_keys = list(breakdown.get("counter_columns") or [])
            columns.extend(rd_frame_stats._counter_label(key) for key in counter_keys)
            table.setColumnCount(len(columns))
            table.setHorizontalHeaderLabels(columns)
            table.setRowCount(len(events))
            for row_index, row in enumerate(events):
                label = row.get("name") or ""
                if row.get("indirect"):
                    label += " (indirect)"
                values = [
                    (str(row.get("eid") or ""), int(row.get("eid") or 0)),
                    (label, label),
                    (row.get("kind") or "", row.get("kind") or ""),
                    (
                        rd_frame_stats.format_ms(row.get("gpu_seconds")) if row.get("gpu_ms") is not None else "---",
                        row.get("gpu_ms"),
                    ),
                    (rd_frame_stats.format_share(row.get("pct")), row.get("pct")),
                    (rd_frame_stats._comma(row.get("triangles") or 0), int(row.get("triangles") or 0)),
                    (rd_frame_stats.format_rate(row.get("ms_per_mtri")), row.get("ms_per_mtri")),
                    (rd_frame_stats._comma(row.get("groups") or 0), int(row.get("groups") or 0)),
                ]
                cmap = row.get("counters") or {}
                for key in counter_keys:
                    amount = cmap.get(key) or 0
                    values.append((rd_frame_stats._comma(round(amount)), float(amount)))
                for col, (text, sort_value) in enumerate(values):
                    if col in (1, 2):
                        item = QTableWidgetItem(text)
                    else:
                        item = _NumItem(text)
                        item.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
                    item.setData(Qt.UserRole, sort_value)
                    table.setItem(row_index, col, item)
            header = table.horizontalHeader()
            header.setSectionResizeMode(QHeaderView.ResizeToContents)
            if table.columnCount() > 1:
                header.setSectionResizeMode(1, QHeaderView.Stretch)
            table.setSortingEnabled(True)

        def _selected_eid(self):
            row = self._table.currentRow()
            if row < 0:
                return None
            item = self._table.item(row, 0)
            if item is None:
                return None
            eid = item.data(Qt.UserRole)
            if eid is None:
                return None
            return int(eid)

        def _jump_to(self, eid):
            if eid is None or rd is None:
                return
            rd.SetEventID(None, int(eid), int(eid), False)

        def _on_double_click(self, row, _column):
            item = self._table.item(row, 0)
            if item is None:
                return
            self._jump_to(item.data(Qt.UserRole))

        def _on_event_double_click(self, row, _column):
            item = self._events.item(row, 0)
            if item is None:
                return
            self._jump_to(item.data(Qt.UserRole))

        def _on_jump(self):
            eid = None
            row = self._events.currentRow()
            selected = self._events.selectionModel()
            if row >= 0 and selected is not None and selected.hasSelection():
                item = self._events.item(row, 0)
                if item is not None:
                    eid = item.data(Qt.UserRole)
            if eid is None:
                eid = self._selected_eid()
            self._jump_to(eid)

        def _report_text(self):
            text = self._markdown or ""
            name = self._selected_pass_name()
            if name:
                text = text.rstrip() + "\n\n" + rd_frame_stats.format_pass_events(_pass_events(name, limit=300))
            return text

        def _on_copy(self):
            QApplication.clipboard().setText(self._report_text())

        def _on_export(self):
            path, _chosen = QFileDialog.getSaveFileName(
                self, "Export pass timings", "pass-timings.md", "Markdown (*.md)"
            )
            if not path:
                return
            try:
                with open(path, "w", encoding="utf-8") as handle:
                    handle.write(self._report_text())
            except OSError as exc:
                self._status.setText("Error — could not write %s" % exc)
                return
            self._status.setText("Exported %s" % path)

        def _on_sample_tick(self):
            if not self._sampling:
                self._tick.stop()
                return
            import time

            elapsed = int(time.monotonic() - self._sample_started)
            self._status.setText(
                "Replaying the frame… %ds. The rest of the UI stays usable." % elapsed
            )

        def _on_sample(self):
            if self._sampling:
                return
            if rd is None or not rd.IsCaptureLoaded():
                self._status.setText("Load a capture first.")
                return
            import time

            self._sample_error = ""
            self._sampling = True
            self._sample_started = time.monotonic()
            self._sample_btn.setEnabled(False)
            self._status.setText("Replaying the frame… 0s. The rest of the UI stays usable.")
            self._tick.start()

            def work():
                message = ""
                try:
                    result = _sample_counters()
                    if isinstance(result, dict) and result.get("error"):
                        message = str(result["error"])
                except Exception as exc:
                    message = "%s: %s" % (type(exc).__name__, exc)
                self.sampleFinished.emit(message)

            threading.Thread(target=work, daemon=True).start()

        def _on_sample_finished(self, message):
            self._tick.stop()
            self._sampling = False
            self._sample_btn.setEnabled(True)
            self._sample_error = message or ""
            if message:
                self._status.setText("Error — %s" % message)
            self.reload()

    _panel_cls = PassTimingsPanel
    return _panel_cls


def _dock_pass_timings(widget):
    ref = None
    try:
        ref = rd.GetEventBrowser().Widget()
    except Exception:
        ref = None
    try:
        if ref is not None:
            rd.AddDockWindow(widget, qrenderdoc.DockReference.BottomOf, ref, 0.35)
        else:
            rd.AddDockWindow(widget, qrenderdoc.DockReference.MainToolArea, None)
        widget._docked = True
    except Exception as exc:
        print("[rd_mcp_bridge] Pass Timings dock failed: %s" % exc)
        widget.show()


def _ensure_timing_viewer(panel):
    global _timing_viewer
    if _timing_viewer is not None:
        _timing_viewer.panel = panel
        return

    class _TimingViewer(qrenderdoc.CaptureViewer):
        def __init__(self):
            super(_TimingViewer, self).__init__()
            self.panel = None

        def OnCaptureLoaded(self):
            _reset_timing_cache()
            panel_ref = self.panel
            if not _panel_alive(panel_ref):
                return
            panel_ref._sample_error = ""
            from PySide2.QtCore import QTimer

            QTimer.singleShot(0, panel_ref.reload)

        def OnCaptureClosed(self):
            _reset_timing_cache()
            if _panel_alive(self.panel):
                self.panel.clear_for_close()

        def OnEventChanged(self, _event):
            return

        def OnSelectedEventChanged(self, _event):
            return

    _timing_viewer = _TimingViewer()
    _timing_viewer.panel = panel
    try:
        rd.AddCaptureViewer(_timing_viewer)
    except Exception as exc:
        print("[rd_mcp_bridge] Pass Timings capture watcher failed: %s" % exc)


def _open_pass_timings(_ctx, _data):
    global _pass_timings_panel
    if not _panel_alive(_pass_timings_panel):
        try:
            cls = _ensure_panel_class()
        except Exception as exc:
            print("[rd_mcp_bridge] Pass Timings unavailable: %s" % exc)
            return
        _pass_timings_panel = cls(_main_window_widget())
        _ensure_timing_viewer(_pass_timings_panel)
        _dock_pass_timings(_pass_timings_panel)
    else:
        _pass_timings_panel.reload()
    try:
        rd.RaiseDockWindow(_pass_timings_panel)
    except Exception:
        pass
    _pass_timings_panel.show()
    _pass_timings_panel.raise_()


def _register_pass_timings_menu():
    try:
        rd.Extensions().RegisterWindowMenu(
            qrenderdoc.WindowMenu.Tools, ["Pass Timings"], _open_pass_timings
        )
        print("[rd_mcp_bridge] Tools -> Pass Timings registered")
    except Exception as exc:
        print("[rd_mcp_bridge] Pass Timings menu failed: %s" % exc)


TOOLS = {
    "capture_info": tool_capture_info,
    "list_events": tool_list_events,
    "fetch_eid": tool_fetch_eid,
    "get_texture": tool_get_texture,
    "read_texture_b64": tool_read_texture_b64,
    "pixel_history": tool_pixel_history,
    "frame_hotspots": tool_frame_hotspots,
    "pass_timings": tool_pass_timings,
    "pass_costs": tool_pass_costs,
    "heaviest_draws": tool_heaviest_draws,
    "search_events": tool_search_events,
    "resource_census": tool_resource_census,
    "get_shader": tool_get_shader,
    "run_python": tool_run_python,
}


# ---------------------------------------------------------------------------
# queue pump: runs on the UI thread via QTimer
# ---------------------------------------------------------------------------

def _pump():
    while True:
        try:
            call_id, tool, args = _reqq.get_nowait()
        except queue.Empty:
            break
        fn = TOOLS.get(tool)
        if fn is None:
            resp = {"ok": False, "error": "unknown tool '%s'" % tool}
        else:
            try:
                resp = {"ok": True, "result": fn(args)}
            except Exception as e:
                resp = {"ok": False, "error": "%s: %s" % (type(e).__name__, e)}
        with _pending_lock:
            pend = _pending.pop(call_id, None)
        if pend is not None:
            pend[1]["resp"] = resp
            pend[0].set()


def _dispatch(tool, args, timeout):
    call_id = _call_ids[0]
    _call_ids[0] += 1
    ev = threading.Event()
    box = {}
    with _pending_lock:
        _pending[call_id] = (ev, box)
    _reqq.put((call_id, tool, args))
    if not ev.wait(timeout):
        with _pending_lock:
            _pending.pop(call_id, None)
        return {"ok": False, "error": "timed out after %ss (UI thread busy?)" % timeout}
    return box.get("resp", {"ok": False, "error": "no response"})


# ---------------------------------------------------------------------------
# TCP server thread
# ---------------------------------------------------------------------------

def _client_loop(conn):
    try:
        f = conn.makefile("rb")
        while not _stop.is_set():
            line = f.readline()
            if not line:
                break
            try:
                req = json.loads(line.decode("utf-8"))
            except Exception as e:
                resp = {"ok": False, "error": "bad json: %s" % e}
            else:
                timeout = float(req.get("args", {}).get("timeout", 120) or 120)
                resp = _dispatch(str(req.get("tool", "")), req.get("args") or {}, timeout)
            try:
                conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
            except OSError:
                break
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def _serve():
    global _server
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((HOST, PORT))
        srv.listen(4)
        srv.settimeout(0.5)
        _server = srv
        print("[rd_mcp_bridge] listening on %s:%d" % (HOST, PORT))
        while not _stop.is_set():
            try:
                conn, _addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=_client_loop, args=(conn,), daemon=True).start()
    except OSError as e:
        print("[rd_mcp_bridge] could not bind %s:%d (%s) - another instance owns it?" % (HOST, PORT, e))
    finally:
        try:
            srv.close()
        except Exception:
            pass


def start():
    global _timer
    if _timer is not None:
        return
    try:
        from PySide2.QtCore import QTimer
    except Exception as e:
        print("[rd_mcp_bridge] PySide2 unavailable, cannot marshal to UI thread: %s" % e)
        return
    threading.Thread(target=_serve, daemon=True).start()
    _timer = QTimer()
    _timer.timeout.connect(_pump)
    _timer.start(50)
    _register_hotspot_menu()
    _register_pass_timings_menu()
    print("[rd_mcp_bridge] loaded (tools: %s)" % ", ".join(sorted(TOOLS)))


def unregister():
    global _timer, _pass_timings_panel, _timing_viewer
    _stop.set()
    if _timer is not None:
        _timer.stop()
        _timer = None
    if _timing_viewer is not None and rd is not None:
        try:
            rd.RemoveCaptureViewer(_timing_viewer)
        except Exception:
            pass
        _timing_viewer = None
    _pass_timings_panel = None
    print("[rd_mcp_bridge] unloaded")


def register(_version_string, pyrenderdoc_ctx):
    """Entry point called by qrenderdoc with the CaptureContext instance."""
    global rd
    rd = pyrenderdoc_ctx
    start()
