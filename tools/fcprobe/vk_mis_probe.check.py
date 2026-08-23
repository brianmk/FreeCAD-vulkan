#!/usr/bin/env python3
"""Host-side assertions for vk_mis_probe.py (Phase 4: emissive NEE + MIS).

The probe runs two accumulation passes in one session: pass A with NEE on,
then a runtime FC_VULKAN_PT_NEE=0 toggle plus a camera nudge (accumulation
restart) for pass B.  Same scene/camera/lights in both passes, so the
frame difference is the pure NEE signal.

Per-frame ticks: the [RTDBG] adaptive lines print once per accumulating
frame and map 1:1 to the collected frame dumps (same assumption the BLAS
check makes with its own per-frame line).

Branches on the run's env:
  - NEE ON (default): pool holds >= 12 triangles, and the pass-A warm-glow
    pixel count (unsaturated emission-tinted pixels in the central region,
    excluding the cube's own saturated image and the UI overlays) must
    exceed pass B's by > 25%.
  - FC_VULKAN_PT_NEE=0: control - both passes are equal (< 10% drift).
"""

import glob
import os
import re
import sys

POOL_LINE = re.compile(r"\[RTDBG\] nee pool triangles=(\d+) bytes=(\d+)")
TICK_LINE = re.compile(r"\[RTDBG\] adaptive active=(\d+)/(\d+) "
                       r"fraction=([0-9.]+) frameIndex=(\d+) accum=(\d+)")
PHASE_LINE = re.compile(r"MIS phase=(\S+)")


def _stats(frame_path):
    from PIL import Image

    img = Image.open(frame_path).convert("RGB")
    w, h = img.size
    px = img.load()
    # Central 80%: excludes the navicube (top-right) and axis cross
    # (bottom-left) overlays, which are raster-rendered on top of the
    # traced image and carry their own red pixels.
    x0, x1 = w // 10, 9 * w // 10
    y0, y1 = h // 10, 9 * h // 10
    total = 0.0
    warm = 0
    n = 0
    for y in range(y0, y1, 4):
        for x in range(x0, x1, 4):
            r, g, b = px[x, y]
            total += 0.299 * r + 0.587 * g + 0.114 * b
            # Warm = orange-tinted (emission-colored) lit pixels: the
            # cube's glow.  Excludes the neutral floor/background and the
            # saturated emissive cube's own image (r < 250).
            if r > 60 and r < 250 and r > g * 1.4 and g > b:
                warm += 1
            n += 1
    return total / max(n, 1), warm


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    env = report.session.get("env_overrides", {})
    nee_off = env.get("FC_VULKAN_PT_NEE") == "0"

    pools = [int(m.group(1)) for line in lines
             for m in [POOL_LINE.search(line)] if m]
    if not pools:
        err("no [RTDBG] nee pool line (emissive material never reached "
            "the renderer?)")
        return
    if max(pools) < 12:
        err(f"nee pool too small: {max(pools)} < 12 (emissive cube "
            "triangles missing)")

    # Locate the toggle marker in the tick stream (1:1 with dumps).
    tick_idx = 0
    toggle_idx = None
    for i, line in enumerate(lines):
        m = PHASE_LINE.search(line)
        if m and m.group(1) == "toggle":
            toggle_idx = tick_idx
            break
        if TICK_LINE.search(line):
            tick_idx += 1
    if toggle_idx is None:
        err("no MIS phase=toggle marker (probe never reached the toggle)")
        return

    frames_dir = os.path.join(report.artifact_dir, "frames")
    frames = sorted(glob.glob(os.path.join(frames_dir, "*.png")),
                    key=lambda p: int(re.search(r"(\d+)",
                                                os.path.basename(p))
                                      .group(1)))
    if len(frames) < 16:
        err(f"too few frame dumps ({len(frames)})")
        return
    if toggle_idx >= len(frames):
        err("toggle marker beyond the collected dumps "
            f"(toggle={toggle_idx}, dumps={len(frames)})")
        return

    a = [_stats(f) for f in frames[max(2, toggle_idx - 6):toggle_idx]]
    b = [_stats(f) for f in frames[toggle_idx + 2:toggle_idx + 8]]
    mean_a = sum(s[0] for s in a) / max(len(a), 1)
    mean_b = sum(s[0] for s in b) / max(len(b), 1)
    warm_a = max(s[1] for s in a)
    warm_b = max(s[1] for s in b)
    sys.stderr.write(f"[CHECK] mis passA={mean_a:.2f} warmA={warm_a} "
                     f"passB={mean_b:.2f} warmB={warm_b} "
                     f"toggle={toggle_idx} nee_off={nee_off}\n")

    if nee_off:
        drift = abs(mean_a - mean_b) / max(mean_a, 1.0)
        if drift > 0.10:
            err(f"NEE OFF: passes drifted ({drift:.3f} > 0.10) - "
                "control run not stable")
    else:
        if warm_a <= warm_b * 1.25:
            err(f"NEE ON: glow not stronger with NEE "
                f"(warmA={warm_a}, warmB={warm_b}) - area sampling "
                "contributed no light")
        elif warm_a < 500:
            err(f"NEE ON: glow region too small (warmA={warm_a})")
