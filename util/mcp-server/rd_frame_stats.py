"""Pure helpers for frame cost, GPU pass timings, resource size, and shaders.

No dependency on the renderdoc module, so the ranking and report text can be
unit-tested without a capture or a GUI. The MCP bridge turns live capture
objects into the dicts these functions expect.

Pass GPU time follows the Event Browser: a marker's duration is the sum of
its children, and the marker's own timestamp is not added on top.
"""

import re


def eid_from_report_line(line):
    """EID from the first markdown cell. Later numeric columns are ignored."""
    match = re.match(r"^\|\s*(\d+)\s*\|", (line or "").strip())
    if not match:
        return None
    return int(match.group(1))


def format_bytes(n):
    n = float(n or 0)
    if n < 0:
        n = 0
    units = ("B", "KB", "MB", "GB", "TB")
    i = 0
    while n >= 1024.0 and i < len(units) - 1:
        n /= 1024.0
        i += 1
    if i == 0:
        return "%d B" % int(n)
    return "%.2f %s" % (n, units[i])


def _comma(n):
    return "{:,}".format(int(n))


def draw_cost(rec):
    """Index count times instance count. A zero instance count counts as one."""
    inst = int(rec.get("num_instances") or 0)
    if inst < 1:
        inst = 1
    return int(rec.get("num_indices") or 0) * inst


def dispatch_groups(rec):
    dims = rec.get("dispatch") or (0, 0, 0)
    try:
        x, y, z = dims
    except (TypeError, ValueError):
        return 0
    return max(int(x), 0) * max(int(y), 0) * max(int(z), 0)


def flatten(actions, stack=None):
    """Depth-first list of action dicts with a 'marker' path of parent names.

    ``nchildren`` is the number of direct children and is what pass timings use
    to roll durations up the tree. A parent sample must not be added on top of
    those children.
    """
    stack = list(stack or [])
    out = []
    for a in actions or []:
        kids = a.get("children") or []
        rec = {
            "eid": int(a.get("eid") or 0),
            "name": a.get("name") or a.get("kind") or "event",
            "kind": a.get("kind") or "other",
            "num_indices": int(a.get("num_indices") or 0),
            "num_instances": int(a.get("num_instances") or 0),
            "dispatch": a.get("dispatch"),
            "indirect": bool(a.get("indirect")),
            "marker": " / ".join(stack),
            "nchildren": len(kids),
        }
        out.append(rec)
        child_stack = stack
        if rec["kind"] == "marker":
            child_stack = stack + [rec["name"]]
        out.extend(flatten(kids, child_stack))
    return out


def grouping_depth(flat):
    """Use the second marker level when the whole frame sits under one root."""
    tops = set()
    for e in flat:
        if e["kind"] == "marker" or not e["marker"]:
            continue
        tops.add(e["marker"].split(" / ")[0])
    return 2 if len(tops) <= 1 else 1


def _group_key(marker, depth):
    if not marker:
        return "(unmarked)"
    parts = [p for p in marker.split(" / ") if p]
    if not parts:
        return "(unmarked)"
    return " / ".join(parts[:depth])


def _marker_eid(flat, key):
    for e in flat:
        if e["kind"] != "marker":
            continue
        full = (e["marker"] + " / " + e["name"]).strip(" /")
        if full == key or e["name"] == key:
            return e["eid"]
    return None


# Invocation / primitive counters the Pass Timings panel knows how to show.
# The bridge only fills the ones EnumerateCounters() actually returned.
COUNTER_COLUMNS = (
    ("ps", "PS invocations"),
    ("cs", "CS invocations"),
    ("vs", "VS invocations"),
    ("samples", "Samples passed"),
    ("ia_primitives", "IA primitives"),
)

UNSAMPLED_TIMING_NOTE = (
    "Estimated triangles = indices/3 x instances. "
    "GPU milliseconds appear after sampling EventGPUDuration. "
    "That sample replays the frame and is not stored in the capture. "
    "Marker time is the sum of child events, matching the Event Browser."
)

SAMPLED_TIMING_NOTE = (
    "GPU time is EventGPUDuration in seconds, shown here as milliseconds. "
    "Marker and parent time is the sum of child events; a parent timestamp is not "
    "added on top. The sample replays the frame and is not stored in the capture."
)

