#!/usr/bin/env python3
"""Host-side assertions for vk_denoiser_probe.py (all path-tracing denoisers).

The probe selects one denoiser via FC_DENOISER.  This check reads that back out
of the run's env overrides and asserts the REQUESTED backend actually came up
and ran, rather than silently degrading to a different one:

  rtx   kind=2 -- OptiX/CUDA interop      [DENOISE] backend configured kind=2 active=yes
                                            [DENOISE] kind=2 frame denoise took
  oidn  kind=1 -- CPU (async worker)      [DENOISE] backend configured kind=1 active=yes
                                            [DENOISE] OIDN async worker published
  fsr   kind=3 -- AMD FFX DNSR            [DENOISE] backend configured kind=3 active=yes
                                            [DENOISE] kind=3 FSR prefilter took
  none  kind=0 -- denoiser disabled       NO "[DENOISE] backend configured" line at all

Each backend's degradation messages must be absent, and the dumped frame must be
non-black (a filter that dispatches but publishes a black/garbage image is
caught here, not by the breadcrumbs alone).

Breadcrumbs need FC_VULKAN_PT_DENOISER_DEBUG=1 + FC_VULKAN_PT_DENOISE_TIMING=1;
the pixel check needs the Debug-only FC_VULKAN_DUMP_FRAME hook and is skipped
(with a log) when no frames are dumped.
"""

import glob
import os

# Combo index / backend-name mapping, mirrors DlgSettings3DView.ui.
KINDS = {"rtx": 2, "oidn": 1, "fsr": 3, "none": 0}

POSITIVE = {
    "rtx": [
        "[DENOISE] backend configured kind=2 active=yes",
        "[DENOISE] kind=2 frame denoise took",
    ],
    "oidn": [
        "[DENOISE] backend configured kind=1 active=yes",
        "[DENOISE] OIDN async worker published",
    ],
    "fsr": [
        "[DENOISE] backend configured kind=3 active=yes",
        "[DENOISE] kind=3 FSR prefilter took",
    ],
    "none": [],
}

# Messages that mean the requested backend did NOT run.
DEGRADE = {
    "rtx": [
        "RTX denoiser unavailable",
        "is not NVIDIA",
        "degrading to OIDN",
    ],
    "oidn": [
        "OIDN denoiser unavailable",
        "OIDN device null",
        "no backend configured for kind=1",
        "degrading to OIDN",
    ],
    "fsr": [
        "AMD FSR denoiser is not built in",
        "FSR denoiser unavailable",
        "degrading to OIDN",
    ],
    "none": [
        "[DENOISE] backend configured",
        "[DENOISE] OIDN async worker published",
        "[DENOISE] kind=2 frame denoise took",
        "[DENOISE] kind=3 FSR prefilter took",
    ],
}

# A real frame is mostly lit geometry/background, not a near-black image; allow
# a generous floor (>= 2% of pixels non-black) so a broken filter that publishes
# black fails while a dark-but-valid scene still passes.
MIN_NONBLACK_FRACTION = 0.02


def _expected_name(report, lines):
    env = getattr(report, "session", {}).get("env_overrides", {}) or {}
    name = (env.get("FC_DENOISER") or "").strip().lower()
    if name not in KINDS:
        # Fall back to the backend env the harness also passes.
        name = (env.get("FC_VULKAN_PT_DENOISER") or "").strip().lower()
    return name if name in KINDS else None


def _best_nonblack(artifact_dir):
    """Return (path, nonblack, total) for the most-lit dumped frame."""
    frames_dir = os.path.join(artifact_dir, "frames")
    best = None
    for path in sorted(glob.glob(os.path.join(frames_dir, "*.png"))):
        try:
            from PIL import Image
            im = Image.open(path).convert("RGB")
            px = list(im.getdata())
            nb = sum(1 for c in px if c != (0, 0, 0))
            if best is None or nb > best[1]:
                best = (path, nb, len(px))
        except Exception:
            continue
    return best


def check(lines, report):
    blob = "\n".join(lines)
    name = _expected_name(report, lines)
    if name is None:
        report.add_error(
            "cannot determine the requested denoiser: pass --env FC_DENOISER="
            "rtx|oidn|fsr|none")
        return

    kind = KINDS[name]

    for msg in DEGRADE[name]:
        if msg in blob:
            report.add_error(
                "denoiser %r degraded instead of running: %r" % (name, msg))

    for msg in POSITIVE[name]:
        if msg not in blob:
            report.add_error(
                "denoiser %r did not report %r (expected kind=%d) -- the "
                "backend did not come up (run with FC_VULKAN_PT_DENOISER_DEBUG=1 "
                "and FC_VULKAN_PT_DENOISE_TIMING=1)" % (name, msg, kind))

    if name != "none" and ("[DENOISE] resolved kind=%d" % kind) not in blob:
        report.add_error(
            "denoiser %r: no '[DENOISE] resolved kind=%d' line -- the pref was "
            "not applied" % (name, kind))

    # Pixel-level sanity: the presented frame must not be black.  The frame
    # dumper is a Debug-only hook, so skip with a log when no frames exist.
    artifact_dir = getattr(report, "artifact_dir", None)
    best = _best_nonblack(artifact_dir) if artifact_dir else None
    if best is None:
        report.log_event("denoiser-" + name, "pixels-skipped",
                         reason="no frame dumps (FC_VULKAN_DUMP_FRAME is a "
                                "Debug-only hook and may be unavailable)")
    else:
        path, nb, total = best
        frac = nb / float(total) if total else 0.0
        if frac < MIN_NONBLACK_FRACTION:
            report.add_error(
                "denoiser %r output frame is near-black: %s has only %.1f%% "
                "non-black pixels (>= %.0f%% expected) -- the denoised image is "
                "likely black/garbage" % (name, os.path.basename(path),
                                          frac * 100.0,
                                          MIN_NONBLACK_FRACTION * 100.0))
        else:
            report.log_event("denoiser-" + name, "pixels",
                             frame=os.path.basename(path), nonblack=nb,
                             total=total, fraction=round(frac, 4))

    if not report.errors:
        report.log_event("denoiser-" + name, "ok", denoise_kind=kind)
