#!/usr/bin/env python3
"""Three-arm performance suite: upstream `base` vs fork `gl` vs fork `vk`.

Runs the same GUI probe (tests/perf/probe.py) against all three arms so the
numbers are apples-to-apples, plus the fork-only C++ benchmarks
(PartPickBench / ImportBench) on the fork arms, then renders a comparison graph.

Usage:
    python3 tests/perf/run_suite.py --quick
    python3 tests/perf/run_suite.py --samples 40 --arms base,gl,vk
    python3 tests/perf/run_suite.py --works step:open,box:orbit,box:pick

See tests/perf/README.md.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "probe.py")
REPO = "/home/phantom/dev/FreeCAD-Integrate/FreeCAD-vulkan"

ARMS = {
    "base": {
        "bin": "/home/phantom/dev/FreeCAD_base/FreeCAD/build/release/bin/FreeCAD",
        "bindir": "/home/phantom/dev/FreeCAD_base/FreeCAD/build/release",
        "ld": True,
    },
    "gl": {
        "bin": "/home/phantom/dev/FreeCAD-Integrate/build-freecad-off/bin/FreeCAD",
        "build": "/home/phantom/dev/FreeCAD-Integrate/build-freecad-off",
        "ld": False,
    },
    "vk": {
        "bin": "/home/phantom/dev/FreeCAD-Integrate/build-freecad/bin/FreeCAD",
        "build": "/home/phantom/dev/FreeCAD-Integrate/build-freecad",
        "ld": False,
    },
}

STEP_CANDIDATES = [
    os.path.join(REPO, "data/examples/Schenkel.stp"),
    "/home/phantom/dev/FreeCAD-Integrate/wt-camdep/data/tests/Step/as1-ac-214.stp",
]


def find_step():
    for path in STEP_CANDIDATES:
        if os.path.exists(path):
            return path
    return ""


def child_env(arm, out_dir):
    env = dict(os.environ)
    env["HOME"] = "/home/phantom"
    env["QT_QPA_PLATFORM"] = "xcb"
    env["FREECAD_USER_HOME"] = os.path.join(out_dir, "home_" + arm)
    env["FCP_ARM"] = arm
    env["FCP_REPO"] = REPO
    env["FCP_STEP"] = find_step()
    cfg = ARMS[arm]
    if cfg.get("ld"):
        bindir = cfg["bindir"]
        mods = [
            os.path.join(bindir, "lib")
        ] + [
            os.path.join(bindir, "Mod", d)
            for d in sorted(os.listdir(os.path.join(bindir, "Mod")))
        ]
        env["LD_LIBRARY_PATH"] = ":".join(mods + [env.get("LD_LIBRARY_PATH", "")])
    return env


def run_probe(arm, scene, work, samples, out_dir, timeout):
    case_id = "%s_%s_%s" % (arm, scene, work)
    out_json = os.path.join(out_dir, "raw", case_id + ".json")
    log = os.path.join(out_dir, "logs", case_id + ".log")
    os.makedirs(os.path.dirname(out_json), exist_ok=True)
    os.makedirs(os.path.dirname(log), exist_ok=True)
    env = child_env(arm, out_dir)
    env.update({
        "FCP_SCENE": scene,
        "FCP_WORK": work,
        "FCP_SAMPLES": str(samples),
        "FCP_OUT": out_json,
    })
    if os.path.exists(out_json):
        os.remove(out_json)
    t0 = time.perf_counter()
    with open(log, "wb") as lf:
        p = subprocess.Popen(
            [ARMS[arm]["bin"], PROBE],
            stdout=lf,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        code = None
        try:
            code = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except Exception:
                p.kill()
            p.wait()
            code = "timeout"
    runtime = time.perf_counter() - t0
    data = {}
    if os.path.exists(out_json):
        try:
            with open(out_json) as f:
                data = json.load(f)
        except Exception:
            data = {}
    data.setdefault("arm", arm)
    data.setdefault("scene", scene)
    data.setdefault("work", work)
    data["ok"] = bool(data.get("ok")) and code == 0
    data["exit_code"] = code
    data["runtime_s"] = round(runtime, 1)
    print(
        "  %-22s %s (%.0fs)" % (case_id, "ok" if data["ok"] else "FAIL", runtime),
        flush=True,
    )
    return data


def _last_float(line):
    for tok in reversed(line.split()):
        try:
            return float(tok)
        except ValueError:
            continue
    return None


def parse_bench_stdout(text):
    out = {}
    for line in text.splitlines():
        low = line.lower()
        if "ms/pick" in low and "unculled" in low and "edge" not in low:
            v = _last_float(line)
            if v is not None:
                out["pick_unculled_ms"] = v
        elif "ms/pick" in low and "culled" in low and "edge" not in low:
            v = _last_float(line)
            if v is not None:
                out["pick_culled_ms"] = v
        elif "parse+transfer" in low:
            v = _last_float(line.split(":")[-1])
            if v is not None:
                out["import_parse_ms"] = v
        elif "cache hit" in low:
            for i, tok in enumerate(line.split()):
                if tok == "ms" and i > 0:
                    try:
                        out["import_cache_ms"] = float(line.split()[i - 1])
                    except ValueError:
                        pass
            for tok in line.split():
                if tok.startswith("speedup="):
                    try:
                        out["import_speedup"] = float(tok.split("=")[1].rstrip("x"))
                    except ValueError:
                        pass
        if "PICKBENCH] speedup" in line:
            v = _last_float(line)
            if v is not None:
                out["pick_speedup"] = v
    return out


def run_bench(arm, exe, env, out_dir, repeats=3):
    path = os.path.join(ARMS[arm]["build"], "tests", exe)
    if not os.path.exists(path):
        return {}
    best = {}
    try:
        for _ in range(repeats):
            r = subprocess.run(
                [path], capture_output=True, text=True, timeout=600, env=env
            )
            parsed = parse_bench_stdout(r.stdout)
            for key, value in parsed.items():
                # A microbenchmark: keep the fastest sample so a transient
                # contention spike from the preceding GUI runs cannot inflate it.
                if key.endswith("_ms"):
                    if key not in best or value < best[key]:
                        best[key] = value
                else:
                    best.setdefault(key, value)
        return best
    except Exception as exc:  # noqa: BLE001
        print("  bench %s/%s failed: %s" % (arm, exe, exc), flush=True)
        return best


def _case_scalar(data, arm, work, key):
    for c in data.get("cases", []):
        if c.get("arm") == arm and c.get("work") == work:
            v = c.get(key)
            if v:
                return float(v)
    return None


def _case_frame(data, arm, work):
    for c in data.get("cases", []):
        if c.get("arm") == arm and c.get("work") == work:
            v = c.get("frame_ms", {}).get("mean")
            if v:
                return float(v)
    return None


def write_summary(data, path):
    arms = data.get("arms") or ["base", "gl", "vk"]
    benches = data.get("benches", {})

    def bench(arm, key):
        v = benches.get(arm, {}).get(key)
        return float(v) if v else None

    rows = [
        ("STEP import (ms)", lambda a: _case_scalar(data, a, "open", "open_ms")),
        ("Orbit frame, moving (ms)", lambda a: _case_frame(data, a, "orbit")),
        ("Idle frame, static (ms)", lambda a: _case_frame(data, a, "idle")),
        ("Pick (ms/pick)", lambda a: _case_scalar(data, a, "pick", "pick_ms_mean")),
        ("[bench] pick unculled (ms)", lambda a: bench(a, "pick_unculled_ms")),
        ("[bench] pick culled (ms)", lambda a: bench(a, "pick_culled_ms")),
        ("[bench] import parse (ms)", lambda a: bench(a, "import_parse_ms")),
        ("[bench] import cache hit (ms)", lambda a: bench(a, "import_cache_ms")),
    ]
    lines = [
        "# Three-arm perf summary",
        "",
        "Generated: %s" % data.get("generated", ""),
        "",
        "STEP: %s" % data.get("step", ""),
        "",
        "| metric | " + " | ".join(arms) + " |",
        "|" + "---|" * (len(arms) + 1),
    ]
    for name, fn in rows:
        cells = []
        for arm in arms:
            v = fn(arm)
            cells.append("n/a" if v is None else "%.3f" % v)
        lines.append("| %s | %s |" % (name, " | ".join(cells)))
    lines += [
        "",
        "`base` = upstream FreeCAD (no Vulkan); `gl` = fork with "
        "`FREECAD_USE_VULKAN=OFF`; `vk` = fork Vulkan.",
        "Lower is better. The `[bench]` rows are fork-only C++ microbenchmarks "
        "(base has no equivalent API).",
    ]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s" % path, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", default="base,gl,vk")
    ap.add_argument("--works", default="step:open,step:pick,box:orbit,box:idle")
    ap.add_argument("--samples", type=int, default=30)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--out", default=os.path.join(HERE, "results"))
    ap.add_argument("--quick", action="store_true", help="samples=8, skip benches")
    ap.add_argument("--no-benches", action="store_true")
    ap.add_argument("--no-graph", action="store_true")
    args = ap.parse_args()

    if args.quick:
        args.samples = 8
        args.no_benches = True

    arms = [a for a in args.arms.split(",") if a]
    works = []
    for spec in args.works.split(","):
        if not spec:
            continue
        scene, work = spec.split(":")
        works.append((scene, work))

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    print("suite: arms=%s works=%s samples=%d" % (arms, works, args.samples), flush=True)
    print("step: %s" % find_step(), flush=True)

    # Run the fork C++ microbenchmarks FIRST: they are short and CPU-sensitive,
    # and running them before the (many) GUI processes avoids their teardown
    # contending with the culled-pick measurement.
    benches = {}
    if not args.no_benches:
        for arm in arms:
            if arm not in ("gl", "vk"):
                continue
            env = child_env(arm, out_dir)
            bench = {}
            bench.update(run_bench(arm, "PartPickBench", env, out_dir))
            bench.update(run_bench(arm, "ImportBench", env, out_dir))
            if bench:
                benches[arm] = bench
                print("  bench %s: %s" % (arm, bench), flush=True)

    cases = []
    for arm in arms:
        if not os.path.exists(ARMS[arm]["bin"]):
            print("  skip %s: binary not found" % arm, flush=True)
            continue
        for scene, work in works:
            cases.append(run_probe(arm, scene, work, args.samples, out_dir, args.timeout))

    results = {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "step": find_step(),
        "arms": arms,
        "cases": cases,
        "benches": benches,
    }
    results_path = os.path.join(out_dir, "results.json")
    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)
        f.write("\n")
    print("wrote %s" % results_path, flush=True)

    write_summary(results, os.path.join(out_dir, "summary.md"))

    if not args.no_graph:
        import graph

        graph.render(results_path, os.path.join(out_dir, "graph.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
