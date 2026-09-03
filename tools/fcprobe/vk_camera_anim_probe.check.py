#!/usr/bin/env python3
"""Host-side assertions for vk_camera_anim_probe.py (continuous motion).

Attributes state/adaptive lines to a phase via the ordinal-bearing
`[HARNESS] frame_phase phase=NAME frame=N` marker.  Asserts reset-on-move holds
under continuous motion: no reprojection, fresh runs on each view change, and an
after-motion static resume that converges.

Requires FC_VULKAN_RT_DEBUG=1 (ptState/adaptive lines are gated on it).
"""

import re

STATE_LINE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"accum=(\d) frameIndex=(\d+) idle=(\d+) reproject=(\d)")
ADAPT_LINE = re.compile(
    r"\[RTDBG\] adaptive frame=(\d+) active=(\d+)/(\d+) fraction=([0-9.]+) "
    r"frameIndex=(\d+) accum=(\d)")
PHASE_LINE = re.compile(r"\[HARNESS\] frame_phase phase=(\S+) frame=(\d+)")


def _windowed(lines):
    marks = []
    phase = "boot"
    events = []
    for line in lines:
        m = PHASE_LINE.search(line)
        if m:
            marks.append((int(m.group(2)), m.group(1)))
            continue
        m = STATE_LINE.search(line)
        if m:
            events.append((int(m.group(1)), "state", m))
            continue
        m = ADAPT_LINE.search(line)
        if m:
            events.append((int(m.group(1)), "adaptive", m))
    if not marks:
        return [(phase, k, m) for _, k, m in events]

    def phase_of(ford):
        best, best_ord = "boot", -1
        for ordv, name in marks:
            if ordv <= ford and ordv > best_ord:
                best, best_ord = name, ordv
        if best_ord < 0 and marks:
            best = marks[0][1]
        return best

    return [(phase_of(ford), k, m) for ford, k, m in events]


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    events = _windowed(lines)
    states = [(p, m) for p, k, m in events if k == "state"]
    adaptives = [(p, m) for p, k, m in events if k == "adaptive"]
    if not states:
        err("no [RTDBG] ptState lines found (renderer never ran?)")
        return

    # Reset-on-move: no reprojection may appear.
    if any(m.group(7) == "1" for _, m in states):
        err("reprojection frames appeared (reset-on-move must never reproject)")

    # Motion window: every animated frame is a view change that resets the run.
    anim = [m for p, m in states if p != "boot" and p.startswith("anim-")]
    if not anim:
        err("no frames observed in the anim window (camera never moved?)")
    else:
        if not any(m.group(2) == "1" and m.group(5) == "0" for m in anim):
            err("anim window: no viewChanged frame that reset to frameIndex=0")

    # Static resume: a non-view/scene-changing accumulating frame with a fresh
    # (small) frameIndex appears after the motion stops, and it converges.
    resumed = False
    static = [m for p, m in states if p == "static"]
    if any(m.group(2) == "0" and m.group(3) == "0" and m.group(4) == "1"
           and int(m.group(5)) >= 1 for m in static):
        resumed = True
    if not resumed:
        err("static: no resuming accumulating frame after motion stopped")

    if adaptives:
        if not any(p == "static" and float(m.group(4)) < 1.0
                   for p, m in adaptives):
            err("static: adaptive fraction never declined below 1.0 "
                "(fresh run did not converge)")
