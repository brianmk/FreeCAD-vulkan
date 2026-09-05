#!/usr/bin/env python3
"""Guest probe for the renderer-perf toolchain.

Builds a deterministic benchmark scene (an NxN grid of boxes, a cylinder and a
sphere) in one render mode and emits timing data the host parses:

  * Vulkan single-frame cost (modes 1-5): the probe forces ``FC_PERF_FRAMES``
    on-demand frames via ``view.requestVulkanRender()`` and times how long the
    Vulkan frame counter takes to advance each one, emitting
    ``[HARNESS] vkfps frames=.. frame_ms=.. fps=..``.  This is a consistent,
    timer-independent "one presented frame" metric across the raster Vulkan and
    ray-traced backends.
  * RT frame phase breakdown (modes 3/4/5): each forced frame also makes the
    backend emit ``[RTDBG] frameTiming`` (asRecord / asGpu / denoise /
    traceRecord + the precise per-frame interval).  Those are the flame-graph
    bands and the exact steady cadence.
  * RasterCoin / GL (mode 0): no such instrumentation, so the probe times a
    loop of ``view.saveImage`` calls (each forces a full GL render + readback)
    directly.

Environment:
  FC_PERF_MODE      0-5 (default 4).  0=RasterCoin, 1=RasterVulkan, 2=Wireframe,
                    3=RayTracing, 4=PathTracing, 5=Environment.
  FC_PERF_N         grid side (default 10 -> 100 boxes).
  FC_PERF_FRAMES    number of forced frames / saveImage reps (default 40).
  FC_PERF_BOUNCES   PT bounces (default 3).

The host tool is ``tools/perf/renderer_perf.py`` (bench / flame / chart /
analyze).
"""

import os
import statistics
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_FCPROBE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fcprobe")
sys.path.insert(0, _FCPROBE)
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"

MODE = int(os.environ.get("FC_PERF_MODE", "4"))
N = int(os.environ.get("FC_PERF_N", "10"))
FRAMES = int(os.environ.get("FC_PERF_FRAMES", "40"))
BOUNCES = int(os.environ.get("FC_PERF_BOUNCES", "3"))


def log(msg):
    print("PERF mode=%d n=%d %s" % (MODE, N, msg), file=sys.stderr)


def build_scene(doc):
    step_size = 12.0
    half = N * step_size / 2.0
    for i in range(N):
        for j in range(N):
            b = doc.addObject("Part::Box", "b%d_%d" % (i, j))
            b.Length = 4
            b.Width = 4
            b.Height = 4
            b.Placement = FreeCAD.Placement(
                FreeCAD.Vector(i * step_size - half, j * step_size - half,
                               6.0 * (1.0 + (i % 5) / 4.0)),
                FreeCAD.Rotation())
    cyl = doc.addObject("Part::Cylinder", "Cyl")
    cyl.Radius = 8
    cyl.Height = 16
    cyl.Placement = FreeCAD.Placement(
        FreeCAD.Vector(half, half, 8), FreeCAD.Rotation())
    sph = doc.addObject("Part::Sphere", "Sph")
    sph.Radius = 6
    sph.Placement = FreeCAD.Placement(
        FreeCAD.Vector(-half, -half, 6), FreeCAD.Rotation())


def _wait_frame(view, prior, timeout_s=5.0):
    """Block the (guest) thread until the Vulkan frame counter advances past
    ``prior``, so an on-demand frame is provably presented."""
    for _ in range(int(timeout_s * 1000)):
        QtCore.QCoreApplication.processEvents()
        try:
            if int(view.getVulkanFrameCount()) > prior:
                return True
        except Exception:
            return True
        time.sleep(0.001)
    return False


def measure_gl_fps(s):
    view = FreeCADGui.ActiveDocument.ActiveView
    out = "/tmp/opencode/perf_gl.png"
    # saveImage(path, w, h, color): force a full offscreen GL render (+readback).
    view.saveImage(out, -1, -1, "White")
    view.saveImage(out, -1, -1, "White")
    n = FRAMES
    t0 = time.perf_counter()
    for _ in range(n):
        view.saveImage(out, -1, -1, "White")
    dt_ms = 1000.0 * (time.perf_counter() - t0)
    fps = (1000.0 * n) / dt_ms if dt_ms > 0 else 0.0
    s.emit("glfps", mode=MODE, frames=n, ms=round(dt_ms / n, 3), fps=round(fps, 2))
    log("gl fps=%.2f ms/frame=%.3f" % (fps, dt_ms / n))


def measure_vk_fps(s):
    view = FreeCADGui.ActiveDocument.ActiveView
    dts = []
    n = 0
    t0 = time.perf_counter()
    deadline = t0 + 30.0
    try:
        prior = int(view.getVulkanFrameCount())
    except Exception:
        prior = 0
    while n < FRAMES and time.perf_counter() < deadline:
        c0 = int(view.getVulkanFrameCount())
        t = time.perf_counter()
        view.requestVulkanRender()
        _wait_frame(view, c0)
        dts.append((time.perf_counter() - t) * 1000.0)
        n += 1
    elapsed_ms = max(1.0, 1000.0 * (time.perf_counter() - t0))
    med_ms = statistics.median(dts) if dts else 0.0
    fps = (1000.0 * n) / elapsed_ms if elapsed_ms > 0 else 0.0
    s.emit("vkfps", mode=MODE, frames=n, elapsed_ms=round(elapsed_ms, 2),
           frame_ms=round(med_ms, 3), fps=round(fps, 2))
    log("vkfps fps=%.2f frame_ms=%.3f frames=%d elapsed=%.2fms"
        % (fps, med_ms, n, elapsed_ms))


def finish(s):
    s.frame_phase("finish")
    s.snapshot()
    s.finish()
    FreeCADGui.getMainWindow().close()


s = Session(name="renderer%d" % MODE)
phase = [0]


def step():
    phase[0] += 1
    k = phase[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "VulkanRenderMode", MODE)
        s.set_pref(VIEW, "VulkanPathTracing", MODE == 4)
        s.set_pref(VIEW, "UseVulkanRayTracing", MODE in (3, 4, 5))
        s.set_pref(VIEW, "VulkanPathTracingBounces", BOUNCES)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 8)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Perf")
        build_scene(doc)
        doc.recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        s.frame_phase("build")
        log("phase=build mode=%d instances=%d" % (MODE, N * N))
        if MODE == 0:
            measure_gl_fps(s)
        else:
            measure_vk_fps(s)
        finish(s)
        return
    QtCore.QTimer.singleShot(500, step)


QtCore.QTimer.singleShot(500, step)
