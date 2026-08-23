#!/usr/bin/env python3
"""Verify BLAS refit: position-only edits update in place, unchanged geometry
is reused, new geometry builds.

Drives the RTX backend (path tracing ON) so the BLAS pipeline runs every
frame, and phases a Part::Box through:
  build-box      : new geometry -> BLAS built
  refit-box      : Box.Width change (same topology, moved vertices) -> refit
  reuse-transform: Placement change (object-space vertices unchanged) -> reused
  build-cylinder : new shape -> second BLAS built

The [RTDBG] blas built=/refit=/reused= counters (FC_VULKAN_RT_DEBUG=1) in the
harness stdout.log are the observable; the host-side check asserts the
expected per-phase sequence.

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_BREADCRUMBS=1 FreeCAD vk_blas_probe.py
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
    print("BLAS " + msg, flush=True)


s = Session(name="blas")
steps = [0]


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        # Fresh raster-open view, path tracing switched on live (exercises
        # the dynamic toggle while at it).
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Blas")
        box = doc.addObject("Part::Box", "Box")
        box.Length = 10
        box.Width = 10
        box.Height = 10
        doc.recompute()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("build-box")
        log("phase=build-box")
    elif k == 3:
        doc = FreeCAD.getDocument("Blas")
        box = doc.getObject("Box")
        box.Width = 20  # same vertex/index counts, moved vertices -> refit
        doc.recompute()
        s.frame_phase("refit-box")
        log("phase=refit-box")
    elif k == 5:
        doc = FreeCAD.getDocument("Blas")
        box = doc.getObject("Box")
        # Object-space vertices unchanged; only the instance transform moves.
        box.Placement = FreeCAD.Placement(FreeCAD.Vector(0, 0, 5),
                                          FreeCAD.Rotation())
        doc.recompute()
        s.frame_phase("reuse-transform")
        log("phase=reuse-transform")
    elif k == 7:
        doc = FreeCAD.getDocument("Blas")
        cyl = doc.addObject("Part::Cylinder", "Cyl")
        cyl.Radius = 3
        cyl.Height = 8
        doc.recompute()
        s.frame_phase("build-cylinder")
        log("phase=build-cylinder")
    elif k == 9:
        # Identical content to Box: the draw-list storage reallocates when an
        # object is added, so the box's command pointer changes; the cache
        # must re-key by content hash instead of rebuilding every BLAS.
        doc = FreeCAD.getDocument("Blas")
        box2 = doc.addObject("Part::Box", "Box2")
        box2.Length = 10
        box2.Width = 20
        box2.Height = 10
        doc.recompute()
        s.frame_phase("add-identical-box")
        log("phase=add-identical-box")
    elif k == 11:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
