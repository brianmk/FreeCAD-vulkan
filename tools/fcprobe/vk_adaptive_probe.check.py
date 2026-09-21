#!/usr/bin/env python3
"""Host-side assertions for vk_adaptive_probe.py (Phase 3: adaptive sampling).

Since the full-resolve change (coin ee7c033f7) the renderer forces a
full-resolve run after a fresh start / scene change / camera move: the
per-pixel adaptive freeze is gated off (u_adaptive.w = 0) while
ptForceFullResolve is set, which lasts until the hard sample cap is reached.
So a forced run never early-outs and never stops below the cap.

Branches on the run's env:
  - adaptive ON  (default): every accumulating frame is a forced full-resolve
    frame (fill=1), the active fraction stays 1.0, and the run reaches the cap.
  - FC_VULKAN_PT_ADAPTIVE=0: control; same shape (fraction 1.0, reaches cap).
"""

import re

STATE_LINE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"bgChanged=(\d) latch=(\d) accum=(\d) frameIndex=(\d+) idle=(\d+) "
    r"reproject=(\d)")
ADAPT_LINE = re.compile(
    r"\[RTDBG\] adaptive frame=(\d+) active=(\d+)/(\d+) fraction=([0-9.]+) "
    r"frameIndex=(\d+) accum=(\d).*?maxSamp=(\d+) minSamp=(\d+) fill=(\d)")


def _states(lines):
    # groups: 1=frame, 2=viewChanged, 3=sceneChanged, 4=bgChanged, 5=latch,
    # 6=accum, 7=frameIndex, 8=idle, 9=reproject.
    return [(m, int(m.group(6)), int(m.group(7)), int(m.group(8)))
            for line in lines for m in [STATE_LINE.search(line)] if m]


def _adaptives(lines):
    # groups: 1=frame, 2=active, 3=total, 4=fraction, 5=frameIndex, 6=accum,
    # 7=maxSamp, 8=minSamp, 9=fill (ptForceFullResolve).
    return [(m, int(m.group(2)), int(m.group(3)), float(m.group(4)),
             int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(9)))
            for line in lines for m in [ADAPT_LINE.search(line)] if m]


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    env = report.session.get("env_overrides", {})
    adaptive_off = env.get("FC_VULKAN_PT_ADAPTIVE") == "0"
    maxsamples = int(env.get("FC_VULKAN_PT_MAXSAMPLES", "256"))

    states = _states(lines)
    adaptives = _adaptives(lines)
    if not adaptives:
        err("no [RTDBG] adaptive lines found (renderer never ran?)")
        return

    # Trust the cap the renderer actually used: the View preference
    # (VulkanPathTracingMaxSamples) can override the FC_VULKAN_PT_MAXSAMPLES
    # env the manifest sets, so the env value alone is not authoritative.
    maxsamples = max((a[6] for a in adaptives), default=maxsamples)

    # Active accumulating frames, excluding the cap-transition frame (where
    # frameIndex == maxSamp, ptForceFullResolve has just been cleared and the
    # active counter reads 0).
    accumulating = [a for a in adaptives if a[5] == 1 and a[4] < a[6]]
    if not accumulating:
        err("no accumulating frames observed")
        return

    label = "adaptive OFF" if adaptive_off else "adaptive ON"

    # Every accumulating frame must trace every pixel: with adaptive off that
    # is the point of the control; with adaptive on the forced full-resolve
    # run gates the freeze off (u_adaptive.w = 0) until the cap.
    active = [a for a in accumulating if a[3] != 1.0]
    if active:
        err(f"{label}: {len(active)} accumulating frames with fraction != 1.0 "
            "(adaptive freeze fired during the forced full-resolve run)")

    if not adaptive_off:
        # With adaptive on, the suppression is specifically the forced
        # full-resolve flag; assert it is actually set on the run.
        not_full = [a for a in accumulating if a[7] != 1]
        if not_full:
            err(f"adaptive ON: {len(not_full)} accumulating frames were not a "
                "forced full-resolve run (fill != 1)")

    # The forced run must reach the hard cap, not stop below it.  With a
    # denoiser the run keeps accumulating past the cap until the async denoise
    # publishes, so the idle transition's frameIndex is not the cap: the cap is
    # reached as soon as an accumulating frame hits it.
    reached_cap = max((a[4] for a in adaptives), default=0) >= maxsamples
    if not reached_cap:
        err(f"{label}: run did not reach the cap (max={maxsamples}); "
            "it stopped early")
    if len(states) < 2:
        err(f"{label}: not enough state transitions observed")
