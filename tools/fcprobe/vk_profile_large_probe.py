#!/usr/bin/env python3
"""Profile the RT AS-build time at SCALE.

Builds an NxN grid of boxes (N from FC_PROFILE_N, default 24 -> 576 instances)
and measures the [RTDBG] frameTiming asGpu value in path tracing mode.  Used to
determine whether the AS build time scales with geometry/instance count (where
TLAS culling + 16-bit format + compaction help) or stays flat (fixed per-frame
overhead, i.e. a BAD place to optimise).

Run twice with the suite env toggled (FC_VULKAN_TLAS_CULL / FC_VULKAN_AS_PACK /
FC_VULKAN_AS_COMPACT off vs on) and compare the per-frame asGpu and interval.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"

N = int(os.environ.get("FC_PROFILE_N", "24"))


def log(msg):
    print("PROFILE n=%d %s" % (N, msg), file=sys.stderr)


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
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 12)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc[0] = FreeCAD.newDocument("Profile")
        # Spread the grid in a shallow wave so, in the 3/4 view, far instances
        # are heavily foreshortened (small) and some fall outside the frustum,
        # exercising the TLAS cull / sub-pixel filters.
        stepSize = 12
        for i in range(N):
            for j in range(N):
                b = doc[0].addObject("Part::Box", "b%d_%d" % (i, j))
                b.Length = 4
                b.Width = 4
                b.Height = 4
                b.Placement = FreeCAD.Placement(
                    FreeCAD.Vector(i * stepSize - N * stepSize / 2.0,
                                   j * stepSize - N * stepSize / 2.0,
                                   6.0 * (1.0 + (i % 5) / 4.0)),
                    FreeCAD.Rotation())
        doc[0].recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        # FC_PROFILE_ZOOM:n zooms in n steps so only the grid centre is in
        # view: most instances fall outside the frustum, exercising the TLAS
        # cull.  Without it (default) the whole grid fits -> culled stays 0.
        for _ in range(int(os.environ.get("FC_PROFILE_ZOOM", "0"))):
            view.zoomIn()
        s.frame_phase("build")
        log("phase=build instances=%d" % (N * N))
    elif k == 24:
        s.frame_phase("finish")
        log("phase=finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(500, step)


QtCore.QTimer.singleShot(500, step)
