#!/usr/bin/env python3
"""Diagnostics-tooling verification (Phase 1: validation + debug utils + GPU
timing + pipeline feedback).

Exercises the opt-in observability added in Phase 1:

  - SoVulkanConfig::Diagnostics resolves FC_VULKAN_* flags and dumps them
    ([VKCONFIG] diagnostics ...), gated on FC_VULKAN_BACKEND_DEBUG.
  - SoVulkanGpuTimers records per-pass timestamps and prints
    "[RTDBG] gpuTiming <scope>=<ms>" (FC_VULKAN_GPU_TIMING).  That only
    happens on the own-queue render() path: the GUI renders through
    renderExternal(), whose caller-owned command buffer is already inside a
    render pass, where the required vkCmdResetQueryPool is illegal.  The
    check treats missing gpuTiming lines as informational.
  - VK_EXT_pipeline_creation_feedback logs
    "[RTDBG] pipelineFeedback <label> cacheHit=.. creation=..us"
    (FC_VULKAN_PIPELINE_FEEDBACK; the app enables the extension when the
    device advertises it).
  - VK_EXT_debug_utils object names / command-buffer labels are installed
    (FC_VULKAN_DEBUG_UTILS); they surface in a capture, not on stderr.

Run it with those flags set (see vk_diag_probe.check.py):

  FC_VULKAN_BACKEND_DEBUG=1 FC_VULKAN_DEBUG_UTILS=1 \\
  FC_VULKAN_GPU_TIMING=1 FC_VULKAN_PIPELINE_FEEDBACK=1 \\
      python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_diag_probe.py \\
      --profile vulkan --validation-profile sync --name diag

Raster mode keeps it fast and device-agnostic; the pipeline-feedback line only
appears on a device where the app enabled VK_EXT_pipeline_creation_feedback
(whenever the device advertises the extension, in either the raster or the RT
path).
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
N = int(os.environ.get("FC_PROFILE_N", "3"))


def log(msg):
    print("DIAG " + msg, flush=True)


s = Session(name="diag")
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


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("Diag")
    step_size = 12
    for i in range(N):
        for j in range(N):
            b = doc.addObject("Part::Box", "b%d_%d" % (i, j))
            b.Length = 6
            b.Width = 6
            b.Height = 6
            b.Placement = FreeCAD.Placement(
                FreeCAD.Vector(i * step_size - N * step_size / 2.0,
                               j * step_size - N * step_size / 2.0,
                               6.0 * (1.0 + (i % 3) / 2.0)),
                FreeCAD.Rotation())
    doc.recompute()
    FreeCADGui.updateGui()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewIsometric()
    view.fitAll()


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        # Raster path: no RTX bring-up required, so the probe runs on any
        # device.  The device extensions (incl. pipeline creation feedback) are
        # still requested when the GPU advertises them.
        # Force the Vulkan renderer: the pref is persisted, so a preceding
        # forced-GL probe would otherwise leave every later run on OpenGL and
        # silently starve the [VKCONFIG]/[RTDBG] diagnostics this gate asserts.
        s.set_pref(VIEW, "UseVulkanRenderer", True)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", False)
        s.set_pref(VIEW, "VulkanRenderMode", 1)  # RasterVulkan
        build_scene()
        s.frame_phase("diag-open")
        log("phase=open")
    elif k <= 8:
        # Keep rendering frames: the GPU timer ring is 4 deep, so the first
        # "[RTDBG] gpuTiming" lines only appear after a handful of frames.
        orbit(1.5)
        if k in (4, 8):
            s.frame_phase("diag-move-%d" % k)
            log("phase=move-%d" % k)
    elif k == 10:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