UNSUPPORTED_DURATION_NOTE = (
    "This driver does not support EventGPUDuration, so GPU milliseconds are unavailable. "
    "Invocation and primitive counters are included when the driver returned them. "
    "Counters come from a replay sample, not from the capture file."
)


def _valid_nonneg(value):
    """Finite value >= 0, or None when the sample should be ignored.

    Matches Event Browser's CalculateTotalDuration: NaN, inf, and negatives
    are skipped. Zero is a real sample.
    """
    if value is None or isinstance(value, bool):
        return None
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    if v != v or v == float("inf") or v == float("-inf"):
        return None
    if v < 0.0:
        return None
    return v


def _eid_map(raw):
    out = {}
    for key, value in (raw or {}).items():
        try:
            out[int(key)] = value
        except (TypeError, ValueError):
            continue
    return out


def _tree_from_flat(flat):
    """Rebuild the action tree from a depth-first flatten() list."""
    flat = list(flat or [])
    index = [0]

    def take():
        event = flat[index[0]]
        index[0] += 1
        children = []
        for _ in range(int(event.get("nchildren") or 0)):
            if index[0] >= len(flat):
                break
            children.append(take())
        return {"event": event, "children": children}

    roots = []
    while index[0] < len(flat):
        roots.append(take())
    return roots


def rollup_durations(flat, durations):
    """Per-event seconds after the Event Browser rollup.

    An event with children gets the sum of its children's rolled-up times.
    Its own sample is discarded so a parent timestamp cannot be added on top
    of the children. A leaf keeps its own sample when that sample is finite
    and non-negative, including zero. Missing, negative, NaN, and infinite
    leaf samples are omitted.

    Returns ``(eid_to_seconds, frame_seconds)``. ``frame_seconds`` is the sum
    of the root events, which is the same total the Event Browser shows for
    the frame.
    """
    samples = _eid_map(durations)
    rolled = {}

    def walk(node):
        eid = int(node["event"]["eid"])
        if node["children"]:
            total = 0.0
            for child in node["children"]:
                child_value = walk(child)
                if child_value is None:
                    continue
                total += child_value
            rolled[eid] = total
            return total
        if eid not in samples:
            return None
        value = _valid_nonneg(samples.get(eid))
        if value is None:
            return None
        rolled[eid] = value
        return value

    frame = 0.0
    for root in _tree_from_flat(flat):
        value = walk(root)
        if value is not None:
            frame += value
    return rolled, frame


def _counter_columns(counters, explicit):
    if explicit is not None:
        return [str(name) for name in explicit]
    seen = []
    for cmap in _eid_map(counters).values():
        if not isinstance(cmap, dict):
            continue
        for key in cmap:
            if key not in seen:
                seen.append(key)
    known = [key for key, _label in COUNTER_COLUMNS if key in seen]
    extras = [key for key in seen if key not in known]
    return known + extras


def _leaf_count(counters, eid, key):
    cmap = counters.get(eid)
    if not isinstance(cmap, dict) or key not in cmap:
        return 0.0
    value = _valid_nonneg(cmap.get(key))
    if value is None:
        return 0.0
    return value


def _counter_label(key):
    for name, label in COUNTER_COLUMNS:
        if name == key:
            return label
    return str(key).replace("_", " ")


def format_ms(seconds):
    if seconds is None:
        return "---"
    return "%.3f" % (float(seconds) * 1000.0)


def format_pct(pct):
    if pct is None:
        return "---"
    return "%.1f%%" % float(pct)


def _timing_key(event, depth):
    """Group a timing leaf into a pass.

    Draws use their parent marker path. An empty marker is its own leaf, so
    it keeps its own name (truncated to the grouping depth) instead of
    folding into the parent path.
    """
    marker = event.get("marker") or ""
    if event.get("kind") == "marker":
        full = (marker + " / " + (event.get("name") or "")).strip(" /")
        return _group_key(full, depth)
    return _group_key(marker, depth)


