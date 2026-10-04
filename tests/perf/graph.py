#!/usr/bin/env python3
"""Render the three-arm comparison graph from a results.json produced by
tests/perf/run_suite.py.

    python3 tests/perf/graph.py results/results.json results/graph.png
"""

import json
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ARM_ORDER = ["base", "gl", "vk"]
COLORS = {"base": "#8c8c8c", "gl": "#4c72b0", "vk": "#dd8452"}


def _frame_mean(cases, arm, work):
    for c in cases:
        if c.get("arm") == arm and c.get("work") == work:
            v = c.get("frame_ms", {}).get("mean")
            if v:
                return float(v)
    return None


def _scalar(cases, arm, work, key):
    for c in cases:
        if c.get("arm") == arm and c.get("work") == work:
            v = c.get(key)
            if v:
                return float(v)
    return None


def _has_work(cases, work):
    return any(c.get("work") == work for c in cases)


def bar_panel(ax, title, labels, values, ylabel, note="lower is better"):
    xs = list(range(len(labels)))
    vals = [(v if v is not None else 0.0) for v in values]
    colors = [COLORS.get(label, "#cccccc") for label in labels]
    ax.bar(xs, vals, color=colors)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, fontsize=12)
    ax.set_title("%s\n(%s)" % (title, note), fontsize=12)
    ax.set_ylabel(ylabel)
    top = max(vals) if vals else 1.0
    for x, v in zip(xs, values):
        ax.text(
            x,
            (v if v is not None else 0.0) + top * 0.02,
            "n/a" if v is None else "%.2f" % v,
            ha="center",
            va="bottom",
            fontsize=10,
        )
    ax.margins(y=0.18)


def grouped_panel(ax, title, categories, series, ylabel):
    """series: list of (label, [values per category], color)."""
    n = len(categories)
    m = len(series)
    width = 0.8 / max(m, 1)
    xs = list(range(n))
    for i, (label, values, color) in enumerate(series):
        off = (i - (m - 1) / 2.0) * width
        vals = [(v if v is not None else 0.0) for v in values]
        ax.bar([x + off for x in xs], vals, width=width, label=label, color=color)
    ax.set_xticks(xs)
    ax.set_xticklabels(categories, fontsize=12)
    ax.set_title(title + "\n(lower is better)", fontsize=12)
    ax.set_ylabel(ylabel)
    ax.legend(fontsize=9)


def render(results_path, out_png):
    with open(results_path) as f:
        data = json.load(f)
    cases = data.get("cases", [])
    benches = data.get("benches", {})
    arms = data.get("arms") or ARM_ORDER
    labels = [a for a in ARM_ORDER if a in arms] or arms

    fig, axs = plt.subplots(2, 3, figsize=(17, 9.5))
    axs = axs.ravel()

    bar_panel(
        axs[0],
        "STEP import (Import.open)",
        labels,
        [_scalar(cases, a, "open", "open_ms") for a in labels],
        "ms",
    )
    bar_panel(
        axs[1],
        "Orbit frame (moving camera)",
        labels,
        [_frame_mean(cases, a, "orbit") for a in labels],
        "ms/frame",
    )
    bar_panel(
        axs[2],
        "Pick (getObjectInfo)",
        labels,
        [_scalar(cases, a, "pick", "pick_ms_mean") for a in labels],
        "ms/pick",
    )
    bar_panel(
        axs[3],
        "Idle frame (static camera)",
        labels,
        [_frame_mean(cases, a, "idle") for a in labels],
        "ms/frame",
    )

    bench_arms = [a for a in ARM_ORDER if a in benches]
    grouped_panel(
        axs[4],
        "Fork pick bench: face ray-pick",
        bench_arms,
        [
            ("unculled", [benches[a].get("pick_unculled_ms") for a in bench_arms], "#c44e52"),
            ("culled", [benches[a].get("pick_culled_ms") for a in bench_arms], "#55a868"),
        ],
        "ms/pick",
    )
    grouped_panel(
        axs[5],
        "Fork import bench: STEP parse vs cache",
        bench_arms,
        [
            ("parse+transfer", [benches[a].get("import_parse_ms") for a in bench_arms], "#c44e52"),
            ("cache hit", [benches[a].get("import_cache_ms") for a in bench_arms], "#55a868"),
        ],
        "ms",
    )

    fig.suptitle(
        "FreeCAD perf: upstream base vs fork GL vs fork Vulkan   (%s)"
        % data.get("generated", ""),
        fontsize=14,
    )
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out_png, dpi=120)
    print("wrote %s" % out_png, flush=True)
    return out_png


if __name__ == "__main__":
    rp = sys.argv[1] if len(sys.argv) > 1 else "results/results.json"
    op = sys.argv[2] if len(sys.argv) > 2 else "results/graph.png"
    render(rp, op)
