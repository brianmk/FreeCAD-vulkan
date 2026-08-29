#!/usr/bin/env python3
"""assistant_probe - headless test of the Assistant addon with a mock LLM.

Launches FreeCAD (GUI) with the addon loaded, drives the chat panel with a mock
provider (no network), and asserts:
  1. the Assistant workbench + toggle command are discovered;
  2. the dock toggles open;
  3. a single tool_inspect round executes a real tool in-process (list_objects)
     and renders a 'ran' tool card plus a final message.

Run:  python3 tools/assistant-addon/tests/assistant_probe.py
Exits 0 on pass, 1 on fail.
"""

import asyncio
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, "/tmp/opencode/mcp-venv/lib/python3.11/site-packages")
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = "/home/phantom/dev/FreeCAD"
FREECAD = ROOT + "/build/debug/bin/FreeCAD"
GUEST = ROOT + "/tools/fcprobe/mcp/freecad_mcp_guest.py"
SERVER = ROOT + "/tools/fcprobe/mcp/freecad_mcp_server.py"
PY = "/tmp/opencode/mcp-venv/bin/python"
SOCK = "/tmp/opencode/freecad_mcp.sock"
ENV = dict(os.environ, LD_LIBRARY_PATH="/tmp/opencode/boost91",
           QT_STYLE_OVERRIDE="fusion", QT_QPA_PLATFORM="xcb", DISPLAY=":0")

failures = []


def check(cond, label):
    print(("PASS" if cond else "FAIL"), "-", label)
    if not cond:
        failures.append(label)


def launch():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    log = open("/tmp/opencode/fc_console.log", "w")
    p = subprocess.Popen([FREECAD, GUEST], env=ENV, stdin=subprocess.DEVNULL,
                         stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    for _ in range(240):
        if os.path.exists(SOCK):
            break
        time.sleep(0.5)
    if not os.path.exists(SOCK):
        p.kill()
        sys.exit("FreeCAD failed to create socket")
    time.sleep(3)
    return p


async def main():
    fc = launch()
    try:
        async with stdio_client(StdioServerParameters(command=PY, args=[SERVER], env=ENV)) as (r, w):
            async with ClientSession(r, w) as s:
                await s.initialize()

                async def py(code):
                    res = await s.call_tool("run_python", {"code": code, "mode": "exec"})
                    raw = "".join(getattr(c, "text", "") or "" for c in res.content)
                    try:
                        return json.loads(raw)
                    except Exception:
                        return {"raw": raw}

                # 1. discovery
                out = await py(
                    "import FreeCADGui as Gui\n"
                    "print('WB=' + repr([x for x in Gui.listWorkbenches() if 'Assistant' in x]))\n"
                    "print('CMD=' + repr([c for c in Gui.listCommands() if c.startswith('Assistant')]))\n")
                check("AssistantWorkbench" in out.get("stdout", ""), "workbench discovered")
                check("Assistant_Toggle" in out.get("stdout", ""), "toggle command discovered")

                # 2. toggle open
                # 2. dock is visible by default (auto-installed), locked under Tasks
                out = await py(
                    "import FreeCADGui as Gui, time\n"
                    "from init_impl import get_panel\n"
                    "from PySide import QtWidgets\n"
                    "p=get_panel(); time.sleep(0.3)\n"
                    "v=p.isVisible()\n"
                    "f=p.features()\n"
                    "locked = not bool(f & QtWidgets.QDockWidget.DockWidgetFloatable) and not bool(f & QtWidgets.QDockWidget.DockWidgetMovable)\n"
                    "mw=Gui.getMainWindow()\n"
                    "tasks=mw.findChild(QtWidgets.QDockWidget,'Tasks')\n"
                    "tabbed = bool(tasks and (p in mw.tabifiedDockWidgets(tasks)))\n"
                    "print('DEFAULT_VISIBLE=' + str(v))\n"
                    "print('LOCKED=' + str(locked))\n"
                    "print('TABBED_TASKS=' + str(tabbed))\n")
                check("DEFAULT_VISIBLE=True" in out.get("stdout", ""), "dock visible by default")
                check("LOCKED=True" in out.get("stdout", ""), "dock locked (no float/move)")
                check("TABBED_TASKS=True" in out.get("stdout", ""), "dock tabified with Tasks")

                # toggle off then on (command still works)
                out = await py(
                    "import FreeCADGui as Gui, time\n"
                    "from init_impl import get_panel\n"
                    "p=get_panel()\n"
                    "Gui.runCommand('Assistant_Toggle'); time.sleep(0.4); a=p.isVisible()\n"
                    "Gui.runCommand('Assistant_Toggle'); time.sleep(0.4); b=p.isVisible()\n"
                    "print('OFF_THEN_ON=' + str((not a) and b))\n")
                check("OFF_THEN_ON=True" in out.get("stdout", ""), "toggle command hides/shows")

                # 3. mock single-round tool use
                out = await py(
                    "from init_impl import get_panel\n"
                    "import Preferences as P\n"
                    "P.set('Mode', P.MODE_AUTO)\n"
                    "p=get_panel(); p._agent.clear_history()\n"
                    "script=[{'type':'tool_calls','calls':[{'name':'list_objects','arguments':{}}]},\n"
                    "        [{'type':'text_delta','delta':'Scene: ok.'},\n"
                    "         {'type':'done','content':'Scene: ok.','finish_reason':'stop'}]]\n"
                    "p._agent.send('inspect', mock=True, mock_script=script)\n"
                    "print('STARTED')\n")
                check("STARTED" in out.get("stdout", ""), "mock agent started")

                time.sleep(4.0)  # let worker thread + Qt loop process the tool call
                out = await py(
                    "from init_impl import get_panel\n"
                    "p=get_panel()\n"
                    "html=''.join(p._blocks)\n"
                    "print('RAN=' + str('list_objects' in html and '(ran)' in html))\n"
                    "print('RESULT=' + str('list_objects' in html))\n"
                    "print('FINAL=' + str('Scene: ok.' in html))\n")
                check("RAN=True" in out.get("stdout", ""), "tool card rendered with ran status")
                check("RESULT=True" in out.get("stdout", ""), "tool result rendered")
                check("FINAL=True" in out.get("stdout", ""), "final assistant text rendered")
    finally:
        fc.terminate()
        try:
            fc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            fc.kill()
        try:
            os.unlink(SOCK)
        except FileNotFoundError:
            pass

    if failures:
        print("\n%d FAILURE(S):" % len(failures))
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print("\nALL CHECKS PASSED")


if __name__ == "__main__":
    asyncio.run(main())