def pass_timings(
    flat,
    durations=None,
    counters=None,
    limit=None,
    depth=None,
    counter_columns=None,
    duration_supported=None,
):
    """Per-pass GPU time and invocation counters.

    ``durations`` maps event id to a raw EventGPUDuration sample in seconds.
    ``counters`` maps event id to ``{column: count}``. Both are optional.

    Pass GPU time is the sum of leaf samples in that pass. Leaves are events
    with no children. A parent that has children contributes only through
    those children, never through its own timestamp or counter sample. That
    is the Event Browser rule (CalculateTotalDuration replaces the marker
    with the sum of its children).

    When ``durations`` is None the geometric columns are still filled and
    ``gpu_ms`` stays None: milliseconds appear only after a replay sample.
    """
    flat = list(flat or [])
    if depth is None:
        depth = grouping_depth(flat)
    columns = _counter_columns(counters, counter_columns) if counters is not None else []
    if duration_supported is None:
        duration_supported = durations is not None
    sampled = durations is not None or counters is not None

    rolled = {}
    frame_seconds = 0.0
    if durations is not None:
        rolled, frame_seconds = rollup_durations(flat, durations)
    count_map = _eid_map(counters) if counters is not None else None

    buckets = {}
    order = []

    def bucket(key):
        if key not in buckets:
            buckets[key] = {
                "eid": _marker_eid(flat, key),
                "name": key,
                "draws": 0,
                "dispatches": 0,
                "clears": 0,
                "copies": 0,
                "cost": 0,
                "triangles": 0,
                "groups": 0,
                "gpu_seconds": 0.0 if durations is not None else None,
                "counters": {col: 0.0 for col in columns},
            }
            order.append(key)
        return buckets[key]

    for event in flat:
        if int(event.get("nchildren") or 0) != 0:
            continue
        key = _timing_key(event, depth)
        row = bucket(key)
        eid = int(event.get("eid") or 0)
        if durations is not None and eid in rolled:
            row["gpu_seconds"] += rolled[eid]
        if count_map is not None:
            for col in columns:
                row["counters"][col] += _leaf_count(count_map, eid, col)

    # Geometry counts every non-marker, matching pass_costs. Timing above
    # already skipped parents so their samples are not added again.
    for event in flat:
        if event.get("kind") == "marker":
            continue
        key = _group_key(event.get("marker") or "", depth)
        row = bucket(key)
        if event.get("kind") == "draw":
            cost = draw_cost(event)
            row["draws"] += 1
            row["cost"] += cost
            row["triangles"] += cost // 3
        elif event.get("kind") == "dispatch":
            row["dispatches"] += 1
            row["groups"] += dispatch_groups(event)
        elif event.get("kind") == "clear":
            row["clears"] += 1
        elif event.get("kind") == "copy":
            row["copies"] += 1

    totals = {
        "gpu_seconds": frame_seconds if durations is not None else None,
        "gpu_ms": (frame_seconds * 1000.0) if durations is not None else None,
        "draws": 0,
        "dispatches": 0,
        "triangles": 0,
        "counters": {col: 0.0 for col in columns},
    }
    rows = []
    for key in order:
        row = buckets[key]
        totals["draws"] += row["draws"]
        totals["dispatches"] += row["dispatches"]
        totals["triangles"] += row["triangles"]
        if durations is not None:
            seconds = row["gpu_seconds"]
            row["gpu_ms"] = seconds * 1000.0
            if frame_seconds > 0.0:
                row["pct"] = 100.0 * seconds / frame_seconds
            else:
                row["pct"] = 0.0
        else:
            row["gpu_ms"] = None
            row["pct"] = None
        for col in columns:
            totals["counters"][col] += row["counters"][col]
        rows.append(row)

    if durations is not None and duration_supported:
        rows.sort(
            key=lambda r: (r["gpu_seconds"] or 0.0, r["triangles"], r["dispatches"]),
            reverse=True,
        )
    else:
        rows.sort(key=lambda r: (r["cost"], r["groups"], r["draws"]), reverse=True)

    if limit is not None:
        rows = rows[: max(int(limit), 0)]

    if not sampled:
        note = UNSAMPLED_TIMING_NOTE
    elif not duration_supported:
        note = UNSUPPORTED_DURATION_NOTE
    else:
        note = SAMPLED_TIMING_NOTE

    report = {
        "sampled": sampled,
        "duration_supported": bool(duration_supported) if sampled else False,
        "depth": depth,
        "passes": rows,
        "totals": totals,
        "counter_columns": columns,
        "note": note,
    }
    report["text"] = format_pass_timings(report)
    return report


