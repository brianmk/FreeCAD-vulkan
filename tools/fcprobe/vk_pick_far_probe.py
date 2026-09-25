#!/usr/bin/env python3
"""Hover/pick + clip-plane regression probe for a large, off-origin model.

Reproduces the coupled failures seen with large site-scale documents
(``BIMExample.FCStd``):

1. the raster Vulkan viewport's camera near/far must reach the model so the CPU
   pick (``SoRayPickAction``) can hover/select it, and
2. publishing those planes onto the camera must NOT feed back through the
   ground-plane grid, whose extent is derived from the view volume -- before the
   fix that closed a loop that compounded near/far ~3x per frame until the
   coordinates blew up.

The probe builds a large box far from the origin, forces the Vulkan raster
viewport (a new document otherwise opens the classic Coin/GL backend, which
does not exercise this path at all), enables the ground plane, then drives a
few Vulkan frames while sampling the camera clip planes, and finally scans for
a hover + pick hit.  The paired ``vk_pick_far_probe.check.py`` asserts the hit
AND that the far plane stayed bounded (the runaway detector).

Environment:
  PROBE_SIZE    : box edge length in mm (default 50000)
  PROBE_OFFSET  : box origin offset from world origin in mm (default 200000)
  PROBE_FRAMES  : forced Vulkan frames sampled for clip-plane growth (default 8)
  PROBE_STEP    : scan grid step in pixels (default: width/16)
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

BOX_SIZE = float(os.environ.get("PROBE_SIZE", "50000"))
BOX_OFFSET = float(os.environ.get("PROBE_OFFSET", "200000"))
FRAMES = int(os.environ.get("PROBE_FRAMES", "8"))

# Gui::ViewRenderMode::RasterVulkan -- the interactive raster Vulkan viewport.
RENDER_MODE_RASTER_VULKAN = 1


def log(msg):
    # sys.__stdout__ (not print): FreeCAD redirects sys.stdout to its Python
    # console, so a plain print() never reaches the harness.
    sys.__stdout__.write("FARPICK " + msg + "\n")
    sys.__stdout__.flush()


def settle(ms):
    timer = QtCore.QElapsedTimer()
    timer.start()
    while timer.elapsed() < ms:
        QtCore.QCoreApplication.processEvents()
        QtCore.QThread.msleep(10)


def camera_planes(view):
    """Near/far as currently stored on the shared camera node."""
    cam = view.getCameraNode()
    return cam.nearDistance.getValue(), cam.farDistance.getValue()


def activate_window():
    win = FreeCADGui.getMainWindow()
    win.show()
    win.raise_()
    win.activateWindow()
    mdi = win.findChild(QtWidgets.QMdiArea)
    for sub in (mdi.subWindowList() if mdi else []):
        w = sub.widget()
        if w is not None and w.findChild(QtWidgets.QStackedWidget):
            mdi.setActiveSubWindow(sub)
            break


def main():
    doc = FreeCAD.newDocument("PickFarProbe")
    box = doc.addObject("Part::Box", "Box")
    box.Length = box.Width = box.Height = BOX_SIZE
    box.Placement.Base = FreeCAD.Vector(BOX_OFFSET, BOX_OFFSET, 0.0)
    doc.recompute()

    FreeCADGui.activateWorkbench("PartWorkbench")
    activate_window()
    settle(600)

    view = FreeCADGui.activeView()
    # Force the Vulkan raster viewport: a new document otherwise opens the
    # classic Coin/GL backend, so setClippingPlanes() would never run.
    try:
        view.setRenderMode(RENDER_MODE_RASTER_VULKAN)
    except Exception as exc:  # noqa: BLE001
        log(f"WARN: setRenderMode failed: {exc}")
    view.viewIsometric()
    view.fitAll()
    settle(1200)

    # The ground plane is the node that closes the near/far feedback loop when
    # the viewport publishes those planes onto the camera.
    try:
        view.setGroundPlane(True)
        log("groundplane=on")
    except Exception as exc:  # noqa: BLE001
        log(f"WARN: setGroundPlane failed: {exc}")
    settle(600)

    s = Session("FARPICK")
    if not s.available:
        log("FATAL: could not locate the 3D viewport widget")
        return 2

    log(f"viewport {s.width}x{s.height} dpr={s.dpr} rendermode={view.getRenderMode()} "
        f"box={BOX_SIZE} offset={BOX_OFFSET}")

    # Drive Vulkan frames and sample the clip planes: the grid feedback
    # compounded them ~3x per frame.
    for i in range(FRAMES):
        s.vulkan_render()
        settle(250)
        near, far = camera_planes(view)
        log(f"frame={i} near={near:.3f} far={far:.3f}")

    # Scan for pickable points (the pick is the deterministic signal; hover is
    # sampled at a hit below).
    step = int(os.environ.get("PROBE_STEP", str(max(1, int(s.width / 16)))))
    hits = []
    for y in range(30, int(s.height) - 30, step):
        for x in range(30, int(s.width) - 30, step):
            if s.get_object_info(x, y):
                hits.append((x, y))
    log(f"scan hits={len(hits)}")
    if not hits:
        log("FATAL: scan found no geometry on a large off-origin model")
        log("VERDICT FARPICK FAIL")
        return 1

    # Sample hover + pick at a few hit points; require both at one of them.
    found = None
    pick_only = None
    for x, y in hits[:8]:
        s.move(x, y)
        settle(220)
        pre = FreeCADGui.Selection.getPreselection()
        hover = pre.ObjectName if (pre and pre.ObjectName) else None
        info = s.get_object_info(x, y)
        log(f"sample at=({x},{y}) hover={hover is not None} pick={info is not None}")
        if info is not None and pick_only is None:
            pick_only = (x, y)
        if info is not None and hover is not None:
            found = (x, y, hover, info)
            break

    if found is None:
        log("pick-only hit, but NO hover (preselection path)"
            if pick_only is not None else "no hover/pick hit at any sampled point")
        log("VERDICT FARPICK FAIL")
        return 1

    x, y, hover, info = found
    log(f"hit=({x},{y}) sub={info.get('Component', '')} hover={hover} "
        f"xyz=({info['x']:.3f},{info['y']:.3f},{info['z']:.3f})")
    log("VERDICT FARPICK PASS")
    return 0


def run():
    main()
    sys.__stdout__.write("FARPICK DONE\n")
    sys.__stdout__.flush()
    FreeCADGui.getMainWindow().close()


QtCore.QTimer.singleShot(500, run)
