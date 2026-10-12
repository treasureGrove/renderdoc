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
    return rd_frame_stats.flatten(_actions_to_tree(rd.CurRootActions()))


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


def tool_frame_hotspots(args):
    err = _require_capture()
    if err:
        return err
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
    return summary


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


TOOLS = {
    "capture_info": tool_capture_info,
    "list_events": tool_list_events,
    "fetch_eid": tool_fetch_eid,
    "get_texture": tool_get_texture,
    "read_texture_b64": tool_read_texture_b64,
    "pixel_history": tool_pixel_history,
    "frame_hotspots": tool_frame_hotspots,
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
    print("[rd_mcp_bridge] loaded (tools: %s)" % ", ".join(sorted(TOOLS)))


def unregister():
    global _timer
    _stop.set()
    if _timer is not None:
        _timer.stop()
        _timer = None
    print("[rd_mcp_bridge] unloaded")


def register(_version_string, pyrenderdoc_ctx):
    """Entry point called by qrenderdoc with the CaptureContext instance."""
    global rd
    rd = pyrenderdoc_ctx
    start()
