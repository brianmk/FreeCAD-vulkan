#!/usr/bin/env python3
"""Pick-probe sweep harness (companion to the FC_PICK_PROBE flag).

Drives the mouse across the 3D viewport along a horizontal line through the
viewport center, samples hover (preselection) and pick at each pixel, clicks
at boundary pixels, and reports the scene-space trigger boundaries against
the target object's geometry planes.

This is a consumer of the unified ``freecad_probe`` harness (Session): the
viewport discovery and synthetic-mouse plumbing are provided by the module
instead of being duplicated here.

Usage:
  FC_PICK_PROBE=1 FreeCAD vk_pick_probe.py

Configuration via environment:
  PROBE_AXIS                  : sweep axis, 'x' (default) or 'y'
  PROBE_L / PROBE_W / PROBE_H : Part::Box dimensions in mm (default 10)
  PROBE_STEP                  : coarse sweep step in pixels (default 8)
  PROBE_REFINE                : boundary refine window in pixels (default 14)
  PROBE_Y                     : sweep line as fraction of viewport height
                                (default 0.5)

The harness exits with a printed verdict: PICKPROBE PASS / PICKPROBE FAIL.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session

BOX_L = float(os.environ.get("PROBE_L", "10"))
BOX_W = float(os.environ.get("PROBE_W", "10"))
BOX_H = float(os.environ.get("PROBE_H", "10"))
COARSE_STEP = int(os.environ.get("PROBE_STEP", "8"))
REFINE_WINDOW = int(os.environ.get("PROBE_REFINE", "14"))
SWEEP_Y_FRAC = float(os.environ.get("PROBE_Y", "0.5"))


def log(msg):
    print("PICKHARNESS " + msg, flush=True)


def sample_hover(s, x, y):
    s.move(x, y)
    pre = FreeCADGui.Selection.getPreselection()
    hover = None
    if pre and pre.ObjectName:
        subs = getattr(pre, "SubElementNames", None) or []
        hover = (pre.ObjectName, ",".join(subs))
    info = s.get_object_info(x, y)
    pick = None
    if info:
        pick = (info.get("Component", ""), info["x"], info["y"], info["z"])
    return hover, pick


def click_at(s, x, y):
    return s.click(x, y)


def main():
    doc = FreeCAD.newDocument("PickProbe")
    box = doc.addObject("Part::Box", "Box")
    box.Length = BOX_L
    box.Width = BOX_W
    box.Height = BOX_H
    doc.recompute()

    FreeCADGui.activateWorkbench("PartWorkbench")
    view = FreeCADGui.activeView()
    view.viewTop()
    view.fitAll()

    s = Session("PICKPROBE")
    if not s.available:
        log("FATAL: could not locate the 3D viewport widget")
        return 2

    dpr = s.dpr
    w = s.width
    h = s.height
    axis = os.environ.get("PROBE_AXIS", "x")
    span = w if axis == "x" else h
    fixed = int((h if axis == "x" else w) * SWEEP_Y_FRAC)

    def coords(c):
        return (c, fixed) if axis == "x" else (fixed, c)

    log(f"viewport {w}x{h} dpr={dpr} axis={axis} span={span} fixed={fixed}")

    # Self-calibrate the scene size of one pixel on the focal plane.
    cx, cy = coords(span // 2)
    p0 = view.getPointOnFocalPlane(s.device(cx, cy))
    p1 = view.getPointOnFocalPlane(s.device(cx + 1, cy))
    mm_per_px = (p1 - p0).Length if p0 and p1 else 0.0
    tol = max(mm_per_px * 1.5, 1e-6)
    log(f"mm per pixel={mm_per_px:.4f} tolerance={tol:.4f}")

    # Local reference geometry (placement-independent ground truth).
    bb = box.Shape.BoundBox
    pl = box.Placement
    local_planes = {
        "x": (bb.XMin, bb.XMax),
        "y": (bb.YMin, bb.YMax),
        "z": (bb.ZMin, bb.ZMax),
    }
    log(f"local bbox x=[{bb.XMin},{bb.XMax}] y=[{bb.YMin},{bb.YMax}] "
        f"z=[{bb.ZMin},{bb.ZMax}]")

    def pick_at(c):
        x, y = coords(c)
        info = s.get_object_info(x, y)
        if info:
            return (info.get("Component", ""), info["x"], info["y"], info["z"])
        return None

    # ---- coarse pass: pick only (fast) ----
    pick_hits = []
    for c in range(0, span, COARSE_STEP):
        pk = pick_at(c)
        if pk:
            pick_hits.append((c, pk))

    if not pick_hits:
        log("FATAL: sweep never hit the target object")
        return 2

    # ---- refine windows around the pick boundaries ----
    bounds = [pick_hits[0][0], pick_hits[-1][0]]
    refine = set()
    for b in bounds:
        for c in range(max(0, b - REFINE_WINDOW),
                       min(span, b + REFINE_WINDOW + 1), 2):
            refine.add(c)
    refine = sorted(refine)
    fine_rows = {}
    for c in refine:
        x, y = coords(c)
        hover, pk = sample_hover(s, x, y)
        fine_rows[c] = (hover, pk)
        log(f"fine {axis}={c:4d} hover={hover is not None}"
            + (f" sub={pk[0]} hit=({pk[1]:.3f},{pk[2]:.3f},{pk[3]:.3f})"
               if pk else " no-hit"))

    # ---- analyze ----
    def first_last(xs):
        return (min(xs), max(xs)) if xs else None

    hover_range = first_last([c for c, (hv, _) in fine_rows.items() if hv])
    pick_range = first_last([c for c, (_, pk) in fine_rows.items() if pk])

    errors = []

    radius_dev = 5
    radius_logical = radius_dev / dpr if dpr else radius_dev
    radius_slack = radius_logical + 2

    def check_boundary(label, c, hover, pk):
        if pk is None:
            if hover and label == "pick":
                errors.append(f"{label}: pick hit at {axis}={c} but NO hover")
            return
        comp, hx, hy, hz = pk
        local = pl.inverse().multVec(FreeCAD.Vector(hx, hy, hz))
        lo, hi = local_planes[axis]
        d = min(abs(getattr(local, axis) - lo),
                abs(getattr(local, axis) - hi))
        if d > tol:
            errors.append(
                f"{label}: boundary hit at local {axis}="
                f"{getattr(local, axis):.4f} is {d:.4f} mm off the "
                f"plane [{lo},{hi}] (tol {tol:.4f})")
        if hover is None:
            errors.append(f"{label}: pick hit at {axis}={c} but NO hover")

    if hover_range and pick_range:
        for c, pk_edge in zip(hover_range, pick_range):
            if abs(c - pk_edge) > radius_slack:
                errors.append(
                    f"hover: hover boundary {axis}={c} is "
                    f"{abs(c - pk_edge)}px from pick boundary {pk_edge} "
                    f"(slack {radius_slack:.1f}px)")
    if pick_range:
        check_boundary("pick", pick_range[0], *fine_rows[pick_range[0]])
        check_boundary("pick", pick_range[1], *fine_rows[pick_range[1]])
    if hover_range:
        for c in hover_range:
            hv, pk = fine_rows[c]
            if hv and pk is not None:
                check_boundary("hover", c, hv, pk)

    # ---- click phase ----
    radius_px = int(round(5 * dpr)) if dpr else 5
    radius_tol = radius_px * mm_per_px * 1.2
    click_pixels = [span // 2]
    if hover_range:
        click_pixels += [hover_range[0] - 2, hover_range[0], hover_range[0] + 2,
                         hover_range[1] - 2, hover_range[1], hover_range[1] + 2]
    for c in list(click_pixels):
        if c < 0 or c >= span:
            click_pixels.remove(c)
    for c in click_pixels:
        x, y = coords(c)
        ok = click_at(s, x, y)
        info = pick_at(c)
        log(f"click {axis}={c:4d} selected={ok} expected={info is not None}")
        if ok and info is None:
            if hover_range:
                dlo = abs(c - hover_range[0])
                dhi = abs(c - hover_range[1])
                if min(dlo, dhi) > radius_tol / mm_per_px:
                    errors.append(f"click at {axis}={c} selected with NO geometry hit")
            else:
                errors.append(f"click at {axis}={c} selected with NO geometry hit")
        if info and not ok:
            errors.append(f"click at {axis}={c} missed selection over geometry")

    log(f"hover range {axis}=[{hover_range}] pick range {axis}=[{pick_range}]"
        if hover_range else "hover range: none")

    if errors:
        for e in errors:
            log("ERROR " + e)
        log("VERDICT PICKPROBE FAIL")
        return 1

    log("VERDICT PICKPROBE PASS")
    return 0


result = 0
steps = [0]


def step():
    global result
    steps[0] += 1
    if steps[0] == 2:
        result = main()
    if steps[0] >= 4:
        print("PICKHARNESS DONE", flush=True)
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(200, step)


QtCore.QTimer.singleShot(500, step)
