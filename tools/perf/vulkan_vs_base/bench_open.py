#!/usr/bin/env python3
"""Startup + cube-document open times across base GL / fork GL / fork Vulkan."""

from __future__ import annotations

import json
import os
import re
import statistics
import subprocess
import sys
import time

from _paths import BASE, FORK, here, out_dir

PROBE = here("open_bench_probe.py")
OUT = out_dir("results_open")
REPS = int(os.environ.get("OB_REPS", "5"))

CONFIGS = [
    # name, binary, backend, mode
    ("open_base_gl", BASE, "gl", ""),
    ("open_fork_gl", FORK, "gl", ""),
    ("open_fork_vk", FORK, "vulkan", "1"),
]

_KV = re.compile(r"(\w+)=(\S+)")


def parse(lines, prefix):
    return [{k: v for k, v in _KV.findall(ln[len(prefix):])}
            for ln in lines if ln.startswith(prefix)]


def run(cfg, timeout=300):
    name, binary, backend, mode = cfg
    env = dict(os.environ)
    env.update({
        "QT_STYLE_OVERRIDE": "fusion", "QT_QPA_PLATFORM": "xcb",
        "FC_SKIP_UNSAVED_PROMPT": "1", "PYTHONUNBUFFERED": "1",
        "FC_OPEN_REPS": str(REPS), "FC_BENCH_BACKEND": backend,
        "FC_OPEN_T0": "%.6f" % time.time(),
    })
    if mode:
        env["FC_BENCH_MODE"] = mode
        env["FC_BENCH_PRESENT"] = "2"
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
    gui = parse(lines, "[HARNESS] open_gui_ready ")
    cubes = parse(lines, "[HARNESS] open_cube ")
    done = parse(lines, "[HARNESS] open_done ")
    doc_ms = [float(c["doc_ms"]) for c in cubes]
    rec = {
        "name": name, "backend": backend, "status": status,
        "wall_s": round(time.time() - t0, 1),
        "gui_ready_ms": float(gui[0]["ms"]) if gui else None,
        "doc_ms": doc_ms,
        "doc_median_ms": statistics.median(doc_ms) if doc_ms else None,
        "open_total_ms": float(done[0]["total_ms"]) if done else None,
        "reps": len(cubes),
    }
    print(f"[{name}] {status} gui_ready={rec['gui_ready_ms']}ms "
          f"doc_median={rec['doc_median_ms']}ms "
          f"open_total={rec['open_total_ms']}ms", flush=True)
    return rec


def main():
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[1:] or None
    res = [run(c) for c in CONFIGS if not only or c[0] in only]
    json.dump(res, open(os.path.join(OUT, "open.json"), "w"), indent=2)
    print("wrote", os.path.join(OUT, "open.json"))


if __name__ == "__main__":
    main()