def format_pass_timings(report, level=1):
    """Markdown table. The first cell is the EID so jump-to-row keeps working."""
    report = report or {}
    totals = report.get("totals") or {}
    columns = list(report.get("counter_columns") or [])
    hashes = "#" * max(int(level), 1)
    lines = ["%s Pass timings" % hashes, ""]
    if totals.get("gpu_ms") is not None:
        lines.append("- Frame GPU: %s ms" % format_ms(totals.get("gpu_seconds")))
    lines.append("- Estimated triangles: %s" % _comma(totals.get("triangles") or 0))
    lines.append("- Draws: %s" % _comma(totals.get("draws") or 0))
    lines.append("- Dispatches: %s" % _comma(totals.get("dispatches") or 0))
    counter_totals = totals.get("counters") or {}
    for key in columns:
        lines.append("- %s: %s" % (_counter_label(key), _comma(round(counter_totals.get(key) or 0))))
    lines.append("")
    lines.append(report.get("note") or "")
    lines.append("")

    headers = ["EID", "Pass", "GPU ms", "% of frame", "Draws", "Dispatches", "Est. triangles"]
    headers.extend(_counter_label(key) for key in columns)
    aligns = ["----:", "------", "------:", "----------:", "------:", "-----------:", "---------------:"]
    aligns.extend(["------:"] * len(columns))
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("| " + " | ".join(aligns) + " |")
    passes = report.get("passes") or []
    if not passes:
        return "\n".join(lines) + "\n"
    for row in passes:
        eid = "" if row.get("eid") is None else str(row["eid"])
        cells = [
            eid,
            (row.get("name") or "").replace("|", "/"),
            format_ms(row.get("gpu_seconds")) if row.get("gpu_ms") is not None else "---",
            format_pct(row.get("pct")),
            _comma(row.get("draws") or 0),
            _comma(row.get("dispatches") or 0),
            _comma(row.get("triangles") or 0),
        ]
        cmap = row.get("counters") or {}
        for key in columns:
            cells.append(_comma(round(cmap.get(key) or 0)))
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def pass_costs(flat, limit=40, depth=None):
    if depth is None:
        depth = grouping_depth(flat)
    buckets = {}
    order = []
    for e in flat:
        if e["kind"] == "marker":
            continue
        key = _group_key(e["marker"], depth)
        if key not in buckets:
            buckets[key] = {
                "eid": _marker_eid(flat, key),
                "name": key,
                "draws": 0,
                "dispatches": 0,
                "clears": 0,
                "copies": 0,
                "cost": 0,
                "triangles": 0,
                "groups": 0,
            }
            order.append(key)
        b = buckets[key]
        if e["kind"] == "draw":
            cost = draw_cost(e)
            b["draws"] += 1
            b["cost"] += cost
            b["triangles"] += cost // 3
        elif e["kind"] == "dispatch":
            b["dispatches"] += 1
            b["groups"] += dispatch_groups(e)
        elif e["kind"] == "clear":
            b["clears"] += 1
        elif e["kind"] == "copy":
            b["copies"] += 1
    rows = [buckets[k] for k in order]
    rows.sort(key=lambda r: (r["cost"], r["groups"], r["draws"]), reverse=True)
    return rows[: max(int(limit), 0)]


def heaviest_draws(flat, limit=20):
    draws = []
    for e in flat:
        if e["kind"] != "draw":
            continue
        cost = draw_cost(e)
        draws.append(
            {
                "eid": e["eid"],
                "name": e["name"],
                "marker": e["marker"],
                "indices": int(e.get("num_indices") or 0),
                "instances": int(e.get("num_instances") or 0) or 1,
                "cost": cost,
                "triangles": cost // 3,
                "indirect": bool(e.get("indirect")),
            }
        )
    draws.sort(key=lambda d: (d["cost"], d["indices"]), reverse=True)
    return draws[: max(int(limit), 0)]


def search_events(flat, query, limit=50):
    q = (query or "").strip().lower()
    if not q:
        return []
    hits = []
    for e in flat:
        hay = (e["name"] + " " + e["marker"]).lower()
        if q in hay:
            hits.append(
                {
                    "eid": e["eid"],
                    "kind": e["kind"],
                    "name": e["name"],
                    "marker": e["marker"],
                }
            )
            if len(hits) >= int(limit):
                break
    return hits


