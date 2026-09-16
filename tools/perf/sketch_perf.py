#!/usr/bin/env python3
"""sketch_perf - benchmark, CPU-flame and chart toolchain for the Sketcher solver.

Drives ``tools/perf/sketch_scene_probe.py`` (a guest probe that builds a
deterministic sketch -- by default hundreds of separate, fully-constrained
closed loops -- and re-solves it REPS times) and produces:

  * a solve-time table + ``sketch_bench.json``          (``bench``)
  * a CPU flame graph of the solver (samplib)           (``flame`` -> svg)
  * a per-module / per-submodule CPU attribution table  (``flame`` -> modules)
  * a matplotlib solve-time-vs-size chart               (``chart``)
  * all of the above in one run                         (``all``)

Every subcommand accepts ``--sections`` (comma list) to enable/disable output
sections independently; ``--sections all`` (the default) turns everything on
for that subcommand.  Sections::

    bench      run the timing benchmark + print the table
    json       write sketch_bench.json
    svg        render the CPU flame graph (requires the sampler)
    modules    print per-module / per-submodule CPU attribution
    functions  print the top self-time functions
    chart      render the matplotlib chart

Scene kinds (``--kind``):
  ``loops``  (default) ``--sizes`` separate closed loops (the heavy,
             super-linear "hundreds of drawings in one sketch" case)
  ``chain``  one N-segment staircase (the small, fast baseline)

Examples
--------
  # solve-time sweep over loop counts
  python3 tools/perf/sketch_perf.py bench --kind loops --sizes 25,50,100,200 \\
      --reps 3 --out /tmp/opencode/sketch

  # CPU flame graph of the 100-loop solver (samplib), plus the module table
  python3 tools/perf/sketch_perf.py flame --kind loops --loops 100 --reps 3 \\
      --sections svg,modules,functions --out /tmp/opencode/sketch

  # everything: bench + flame + module table + chart
  python3 tools/perf/sketch_perf.py all --sizes 25,50,100,200 --out /tmp/opencode/sketch

  # only the module table from an existing sampling run
  python3 tools/perf/sketch_perf.py flame \\
      --stacks /tmp/opencode/sketch/sketch.stacks \\
      --maps /tmp/opencode/sketch/sketch.maps --sections modules,functions
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Iterable, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_FCPROBE = os.path.join(os.path.dirname(_HERE), "fcprobe")
if _FCPROBE not in sys.path:
    sys.path.insert(0, _FCPROBE)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from freecad_probe import run_case  # noqa: E402

PROBE = os.path.join(_HERE, "sketch_scene_probe.py")
SAMPLER_SRC = os.path.join(_HERE, "samplib.c")
SAMPLER_SO = "/tmp/opencode/samplib.so"
DEFAULT_BINARY = "/home/phantom/dev/FreeCAD/build/debug/bin/FreeCAD"

ALL_SECTIONS = ("bench", "json", "svg", "modules", "functions", "chart")

SKETCH_LINE = re.compile(
    r"\bsketch scene=(\w+) loops=(\d+) geometry=(\d+) constraints=(\d+) "
    r"reps=(\d+) build_ms=([\d.]+) median_ms=([\d.]+) min_ms=([\d.]+) "
    r"max_ms=([\d.]+) total_ms=([\d.]+)")
REP_LINE = re.compile(
    r"\bsketch_rep scene=(\w+) loops=(\d+) rep=(\d+) ms=([\d.]+)")


@dataclass
class SketchResult:
    kind: str = ""
    loops: int = 0
    geometry: int = 0
    constraints: int = 0
    reps: int = 0
    build_ms: float = 0.0
    median_ms: float = 0.0
    min_ms: float = 0.0
    max_ms: float = 0.0
    total_ms: float = 0.0
    verdict: str = ""
    series: list[float] = field(default_factory=list)
    artifact_dir: str = ""
    errors: list[str] = field(default_factory=list)

    @property
    def size(self) -> int:
        return self.loops

    def to_json(self) -> dict:
        return {
            "kind": self.kind,
            "loops": self.loops,
            "geometry": self.geometry,
            "constraints": self.constraints,
            "reps": self.reps,
            "build_ms": self.build_ms,
            "median_ms": self.median_ms,
            "min_ms": self.min_ms,
            "max_ms": self.max_ms,
            "total_ms": self.total_ms,
            "verdict": self.verdict,
            "artifact_dir": self.artifact_dir,
            "errors": self.errors,
            "series": [round(x, 4) for x in self.series],
        }


def parse_sections(value: str) -> list[str]:
    if not value or value.strip() == "all":
        return list(ALL_SECTIONS)
    wanted = [s.strip() for s in value.split(",") if s.strip()]
    bad = [s for s in wanted if s not in ALL_SECTIONS]
    if bad:
        raise SystemExit("unknown section(s): %s (known: %s)"
                         % (", ".join(bad), ", ".join(ALL_SECTIONS)))
    return wanted


def parse_sketch(lines: Iterable[str]) -> SketchResult:
    res = SketchResult()
    series: dict[int, float] = {}
    for line in lines:
        m = SKETCH_LINE.search(line)
        if m:
            res = SketchResult(
                kind=m.group(1), loops=int(m.group(2)),
                geometry=int(m.group(3)), constraints=int(m.group(4)),
                reps=int(m.group(5)), build_ms=float(m.group(6)),
                median_ms=float(m.group(7)), min_ms=float(m.group(8)),
                max_ms=float(m.group(9)), total_ms=float(m.group(10)))
        m = REP_LINE.search(line)
        if m:
            series[int(m.group(3))] = float(m.group(4))
    res.series = [series[k] for k in sorted(series)]
    return res


def _read_stdout(rep) -> str:
    path = os.path.join(rep.artifact_dir, "stdout.log")
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def _size_env(kind: str, size: int, reps: int, amp: float) -> dict:
    env = {
        "FC_SKETCH_KIND": kind,
        "FC_SKETCH_REPS": str(reps),
        "FC_SKETCH_AMP": str(amp),
    }
    if kind == "chain":
        env["FC_SKETCH_N"] = str(size)
    else:
        env["FC_SKETCH_LOOPS"] = str(size)
    return env


# ---------------------------------------------------------------------------
# bench
# ---------------------------------------------------------------------------
def bench(kind: str, sizes: list[int], reps: int, binary: str, out_dir: str,
          timeout: int, sections: list[str], amp: float) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    results: dict[str, dict] = {}
    for size in sizes:
        env = _size_env(kind, size, reps, amp)
        print(f"[SKETCH] benching kind={kind} size={size} reps={reps}...",
              file=sys.stderr, flush=True)
        rep = run_case(PROBE, binary=binary, profile="gl",
                       env_overrides=env, out_dir=out_dir, timeout=timeout,
                       report_name=f"sketch-{kind}-{size}")
        res = parse_sketch(_read_stdout(rep).splitlines())
        res.kind = kind
        res.verdict = rep.verdict
        res.artifact_dir = rep.artifact_dir
        res.errors = list(rep.errors)
        results[str(size)] = res.to_json()
        print(f"[SKETCH]   -> size={size} geom={res.geometry} "
              f"constraints={res.constraints} build={res.build_ms:.0f}ms "
              f"median={res.median_ms:.0f}ms verdict={rep.verdict}",
              file=sys.stderr, flush=True)

    agg = {"binary": binary, "kind": kind, "reps": reps, "amp": amp,
           "runs": results}

    if "bench" in sections and results:
        print("\n# Sketcher solver: solve time vs sketch size (%s)" % kind)
        print("  %6s %8s %12s %12s %12s %12s" %
              ("size", "geom", "constraints", "build_ms", "median_ms", "max_ms"))
        for key in sorted(results, key=lambda k: int(k)):
            r = results[key]
            print("  %6d %8d %12d %12.1f %12.1f %12.1f" %
                  (r["loops"], r["geometry"], r["constraints"], r["build_ms"],
                   r["median_ms"], r["max_ms"]))

    if "json" in sections:
        agg_path = os.path.join(out_dir, "sketch_bench.json")
        with open(agg_path, "w", encoding="utf-8") as f:
            json.dump(agg, f, indent=2)
        print(f"\n[SKETCH] wrote {agg_path}", file=sys.stderr, flush=True)
    return agg


# ---------------------------------------------------------------------------
# flame (samplib CPU sampling + module attribution)
# ---------------------------------------------------------------------------
def build_sampler() -> str:
    if not os.path.exists(SAMPLER_SO) or (
            os.path.getmtime(SAMPLER_SRC) > os.path.getmtime(SAMPLER_SO)):
        cc = os.environ.get("CC", "gcc")
        cmd = [cc, "-shared", "-fPIC", "-O2", "-o", SAMPLER_SO, SAMPLER_SRC,
               "-ldl"]
        print("[SKETCH] building sampler:", " ".join(cmd),
              file=sys.stderr, flush=True)
        subprocess.run(cmd, check=True)
    return SAMPLER_SO


def _sample(kind: str, size: int, reps: int, binary: str, out_dir: str,
            timeout: int, amp: float, hz: int, stacks: str, maps: str) -> None:
    so = build_sampler()
    env = _size_env(kind, size, reps, amp)
    env.update({
        "LD_PRELOAD": so,
        "FC_PROF_STACK_FILE": stacks,
        "FC_PROF_MAPS_FILE": maps,
        "FC_PROF_HZ": str(hz),
    })
    print(f"[SKETCH] sampling solver kind={kind} size={size} reps={reps} "
          f"hz={hz}...", file=sys.stderr, flush=True)
    rep = run_case(PROBE, binary=binary, profile="gl", env_overrides=env,
                   out_dir=out_dir, timeout=timeout,
                   report_name=f"sketch-sample-{kind}-{size}")
    if rep.errors:
        print("[SKETCH] probe reported errors: %s" % (rep.errors[:3],),
              file=sys.stderr, flush=True)


def flame(kind: str, size: int, reps: int, binary: str, out_dir: str,
          timeout: int, amp: float, hz: int, sections: list[str],
          stacks: Optional[str], maps: Optional[str], focus: str) -> None:
    os.makedirs(out_dir, exist_ok=True)
    # Re-sample unless the caller explicitly handed us a captured pair.
    explicit = stacks is not None and maps is not None
    stacks = stacks or os.path.join(out_dir, "sketch.stacks")
    maps = maps or os.path.join(out_dir, "sketch.maps")
    if not explicit:
        _sample(kind, size, reps, binary, out_dir, timeout, amp, hz, stacks,
                maps)

    if "svg" in sections:
        import sample_flame
        svg = os.path.join(out_dir, "sketch_cpu.svg")
        rc = sample_flame.main([
            "--stacks", stacks, "--maps", maps, "--out", svg,
            "--title", f"Sketch solver CPU ({kind}, size={size}, reps={reps})",
            "--sections", "svg", "--depth", "16", "--min-samples", "20",
        ])
        if rc == 0:
            print(f"[SKETCH] wrote {svg}", file=sys.stderr, flush=True)

    if ("modules" in sections) or ("functions" in sections):
        import module_rollup
        wanted = []
        if "modules" in sections:
            wanted += ["module", "submodule"]
        if "functions" in sections:
            wanted += ["functions"]
        module_rollup.main([
            "--stacks", stacks, "--maps", maps,
            "--sections", ",".join(wanted),
            "--focus", focus,
        ])


# ---------------------------------------------------------------------------
# chart
# ---------------------------------------------------------------------------
def make_chart(bench_path: str, out_path: str) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    with open(bench_path, encoding="utf-8") as f:
        agg = json.load(f)
    runs = sorted(agg["runs"].values(), key=lambda r: r["loops"])
    ns = [r["loops"] for r in runs]
    med = [r["median_ms"] for r in runs]
    lo = [max(0.0, r["median_ms"] - r["min_ms"]) for r in runs]
    hi = [max(0.0, r["max_ms"] - r["median_ms"]) for r in runs]

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.errorbar(ns, med, yerr=[lo, hi], marker="o", capsize=4,
                color="#7a92c4", ecolor="#c4a2a2")
    ax.set_xlabel("sketch size (loops / segments)")
    ax.set_ylabel("solve() time (ms)")
    ax.set_title("Sketcher solver: perturbed solve time vs sketch size (%s)"
                 % agg.get("kind", ""))
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    print(f"[SKETCH] wrote {out_path}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _size_list(value: str) -> list[int]:
    return [int(x) for x in value.split(",") if x.strip()]


def cmd_bench(args) -> None:
    bench(args.kind, _size_list(args.sizes), args.reps, args.binary, args.out,
          args.timeout, parse_sections(args.sections), args.amp)


def cmd_flame(args) -> None:
    flame(args.kind, args.loops, args.reps, args.binary, args.out, args.timeout,
          args.amp, args.hz, parse_sections(args.sections), args.stacks,
          args.maps, args.focus)


def cmd_chart(args) -> None:
    path = args.bench or os.path.join(args.out, "sketch_bench.json")
    out = args.chart_out or os.path.join(args.out, "sketch_chart.png")
    make_chart(path, out)


def cmd_all(args) -> None:
    sections = parse_sections(args.sections)
    sizes = _size_list(args.sizes)
    bench(args.kind, sizes, args.reps, args.binary, args.out, args.timeout,
          sections, args.amp)
    if "svg" in sections or "modules" in sections or "functions" in sections:
        flame(args.kind, args.sample_size or max(sizes), args.sample_reps,
              args.binary, args.out, args.timeout, args.amp, args.hz, sections,
              None, None, args.focus)
    if "chart" in sections:
        make_chart(os.path.join(args.out, "sketch_bench.json"),
                   os.path.join(args.out, "sketch_chart.png"))


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="FreeCAD Sketcher solver perf toolchain")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--binary", default=DEFAULT_BINARY)
        sp.add_argument("--out", default="/tmp/opencode/sketch")
        sp.add_argument("--kind", default="loops",
                        choices=("loops", "chain"))
        sp.add_argument("--amp", type=float, default=0.1,
                        help="perturbation amplitude (mm)")
        sp.add_argument("--sections", default="all",
                        help="comma list: %s (or 'all')" % ",".join(ALL_SECTIONS))

    b = sub.add_parser("bench", help="solve-time sweep over sketch sizes")
    b.add_argument("--sizes", default="25,50,100,200",
                   help="comma list of loop counts (or chain segment counts)")
    b.add_argument("--reps", type=int, default=3)
    b.add_argument("--timeout", type=int, default=600)
    add_common(b)
    b.set_defaults(fn=cmd_bench)

    f = sub.add_parser("flame", help="CPU-sample the solver -> flame + module table")
    f.add_argument("--loops", type=int, default=100,
                   help="loop count (or chain segments) to sample")
    f.add_argument("--reps", type=int, default=3)
    f.add_argument("--hz", type=int, default=200, help="sampler frequency")
    f.add_argument("--timeout", type=int, default=600)
    f.add_argument("--stacks", default=None, help="reuse an existing stacks file")
    f.add_argument("--maps", default=None, help="reuse an existing maps file")
    f.add_argument("--focus", default="", help="keep frames whose file matches")
    add_common(f)
    f.set_defaults(fn=cmd_flame)

    c = sub.add_parser("chart", help="solve-time-vs-size chart from a bench json")
    c.add_argument("--bench", default=None)
    c.add_argument("--out", default="/tmp/opencode/sketch")
    c.add_argument("--chart-out", default=None)
    c.set_defaults(fn=cmd_chart)

    al = sub.add_parser("all", help="bench + flame + modules + chart in one go")
    al.add_argument("--sizes", default="25,50,100,200")
    al.add_argument("--reps", type=int, default=3, help="bench reps per size")
    al.add_argument("--sample-size", type=int, default=100,
                    help="size to sample for the flame (0 = max size)")
    al.add_argument("--sample-reps", type=int, default=3, help="flame reps")
    al.add_argument("--hz", type=int, default=200)
    al.add_argument("--timeout", type=int, default=600)
    al.add_argument("--focus", default="")
    add_common(al)
    al.set_defaults(fn=cmd_all)

    args = p.parse_args(argv)
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
