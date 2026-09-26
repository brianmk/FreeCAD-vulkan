#!/usr/bin/env python3
"""record_houses_probe - fly around the BIMExample house; clone it into 8
houses mid-animation, then double to 16 near the end, and keep flying around.

Clones are Part::Feature copies of every visible BIM object, preserving
colour/transparency.  Edges are on for Vulkan (VulkanWireframe).
"""

import math
import os
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore

BACKEND = os.environ.get("FC_REC_BACKEND", "vulkan")
FILE = os.environ.get("FC_REC_FILE", "")
OUT = os.environ.get("FC_REC_OUT", "/tmp/opencode/gif")
N = int(os.environ.get("FC_REC_FRAMES", "84"))
M1 = int(os.environ.get("FC_REC_CLONE_AT", "26"))      # 1 -> 8
M2 = int(os.environ.get("FC_REC_CLONE2_AT", "58"))     # 8 -> 16
T = int(os.environ.get("FC_REC_TRANSITION", "10"))
VW, VH = (int(x) for x in os.environ.get("FC_REC_VIEWPORT", "1280x720").split("x"))
VIEW = "User parameter:BaseApp/Preferences/View"

os.makedirs(OUT, exist_ok=True)
st = {"step": 0, "view": None, "frames": [], "vis": [], "objs": [],
      "c8": False, "c16": False, "sx": 0.0, "sy": 0.0,
      "fA": None, "fB": None, "fC": None, "R1": 0.0, "R2": 0.0, "R3": 0.0}


def emit(tag, **kv):
    sys.__stdout__.write("[HARNESS] " + tag + " "
                         + " ".join("%s=%s" % (k, v) for k, v in kv.items()) + "\n")
    sys.__stdout__.flush()


def log(msg):
    print("HOUSES " + msg, file=sys.stderr, flush=True)


def set_cam(focus, radius, angle, zoff):
    from pivy import coin
    cam = st["view"].getCameraNode()
    fx, fy, fz = focus
    cam.position.setValue(coin.SbVec3f(fx + radius * math.cos(angle),
                                       fy + radius * math.sin(angle),
                                       fz + zoff))
    cam.pointAt(coin.SbVec3f(fx, fy, fz))


def render(idx):
    view = st["view"]
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
    st["frames"].append(dt)
    ordinal = -1
    if BACKEND == "vulkan":
        try:
            ordinal = int(view.getVulkanFrameCount())
        except Exception:  # noqa: BLE001
            ordinal = -1
    emit("rec_frame", i=idx, backend=BACKEND, ordinal=ordinal, ms=round(dt, 3))


def visible_objects(doc):
    out = []
    for o in doc.Objects:
        try:
            vp = o.ViewObject
        except Exception:  # noqa: BLE001
            continue
        if vp is None or not vp.Visibility or not hasattr(o, "Placement"):
            continue
        try:
            sh = o.Shape
        except Exception:  # noqa: BLE001
            continue
        if sh is None or sh.isNull() or sh.BoundBox.XLength <= 0:
            continue
        out.append(o)
    return out


def union_bbox(objs):
    xs, ys, zs = [], [], []
    for o in objs:
        bb = o.Shape.BoundBox
        xs += [bb.XMin, bb.XMax]
        ys += [bb.YMin, bb.YMax]
        zs += [bb.ZMin, bb.ZMax]
    return (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs))


def setup():
    pref = FreeCAD.ParamGet(VIEW)
    pref.SetInt("VulkanPresentMode", 2)
    pref.SetInt("VulkanRenderMode", 1)
    pref.SetInt("AntiAliasing", 0)
    pref.SetBool("VulkanHDR", False)
    pref.SetBool("VulkanWireframe", True)
    pref.SetBool("ShowNaviCube", False)
    pref.SetBool("CornerCoordSystem", False)
    pref.SetBool("ShowAxisCross", False)
    pref.SetBool("ShowGroundPlane", False)
    pref.SetUnsigned("BackgroundColor", 0x1B1D22FF)
    try:
        d = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Draft")
        d.SetBool("grid", False)
        d.SetBool("alwaysShowGrid", False)
    except Exception:  # noqa: BLE001
        pass

    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    FreeCAD.openDocument(FILE)
    doc = FreeCAD.ActiveDocument
    doc.recompute()

    mw = FreeCADGui.getMainWindow()
    mw.resize(VW, VH)
    mw.showNormal()
    QtCore.QCoreApplication.processEvents()
    FreeCADGui.updateGui()
    view = FreeCADGui.ActiveDocument.ActiveView
    st["view"] = view
    view.setAnimationEnabled(False)
    try:
        view.setAxisCross(False)
    except Exception:  # noqa: BLE001
        pass
    view.viewIsometric()
    view.fitAll()
    QtCore.QCoreApplication.processEvents()

    vis = visible_objects(doc)
    st["vis"] = vis
    bb = union_bbox(vis)
    cx, cy, cz = (bb[0] + bb[1]) / 2, (bb[2] + bb[3]) / 2, (bb[4] + bb[5]) / 2
    st["sx"] = (bb[1] - bb[0]) * 1.15
    st["sy"] = (bb[3] - bb[2]) * 1.15
    st["fA"] = (cx, cy, cz)
    st["fB"] = (cx + 1.5 * st["sx"], cy + 0.5 * st["sy"], cz)
    st["fC"] = (cx + 1.5 * st["sx"], cy + 1.5 * st["sy"], cz)
    st["R1"] = float(view.getCameraNode().focalDistance.getValue()) * 1.25
    log("visible=%d R1=%.0f step=%.0f,%.0f" % (len(vis), st["R1"], st["sx"], st["sy"]))


