#!/usr/bin/env python3
"""Render the performance comparison charts (PNG) from the collected JSON."""

from __future__ import annotations

import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from _paths import HERE as PC, REPO

OUT = os.path.join(REPO, "docs", "vulkan-perf")
os.makedirs(OUT, exist_ok=True)

C_BASE = "#6c757d"
C_FGL = "#4c9be8"
C_FVK = "#f76707"
C_RT = "#7048e8"
C_PT = "#2f9e44"

plt.rcParams.update({
    "figure.facecolor": "white",
    "axes.grid": True,
    "grid.alpha": 0.25,
    "grid.linestyle": "--",
    "axes.axisbelow": True,
    "font.size": 10,
    "axes.titlesize": 12,
    "axes.titleweight": "bold",
})


def load():
    xcb = json.load(open(f"{PC}/results/scene_results_novsync.json"))
    extra = json.load(open(f"{PC}/results_extra/extra.json"))
    open_ = json.load(open(f"{PC}/results_open/open.json"))
    return xcb, extra, open_


def idx(rows, name):
    for r in rows:
        if r["name"] == name:
            return r
    return None


def bench_val(rec, key):
    if not rec or not rec.get("bench"):
        return None
    return float(rec["bench"][key])


def bar_labels(ax, bars, fmt="{:.1f}", pct=False):
    for b in bars:
        h = b.get_height()
        if h is None or (isinstance(h, float) and (np.isnan(h))):
            continue
        ax.annotate(fmt.format(h), (b.get_x() + b.get_width() / 2, h),
                    ha="center", va="bottom", fontsize=8, xytext=(0, 2),
                    textcoords="offset points")


def grouped(ax, groups, series, log=True, ylabel="fps", title=""):
    n = len(groups)
    m = len(series)
    x = np.arange(n)
    w = 0.8 / m
    for i, (label, color, vals) in enumerate(series):
        bars = ax.bar(x + (i - (m - 1) / 2) * w, vals, w, label=label, color=color)
        bar_labels(ax, bars)
    ax.set_xticks(x)
    ax.set_xticklabels(groups)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if log:
        ax.set_yscale("log")
    ax.legend(fontsize=8, framealpha=0.9)
    ax.margins(y=0.18)


# ---------------------------------------------------------------------------
def chart_throughput(ax):
    rows, _, _ = load()
    groups = ["empty", "sketches 200", "objects 1k", "objects 5k", "objects 15k"]
    keys = ["empty", "sketches200", "objects1000", "objects5000", "objects15000"]
    series = []
    for pref, label, color in [("base_gl_x", "base GL", C_BASE),
                               ("fork_gl_x", "fork GL", C_FGL),
                               ("fork_vk_x", "fork Vulkan", C_FVK)]:
        vals = [bench_val(idx(rows, f"{pref}_{k}"), "fps") for k in keys]
        series.append((label, color, vals))
    grouped(ax, groups, series, log=True, ylabel="frames / second (log)",
            title="Scene throughput (xcb, VulkanPresentMode=Immediate)")


def chart_vorontest(ax):
    _, extra, _ = load()
    names = ["voron_base_gl", "voron_fork_gl", "voron_fork_vk", "voron_fork_vk_pt"]
    labels = ["base GL", "fork GL", "fork Vulkan", "fork Vk path-trace"]
    colors = [C_BASE, C_FGL, C_FVK, C_PT]
    open_ms = [bench_val(idx(extra, n), "build_ms") for n in names]
    fps = [bench_val(idx(extra, n), "fps") for n in names]
    x = np.arange(len(names))
    ax.bar(x - 0.2, open_ms, 0.4, label="open (ms, log)", color=C_BASE)
    ax.set_yscale("log")
    for xi, v in zip(x - 0.2, open_ms):
        ax.annotate(f"{v:.0f}", (xi, v), ha="center", va="bottom", fontsize=8)
    ax.set_ylim(top=max(open_ms) * 4)
    ax2 = ax.twinx()
    ax2.bar(x + 0.2, fps, 0.4, label="render fps", color=C_FVK)
    ax2.set_yscale("log")
    for xi, v in zip(x + 0.2, fps):
        ax2.annotate(f"{v:.0f}", (xi, v), ha="center", va="bottom", fontsize=8)
    ax2.set_ylim(top=max(fps) * 4)
    ax2.grid(False)
    ax.set_xticks(x)
    ax.set_xticklabels([l.replace(" ", "\n", 1) for l in labels], fontsize=8)
    ax.set_ylabel("document open (ms, log)")
    ax2.set_ylabel("render (fps, log)")
    ax.set_title("vorontest.FCStd (44.6 MB, single huge Part)")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, fontsize=8, loc="upper left")


