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
    elif k == 12:
        # Runtime toggle: the backend re-reads the env per frame.  Pass A must
        # be followed by a clean pass B without NEE so the two differ only by
        # the emissive-area contribution.  A camera nudge alone does not
        # restart the accumulation: the Vulkan backend owns its own synced
        # camera and never sees the node edit, so pass B would never render
        # (converged-idle viewport under the reset-on-move architecture).
        # Instead, change the scene graph (scale the emissive cube) which
        # updateGeometryCache detects as a scene change, and wake the GUI so
        # the reset -> re-accumulate of pass B renders frames into the dump
        # window.  The scale delta is tiny so the geometry stays effectively
        # identical for the pixel comparison.
        s.frame_phase("toggle")
        log("phase=toggle (NEE off, scene change restarts pass B)")
        os.environ["FC_VULKAN_PT_NEE"] = "0"
        os.environ["FC_VULKAN_PT_MIS"] = "0"
        doc = FreeCAD.getDocument("Mis")
        cube = doc.getObject("Emissive")
        # A tiny placement nudge is detected by updateGeometryCache as a
        # scene change (the BLAS instance transform changed): this resets the
        # accumulation so pass B is a fresh NEE-off run, without moving the
        # cube (and its glow) out of the comparison window.  The nudge must be
        # RELATIVE to the cube's current placement -- using an absolute
        # Placement(Vector(0,0,0.001)) used to teleport the cube to the
        # origin, which was masked by the TLAS transform bug (every instance
        # traced at the origin) and only became visible once the renderer
        # placed the cube correctly.
        base = cube.Placement.Base
        cube.Placement = FreeCAD.Placement(
            FreeCAD.Vector(base.x, base.y, base.z + 0.001),
            FreeCAD.Rotation())
        doc.recompute()
        FreeCADGui.updateGui()
        s.vulkan_render()
    elif k == 16:
        FreeCADGui.updateGui()
    elif k == 24:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
