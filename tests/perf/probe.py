#!/usr/bin/env python3
"""Three-arm performance probe (base / gl / vk), executed inside the FreeCAD GUI.

    FreeCAD /abs/path/to/tests/perf/probe.py

Reads FCP_* env (see README.md), runs one workload, writes a result JSON to
FCP_OUT, then exits. It is deliberately fork-agnostic: everything that does not
exist upstream (setRenderMode / requestVulkanRender / getVulkanFrameCount) is
guarded with hasattr, so the *same* probe runs against the upstream `base` arm,
the fork GL arm and the fork Vulkan arm.
"""

import json
import math
import os
import sys
import time
import traceback

ARM = os.environ.get("FCP_ARM", "vk")
WORK = os.environ.get("FCP_WORK", "orbit")
SCENE = os.environ.get("FCP_SCENE", "box")
SAMPLES = int(os.environ.get("FCP_SAMPLES", "30"))
OUT = os.environ.get("FCP_OUT", "/tmp/fcp_arm.json")
STEP = os.environ.get("FCP_STEP", "")
REPO = os.environ.get("FCP_REPO", "/home/phantom/dev/FreeCAD-Integrate/FreeCAD-vulkan")


def import_qt():
    import importlib
    import types

    for name in ("PySide6", "PySide2", "PySide"):
        try:
            core = importlib.import_module(name + ".QtCore")
            widgets = importlib.import_module(name + ".QtWidgets")
            ns = types.SimpleNamespace(QApplication=widgets.QApplication, QTimer=core.QTimer)
            return ns
        except Exception:
            continue
    raise RuntimeError("no PySide module available")


QT = import_qt()

import FreeCAD  # noqa: E402
import FreeCADGui as Gui  # noqa: E402


def terminate(result):
    try:
        with open(OUT, "w") as f:
            json.dump(result, f)
            f.write("\n")
    except Exception:
        pass
    try:
        for name in list(FreeCAD.listDocuments().keys()):
            try:
                FreeCAD.closeDocument(name)
            except Exception:
                pass
    except Exception:
        pass
    try:
        app = QT.QApplication.instance()
        if app is not None:
            app.quit()
        QT.QTimer.singleShot(3000, lambda: os._exit(0))
    except Exception:
        os._exit(0)


def frame_stats(values):
    if not values:
        return {"p50": 0.0, "p95": 0.0, "mean": 0.0, "max": 0.0}
    s = sorted(values)

    def pct(p):
        k = (len(s) - 1) * p / 100.0
        f = int(k)
        c = min(f + 1, len(s) - 1)
        return s[f] + (s[c] - s[f]) * (k - f)

    return {"p50": pct(50), "p95": pct(95), "mean": sum(values) / len(values), "max": max(values)}


