#!/usr/bin/env python3
"""Host-side assertions for vk_fullresolve_probe.py (ptForceFullResolve fix).

Invariant enforced by the fix: while the post-move full-resolve latch is set,
the adaptive per-pixel freeze is disabled, so an accumulating frame may only
show fill=0 (freeze re-engaged) once it has reached the sample cap
(frameIndex >= maxSamp).  If any accumulating frame has fill=0 BELOW the cap,
the run ram to idle on an adaptive-frozen (faint) image = the bug is back.

Parses the [RTDBG] adaptive lines:
  frameIndex=F accum=A maxSamp=M fill=P
Requires FC_VULKAN_RT_DEBUG=1.
"""

import re

ADAPTIVE = re.compile(
    r"adaptive frame=\d+ .*? frameIndex=(\d+) accum=(\d) .*? maxSamp=(\d+) "
    r"minSamp=\d+ fill=(\d)")


def _frames(lines):
    out = []
    for line in lines:
        m = ADAPTIVE.search(line)
        if m and int(m.group(2)) == 1:  # accumulating only
            out.append((int(m.group(1)), int(m.group(3)), int(m.group(4))))
    return out


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    acc_frames = _frames(lines)
    if not acc_frames:
        err("no accumulating [RTDBG] adaptive frame seen (probe never "
            "accumulated)")
        return

    maxsamp = max(f[1] for f in acc_frames)
    precap_fill0 = [f for f in acc_frames if f[2] == 0 and f[0] < f[1]]
    minsi_below = [f for f in acc_frames if f[0] < maxsamp]
    if precap_fill0:
        err("full-resolve broken: %d accumulating frame(s) re-engaged the "
            "freeze (fill=0) below the sample cap (maxSamp=%d): "
            "frameIndex=%s" % (len(precap_fill0), maxsamp,
                               [f[0] for f in precap_fill0]))
    else:
        # Positive check: the freeze stayed off through the cap (fill=1 held).
        reached = max(f for f in acc_frames if f[2] == 1)[0]
        report.log_event("fullresolve", "state",
                         frames=len(acc_frames), maxsamp=maxsamp,
                         freeze_off_through=reached)
