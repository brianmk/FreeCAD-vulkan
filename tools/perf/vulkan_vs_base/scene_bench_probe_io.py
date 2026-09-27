#!/usr/bin/env python3
"""scene_bench - guest probe for the cross-backend scene performance suite.

Builds one deterministic workload, forces frames through the viewport's own
presenter and reports per-frame cost, scene build/recompute cost, process
memory and an optional soak/teardown cycle.  It is deliberately
**self-contained**: it imports nothing from tools/fcprobe or the rest of the
fork, so the very same file runs under this fork and under a pristine upstream
FreeCAD build.  The host tool is ``tools/perf/scene_bench.py``.

Workloads (FC_BENCH_KIND):
  objects    ``count`` Part::Box on a square grid (default workload)
  sketches   ``count`` Sketcher::SketchObject, each a small constrained
             rectangle on a grid
  vorontest  open ``FC_BENCH_FILE`` (a single, very large Part::Feature) --
             the final heavy scene
  empty      no geometry (the measurement floor)

Backends: the active presenter is auto-detected per binary.  This fork exposes
``getVulkanFrameCount``/``requestVulkanRender``; upstream does not.  Every run
also measures the portable GL offscreen path (``saveImage``) so the fork's GL
fallback can be calibrated against upstream's GL.

Environment:
  FC_BENCH_KIND        objects|sketches|vorontest|empty   (default objects)
  FC_BENCH_COUNT       object / sketch count             (default 1000)
  FC_BENCH_FRAMES      measured frames                   (default 40)
  FC_BENCH_WARMUP      discarded warm-up frames          (default 8)
  FC_BENCH_VIEWPORT    WxH                                (default 1600x900)
  FC_BENCH_SOAK        soak iterations, 0 = off           (default 0)
  FC_BENCH_TEARDOWN    1 = time closeDocument             (default 1)
  FC_BENCH_FILE        vorontest .FCStd path
  FC_BENCH_SCENE       distinct|shared  (objects geometry)(default distinct)
  FC_BENCH_BACKEND     auto|vulkan|gl    (headline metric)(default auto)

Emits ``[HARNESS] scene_<record> ...`` lines and a ``[VERDICT] scene_bench
PASS|FAIL`` line.
"""

import math
import os
import sys
import time

import FreeCAD
import FreeCADGui
import Part
import Sketcher
from PySide import QtCore

VIEW = "User parameter:BaseApp/Preferences/View"
GL_PNG = "/dev/shm/fc_scene_bench.png"

KIND = os.environ.get("FC_BENCH_KIND", "objects")
COUNT = int(os.environ.get("FC_BENCH_COUNT", "1000"))
FRAMES = int(os.environ.get("FC_BENCH_FRAMES", "40"))
WARMUP = int(os.environ.get("FC_BENCH_WARMUP", "8"))
SOAK = int(os.environ.get("FC_BENCH_SOAK", "0"))
TEARDOWN = os.environ.get("FC_BENCH_TEARDOWN", "1") == "1"
FILE = os.environ.get("FC_BENCH_FILE", "")
SCENE = os.environ.get("FC_BENCH_SCENE", "distinct")
BACKEND = os.environ.get("FC_BENCH_BACKEND", "auto")
MODE_REQ = os.environ.get("FC_BENCH_MODE", "")   # explicit VulkanRenderMode (0/1/3/4/5)
MOTION = os.environ.get("FC_BENCH_MOTION", "")     # "" | "orbit"
ORBIT_DEG = float(os.environ.get("FC_BENCH_ORBIT_DEG", "3"))


_vp = os.environ.get("FC_BENCH_VIEWPORT", "1600x900").lower().split("x")
VW, VH = int(_vp[0]), int(_vp[1])

_errors = []
_doc = None
_view = None
_build_ms = 0.0
_recompute_ms = 0.0


def log(msg):
    print("SCENEBENCH kind=%s %s" % (KIND, msg), file=sys.stderr, flush=True)


def emit(record, **fields):
    parts = [record]
    for key, value in fields.items():
        parts.append("%s=%s" % (key, value))
    sys.__stdout__.write("[HARNESS] " + " ".join(parts) + "\n")
    sys.__stdout__.flush()


def fail(msg):
    _errors.append(msg)
    log("ERROR " + msg)
    emit("scene_error", kind=KIND, error=msg.replace(" ", "_")[:200])
    emit_verdict()