def main():
    result = {
        "arm": ARM,
        "scene": SCENE,
        "work": WORK,
        "open_ms": 0.0,
        "frame_ms": {"p50": 0.0, "p95": 0.0, "mean": 0.0, "max": 0.0},
        "samples": 0,
        "pick_ms_mean": 0.0,
        "pick_hits": 0,
        "notes": "",
        "ok": False,
    }

    if ARM == "vk":
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
            "UseVulkanRenderer", True
        )

    t0 = time.perf_counter()
    deadline = t0 + 60.0
    while Gui.getMainWindow() is None:
        QT.QApplication.processEvents()
        time.sleep(0.005)
        if time.perf_counter() > deadline:
            result["notes"] = "timeout waiting for main window"
            terminate(result)
            return

    # ---- scene ----
    if SCENE == "step":
        import Import

        t = time.perf_counter()
        Import.open(STEP, merge=True)
        doc = FreeCAD.ActiveDocument
        result["open_ms"] = round((time.perf_counter() - t) * 1000.0, 1)
    else:
        t = time.perf_counter()
        doc = FreeCAD.newDocument("bench")
        box = doc.addObject("Part::Box", "B")
        box.Length = 40
        box.Width = 30
        box.Height = 20
        doc.recompute()
        try:
            box.ViewObject.show()
        except Exception:
            pass
        result["open_ms"] = round((time.perf_counter() - t) * 1000.0, 1)

    if WORK == "open":
        # The scene build itself is the measurement; no 3D view is needed.
        result["ok"] = True
        terminate(result)
        return

    view = None
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 15.0:
        try:
            view = Gui.getDocument(doc.Name).ActiveView
        except Exception:
            view = None
        if view is not None:
            break
        QT.QApplication.processEvents()
        time.sleep(0.05)
    if view is None:
        result["notes"] += "; no 3d view"
        terminate(result)
        return
    try:
        view.viewAxonometric()
        view.fitAll()
    except Exception:
        pass
    QT.QApplication.processEvents()

    # ---- render mode (fork only) ----
    if ARM == "vk" and hasattr(view, "setRenderMode"):
        view.setRenderMode(1)
        QT.QApplication.processEvents()
    elif ARM == "gl" and hasattr(view, "setRenderMode"):
        view.setRenderMode(0)
        QT.QApplication.processEvents()

    from pivy import coin

    def force_frame(moving, angle):
        if ARM == "vk":
            if moving:
                cam = view.getCameraNode()
                if cam is not None:
                    cam.orientation.setValue(
                        coin.SbRotation(coin.SbVec3f(0, 1, 0), math.radians(angle))
                    )
            c0 = view.getVulkanFrameCount()
            t = time.perf_counter()
            view.requestVulkanRender()
            d = time.perf_counter() + 2.0
            while time.perf_counter() < d:
                if view.getVulkanFrameCount() > c0:
                    break
                QT.QApplication.processEvents()
                time.sleep(0.001)
            return (time.perf_counter() - t) * 1000.0
        # base + gl: GL path
        if moving:
            cam = view.getCameraNode()
            if cam is not None:
                cam.orientation.setValue(
                    coin.SbRotation(coin.SbVec3f(0, 1, 0), math.radians(angle))
                )
        t = time.perf_counter()
        view.redraw()
        try:
            w = view.graphicsView()
            if w is not None:
                w.repaint()
        except Exception:
            pass
        QT.QApplication.processEvents()
        return (time.perf_counter() - t) * 1000.0

    # ---- workloads ----
    if WORK in ("idle", "orbit"):
        frames = []
        angle = 0.0
        for _ in range(SAMPLES):
            if WORK == "orbit":
                angle += 2.0
            frames.append(force_frame(WORK == "orbit", angle))
        result["frame_ms"] = {k: round(v, 3) for k, v in frame_stats(frames).items()}
        result["samples"] = len(frames)
    elif WORK == "pick":
        try:
            w = view.graphicsView()
            width, height = w.width(), w.height()
        except Exception:
            width, height = 800, 600
        cx, cy = width // 2, height // 2
        pts = [
            (cx + dx, cy + dy)
            for dy in (-40, -20, 0, 20, 40)
            for dx in (-40, -20, 0, 20, 40)
        ]
        times = []
        hits = 0
        for _ in range(max(1, SAMPLES // len(pts))):
            for (x, y) in pts:
                t = time.perf_counter()
                info = view.getObjectInfo((int(x), int(y)))
                times.append((time.perf_counter() - t) * 1000.0)
                if info:
                    hits += 1
        result["pick_ms_mean"] = round(sum(times) / len(times), 4) if times else 0.0
        result["pick_hits"] = hits
        result["samples"] = len(times)
    else:
        result["notes"] = "unknown work %s" % WORK
        terminate(result)
        return

    result["ok"] = True
    terminate(result)


try:
    main()
except Exception:
    terminate(
        {
            "arm": ARM,
            "scene": SCENE,
            "work": WORK,
            "ok": False,
            "notes": "traceback:\n" + traceback.format_exc(),
        }
    )
os._exit(0)
