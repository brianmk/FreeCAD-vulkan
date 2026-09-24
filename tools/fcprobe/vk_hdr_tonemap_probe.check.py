#!/usr/bin/env python3
"""Host-side assertions for vk_hdr_tonemap_probe.py.

The probe cycles the VulkanHDRToneMap operator through Clip / Reinhard / ACES /
Hable while rendering a bright emissive box in the Vulkan raster viewport.

Two invariants:
  - The probe reached every phase (the `HDRTM phase=<name> hdrToneMap=<n>`
    markers are emitted unconditionally).
  - The renderer actually accepted every operator: a `[VK-SET] ... hdrToneMap=N`
    breadcrumb must exist for each index.  This needs FC_VULKAN_BACKEND_DEBUG=1
    (set by the suite case); without it the [VK-SET] lines are not emitted and
    the check fails rather than silently passing.
"""

import re

SET_LINE = re.compile(r"\[VK-SET\].*hdrToneMap=(\d+)")
PHASE_LINE = re.compile(r"HDRTM phase=(\w+) hdrToneMap=(\d+)")

# VulkanHDRToneMap index order.
OPERATORS = {0: "clip", 1: "reinhard", 2: "aces", 3: "hable"}


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    phases = {int(m.group(2)) for line in lines for m in [PHASE_LINE.search(line)] if m}
    if set(OPERATORS) - phases:
        err("probe did not reach every tone-map phase (saw %s, expect %s)"
            % (sorted(phases), sorted(OPERATORS)))

    accepted = {int(m.group(1)) for line in lines for m in [SET_LINE.search(line)] if m}
    if not accepted:
        err("no [VK-SET] hdrToneMap breadcrumbs (is FC_VULKAN_BACKEND_DEBUG=1 set?)")
        return
    missing = set(OPERATORS) - accepted
    if missing:
        err("renderer never accepted tone-map operator(s) %s (saw %s)"
            % ([OPERATORS[i] for i in sorted(missing)], sorted(accepted)))