def emit_verdict():
    """Stamp the current verdict.  Called after the measurement (so a verdict
    always exists even if a later closeDocument ends the Qt event loop) and
    again at the very end; the last verdict on the line wins."""
    status = "PASS" if not _errors else "FAIL"
    emit("scene_done", kind=KIND, count=COUNT, status=status,
         errors=";".join(_errors).replace(" ", "_")[:300])
    sys.__stdout__.write("[VERDICT] scene_bench %s\n" % status)
    sys.__stdout__.flush()


def rss_mb():
    """Resident set size of this process, in MiB (0.0 if unavailable)."""
    try:
        with open("/proc/self/status", encoding="ascii") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    return 0.0


# ---------------------------------------------------------------------------
# timing primitives
# ---------------------------------------------------------------------------
def _wait_vk(view, prior, timeout_s=15.0):
    for _ in range(int(timeout_s * 1000)):
        QtCore.QCoreApplication.processEvents()
        try:
            if int(view.getVulkanFrameCount()) > prior:
                return True
        except Exception:  # noqa: BLE001 - a missing counter means no wait
            return True
        time.sleep(0.001)
    return False


def measure_redraw(view, n):
    """Best-effort throughput loop on the active presenter (no forced sync)."""
    dts = []
    for _ in range(n):
        t = time.perf_counter()
        view.redraw()
        dts.append(1000.0 * (time.perf_counter() - t))
    return dts


def _maybe_orbit():
    if MOTION != "orbit" or _view is None:
        return
    try:
        from pivy import coin
        cam = _view.getCameraNode()
        pos = cam.position.getValue()
        a = math.radians(ORBIT_DEG)
        x = pos[0] * math.cos(a) - pos[1] * math.sin(a)
        y = pos[0] * math.sin(a) + pos[1] * math.cos(a)
        cam.position.setValue(x, y, pos[2])
        cam.pointAt(coin.SbVec3f(0.0, 0.0, 0.0))
    except Exception as exc:  # noqa: BLE001
        log("orbit failed: %r" % (exc,))


def measure_vk(view, n):
    """One forced, presented Vulkan frame per sample (fork backend)."""
    dts = []
    for _ in range(n):
        _maybe_orbit()
        try:
            c0 = int(view.getVulkanFrameCount())
        except Exception:  # noqa: BLE001
            c0 = 0
        t = time.perf_counter()
        view.requestVulkanRender()
        _wait_vk(view, c0)
        dts.append(1000.0 * (time.perf_counter() - t))
    return dts


def measure_saveimage(view, n):
    """Portable GL offscreen render + readback, identical on fork and upstream."""
    view.saveImage(GL_PNG, -1, -1, "Current")
    dts = []
    for _ in range(n):
        _maybe_orbit()
        t = time.perf_counter()
        view.saveImage(GL_PNG, -1, -1, "Current")
        dts.append(1000.0 * (time.perf_counter() - t))
    return dts


