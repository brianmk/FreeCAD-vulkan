#!/usr/bin/env python3
"""Verify the BLAS 16-bit position packing (FC_VULKAN_AS_PACK): when enabled
and the object coords fit the half range, buildBlas() uploads R16G16B16_SFLOAT
positions (half AS memory).  The mode is read from os.environ so the shared
check can assert per-mode invariants.

  mode=on  : some (RTDBG) blasFmt build=1 packed=1 line -> packing engaged.
  mode=off : every blasFmt line has packed=0 -> 32-bit default path.

Observable: the [RTDBG] blasFmt build=1 packed=N stride=U fmt=0xV line.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"

PACK = "on" if os.environ.get("FC_VULKAN_AS_PACK") else "off"


def log(msg):
    print("ASPACK[%s] %s" % (PACK, msg), file=sys.stderr)


s = Session(name="aspack")
steps = [0]
doc = [None]


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
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc[0] = FreeCAD.newDocument("Aspack")
        b = doc[0].addObject("Part::Box", "Box")
        b.Length = 10
        b.Width = 10
        b.Height = 10
        doc[0].recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("build")
        log("phase=build")
    elif k == 4:
        s.frame_phase("finish")
        log("phase=finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
