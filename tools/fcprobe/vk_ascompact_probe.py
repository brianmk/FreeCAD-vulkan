#!/usr/bin/env python3
"""Verify AS compaction (FC_VULKAN_AS_COMPACT): after the frame that built a
BLAS completes, the AS is copied into a smaller buffer (compacted residency)
and the original is deferred-destroyed.  The mode is read from os.environ.

  mode=on  : some [RTDBG] compact ... saved=1 line -> the AS was shrunk.
  mode=off : no [RTDBG] compact line (compaction never ran).

Observable: the [RTDBG] compact size=N -> M saved=K line (FC_VULKAN_RT_DEBUG).
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"

COMPACT = "on" if os.environ.get("FC_VULKAN_AS_COMPACT") else "off"


def log(msg):
    print("ASCOMPACT[%s] %s" % (COMPACT, msg), file=sys.stderr)


s = Session(name="ascompact")
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
        doc[0] = FreeCAD.newDocument("Ascompact")
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
