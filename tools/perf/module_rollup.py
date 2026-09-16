#!/usr/bin/env python3
"""module_rollup - per-module / per-submodule CPU attribution from samplib stacks.

Companion to ``tools/perf/sample_flame.py``.  Where ``sample_flame`` folds the
sampled call trees into a flame graph and a per-function/per-file top list,
this tool rolls the *same* samples up to FreeCAD's component layout so you can
answer "what does each module and submodule cost at runtime":

  * ``module``    inclusive + self share per top-level module
                  (``Mod/Part``, ``Mod/Sketcher``, ``Base``, ``App``, ``Gui``,
                  ``3rdParty/coin``, ``python``, ``qt``, ``gpu-driver``, ...)
  * ``submodule`` inclusive + self share one level deeper
                  (``Mod/Part/App`` vs ``Mod/Part/Gui``, ``3rdParty/<lib>``, ...)
  * ``functions`` top self-time (CPU-burn) functions

Inclusive = the module appears anywhere in the sampled stack (time spent in or
under the module).  Self = the innermost symbolizable frame is in the module
(the actual burn site).

The input is the pair of files written by ``samplib.so`` (``FC_PROF_STACK_FILE``
/ ``FC_PROF_MAPS_FILE``).  ``--sections`` selects which tables to print, so a
caller can ask for just modules, just functions, etc.

Examples
--------
  # full rollup of a captured run
  python3 tools/perf/module_rollup.py --stacks /tmp/opencode/prof/startup.stacks \\
      --maps /tmp/opencode/prof/startup.maps

  # only the per-module table, more rows, restricted to Part/Sketcher frames
  python3 tools/perf/module_rollup.py --stacks s.txt --maps m.txt \\
      --sections module --top 40 --focus src/Mod/Part,src/Mod/Sketcher
"""

from __future__ import annotations

import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sample_flame as sf  # noqa: E402

ALL_SECTIONS = ("module", "submodule", "functions")

_MOD_RE = re.compile(r"/src/Mod/([^/]+)/([^/]+)/")
_PARTY_RE = re.compile(r"/src/3rdParty/([^/]+)/")
_TOP_RE = re.compile(r"/src/(Base|App|Gui)/")


def _lib_class(path: str) -> str:
    p = path.lower()
    base = os.path.basename(p)
    if base.startswith("libpython") or base.startswith("python") or "/python" in p:
        return "python"
    if base.startswith("libqt") or base.startswith("qt") or "/qt" in p:
        return "qt"
    if any(k in p for k in ("libgl", "vulkan", "nvidia", "amdvlk", "radeon",
                            "libegl", "mesa", "swrast", "libx11", "libxcb",
                            "libllvm")):
        return "gpu-driver"
    return "(system)"


def _tree_module(path: str, marker: str):
    """Classify a path by the FreeCAD tree layout after ``marker`` (e.g. /src/).

    Handles both ``src/Mod/Part/App/Foo.cpp`` and ``src/Mod/Part/Foo.cpp``,
    and ``build/debug/Mod/Part/Part.so``.
    """
    idx = path.find(marker)
    if idx < 0:
        return None
    parts = [p for p in path[idx + len(marker):].split("/") if p]
    if not parts:
        return None
    if parts[0] == "Mod" and len(parts) >= 2:
        mod = parts[1]
        sub = parts[2] if len(parts) >= 3 else mod
        return ("Mod/%s" % mod, "Mod/%s/%s" % (mod, sub))
    if parts[0] == "3rdParty" and len(parts) >= 2:
        return ("3rdParty/%s" % parts[1], "3rdParty/%s" % parts[1])
    if parts[0] in ("Base", "App", "Gui"):
        return (parts[0], parts[0])
    return None


def _src_module(path: str):
    low = path.lower()
    if "/eigen3/" in low or "/eigen/" in low:
        m = re.search(r"/Eigen/(?:src/)?([^/]+)/", path)
        sub = m.group(1) if m else "Eigen"
        return ("Eigen", "Eigen/%s" % sub)
    if "/opencascade/" in low or "/occt/" in low:
        return ("OCCT", "OCCT")
    for marker in ("/src/", "/build/"):
        mm = _tree_module(path, marker)
        if mm:
            return mm
    if "/src/" in path:
        parts = [p for p in path.split("/src/", 1)[1].split("/") if p]
        if parts:
            return ("src/%s" % parts[0], "src/%s" % parts[0])
        return ("src/other", "src/other")
    if path.startswith("/usr/") or path.startswith("/lib") or path.startswith("/opt/"):
        return (_lib_class(path), "(system)")
    return None


def _so_module(so: str):
    """Classify a frame by its shared object when the source file is unknown."""
    base = os.path.basename(so)
    if "/Mod/" in so:
        parts = [p for p in so.split("/Mod/", 1)[1].split("/") if p]
        if parts:
            return ("Mod/%s" % parts[0], "Mod/%s/%s" % (parts[0], parts[0]))
    if base.startswith("libTK") or "opencascade" in so.lower():
        return ("OCCT", "OCCT")
    if base.startswith("libFreeCAD"):
        stem = base[len("libFreeCAD"):].split(".so")[0]
        if stem in ("Base", "App", "Gui"):
            return (stem, stem)
        return ("FreeCAD/%s" % stem, "FreeCAD/%s" % stem)
    if base.startswith("FreeCAD"):
        return ("FreeCAD", "FreeCAD")
    if base.startswith("libpython") or base.startswith("python"):
        return ("python", "python")
    if base.startswith("libQt") or base.startswith("Qt"):
        return ("qt", "qt")
    if any(k in base.lower() for k in (
            "libgl", "vulkan", "nvidia", "amdvlk", "radeon", "libegl",
            "mesa", "swrast", "libx11", "libxcb", "libllvm")):
        return ("gpu-driver", "gpu-driver")
    if so.startswith("/usr/") or so.startswith("/lib") or so.startswith("/opt/"):
        return ("(system)", "(system)")
    return ("(other)", "(other)")


