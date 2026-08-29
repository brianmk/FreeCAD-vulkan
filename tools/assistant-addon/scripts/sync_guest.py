#!/usr/bin/env python3
"""sync_guest - vendor a copy of the FreeCAD MCP guest into the addon.

The addon must be self-contained (it ships to a user Mod dir with no access to
this repo), so we embed the in-FreeCAD tool registry.  We ALSO neutralise the
guest's auto-start block so that importing it *inside* FreeCAD does not spin up
the socket listener - the addon calls ``handle_request`` directly.

Run this whenever tools/fcprobe/mcp/freecad_mcp_guest.py changes.
"""

import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_GUEST = os.path.normpath(os.path.join(
    HERE, "..", "..", "..", "tools", "fcprobe", "mcp", "freecad_mcp_guest.py"))
VENDOR = os.path.normpath(os.path.join(HERE, "..", "Assistant", "vendor"))

GUARDED_TAIL = '''
# --- addon guard: the vendored guest must NOT auto-run the socket server ---
# The host-side server runner sets FC_MCP_GUEST_SERVER=1 when it genuinely wants
# the listener.  Imported by the Assistant addon (inside FreeCAD) it stays quiet
# so the ToolRegistry can dispatch tool calls in-process via handle_request().
if "FreeCAD" in sys.modules and os.environ.get("FC_MCP_GUEST_SERVER") == "1":
    run_guest()
'''


def main():
    os.makedirs(VENDOR, exist_ok=True)
    with open(REPO_GUEST, "r", encoding="utf-8") as fh:
        src = fh.read()
    marker = 'if "FreeCAD" in sys.modules:'
    idx = src.find(marker)
    if idx == -1:
        raise SystemExit(f"auto-run marker not found in {REPO_GUEST}")
    src = src[:idx] + GUARDED_TAIL.lstrip("\n")
    dst = os.path.join(VENDOR, "freecad_mcp_guest.py")
    with open(dst, "w", encoding="utf-8") as fh:
        fh.write(src)
    print(f"[sync_guest] wrote {dst} ({len(src)} bytes, HEAD {os.path.basename(REPO_GUEST)})")


if __name__ == "__main__":
    main()
