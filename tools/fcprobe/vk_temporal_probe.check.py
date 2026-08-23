#!/usr/bin/env python3
"""Host-side assertions for vk_temporal_probe.py (Phase 2: temporal
reprojection).

Branches on the run's env:
  - temporal ON  (default): camera-only moves keep accumulating with
    reprojection (no preview drop), history is accepted per-pixel on the
    small move, rejected for most pixels on the 90-degree move, convergence
    resumes after the move, and a scene edit drops to preview WITHOUT
    reprojecting.
  - FC_VULKAN_PT_TEMPORAL=0: no reprojection frame may appear anywhere.
"""

import re

STATE_LINE = re.compile(
    r"\[RTDBG\] ptState viewChanged=(\d) sceneChanged=(\d) accum=(\d) "
    r"frameIndex=(\d+) idle=(\d+) reproject=(\d)")
ADAPT_LINE = re.compile(
    r"\[RTDBG\] adaptive active=(\d+)/(\d+) fraction=([0-9.]+) "
    r"frameIndex=(\d+) accum=(\d) .*reprojected=(\d+)")
PHASE_LINE = re.compile(r"TEMPORAL phase=(\S+)")


def _windowed(lines):
    """List of (phase, kind, match) in real-time log order."""
    events = []
    phase = "boot"
    for line in lines:
        m = PHASE_LINE.search(line)
        if m:
            phase = m.group(1)
            continue
        m = STATE_LINE.search(line)
        if m:
            events.append((phase, "state", m))
            continue
        m = ADAPT_LINE.search(line)
        if m:
            events.append((phase, "adaptive", m))
    return events


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    env = report.session.get("env_overrides", {})
    temporal_off = env.get("FC_VULKAN_PT_TEMPORAL") == "0"

    events = _windowed(lines)
    states = [(p, m) for p, k, m in events if k == "state"]
    adaptives = [(p, m) for p, k, m in events if k == "adaptive"]

    reprojects = [m for _, m in states if m.group(6) == "1"]

    if temporal_off:
        if reprojects:
            err(f"temporal OFF: {len(reprojects)} reprojection frames "
                "appeared")
        accepted = [m for _, m in adaptives if int(m.group(6)) > 0]
        if accepted:
            err(f"temporal OFF: {len(accepted)} frames accepted history")
        return

    # ON: camera-only moves must keep accumulating with reprojection.
    small_states = [m for p, m in states if p == "move-small"]
    if not small_states:
        err("no frames observed in the move-small window (wake-up failed?)")
        return
    small_reproject = [m for m in small_states if m.group(6) == "1"]
    small_drops = [m for m in small_states
                   if m.group(1) == "1" and m.group(2) == "0" and
                   m.group(3) == "0"]
    if not small_reproject:
        err("move-small: no reproject=1 frame (camera move did not "
            "reproject)")
    if small_drops:
        err(f"move-small: {len(small_drops)} preview drops on camera-only "
            "moves (accumulation was discarded instead of reprojected)")

    total = max((int(m.group(2)) for _, m in adaptives), default=0)
    small_accepted = [int(m.group(6))
                      for p, m in adaptives if p == "move-small"]
    if total and small_accepted and max(small_accepted) < total * 0.01:
        err(f"move-small: at most {max(small_accepted)}/{total} pixels "
            "accepted history (expected a meaningful subset)")

    big_accepted = [int(m.group(6))
                    for p, m in adaptives if p == "move-big"]
    if total and big_accepted and max(big_accepted) > total * 0.9:
        err(f"move-big: {max(big_accepted)}/{total} pixels accepted "
            "history after a 90-degree orbit (disocclusion not detected)")

    # After the small move, convergence resumes (fraction < 1.0).
    resumed = False
    seen_move = False
    for phase, kind, m in events:
        if phase == "move-small":
            seen_move = True
        if seen_move and phase == "move-big":
            break
        if seen_move and kind == "adaptive" and float(m.group(3)) < 1.0:
            resumed = True
    if not resumed:
        err("fraction never declined below 1.0 after the camera move "
            "(carried history not reused)")

    # Scene edit: must drop to preview WITHOUT reprojecting.
    edit_states = [m for p, m in states if p == "edit-box"]
    if not edit_states:
        err("edit-box: no frames observed (scene change did not wake the "
            "converged viewport)")
    else:
        scene_drop = [m for m in edit_states
                      if m.group(2) == "1" and m.group(3) == "0" and
                      m.group(6) == "0"]
        edit_reproject = [m for m in edit_states
                          if m.group(2) == "1" and m.group(6) == "1"]
        if not scene_drop:
            err("edit-box: no sceneChanged preview drop (accum=0, "
                "reproject=0) observed")
        if edit_reproject:
            err(f"edit-box: {len(edit_reproject)} scene-change frames "
                "reprojected (history must not be reused across scene "
                "edits)")
