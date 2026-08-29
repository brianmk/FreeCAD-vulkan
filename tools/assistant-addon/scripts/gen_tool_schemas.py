#!/usr/bin/env python3
"""gen_tool_schemas - build vendor/tool_schemas.json from the FastMCP tool list.

FreeCAD's embedded Python does NOT ship the `mcp` package, so the assistant cannot
query the server at runtime.  Instead we enumerate the tool JSON Schemas here (in
a venv that has `mcp`) and ship them with the addon.  The ToolRegistry executes
the matching call in-process against the vendored guest registry.

Run with a venv Python that has the `mcp` package:
    /tmp/opencode/mcp-venv/bin/python scripts/gen_tool_schemas.py

Use alongside scripts/sync_guest.py so names stay in lockstep.
"""

import asyncio
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_SERVER = os.path.normpath(os.path.join(
    HERE, "..", "..", "..", "tools", "fcprobe", "mcp", "freecad_mcp_server.py"))
VENDOR = os.path.normpath(os.path.join(HERE, "..", "Assistant", "vendor"))


def main():
    try:
        from mcp.server.fastmcp import FastMCP  # noqa: F401
    except ImportError:
        raise SystemExit("this script needs the 'mcp' package - run it with a venv python that has it")

    sys.path.insert(0, os.path.dirname(REPO_SERVER))
    import importlib.util
    spec = importlib.util.spec_from_file_location("_fcmcp_server", REPO_SERVER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)          # only defines/registers tools - no I/O
    mcp = mod.mcp

    tools = asyncio.run(mcp.list_tools())
    schema = [{
        "name": t.name,
        "description": t.description or "",
        "inputSchema": _plain(t.inputSchema),
    } for t in tools]
    schema.sort(key=lambda x: x["name"])

    os.makedirs(VENDOR, exist_ok=True)
    dst = os.path.join(VENDOR, "tool_schemas.json")
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(schema, fh, indent=2)
    print(f"[gen_tool_schemas] wrote {dst} with {len(schema)} tools")


def _plain(obj):
    """FastMCP may return pydantic/serializable dicts - normalise to plain dicts."""
    if isinstance(obj, dict):
        return {k: _plain(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_plain(v) for v in obj]
    return obj


if __name__ == "__main__":
    main()
