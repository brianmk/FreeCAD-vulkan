#!/usr/bin/env python3
"""Extended benchmarks: vorontest, rotating-camera, and path-tracing modes.

Uses the patched scene_bench_probe_io.py with FC_BENCH_MODE / FC_BENCH_MOTION.
All runs on xcb with VulkanPresentMode=2 so the Vulkan numbers are uncapped.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time

from _paths import BASE, FORK, VORON, here, out_dir

PROBE = here("scene_bench_probe_io.py")
OUT = out_dir("results_extra")

# name, binary, backend, kind, count, mode, motion, frames, warmup, file
CONFIGS = [
    # --- vorontest (single huge Part::Feature) ---
    ("voron_base_gl", BASE, "gl", "vorontest", 1, "0", "static", 15, 3, VORON),
    ("voron_fork_gl", FORK, "gl", "vorontest", 1, "0", "static", 15, 3, VORON),
    ("voron_fork_vk", FORK, "vulkan", "vorontest", 1, "1", "static", 30, 5, VORON),
    ("voron_fork_vk_pt", FORK, "vulkan", "vorontest", 1, "4", "orbit", 20, 3, VORON),
    # --- rotating camera on a 1000-box grid ---
    ("rot_base_gl", BASE, "gl", "objects", 1000, "0", "orbit", 40, 8, ""),
    ("rot_fork_gl", FORK, "gl", "objects", 1000, "0", "orbit", 40, 8, ""),
    ("rot_fork_vk", FORK, "vulkan", "objects", 1000, "1", "orbit", 40, 8, ""),
    ("rot_fork_vk_rt", FORK, "vulkan", "objects", 1000, "3", "orbit", 40, 8, ""),
    ("rot_fork_vk_pt", FORK, "vulkan", "objects", 1000, "4", "orbit", 40, 8, ""),
    ("rot_fork_vk_env", FORK, "vulkan", "objects", 1000, "5", "orbit", 40, 8, ""),
    # --- path tracing, static accumulation, smaller grid ---
    ("pt_fork_vk_static", FORK, "vulkan", "objects", 250, "4", "static", 40, 8, ""),
]

_KV = re.compile(r"(\w+)=(\S+)")


def parse(lines, prefix):
    return [{k: v for k, v in _KV.findall(ln[len(prefix):])}
            for ln in lines if ln.startswith(prefix)]


def run(cfg, timeout=900):
    name, binary, backend, kind, count, mode, motion, frames, warmup, fpath = cfg
    env = dict(os.environ)
    env.update({
        "QT_STYLE_OVERRIDE": "fusion", "QT_QPA_PLATFORM": "xcb",
        "FC_SKIP_UNSAVED_PROMPT": "1", "PYTHONUNBUFFERED": "1",
        "FC_BENCH_KIND": kind, "FC_BENCH_COUNT": str(count),
        "FC_BENCH_FRAMES": str(frames), "FC_BENCH_WARMUP": str(warmup),
        "FC_BENCH_VIEWPORT": "1600x900", "FC_BENCH_BACKEND": backend,
        "FC_BENCH_MODE": mode, "FC_BENCH_MOTION": motion,
        "FC_BENCH_PRESENT": "2", "FC_BENCH_SOAK": "0", "FC_BENCH_TEARDOWN": "1",
        "FC_VULKAN_PT_MAXSAMPLES": "4",
    })
    if fpath:
        env["FC_BENCH_FILE"] = fpath
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
    rec = {"name": name, "backend": backend, "kind": kind, "count": count,
           "mode": mode, "motion": motion, "status": status,
           "wall_s": round(time.time() - t0, 1),
           "build": build[-1] if build else None,
           "bench": bench[-1] if bench else None}
    b = rec["bench"] or {}
    print(f"[{name}] {status} wall={rec['wall_s']}s build_ms={b.get('build_ms','?')} "
          f"frame_ms={b.get('frame_median_ms','?')} fps={b.get('fps','?')}",
          flush=True)
    return rec


def main():
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[1:] or None
    res = []
    for cfg in CONFIGS:
        if only and cfg[0] not in only:
            continue
        res.append(run(cfg))
    json.dump(res, open(os.path.join(OUT, "extra.json"), "w"), indent=2)
    print("wrote", os.path.join(OUT, "extra.json"))


if __name__ == "__main__":
    main()
