#!/usr/bin/env python3
"""renderer_perf - benchmark, flame-graph and chart toolchain for the FreeCAD
viewport renderers.

This host tool drives ``tools/perf/renderer_scene_probe.py`` (a guest probe
that builds a deterministic benchmark scene in one render mode) for each
requested render mode, collects the backend's ``[RTDBG] frameTiming`` phase
breakdown, and emits:

  * a per-mode summary + an aggregate ``bench.json``  (``bench``)
  * a self-contained SVG flame graph per renderer        (``flame``)
  * a matplotlib comparison chart                       (``chart``)
  * a textual Vulkan improvement analysis               (``analyze``)

The ``interval``/phase numbers are host wall-clock ms per frame as measured by
the backend itself (``SoRTXRenderBackendCore::renderExternal``), so the flame
graph reflects the *real* per-frame CPU/GPU phase spend::

    frame (interval)                          <- whole presented frame
      asRecord   CPU to record BLAS/TLAS build cmds
      asGpu      AS-phase GPU time (submit+waitIdle)
      denoise    CPU updateDenoise
      traceRecord CPU recordTraceAndPresent
      trace+present+vsync  (derived = interval - the above)  <- dominates (PT)

RasterCoin (OpenGL) has no such instrumentation; for mode 0 the probe measures
an on-demand GL render directly (``fps`` / ``ms`` emitted per mode).

Examples
--------
  # benchmark RasterVulkan, RayTracing, PathTracing, Environment
  python3 tools/perf/renderer_perf.py bench --modes 1,3,4,5 --grid 10 \\
      --out /tmp/opencode/bench

  # + chart, flame graphs, analysis in one go
  python3 tools/perf/renderer_perf.py all --modes 0,1,3,4,5 --grid 10 \\
      --out /tmp/opencode/bench

  # regenerate chart + flames from a saved bench.json (no re-run)
  python3 tools/perf/renderer_perf.py chart --bench /tmp/opencode/bench/bench.json
  python3 tools/perf/renderer_perf.py flame --bench /tmp/opencode/bench/bench.json
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import statistics
import sys
from dataclasses import dataclass, field
from typing import Iterable, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_FCPROBE = os.path.join(os.path.dirname(_HERE), "fcprobe")
if _FCPROBE not in sys.path:
    sys.path.insert(0, _FCPROBE)

from freecad_probe import run_case  # noqa: E402

PROBE = os.path.join(_HERE, "renderer_scene_probe.py")
DEFAULT_BINARY = "/home/phantom/dev/FreeCAD/build/debug/bin/FreeCAD"

MODE_NAMES = {
    0: "RasterCoin (OpenGL)",
    1: "RasterVulkan",
    2: "Wireframe",
    3: "RayTracing",
    4: "PathTracing",
    5: "Environment",
}

# Modes the Vulkan backend emits a frameTiming breakdown for.
TIMED_MODES = frozenset((1, 3, 4, 5))

FRAMETIMING = re.compile(
    r"frameTiming interval=([\d.]+) asRecord=([\d.]+) asGpu=([\d.]+) "
    r"denoise=([\d.]+) traceRecord=([\d.]+)")
GL_LINE = re.compile(r"\bglfps mode=(\d+) frames=(\d+) ms=([\d.]+) fps=([\d.]+)")
VKFPS_LINE = re.compile(r"\bvkfps mode=(\d+) frames=(\d+) elapsed_ms=([\d.]+) frame_ms=([\d.]+) fps=([\d.]+)")


@dataclass
class ModeResult:
    mode: int
    name: str
    verdict: str
    frames: int
    interval: list[float] = field(default_factory=list)
    as_record: list[float] = field(default_factory=list)
    as_gpu: list[float] = field(default_factory=list)
    denoise: list[float] = field(default_factory=list)
    trace_record: list[float] = field(default_factory=list)
    gl_fps: Optional[float] = None
    gl_ms: Optional[float] = None
    vk_fps: Optional[float] = None
    vk_frame_ms: Optional[float] = None
    timed: bool = False
    artifact_dir: str = ""
    errors: list[str] = field(default_factory=list)

    def median(self, xs: list[float]) -> float:
        return statistics.median(xs) if xs else 0.0

    @property
    def fps(self) -> Optional[float]:
        # Sustained FPS: RT uses the tracer's own interval (accumulation
        # cadence); raster/GL use the single-frame on-demand measure.  Both
        # collapse to 1000/interval_ms which is the authoritative frame time.
        if self.interval_ms > 0:
            return 1000.0 / self.interval_ms
        return None

    @property
    def interval_ms(self) -> float:
        # Precise per-frame interval from the renderer's own pacing (RT modes);
        # otherwise the single-frame on-demand measurement (raster Vulkan / GL).
        if self.interval:
            return self.median(self.interval)
        if self.vk_frame_ms:
            return self.vk_frame_ms
        if self.vk_fps:
            return 1000.0 / self.vk_fps
        if self.gl_fps:
            return 1000.0 / self.gl_fps
        return 0.0

    @property
    def one_frame_ms(self) -> float:
        """Single presented frame cost, consistent across ALL backends (on-demand
        frame for Vulkan, offscreen readback for GL).  Used for the cross-mode
        frame-budget comparison because the RT ``interval`` (warm accumulation
        cadence) is NOT directly comparable to a raster on-demand frame."""
        if self.vk_frame_ms:
            return self.vk_frame_ms
        if self.gl_ms:
            return self.gl_ms
        return self.interval_ms

    @property
    def as_record_ms(self) -> float:
        return self.median(self.as_record)

    @property
    def as_gpu_ms(self) -> float:
        return self.median(self.as_gpu)

    @property
    def denoise_ms(self) -> float:
        return self.median(self.denoise)

    @property
    def trace_record_ms(self) -> float:
        return self.median(self.trace_record)

    @property
    def trace_present_ms(self) -> float:
        """Derived 'trace kernel + present + vsync' band (the PT dominant)."""
        buffered = (self.as_record_ms + self.as_gpu_ms + self.denoise_ms
                    + self.trace_record_ms)
        return max(0.0, self.interval_ms - buffered)

    def bands(self) -> dict[str, float]:
        if self.interval_ms <= 0:
            return {}
        return {
            "asRecord": self.as_record_ms,
            "asGpu": self.as_gpu_ms,
            "denoise": self.denoise_ms,
            "traceRecord": self.trace_record_ms,
            "trace+present+vsync": self.trace_present_ms,
        }

    def to_json(self) -> dict:
        return {
            "mode": self.mode,
            "name": self.name,
            "verdict": self.verdict,
            "frames": self.frames,
            "timed": self.timed,
            "fps": self.fps,
            "interval_ms": self.interval_ms,
            "one_frame_ms": self.one_frame_ms,
            "asRecord_ms": self.as_record_ms,
            "asGpu_ms": self.as_gpu_ms,
            "denoise_ms": self.denoise_ms,
            "traceRecord_ms": self.trace_record_ms,
            "tracePresent_ms": self.trace_present_ms,
            "median_ms": {
                "interval": self.interval_ms,
                "asRecord": self.as_record_ms,
                "asGpu": self.as_gpu_ms,
                "denoise": self.denoise_ms,
                "traceRecord": self.trace_record_ms,
                "trace+present+vsync": self.trace_present_ms,
            },
            "series": {
                "interval": _round(self.interval, 3),
                "asRecord": _round(self.as_record, 3),
                "asGpu": _round(self.as_gpu, 3),
                "denoise": _round(self.denoise, 3),
                "traceRecord": _round(self.trace_record, 3),
            },
            "gl": {"fps": self.gl_fps, "ms": self.gl_ms},
            "vk": {"fps": self.vk_fps, "frame_ms": self.vk_frame_ms},
            "artifact_dir": self.artifact_dir,
            "errors": self.errors,
        }


def _round(xs: list[float], nd: int) -> list[float]:
    return [round(float(x), nd) for x in xs]


def parse_frametiming(lines: Iterable[str]) -> ModeResult:
    res = ModeResult(mode=0, name="", verdict="", frames=0)
    for line in lines:
        for m in FRAMETIMING.finditer(line):
            res.interval.append(float(m.group(1)))
            res.as_record.append(float(m.group(2)))
            res.as_gpu.append(float(m.group(3)))
            res.denoise.append(float(m.group(4)))
            res.trace_record.append(float(m.group(5)))
            res.timed = True
        m = GL_LINE.search(line)
        if m:
            res.gl_ms = float(m.group(3))
            res.gl_fps = float(m.group(4))
        m = VKFPS_LINE.search(line)
        if m:
            res.vk_fps = float(m.group(5))
            res.vk_frame_ms = float(m.group(4))
            res.frames = max(res.frames, int(m.group(2)))
    # Drop the first sample (asRecord is inflated by the initial shader build).
    if res.interval:
        res.interval = res.interval[1:]
        res.as_record = res.as_record[1:]
        res.as_gpu = res.as_gpu[1:]
        res.denoise = res.denoise[1:]
        res.trace_record = res.trace_record[1:]
        res.frames = max(res.frames, len(res.interval))
    return res


def _parse_verdict(log_text: str) -> str:
    for line in log_text.splitlines():
        m = re.search(r"\[VERDICT\] (\S+) (PASS|FAIL)", line)
        if m:
            return m.group(2)
    return "UNKNOWN"


def bench(modes: list[int], grid: int, binary: str, out_dir: str,
          timeout: int, frames: int, samples: int, bounces: int) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    results: dict[str, dict] = {}
    for mode in modes:
        name = MODE_NAMES.get(mode, f"mode{mode}")
        env = {
            "FC_PERF_MODE": str(mode),
            "FC_PERF_N": str(grid),
            "FC_PERF_FRAMES": str(frames),
            "FC_PERF_BOUNCES": str(bounces),
            "FC_VULKAN_RT_DEBUG": "1",
            "FC_VULKAN_FRAME_TIMING": "1",
            "FC_VULKAN_PT_MAXSAMPLES": str(samples),
        }
        print(f"[PERF] benching {name} (mode {mode}, grid {grid})...",
              file=sys.stderr, flush=True)
        rep = run_case(PROBE, binary=binary, profile="vulkan",
                       env_overrides=env, out_dir=out_dir,
                       timeout=timeout,
                       report_name=f"renderer-{mode}")
        log_path = os.path.join(rep.artifact_dir, "stdout.log")
        with open(log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        res = parse_frametiming(text.splitlines())
        res.mode = mode
        res.name = name
        res.verdict = rep.verdict
        res.artifact_dir = rep.artifact_dir
        res.errors = list(rep.errors)
        results[str(mode)] = res.to_json()
        print(f"[PERF]   -> {name}: verdict={rep.verdict} "
              f"fps={res.fps} interval={res.interval_ms:.2f}ms "
              f"trace={res.trace_present_ms:.2f}ms", file=sys.stderr, flush=True)
    agg = {
        "binary": binary,
        "grid": grid,
        "frames": frames,
        "samples": samples,
        "bounces": bounces,
        "modes": results,
    }
    agg_path = os.path.join(out_dir, "bench.json")
    with open(agg_path, "w", encoding="utf-8") as f:
        json.dump(agg, f, indent=2)
    print(f"[PERF] wrote {agg_path}", file=sys.stderr, flush=True)
    return agg


# ---------------------------------------------------------------------------
# Flame graph (self-contained interactive SVG)
# ---------------------------------------------------------------------------
def _shared_bucket(bands: dict[str, float], total: float) -> float:
    """Proportional throughput band for a flame-graph (px/sample) scaling."""
    return total


def flame_svg(title: str, root: dict[str, float], width: int = 1200,
              height: float = 260) -> str:
    """Render one modal frame's phase bands as an SVG flame graph.

    ``root`` is a ``{band_name: ms}`` map (frame total = sum of bands).  Each
    band is a rectangle whose width is proportional to its share of the frame;
    hovering shows the name + ms + %.  Rendered top-of-frame-first (like a
    flame graph root) with child bands stacked below.
    """
    total = math.fsum(root.values()) or 1.0
    xpad, ypad = 10, 30
    title_h = 24
    row_h = 44
    usable_w = width - 2 * xpad
    svg_h = title_h + ypad + row_h + 34

    # One stacked row (the "frame" as a flame) so a dominating phase doesn't
    # push the small ones off-scale: phases are laid back-to-back along x and
    # each segment is labelled with its ms + % of the frame.
    ordered = sorted(root.items(), key=lambda kv: (-kv[1], kv[0]))
    palette = ["#7a92c4", "#a2c4e0", "#d0b7e0", "#e0c9a2", "#c4e0cf",
               "#e0a2a2", "#b0d0e0", "#c9a2e0"]
    rects = []
    x = float(xpad)
    for i, (label, ms) in enumerate(ordered):
        w = (ms / total) * usable_w
        share = (ms / total) * 100.0 if total else 0.0
        fill = palette[i % len(palette)]
        rects.append(
            f'  <g class="band" data-label="{_esc(label)}" data-ms="{ms:.3f}" '
            f'data-share="{share:.1f}">\n'
            f'    <rect x="{x:.1f}" y="{ypad + row_h:.1f}" '
            f'width="{max(w, 1.0):.1f}" height="{row_h - 6:.1f}" '
            f'rx="2" fill="{fill}" stroke="#3a4a66" stroke-width="0.5"/>\n'
            f'    <text x="{x + 6:.1f}" y="{ypad + row_h + 24:.1f}" '
            f'font-family="monospace" font-size="12" fill="#10243f">{_esc(label)}'
            f'  {ms:.2f}ms ({share:.1f}%)</text>\n  </g>')
        x += w

    tooltip = ('  <div id="tip" style="position:fixed;pointer-events:none;'
               'background:#fff;border:1px solid #999;padding:4px 8px;'
               'font:12px monospace;display:none"></div>\n')
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{svg_h}" \
viewBox="0 0 {width} {svg_h}" font-family="sans-serif">
  <rect width="{width}" height="{svg_h}" fill="#f4f6fa"/>
  <text x="{xpad}" y="18" font-family="monospace" font-size="13" fill="#10243f">\
{_esc(title)}</text>
  <text x="{width - xpad}" y="18" text-anchor="end" font-family="monospace" \
font-size="12" fill="#5a6a80">frame {total:.2f} ms</text>
  {tooltip}
  {'\n'.join(rects)}
  <script type="text/javascript"><![CDATA[
    (function() {{
      var tip = document.getElementById('tip');
      document.querySelectorAll('g.band').forEach(function(g) {{
        g.addEventListener('mousemove', function(e) {{
          tip.style.display = 'block';
          tip.style.left = (e.pageX + 12) + 'px';
          tip.style.top = (e.pageY + 12) + 'px';
          tip.textContent = g.getAttribute('data-label') + ' — ' +
            g.getAttribute('data-ms') + ' ms (' +
            g.getAttribute('data-share') + '%)';
        }});
        g.addEventListener('mouseleave', function() {{ tip.style.display = 'none'; }});
      }});
    }})();
  ]]></script>
</svg>
"""


