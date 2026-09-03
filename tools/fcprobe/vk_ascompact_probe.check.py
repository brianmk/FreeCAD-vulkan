#!/usr/bin/env python3
"""Host-side assertions for vk_ascompact_probe.py (AS compaction).

The probe reads FC_VULKAN_AS_COMPACT itself and logs a [ASCOMPACT:on|off]
marker so this shared check can assert per-mode invariants:

  mode=on  : some [RTDBG] compact ... saved=1 line exists (the BLAS was
             shrunk into a smaller buffer, or a saved=0 run still proves the
             query+compare code path executed).
  mode=off : no [RTDBG] compact line (compaction never ran).

Requires FC_VULKAN_RT_DEBUG=1 (the compact line is gated on it).
"""

import re

COMPACT_LINE = re.compile(r"\[RTDBG\] compact size=(\d+) -> (\d+) saved=(\d+)")
MODE_LINE = re.compile(r"ASCOMPACT\[(on|off)\]")


def _compact(lines):
    out = []
    for line in lines:
        m = COMPACT_LINE.search(line)
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
        err("no [ASCOMPACT:on|off] marker found (probe never ran)")
        return

    ev = _compact(lines)
    if mode == "on":
        if not ev:
            err("ascompact=on: no [RTDBG] compact line found (compaction "
                "never ran)")
    else:
        if ev:
            err("ascompact=off: a [RTDBG] compact line was found but "
                "FC_VULKAN_AS_COMPACT was not set")
