#!/usr/bin/env python3
"""Profile the RT frame time.  Renders a small multi-object scene in path
tracing mode for enough frames to sample the [RTDBG] frameTiming breakdown
(interval / asRecord / asGpu / denoise / traceRecord) emitted by the backend
when FC_VULKAN_FRAME_TIMING=1.

The values are host wall-clock milliseconds:
  interval     frame-to-frame host pacing (= 1/FPS; includes the Qt-submitted
               trace + present + vblank)
  asRecord     CPU time to record the BLAS/TLAS build commands
  asGpu        AS-phase GPU time (the submit+waitIdle blocks until done)
  denoise      updateDenoise (CPU)
  traceRecord  present-pass record time (CPU)
The trace itself runs in Qt's present submit, so it is the largest part of the
interval minus asGpu -- a large interval with a small asGpu points at the
path-trace kernel as the bottleneck.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("PROFILE %s" % msg, file=sys.stderr)


s = Session(name="profile")
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
        s.set_pref(VIEW, "VulkanPathTracingBounces", 3)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 20)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc[0] = FreeCAD.newDocument("Profile")
        b = doc[0].addObject("Part::Box", "Box")
        b.Length = 10
        b.Width = 10
        b.Height = 10
        cyl = doc[0].addObject("Part::Cylinder", "Cyl")
        cyl.Radius = 3
        cyl.Height = 12
        sph = doc[0].addObject("Part::Sphere", "Sph")
        sph.Radius = 4
        sph.Placement = FreeCAD.Placement(
            FreeCAD.Vector(6, 6, 4), FreeCAD.Rotation())
        doc[0].recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("build")
        log("phase=build")
    elif k == 30:
        s.frame_phase("finish")
        log("phase=finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(400, step)


QtCore.QTimer.singleShot(500, step)
