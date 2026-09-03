#!/usr/bin/env python3
"""Reproduce the hover-selection highlight in RT and dump the overlay camera.

Builds a grid, enters RT, then hovers the mouse over the centre box so the
viewer raises a PRESELECTION highlight (an OVERLAY command).  With
FC_VULKAN_OVERLAY_CAM_DEBUG the backend logs [OVCAM-FULL] projection matrices
for the highlight vs the frame (params) camera, letting us check whether the
highlight is projected through a stale/different view or projection (seen as
glitched/offset highlighted edges and mismatch with the Coin pick ray).
"""

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
    print("HOVER n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="hover")
steps = [0]


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
        s.set_pref(VIEW, "VulkanPathTracingSettle", 6)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Hover")
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
        view.viewIsometric()
        view.fitAll()
        log("built n=%d" % N)
    elif k == 18:
        s._relocate_viewport()
        # Centre of the viewport should land on a box near the grid centre.
        cx = s.width * 0.5
        cy = s.height * 0.5
        log("hovering at %.0f,%.0f (viewport %dx%d)" % (cx, cy, s.width, s.height))
        s.move(cx, cy)
        # Give the highlight/overlay a couple of frames to record + render.
    elif k == 22:
        # Off to the side -> should clear preselection.
        s.move(s.width * 0.05, s.height * 0.9)
    elif k == 26:
        # Hover a visible box edge on the left-ish region.
        s.move(s.width * 0.35, s.height * 0.4)
    elif k == 32:
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(350, step)


QtCore.QTimer.singleShot(400, step)
