#!/usr/bin/env python3
"""Host-side assertions for vk_pick_far_probe.py.

Guards the coupled regressions on large / off-origin models:

* the CPU pick must reach the model -- the probe must find a hover + pick hit;
  and
* the ground-plane grid must not feed the camera's published near/far planes
  back into the scene bounding box -- the sampled far plane must stay *bounded*
  instead of compounding ~3x per frame.

The far plane is now deliberately extended past the model so the clip range
covers the camera-coupled ground grid (otherwise the rasterizer clips the drawn
ground to a thin strip).  That is a stable, far-independent extension, so the
failure signal is no longer "far larger than the model": it is a far that grows
frame over frame (compounding feedback) or runs away past ``MAX_SANE_FAR``.
The probe reports the model edge on its ``viewport ... box=<n>`` line; a far
below the model means the auto-clip never ran.
"""

import re

FRAME = re.compile(r"FARPICK frame=\d+ near=[-\d.eE+]+ far=([-\d.eE+]+)")
HIT = re.compile(r"FARPICK hit=\(")
BOX = re.compile(r"FARPICK viewport .* box=([\d.eE+]+)")

# The auto-clip far plane must reach at least the model.  There is no tight
# upper multiple: the ground grid legitimately extends the far to cover the
# visible ground.  A runaway is caught by the compounding-streak check and the
# absolute bound below.
MIN_FAR_TO_BOX = 0.9
MAX_SANE_FAR = 1.0e6


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    hit = any(HIT.search(line) for line in lines)

    box = None
    fars = []
    for line in lines:
        m = FRAME.search(line)
        if m:
            try:
                fars.append(float(m.group(1)))
            except ValueError:
                pass
        b = BOX.search(line)
        if b:
            try:
                box = float(b.group(1))
            except ValueError:
                pass

    if not hit:
        err("pick/hover found no geometry on a large off-origin model "
            "(the CPU pick cannot reach the model)")
    if not fars:
        err("no 'FARPICK frame=' plane samples captured (the probe never drove "
            "a Vulkan frame)")
        return

    maxfar = max(fars)
    if box:
        if maxfar < box * MIN_FAR_TO_BOX:
            err(f"camera far {maxfar:.1f} is below the {box:.0f} model: the "
                "auto-clip path was not exercised")
    if maxfar > MAX_SANE_FAR:
        err(f"camera far ran away to {maxfar:.3e} (ground-plane/view-volume "
            "feedback loop)")

    # A compounding runaway shows several monotonic >1.5x increases; report the
    # longest such streak so a regression is visible even below the absolute
    # bound.
    streak = 0
    worst = 0
    prev = None
    for v in fars:
        if prev is not None and v > prev * 1.5:
            streak += 1
            worst = max(worst, streak)
        else:
            streak = 0
        prev = v
    if worst >= 3:
        err(f"camera far compounded >1.5x on {worst} consecutive frames "
            "(runaway feedback)")

    report.log_event(
        "pick-far", "ok",
        hit=hit, box=box, max_far=maxfar, samples=len(fars),
        worst_growth_streak=worst)