def top_resources(rows, size_key, limit):
    ranked = sorted(
        rows or [],
        key=lambda r: int(r.get(size_key) or 0),
        reverse=True,
    )
    return ranked[: max(int(limit), 0)]


def _totals(flat, textures, buffers):
    draws = [e for e in flat if e["kind"] == "draw"]
    dispatches = [e for e in flat if e["kind"] == "dispatch"]
    tri = sum(draw_cost(e) // 3 for e in draws)
    indirect_zero = sum(1 for e in draws if e.get("indirect") and draw_cost(e) == 0)
    tex_bytes = sum(int(t.get("byte_size") or 0) for t in textures or [])
    buf_bytes = sum(int(b.get("length") or 0) for b in buffers or [])
    unknown_tex = sum(1 for t in textures or [] if not int(t.get("byte_size") or 0))
    return {
        "draws": len(draws),
        "dispatches": len(dispatches),
        "triangles": tri,
        "indirect_zero": indirect_zero,
        "textures": len(textures or []),
        "texture_bytes": tex_bytes,
        "unknown_textures": unknown_tex,
        "buffers": len(buffers or []),
        "buffer_bytes": buf_bytes,
    }


def format_passes(rows):
    if not rows:
        return "(no passes)\n"
    lines = [
        "| EID | Pass | Draws | Dispatches | Est. triangles | Thread groups |",
        "|----:|------|------:|-----------:|---------------:|--------------:|",
    ]
    for r in rows:
        eid = "" if r.get("eid") is None else str(r["eid"])
        lines.append(
            "| %s | %s | %s | %s | %s | %s |"
            % (
                eid,
                r["name"].replace("|", "/"),
                _comma(r["draws"]),
                _comma(r["dispatches"]),
                _comma(r["triangles"]),
                _comma(r["groups"]),
            )
        )
    return "\n".join(lines) + "\n"


def format_draws(rows):
    if not rows:
        return "(no draw calls)\n"
    lines = [
        "| EID | Est. triangles | Indices | Instances | Pass | Name |",
        "|----:|---------------:|--------:|----------:|------|------|",
    ]
    for d in rows:
        name = d["name"] + (" (indirect)" if d.get("indirect") else "")
        lines.append(
            "| %s | %s | %s | %s | %s | %s |"
            % (
                d["eid"],
                _comma(d["triangles"]),
                _comma(d["indices"]),
                _comma(d["instances"]),
                (d.get("marker") or "").replace("|", "/"),
                name.replace("|", "/"),
            )
        )
    return "\n".join(lines) + "\n"


def format_search(query, hits):
    lines = ["# Search: %s" % query, "", "%d match(es)" % len(hits), ""]
    if not hits:
        lines.append("(no events matched)")
        return "\n".join(lines) + "\n"
    lines.append("| EID | Kind | Pass | Name |")
    lines.append("|----:|------|------|------|")
    for h in hits:
        lines.append(
            "| %s | %s | %s | %s |"
            % (
                h["eid"],
                h["kind"],
                (h.get("marker") or "").replace("|", "/"),
                h["name"].replace("|", "/"),
            )
        )
    return "\n".join(lines) + "\n"


def _tex_size_label(t):
    w = int(t.get("width") or 0)
    h = int(t.get("height") or 0)
    d = int(t.get("depth") or 0)
    if d > 1:
        return "%dx%dx%d" % (w, h, d)
    return "%dx%d" % (w, h)


def format_textures(rows):
    if not rows:
        return "(no textures)\n"
    lines = [
        "| Bytes | Dimensions | Format | Mips | Name |",
        "|------:|------------|--------|-----:|------|",
    ]
    for t in rows:
        lines.append(
            "| %s | %s | %s | %s | %s |"
            % (
                format_bytes(t.get("byte_size") or 0),
                _tex_size_label(t),
                (t.get("format") or "").replace("|", "/"),
                int(t.get("mips") or 0),
                (t.get("name") or "").replace("|", "/"),
            )
        )
    return "\n".join(lines) + "\n"


def format_buffers(rows):
    if not rows:
        return "(no buffers)\n"
    lines = ["| Bytes | Name |", "|------:|------|"]
    for b in rows:
        lines.append(
            "| %s | %s |"
            % (format_bytes(b.get("length") or 0), (b.get("name") or "").replace("|", "/"))
        )
    return "\n".join(lines) + "\n"


def format_report(flat, textures, buffers, draw_limit=20, texture_limit=15, buffer_limit=10, pass_limit=40):
    totals = _totals(flat, textures, buffers)
    passes = pass_costs(flat, limit=pass_limit)
    draws = heaviest_draws(flat, limit=draw_limit)
    tex_rows = top_resources(textures, "byte_size", texture_limit)
    buf_rows = top_resources(buffers, "length", buffer_limit)
    note = (
        "Estimated triangles = indices/3 x instances. "
        "Triangle strips and topology other than triangle lists are approximate."
    )
    if totals["indirect_zero"]:
        note += (
            " %d indirect draw(s) reported 0 indices and are excluded from the triangle total."
            % totals["indirect_zero"]
        )
    if totals["unknown_textures"]:
        note += " %d texture(s) have an unknown byte size (shown as 0 B)." % totals["unknown_textures"]
    parts = [
        "# Frame hotspots",
        "",
        "- Draws: %s" % _comma(totals["draws"]),
        "- Dispatches: %s" % _comma(totals["dispatches"]),
        "- Estimated triangles: %s" % _comma(totals["triangles"]),
        "- Textures: %s (%s)" % (_comma(totals["textures"]), format_bytes(totals["texture_bytes"])),
        "- Buffers: %s (%s)" % (_comma(totals["buffers"]), format_bytes(totals["buffer_bytes"])),
        "",
        note,
        "",
        "## Passes",
        "",
        format_passes(passes).rstrip(),
        "",
        "## Heaviest draws",
        "",
        format_draws(draws).rstrip(),
        "",
        "## Largest textures",
        "",
        format_textures(tex_rows).rstrip(),
        "",
        "## Largest buffers",
        "",
        format_buffers(buf_rows).rstrip(),
        "",
    ]
    return "\n".join(parts)


def summarize(flat, textures, buffers, draw_limit=20, texture_limit=15, buffer_limit=10, pass_limit=40):
    return {
        "passes": pass_costs(flat, limit=pass_limit),
        "draws": heaviest_draws(flat, limit=draw_limit),
        "textures": top_resources(textures, "byte_size", texture_limit),
        "buffers": top_resources(buffers, "length", buffer_limit),
        "text": format_report(
            flat,
            textures,
            buffers,
            draw_limit=draw_limit,
            texture_limit=texture_limit,
            buffer_limit=buffer_limit,
            pass_limit=pass_limit,
        ),
    }


def select_stages(stages, stage_filter):
    q = (stage_filter or "").strip().lower()
    if not q:
        return list(stages or [])
    hits = []
    for s in stages or []:
        if q in (s.get("stage") or "").lower() or q in (s.get("entry") or "").lower():
            hits.append(s)
    return hits


def format_shader_stages(stages, stage_filter="", max_chars=8000):
    chosen = select_stages(stages, stage_filter)
    if stage_filter and not chosen:
        names = ", ".join(s.get("stage") or "?" for s in stages or []) or "(none bound)"
        return "No shader stage matched '%s'. Bound stages: %s\n" % (stage_filter, names)
    if not chosen:
        return "(No shader bound at this event.)\n"
    cap = max(int(max_chars), 200)
    blocks = []
    for s in chosen:
        lines = [
            "=== %s  entry=%s  id=%s ==="
            % (s.get("stage") or "?", s.get("entry") or "", s.get("resource_id") or "")
        ]
        cbs = s.get("constant_blocks") or []
        lines.append("constant blocks: %s" % (", ".join(cbs) if cbs else "(none)"))
        texs = s.get("textures") or []
        lines.append("read-only resources: %s" % (", ".join(texs) if texs else "(none)"))
        samps = s.get("samplers") or []
        lines.append("samplers: %s" % (", ".join(samps) if samps else "(none)"))
        dis = s.get("disassembly") or ""
        if dis:
            if len(dis) > cap:
                dis = dis[:cap] + "\n... (disassembly truncated)\n"
            lines.append("--- disassembly ---")
            lines.append(dis.rstrip())
        blocks.append("\n".join(lines))
    return "\n\n".join(blocks) + "\n"
