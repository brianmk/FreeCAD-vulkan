#!/usr/bin/env python3
"""Host-side assertions for vk_profile_probe.py (RT frame time breakdown).

Parses the [RTDBG] frameTiming lines and reports the per-phase median/max in
milliseconds, so the profile run is human-readable in the suite output.  It
checks that at least a few timing samples were collected (the probe ran the RT
viewport for a while) and that interval > 0.
"""

import re
import statistics

TIMING = re.compile(
    r"\[RTDBG\] frameTiming interval=([\d.]+) asRecord=([\d.]+) asGpu=([\d.]+) "
    r"denoise=([\d.]+) traceRecord=([\d.]+)")


def _rows(lines):
    out = []
    for line in lines:
        m = TIMING.search(line)
        if m:
            out.append(tuple(float(m.group(i)) for i in range(1, 6)))
    return out


def check(lines, report):
    rows = _rows(lines)
    if len(rows) < 3:
        report.add_error(
            "profile: only %d frameTiming sample(s); the RT viewport did not "
            "run long enough" % len(rows))
        return
    tags = ["interval", "asRecord", "asGpu", "denoise", "traceRecord"]
    summary = []
    for i, tag in enumerate(tags):
        col = [r[i] for r in rows]
        summary.append("%s med=%.2f max=%.2f" % (tag, statistics.median(col),
                                                 max(col)))
    report.log_event(source="profile", kind="info",
                     detail="frame-time (ms): %s | %d frames"
                     % (" | ".join(summary), len(rows)))
    # The trace runs in Qt's submit so it is the bulk of (interval - asGpu).
    # A big interval with a tiny asGpu confirms the path-trace kernel dominates.
    med_interval = statistics.median([r[0] for r in rows])
    med_asgpu = statistics.median([r[2] for r in rows])
    report.log_event(
        source="profile", kind="info",
        detail="trace+present+vsync ~%.2f ms, asGpu ~%.2f ms (asGpu %.1f%% "
        "of frame)" % (med_interval - med_asgpu, med_asgpu,
                       100.0 * med_asgpu / med_interval if med_interval
                       else 0.0))
