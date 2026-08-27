#!/usr/bin/env python3
"""Diagnose every light source position.

Adds real SoDirectionalLight and SoPointLight nodes to the scene graph (so
the analytic-light diagnostics fire), plus an emissive cube (so the emissive
NEE-triangle diagnostics fire), then reports the [RTDBG] light / [RTDBG]
neeTri lines from SoRTXRenderBackend.  A misplaced light (analytic or
emissive) is obvious from the log.

Usage:
  FC_VULKAN_RT_DEBUG=1 FreeCAD vk_light_diag_probe.py
"""

import os
import sys

import FreeCAD
import FreeCADGui
from pivy import coin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("LIGHTDIAG " + msg, file=sys.stderr)


s = Session(name="lightdiag")

try:
    FreeCADGui.activateWorkbench("PartWorkbench")

    doc = FreeCAD.newDocument("LightDiag")
    floor = doc.addObject("Part::Box", "Floor")
    floor.Length = 20
    floor.Width = 20
    floor.Height = 1
    fm = FreeCAD.Material()
    fm.AmbientColor = (0.0, 0.0, 0.0)
    fm.DiffuseColor = (0.5, 0.5, 0.5)
    fm.SpecularColor = (0.1, 0.1, 0.1)
    fm.Shininess = 0.1
    floor.ViewObject.ShapeAppearance = fm
    cube = doc.addObject("Part::Box", "Emissive")
    cube.Length = 2
    cube.Width = 2
    cube.Height = 2
    cube.Placement.Base = FreeCAD.Vector(5, 0, 3)
    cm = FreeCAD.Material()
    cm.AmbientColor = (0.0, 0.0, 0.0)
    cm.DiffuseColor = (0.1, 0.1, 0.1)
    cm.SpecularColor = (0.0, 0.0, 0.0)
    cm.EmissiveColor = (2.0, 0.8, 0.2)
    cube.ViewObject.ShapeAppearance = cm
    doc.recompute()

    # Turn the headlight off so the injected light nodes are the only
    # analytic sources, then add a directional light (from above-right) and
    # a point light (offset to one side) directly into the scene graph.
    for key in ("EnableHeadlight", "EnableBacklight", "EnableFillLight"):
        s.set_pref(VIEW + "/LightSources", key, False)

    view = FreeCADGui.ActiveDocument.ActiveView
    sg = view.getSceneGraph()

    dir_node = coin.SoDirectionalLight()
    dir_node.direction = (0.0, 0.0, -1.0)
    dir_node.color = (1.0, 0.9, 0.8)
    dir_node.intensity = 1.0
    sg.addChild(dir_node)

    point = coin.SoPointLight()
    point.location = (12.0, 4.0, 9.0)
    point.color = (0.3, 0.6, 1.0)
    point.intensity = 2.0
    sg.addChild(point)

    view.setAnimationEnabled(False)
    view.viewTop()
    cam = view.getCameraNode()
    cam.position.setValue(5.0, 0.0, 8.0)
    cam.height.setValue(14.0)
    log("phase=setup (directional + point lights + emissive cube)")
    s.snapshot()
    s.finish()
except Exception as exc:  # noqa: BLE001
    log("PROBE ERROR: %s" % (exc,))
    import traceback
    traceback.print_exc()
    s.finish()
    raise
