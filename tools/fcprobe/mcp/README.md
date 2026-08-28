# FreeCAD MCP server

An [MCP](https://modelcontextprotocol.io) server that drives a **live FreeCAD**
over a Unix socket. It can build sketches of any shape, extrude/pad/pocket,
thread holes, mirror features, read & write the selection, read/move the
viewport cursor, and inspect the process logs.

The work splits across two processes:

| file                       | runs in                          | role                                   |
|----------------------------|----------------------------------|----------------------------------------|
| `freecad_mcp_guest.py`     | **inside FreeCAD**               | receives RPC over a Unix socket, performs the work on FreeCAD's main thread |
| `freecad_mcp_server.py`    | **host Python** (the MCP server) | exposes the MCP tools; forwards each call to the guest |

```
MCP client (Claude/Cursor/OpenCode)
        │  stdio (JSON-RPC)
        ▼
freecad_mcp_server.py   ──Unix socket──▶  freecad_mcp_guest.py  (inside FreeCAD)
```

## Prerequisites

- The FreeCAD build (this repo): `build/debug/bin/FreeCAD`.
- A Python venv with the MCP SDK (v1.x `FastMCP`):

```bash
python3 -m venv /tmp/opencode/mcp-venv
/tmp/opencode/mcp-venv/bin/pip install 'mcp<2'
```

- The debug build needs the boost lib path on `LD_LIBRARY_PATH`
  (`/tmp/opencode/boost91` here) and `QT_STYLE_OVERRIDE=fusion` for the GUI.

## Launching

### 1. Start the guest inside FreeCAD

**Live GUI** (recommended — you watch the model being built; cursor, selection,
screenshots all work):

```bash
env LD_LIBRARY_PATH=/tmp/opencode/boost91 QT_STYLE_OVERRIDE=fusion \
    QT_QPA_PLATFORM=xcb build/debug/bin/FreeCAD tools/fcprobe/mcp/freecad_mcp_guest.py
```

**Headless** (no window; everything except cursor/view/screenshot/selection):

```bash
env LD_LIBRARY_PATH=/tmp/opencode/boost91 QT_STYLE_OVERRIDE=fusion \
    build/debug/bin/FreeCADCmd tools/fcprobe/mcp/freecad_mcp_guest.py
```

You should see `/tmp/opencode/freecad_mcp.sock` appear (override with
`FC_MCP_SOCKET=/path`).

### 2. Start the MCP server

```bash
/tmp/opencode/mcp-venv/bin/python tools/fcprobe/mcp/freecad_mcp_server.py
```

To have the server launch FreeCAD itself instead (and wait for the socket):

```bash
/tmp/opencode/mcp-venv/bin/python tools/fcprobe/mcp/freecad_mcp_server.py --spawn
# GUI FreeCAD; add --headless to launch FreeCADCmd, --env K=V to pass env through
```

## MCP client configuration

**OpenCode** (`~/.config/opencode/opencode.json`):

```json
{
  "mcp": {
    "freecad": {
      "command": "/tmp/opencode/mcp-venv/bin/python",
      "args": ["/home/phantom/dev/FreeCAD/tools/fcprobe/mcp/freecad_mcp_server.py"],
      "env": {
        "LD_LIBRARY_PATH": "/tmp/opencode/boost91",
        "QT_STYLE_OVERRIDE": "fusion",
        "QT_QPA_PLATFORM": "xcb"
      }
    }
  }
}
```

**Claude Desktop** (`~/Library/Application Support/Claude/claude_desktop_config.json`
on macOS, `~/.config/Claude/claude_desktop_config.json` on Linux):

```json
{
  "mcpServers": {
    "freecad": {
      "command": "/tmp/opencode/mcp-venv/bin/python",
      "args": ["/home/phantom/dev/FreeCAD/tools/fcprobe/mcp/freecad_mcp_server.py"],
      "env": {
        "LD_LIBRARY_PATH": "/tmp/opencode/boost91",
        "QT_STYLE_OVERRIDE": "fusion",
        "QT_QPA_PLATFORM": "xcb"
      }
    }
  }
}
```

The MCP server auto-starts the guest via `--spawn` if you prefer not to launch
it by hand.

## Tools (41)

Modelling: `new_document`, `open_document`, `active_document`,
`set_active_document`, `list_objects`, `delete_object`, `recompute`,
`get_placement`, `set_placement`.

Sketches ("any shape"): `new_sketch` (plane XY/XZ/YZ; body auto/None), then
`sketch_rectangle`, `sketch_polygon`, `sketch_polyline`, `sketch_line`,
`sketch_circle`, `sketch_arc`, `sketch_spline`, `add_constraint`.

Features: `pad`, `pocket`, `extrude` (Part workbench), `add_hole`
(`threaded=True` sets cosmetic threads, `model_thread=True` generates real
threads, `throughall`, `hole_cut`, `drill_point`), `mirror` (PartDesign
`Mirrored` about a body plane), `mirror_object` (Part `Mirroring`).

Selection & cursor: `get_selection`, `select_objects`, `clear_selection`,
`get_cursor` (global/viewport/device px + hover), `move_cursor` (warp to a
viewport point), `click`.

View / GUI: `set_view`, `fit_view`, `screenshot`, `set_workbench`, `run_command`.

Logs: `get_log`, `log`, `clear_log`, plus `snapshot` (viewport + doc + camera +
selection) and `run_python` (an eval/exec escape hatch).

## Logs

`freecad_mcp_guest.py` captures the FreeCAD process's stdout/stderr **at the
fd level** at startup, so every C++ / Qt / Python console message lands in an
in-memory ring buffer (default 8000 lines, `FC_MCP_LOG_LINES`). Read it with:

```python
await session.call_tool("get_log", {"limit": 200, "tail": True})
```

The captured bytes are also echoed back to the original stdout, so a user's
terminal keeps showing everything. `log(level, message)` writes a line into the
FreeCAD console *and* into the buffer; `clear_log` empties it.

## Notes

- The guest serializes every request onto FreeCAD's main thread (a Qt `QTimer`
  pump in the GUI, a drain loop when headless), so scene/UI mutations never race
  the viewport.
- Cursor / view / screenshot / GUI selection need the **GUI** FreeCAD; in a
  headless `FreeCADCmd` those tools report a clear "requires GUI" error while
  all modelling tools keep working.
- Passing the selection yourself: `select_objects(["Pad"])` then
  `get_selection()` mirrors the probe pick behaviour; in the GUI use
  `move_cursor` + `click(x, y)` to pick interactively.
