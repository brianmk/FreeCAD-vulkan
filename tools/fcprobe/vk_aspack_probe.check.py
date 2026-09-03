#!/usr/bin/env python3
"""Host-side assertions for vk_aspack_probe.py (BLAS 16-bit position packing).

The probe reads FC_VULKAN_AS_PACK itself and logs a [ASPACK:on|off] marker so
this shared check can assert per-mode invariants:

  mode=on  : some [RTDBG] blasFmt build=1 packed=1 line exists (payload
             uploaded as R16G16B16_SFLOAT).
  mode=off : every [RTDBG] blasFmt line has packed=0 (32-bit default path).

Requires FC_VULKAN_RT_DEBUG=1 (the blasFmt line is gated on it).
"""

import re

FMT_LINE = re.compile(r"\[RTDBG\] blasFmt build=(\d+) packed=(\d+) stride=(\d+)")
MODE_LINE = re.compile(r"ASPACK\[(on|off)\]")


def _fmt(lines):
    out = []
    for line in lines:
        m = FMT_LINE.search(line)
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
        err("no [ASPACK:on|off] marker found (probe never ran)")
        return

    ev = _fmt(lines)
    if not ev:
        err("no [RTDBG] blasFmt lines found (renderer never ran?)")
        return

    if mode == "on":
        if not any(packed == 1 for _, packed, _ in ev):
            err("aspack=on: expected a [RTDBG] blasFmt packed=1 line "
                "(16-bit packing did not engage)")
    else:
        if any(packed == 1 for _, packed, _ in ev):
            err("aspack=off: a [RTDBG] blasFmt line reported packed=1 but "
                "FC_VULKAN_AS_PACK was not set")
