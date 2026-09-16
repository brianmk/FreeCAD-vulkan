#!/usr/bin/env python3
"""Guest probe for the sketch-solver perf toolchain.

Builds a deterministic sketch and drives the Sketcher solver so each
``solve()`` does real work, then emits timing the host parses:

  * one summary line per run:
      ``[HARNESS] sketch kind=.. loops=.. geometry=.. constraints=.. reps=..
      build_ms=.. median_ms=.. min_ms=.. max_ms=.. total_ms=..``
  * one line per repetition (per-solve series/chart):
      ``[HARNESS] sketch_rep kind=.. loops=.. rep=.. ms=..``

Two scene kinds:

``loops`` (default)
    ``FC_SKETCH_LOOPS`` separate, fully-constrained closed rectangular loops
    laid out on a grid.  Each loop is 4 lines + 4 coincident + 4 H/V + 2
    dimensions + 2 anchors, so ``loops`` loops is ``4*loops`` geometry and
    ``12*loops`` constraints.  This is the "hundreds of separate drawings in
    one sketch" stress case: the solver sees many disconnected components and
    scales super-linearly with the loop count.

``chain``
    A single N-segment horizontal/vertical staircase (``FC_SKETCH_N``) anchored
    at the origin -- the small, fast baseline.

Constraints are added in a single batched ``addConstraint(list)`` call: adding
them one at a time re-solves after every constraint and is ~50x slower to build.

Environment:
  FC_SKETCH_KIND   ``loops`` (default) or ``chain``
  FC_SKETCH_LOOPS  loop count for kind=loops (default 200)
  FC_SKETCH_N      segment count for kind=chain (default 40)
  FC_SKETCH_REPS   number of perturbed solve() calls (default 3)
  FC_SKETCH_AMP    dimensional perturbation amplitude, mm (default 0.1)
  FC_SKETCH_BATCH  ``1`` (default) batched constraint add, ``0`` one-by-one

The host tool is ``tools/perf/sketch_perf.py`` (bench / flame / chart / all).
Under ``samplib`` (``sketch_perf.py flame``) the same probe is CPU-sampled so
the solver's hot path can be attributed per FreeCAD module.
"""

import math
import os
import statistics
import sys
import time

import FreeCAD
import FreeCADGui
import Part
import Sketcher
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_FCPROBE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fcprobe"
)
sys.path.insert(0, _FCPROBE)
from freecad_probe import Session  # noqa: E402

KIND = os.environ.get("FC_SKETCH_KIND", "loops")
LOOPS = int(os.environ.get("FC_SKETCH_LOOPS", "200"))
N = int(os.environ.get("FC_SKETCH_N", "40"))
REPS = int(os.environ.get("FC_SKETCH_REPS", "3"))
AMP = float(os.environ.get("FC_SKETCH_AMP", "0.1"))
BATCH = os.environ.get("FC_SKETCH_BATCH", "1") == "1"


def log(msg):
    print("SKETCH kind=%s %s" % (KIND, msg), file=sys.stderr, flush=True)


def _add(sk, cons, c):
    if BATCH:
        cons.append(c)
    else:
        sk.addConstraint(c)


