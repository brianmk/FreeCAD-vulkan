#!/usr/bin/env python3
"""Verify temporal reprojection: converged samples survive camera moves.

Sequence (single-camera-change steps, no continuous animation):
  1. static accumulation builds history
  2. small camera orbit     -> [RTDBG] ptState reproject=1, accum stays 1
     (no preview drop) and the adaptive line reports most pixels
     reprojected (reprojected close to the total)
  3. 90-degree orbit        -> reproject frame again, but nearly every
     pixel rejects the history (disocclusion): reprojected ~0, no ghosting
  4. fraction recovers below 1.0 as convergence resumes

Control run (FC_VULKAN_PT_TEMPORAL=0): no reproject=1 lines, the small
move drops to preview (accum=0).

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_PT_MAXSAMPLES=64 \\
      FreeCAD vk_temporal_probe.py
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


def log(msg):
    print("TEMPORAL " + msg, flush=True)


s = Session(name="temporal")
steps = [0]


def orbit(degrees):
    """Rotate the camera around the world Z axis (viewTop has the camera
    on +Z looking at the origin)."""
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
    try:
        if k == 1:
            for name in list(FreeCAD.listDocuments()):
                FreeCAD.closeDocument(name)
            s.set_pref(VIEW, "UseVulkanRayTracing", False)
            s.set_pref(VIEW, "VulkanPathTracing", True)
            s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
            s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
            FreeCADGui.activateWorkbench("PartWorkbench")
            doc = FreeCAD.newDocument("Temporal")
            box = doc.addObject("Part::Box", "Box")
            box.Length = 10
            box.Width = 10
            box.Height = 10
            doc.recompute()
            view = FreeCADGui.ActiveDocument.ActiveView
            view.viewTop()
            s.frame_phase("setup")
            log("phase=setup (static accumulation)")
        elif k == 7:
            orbit(8)
            s.frame_phase("move-small")
            log("phase=move-small (8 deg orbit)")
        elif k == 13:
            orbit(90)
            s.frame_phase("move-big")
            log("phase=move-big (90 deg orbit)")
        elif k == 19:
            log("snapshot + finish")
            s.snapshot()
            s.finish()
            FreeCADGui.getMainWindow().close()
            return
    except Exception:
        import traceback

        traceback.print_exc()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
