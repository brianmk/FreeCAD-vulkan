#!/usr/bin/env python3
"""Verify Phase 4: emissive-surface next-event estimation + MIS.

Scene: a 20x20 floor with an emissive cube above it and every analytic
light disabled (headlight, backlight, fill), so the floor is lit only by
the cube.  The cube's material carries EmissiveColor; the producer routes
it into the RTX material records and the backend pools its triangles.

The probe runs one accumulation pass with NEE ON, then toggles
FC_VULKAN_PT_NEE=0 at runtime (the backend re-reads the env every frame)
and nudges the camera to restart accumulation.  The same scene, camera
and headlight in both passes means the frame difference IS the NEE
signal; the host-side check compares the two passes' frame means.

Run twice:
  - default (NEE ON): pass A is brighter than pass B.
  - FC_VULKAN_PT_NEE=0: both passes are equal (control).

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 \\
      FC_VULKAN_PT_MAXSAMPLES=32 FC_VULKAN_PT_STOP_FRACTION=0 \\
      FC_VULKAN_PT_TEMPORAL=0 FreeCAD vk_mis_probe.py
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
    print("MIS " + msg, file=sys.stderr)


s = Session(name="mis")
steps = [0]


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 3)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        # Analytic lights off: the floor is lit only by the emissive cube.
        for key in ("EnableHeadlight", "EnableBacklight", "EnableFillLight"):
            s.set_pref(VIEW + "/LightSources", key, False)
        FreeCADGui.activateWorkbench("PartWorkbench")

        doc = FreeCAD.newDocument("Mis")
        floor = doc.addObject("Part::Box", "Floor")
        floor.Length = 20
        floor.Width = 20
        floor.Height = 1
        floor_mat = FreeCAD.Material()
        floor_mat.AmbientColor = (0.0, 0.0, 0.0)
        floor_mat.DiffuseColor = (0.5, 0.5, 0.5)
        floor_mat.SpecularColor = (0.1, 0.1, 0.1)
        floor_mat.Shininess = 0.1
        floor.ViewObject.ShapeAppearance = floor_mat
        cube = doc.addObject("Part::Box", "Emissive")
        cube.Length = 2
        cube.Width = 2
        cube.Height = 2
        cube.Placement.Base = FreeCAD.Vector(5, 0, 3)
        cube_mat = FreeCAD.Material()
        cube_mat.AmbientColor = (0.0, 0.0, 0.0)
        cube_mat.DiffuseColor = (0.1, 0.1, 0.1)
        cube_mat.SpecularColor = (0.0, 0.0, 0.0)
        cube_mat.EmissiveColor = (2.0, 0.8, 0.2)
        cube.ViewObject.ShapeAppearance = cube_mat
        doc.recompute()
        view = FreeCADGui.ActiveDocument.ActiveView
        # The document-view creation runs an animated fit-all which would
        # keep overriding our camera for several seconds.  Kill it and
        # frame the scene deterministically: straight down from above the
        # cube, wide enough for the glow on the floor.  viewTop() is
        # instant (rotation reset to straight down); the node edits then
        # reach the renderer through the viewport adapter.
        view.setAnimationEnabled(False)
        view.viewTop()
        cam = view.getCameraNode()
        cam.position.setValue(5.0, 0.0, 8.0)
        cam.height.setValue(14.0)
        s.frame_phase("setup")
        log("phase=setup (path tracing on, analytic lights off)")
    elif 2 <= k <= 22:
        view = FreeCADGui.ActiveDocument.ActiveView
        cam = view.getCameraNode()
        p = cam.position.getValue()
        cam.position.setValue(p[0] + 0.01, p[1], p[2])
    elif k == 24:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
