#!/usr/bin/env python3
"""Verify reset-on-move under CONTINUOUS camera motion (not discrete steps).

The static-accumulation path tracing restarts on every camera movement.  Probing
that machinery with steady motion exercises the viewChanged/accum reset each
frame, then a static window must resume a fresh accumulation.  Asserted upstream
(in vk_camera_anim_probe.check.py) from the [RTDBG] ptState / adaptive lines:

  - reproject=1 never appears (reset-on-move, not temporal reprojection)
  - the anim window sees viewChanged resets (fresh frameIndex each move)
  - after motion stops a static accumulating frame resumes with a growing
    frameIndex and the active fraction declines below 1.0 (convergence)

Run with FC_VULKAN_PT_STOP_FRACTION=0 so the loop keeps accumulating (the
presented frames feed the dump window).

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_PT_STOP_FRACTION=0 \\
      FC_VULKAN_PT_MAXSAMPLES=64 FreeCAD vk_camera_anim_probe.py
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
    print("CAMANIM " + msg, file=sys.stderr)


s = Session(name="camera-anim")
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
    try:
        if k == 1:
            for name in list(FreeCAD.listDocuments()):
                FreeCAD.closeDocument(name)
            s.set_pref(VIEW, "UseVulkanRayTracing", False)
            s.set_pref(VIEW, "VulkanPathTracing", True)
            s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4=RayTracing: the real RT gate
            s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
            s.set_pref(VIEW, "VulkanPathTracingSettle", 1)
            FreeCADGui.activateWorkbench("PartWorkbench")
            doc = FreeCAD.newDocument("CamAnim")
            box = doc.addObject("Part::Box", "Box")
            box.Length = 10
            box.Width = 10
            box.Height = 10
            doc.recompute()
            view = FreeCADGui.ActiveDocument.ActiveView
            view.viewTop()
            s.frame_phase("anim-start")
            log("phase=anim-start (continuous orbit begins)")
        elif 2 <= k <= 12:
            # Continuous motion: step the camera every probe step so each
            # accumulated block renders under a moved camera.
            s.frame_phase("anim-%d" % k)
            orbit(4)
            s.vulkan_render()
        elif k == 14:
            s.frame_phase("static")
            log("phase=static (motion stops, accumulation resumes)")
        elif k == 20:
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
    QtCore.QTimer.singleShot(500, step)


QtCore.QTimer.singleShot(400, step)
