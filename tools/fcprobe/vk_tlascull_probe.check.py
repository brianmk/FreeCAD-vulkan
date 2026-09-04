#!/usr/bin/env python3
"""Host-side assertions for vk_tlascull_probe.py (TLAS instance culling).

The probe reads FC_VULKAN_TLAS_CULL itself and logs a [TLASCULL:on|off] marker
so this shared check can assert per-mode invariants:

  mode=on  : some [RTDBG] buildTlas line has culled>0 AND instances>0 on that
             same line -- the tiny field is culled while the keeper is still
             traced (selective cull, no over-cull of a visible object).
  mode=off : every [RTDBG] buildTlas line has culled==0 and at least one line
             still traces an instance (the scene renders normally).

Requires FC_VULKAN_RT_DEBUG=1 (the buildTlas line is gated on it).
"""

import re

TLAS_LINE = re.compile(
    r"\[RTDBG\] buildTlas: drawlist commands=(\d+) instances=(\d+) "
    r"culled=(\d+)")
MODE_LINE = re.compile(r"TLASCULL\[(on|off)\]")


def _tlas(lines):
    out = []
    for line in lines:
        m = TLAS_LINE.search(line)
        if m:
            out.append(tuple(int(m.group(i)) for i in range(1, 4)))
    return out


def _mode(lines):
    for line in lines:
        m = MODE_LINE.search(line)
        if m:
            return m.group(1)
    return None


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    mode = _mode(lines)
    if mode is None:
        err("no [TLASCULL:on|off] marker found (probe never ran)")
        return

    ev = _tlas(lines)
    if not ev:
        err("no [RTDBG] buildTlas lines found (renderer never ran?)")
        return

    if mode == "on":
        # A frame that culled at least one of the field AND still traced the
        # keeper proves selective culling (never over-cull a visible object).
        if not any(culled > 0 and inst > 0 for _, inst, culled in ev):
            err("cull=on: expected some [RTDBG] buildTlas line with "
                "culled>0 and instances>0 (field culled, keeper preserved); "
                "saw instances=%s culled=%s" %
                (max(i for _, i, _ in ev), max(c for _, _, c in ev)))
    else:
        # Culling disabled: nothing may be culled, and the scene still traces.
        if any(culled > 0 for _, _, culled in ev):
            err("cull=off: some [RTDBG] buildTlas line reported culled>0 "
                "but FC_VULKAN_TLAS_CULL was not set")
        if not any(inst > 0 for _, inst, _ in ev):
            err("cull=off: no instance was ever traced (instances==0)")
