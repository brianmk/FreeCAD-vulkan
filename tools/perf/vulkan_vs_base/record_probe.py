#!/usr/bin/env python3
"""record_probe - capture frame sequences for the demo GIFs.

FC_REC_KIND:
  rotate  open a box grid, orbit the camera 360 deg, capture FC_REC_FRAMES frames
  bim     open BIMExample.FCStd, show raster Vulkan, then switch to
          PathTracingMax (mode 6) and let it converge

Backend: FC_REC_BACKEND=gl  -> view.saveImage() to $FC_REC_OUT/f%04d.png
         FC_REC_BACKEND=vulkan -> view.requestVulkanRender(); the Vulkan
         frame dumper writes /tmp/vk_frame_<ordinal>.png (FC_VULKAN_DUMP_FRAME).
"""

import math
import os
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore

BACKEND = os.environ.get("FC_REC_BACKEND", "vulkan")
KIND = os.environ.get("FC_REC_KIND", "rotate")
OUT = os.environ.get("FC_REC_OUT", "/tmp/opencode/gif")
N = int(os.environ.get("FC_REC_FRAMES", "60"))
DEG = float(os.environ.get("FC_REC_DEG", "6"))
FILE = os.environ.get("FC_REC_FILE", "")
SCENE_N = int(os.environ.get("FC_REC_SCENE_N", "300"))
VW, VH = (int(x) for x in os.environ.get("FC_REC_VIEWPORT", "960x540").split("x"))
VIEW = "User parameter:BaseApp/Preferences/View"

os.makedirs(OUT, exist_ok=True)
state = {"step": 0, "frames": [], "view": None, "phase": "setup", "phase_start": 0.0}


def emit(tag, **kv):
    sys.__stdout__.write(
        "[HARNESS] " + tag + " " + " ".join("%s=%s" % (k, v) for k, v in kv.items()) + "\n")
    sys.__stdout__.flush()


def log(msg):
    print("REC " + msg, file=sys.stderr, flush=True)


def orbit(deg):
    from pivy import coin
    cam = state["view"].getCameraNode()
    p = cam.position.getValue()
    pos = coin.SbVec3f(p[0], p[1], p[2])
    o = cam.orientation.getValue()
    fwd = o.multVec(coin.SbVec3f(0.0, 0.0, -1.0))
    target = pos + fwd * cam.focalDistance.getValue()
    d = pos - target
    a = math.radians(deg)
    x = d[0] * math.cos(a) - d[1] * math.sin(a)
    y = d[0] * math.sin(a) + d[1] * math.cos(a)
    newpos = target + coin.SbVec3f(x, y, d[2])
    cam.position.setValue(newpos)
    cam.pointAt(target)


def render(idx):
    view = state["view"]
    t = time.perf_counter()
    if BACKEND == "vulkan":
        try:
            c0 = int(view.getVulkanFrameCount())
        except Exception:  # noqa: BLE001
            c0 = 0
        view.requestVulkanRender()
        for _ in range(8000):
            QtCore.QCoreApplication.processEvents()
            try:
                if int(view.getVulkanFrameCount()) > c0:
                    break
            except Exception:  # noqa: BLE001
                break
            time.sleep(0.001)
    else:
        view.redraw()
        view.saveImage(os.path.join(OUT, "f%04d.png" % idx), -1, -1, "Current")
    dt = 1000.0 * (time.perf_counter() - t)
    state["frames"].append(dt)
    ordinal = -1
    if BACKEND == "vulkan":
        try:
            ordinal = int(view.getVulkanFrameCount())
        except Exception:  # noqa: BLE001
            ordinal = -1
    emit("rec_frame", kind=KIND, i=idx, backend=BACKEND,
         ordinal=ordinal, ms=round(dt, 3))