def stats(ms):
    if not ms:
        return {}
    ordered = sorted(ms)
    n = len(ordered)
    k = max(1, n // 100)
    low1 = sum(ordered[-k:]) / k
    p95 = ordered[min(n - 1, int(round(0.95 * (n - 1))))]
    med = ordered[n // 2] if n % 2 else 0.5 * (ordered[n // 2 - 1] + ordered[n // 2])
    return {"median": med, "min": ordered[0], "max": ordered[-1], "p95": p95, "low1": low1}


def backend_of(view):
    if BACKEND in ("vulkan", "gl"):
        return BACKEND
    return "vulkan" if hasattr(view, "getVulkanFrameCount") else "gl"


# ---------------------------------------------------------------------------
# scene construction
# ---------------------------------------------------------------------------
def build_objects(doc, n):
    side = int(math.ceil(math.sqrt(n)))
    step = 12.0
    half = side * step / 2.0
    shared = Part.makeBox(4, 4, 4) if SCENE == "shared" else None
    for i in range(n):
        r, c = divmod(i, side)
        if shared is not None:
            obj = doc.addObject("Part::Feature", "f%d" % i)
            obj.Shape = shared
        else:
            obj = doc.addObject("Part::Box", "b%d" % i)
            obj.Length = obj.Width = obj.Height = 4
        obj.Placement = FreeCAD.Placement(
            FreeCAD.Vector(c * step - half, r * step - half, 0.0),
            FreeCAD.Rotation(),
        )


def build_sketches(doc, n):
    side = int(math.ceil(math.sqrt(n)))
    step = 3.0
    half = side * step / 2.0
    pts = [(0.0, 0.0), (1.0, 0.0), (1.0, 0.6), (0.0, 0.6)]
    for i in range(n):
        r, c = divmod(i, side)
        sk = doc.addObject("Sketcher::SketchObject", "s%d" % i)
        for k in range(4):
            a, b = pts[k], pts[(k + 1) % 4]
            sk.addGeometry(
                Part.LineSegment(
                    FreeCAD.Vector(a[0], a[1], 0.0),
                    FreeCAD.Vector(b[0], b[1], 0.0),
                ),
                False,
            )
        sk.addConstraint([
            Sketcher.Constraint("Coincident", 0, 2, 1, 1),
            Sketcher.Constraint("Coincident", 1, 2, 2, 1),
            Sketcher.Constraint("Coincident", 2, 2, 3, 1),
            Sketcher.Constraint("Coincident", 3, 2, 0, 1),
            Sketcher.Constraint("Horizontal", 0),
            Sketcher.Constraint("Vertical", 1),
            Sketcher.Constraint("Horizontal", 2),
            Sketcher.Constraint("Vertical", 3),
            Sketcher.Constraint("DistanceX", 0, 1, -1, 1, 0.0),
            Sketcher.Constraint("DistanceY", 0, 1, -1, 1, 0.0),
        ])
        sk.Placement = FreeCAD.Placement(
            FreeCAD.Vector(c * step - half, r * step - half, 0.0),
            FreeCAD.Rotation(),
        )


def scene_stats(doc, detailed=True):
    objs = geom = faces = edges = 0
    for o in doc.Objects:
        objs += 1
        try:
            geom += int(getattr(o, "GeometryCount", 0))
        except Exception:  # noqa: BLE001
            pass
        if not detailed:
            # A single enormous BRep (vorontest) can hold hundreds of
            # thousands of faces; materialising Shape.Faces/Edges as Python
            # lists is prohibitively slow and memory-hungry, so skip it.
            continue
        try:
            shape = getattr(o, "Shape", None)
            if shape is not None and not shape.isNull():
                faces += len(shape.Faces)
                edges += len(shape.Edges)
        except Exception:  # noqa: BLE001
            pass
    return objs, geom, faces, edges


# ---------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------
# Performance-relevant View preferences pinned to one value on *both* binaries
# so a user's tweaked user.cfg (higher MSAA, render cache, HDR, ...) cannot make
# the two builds render different work.  (key, kind, value).  VulkanRenderMode
# is set separately (raster vs classic GL).  Restored in step_finish.
_PINNED_PREFS = [
    ("AntiAliasing", "int", 0),   # 0 = no MSAA
    ("RenderCache", "int", 0),    # off: re-issue geometry every measured frame
    ("UseVBO", "bool", True),
    ("Gradient", "bool", False),
    ("VulkanHDR", "bool", False),  # fork-only; ignored by base
]
_orig_prefs: dict = {}


def _pin_prefs():
    pref = FreeCAD.ParamGet(VIEW)
    _pv = os.environ.get("FC_BENCH_PRESENT", "")
    if _pv:
        try:
            _orig_prefs["VulkanPresentMode"] = pref.GetInt("VulkanPresentMode", -1)
            pref.SetInt("VulkanPresentMode", int(_pv))
        except Exception as exc:  # noqa: BLE001
            log("present pref pin failed: %r" % (exc,))
    _orig_prefs["VulkanRenderMode"] = pref.GetInt("VulkanRenderMode", -1)
    pref.SetInt("VulkanRenderMode", int(MODE_REQ) if MODE_REQ else (0 if BACKEND == "gl" else 1))
    for key, kind, value in _PINNED_PREFS:
        try:
            if kind == "int":
                _orig_prefs[key] = pref.GetInt(key, 1 << 30)
                pref.SetInt(key, value)
            else:
                _orig_prefs[key] = pref.GetBool(key, False)
                pref.SetBool(key, value)
        except Exception as exc:  # noqa: BLE001
            log("pref pin %s failed: %r" % (key, exc))


def _restore_prefs():
    pref = FreeCAD.ParamGet(VIEW)
    for key, kind, _ in _PINNED_PREFS + [("VulkanRenderMode", "int", 0)]:
        orig = _orig_prefs.get(key)
        try:
            if kind == "int" and orig is not None and orig != (1 << 30):
                pref.SetInt(key, orig)
            elif kind == "bool" and orig is not None:
                pref.SetBool(key, orig)
        except Exception:  # noqa: BLE001
            pass


def pref_snapshot():
    pref = FreeCAD.ParamGet(VIEW)
    return {
        "render_mode": pref.GetInt("VulkanRenderMode", -1),
        "msaa": pref.GetInt("AntiAliasing", -1),
        "render_cache": pref.GetInt("RenderCache", -1),
        "use_vbo": int(pref.GetBool("UseVBO", True)),
        "hdr": int(pref.GetBool("VulkanHDR", False)),
    }


def step_setup():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    FreeCADGui.activateWorkbench("PartWorkbench")
    _pin_prefs()
    try:
        mw = FreeCADGui.getMainWindow()
        mw.resize(VW, VH)
        mw.showNormal()
        QtCore.QCoreApplication.processEvents()
    except Exception as exc:  # noqa: BLE001
        log("window resize failed: %r" % (exc,))
    # A stable, featureless background keeps the rasteriser's pixel work
    # comparable across binaries.
    try:
        FreeCAD.ParamGet(VIEW).SetUnsigned("BackgroundColor", 0x20202000)
    except Exception:  # noqa: BLE001
        pass


def step_build():
    global _doc, _view, _build_ms, _recompute_ms
    rss_before = rss_mb()
    t = time.perf_counter()
    if KIND in ("file", "vorontest"):
        if not FILE or not os.path.isfile(FILE):
            raise RuntimeError("FC_BENCH_FILE missing or not a file: %r" % FILE)
        try:
            FreeCADGui.openDocument(FILE)
        except Exception:  # noqa: BLE001 - not on every FreeCAD build
            FreeCAD.openDocument(FILE)
        _doc = FreeCAD.ActiveDocument
    else:
        _doc = FreeCAD.newDocument("SceneBench")
        if KIND == "objects":
            build_objects(_doc, COUNT)
        elif KIND == "sketches":
            build_sketches(_doc, COUNT)
        elif KIND != "empty":
            raise RuntimeError("unknown FC_BENCH_KIND %r" % KIND)
    _build_ms = 1000.0 * (time.perf_counter() - t)

    t = time.perf_counter()
    _doc.recompute()
    _recompute_ms = 1000.0 * (time.perf_counter() - t)

    FreeCADGui.updateGui()
    _view = FreeCADGui.ActiveDocument.ActiveView
    # Force the Vulkan adapter (or the GL viewer) to re-apply the render mode to
    # *this* view.  Without it, a view created/opened after the pref was set
    # keeps a stale scene binding and the Vulkan viewport stays blank.
    try:
        _view.setRenderMode(int(MODE_REQ) if MODE_REQ else (0 if BACKEND == "gl" else 1))
    except Exception as exc:  # noqa: BLE001
        log("setRenderMode failed: %r" % (exc,))
    _view.viewIsometric()
    _view.fitAll()
    QtCore.QCoreApplication.processEvents()

    # A blank scene makes every timing meaningless, so save one small frame
    # (portable GL offscreen path) for the host check to verify non-emptiness.
    render_png = "/dev/shm/fc_scene_bench_frame.png"
    try:
        _view.saveImage(render_png, -1, -1, "Current")
    except Exception as exc:  # noqa: BLE001
        render_png = ""
        log("render sanity snapshot failed: %r" % (exc,))

    objs, geom, faces, edges = scene_stats(_doc, detailed=(KIND != "vorontest"))
    file_mb = 0.0
    if KIND == "vorontest" and FILE and os.path.isfile(FILE):
        file_mb = os.path.getsize(FILE) / 1e6
    emit(
        "scene_build",
        kind=KIND,
        count=COUNT,
        scene=SCENE,
        objects=objs,
        geometry=geom,
        faces=faces if KIND != "vorontest" else -1,
        edges=edges if KIND != "vorontest" else -1,
        file_mb=round(file_mb, 1),
        build_ms=round(_build_ms, 2),
        recompute_ms=round(_recompute_ms, 2),
        rss_before_mb=round(rss_before, 1),
        rss_mb=round(rss_mb(), 1),
        backend=backend_of(_view),
        render_png=render_png or "none",
        **pref_snapshot(),
    )
    log(
        "built objects=%d geom=%d faces=%d edges=%d build=%.1fms recompute=%.1fms"
        % (objs, geom, faces, edges, _build_ms, _recompute_ms)
    )


def _force_frame(view, backend):
    if backend == "vulkan":
        try:
            c0 = int(view.getVulkanFrameCount())
        except Exception:  # noqa: BLE001
            c0 = 0
        view.requestVulkanRender()
        _wait_vk(view, c0)
    else:
        view.redraw()
        view.saveImage(GL_PNG, -1, -1, "Current")


def step_measure():
    view = _view
    backend = backend_of(view)
    for _ in range(WARMUP):
        _force_frame(view, backend)
    QtCore.QCoreApplication.processEvents()

    frame_ms = measure_vk(view, FRAMES) if backend == "vulkan" else measure_saveimage(view, FRAMES)
    redraw_ms = measure_redraw(view, FRAMES)
    saveimage_ms = measure_saveimage(view, FRAMES) if backend == "vulkan" else []

    for i, ms in enumerate(frame_ms):
        emit("scene_frame", kind=KIND, count=COUNT, backend=backend, i=i, ms=round(ms, 4))

    fs, rs = stats(frame_ms), stats(redraw_ms)
    ss = stats(saveimage_ms) if saveimage_ms else {}
    emit(
        "scene_bench",
        kind=KIND,
        count=COUNT,
        backend=backend,
        frames=len(frame_ms),
        motion=(MOTION or "static"),
        mode=(MODE_REQ or "auto"),
        build_ms=round(_build_ms, 2),
        recompute_ms=round(_recompute_ms, 2),
        frame_median_ms=round(fs.get("median", 0.0), 4),
        frame_min_ms=round(fs.get("min", 0.0), 4),
        frame_max_ms=round(fs.get("max", 0.0), 4),
        frame_p95_ms=round(fs.get("p95", 0.0), 4),
        frame_low1_ms=round(fs.get("low1", 0.0), 4),
        fps=round(1000.0 / fs["median"], 2) if fs.get("median") else 0.0,
        redraw_median_ms=round(rs.get("median", 0.0), 4),
        redraw_p95_ms=round(rs.get("p95", 0.0), 4),
        saveimage_median_ms=round(ss.get("median", 0.0), 4),
        rss_mb=round(rss_mb(), 1),
    )
    log(
        "frame median=%.3fms p95=%.3f low1=%.3f fps=%.2f (redraw=%.3f saveimg=%.3f)"
        % (
            fs.get("median", 0.0),
            fs.get("p95", 0.0),
            fs.get("low1", 0.0),
            1000.0 / fs["median"] if fs.get("median") else 0.0,
            rs.get("median", 0.0),
            ss.get("median", 0.0),
        )
    )
    emit_verdict()


def step_soak():
    if SOAK <= 0:
        return
    view = _view
    backend = backend_of(view)
    for it in range(SOAK):
        series = measure_vk(view, FRAMES) if backend == "vulkan" else measure_saveimage(view, FRAMES)
        s = stats(series)
        emit(
            "scene_soak",
            kind=KIND,
            count=COUNT,
            backend=backend,
            iteration=it,
            median_ms=round(s.get("median", 0.0), 4),
            p95_ms=round(s.get("p95", 0.0), 4),
            rss_mb=round(rss_mb(), 1),
        )
    log("soak %d iterations done, rss=%.1fMB" % (SOAK, rss_mb()))


def step_teardown():
    if not TEARDOWN or _doc is None:
        return
    name = _doc.Name
    t = time.perf_counter()
    ok = True
    try:
        FreeCAD.closeDocument(name)
    except Exception as exc:  # noqa: BLE001
        ok = False
        fail("closeDocument raised %r" % (exc,))
    close_ms = 1000.0 * (time.perf_counter() - t)
    emit(
        "scene_teardown",
        kind=KIND,
        count=COUNT,
        close_ms=round(close_ms, 2),
        ok=int(ok),
        rss_mb=round(rss_mb(), 1),
    )
    log("teardown close=%.1fms ok=%s" % (close_ms, ok))


def step_finish():
    emit_verdict()
    _restore_prefs()
    try:
        FreeCADGui.getMainWindow().close()
    except Exception:  # noqa: BLE001
        pass


_STEPS = [step_setup, step_build, step_measure, step_soak, step_teardown, step_finish]


def _run(i):
    if i >= len(_STEPS):
        return
    try:
        _STEPS[i]()
    except Exception as exc:  # noqa: BLE001
        fail("%s: %r" % (_STEPS[i].__name__, exc))
    QtCore.QTimer.singleShot(300 if i == 1 else 400, lambda: _run(i + 1))


QtCore.QTimer.singleShot(500, lambda: _run(0))
