#!/usr/bin/env python3
"""Host-side assertions for vk_sketch_view_anim_probe.py.

Re-parses the probe's `[HARNESS] sketchviewanim*` records and enforces the
Create-sketch rotation invariants (independent of the probe's own verdict):

  - with nothing selected the view animates to isometric: the final direction
    is within 3 deg of the axonometric view, intermediate orientations exist
    (a snap would report 0) and the motion span is near the configured
    NewSketchViewAnimationDuration;
  - with a plane selected the view does not rotate.
"""

import re

UNSELECTED = re.compile(
    r"sketchviewanim_result phase=unselected command_ok=(\d) "
    r"from_iso=([0-9.]+) intermediate=(\d+) span=([0-9.]+)"
)
SELECTED = re.compile(r"sketchviewanim_result phase=selected max_angle=([0-9.]+)")
CONFIGURED = re.compile(r"sketchviewanim phase=unselected dur=([0-9.]+)")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    configured = None
    unselected = None
    selected = None
    for line in lines:
        m = CONFIGURED.search(line)
        if m:
            configured = float(m.group(1))
        m = UNSELECTED.search(line)
        if m:
            unselected = m
        m = SELECTED.search(line)
        if m:
            selected = m

    if configured is None:
        err("unselected phase never ran (no configured-duration record)")
    elif not 0.9 <= configured <= 1.1:
        err(f"configured duration {configured}s was not the probe's 1.0s")

    if unselected is None:
        err("unselected phase produced no result record")
    else:
        command_ok = int(unselected.group(1))
        from_iso = float(unselected.group(2))
        intermediate = int(unselected.group(3))
        span = float(unselected.group(4))
        if command_ok != 1:
            err("PartDesign_NewSketch did not run in the unselected phase")
        if from_iso > 3.0:
            err(f"camera settled {from_iso:.2f} deg off the isometric view")
        if intermediate < 3:
            err(f"only {intermediate} intermediate orientations: "
                "the view snapped instead of animating")
        if not 0.4 <= span <= 1.8:
            err(f"rotation span {span:.2f}s is not near the configured "
                f"{configured if configured else 1.0:.1f}s")

    if selected is None:
        err("selected phase produced no result record")
    else:
        max_angle = float(selected.group(1))
        if max_angle > 3.0:
            err(f"view rotated {max_angle:.2f} deg despite a selected plane")
