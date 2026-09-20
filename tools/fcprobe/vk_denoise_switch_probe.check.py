#!/usr/bin/env python3
"""Host-side assertions for vk_denoise_switch_probe.py (runtime denoiser switch).

Correlates the renderer's FC_VULKAN_PT_DENOISE_TIMING [DENOISE-STATE] lines and
the FC_VULKAN_PT_DENOISER_DEBUG [DENOISE] breadcrumbs with the probe's
[HARNESS] frame_phase markers by presented-frame ordinal:

  [DENOISE] backend configured kind=<n> active=<yes/no>
  [DENOISE-STATE] ord=<n> frame=<n> accum=<0/1> pend=<0/1> ready=<0/1>
                  denoise=<0/1> kind=<n>
  [HARNESS] frame_phase phase=<name> frame=<ord>

The regression guarded (coin PR #41 / FreeCAD PR #66): a runtime denoiser
switch must
  * rebuild the denoise backend for the new kind -- without the fix
    createPathTracingBuffers() early-returned on a same-resolution call and
    never emitted "[DENOISE] backend configured kind=<new>", and
  * invalidate the presented result -- without the fix setDenoiserFilter()
    left denoiseResultReady=1, so the *old* denoiser's buffer (black after the
    switch) kept being presented, with no ready 1->0 edge.

Requires FC_VULKAN_PT_DENOISER_DEBUG=1 (breadcrumbs) and
FC_VULKAN_PT_DENOISE_TIMING=1 ([DENOISE-STATE]).
"""

import re

PHASE = re.compile(r"\[HARNESS\] frame_phase phase=(\w+) frame=(\d+)")
STATE = re.compile(
    r"\[DENOISE-STATE\] ord=(\d+) frame=(\d+) accum=(\d) pend=(\d) "
    r"ready=(\d) denoise=(\d) kind=(\d)")
CONFIGURED = re.compile(r"\[DENOISE\] backend configured kind=(\d+) active=(\w+)")

# Denoiser kind enum: 0=RTX, 1=OIDN, 2=FSR, 3=None.
OIDN = 1
RTX = 0


def _states(lines):
    rows = []
    for line in lines:
        m = STATE.search(line)
        if m:
            rows.append(dict(ord=int(m.group(1)), frame=int(m.group(2)),
                             accum=int(m.group(3)), pend=int(m.group(4)),
                             ready=int(m.group(5)), denoise=int(m.group(6)),
                             kind=int(m.group(7))))
    return rows


def _phases(lines):
    out = {}
    for line in lines:
        m = PHASE.search(line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def _configured(lines):
    """Count of '[DENOISE] backend configured kind=<n>' per kind."""
    out = {}
    for line in lines:
        m = CONFIGURED.search(line)
        if m:
            out[int(m.group(1))] = out.get(int(m.group(1)), 0) + 1
    return out


def _fresh_publish(seg):
    """First ready=1 that follows a ready=0 within seg (a 0->1 edge)."""
    seen_zero = False
    for r in seg:
        if not seen_zero:
            if r["ready"] == 0:
                seen_zero = True
            continue
        if r["ready"] == 1:
            return r
    return None


def check(lines, report):
    rows = _states(lines)
    if not rows:
        report.add_error("no [DENOISE-STATE] lines seen (FC_VULKAN_PT_DENOISE_TIMING "
                         "unset or the RT viewport never presented)")
        return

    phases = _phases(lines)
    required = ("oidn_settled", "switch_rtx", "rtx_settled",
                "switch_oidn", "oidn_settled2")
    missing = [p for p in required if p not in phases]
    if missing:
        report.add_error("missing frame_phase markers: %s (probe did not "
                         "complete its switch sequence)", ", ".join(missing))
        return

    sw_rtx = phases["switch_rtx"]
    sw_oidn = phases["switch_oidn"]
    pre = [r for r in rows if r["ord"] <= sw_rtx]
    rtx = [r for r in rows if sw_rtx < r["ord"] <= sw_oidn]
    oidn2 = [r for r in rows if r["ord"] > sw_oidn]

    if not any(r["ready"] == 1 and r["kind"] == OIDN for r in pre):
        report.add_error("baseline OIDN denoise never published before the "
                         "switch (no ready=1 kind=1 state) -> denoiser not "
                         "functional, cannot test the switch")
        return

    conf = _configured(lines)
    if conf.get(RTX, 0) == 0:
        report.add_error("denoiser switch BROKEN: no '[DENOISE] backend "
                         "configured kind=0' after switching to RTX -- "
                         "createPathTracingBuffers did not rebuild the denoise "
                         "backend (or RTX is unavailable on this device)")
    if conf.get(OIDN, 0) < 2:
        report.add_error("denoiser switch BROKEN: OIDN backend configured %d "
                         "time(s), expected >= 2 (initial + after switching "
                         "back)" % conf.get(OIDN, 0))

    if not any(r["kind"] == RTX for r in rtx):
        report.add_error("denoiser switch BROKEN: the presented kind never "
                         "became 0 (RTX) after the switch (rtx segment "
                         "states: %d)" % len(rtx))
    else:
        edge = _fresh_publish(rtx)
        if edge is None:
            report.add_error("denoiser switch BROKEN: no ready 1->0->1 "
                             "sequence in the RTX segment -- the presented "
                             "result was not invalidated (stale buffer "
                             "survives the switch)")
        elif edge["kind"] != RTX:
            report.add_error("denoiser switch BROKEN: RTX segment published "
                             "kind=%d, expected %d" % (edge["kind"], RTX))

    if not any(r["kind"] == OIDN and r["ready"] == 1 for r in oidn2):
        report.add_error("denoiser switch BROKEN: OIDN did not publish again "
                         "after switching back (no ready=1 kind=1 state in the "
                         "oidn2 segment)")

    report.log_event("denoise-switch", "ok",
                     switch_rtx_ord=sw_rtx, switch_oidn_ord=sw_oidn,
                     pre_states=len(pre), rtx_states=len(rtx),
                     oidn2_states=len(oidn2),
                     configured_rtx=conf.get(RTX, 0),
                     configured_oidn=conf.get(OIDN, 0))
