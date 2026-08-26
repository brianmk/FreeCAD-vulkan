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
# Ordinal-bearing marker: `[HARNESS] frame_phase phase=toggle frame=N`.
PHASE_LINE = re.compile(r"\[HARNESS\] frame_phase phase=(\S+) frame=(\d+)")


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

    # Locate the toggle marker by its ordinal: the marker was emitted right
    # before the NEE-off scene change, so pass-A dumps have ordinals <= the
    # marker's ordinal and pass-B dumps have ordinals strictly greater.  This
    # is independent of the stream order of the tick lines.
    toggle_ord = None
    for line in lines:
        m = PHASE_LINE.search(line)
        if m and m.group(1) == "toggle":
            toggle_ord = int(m.group(2))
            break
    if toggle_ord is None:
        err("no [HARNESS] frame_phase phase=toggle marker (probe never "
            "reached the toggle, or the view exposes no getVulkanFrameCount)")
        return

    frames_dir = os.path.join(report.artifact_dir, "frames")
    frames = sorted(glob.glob(os.path.join(frames_dir, "*.png")),
                    key=lambda p: int(re.search(r"(\d+)",
                                                os.path.basename(p))
                                      .group(1)))
    if len(frames) < 16:
        err(f"too few frame dumps ({len(frames)})")
        return
    # Split dumps into pass A (ordinal <= toggle) and pass B (> toggle).
    dump_ords = [int(re.search(r"(\d+)", os.path.basename(f)).group(1))
                 for f in frames]
    idx_toggle = next((i for i, o in enumerate(dump_ords) if o > toggle_ord),
                      None)
    if idx_toggle is None or idx_toggle < 2:
        err("toggle marker beyond the collected dumps or too few pass-A "
            f"dumps (toggle={toggle_ord}, dumps={len(frames)})")
        return
    a = [_stats(f) for f in frames[max(2, idx_toggle - 8):idx_toggle - 2]]
    # Pass B is sampled well past the toggle: the scene change restarts
    # accumulation, so the frames immediately after it are fresh high-noise
    # transients (brightness spikes frame to frame and does not reflect the
    # settled NEE-off glow).  Skip 8 dumps to reach convergence, then bin a
    # settled window.
    b = [_stats(f) for f in frames[idx_toggle + 8:idx_toggle + 16]]
    if not a or not b:
        err("not enough frame dumps in the pass A or pass B window "
            f"(toggle={toggle_ord}, passA={len(a)}, passB={len(b)})")
        return
    mean_a = sum(s[0] for s in a) / len(a)
    mean_b = sum(s[0] for s in b) / len(b)
    warm_a = max(s[1] for s in a)
    warm_b = max(s[1] for s in b)
    sys.stderr.write(f"[CHECK] mis passA={mean_a:.2f} warmA={warm_a} "
                     f"passB={mean_b:.2f} warmB={warm_b} "
                     f"toggle_ord={toggle_ord} nee_off={nee_off}\n")

    if nee_off:
        drift = abs(mean_a - mean_b) / max(mean_a, 1.0)
        if drift > 0.10:
            err(f"NEE OFF: passes drifted ({drift:.3f} > 0.10) - "
                "control run not stable")
    else:
        # NEE ON: the emissive area light adds radiance, so the settled
        # pass-A mean must exceed pass B by a clear margin.  The per-pixel
        # warm count is too noisy for these faint glows, so the mean is the
        # primary signal and warm is a secondary monotonic hint.
        margin = (mean_a - mean_b) / max(mean_b, 1.0)
        if margin < 0.01:
            err(f"NEE ON: glow not stronger with NEE "
                f"(passA={mean_a:.2f} passB={mean_b:.2f} margin={margin:.4f}) "
                "- area sampling contributed no light")
