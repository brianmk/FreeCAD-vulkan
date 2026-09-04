#!/usr/bin/env python3
"""Host-side assertions for vk_points_probe.py (VulkanShowPoints / round glyph).

The probe pins VulkanEdgeColor to red and toggles VulkanShowPoints on a
tessellated sphere, emitting ordinal-bearing `[HARNESS] frame_phase` markers.
This check asserts:

  - code path: the `applyVulkanSettings` breadcrumb recorded points=0 and then
    points=1 (the pref was actually read by View3DSettings::OnChange).
  - render: the "points" window produced more overlay-colored pixels than the
    baseline window (points actually drew), and frame dumps exist.

Requires FC_VULKAN_BREADCRUMBS=1 (breadcrumb) and FC_VULKAN_DUMP_FRAME=1.
"""

import glob
import os
import re

EDGE_LINE = re.compile(
    r"\[VK-TRACE\] View3DInventorViewer::applyVulkanSettings "
    r"edges=(\d+) points=(\d+)")
PHASE_LINE = re.compile(r"\[HARNESS\] frame_phase phase=(\S+) frame=(\d+)")
RED = (255, 0, 0)
TOL = 12


def _crumbs(lines):
    return [(int(m.group(1)), int(m.group(2)))
            for line in lines for m in [EDGE_LINE.search(line)] if m]


def _by_phase(lines):
    marks = []
    phase = "boot"
    for line in lines:
        m = PHASE_LINE.search(line)
        if m:
            marks.append((int(m.group(2)), m.group(1)))
            continue
        # phase is implied by the previous marker for the dump-ordinal windows
    # Simplest: return ordered (frame_ord, name); callers bucket the dumps.
    return marks


def _red_counts(frames_dir):
    """[(frame_ordinal, count)] sorted by the ordinal in the filename."""
    try:
        from PIL import Image
    except ImportError:
        return None
    import numpy as np
    target = np.array(RED, dtype=np.int32)
    out = []
    for p in glob.glob(os.path.join(frames_dir, "*.png")):
        ordv = int(re.search(r"(\d+)", os.path.basename(p)).group(1) or 0)
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.int32)
        cnt = int((np.abs(a - target).max(axis=2) <= TOL).sum())
        out.append((ordv, cnt))
    return sorted(out)


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    crumbs = _crumbs(lines)
    if not crumbs:
        err("no applyVulkanSettings breadcrumb (is FC_VULKAN_BREADCRUMBS=1 set?)")
        return
    if not any(p == 1 for _, p in crumbs):
        err("applyVulkanSettings never recorded points=1")
    if not any(p == 0 for _, p in crumbs):
        err("applyVulkanSettings never recorded points=0 (baseline)")

    marks = _by_phase(lines)
    ords = {name: ordv for ordv, name in marks}
    frames_dir = os.path.join(report.artifact_dir, "frames")
    counts = _red_counts(frames_dir)
    if counts is None:
        err("point-count check skipped: PIL/numpy not available")
        return
    if not counts:
        err("no frame dumps to analyze (is FC_VULKAN_DUMP_FRAME=1 set?)")
        return

    # Bucket dumps by the marker whose ordinal is <= the dump ordinal.
    def phase_of(ford):
        best, best_ord = "boot", -1
        for ordv, name in marks:
            if ordv <= ford and ordv > best_ord:
                best, best_ord = name, ordv
        if best_ord < 0 and marks:
            best = marks[0][1]
        return best

    bucketed = {}
    for ford, c in counts:
        bucketed.setdefault(phase_of(ford), []).append(c)
    base = max(bucketed.get("baseline", [0]), default=0)
    pts = max(bucketed.get("points", [0]), default=0)
    if pts <= base:
        err(f"points window (max {pts} px) did not exceed baseline "
            f"({base} px) - points not rendered / not edge-colored")
