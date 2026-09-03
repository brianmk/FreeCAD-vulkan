#!/usr/bin/env python3
"""Verify TLAS instance culling (FC_VULKAN_TLAS_CULL): a scene with one large
keeper box plus a field of tiny sub-pixel boxes must keep the keeper traced
while the tiny instances are culled from the TLAS.

The env-gated mode is read from os.environ so the shared check can assert the
on/off invariants: mode=on requires culled>0 on a frame that still traces the
keeper (instances>0); mode=off requires culled==0 on every frame.

Phases:
  keeper : one large box -> the keeper is always traced (never sub-pixel)
  field  : many tiny boxes spread wider than the keeper -> fitAll zooms out so
           the field is sub-pixel (culled) while the keeper stays large.

Observable: the [RTDBG] buildTlas ... instances=N culled=M line (FC_VULKAN_RT_DEBUG).
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

CULLMODE = "on" if os.environ.get("FC_VULKAN_TLAS_CULL") else "off"


def log(msg):
    print("TLASCULL[%s] %s" % (CULLMODE, msg), file=sys.stderr)


s = Session(name="tlascull")
steps = [0]
doc = [None]


def box(name, length, width, height, x, y, z):
    o = doc[0].addObject("Part::Box", name)
    o.Length = length
    o.Width = width
    o.Height = height
    o.Placement = FreeCAD.Placement(FreeCAD.Vector(x, y, z),
                                    FreeCAD.Rotation())
    return o


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        # The RT backend is gated on the VIEW MODE (VulkanRenderMode), not the
        # path-tracing bool: mode 4 = RayTracing and is what drops rasterOnly()
        # so the RTX backend actually brings up and buildTlas() runs.  The
        # path-tracing pref alone leaves renderMode at 1 (raster) and never
        # exercises the TLAS.
        s.set_pref(VIEW, "VulkanRenderMode", 4)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc[0] = FreeCAD.newDocument("TlasCull")
        box("Keeper", 40, 40, 4, 0, 0, 0)
        doc[0].recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("keeper")
        log("phase=keeper")
    elif k == 3:
        s.frame_phase("field")
        log("phase=field")
        # 40 tiny boxes on a radius wider than the keeper so fitAll zooms out
        # enough to make them sub-pixel while the keeper stays ~1/3 of the view.
        for i in range(40):
            ang = i * (2.0 * math.pi / 40.0)
            box("T%02d" % i, 0.1, 0.1, 0.05, 55.0 * math.cos(ang),
                55.0 * math.sin(ang), 0)
        doc[0].recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 5:
        s.frame_phase("finish")
        log("phase=finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(600, step)


QtCore.QTimer.singleShot(500, step)
