#!/usr/bin/env python3
"""1000-cube render stress + perf probe: Coin/OpenGL vs Vulkan.

Creates a 40x(FC_CUBES_N/40) grid of Part::Box, fits the view, then times a
fixed incremental camera orbit of FC_CUBES_FRAMES frames so both backends draw
the identical scene:

  * gl     profile: view.redraw() drives Coin's synchronous render path.
  * vulkan profile: view.requestVulkanRender() forces one frame per step and
    the Qt event loop is drained after each, so presents complete within the
    timed window.

Prints parseable [CUBEPERF] lines (frames, total_ms, avg_ms, p50_ms, p95_ms,
fps_avg), a motion proof (scene pixels actually changed during the orbit), and
saves a settled screenshot for visual parity:
  compare /tmp/opencode/cubes1000_gl.png vs cubes1000_vulkan.png

Run under the fcprobe harness:
  freecad_probe.py run vk_cubes1000_probe.py --profile gl     --env FC_RENDERER=gl
  freecad_probe.py run vk_cubes1000_probe.py --profile vulkan --env FC_RENDERER=vulkan
"""

import hashlib
import math
import os
import statistics
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"

USE_VULKAN = os.environ.get("FC_RENDERER", "vulkan") != "gl"
TOTAL = int(os.environ.get("FC_CUBES_N", "1000"))
COLS = 40
ROWS = (TOTAL + COLS - 1) // COLS
WARMUP = int(os.environ.get("FC_CUBES_WARMUP", "100"))
MEASURE = int(os.environ.get("FC_CUBES_FRAMES", "200"))


def log(msg):
    print("[CUBEPERF] renderer=%s %s" % ("vulkan" if USE_VULKAN else "gl", msg),
          file=sys.stderr, flush=True)


s = Session(name="cubes1000")
steps = [0]
doc = [None]
phase = [""]
times = []
hashes = []


def orbit(degrees):
    from pivy import coin

    view = FreeCADGui.ActiveDocument.ActiveView
    cam = view.getCameraNode()
    pos = cam.position.getValue()
    angle = math.radians(degrees)
    x = pos[0] * math.cos(angle) - pos[1] * math.sin(angle)
    y = pos[0] * math.sin(angle) + pos[1] * math.cos(angle)
    cam.position.setValue(x, y, pos[2])
    cam.pointAt(coin.SbVec3f(0.0, 0.0, 0.0))


def drain(max_ms=250):
    QtGui.QApplication.processEvents()
    QtGui.QApplication.processEvents()
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < max_ms / 1000.0:
        time.sleep(0.002)
        QtGui.QApplication.processEvents()


def draw_one():
    t0 = time.perf_counter()
    orbit(0.3)
    view = FreeCADGui.ActiveDocument.ActiveView
    if USE_VULKAN:
        view.requestVulkanRender()
    else:
        view.redraw()
    QtGui.QApplication.processEvents()
    times.append((time.perf_counter() - t0) * 1000.0)


def shot_hash():
    path = "/tmp/opencode/cubes1000_probe_shot.png"
    FreeCADGui.ActiveDocument.ActiveView.saveImage(path, 640, 420, "White")
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def report():
    if not times:
        s.error("no frames measured")
        return
    log("frames=%d total_ms=%.0f avg_ms=%.2f p50_ms=%.2f p95_ms=%.2f "
        "min_ms=%.2f max_ms=%.2f fps_avg=%.1f"
        % (len(times), sum(times), statistics.mean(times),
           statistics.median(times),
           times[max(0, int(len(times) * 0.95) - 1)],
           min(times), max(times), 1000.0 / statistics.mean(times)))
    log("motion_frames=%d distinct_hashes=%d changed=%d"
        % (len(hashes), len(set(hashes)), len(set(hashes)) > 1))
    shot = "/tmp/opencode/cubes1000_%s.png" % ("vulkan" if USE_VULKAN else "gl")
    try:
        FreeCADGui.ActiveDocument.ActiveView.saveImage(shot, 1200, 800, "White")
        log("screenshot=%s" % shot)
    except Exception as exc:
        log("screenshot failed: %s" % exc)


def step():
    steps[0] += 1
    k = steps[0]
    try:
        if k == 1:
            for name in list(FreeCAD.listDocuments()):
                FreeCAD.closeDocument(name)
            s.set_pref(VIEW, "UseVulkanRenderer", USE_VULKAN)
            s.set_pref(VIEW, "UseVulkanRayTracing", False)
            s.set_pref(VIEW, "VulkanPathTracing", False)
            s.set_pref(VIEW, "VulkanRenderMode", 1)  # 1 = RasterVulkan
            FreeCADGui.activateWorkbench("PartWorkbench")
        elif k == 2:
            doc[0] = FreeCAD.newDocument("Cubes1000")
            for i in range(TOTAL):
                b = doc[0].addObject("Part::Box", "b%d" % i)
                b.Length = 6
                b.Width = 6
                b.Height = 6
                b.Placement.Base = FreeCAD.Vector((i % COLS) * 10.0,
                                                  (i // COLS) * 10.0, 0.0)
            doc[0].recompute()
            log("created=%d" % len(doc[0].Objects))
            FreeCADGui.SendMsgToActiveView("ViewFit")
            phase[0] = "warmup"
        elif phase[0] == "warmup":
            drain(600)  # let the first frames settle
            while len(times) < WARMUP:
                draw_one()
            times.clear()
            phase[0] = "measure"
        elif phase[0] == "measure":
            hashes.append(shot_hash())
            while len(times) < MEASURE:
                draw_one()
            hashes.append(shot_hash())
            drain(400)
            phase[0] = "report"
        elif phase[0] == "report":
            s.frame_phase("measured")
            report()
            s.snapshot()
            if len(times) >= MEASURE:
                s.verdict("PASS")
            else:
                s.verdict("FAIL", "only %d frames" % len(times))
            s.finish()
            FreeCADGui.getMainWindow().close()
            return
    except Exception:
        import traceback

        traceback.print_exc()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(30, step)


QtCore.QTimer.singleShot(400, step)
