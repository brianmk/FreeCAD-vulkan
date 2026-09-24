#!/usr/bin/env python3
"""Exercise one path-tracing denoiser backend selected by FC_DENOISER.

FC_DENOISER names the backend to test -- ``rtx`` (OptiX/CUDA), ``oidn`` (CPU),
``fsr`` (AMD FFX DNSR) or ``none``.  The probe maps it onto the
VulkanPathTracingDenoiser combo index, enables path tracing on a small grid, and
lets the run reach the sample cap so denoise-at-target fires.  The host check
(vk_denoiser_probe.check.py) asserts the requested backend actually came up and
ran -- or, for ``none``, that no denoiser was configured at all -- so a silent
fallback to a different backend cannot pass.

Run one backend:
  python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_denoiser_probe.py \\
    --profile vulkan \\
    --env FC_DENOISER=rtx \\
    --env FC_VULKAN_PT_DENOISER=rtx \\
    --env FC_VULKAN_PT_DENOISER_DEBUG=1 \\
    --env FC_VULKAN_PT_DENOISE_TIMING=1

The four suite cases (denoiser-rtx/-oidn/-fsr/-none) pass exactly those envs;
the check reads FC_DENOISER back out of the run's env overrides.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
# DlgSettings3DView.ui ComboBox_VulkanDenoiser item order.
COMBO = {"rtx": 0, "oidn": 1, "fsr": 2, "none": 3}
DENOISER = os.environ.get("FC_DENOISER", "oidn").strip().lower()
if DENOISER not in COMBO:
    DENOISER = "oidn"
# Steps to wait (250 ms each): comfortably exceeds sample cap + settle idle +
# the denoise-at-target dispatch.
FIN = int(os.environ.get("FC_DENOISER_SETTLE", "140"))

s = Session(name="denoiser-" + DENOISER)
steps = [0]


def log(msg):
    print("DENOISER-PROBE name=%s %s" % (DENOISER, msg), file=sys.stderr)


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        # The startup view is created from the persisted prefs, so enable the
        # Vulkan renderer and the PathTracing mode BEFORE opening the document
        # (setting them afterwards would leave the view on the GL/raster path).
        s.set_pref(VIEW, "UseVulkanRenderer", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4 = PathTracing (RT gate)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 4)
        s.set_pref(VIEW, "VulkanPathTracingMaxSamples", 32)
        s.set_pref(VIEW, "VulkanPathTracingDenoiser", COMBO[DENOISER])
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Denoiser")
        for i in range(4):
            for j in range(4):
                b = doc.addObject("Part::Box", "b%d_%d" % (i, j))
                b.Length = 4
                b.Width = 4
                b.Height = 4
                b.Placement = FreeCAD.Placement(
                    FreeCAD.Vector(i * 12 - 24, j * 12 - 24, 6.0),
                    FreeCAD.Rotation())
        doc.recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("build")
        log("phase=build backend=%s combo=%d" % (DENOISER, COMBO[DENOISER]))
    elif k == FIN:
        s.frame_phase("settled")
        log("phase=settled")
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    # Force one frame per tick: the demand-driven widget goes idle once the
    # viewport converges, and without a redraw the accumulate -> denoise-at-
    # target state machine would never reach its sample cap and publish.
    s.vulkan_render()
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
