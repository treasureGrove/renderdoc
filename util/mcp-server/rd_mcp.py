#!/usr/bin/env python3
"""RenderDoc MCP server (stdio).

Zero-dependency Model Context Protocol server that bridges to a running
qrenderdoc instance via the rd_mcp_bridge.py extension (localhost TCP).

Run it as an MCP server in your AI client, e.g. for ZCode / Claude Code:

    {
      "mcpServers": {
        "renderdoc": {
          "command": "python",
          "args": ["<repo>/util/mcp-server/rd_mcp.py"]
        }
      }
    }

MCP stdio framing here is newline-delimited JSON-RPC 2.0, which is what the
protocol expects on stdio. No third-party packages are required.
"""

import json
import os
import socket
import sys

HOST = "127.0.0.1"
PORT = int(os.environ.get("RD_MCP_PORT", "39944"))
PROTOCOL_VERSION = "2024-11-05"

TOOLS = [
    {
        "name": "capture_info",
        "description": (
            "Get info about the capture currently loaded in the running RenderDoc GUI: "
            "filename, graphics API, current event ID and action counts. Cheap call - "
            "start here to check the bridge is alive and a capture is loaded."
        ),
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "list_events",
        "description": (
            "Return the frame event tree of the loaded capture as indented text: markers, "
            "draw calls, dispatches, clears, copies and presents with their EIDs. Use first "
            "when analysing the rendering pipeline, then fetch_eid on interesting draws."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "max_depth": {
                    "type": "integer",
                    "description": "How deep to descend the marker hierarchy (default 4).",
                    "default": 4,
                },
                "max_lines": {
                    "type": "integer",
                    "description": "Maximum number of tree lines returned (default 3000).",
                    "default": 3000,
                },
            },
            "required": [],
        },
    },
    {
        "name": "fetch_eid",
        "description": (
            "Switch the RenderDoc UI to the given event ID and return a summary: action "
            "name, draw/dispatch parameters, bound colour outputs and depth buffer with "
            "their dimensions and formats."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "eid": {"type": "integer", "description": "The event ID to inspect."}
            },
            "required": ["eid"],
        },
    },
    {
        "name": "get_texture",
        "description": (
            "Save the colour outputs (and depth buffer) of the given event as PNG files on "
            "disk and return their paths. Renders through the replay thread. You can then "
            "read the PNG files to visually inspect the frame."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "eid": {
                    "type": "integer",
                    "description": "Event ID to grab (default: the current event).",
                },
                "dest_dir": {
                    "type": "string",
                    "description": "Directory to write PNGs into (default: system temp).",
                },
            },
            "required": [],
        },
    },
    {
        "name": "read_texture_b64",
        "description": (
            "Save the given colour output (or depth buffer) of an event as PNG and return it "
            "base64-encoded, downscaled so the longest edge is <= max_size (default 512). Use "
            "when you need the image content itself rather than a file path."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "eid": {"type": "integer", "description": "Event ID (default: current event)."},
                "output_index": {
                    "type": "integer",
                    "description": "Colour output index (default 0). Ignored when depth=true.",
                },
                "depth": {"type": "boolean", "description": "Grab depth buffer instead."},
                "max_size": {
                    "type": "integer",
                    "description": "Longest edge cap in pixels (default 512).",
                },
            },
            "required": [],
        },
    },
    {
        "name": "pixel_history",
        "description": (
            "Query the pixel history of one pixel on the event's first colour output: every "
            "modification across the frame with pre/post values and rejection flags (discard, "
            "depth/stencil fail, culled). Best way to explain why a pixel looks the way it does."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "eid": {"type": "integer", "description": "Event ID (default: current event)."},
                "x": {"type": "integer", "description": "Pixel X coordinate."},
                "y": {"type": "integer", "description": "Pixel Y coordinate."},
            },
            "required": ["x", "y"],
        },
    },
    {
        "name": "frame_hotspots",
        "description": (
            "Rank the loaded capture: per-pass draw/dispatch cost, the heaviest draws by "
            "index count times instances, and the largest textures and buffers. When the "
            "Pass Timings panel (or pass_timings) has sampled GPU counters, the report also "
            "includes per-pass milliseconds and invocation totals from that cached sample. "
            "Set refresh=true to replay the frame and resample; that is slow and the numbers "
            "are not stored in the .rdc. The 'text' field is a markdown report."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "limit": {
                    "type": "integer",
                    "description": "How many draws and resources to include (default 20).",
                    "default": 20,
                },
                "refresh": {
                    "type": "boolean",
                    "description": (
                        "Replay the frame and resample GPU counters before building the report. "
                        "Default false reuses the last sample."
                    ),
                    "default": False,
                },
            },
            "required": [],
        },
    },
    {
        "name": "pass_timings",
        "description": (
            "Per-pass GPU milliseconds, percent of frame, draws, dispatches, estimated "
            "triangles, and whichever of PS/CS/VS invocations, samples passed, and IA "
            "primitives the driver returned. Marker time is the sum of child events, matching "
            "the Event Browser, so a parent timestamp is not added on top. Without refresh, "
            "returns the geometric estimate plus the last cached sample if there is one. "
            "refresh=true replays the frame via FetchCounters (slow; not stored in the .rdc)."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "refresh": {
                    "type": "boolean",
                    "description": (
                        "Replay the frame and fetch GPU counters. Default false reuses the cache."
                    ),
                    "default": False,
                },
                "limit": {
                    "type": "integer",
                    "description": "Maximum passes returned (default: all).",
                },
            },
            "required": [],
        },
    },
    {
        "name": "pass_costs",
        "description": (
            "Summarise each marker pass: draw count, dispatch count, estimated triangles "
            "(indices/3 x instances) and compute thread groups, sorted by triangle cost. "
            "The EID column is the marker to jump to."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "limit": {
                    "type": "integer",
                    "description": "Maximum passes returned (default 40).",
                    "default": 40,
                }
            },
            "required": [],
        },
    },
    {
        "name": "heaviest_draws",
        "description": (
            "List the most expensive draw calls, ranked by index count times instance count. "
            "Use the returned EID with fetch_eid or get_texture to inspect a hotspot."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "limit": {
                    "type": "integer",
                    "description": "How many draws to return (default 20).",
                    "default": 20,
                }
            },
            "required": [],
        },
    },
    {
        "name": "search_events",
        "description": (
            "Find events whose name or parent marker contains a substring, for example "
            "'shadow', 'gbuffer', or 'bloom'. Returns EIDs to pass to fetch_eid."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Case-insensitive substring."},
                "limit": {
                    "type": "integer",
                    "description": "Maximum matches (default 50).",
                    "default": 50,
                },
            },
            "required": ["query"],
        },
    },
    {
        "name": "resource_census",
        "description": (
            "List the largest textures and buffers in the capture by byte size, plus totals. "
            "Use this to see what is occupying VRAM before opening individual resources."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "limit": {
                    "type": "integer",
                    "description": "How many textures and buffers to list (default 20).",
                    "default": 20,
                }
            },
            "required": [],
        },
    },
    {
        "name": "get_shader",
        "description": (
            "Switch to an event and return the bound shader stages: entry point, constant "
            "blocks, read-only resources, samplers, and truncated disassembly. Filter with "
            "stage (vertex, pixel, compute, or an entry-point substring)."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "eid": {"type": "integer", "description": "Event ID (default: current event)."},
                "stage": {
                    "type": "string",
                    "description": "Optional stage or entry-point filter, e.g. 'pixel'.",
                },
                "disassembly": {
                    "type": "boolean",
                    "description": "Include disassembly text (default true).",
                    "default": True,
                },
                "max_chars": {
                    "type": "integer",
                    "description": "Per-stage disassembly cap (default 8000, max 40000).",
                    "default": 8000,
                },
            },
            "required": [],
        },
    },
    {
        "name": "run_python",
        "description": (
            "Execute arbitrary python code inside the RenderDoc GUI (UI thread) and return "
            "its stdout/stderr. Globals available: pyrenderdoc (the CaptureContext), "
            "renderdoc, qrenderdoc. Set a variable named 'result' to return a value. Use "
            "for anything the dedicated tools cannot express."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {"type": "string", "description": "Python source code to execute."}
            },
            "required": ["code"],
        },
    },
]


