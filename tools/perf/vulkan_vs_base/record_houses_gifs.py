#!/usr/bin/env python3
"""Capture the house fly-around (1 -> 8 houses) for base GL and fork Vulkan and
assemble the replacement GIFs."""

from __future__ import annotations

import glob
import json
import os
import re
import shutil
import subprocess

from _paths import HERE as PC, REPO, BASE, FORK

PROBE = f"{PC}/record_houses_probe.py"
OUTDIR = f"{REPO}/docs/vulkan-perf"
STAGE = f"{PC}/house_frames"
N = 84
M = 26
M2 = 58
T = 10
VP = "1280x720"
BIM = "/home/phantom/dev/FreeCAD/data/examples/BIMExample.FCStd"

_KV = re.compile(r"(\w+)=(\S+)")


def parse(lines, tag):
    return [{k: v for k, v in _KV.findall(ln[len("[HARNESS] " + tag):])}
            for ln in lines if ln.startswith("[HARNESS] " + tag + " ")]


def run(name, binary, backend):
    out = os.path.join(STAGE, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    if backend == "vulkan":
        for f in glob.glob("/tmp/vk_frame_*.png"):
            os.remove(f)
    env = dict(os.environ)
    env.update({
        "QT_STYLE_OVERRIDE": "fusion", "QT_QPA_PLATFORM": "xcb",
        "FC_SKIP_UNSAVED_PROMPT": "1", "PYTHONUNBUFFERED": "1",
        "FC_REC_BACKEND": backend, "FC_REC_FRAMES": str(N),
        "FC_REC_CLONE_AT": str(M), "FC_REC_CLONE2_AT": str(M2),
        "FC_REC_TRANSITION": str(T),
        "FC_REC_VIEWPORT": VP, "FC_REC_OUT": out, "FC_REC_FILE": BIM,
    })
    if backend == "vulkan":
        env.update({"FC_VULKAN_DUMP_FRAME": "1", "FC_VULKAN_DUMP_START": "1",
                    "FC_VULKAN_DUMP_END": "100000"})
    log = f"{PC}/{name}.rec.log"
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        subprocess.run([binary, PROBE], env=env, stdout=lf, stderr=subprocess.STDOUT,
                       timeout=900)
    lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
    recs = parse(lines, "rec_frame")
    med = sorted(float(r["ms"]) for r in recs)[len(recs) // 2] if recs else 50.0
    # per-frame times in capture order (ms)
    times = [float(r["ms"]) for r in recs]
    if backend == "vulkan":
        files = []
        tsel = []
        for r in recs:
            p = f"/tmp/vk_frame_{r['ordinal']}.png"
            if os.path.isfile(p):
                files.append(p)
                tsel.append(float(r["ms"]))
        for i, src in enumerate(files):
            shutil.copy(src, os.path.join(out, "f%04d.png" % i))
        times = tsel
    else:
        files = sorted(glob.glob(os.path.join(out, "f*.png")))
    print(f"[{name}] rec={len(recs)} frames={len(files)} median={med:.1f}ms", flush=True)
    return files, times


def gif(name, files, times, width=640):
    """Single approximate delay per GIF from the mean captured frame time."""
    out = os.path.join(OUTDIR, name + ".gif")
    mean_ms = sum(times) / len(times)
    delay_cs = max(2, int(round(mean_ms / 10.0)))
    subprocess.run(["magick", "-delay", str(delay_cs), "-loop", "0"] + files
                   + ["-resize", f"{width}x", "-colors", "128", "-layers",
                      "Optimize", out], check=True)
    print("wrote", out, "%.2f MB" % (os.path.getsize(out) / 1e6),
          "delay=%dcs (~%.1f fps, mean %.1f ms)" % (delay_cs, 100.0 / delay_cs, mean_ms))


def main():
    os.makedirs(STAGE, exist_ok=True)
    bf, bt = run("houses_base_gl", BASE, "gl")
    gif("houses_base_gl", bf, bt)
    vf, vt = run("houses_vulkan", FORK, "vulkan")
    gif("houses_vulkan", vf, vt)
    json.dump({"base_mean_ms": sum(bt) / len(bt), "vulkan_mean_ms": sum(vt) / len(vt)},
              open(f"{PC}/house_gif.json", "w"), indent=2)


if __name__ == "__main__":
    main()
