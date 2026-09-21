#!/usr/bin/env python3
"""Interaction-authority baseline probe.

Records the camera pose and selection after a fixed scripted input sequence
(click-select, orbit drag, pan drag) so later migration phases can diff against
this baseline.  The same sequence runs on the GL and the Vulkan surface; pick
the surface with ``FC_INTERACTION_RENDER=gl|vulkan``.

The probe emits one ``[HARNESS] interaction phase=... surface=... camera=...
selection=...`` record per phase.  ``vk_interaction_probe.check.py`` asserts the
sequence actually drove navigation and selection, so a refactor that silently
stops delivering events fails the run instead of recording a flat baseline.

Usage:
  python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_interaction_probe.py \\
      --profile vulkan --env FC_INTERACTION_RENDER=vulkan
  python3 tools/fcprobe/freecad_probe.py run tools/fcprobe/vk_interaction_probe.py \\
      --profile gl --env FC_INTERACTION_RENDER=gl

Configuration via environment:
  FC_INTERACTION_RENDER : "vulkan" (default) or "gl"
  FC_INTERACTION_BOX    : Part::Box size in mm (default 10)
"""

import json
import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
RENDER = os.environ.get("FC_INTERACTION_RENDER", "vulkan").lower()
BOX = float(os.environ.get("FC_INTERACTION_BOX", "10"))

s = Session(name="interaction")
steps = [0]


def log(msg):
    print("INTHARNESS " + msg, flush=True)


def camera_state(view):
    cam = view.getCameraNode()
    pos = cam.position.getValue()
    rot = cam.orientation.getValue().getValue()
    try:
        focal = float(cam.focalDistance.getValue())
    except Exception:
        focal = 0.0
    return {
        "pos": [round(float(v), 6) for v in pos],
        "rot": [round(float(v), 6) for v in rot],
        "focal": round(focal, 6),
    }


def selection_state():
    try:
        return sorted(o.ObjectName for o in FreeCADGui.Selection.getSelectionEx())
    except Exception:
        return []


def record(phase, view):
    s.emit(
        "interaction",
        phase=phase,
        surface=RENDER,
        camera=json.dumps(camera_state(view), separators=(",", ":")),
        selection=json.dumps(selection_state(), separators=(",", ":")),
    )


def click_raw(x, y):
    """Left click without clearing the selection (Session.click() clears it)."""
    QtCore = s._QtCore
    Qt = QtCore.Qt
    pos = QtCore.QPoint(int(x), int(y))
    s.send_mouse(QtCore.QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton)
    s.send_mouse(QtCore.QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton)
    s._QtWidgets.QApplication.processEvents()
    return selection_state()


def send_mouse_mods(etype, x, y, button, buttons, mods):
    """Session.send_mouse, but able to carry keyboard modifiers."""
    QtCore = s._QtCore
    QtGui = s._QtGui
    Qt = QtCore.Qt
    QW = s._QtWidgets
    s._relocate_viewport()
    container = s.container
    if container is None:
        return
    pos = QtCore.QPoint(int(x), int(y))
    target = container.childAt(pos) or container
    target.setMouseTracking(True)
    tpos = target.mapFrom(container, pos)
    ev = QtGui.QMouseEvent(
        etype, tpos, target.mapToGlobal(tpos), button, buttons, mods
    )
    QW.QApplication.sendEvent(target, ev)
    QW.QApplication.processEvents()


def drag(x0, y0, x1, y1, button, mods=None, n=6):
    QtCore = s._QtCore
    Qt = QtCore.Qt
    mods = Qt.NoModifier if mods is None else mods
    btn = {
        "left": Qt.LeftButton,
        "middle": Qt.MiddleButton,
        "right": Qt.RightButton,
    }[button]
    send_mouse_mods(QtCore.QEvent.MouseButtonPress, x0, y0, btn, btn, mods)
    for i in range(1, n + 1):
        t = i / n
        x = int(x0 + (x1 - x0) * t)
        y = int(y0 + (y1 - y0) * t)
        send_mouse_mods(QtCore.QEvent.MouseMove, x, y, Qt.NoButton, btn, mods)
    send_mouse_mods(QtCore.QEvent.MouseButtonRelease, x1, y1, btn, Qt.NoButton, mods)


def step():
    steps[0] += 1
    k = steps[0]
    try:
        if k == 1:
            for name in list(FreeCAD.listDocuments()):
                FreeCAD.closeDocument(name)
            s.set_pref(VIEW, "UseVulkanRenderer", RENDER == "vulkan")
            s.set_pref(VIEW, "VulkanRenderMode", 1)  # RasterVulkan
            # CAD navigation: LMB selects (so click-select works), shift+MMB
            # orbits and ctrl+MMB pans.  Inventor navigation swallows the LMB
            # press for its viewing mode, so selection never runs.
            s.set_pref(VIEW, "NavigationStyle", "Gui::CADNavigationStyle")
            FreeCADGui.activateWorkbench("PartWorkbench")
            doc = FreeCAD.newDocument("InteractionProbe")
            box = doc.addObject("Part::Box", "Box")
            box.Length = BOX
            box.Width = BOX
            box.Height = BOX
            doc.recompute()
            view = FreeCADGui.ActiveDocument.ActiveView
            view.setAnimationEnabled(False)
            view.setNavigationType("Gui::CADNavigationStyle")
            view.viewTop()
            view.fitAll()
            FreeCADGui.updateGui()
            s._relocate_viewport()
            log("setup done render=%s viewport=%dx%d dpr=%s" % (RENDER, s.width, s.height, s.dpr))
        elif k == 3:
            view = FreeCADGui.ActiveDocument.ActiveView
            FreeCADGui.updateGui()
            record("baseline", view)
            cx, cy = s.width // 2, s.height // 2
            s.move(cx, cy)
            pre = FreeCADGui.Selection.getPreselection()
            log(
                "hover center pre=%s info=%s"
                % (
                    getattr(pre, "ObjectName", None),
                    s.get_object_info(cx, cy),
                )
            )
            sel = click_raw(cx, cy)
            record("after-click", view)
            log("click center selection=%s" % (sel,))
        elif k == 4:
            view = FreeCADGui.ActiveDocument.ActiveView
            cx, cy = s.width // 2, s.height // 2
            # Orbit: shift + right-drag (CAD navigation's spin).  Note the
            # Qt->Coin button remap: Qt Right == Coin BUTTON2, Qt Middle ==
            # Coin BUTTON3.
            drag(
                cx,
                cy,
                cx + s.width // 4,
                cy + s.height // 5,
                "right",
                mods=QtCore.Qt.ShiftModifier,
            )
            FreeCADGui.updateGui()
            record("after-orbit", view)
        elif k == 5:
            view = FreeCADGui.ActiveDocument.ActiveView
            cx, cy = s.width // 2, s.height // 2
            # Pan: middle-drag (Coin BUTTON3 -> PANNING).
            drag(cx, cy, cx - s.width // 5, cy - s.height // 6, "middle")
            FreeCADGui.updateGui()
            record("after-pan", view)
        elif k >= 7:
            s.finish()
            FreeCADGui.getMainWindow().close()
            return
    except Exception:
        import traceback

        traceback.print_exc()
        s.error("interaction probe step %d failed", k)
        return
    QtCore.QTimer.singleShot(500, step)


QtCore.QTimer.singleShot(600, step)
