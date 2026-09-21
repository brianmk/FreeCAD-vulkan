#!/usr/bin/env python3
"""Exercise the HDR output tone-mapping operators end to end.

Enables the Vulkan raster viewport with HDR output, builds a scene with a
bright emissive shape (so there is real above-diffuse-white radiance for the
tone mapper to compress), then cycles the VulkanHDRToneMap operator through
Clip / Reinhard / ACES / Hable, emitting a marker per phase.

The host asserts the renderer accepted each operator by reading the
``[VK-SET] ... hdrToneMap=N`` breadcrumbs, and fails on any Vulkan validation
error or report-view error (the harness does that automatically).  The swapchain
format (``[VK-HDR] initSwapChainResources colorFormat=...``) records whether HDR
was actually active: 64 = VK_FORMAT_A2B10G10R10_UNORM_PACK32, 44 = 8-bit BGRA.

Usage:
  FreeCAD tools/fcprobe/vk_hdr_tonemap_probe.py
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"

# Tone-map operator names, in VulkanHDRToneMap index order.
OPERATORS = ["clip", "reinhard", "aces", "hable"]


def log(msg):
    # sys.__stdout__ (FreeCAD redirects sys.stdout to its Python console).
    sys.__stdout__.write("HDRTM " + msg + "\n")
    sys.__stdout__.flush()


s = Session(name="hdrtonemap")
steps = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("HdrToneMap")
    box = doc.addObject("Part::Box", "Box")
    box.Length = box.Width = box.Height = 10
    doc.recompute()
    try:
        box.ViewObject.ShapeColor = (1.0, 1.0, 1.0)
        box.ViewObject.Lighting = "One side"
    except Exception as exc:
        log("warn: could not set Box view properties (%s)" % exc)
    doc.recompute()
    FreeCADGui.updateGui()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewIsometric()
    view.fitAll()


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRenderer", True)
        s.set_pref(VIEW, "VulkanRenderMode", 1)   # RasterVulkan
        s.set_pref(VIEW, "VulkanHDR", True)
        s.set_pref(VIEW, "VulkanHDRExposure", 0.02)
        s.set_pref(VIEW, "VulkanHDRToneMap", 0)
        build_scene()
        s.frame_phase("clip")
        log("phase=clip hdrToneMap=0")
    elif k == 3:
        # Prefs are applied on the preference-change signal; force a couple of
        # frames so the new operator is pushed before the phase marker.
        for _ in range(4):
            s.vulkan_render()
        s.frame_phase("clip")
        s.set_pref(VIEW, "VulkanHDRToneMap", 1)
        log("phase=reinhard hdrToneMap=1")
    elif k == 5:
        for _ in range(4):
            s.vulkan_render()
        s.frame_phase("reinhard")
        s.set_pref(VIEW, "VulkanHDRToneMap", 2)
        log("phase=aces hdrToneMap=2")
    elif k == 7:
        for _ in range(4):
            s.vulkan_render()
        s.frame_phase("aces")
        s.set_pref(VIEW, "VulkanHDRToneMap", 3)
        log("phase=hable hdrToneMap=3")
    elif k == 9:
        for _ in range(4):
            s.vulkan_render()
        s.frame_phase("hable")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(650, step)


QtCore.QTimer.singleShot(500, step)