def _esc(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def generate_flames(agg: dict, out_dir: str) -> list[str]:
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for mode, info in agg["modes"].items():
        # Only RT modes carry a real phase breakdown; the derived interval for
        # non-RT Vulkan modes has no genuine asRecord/asGpu/denoise bands.
        if not info.get("timed"):
            continue
        median = info.get("median_ms", {})
        # Drop zero/absent bands so the flame only shows real phases.
        bands = {k: v for k, v in median.items()
                 if k != "interval" and (v or 0) > 0}
        if not bands:
            continue
        title = f"{info['name']} (mode {mode}) — per-frame phase spend"
        svg = flame_svg(title, bands)
        path = os.path.join(out_dir, f"flame_{mode}.svg")
        with open(path, "w", encoding="utf-8") as f:
            f.write(svg)
        paths.append(path)
    return paths


# ---------------------------------------------------------------------------
# Chart (matplotlib)
# ---------------------------------------------------------------------------
def make_chart(agg: dict, out_path: str) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    modes = sorted(agg["modes"].values(), key=lambda m: m["mode"])
    labels = [m["name"] for m in modes]
    fps = [(1000.0 / m["interval_ms"]) if m["interval_ms"] > 0 else None
           for m in modes]
    interval = [m["interval_ms"] for m in modes]

    fig, axes = plt.subplots(1, 3, figsize=(19, 5.2))
    fig.suptitle("FreeCAD viewport renderers — benchmark (same scene)", fontsize=14)

    # 1) FPS
    colors = ["#7a92c4" if f is not None else "#cccccc" for f in fps]
    axes[0].bar(labels, [f or 0 for f in fps], color=colors)
    for i, f in enumerate(fps):
        axes[0].text(i, (f or 0) + max([x or 0 for x in fps]) * 0.02,
                     f"{f:,.1f}" if f else "n/a", ha="center", fontsize=9)
    axes[0].set_title("Steady-state FPS")
    axes[0].set_ylabel("frames / sec")
    axes[0].tick_params(axis="x", rotation=20, labelsize=8)

    # 2) interval (ms/frame) with phase stack.  Uninstrumented raster/GL modes
    # get a single neutral "present/sync" band, not a fake trace phase.
    phases = ["asRecord", "asGpu", "denoise", "traceRecord",
              "trace+present+vsync"]
    phase_colors = ["#7a92c4", "#a2c4e0", "#d0b7e0", "#e0c9a2", "#e0a2a2"]
    bottoms = np.zeros(len(modes))
    for p, pc in zip(phases, phase_colors):
        vals = np.array([m["median_ms"].get(p, 0.0) if m["timed"] else 0.0
                         for m in modes])
        axes[1].bar(labels, vals, bottom=bottoms, color=pc, label=p)
        if len(modes) == 1:
            axes[1].text(0, bottoms[0] + vals[0] / 2, f"{p}", ha="center",
                         fontsize=7)
        bottoms += vals
    # raster/GL modes: no frameTiming phases, so show their total as present/sync.
    for i, m in enumerate(modes):
        if not m["timed"] and m["interval_ms"] > 0:
            axes[1].bar(labels[i], m["interval_ms"], bottom=0,
                        color="#b8b8c0",
                        label="present/sync (uninstrumented)" if i == 0 else None)
            if len(modes) == 1:
                axes[1].text(0, m["interval_ms"] / 2, "present/sync",
                             ha="center", fontsize=7)
    axes[1].set_title("Per-frame time by phase (ms)")
    axes[1].set_ylabel("ms / frame")
    axes[1].tick_params(axis="x", rotation=20, labelsize=8)
    axes[1].legend(fontsize="7", loc="upper right")

    # 3) frame budget split: trace+present vs everything else (RT modes only)
    trace_pct = []
    for m in modes:
        it = m["interval_ms"]
        traced = m["median_ms"].get("trace+present+vsync", 0) if m["timed"] else 0.0
        trace_pct.append(100 * traced / it if it > 0 else 0.0)
    axes[2].bar(labels, trace_pct, color="#a2c4e0")
    for i, v in enumerate(trace_pct):
        axes[2].text(i, v + 1, (f"{v:.1f}%" if v > 0 else "n/a"),
                     ha="center", fontsize=9)
    axes[2].set_title("Share of frame in trace/present/vsync (RT modes)")
    axes[2].set_ylabel("% of frame")
    axes[2].set_ylim(0, 108)
    axes[2].tick_params(axis="x", rotation=20, labelsize=8)

    fig.tight_layout()
    fig.savefig(out_path, dpi=110)
    print(f"[PERF] wrote {out_path}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------
def analyze(agg: dict, out_path: Optional[str] = None) -> str:
    lines = []
    lines.append("FreeCAD viewport renderers — performance analysis")
    lines.append("==================================================")
    info0 = next(iter(agg["modes"].values()), {})
    lines.append(f"scene: {agg['grid']}x{agg['grid']} box grid (+cyl+sphere), "
                 f"{agg['bounces']} PT bounces, frames {agg['frames']}")
    lines.append("FPS = 1000 / frame time.  Frame time = the tracer's own "
                 "frameTiming interval for RT modes (3/4/5, sustained "
                 "accumulation cadence) and the single-frame on-demand measure "
                 "for the raster/GL backends.")
    lines.append("")

    for mode, info in sorted(agg["modes"].items(), key=lambda kv: kv[1]["mode"]):
        name = info["name"]
        it = info["interval_ms"]
        fps = (1000.0 / it) if it > 0 else None
        lines.append(f"-- {name} (mode {mode})")
        if it > 0:
            lines.append(f"     fps={fps and round(fps, 1)}   "
                         f"frame time {it:.2f} ms")
        else:
            lines.append("     (no frame data)")
        if info.get("timed"):
            med = info["median_ms"]
            trace = med.get("trace+present+vsync", 0.0)
            asgpu = med.get("asGpu", 0.0)
            asrec = med.get("asRecord", 0.0)
            den = med.get("denoise", 0.0)
            lines.append("        phase split:")
            lines.append(f"           asRecord {asrec:.2f} ms  "
                         f"(asGpu {asgpu:.2f} ms, denoise {den:.2f} ms)")
            lines.append(f"           trace+present+vsync {trace:.2f} ms "
                         f"({100 * trace / it:.0f}% of frame)")
        else:
            lines.append("        (raster/GL backend: no phase instrumentation; "
                         "the one-frame cost is present/sync bound)")
        lines.append("")

    # Findings (evidence-based, honest about the two distinct metrics).
    lines.append("Findings / improvement opportunities for Vulkan")
    lines.append("-----------------------------------------------")
    timed = {m: i for m, i in agg["modes"].items()
             if i.get("interval_ms", 0) > 0}
    findings: list[str] = []
    pt = timed.get("4")
    if pt:
        pit = pt["interval_ms"]
        med = pt["median_ms"]
        trace = med.get("trace+present+vsync", 0.0)
        asgpu = med.get("asGpu", 0.0)
        findings.append(
            "PathTracing (mode 4): "
            f"{100 * trace / pit:.0f}% of its frame is the path-trace kernel + "
            "present + vsync.  The AS-build phase is only "
            f"{asgpu:.2f} ms ({100 * asgpu / pit:.1f}%) and denoise "
            f"{med.get('denoise', 0):.2f} ms, so the planned AS optimisations "
            "(TLAS cull / AS pack / compaction — items 1 & 4b of the perf plan) "
            "will NOT move frame time on this scene.  Attack the trace: fewer "
            "bounces, cheaper/off denoiser, FSR 2 upscale of a lower-resolution "
            "accumulation buffer, or an interaction LOD that drops to a "
            "single-sample AO (mode 3) / raster while orbiting.")
        if asgpu / pit > 0.2:
            findings.append(
                "At this grid the AS build "
                f"({asgpu:.2f} ms) is {100 * asgpu / pit:.0f}% of the PT frame.  "
                "If it scales with instance count, enable FC_VULKAN_TLAS_CULL / "
                "FC_VULKAN_AS_PACK / FC_VULKAN_AS_COMPACT; re-run bench at a "
                "larger grid to confirm.")

    if "1" in timed and "4" in timed:
        rv_i, pt_i = timed["1"]["interval_ms"], timed["4"]["interval_ms"]
        vblank = 16.7
        if pt_i < vblank:
            findings.append(
                "On this small scene both Vulkan modes render faster than the "
                "16.7 ms vblank, so the swapchain/vsync dominates: they cluster "
                f"close to {int(vblank)} fps.  The GPU work (RT frameTiming "
                "interval) is hidden under the vblank.  Re-bench at a larger "
                "scene / more bounces so the trace exceeds the vblank budget — "
                "see the scaled flame graphs (and the per-phase SVGs).")
        else:
            findings.append(
                "The tracer exceeds the 16.7 ms vblank here: "
                f"RasterVulkan {rv_i:.1f} ms vs PathTracing {pt_i:.1f} ms — "
                f"RasterVulkan is {pt_i / rv_i:.1f}x faster per frame.  The "
                "path-trace kernel is the visible cost (biggest bar of the PT "
                "flame graph); per-frame trace optimisation (rather than AS work) "
                "is where the win is.")
    if "1" in timed and timed["1"]["interval_ms"] > 30:
        rv = timed["1"]["interval_ms"]
        findings.append(
            "RasterVulkan also collapses at this instance count "
            f"({rv:.1f} ms).  Raster should be far cheaper than the tracer, so "
            "this is a scaling issue in the raster Vulkan path — per-draw CPU "
            "submission, the end-of-frame vkQueueWaitIdle, or unbounded geometry "
            "cache growth.  Investigate draw batching / instancing and the "
            "per-frame synchronisation before buying GPU time.")
    if "0" in timed and timed["0"]["interval_ms"]:
        gl_i = timed["0"]["interval_ms"]
        gl_fps = 1000.0 / gl_i
        msg = ("RasterCoin (OpenGL) reference: "
               f"{round(gl_fps, 1)} fps / {gl_i:.2f} ms (offscreen readback).")
        # Coin is the legacy baseline; if the Vulkan raster backend is slower it
        # is a real regression worth exposing.
        if "1" in timed and timed["1"]["interval_ms"] > gl_i * 1.05:
            rv_i = timed["1"]["interval_ms"]
            msg += (f"  Coin is {rv_i / gl_i:.2f}x faster than the Vulkan raster "
                    "backend at this instance count — the Vulkan raster path must "
                    "be batched/instanced and its per-frame sync fixed before it "
                    "justifies replacing the GL backend.")
        findings.append(msg)
    for i, f in enumerate(findings, 1):
        lines.append(f"  {i}. {f}")

    text = "\n".join(lines)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print(f"[PERF] wrote {out_path}", file=sys.stderr)
    return text


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def cmd_bench(args) -> None:
    modes = [int(m) for m in args.modes.split(",") if m != ""]
    agg = bench(modes=modes, grid=args.grid, binary=args.binary,
                out_dir=args.out, timeout=args.timeout, frames=args.frames,
                samples=args.samples, bounces=args.bounces)


def cmd_flame(args) -> None:
    with open(args.bench, encoding="utf-8") as f:
        agg = json.load(f)
    out = args.out or os.path.dirname(args.bench)
    paths = generate_flames(agg, out)
    for p in paths:
        print(f"[PERF] flame graph: {p}", file=sys.stderr)


def cmd_chart(args) -> None:
    with open(args.bench, encoding="utf-8") as f:
        agg = json.load(f)
    out = args.out or os.path.join(os.path.dirname(args.bench), "chart.png")
    make_chart(agg, out)


def cmd_analyze(args) -> None:
    with open(args.bench, encoding="utf-8") as f:
        agg = json.load(f)
    out = args.out
    print(analyze(agg, out))


def cmd_all(args) -> None:
    modes = [int(m) for m in args.modes.split(",") if m != ""]
    bench_dir = args.out
    agg = bench(modes=modes, grid=args.grid, binary=args.binary,
                out_dir=bench_dir, timeout=args.timeout, frames=args.frames,
                samples=args.samples, bounces=args.bounces)
    flames = generate_flames(agg, bench_dir)
    chart = os.path.join(bench_dir, "chart.png")
    make_chart(agg, chart)
    analyze(agg, os.path.join(bench_dir, "analysis.txt"))
    print(f"[PERF] flame graphs:", file=sys.stderr)
    for p in flames:
        print(f"[PERF]   {p}", file=sys.stderr)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="FreeCAD renderer performance toolchain")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--binary", default=DEFAULT_BINARY)
        sp.add_argument("--out", default="/tmp/opencode/bench")

    b = sub.add_parser("bench", help="run the benchmark across render modes")
    b.add_argument("--modes", default="1,3,4,5")
    b.add_argument("--grid", type=int, default=10, help="box-grid side (N) for the scene")
    b.add_argument("--frames", type=int, default=40)
    b.add_argument("--samples", type=int, default=32, help="PT sample cap")
    b.add_argument("--bounces", type=int, default=3)
    b.add_argument("--timeout", type=int, default=180)
    add_common(b)
    b.set_defaults(fn=cmd_bench)

    f = sub.add_parser("flame", help="generate SVG flame graphs from a bench.json")
    f.add_argument("--bench", required=True)
    f.add_argument("--out", default=None)
    f.set_defaults(fn=cmd_flame)

    c = sub.add_parser("chart", help="build the matplotlib comparison chart")
    c.add_argument("--bench", required=True)
    c.add_argument("--out", default=None)
    c.set_defaults(fn=cmd_chart)

    a = sub.add_parser("analyze", help="print Vulkan improvement analysis")
    a.add_argument("--bench", required=True)
    a.add_argument("--out", default=None)
    a.set_defaults(fn=cmd_analyze)

    al = sub.add_parser("all", help="bench + flame + chart + analyze in one go")
    al.add_argument("--modes", default="0,1,3,4,5")
    al.add_argument("--grid", type=int, default=10)
    al.add_argument("--frames", type=int, default=40)
    al.add_argument("--samples", type=int, default=32)
    al.add_argument("--bounces", type=int, default=3)
    al.add_argument("--timeout", type=int, default=180)
    add_common(al)
    al.set_defaults(fn=cmd_all)

    args = p.parse_args(argv)
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
