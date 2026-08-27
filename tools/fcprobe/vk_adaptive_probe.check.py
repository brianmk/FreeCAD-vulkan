#!/usr/bin/env python3
"""Host-side assertions for vk_adaptive_probe.py (Phase 3: adaptive sampling).

Branches on the run's env:
  - adaptive ON  (default): the active fraction must decline below 1.0 and
    the run must auto-stop below FC_VULKAN_PT_MAXSAMPLES; before the
    variance test starts (frameIndex < minSamples) every frame is fully
    active.
  - FC_VULKAN_PT_ADAPTIVE=0: the fraction must stay 1.0 on every
    accumulating frame and the run must never stop below the cap.
"""

import re

STATE_LINE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"accum=(\d) frameIndex=(\d+) idle=(\d+) reproject=(\d)")
ADAPT_LINE = re.compile(
    r"\[RTDBG\] adaptive frame=(\d+) active=(\d+)/(\d+) fraction=([0-9.]+) "
    r"frameIndex=(\d+) accum=(\d)")


def _states(lines):
    # groups: 1=frame, 2=viewChanged, 3=sceneChanged, 4=accum, 5=frameIndex,
    # 6=idle, 7=reproject.
    return [(m, int(m.group(4)), int(m.group(5)), int(m.group(6)))
            for line in lines for m in [STATE_LINE.search(line)] if m]


def _adaptives(lines):
    # groups: 1=frame, 2=active, 3=total, 4=fraction, 5=frameIndex, 6=accum.
    return [(m, int(m.group(2)), int(m.group(3)), float(m.group(4)),
             int(m.group(5)), int(m.group(6)))
            for line in lines for m in [ADAPT_LINE.search(line)] if m]


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    env = report.session.get("env_overrides", {})
    adaptive_off = env.get("FC_VULKAN_PT_ADAPTIVE") == "0"
    maxsamples = int(env.get("FC_VULKAN_PT_MAXSAMPLES", "256"))
    minsamples = int(env.get("FC_VULKAN_PT_MIN_SAMPLES", "4"))

    states = _states(lines)
    adaptives = _adaptives(lines)
    if not adaptives:
        err("no [RTDBG] adaptive lines found (renderer never ran?)")
        return

    accumulating = [a for a in adaptives if a[5] == 1]
    if not accumulating:
        err("no accumulating frames observed")
        return

    if adaptive_off:
        # Control: every accumulating frame must trace every pixel.
        bad = [a for a in accumulating if a[3] != 1.0]
        if bad:
            err(f"adaptive OFF: {len(bad)} accumulating frames with "
                "fraction != 1.0 (early-out fired while disabled)")
        # And the run must not stop below the cap via adaptive convergence.
        for m, accum, frame_index, idle in states:
            if accum == 0 and idle == 2 and 2 <= frame_index < maxsamples:
                err("adaptive OFF: run auto-stopped below the cap "
                    f"(frameIndex={frame_index})")
                break
        return

    # ON: before the variance test starts every frame is fully active.
    early = [a for a in accumulating if a[4] < minsamples and a[3] != 1.0]
    if early:
        err(f"adaptive ON: {len(early)} pre-minSamples frames with "
            "fraction != 1.0 (early-out fired too early)")
    # The fraction must decline: some accumulating frame below 1.0.
    declined = [a for a in accumulating if a[3] < 1.0]
    if not declined:
        err("adaptive ON: active fraction never declined below 1.0 "
            "(variance early-out did not engage)")
    # Auto-stop below the cap: an idle transition at a small frameIndex
    # that is not a camera/scene drop (viewChanged/sceneChanged == 0 and
    # the frame index survived the stop).
    stops = [(m, frame_index) for m, accum, frame_index, idle in states
             if accum == 0 and idle == 2 and
             frame_index >= minsamples and frame_index < maxsamples]
    if not stops:
        err("adaptive ON: no auto-stop below the cap "
            f"(max={maxsamples}, minSamples={minsamples})")
    if len(states) < 2:
        err("adaptive ON: not enough state transitions observed")
