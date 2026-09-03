#!/usr/bin/env python3
"""Validate the full-resolve (ptForceFullResolve) fix for the interactive PT.

Builds a grid, enters RT, then ORBITS the camera once so a view-change reset
latches ptForceFullResolve.  Under the fix the per-pixel adaptive freeze stays
OFF for the whole post-move accumulation, so the run must reach the sample cap
(fill=1 through frameIndex == maxSamp-1); the freeze may only return at the cap.
Before the fix the run converged at ~minSamps with fill=0, idling on a faint
image.  Requires FC_VULKAN_RT_DEBUG=1 (the [RTDBG] adaptive line is gated on it).
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
N = int(os.environ.get("FC_PROFILE_N", "8"))


def log(msg):
    print("FULLRESOLVE n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="fullresolve")
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
    log("orbited %d deg" % degrees)


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4=RayTracing: RT gate
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 4)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("FullResolve")
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
                                   6.0),
                    FreeCAD.Rotation())
        doc.recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("build")
        log("phase=build n=%d" % N)
    elif k == 14:
        orbit(30)
        s.frame_phase("moved")
        log("phase=moved")
    elif k == 70:
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
