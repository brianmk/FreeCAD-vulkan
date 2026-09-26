#!/usr/bin/env python3
"""open_bench_probe - measure FreeCAD startup and document-open cost.

Host sets FC_OPEN_T0 to the wall-clock time (epoch seconds) it spawned FreeCAD
so the probe can report process-start -> GUI-ready (includes the splash/logo),
process-start -> cube-document-open, and the isolated document-open cost over
FC_OPEN_REPS repeats.

Environment:
  FC_OPEN_T0        host spawn epoch seconds (required for absolute times)
  FC_OPEN_REPS      open/close repeats (default 3)
  FC_BENCH_BACKEND  gl | vulkan (default auto)
  FC_BENCH_MODE     VulkanRenderMode override (0/1/3/4/5)
  FC_BENCH_PRESENT  VulkanPresentMode (0/1/2)
"""

import os
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore

T0 = float(os.environ.get("FC_OPEN_T0") or "0") or time.time()
REPS = int(os.environ.get("FC_OPEN_REPS", "3"))
BACKEND = os.environ.get("FC_BENCH_BACKEND", "auto")
MODE_REQ = os.environ.get("FC_BENCH_MODE", "")
VIEW = "User parameter:BaseApp/Preferences/View"
GL_PNG = "/dev/shm/fc_open_bench.png"


def emit(kind, **kv):
    sys.__stdout__.write(
        "[HARNESS] " + kind + " " + " ".join("%s=%s" % (k, v) for k, v in kv.items()) + "\n")
    sys.__stdout__.flush()


def log(msg):
    print("OPENBENCH " + msg, file=sys.stderr, flush=True)


def ms_since_t0():
    return round(1000.0 * (time.time() - T0), 2)


def backend_of(view):
    if BACKEND in ("vulkan", "gl"):
        return BACKEND
    return "vulkan" if hasattr(view, "getVulkanFrameCount") else "gl"


def render_once(view, backend):
    if backend == "vulkan":
        try:
            c0 = int(view.getVulkanFrameCount())
        except Exception:  # noqa: BLE001
            c0 = 0
        view.requestVulkanRender()
        for _ in range(15000):
            QtCore.QCoreApplication.processEvents()
            try:
                if int(view.getVulkanFrameCount()) > c0:
                    break
            except Exception:  # noqa: BLE001
                break
            time.sleep(0.001)
    else:
        view.redraw()
        try:
            view.saveImage(GL_PNG, -1, -1, "Current")
        except Exception as exc:  # noqa: BLE001
            log("saveImage failed: %r" % (exc,))


state = {"step": 0, "rep": 0, "gui_ready": None, "view": None, "backend": "gl"}


def open_cube():
    doc = FreeCAD.newDocument("OpenBench")
    box = doc.addObject("Part::Box", "Box")
    box.Length = box.Width = box.Height = 10
    doc.recompute()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewIsometric()
    view.fitAll()
    QtCore.QCoreApplication.processEvents()
    render_once(view, state["backend"])
    return doc, view


def step():
    k = state["step"]
    state["step"] += 1
    try:
        if k == 0:
            pref = FreeCAD.ParamGet(VIEW)
            if os.environ.get("FC_BENCH_PRESENT"):
                pref.SetInt("VulkanPresentMode", int(os.environ["FC_BENCH_PRESENT"]))
            pref.SetInt("VulkanRenderMode",
                        int(MODE_REQ) if MODE_REQ else (0 if BACKEND == "gl" else 1))
            state["gui_ready"] = ms_since_t0()
            emit("open_gui_ready", ms=state["gui_ready"])
            log("gui ready at %.1f ms (includes splash/logo)" % state["gui_ready"])
        elif state["rep"] < REPS:
            t = time.time()
            doc, view = open_cube()
            state["view"] = view
            state["backend"] = backend_of(view)
            doc_ms = 1000.0 * (time.time() - t)
            emit("open_cube", rep=state["rep"] + 1, backend=state["backend"],
                 doc_ms=round(doc_ms, 2), total_ms=ms_since_t0())
            log("rep %d doc_open=%.1fms total=%.1fms"
                % (state["rep"] + 1, doc_ms, ms_since_t0()))
            state["rep"] += 1
            if state["rep"] < REPS:
                FreeCAD.closeDocument(doc.Name)
        else:
            emit("open_done", total_ms=ms_since_t0(), reps=REPS,
                 backend=state["backend"])
            log("done total=%.1fms" % ms_since_t0())
            try:
                FreeCADGui.getMainWindow().close()
            except Exception:  # noqa: BLE001
                pass
            return
    except Exception as exc:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        emit("open_error", error=repr(exc))
        try:
            FreeCADGui.getMainWindow().close()
        except Exception:  # noqa: BLE001
            pass
        return
    QtCore.QTimer.singleShot(120, step)


QtCore.QTimer.singleShot(300, step)