def call_bridge(tool, args, timeout):
    sock = socket.create_connection((HOST, PORT), timeout=3)
    try:
        sock.settimeout(timeout)
        f = sock.makefile("rwb")
        req = {"id": 1, "tool": tool, "args": args}
        f.write((json.dumps(req) + "\n").encode("utf-8"))
        f.flush()
        line = f.readline()
        if not line:
            raise RuntimeError("bridge closed the connection without responding")
        resp = json.loads(line.decode("utf-8"))
        if not resp.get("ok"):
            raise RuntimeError(str(resp.get("error", "unknown bridge error")))
        return resp.get("result")
    finally:
        try:
            sock.close()
        except OSError:
            pass


def tool_text_result(text, is_error=False):
    result = {"content": [{"type": "text", "text": text}]}
    if is_error:
        result["isError"] = True
    return result


def handle_request(req):
    method = req.get("method", "")
    rid = req.get("id")

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": rid,
            "result": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "renderdoc-mcp", "version": "0.3.0"},
            },
        }

    if method == "ping":
        return {"jsonrpc": "2.0", "id": rid, "result": {}}

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": rid, "result": {"tools": TOOLS}}

    if method == "tools/call":
        params = req.get("params") or {}
        name = str(params.get("name", ""))
        args = dict(params.get("arguments") or {})
        if name in ("run_python", "get_shader", "pass_timings", "frame_hotspots"):
            timeout = float(args.get("timeout", 180) or 180)
            args["timeout"] = timeout
        else:
            timeout = float(args.pop("timeout", 60) or 60)
        try:
            result = call_bridge(name, args, timeout)
            text = json.dumps(result, ensure_ascii=False, indent=2)
            return {"jsonrpc": "2.0", "id": rid, "result": tool_text_result(text)}
        except Exception as e:
            text = (
                "RenderDoc bridge error: %s\n\nMake sure qrenderdoc is running with the "
                "rd_mcp_bridge.py extension loaded (see util/mcp-server/README.md)." % e
            )
            return {"jsonrpc": "2.0", "id": rid, "result": tool_text_result(text, True)}

    if method.startswith("notifications/"):
        return None

    if rid is None:
        return None
    return {
        "jsonrpc": "2.0",
        "id": rid,
        "error": {"code": -32601, "message": "method not found: %s" % method},
    }


def main():
    for raw in sys.stdin.buffer:
        line = raw.strip()
        if not line:
            continue
        try:
            req = json.loads(line.decode("utf-8"))
        except Exception:
            continue
        resp = handle_request(req)
        if resp is None:
            continue
        sys.stdout.buffer.write((json.dumps(resp) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
