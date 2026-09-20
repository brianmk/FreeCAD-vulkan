#!/usr/bin/env python3
"""Host-side assertions for vk_interaction_probe.py (Phase 0 baseline).

Parses the `[HARNESS] interaction` records and asserts the scripted sequence
actually drove the interaction pipeline on the selected surface:

  - click-select selected the box
  - the orbit drag changed the camera orientation
  - the pan drag changed the camera position

A refactor that stops delivering events (or a navigation host that no longer
reaches the camera) records a flat baseline and fails here.
"""

import json
import math
import re

RECORD = re.compile(
    r"\[HARNESS\] interaction phase=(\S+) surface=(\S+) camera=(\S+) selection=(\S+)"
)
PHASES = ("baseline", "after-click", "after-orbit", "after-pan")


def _records(lines):
    out = {}
    for line in lines:
        m = RECORD.search(line)
        if not m:
            continue
        phase, surface, camera, selection = m.groups()
        try:
            out[phase] = {
                "surface": surface,
                "camera": json.loads(camera),
                "selection": json.loads(selection),
            }
        except ValueError:
            continue
    return out


def _angle_between(q1, q2):
    dot = abs(sum(a * b for a, b in zip(q1, q2)))
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(2.0 * math.acos(dot))


def _distance(p1, p2):
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(p1, p2)))


def check(lines, report):
    recs = _records(lines)
    missing = [p for p in PHASES if p not in recs]
    if missing:
        report.add_error("interaction: missing phases %s", ", ".join(missing))
        return

    surfaces = {recs[p]["surface"] for p in PHASES}
    if len(surfaces) != 1:
        report.add_error("interaction: mixed surfaces in one run: %s", surfaces)

    if "Box" not in recs["after-click"]["selection"]:
        report.add_error(
            "interaction: click did not select the box (selection=%s)",
            recs["after-click"]["selection"],
        )

    base_rot = recs["baseline"]["camera"]["rot"]
    orbit_rot = recs["after-orbit"]["camera"]["rot"]
    rot_delta = _angle_between(base_rot, orbit_rot)
    if rot_delta < 1.0:
        report.add_error(
            "interaction: orbit drag did not change camera orientation "
            "(delta=%.3f deg)",
            rot_delta,
        )

    orbit_pos = recs["after-orbit"]["camera"]["pos"]
    pan_pos = recs["after-pan"]["camera"]["pos"]
    pan_delta = _distance(orbit_pos, pan_pos)
    if pan_delta < 1e-3:
        report.add_error(
            "interaction: pan drag did not change camera position (delta=%.6f)",
            pan_delta,
        )