def make_copies(cells, tag):
    doc = FreeCAD.ActiveDocument
    new = []
    for (i, j) in cells:
        off = FreeCAD.Vector(i * st["sx"], j * st["sy"], 0)
        for o in st["vis"]:
            f = doc.addObject("Part::Feature", "H_%s_%s" % (tag, o.Name))
            f.Shape = o.Shape.copy()
            f.Placement = FreeCAD.Placement(o.Placement)
            f.Placement.Base = o.Placement.Base + off
            new.append((f, o))
    doc.recompute()
    FreeCADGui.updateGui()
    QtCore.QCoreApplication.processEvents()
    for f, o in new:
        src, dst = o.ViewObject, f.ViewObject
        for prop in ("ShapeColor", "Transparency", "LineColor", "LineWidth"):
            try:
                setattr(dst, prop, getattr(src, prop))
            except Exception:  # noqa: BLE001
                pass
        try:
            dc = list(src.DiffuseColor)
            if len(dc) == len(f.Shape.Faces):
                dst.DiffuseColor = dc
        except Exception:  # noqa: BLE001
            pass
        try:
            dst.DisplayMode = "Flat Lines"
        except Exception:  # noqa: BLE001
            pass
        try:
            dst.Visibility = True
        except Exception:  # noqa: BLE001
            pass
    st["objs"].extend(new)
    FreeCADGui.updateGui()
    QtCore.QCoreApplication.processEvents()
    return len(new)


def clone8():
    cells = [(i, j) for i in range(4) for j in range(2) if not (i == 0 and j == 0)]
    n = make_copies(cells, "a")
    st["view"].fitAll()
    QtCore.QCoreApplication.processEvents()
    st["R2"] = float(st["view"].getCameraNode().focalDistance.getValue()) * 0.92
    st["c8"] = True
    log("cloned %d objects -> 8 houses, R2=%.0f" % (n, st["R2"]))


def clone16():
    cells = [(i, j) for i in range(4) for j in range(2, 4)]
    n = make_copies(cells, "b")
    st["view"].fitAll()
    QtCore.QCoreApplication.processEvents()
    st["R3"] = float(st["view"].getCameraNode().focalDistance.getValue()) * 0.95
    st["c16"] = True
    log("cloned %d objects -> 16 houses, R3=%.0f" % (n, st["R3"]))


def params(k):
    if not st["c8"]:
        return st["fA"], st["R1"]
    if k >= M2 and st["c16"]:
        u = min(1.0, (k - M2) / float(T))
        b, c = st["fB"], st["fC"]
        return (tuple(b[d] + (c[d] - b[d]) * u for d in range(3)),
                st["R2"] + (st["R3"] - st["R2"]) * u)
    u = min(1.0, max(0.0, (k - M1) / float(T)))
    a, b = st["fA"], st["fB"]
    return (tuple(a[d] + (b[d] - a[d]) * u for d in range(3)),
            st["R1"] + (st["R2"] - st["R1"]) * u)


def step():
    k = st["step"]
    st["step"] += 1
    try:
        if k == 0:
            setup()
        elif k <= N:
            if k == M1 and not st["c8"]:
                clone8()
            if k == M2 and not st["c16"]:
                clone16()
            focus, radius = params(k)
            angle = 0.7 + 2.0 * math.pi * (k - 1) / float(N)
            set_cam(focus, radius, angle, 0.42 * radius)
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
    fs = st["frames"]
    if fs:
        s = sorted(fs)
        emit("rec_done", backend=BACKEND, frames=len(fs),
             median_ms=round(s[len(s) // 2], 3))
    try:
        FreeCADGui.getMainWindow().close()
    except Exception:  # noqa: BLE001
        pass


QtCore.QTimer.singleShot(400, step)
