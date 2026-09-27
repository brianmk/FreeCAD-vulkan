#!/usr/bin/env python3
"""Stamp each captured frame with the live fps + house count and rebuild the
houses GIFs with per-frame delays from the measured capture times."""

import glob
import os
import re
import subprocess

from _paths import HERE as PC
OUTDIR = "/home/phantom/dev/FreeCAD/docs/vulkan-perf"
STAMP = f"{PC}/stamped"
M1, M2 = 26, 58   # house-count phase boundaries (frame index, 1-based)


def times(name):
    ms = {}
    for ln in open(f"{PC}/{name}.rec.log", errors="replace"):
        m = re.search(r"rec_frame i=(\d+).*?ms=([\d.]+)", ln)
        if m:
            ms[int(m.group(1))] = float(m.group(2))
    return [ms[i] for i in sorted(ms)]


def houses(i):
    return 1 if i < M1 else (8 if i < M2 else 16)


def build(name, label):
    frames = sorted(glob.glob(f"{PC}/house_frames/{name}/f*.png"))
    ts = times(name)
    n = min(len(frames), len(ts))
    outdir = os.path.join(STAMP, name)
    os.makedirs(outdir, exist_ok=True)
    saved = []
    for k in range(n):
        fps = 1000.0 / ts[k] if ts[k] else 0.0
        txt = "%s   %d house%s   %d fps" % (
            label, houses(k + 1), "s" if houses(k + 1) > 1 else "", round(fps))
        dst = os.path.join(outdir, "s%04d.png" % k)
        subprocess.run([
            "magick", frames[k], "-resize", "640x",
            "-gravity", "south", "-font", "DejaVu-Sans-Bold", "-pointsize", "20",
            "-fill", "white", "-undercolor", "#000000B0",
            "-annotate", "+0+8", txt, dst], check=True)
        saved.append(dst)
    gif = os.path.join(OUTDIR, name + ".gif")
    cmd = ["magick"]
    for f, ms in zip(saved, ts[:n]):
        cmd += ["-delay", str(max(2, int(round(ms / 10.0)))), f]
    cmd += ["-loop", "0", "-colors", "128", "-layers", "Optimize", gif]
    subprocess.run(cmd, check=True)
    tot = sum(max(2, int(round(ms / 10.0))) * 10 for ms in ts[:n])
    print("%s: %d frames, play %.1fs, %.2f MB" % (
        name, n, tot / 1000.0, os.path.getsize(gif) / 1e6))


build("houses_base_gl", "base GL")
build("houses_vulkan", "fork Vulkan")