def build_loops(doc, loops, step=3.0, w=1.0, h=0.6):
    """`loops` separate, fully-constrained closed rectangles on a grid."""
    sk = doc.addObject("Sketcher::SketchObject", "Sketch")
    cols = int(math.ceil(math.sqrt(loops)))
    cons = []
    dist_idx = []
    for k in range(loops):
        cx = (k % cols) * step
        cy = (k // cols) * step
        pts = [(cx, cy), (cx + w, cy), (cx + w, cy + h), (cx, cy + h)]
        base = len(sk.Geometry)
        for i in range(4):
            a = pts[i]
            b = pts[(i + 1) % 4]
            sk.addGeometry(
                Part.LineSegment(FreeCAD.Vector(a[0], a[1], 0),
                                 FreeCAD.Vector(b[0], b[1], 0)), False)
        for i in range(4):
            _add(sk, cons, Sketcher.Constraint(
                "Coincident", base + i, 2, base + ((i + 1) % 4), 1))
        _add(sk, cons, Sketcher.Constraint("Horizontal", base + 0))
        _add(sk, cons, Sketcher.Constraint("Vertical", base + 1))
        _add(sk, cons, Sketcher.Constraint("Horizontal", base + 2))
        _add(sk, cons, Sketcher.Constraint("Vertical", base + 3))
        dist_idx.append(len(cons) if BATCH else len(sk.Constraints))
        _add(sk, cons, Sketcher.Constraint("Distance", base + 0, w))
        _add(sk, cons, Sketcher.Constraint("Distance", base + 1, h))
        _add(sk, cons, Sketcher.Constraint(
            "DistanceX", base + 0, 1, -1, 1, cx))
        _add(sk, cons, Sketcher.Constraint(
            "DistanceY", base + 0, 1, -1, 1, cy))
    if BATCH:
        sk.addConstraint(cons)
    return sk, dist_idx


def build_chain(doc, n):
    """An N-segment H/V staircase, fully constrained, anchored at the origin."""
    sk = doc.addObject("Sketcher::SketchObject", "Sketch")
    for i in range(n):
        p0 = FreeCAD.Vector(i, 0, 0)
        p1 = FreeCAD.Vector(i + 1, (1 if i % 2 else 0), 0)
        sk.addGeometry(Part.LineSegment(p0, p1), False)
    cons = []
    dist_idx = []
    for i in range(n):
        dist_idx.append(len(cons) if BATCH else len(sk.Constraints))
        _add(sk, cons, Sketcher.Constraint("Distance", i, 1.0))
        _add(sk, cons, Sketcher.Constraint(
            "Horizontal" if i % 2 == 0 else "Vertical", i))
    for i in range(n - 1):
        _add(sk, cons, Sketcher.Constraint("Coincident", i, 2, i + 1, 1))
    _add(sk, cons, Sketcher.Constraint("DistanceX", 0, 1, -1, 1, 0.0))
    _add(sk, cons, Sketcher.Constraint("DistanceY", 0, 1, -1, 1, 0.0))
    if BATCH:
        sk.addConstraint(cons)
    return sk, dist_idx


def run(s):
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    FreeCADGui.activateWorkbench("SketcherWorkbench")
    doc = FreeCAD.newDocument("SketchPerf")

    t_build = time.perf_counter()
    if KIND == "chain":
        sk, dist_idx = build_chain(doc, N)
    else:
        sk, dist_idx = build_loops(doc, LOOPS)
    doc.recompute()
    build_ms = 1000.0 * (time.perf_counter() - t_build)
    s.frame_phase("build")
    log("built %d geometry, %d constraints in %.1f ms"
        % (sk.GeometryCount, len(sk.Constraints), build_ms))

    rc0 = sk.solve()
    s.frame_phase("first_solve")

    dts = []
    for r in range(REPS):
        delta = AMP * ((r % 5) - 2)
        sk.setDatum(dist_idx[r % len(dist_idx)],
                    FreeCAD.Units.Quantity("%.4f mm" % (1.0 + delta)))
        t = time.perf_counter()
        rc = sk.solve()
        ms = 1000.0 * (time.perf_counter() - t)
        dts.append(ms)
        s.emit("sketch_rep", scene=KIND, loops=(LOOPS if KIND == "loops" else 0),
               rep=r, ms=round(ms, 4))
    total = sum(dts)
    s.emit("sketch", scene=KIND, loops=(LOOPS if KIND == "loops" else 0),
           geometry=sk.GeometryCount, constraints=len(sk.Constraints), reps=REPS,
           build_ms=round(build_ms, 3),
           median_ms=round(statistics.median(dts), 4),
           min_ms=round(min(dts), 4), max_ms=round(max(dts), 4),
           total_ms=round(total, 3))
    log("solved %d reps: median=%.1f ms total=%.1f ms rc0=%d rc=%d"
        % (REPS, statistics.median(dts), total, rc0, rc))

    s.snapshot()
    s.finish()
    FreeCADGui.getMainWindow().close()


s = Session(name="sketch")
QtCore.QTimer.singleShot(500, lambda: run(s))
