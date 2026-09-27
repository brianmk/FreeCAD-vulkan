#!/usr/bin/env python3
"""Capture frame sequences and assemble the demo GIFs.

  rotate_base_gl -> docs/vulkan-perf/rotate_base_gl.gif  (upstream GL)
  rotate_vulkan  -> docs/vulkan-perf/rotate_vulkan.gif   (fork, Immediate)
  bim_pathmax    -> docs/vulkan-perf/bim_pathtracing_max.gif
"""

from __future__ import annotations

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

from _paths import HERE as PC, REPO, BASE, FORK

PROBE = f"{PC}/record_probe.py"
BIM = f"{REPO}/data/examples/BIMExample.FCStd"
OUTDIR = f"{REPO}/docs/vulkan-perf"
STAGE = f"{PC}/gif_frames"
VK_TMP = "/tmp"

_KV = re.compile(r"(\w+)=(\S+)")


def parse(lines, tag):
    out = []
    for ln in lines:
        if ln.startswith("[HARNESS] " + tag + " "):
            out.append({k: v for k, v in _KV.findall(ln[len("[HARNESS] " + tag):])})
    return out


def run(name, binary, backend, kind, frames, scene_n=300, deg=5, viewport="800x450",
        fpath="", zoom=0.0):
    os.makedirs(STAGE, exist_ok=True)
    out = os.path.join(STAGE, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    if backend == "vulkan":
        for f in glob.glob(f"{VK_TMP}/vk_frame_*.png"):
            os.remove(f)
    env = dict(os.environ)
    env.update({
        "QT_STYLE_OVERRIDE": "fusion", "QT_QPA_PLATFORM": "xcb",
        "FC_SKIP_UNSAVED_PROMPT": "1", "PYTHONUNBUFFERED": "1",
        "FC_REC_KIND": kind, "FC_REC_BACKEND": backend,
        "FC_REC_FRAMES": str(frames), "FC_REC_SCENE_N": str(scene_n),
        "FC_REC_DEG": str(deg), "FC_REC_VIEWPORT": viewport, "FC_REC_OUT": out,
        "FC_REC_ZOOM": str(zoom),
    })
    if fpath:
        env["FC_REC_FILE"] = fpath
    if backend == "vulkan":
        env["FC_VULKAN_DUMP_FRAME"] = "1"
        env["FC_VULKAN_DUMP_START"] = "1"
        env["FC_VULKAN_DUMP_END"] = "100000"
    log = f"{PC}/{name}.rec.log"
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        subprocess.run([binary, PROBE], env=env, stdout=lf, stderr=subprocess.STDOUT,
                       timeout=600)
    lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
    recs = parse(lines, "rec_frame")
    done = parse(lines, "rec_done")
    frames_ms = [float(r["ms"]) for r in recs]
    median = sorted(frames_ms)[len(frames_ms) // 2] if frames_ms else 40.0
    fps = 1000.0 / median if median else 25.0

    if backend == "vulkan":
        if kind == "bim":
            first = int(recs[0]["ordinal"]) if recs else 5
            files = sorted(glob.glob(f"{VK_TMP}/vk_frame_*.png"),
                           key=lambda p: int(re.search(r"(\d+)\.png$", p).group(1)))
            files = [f for f in files
                     if int(re.search(r"(\d+)\.png$", f).group(1)) >= first]
        else:
            files = []
            for r in recs:
                p = f"{VK_TMP}/vk_frame_{r['ordinal']}.png"
                if os.path.isfile(p):
                    files.append(p)
        # copy into the stage dir with ordered names
        for i, src in enumerate(files):
            shutil.copy(src, os.path.join(out, "f%04d.png" % i))
    else:
        files = sorted(glob.glob(os.path.join(out, "f*.png")))
    print(f"[{name}] captured={len(recs)} frames={len(files)} "
          f"median={median:.1f}ms fps={fps:.1f} done={bool(done)}", flush=True)
    return {"name": name, "frames": files, "median_ms": median, "fps": fps,
            "out": out, "count": len(files)}


def make_gif(name, files, delay_cs, width=640):
    out = os.path.join(OUTDIR, name + ".gif")
    if not files:
        print("no frames for", name)
        return None
    cmd = ["magick", "-delay", str(delay_cs), "-loop", "0"] + files + [
        "-resize", f"{width}x", "-colors", "160", "-layers", "Optimize", out]
    subprocess.run(cmd, check=True)
    print("wrote", out, "%.1f MB" % (os.path.getsize(out) / 1e6))
    return out


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    which = sys.argv[1:] or ["rotate", "bim"]
    results = {}

    if "rotate" in which:
        r1 = run("rotate_base_gl", BASE, "gl", "rotate", frames=72, scene_n=300,
                 deg=5, viewport="1280x720", fpath=BIM)
        make_gif("rotate_base_gl", r1["frames"],
                 max(4, round(r1["median_ms"] / 10)), width=560)

        r2 = run("rotate_vulkan", FORK, "vulkan", "rotate", frames=72, scene_n=300,
                 deg=5, viewport="1280x720", fpath=BIM)
        make_gif("rotate_vulkan", r2["frames"],
                 max(2, round(r2["median_ms"] / 10)), width=560)
        results["rotate"] = [r1, r2]

    if "bim" in which:
        r3 = run("bim_pathmax", FORK, "vulkan", "bim", frames=95, viewport="1280x720",
                 fpath=BIM, zoom=0.62)
        # subsample to <= 90 frames, keep order (raster -> converged PT)
        files = r3["frames"]
        files = files[4:]  # drop pre-fit warm-up frames
        if len(files) > 90:
            step = len(files) / 90.0
            files = [files[int(i * step)] for i in range(90)]
        make_gif("bim_pathtracing_max", files, 5, width=640)
        results["bim"] = [r3]

    json.dump(results, open(f"{PC}/gif_results.json", "w"), indent=2)


if __name__ == "__main__":
    main()
