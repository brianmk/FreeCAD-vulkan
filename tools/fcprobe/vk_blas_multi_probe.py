#!/usr/bin/env python3
"""Verify BLAS scaling across many objects: each new geometry builds one BLAS,
cached content is re-keyed by hash (identical re-adds do NOT rebuild the stack),
a transform-only edit reuses its BLAS, and a same-topology width edit refits.

This is the multi-object counterpart to vk_blas_probe.py.  Phases (path tracing
ON so the BLAS pipeline runs every frame):
  build-1..3      : three distinct Part::Boxes -> BLAS built for each
  reuse           : transform Box1 (object-space vertices unchanged) -> reused
  refit           : resize Box2 width       (same topology)            -> refit
  add-identical   : add Box4 identical to Box1 -> content-key dedup, no storm

The [RTDBG] blas built=/refit=/reused=/cache= counters in stdout.log are the
observable; the host-side check asserts the per-phase cache/build scaling.

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 FreeCAD vk_blas_multi_probe.py
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
    print("BLASMULTI " + msg, file=sys.stderr)


s = Session(name="blas-multi")
steps = [0]
doc = [None]
boxes = {}


def box(name, length=10, width=10, height=10):
    o = doc[0].addObject("Part::Box", name)
    o.Length = length
    o.Width = width
    o.Height = height
    return o


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4=RayTracing: the real RT gate
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc[0] = FreeCAD.newDocument("BlasMulti")
        boxes["b1"] = box("B1")
        doc[0].recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        s.frame_phase("build-1")
        log("phase=build-1")
    elif k == 3:
        s.frame_phase("build-2")
        log("phase=build-2")
        boxes["b2"] = box("B2", width=6, height=4)
        doc[0].recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 5:
        s.frame_phase("build-3")
        log("phase=build-3")
        boxes["b3"] = box("B3", length=4, height=16)
        doc[0].recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 7:
        s.frame_phase("reuse")
        log("phase=reuse")
        boxes["b1"].Placement = FreeCAD.Placement(
            FreeCAD.Vector(0, 0, 7), FreeCAD.Rotation())
        doc[0].recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 9:
        s.frame_phase("refit")
        log("phase=refit")
        boxes["b2"].Width = 12
        doc[0].recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 11:
        # Identical content to B1: the draw-list storage reallocates when an
        # object is added, so B1's command pointer changes; the cache must
        # re-key by content hash instead of rebuilding every BLAS.
        s.frame_phase("add-identical")
        log("phase=add-identical")
        box("B4", length=10, width=10, height=10)
        doc[0].recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 14:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
