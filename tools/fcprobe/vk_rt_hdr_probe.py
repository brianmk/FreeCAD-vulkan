#!/usr/bin/env python3
"""Verify the path-traced HDR present transform matches the SDR one.

Renders a diffuse scene in the Vulkan path-tracing mode (VulkanRenderMode=4) and
dumps frames.  Run it once with ``FC_RT_HDR=0`` (SDR) and once with
``FC_RT_HDR=1`` (scRGB HDR); the dumped frames should match.  The RT pipeline
works in display-referred sRGB, so the HDR present pass must decode it to linear
before writing the FP16 scRGB surface -- the frame dumper re-encodes that linear
surface back to sRGB, so an equal pair proves the transform is consistent and the
path-traced image is not washed out.

Usage:
  FC_RT_HDR=0 FreeCAD tools/fcprobe/vk_rt_hdr_probe.py
  FC_RT_HDR=1 FreeCAD tools/fcprobe/vk_rt_hdr_probe.py
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
HDR = os.environ.get("FC_RT_HDR", "1") == "1"


def log(msg):
    # sys.__stdout__ (FreeCAD redirects sys.stdout to its Python console).
    sys.__stdout__.write("RTHDR " + msg + "\n")
    sys.__stdout__.flush()


s = Session(name="rthdr")
steps = [0]

# The render-mode and HDR prefs are written to the real user config by
# set_pref(), so save the prior values and restore them at the end.
_p = FreeCAD.ParamGet(VIEW)
_PRIOR = {
    "VulkanRenderMode": _p.GetInt("VulkanRenderMode", 1),
    "VulkanHDR": _p.GetBool("VulkanHDR", False),
    "VulkanHDRExposure": _p.GetFloat("VulkanHDRExposure", 1.0),
    "VulkanHDRToneMap": _p.GetInt("VulkanHDRToneMap", 0),
}


def restore_prefs():
    for key, value in _PRIOR.items():
        s.set_pref(VIEW, key, value)
    log("restored prefs: " + repr(_PRIOR))


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("RtHdr")
    box = doc.addObject("Part::Box", "Box")
    box.Length = box.Width = box.Height = 10
    try:
        box.ViewObject.ShapeColor = (0.5, 0.5, 0.5)
    except Exception as exc:
        log("warn: could not set Box color (%s)" % exc)
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
        s.set_pref(VIEW, "VulkanRenderMode", 4)   # PathTracing
        s.set_pref(VIEW, "VulkanHDR", HDR)
        s.set_pref(VIEW, "VulkanHDRExposure", 1.0)
        s.set_pref(VIEW, "VulkanHDRToneMap", 0)
        build_scene()
        log("mode=pathTracing hdr=%d" % (1 if HDR else 0))
    elif k <= 30:
        s.vulkan_render()
    else:
        s.snapshot()
        restore_prefs()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(100, step)


QtCore.QTimer.singleShot(500, step)
