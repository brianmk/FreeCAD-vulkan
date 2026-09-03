#!/usr/bin/env python3
"""Build the wave grid and STAY OPEN, continuously orbiting (live/refining).

Keeps the path tracer in a live adaptive state (never fully idle) so the
rendered canvas can be screen-captured externally to reproduce the
far-boxes-drawn-as-edges-only bug.  Rings a bell every 40 steps so the wrapper
can tell it is alive.  Never closes.
"""

import math
import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
N = int(os.environ.get("FC_PROFILE_N", "20"))


def log(msg):
    print("LIVE n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="live")
steps = [0]


def orbit(degrees):
    from pivy import coin
    view = FreeCADGui.ActiveDocument.ActiveView
    cam = view.getCameraNode()
    pos = cam.position.getValue()
    angle = math.radians(degrees)
    x = pos[0] * math.cos(angle) - pos[1] * math.sin(angle)
    y = pos[0] * math.sin(angle) + pos[1] * math.cos(angle)
    cam.position.setValue(x, y, pos[2])
    cam.pointAt(coin.SbVec3f(0.0, 0.0, 0.0))


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 8)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Live")
        stepSize = 12
        for i in range(N):
            for j in range(N):
                b = doc.addObject("Part::Box", "b%d_%d" % (i, j))
                b.Length = 4
                b.Width = 4
                b.Height = 4
                b.Placement = FreeCAD.Placement(
                    FreeCAD.Vector(i * stepSize - N * stepSize / 2.0,
                                   j * stepSize - N * stepSize / 2.0,
                                   6.0 * (1.0 + (i % 5) / 4.0)),
                    FreeCAD.Rotation())
        doc.recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        log("built n=%d ALIVE" % N)
    elif k <= 6:
        orbit(1.2)
        if k in (2, 4, 6):
            print("ALIVE step=%d (moving)" % k, flush=True)
    else:
        # Flatten back out / static from here: let the tracer fully settle and
        # idle, then the frame stays up so it can be screen-captured static.
        if k in (20, 40, 80, 120):
            print("ALIVE step=%d (static, settling)" % k, flush=True)
    QtCore.QTimer.singleShot(200, step)


QtCore.QTimer.singleShot(400, step)
