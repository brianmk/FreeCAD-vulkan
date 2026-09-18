#!/usr/bin/env python3
"""Host-side assertions for vk_geomlod_probe.py (raster geometry LOD).

A fresh document's main draw list is usually only the hidden nav cube (~36
vertices), so a nav-cube-only run can make the LOD look like it works while
never touching real document geometry.  This check therefore asserts BOTH:

  * ``[DRAWLIST] mainMaxVc`` reports the shape's real vertex count
    (>= MIN_MAIN_VERTS), i.e. the document geometry reached the main pass; and
  * ``[GEOMLOD] prepass`` compacted that command (compacted>0 and maxPrims>0),
    i.e. the LOD actually ran on it.

Requires FC_VULKAN_BACKEND_DEBUG=1 and FC_VULKAN_GEOM_LOD_ALWAYS=1.
"""

import re

# The nav cube is ~36 vertices.  Anything at or above this is real geometry.
MIN_MAIN_VERTS = 1000

DRAWLIST = re.compile(
    r"\[DRAWLIST\] main=(\d+) total=(\d+) replayed=\d+ "
    r"mainMaxVc=(\d+) totalMaxVc=(\d+)")
PREPASS = re.compile(
    r"\[GEOMLOD\] prepass slot=\d+ compacted=(\d+) skipped=(\d+) "
    r"threshold=[\d.]+px2 maxPrims=(\d+) maxVc=(\d+) maxIc=(\d+)")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    draw = [DRAWLIST.search(line) for line in lines]
    draw = [m for m in draw if m]
    if not draw:
        err("no [DRAWLIST] line found (is FC_VULKAN_BACKEND_DEBUG=1 set?)")
        return
    main_max_vc = max(int(m.group(3)) for m in draw)
    if main_max_vc < MIN_MAIN_VERTS:
        err("mainMaxVc=%d < %d: the main draw list never carried the real "
            "shape (only the nav cube?)" % (main_max_vc, MIN_MAIN_VERTS))

    pre = [PREPASS.search(line) for line in lines]
    pre = [m for m in pre if m]
    if not pre:
        err("no [GEOMLOD] prepass line found "
            "(is FC_VULKAN_GEOM_LOD_ALWAYS=1 set?)")
        return
    if not any(int(m.group(1)) > 0 and int(m.group(3)) > 0 for m in pre):
        err("geometry LOD never compacted the real shape: "
            "compacted=%s maxPrims=%s" %
            ([m.group(1) for m in pre], [m.group(3) for m in pre]))
