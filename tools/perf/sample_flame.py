#!/usr/bin/env python3
"""sample_flame - turn samplib's raw stack dumps into a CPU flame graph.

Reads the `<S addr..E>` samples and /proc maps captured by ``samplib.so``,
symbolizes every address to ``function`` + ``file:line`` via addr2line, folds
the per-sample call trees, and emits:

  * a self-contained SVG flame graph  (``--out flame.svg``)
  * a top-functions table             (printed to stdout unless ``--quiet``)

The stack order from backtrace() is innermost-first; the signal-handler /
trampoline frames are stripped (they live in samplib.so / libc).  The flame
graph follows the usual convention: the root (main) is at the bottom, deepest
callers on top, and rect widths are proportional to sampled CPU time.

Usage:
  python3 sample_flame.py --stacks S.txt --maps M.txt \\
      --out /tmp/opencode/raster_cpu.svg --title "RasterVulkan CPU"
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys


def parse_maps(path: str):
    """Return (base, module_path) list, sorted; base = lowest mapping per module."""
    ranges = []  # (start, end, base, module)
    bases = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"([0-9a-f]+)-([0-9a-f]+)\s+\S+\s+([0-9a-f]+)\s+\S+\s+\d+\s+(.+)",
                         line.strip())
            if not m:
                continue
            start, end, off, module = int(m.group(1), 16), int(m.group(2), 16), \
                int(m.group(3), 16), m.group(4).strip()
            if module.startswith("["):  # [vdso], [heap] etc: ignore for symbols
                continue
            if not os.path.exists(module):
                continue
            ranges.append((start, end, module))
            if off == 0:
                bases.setdefault(module, start)
    # For modules without a clean offset-0 base, use the lowest mapped start.
    low = {}
    for start, end, module in ranges:
        low[module] = min(low.get(module, start), start)
    ranges.sort()
    return ranges, low


def find_module(ranges, addr: int):
    import bisect
    starts = [r[0] for r in ranges]
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0:
        start, end, module = ranges[i]
        if start <= addr < end:
            return module
    return None


def symbolize(addr_to_module, low):
    """Resolve each address to 'func \t file:line' via addr2line (batched per module)."""
    by_mod = collections.defaultdict(list)
    for addr, module in addr_to_module.items():
        by_mod[module].append(addr)
    result = {}
    for module, addrs in by_mod.items():
        base = low.get(module, 0x0)
        offsets = [addr - base for addr in addrs]
        args = [f"0x{o:x}" for o in offsets]
        cmd = ["addr2line", "-f", "-C", "-e", module] + args
        try:
            out = subprocess.run(cmd, capture_output=True, text=True).stdout
        except Exception:
            out = ""
        lines = out.splitlines()
        for i, addr in enumerate(addrs):
            fn = lines[i * 2].strip() if i * 2 < len(lines) else "??"
            fl = lines[i * 2 + 1].strip() if i * 2 + 1 < len(lines) else "??"
            result[addr] = (fn or "??", fl or "??")
    return result


def parse_samples(path: str):
    samples, cur = [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line == "S":
                cur = []
            elif line == "E":
                if cur:
                    samples.append(cur)
                cur = []
            elif line.startswith("0x"):
                try:
                    cur.append(int(line, 16))
                except ValueError:
                    pass
    return samples


def top_frames(samples, syms, depth: int = 40):
    """Inclusive CPU time per (leaf, function, source-file, module)."""
    func = collections.Counter()      # function appears in a sample stack
    leaf = collections.Counter()      # innermost symbolizable frame
    src = collections.Counter()       # source file of any frame in a sample
    mod = collections.Counter()       # shared-object / module of any frame
    for s in samples:
        seen_f, seen_src, seen_mod = set(), set(), set()
        got_leaf = False
        for addr in s[:depth]:
            fn, fl = syms.get(addr, ("??", "??"))
            if fn == "??":
                continue
            if fn not in seen_f:
                seen_f.add(fn)
                func[fn] += 1
            file = fl.split(":")[0]
            if file and file != "??" and file not in seen_src:
                seen_src.add(file)
                src[file] += 1
            # module = path we already resolved addr->module for; recover by
            # using the dirname/file of the source (already tells the component)
            if not got_leaf:
                # leaf = first symbolizable frame (innermost) -> the burn site
                leaf[fn] += 1
                got_leaf = True
    return func, leaf, src, mod


def build_tree(samples, syms, max_depth: int):
    root = {"__count__": 0, "children": {}}
    for s in samples:
        root["__count__"] += 1
        node = root
        for addr in s[:max_depth]:
            fn, fl = syms.get(addr, ("??", "??"))
            key = (fn, fl)
            child = node["children"].get(key)
            if child is None:
                child = {"__count__": 0, "children": {}}
                node["children"][key] = child
            child["__count__"] += 1
            node = child
    return root


def _esc(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def flame_svg(root, title, width: int = 1400, row_h: int = 21, max_depth: int = 0):
    total = root["__count__"] or 1
    xscale = (width - 20) / total
    col = ["#7a92c4", "#a2c4e0", "#d0b7e0", "#e0c9a2", "#c4e0cf", "#e0a2a2",
           "#b0d0e0", "#c9a2e0", "#a2e0c4", "#e0b0b0"]
    svg_h = 26 + (max_depth + 1) * row_h + 8
    rects = []

    def render(node, depth, x):
        cnt = node["__count__"]
        w = max(cnt * xscale, 0.5)
        cx = x
        for c in node["children"].values():
            render(c, depth + 1, cx)
            cx += c["__count__"] * xscale
        if depth >= 0:
            fn, fl = ("(root)", "") if depth == 0 else next(
                iter(node["children"].keys()), ("(leaf)", ""))
            if depth > 0 and not node["children"]:
                fn, fl = "(leaf)", ""
            label = fn if len(fn) <= 34 else fn[:32] + ".."
            pct = 100.0 * cnt / total
            fill = col[depth % len(col)]
            y = 26 + (max_depth - depth) * row_h
            rect = (f'  <g class="frame" data-fn="{_esc(fn)}" '
                    f'data-file="{_esc(fl)}" data-count="{cnt}" '
                    f'data-pct="{pct:.1f}">\n'
                    f'    <rect x="{x + 1:.1f}" y="{y:.1f}" '
                    f'width="{max(w - 2, 1):.1f}" height="{row_h - 2:.1f}" rx="1" '
                    f'fill="{fill}" stroke="#3a4a66" stroke-width="0.4"/>\n'
                    f'    <title>{_esc(fn)} ({cnt} samples, {pct:.1f}%)</title>\n')
            if w > 74:
                rect += (f'    <text x="{x + 4:.1f}" y="{y + 14:.1f}" '
                         f'font-family="monospace" font-size="10" '
                         f'fill="#10243f">{_esc(label)}</text>\n')
            rects.append(rect + "  </g>")

    max_depth = 0

    def md(node):
        if not node["children"]:
            return 0
        return 1 + max(md(c) for c in node["children"].values())

    max_depth = md(root)
    render(root, 0, 10.0)
    rects.reverse()
    tip = ('  <div id="tip" style="position:fixed;pointer-events:none;'
           'background:#fff;border:1px solid #999;padding:2px 6px;'
           'font:11px monospace;display:none"></div>\n')
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{svg_h}" \
viewBox="0 0 {width} {svg_h}" font-family="sans-serif">
  <rect width="{width}" height="{svg_h}" fill="#f4f6fa"/>
  <text x="10" y="16" font-family="monospace" font-size="13" fill="#10243f">\
{_esc(title)}</text>
  <text x="{width - 10}" y="16" text-anchor="end" font-family="monospace" \
font-size="12" fill="#5a6a80">{total} samples</text>
  {tip}
  {'\n'.join(rects)}
  <script type="text/javascript"><![CDATA[
    (function() {{
      var tip = document.getElementById('tip');
      document.querySelectorAll('g.frame').forEach(function(g) {{
        g.addEventListener('mousemove', function(e) {{
          tip.style.display = 'block'; tip.style.left = (e.pageX + 12) + 'px';
          tip.style.top = (e.pageY + 12) + 'px';
          tip.textContent = g.getAttribute('data-fn') + '  —  ' +
            g.getAttribute('data-file') + '  (' + g.getAttribute('data-count') +
            ' samples, ' + g.getAttribute('data-pct') + '%)';
        }});
        g.addEventListener('mouseleave', function() {{ tip.style.display = 'none'; }});
      }});
    }})();
  ]]></script>
</svg>
"""


