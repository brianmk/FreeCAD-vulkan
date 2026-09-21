#!/usr/bin/env python3
"""Host-side assertions for vk_fsr_probe.py (AMD FidelityFX DNSR denoiser).

Requires positive evidence that the fsr denoiser actually came up and ran,
rather than silently degrading to OIDN:

  [DENOISE] FSR (DNSR prefilter+temporal) pipeline ready      (FC_VULKAN_PT_DENOISER_DEBUG=1)
  [DENOISE] kind=3 FSR prefilter dispatched (...)    (FC_VULKAN_PT_DENOISE_TIMING=1)

and forbids the degradation messages:

  AMD FSR denoiser is not built in; falling back to OIDN
  FSR denoiser unavailable; degrading to OIDN

Finally it asserts, at the pixel level, that a rendered frame is actually
non-black: a broken FSR resolve (e.g. reading an uninitialized history buffer)
could dispatch fine yet publish a black/garbage image, which the breadcrumbs
alone would not catch.  This needs frame dumps (FC_VULKAN_DUMP_FRAME, a
Debug-only build hook, enabled by the fsr-denoise suite case); when the build
dumps nothing the check is skipped with a log instead of failing.
"""

import glob
import os

READY = "[DENOISE] FSR (DNSR prefilter+temporal) pipeline ready"
DISPATCH = "[DENOISE] kind=3 FSR prefilter"
DEGRADE = [
    "AMD FSR denoiser is not built in",
    "FSR denoiser unavailable",
]
# A real frame is mostly lit geometry/background, not a near-black image; allow
# a generous floor (>= 2% of pixels non-black) so an all-black FSR output fails
# while a dark-but-valid scene still passes.
MIN_NONBLACK_FRACTION = 0.02


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

    for msg in DEGRADE:
        if msg in blob:
            report.add_error("FSR degraded instead of running: %r" % msg)

    ready = READY in blob
    dispatch = DISPATCH in blob
    if not ready:
        report.add_error(
            "no FSR pipeline-ready breadcrumb (%r) -- backend did not build "
            "(run with FC_VULKAN_PT_DENOISER_DEBUG=1)" % READY)
    if not dispatch:
        report.add_error(
            "no FSR dispatch breadcrumb (%r) -- the pass never ran "
            "(run with FC_VULKAN_PT_DENOISE_TIMING=1)" % DISPATCH)

    # Pixel-level sanity: the FSR output must not be a black frame.  The frame
    # dumper is a Debug-only build hook (FC_VULKAN_DUMP_FRAME); a Release/CI
    # build dumps nothing, so when no frames are available skip the check with a
    # log rather than failing the run.
    artifact_dir = getattr(report, "artifact_dir", None)
    best = _best_nonblack(artifact_dir) if artifact_dir else None
    if best is None:
        report.log_event("fsr-denoise", "pixels-skipped",
                         reason="no frame dumps (FC_VULKAN_DUMP_FRAME is a "
                                "Debug-only hook and may be unavailable)")
    else:
        path, nb, total = best
        frac = nb / float(total) if total else 0.0
        if frac < MIN_NONBLACK_FRACTION:
            report.add_error(
                "FSR output frame is near-black: %s has only %.1f%% non-black "
                "pixels (>= %.0f%% expected) -- the denoised image is likely "
                "black/garbage" % (os.path.basename(path), frac * 100.0,
                                   MIN_NONBLACK_FRACTION * 100.0))
        else:
            report.log_event("fsr-denoise", "pixels", frame=os.path.basename(path),
                             nonblack=nb, total=total, fraction=round(frac, 4))

    if ready and dispatch and not any(m in blob for m in DEGRADE):
        report.log_event("fsr-denoise", "ok", ready=ready, dispatch=dispatch)
