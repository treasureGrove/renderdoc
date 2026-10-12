"""Pure helpers for frame cost, resource size, and shader summaries.

No dependency on the renderdoc module, so the ranking and report text can be
unit-tested without a capture or a GUI. The MCP bridge turns live capture
objects into the dicts these functions expect.
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
    """Depth-first list of action dicts with a 'marker' path of parent names."""
    stack = list(stack or [])
    out = []
    for a in actions or []:
        rec = {
            "eid": int(a.get("eid") or 0),
            "name": a.get("name") or a.get("kind") or "event",
            "kind": a.get("kind") or "other",
            "num_indices": int(a.get("num_indices") or 0),
            "num_instances": int(a.get("num_instances") or 0),
            "dispatch": a.get("dispatch"),
            "indirect": bool(a.get("indirect")),
            "marker": " / ".join(stack),
        }
        out.append(rec)
        child_stack = stack
        if rec["kind"] == "marker":
            child_stack = stack + [rec["name"]]
        out.extend(flatten(a.get("children") or [], child_stack))
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