def setup_scene():
    pref = FreeCAD.ParamGet(VIEW)
    pref.SetInt("VulkanPresentMode", 2)
    pref.SetInt("VulkanRenderMode", 1)
    pref.SetInt("AntiAliasing", 0)
    pref.SetBool("VulkanHDR", False)
    pref.SetBool("ShowNaviCube", False)
    pref.SetBool("CornerCoordSystem", False)
    pref.SetBool("ShowAxisCross", False)
    pref.SetBool("ShowGroundPlane", False)
    pref.SetUnsigned("BackgroundColor", 0x1B1D22FF)
    if KIND == "bim":
        pref.SetInt("VulkanPathTracingBounces", 4)
        pref.SetInt("VulkanPathTracingMaxSamples", 512)
        pref.SetInt("VulkanPathTracingSettle", 1)
    try:
        draft = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Draft")
        draft.SetBool("grid", False)
        draft.SetBool("alwaysShowGrid", False)
    except Exception:  # noqa: BLE001
        pass
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    if FILE:
        FreeCAD.openDocument(FILE)
        doc = FreeCAD.ActiveDocument
    else:
        doc = FreeCAD.newDocument("GifScene")
        side = int(math.ceil(math.sqrt(SCENE_N)))
        step = 14.0
        half = side * step / 2.0
        for i in range(SCENE_N):
            r, c = divmod(i, side)
            box = doc.addObject("Part::Box", "b%d" % i)
            box.Length = box.Width = box.Height = 5
            box.ViewObject.ShapeColor = (
                0.25 + 0.5 * (i % 3) / 2.0,
                0.35 + 0.4 * ((i + 1) % 3) / 2.0,
                0.55 + 0.4 * ((i + 2) % 3) / 2.0)
            box.Placement = FreeCAD.Placement(
                FreeCAD.Vector(c * step - half, r * step - half, 0.0),
                FreeCAD.Rotation())
        doc.recompute()
    mw = FreeCADGui.getMainWindow()
    mw.resize(VW, VH)
    mw.showNormal()
    QtCore.QCoreApplication.processEvents()
    FreeCADGui.updateGui()
    view = FreeCADGui.ActiveDocument.ActiveView
    state["view"] = view
    view.setAnimationEnabled(False)
    try:
        view.setAxisCross(False)
    except Exception:  # noqa: BLE001
        pass
    view.viewIsometric()
    view.fitAll()
    QtCore.QCoreApplication.processEvents()
    from pivy import coin
    cam = view.getCameraNode()
    p = cam.position.getValue()
    P0 = (p[0], p[1], p[2])
    o = cam.orientation.getValue()
    fwd = o.multVec(coin.SbVec3f(0.0, 0.0, -1.0))
    t = cam.focalDistance.getValue()
    target = (P0[0] + fwd[0] * t, P0[1] + fwd[1] * t, P0[2] + fwd[2] * t)
    zoom = float(os.environ.get("FC_REC_ZOOM", "0"))
    P1 = (target[0] + (P0[0] - target[0]) * (1.0 - zoom),
          target[1] + (P0[1] - target[1]) * (1.0 - zoom),
          target[2] + (P0[2] - target[2]) * (1.0 - zoom))
    state["cam0"], state["cam1"], state["target"] = P0, P1, target
    if KIND == "rotate" and zoom > 0:
        cam.position.setValue(coin.SbVec3f(*P1))
        cam.pointAt(coin.SbVec3f(*target))
        QtCore.QCoreApplication.processEvents()
    return doc


def step():
    k = state["step"]
    state["step"] += 1
    try:
        if k == 0:
            setup_scene()
            state["phase"] = "rotate"
            log("scene ready, capturing %d frames" % N)
        elif KIND == "rotate":
            if k <= N:
                orbit(DEG)
                render(k)
            else:
                finish()
                return
        elif KIND == "bim":
            from pivy import coin
            RASTER = 20
            cam = state["view"].getCameraNode()
            c0, c1, tg = state["cam0"], state["cam1"], state["target"]
            if k <= RASTER:
                # raster fly-in: dolly from the wide fit to a close-up
                u = (k - 1) / float(RASTER - 1)
                u = u * u * (3.0 - 2.0 * u)
                pos = tuple(c0[d] + (c1[d] - c0[d]) * u for d in range(3))
                cam.position.setValue(coin.SbVec3f(*pos))
                cam.pointAt(coin.SbVec3f(*tg))
                render(k)
            elif k == RASTER + 1:
                log("switching to PathTracingMax (mode 6) at close-up")
                cam.position.setValue(coin.SbVec3f(*c1))
                cam.pointAt(coin.SbVec3f(*tg))
                state["view"].setRenderMode(6)
                for _ in range(4):
                    QtCore.QCoreApplication.processEvents()
                    time.sleep(0.02)
                render(k)
            elif k <= RASTER + 1 + N:
                render(k)
            else:
                finish()
                return
    except Exception as exc:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        emit("rec_error", error=repr(exc))
        finish()
        return
    QtCore.QTimer.singleShot(16, step)


def finish():
    fs = state["frames"]
    if fs:
        s = sorted(fs)
        emit("rec_done", kind=KIND, backend=BACKEND, frames=len(fs),
             median_ms=round(s[len(s) // 2], 3), min_ms=round(s[0], 3),
             max_ms=round(s[-1], 3))
    try:
        FreeCADGui.getMainWindow().close()
    except Exception:  # noqa: BLE001
        pass


QtCore.QTimer.singleShot(400, step)