def _short_module(fl: str) -> str:
    base = os.path.basename(fl)
    return base


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="CPU flame graph from samplib stacks")
    p.add_argument("--stacks", required=True, help="FC_PROF_STACK_FILE output")
    p.add_argument("--maps", required=True, help="FC_PROF_MAPS_FILE output")
    p.add_argument("--out", default="/tmp/opencode/cpu_flame.svg")
    p.add_argument("--title", default="CPU flame graph")
    p.add_argument("--depth", type=int, default=40, help="max stack depth")
    p.add_argument("--top", type=int, default=15, help="top functions to print")
    p.add_argument("--focus", default="",
                   help="comma-separated substrings; keep frames whose source "
                        "file contains ANY of them")
    p.add_argument("--quiet", action="store_true")
    args = p.parse_args(argv)

    ranges, low = parse_maps(args.maps)
    samples = parse_samples(args.stacks)
    if not samples:
        print("no samples", file=sys.stderr)
        return 1

    # Build (addr -> module) set first, strip sampler/trampoline frames.
    addr_to_module = {}
    for s in samples:
        for addr in s:
            mod = find_module(ranges, addr)
            if mod and os.path.basename(mod) in ("samplib.so", "libc.so.6",
                                                 "ld-linux-x86-64.so.2"):
                continue  # skip signal-handler/trampoline frames
            if mod:
                addr_to_module.setdefault(addr, mod)
    syms = symbolize(addr_to_module, low)

    focus = [s for s in args.focus.split(",") if s]

    def keep(frame_addr):
        fn, fl = syms.get(frame_addr, ("??", "??"))
        if not focus:
            return True
        return any(f in fl for f in focus)

    # Rebuild per-sample stacks without the stripped frames.
    stripped = []
    for s in samples:
        fs = [a for a in s if a in syms and keep(a)]
        if fs:
            stripped.append(fs)
    if not stripped:
        print("no symbolizable frames", file=sys.stderr)
        return 1

    func, leaf, src, mod = top_frames(stripped, syms, args.depth)
    n = len(stripped)
    if not args.quiet:
        print(f"# {n} samples.  Innermost (CPU-burn) functions by self time:")
        for fn, c in leaf.most_common(args.top):
            print(f"   {100.0 * c / n:5.1f}%  {fn}")
        print("\n# Inclusive CPU share by function (appears in stack):")
        for fn, c in func.most_common(args.top):
            print(f"   {100.0 * c / n:5.1f}%  {fn}")
        print("\n# Inclusive CPU share by source file:")
        for fl, c in src.most_common(16):
            print(f"   {100.0 * c / n:5.1f}%  {fl}")

    tree = build_tree(stripped, syms, args.depth)

    def depth_of(node):
        if not node["children"]:
            return 0
        return 1 + max(depth_of(c) for c in node["children"].values())

    svg = flame_svg(tree, args.title, max_depth=depth_of(tree))
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(svg)
    if not args.quiet:
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
