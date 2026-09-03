#!/usr/bin/env python3
"""Validate the denoise-after-move path in the interactive PT.

The regression being guarded: a camera MOVE (which restarts accumulation but
does NOT dirty the acceleration structures, so the AS-skip keeps building)
must still let the run re-reach the sample cap and publish a fresh denoised
result.  In a broken build the denoiser only fires again after a *rebuild
trigger* (e.g. hovering to force asDirty), so a move produces no new
denoised frame.

The probe runs in phases, each stamped with the presented-frame ordinal via
frame_phase (which reports getVulkanFrameCount()):
  - build: grid + RT prefs entered.
  - base_settled: enough steps for the *initial* run to reach the cap and
    publish denoise #1 (baseline proves the denoiser works at all).
  - moved: the camera is orbited once (view change, accumulation restarts,
    AS-skip holds).  frame_phase records the ordinal at this boundary.
  - postmove_settled: enough steps for the re-run to re-reach the cap and
    publish denoise #2.

The host check splits the FC_VULKAN_PT_DENOISE_TIMING [DENOISE-STATE] lines
on the "moved" ordinal and asserts a fresh denoise publish lands AFTER it.

ENABLE MECHANISM (why we only set_pref here): the mode is applied by
View3DInventor::setRenderMode(), which runs when the status-bar "Ray Tracing"
combo changes / when the view is created from a persisted VulkanRenderMode.
The pref itself has NO OnChange handler, so merely writing it is inert -- the
view MUST be (re)created with VulkanRenderMode=4 already persisted.  Select
"Ray Tracing" once in the GUI (persists it), the same as every other RT probe.
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
N = int(os.environ.get("FC_PROFILE_N", "6"))
# Steps to wait in each settle window.  250 ms/step, so this must comfortably
# exceed (sample cap frames + settle idle frames + denoiser readback frames).
PSETTLE = int(os.environ.get("FC_PT_SETTLE", "110"))


def log(msg):
    print("DENOISE-MOVE n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="denoise-move")
steps = [0]
BASE = 60            # step index where the baseline denoise should be done
MOVE = 60 + PSETTLE  # step index of the camera orbit
FIN = MOVE + PSETTLE


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
        doc = FreeCAD.newDocument("DenoiseMove")
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
    elif k == BASE:
        s.frame_phase("base_settled")
        log("phase=base_settled")
    elif k == MOVE:
        orbit(30)
        s.frame_phase("moved")
        log("phase=moved")
    elif k == FIN:
        s.frame_phase("postmove_settled")
        log("phase=postmove_settled")
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
