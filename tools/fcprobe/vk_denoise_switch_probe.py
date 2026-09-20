#!/usr/bin/env python3
"""Validate the runtime denoiser-kind switch in the interactive PT.

The regression being guarded (coin PR #41 / FreeCAD PR #66): switching the
denoiser at runtime (OIDN <-> RTX) must invalidate the presented denoised
result and rebuild the denoise backend for the new kind.  Two bugs made the
viewport go stale/black:

  * setDenoiserFilter() set the kind but never cleared denoiseResultReady, so
    the *old* denoiser's buffer kept being presented;
  * createPathTracingBuffers() early-returned on a same-resolution call and
    ignored denoiseKindDirty, so the new backend was never built.

The probe runs in phases, each stamped with the presented-frame ordinal via
frame_phase:

  - build:         grid + RT prefs entered, denoiser = OIDN.
  - oidn_settled:  OIDN has published a result (kind=1, ready=1).
  - switch_rtx:    VulkanPathTracingDenoiser 1 -> 0 (RTX).  frame_phase marker.
  - rtx_settled:   the RTX backend is configured and has published (kind=0).
  - switch_oidn:   VulkanPathTracingDenoiser 0 -> 1 (OIDN).  frame_phase marker.
  - oidn_settled2: OIDN is active again (kind=1) and published.

Evidence (needs FC_VULKAN_PT_DENOISER_DEBUG=1 and
FC_VULKAN_PT_DENOISE_TIMING=1):

  [DENOISE] resolved kind=<n> explicit=1 pref=<n> ptEnabled=1
  [DENOISE] backend configured kind=<n> active=yes
  [DENOISE-STATE] ord=<n> frame=<n> accum=<0/1> pend=<0/1> ready=<0/1>
                  denoise=<0/1> kind=<n>

The host check asserts the backend is (re)configured for kind=0 after the RTX
switch (createPathTracingBuffers rebuilt) and for kind=1 after the switch
back, and that the presented result is invalidated (a ready 1->0 edge) before
the new kind publishes -- i.e. no stale buffer survives the switch.

ENABLE MECHANISM: VulkanPathTracingDenoiser is a "Vulkan*" display pref, so
writing it fires View3DSettings::OnChange -> applyVulkanSettings() ->
SoVulkanRenderManager::setPathTracingDenoiser -> setDenoiserFilter().  (Unlike
VulkanRenderMode, which has no OnChange handler and is inert when written.)

Run:
  python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_denoise_switch_probe.py \\
    --profile vulkan \\
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
N = int(os.environ.get("FC_PROFILE_N", "6"))
# Steps to wait in each settle window.  250 ms/step, so this must comfortably
# exceed (sample cap frames + settle idle frames + denoiser readback frames).
PSETTLE = int(os.environ.get("FC_PT_SETTLE", "110"))


def log(msg):
    print("DENOISE-SWITCH n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="denoise-switch")
steps = [0]
BASE = 60                       # baseline OIDN publish should be done
SWITCH_RTX = 60 + PSETTLE       # denoiser -> rtx
RTX_SETTLED = SWITCH_RTX + PSETTLE
SWITCH_OIDN = RTX_SETTLED + PSETTLE   # denoiser -> oidn
FIN = SWITCH_OIDN + PSETTLE


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
        s.set_pref(VIEW, "VulkanPathTracingDenoiser", 1)  # 1=OIDN
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("DenoiseSwitch")
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
        s.frame_phase("oidn_settled")
        log("phase=oidn_settled")
    elif k == SWITCH_RTX:
        s.set_pref(VIEW, "VulkanPathTracingDenoiser", 0)  # 0=RTX
        s.frame_phase("switch_rtx")
        log("phase=switch_rtx (denoiser -> rtx)")
    elif k == RTX_SETTLED:
        s.frame_phase("rtx_settled")
        log("phase=rtx_settled")
    elif k == SWITCH_OIDN:
        s.set_pref(VIEW, "VulkanPathTracingDenoiser", 1)  # 1=OIDN
        s.frame_phase("switch_oidn")
        log("phase=switch_oidn (denoiser -> oidn)")
    elif k == FIN:
        s.frame_phase("oidn_settled2")
        log("phase=oidn_settled2")
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