def chart_startup(ax):
    _, _, open_ = load()
    names = ["open_base_gl", "open_fork_gl", "open_fork_vk"]
    labels = ["base GL", "fork GL", "fork Vulkan"]
    gui = [idx(open_, n)["gui_ready_ms"] for n in names]
    doc = [idx(open_, n)["doc_median_ms"] for n in names]
    total = [idx(open_, n)["open_total_ms"] for n in names]
    x = np.arange(len(names))
    w = 0.26
    for i, (vals, lab, col) in enumerate([
            (gui, "startup incl. logo", C_BASE),
            (doc, "open cube (median)", C_FGL),
            (total, "launch -> cube open", C_FVK)]):
        bars = ax.bar(x + (i - 1) * w, vals, w, label=lab, color=col)
        bar_labels(ax, bars, "{:.0f}")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("milliseconds")
    ax.set_title("Startup & cube-document open")
    ax.legend(fontsize=8)
    ax.margins(y=0.18)


def chart_rotate(ax):
    _, extra, _ = load()
    names = ["rot_base_gl", "rot_fork_gl", "rot_fork_vk",
             "rot_fork_vk_rt", "rot_fork_vk_pt", "rot_fork_vk_env"]
    labels = ["base GL", "fork GL", "Vk raster", "Vk ray-trace",
              "Vk path-trace", "Vk environment"]
    colors = [C_BASE, C_FGL, C_FVK, C_RT, C_PT, "#e8590c"]
    fps = [bench_val(idx(extra, n), "fps") for n in names]
    bars = ax.bar(np.arange(len(names)), fps, 0.62, color=colors)
    bar_labels(ax, bars, "{:.0f}")
    ax.set_xticks(np.arange(len(names)))
    ax.set_xticklabels([l.replace(" ", "\n", 1) for l in labels], fontsize=8)
    ax.set_ylabel("frames / second")
    ax.set_title("Rotating camera, 1000 boxes (xcb)")
    ax.margins(y=0.18)


def save(fig, name):
    for ext in ("png",):
        p = os.path.join(OUT, f"{name}.{ext}")
        fig.savefig(p, dpi=150, bbox_inches="tight", facecolor="white")
        print("wrote", p)


def main():
    # individual charts
    fig, ax = plt.subplots(figsize=(9, 5))
    chart_throughput(ax)
    save(fig, "perf_throughput")

    fig, ax = plt.subplots(figsize=(9, 5))
    chart_vorontest(ax)
    save(fig, "perf_vorontest")

    fig, ax = plt.subplots(figsize=(8, 5))
    chart_startup(ax)
    save(fig, "perf_startup")

    fig, ax = plt.subplots(figsize=(9, 5))
    chart_rotate(ax)
    save(fig, "perf_rotating_camera")

    # dashboard
    fig, axes = plt.subplots(2, 2, figsize=(19, 12))
    chart_throughput(axes[0][0])
    chart_rotate(axes[0][1])
    chart_vorontest(axes[1][0])
    chart_startup(axes[1][1])
    fig.suptitle("FreeCAD Vulkan fork vs upstream base — performance dashboard",
                 fontsize=16, fontweight="bold", y=1.005)
    save(fig, "perf_dashboard")


if __name__ == "__main__":
    main()
