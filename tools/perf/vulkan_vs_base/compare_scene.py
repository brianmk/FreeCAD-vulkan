#!/usr/bin/env python3
"""Drive scene_bench_probe_io.py under the baseline and fork binaries and
collect the [HARNESS] scene_* records into one JSON + a printed table."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time

from _paths import BASE, FORK, here, out_dir

PROBE = here("scene_bench_probe_io.py")
OUT = out_dir("results")

FRAMES = int(os.environ.get("PC_FRAMES", "40"))
WARMUP = int(os.environ.get("PC_WARMUP", "8"))
VIEWPORT = os.environ.get("PC_VIEWPORT", "1600x900")

CONFIGS = [
    # name, binary, backend, kind, count
    ("base_gl_empty", BASE, "gl", "empty", 0),
    ("fork_gl_empty", FORK, "gl", "empty", 0),
    ("fork_vk_empty", FORK, "vulkan", "empty", 0),
    ("base_gl_objects1000", BASE, "gl", "objects", 1000),
    ("fork_gl_objects1000", FORK, "gl", "objects", 1000),
    ("fork_vk_objects1000", FORK, "vulkan", "objects", 1000),
    ("base_gl_sketches200", BASE, "gl", "sketches", 200),
    ("fork_gl_sketches200", FORK, "gl", "sketches", 200),
    ("fork_vk_sketches200", FORK, "vulkan", "sketches", 200),
    ("base_gl_objects5000", BASE, "gl", "objects", 5000),
    ("fork_gl_objects5000", FORK, "gl", "objects", 5000),
    ("fork_vk_objects5000", FORK, "vulkan", "objects", 5000),
    ("base_gl_objects15000", BASE, "gl", "objects", 15000),
    ("fork_gl_objects15000", FORK, "gl", "objects", 15000),
    ("fork_vk_objects15000", FORK, "vulkan", "objects", 15000),
]

_KV = re.compile(r"(\w+)=(\S+)")


def parse(lines, prefix):
    out = []
    for ln in lines:
        if ln.startswith(prefix):
            out.append({k: v for k, v in _KV.findall(ln[len(prefix):])})
    return out


def run(cfg, timeout=300):
    name, binary, backend, kind, count = cfg
    env = dict(os.environ)
    env.update({
        "QT_STYLE_OVERRIDE": "fusion",
        "QT_QPA_PLATFORM": "wayland",
        "FC_SKIP_UNSAVED_PROMPT": "1",
        "PYTHONUNBUFFERED": "1",
        "FC_BENCH_KIND": kind,
        "FC_BENCH_COUNT": str(count),
        "FC_BENCH_FRAMES": str(FRAMES),
        "FC_BENCH_WARMUP": str(WARMUP),
        "FC_BENCH_VIEWPORT": VIEWPORT,
        "FC_BENCH_BACKEND": backend,
        "FC_BENCH_SOAK": "0",
        "FC_BENCH_TEARDOWN": "1",
    })
    log = os.path.join(OUT, f"{name}.log")
    t0 = time.time()
    status = "ok"
    try:
        with open(log, "w", encoding="utf-8", errors="replace") as lf:
            subprocess.run([binary, PROBE], env=env, stdout=lf,
                           stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        status = "timeout"
    lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
    bench = parse(lines, "[HARNESS] scene_bench ")
    build = parse(lines, "[HARNESS] scene_build ")
    done = parse(lines, "[HARNESS] scene_done ")
    rec = {
        "name": name, "backend": backend, "kind": kind, "count": count,
        "status": status, "wall_s": round(time.time() - t0, 1),
        "build": build[-1] if build else None,
        "bench": bench[-1] if bench else None,
        "done": done[-1] if done else None,
        "traceback": any("Traceback" in l or "Uncaught" in l for l in lines),
    }
    print(f"[{name}] status={status} wall={rec['wall_s']}s "
          f"build_ms={rec['build']['build_ms'] if rec['build'] else '?'} "
          f"frame_median_ms={rec['bench']['frame_median_ms'] if rec['bench'] else '?'} "
          f"fps={rec['bench']['fps'] if rec['bench'] else '?'}", flush=True)
    return rec


def main():
    os.makedirs(OUT, exist_ok=True)
    results = []
    only = sys.argv[1:] or None
    for cfg in CONFIGS:
        if only and cfg[0] not in only:
            continue
        results.append(run(cfg))
    path = os.path.join(OUT, "scene_results.json")
    json.dump(results, open(path, "w"), indent=2)
    print("wrote", path)


if __name__ == "__main__":
    main()
