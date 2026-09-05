#!/usr/bin/env python3
"""Preference-cover probe for View3DSettings.

Sets each observable View-preference to a distinctive test value, lets the
OnChange handler run (the harness set_pref fires the ParameterObserver), then
reads back the observable state it should have set.  Emits a [COVER] line per
pref so the host-side .check.py / a before-vs-after harness diff can confirm the
handler actually wired the value -- the end-to-end guard for the View3DSettings
data-driven rewrite.

Coverage here is the subset with reliable Python/scene read-back:
  camera type (Orthographic), navigation type (NavigationStyle), and the
  headlight/backlight/filllight/environment light nodes (via a scene walk).
Nav sub-settings (zoom/rotation), dimensions, VBO etc. have no clean read-back
and are covered by the static table-vs-dispatch checker (vk_viewsettings_keys).

Usage:
  FC_VULKAN_RT_DEBUG=1 FreeCAD vk_prefcover_probe.py
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
    print("PFX " + msg, flush=True)


s = Session(name="prefcover")
steps = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("PrefCover")
    box = doc.addObject("Part::Box", "Box")
    box.Length = 10
    box.Width = 10
    box.Height = 10
    doc.recompute()
    FreeCADGui.updateGui()
    FreeCADGui.ActiveDocument.ActiveView.viewIsometric()
    FreeCADGui.ActiveDocument.ActiveView.fitAll()


def fval(field):
    """Read a Coin field value into a Python primitive (SbColor -> (r,g,b))."""
    v = field.getValue()
    try:
        return tuple(float(x) for x in v)
    except TypeError:
        return v


def walk(root, acc, budget=[0]):
    try:
        name = root.getTypeId().getName().getString()
    except Exception:
        return
    if budget[0] > 4000:
        return
    budget[0] += 1
    try:
        if name == "DirectionalLight":
            acc["dir"].append((fval(root.color), float(root.intensity.getValue()),
                               bool(root.on.getValue())))
        elif name == "Environment":
            acc["env"].append((fval(root.ambientColor),
                               float(root.ambientIntensity.getValue())))
    except Exception as exc:
        acc["err"].append("%s: %s" % (name, exc))
    try:
        for c in root.getChildren():
            walk(c, acc, budget)
    except Exception:
        pass


def scene_state():
    acc = {"dir": [], "env": [], "err": []}
    try:
        sg = FreeCADGui.ActiveDocument.ActiveView.getSceneGraph()
        walk(sg, acc)
    except Exception as exc:
        acc["err"].append("walk: %s" % exc)
    return acc


def emit(prefix):
    view = FreeCADGui.ActiveDocument.ActiveView
    st = scene_state()
    d = ";".join("%s" % x[1] for x in st["dir"])
    e = ";".join("%s" % x[1] for x in st["env"])
    try:
        ct = view.getCameraType()
    except Exception:
        ct = "?"
    try:
        nt = view.getNavigationType()
    except Exception:
        nt = "?"
    log("%s cam=%s nav=%s dir[]=%s env[]=%s err=%s"
        % (prefix, ct, nt, d, e, ";".join(st["err"])))


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        build_scene()
        emit("base")
        s.frame_phase("base")
    elif k == 3:
        s.set_pref(VIEW, "Orthographic", False)  # perspective camera
        emit("ortho-off")
    elif k == 5:
        s.set_pref(VIEW, "Orthographic", True)   # back to orthographic
        emit("ortho-on")
    elif k == 7:
        s.set_pref(VIEW, "NavigationStyle", "Gui::TouchpadNavigationStyle")
        emit("navtype-touchpad")
    elif k == 9:
        # headlight setter runs (crash-safety + changes a light node)
        s.set_pref(VIEW, "HeadlightColor", 0xFF0000FF)  # opaque red
        emit("headlight-red")
    elif k == 11:
        s.set_pref(VIEW, "HeadlightIntensity", 33)      # percent
        emit("headlight-33")
    elif k == 13:
        s.set_pref(VIEW, "AmbientLightColor", 0x00FF00FF)  # green
        emit("ambient-green")
    elif k == 15:
        s.set_pref(VIEW, "AmbientLightIntensity", 77)
        emit("ambient-77")
    elif k == 17:
        s.set_pref(VIEW, "EnableFillLight", True)
        emit("fill-on")
    elif k == 19:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(500, step)


QtCore.QTimer.singleShot(500, step)
