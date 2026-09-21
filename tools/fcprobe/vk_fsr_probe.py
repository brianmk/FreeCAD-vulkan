#!/usr/bin/env python3
"""Verify the AMD FidelityFX DNSR 'fsr' denoiser path activates and renders.

The probe selects the fsr denoiser (combo index 2 / FC_VULKAN_PT_DENOISER=fsr),
enables path tracing on a small grid, and lets the run reach the sample cap so
the denoise-at-target fires.  A build without the backend would emit the
"AMD FSR denoiser is not built in" console error (which fails the probe via the
harness console capture); a build with it logs the pipeline-ready breadcrumb
(FC_VULKAN_PT_DENOISER_DEBUG=1) and the kind=3 dispatch timing
(FC_VULKAN_PT_DENOISE_TIMING=1).  The host check requires those breadcrumbs, so
a build that silently ignored the selection cannot pass.

Run:
  python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_fsr_probe.py \\
    --profile vulkan \\
    --env FC_VULKAN_PT_DENOISER=fsr \\
    --env FC_VULKAN_PT_DENOISER_DEBUG=1 \\
    --env FC_VULKAN_PT_DENOISE_TIMING=1
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
# Steps to wait (250 ms each): comfortably exceeds sample cap + settle idle +
# the denoise-at-target dispatch.
FIN = int(os.environ.get("FC_FSR_SETTLE", "140"))

s = Session(name="fsr-denoise")
steps = [0]


def log(msg):
    print("FSR-PROBE %s" % msg, file=sys.stderr)


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4 = RayTracing (RT gate)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 4)
        s.set_pref(VIEW, "VulkanPathTracingDenoiser", 2)  # 2 = FSR combo index
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("FsrDenoise")
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
        log("phase=build")
    elif k == FIN:
        s.frame_phase("settled")
        log("phase=settled")
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