def module_of(src: str, so: str = ""):
    """Return (module, submodule) for a frame's source file and/or library."""
    if src and src != "??":
        mm = _src_module(src)
        if mm:
            return mm
        if "/build/" in src:
            return ("(generated)", "(generated)")
    if so:
        return _so_module(so)
    return ("(other)", "(other)")


def parse_sections(value: str) -> list[str]:
    if not value or value.strip() == "all":
        return list(ALL_SECTIONS)
    wanted = [s.strip() for s in value.split(",") if s.strip()]
    bad = [s for s in wanted if s not in ALL_SECTIONS]
    if bad:
        raise SystemExit("unknown section(s): %s (known: %s)"
                         % (", ".join(bad), ", ".join(ALL_SECTIONS)))
    return wanted


def rollup(stacks: str, maps: str, depth: int, focus: list[str]):
    ranges, low = sf.parse_maps(maps)
    samples = sf.parse_samples(stacks)
    if not samples:
        return None

    addr_to_module = {}
    for s in samples:
        for addr in s:
            mod = sf.find_module(ranges, addr)
            if mod and os.path.basename(mod) in (
                    "samplib.so", "libc.so.6", "ld-linux-x86-64.so.2"):
                continue
            if mod:
                addr_to_module.setdefault(addr, mod)
    syms = sf.symbolize(addr_to_module, low)

    incl = collections.Counter()
    selfc = collections.Counter()
    incl_sub = collections.Counter()
    self_sub = collections.Counter()
    funcs = collections.Counter()
    n = 0
    for s in samples:
        mods, subs, got_leaf, any_frame = set(), set(), False, False
        for addr in s[:depth]:
            fn, fl = syms.get(addr, ("??", "??"))
            if fn == "??":
                continue
            src = fl.split(":")[0]
            if focus and not any(f in src for f in focus):
                continue
            any_frame = True
            mm = module_of(src, addr_to_module.get(addr, ""))
            if mm:
                mods.add(mm[0])
                subs.add(mm[1])
            if not got_leaf:
                selfc[mm[0] if mm else "(other)"] += 1
                self_sub[mm[1] if mm else "(other)"] += 1
                funcs[fn] += 1
                got_leaf = True
        if not any_frame:
            continue
        n += 1
        for mm in mods:
            incl[mm] += 1
        for mm in subs:
            incl_sub[mm] += 1
    return n, incl, selfc, incl_sub, self_sub, funcs


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Per-module CPU attribution from samplib stacks")
    p.add_argument("--stacks", required=True, help="FC_PROF_STACK_FILE output")
    p.add_argument("--maps", required=True, help="FC_PROF_MAPS_FILE output")
    p.add_argument("--depth", type=int, default=60, help="max stack depth")
    p.add_argument("--top", type=int, default=25, help="rows per table")
    p.add_argument("--sections", default="all",
                   help="comma list: %s (or 'all')" % ",".join(ALL_SECTIONS))
    p.add_argument("--focus", default="",
                   help="comma-separated substrings; keep frames whose source "
                        "file contains ANY of them")
    a = p.parse_args(argv)

    sections = parse_sections(a.sections)
    focus = [s for s in a.focus.split(",") if s]
    got = rollup(a.stacks, a.maps, a.depth, focus)
    if not got:
        print("no samples in %s" % a.stacks, file=sys.stderr)
        return 1
    n, incl, selfc, incl_sub, self_sub, funcs = got

    title = os.path.basename(a.stacks)
    if focus:
        title += "  [focus: %s]" % a.focus
    print("== %s : %d samples ==" % (title, n))

    if "module" in sections:
        print("\n# INCLUSIVE CPU share by module (module in stack)")
        for k, c in incl.most_common(a.top):
            print("  %6.2f%%  %s" % (100.0 * c / n, k))
        print("\n# SELF CPU share by module (innermost frame in module)")
        for k, c in selfc.most_common(a.top):
            print("  %6.2f%%  %s" % (100.0 * c / n, k))

    if "submodule" in sections:
        print("\n# INCLUSIVE CPU share by submodule")
        for k, c in incl_sub.most_common(a.top):
            print("  %6.2f%%  %s" % (100.0 * c / n, k))
        print("\n# SELF CPU share by submodule")
        for k, c in self_sub.most_common(a.top):
            print("  %6.2f%%  %s" % (100.0 * c / n, k))

    if "functions" in sections:
        print("\n# Top self (CPU-burn) functions")
        for k, c in funcs.most_common(a.top):
            print("  %6.2f%%  %s" % (100.0 * c / n, k))
    return 0


if __name__ == "__main__":
    sys.exit(main())
