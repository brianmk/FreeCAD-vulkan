#!/usr/bin/env python3
"""Host-side assertions for vk_prefs_probe.py (Vulkan display prefs read+render).

The probe cycles the Vulkan-only View prefs (VulkanShowEdges / VulkanShowPoints
/ VulkanEdgeColor) while rendering a Part::Box, emitting a `[HARNESS] frame_phase`
per phase and dumping frames.  Two invariants:

  - Code path: the `[VK-TRACE] View3DInventorViewer::applyVulkanSettings` breadcrumb
    must have recorded the edges=0->1 (and points=1) transitions the probe made.
  - Render: with edges on, at least one dumped frame must contain >= `MIN_PX`
    pixels of the configured edge color (the overlays actually drew); with edges
    off (baseline) some frame must have none (so a stuck-on overlay is caught).

Run via the suite with FC_VULKAN_BREADCRUMBS=1 (so the applyVulkanSettings
breadcrumb is emitted into the trace log, which the harness folds into the run
events) and FC_VULKAN_DUMP_FRAME=1 (so frames are dumped).
"""

import glob
import os
import re

EDGE_LINE = re.compile(
    r"\[VK-TRACE\] View3DInventorViewer::applyVulkanSettings "
    r"edges=(\d+) points=(\d+)")
EDGE_RGB = (255, 0, 0)      # matches the probe's VulkanEdgeColor=0xFF0000FF
TOL = 12
MIN_PX = 50


def _breadcrumbs(lines):
    # [(edges, points)] for every applyVulkanSettings record.
    return [(int(m.group(1)), int(m.group(2)))
            for line in lines for m in [EDGE_LINE.search(line)] if m]


def _edge_counts(frames_dir):
    try:
        from PIL import Image
    except ImportError:
        return None
    import numpy as np
    target = np.array(EDGE_RGB, dtype=np.int32)
    out = []
    for p in sorted(glob.glob(os.path.join(frames_dir, "*.png"))):
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.int32)
        dist = np.abs(a - target).max(axis=2)
        out.append(int((dist <= TOL).sum()))
    return out


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    crumbs = _breadcrumbs(lines)
    if not crumbs:
        err("no applyVulkanSettings breadcrumb (code path never ran; "
            "is FC_VULKAN_BREADCRUMBS=1 set?)")
        return

    edges_on = [e for e in crumbs if e[0] == 1]
    edges_off = [e for e in crumbs if e[0] == 0]
    points_on = [p for p in crumbs if p[1] == 1]
    if not edges_on:
        err("applyVulkanSettings never recorded edges=1")
    if not edges_off:
        err("applyVulkanSettings never recorded edges=0 (baseline)")
    if not points_on:
        err("applyVulkanSettings never recorded points=1")

    frames_dir = os.path.join(report.artifact_dir, "frames")
    counts = _edge_counts(frames_dir)
    if counts is None:
        err("edge-count check skipped: PIL/numpy not available")
        return
    if not counts:
        err("no frame dumps to analyze (is FC_VULKAN_DUMP_FRAME=1 set?)")
        return
    if max(counts) < MIN_PX:
        err(f"no frame renders edges in {EDGE_RGB}"
            f" (max {max(counts)} px, need >= {MIN_PX})")
    if 0 not in counts:
        err("no frame with 0 edge pixels (edge overlay may be stuck on)")
