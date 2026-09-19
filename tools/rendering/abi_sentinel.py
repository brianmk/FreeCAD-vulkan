#!/usr/bin/env python3
"""ABI/symbol sentinel for the renderer public surface.

Refactors that touch the retained IR or the backend interface can change the
layout of structs that cross the Coin <-> FreeCADGui <-> PartGui boundary (a
real ABI break: "compiles as one TU, misbehaves when linked").  This script
captures a fingerprint of that surface so an accidental break fails loudly and
deliberately.

Two independent fingerprints:

  symbols  exported dynamic symbols of libCoin.so, filtered to the renderer
           prefixes and hashed by qualified name.  Catches removed/renamed
           public entry points.  The argument list is stripped, so a new
           overload on an existing name is NOT caught.
  layout   sizeof() of the public IR/backend PODs, compiled against the real
           headers.  Catches a change in total size (added/removed members, or
           a member whose own size changes).  It does NOT catch a same-size
           reorder or a same-size type swap; that would need per-member
           offsets rather than sizeof().

The library identity (basename) is pinned: a missing libCoin.so fails --check
loudly rather than passing, and a renamed lib is caught.  The build-dir path is
recorded for diagnostics but not compared, so the gate runs against any build
tree (build/debug, build/ci, build/preci, ...); pass --lib to point at a
specific one.

Usage:
  python3 tools/rendering/abi_sentinel.py --capture   # write the baseline
  python3 tools/rendering/abi_sentinel.py --check     # compare (CI/local gate)
  python3 tools/rendering/abi_sentinel.py --check --lib build/preci/lib/libCoin.so

Exit status is non-zero when --check finds a difference, so it composes with
the verify_renderer.sh gate.  A deliberate change is accepted by re-capturing
and committing the new baseline in the same PR.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE = os.path.join(REPO, "tools/rendering/abi_baseline.json")

# Public renderer symbols worth pinning.  Deliberately prefix-based so a new
# member function on an existing class is caught, but an unrelated upstream
# change to the rest of Coin is not.
SYMBOL_PREFIXES = (
    "SoRenderIR",
    "SoIRRenderAction",
    "SoDrawList",
    "SoRenderBackend",
    "SoRenderParams",
    "SoVulkan",
    "SoRTX",
)

# Public PODs whose layout crosses the module boundary.  Keep in sync with the
# structs in Inventor/rendering/SoRenderIR.h + SoRenderBackend.h.
LAYOUT_TYPES = (
    "SoGeometryDesc",
    "SoTextureData",
    "SoMaterialData",
    "SoRenderState",
    "SoRenderCommand",
    "SoLightData",
    "SoLightingData",
    "SoLightingBlock",
    "SoRenderParams",
    "SoRenderBackendInitParams",
)


def find_libcoin(override=None):
    if override:
        return override if os.path.exists(override) else None
    for rel in ("build/debug/lib/libCoin.so", "build/ci/lib/libCoin.so",
                "build/preci/lib/libCoin.so"):
        p = os.path.join(REPO, rel)
        if os.path.exists(p):
            return p
    # Fall back to a glob over any build dir; sort so the choice is
    # deterministic when several build trees exist.
    import glob
    hits = sorted(glob.glob(os.path.join(REPO, "build/*/lib/libCoin.so*")))
    return hits[0] if hits else None


def lib_identity(lib):
    """The comparable part of a libCoin path.

    Pin the library *identity* (basename), not its build-dir-specific path:
    the ABI surface is what matters and the same libCoin.so lives under
    build/debug, build/ci, build/preci, ...  The full path is recorded
    separately for diagnostics and is deliberately NOT compared, so the gate
    is not locked to whichever build tree happened to capture the baseline.
    """
    return os.path.basename(lib) if lib else None


def symbol_fingerprint(lib):
    # -C demangles, so the prefix filter matches C++ qualified names rather
    # than raw _Z... manglings.  Version suffixes (@@COIN_...) are stripped.
    out = subprocess.run(["nm", "-D", "--defined-only", "-C", lib],
                         capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        name = parts[2].split("@")[0]
        # Keep the qualified name up to the argument list, so volatile
        # parameter-type spelling is ignored.  A new overload on an existing
        # name therefore does NOT change the fingerprint.
        name = re.split(r"\(", name, maxsplit=1)[0].strip()
        if name.startswith(SYMBOL_PREFIXES):
            names.add(name)
    ordered = sorted(names)
    digest = hashlib.sha256("\n".join(ordered).encode()).hexdigest()
    return {"count": len(ordered), "sha256": digest}


def include_flags():
    """Reuse the recorded compile command of a Coin rendering TU for includes."""
    db = os.path.join(REPO, "build/debug/compile_commands.json")
    if not os.path.exists(db):
        return None, None
    with open(db) as fh:
        entries = json.load(fh)
    entry = None
    for e in entries:
        if "3rdParty/coin/src/rendering/" in e["file"]:
            entry = e
            break
    if entry is None:
        return None, None
    flags = []
    toks = shlex.split(entry["command"])
    i = 0
    while i < len(toks):
        tok = toks[i]
        if tok in ("-I", "-isystem", "-D", "-include"):
            # Two-token form: keep the flag and its value.
            if i + 1 < len(toks):
                flags.extend([tok, toks[i + 1]])
                i += 2
                continue
        elif tok.startswith(("-I", "-D", "-std=", "-fPIC", "-fvisibility")):
            flags.append(tok)
        i += 1
    return flags, entry["directory"]


def layout_fingerprint(lib):
    if lib is None:
        return None, "libCoin.so not found (build first)"
    flags, cwd = include_flags()
    if flags is None:
        return None, "no compile_commands.json for include flags"
    body = ["#include <Inventor/rendering/SoRenderIR.h>",
            "#include \"rendering/SoRenderBackend.h\"",
            "#include <cstdio>",
            "int main() {"]
    for t in LAYOUT_TYPES:
        body.append(f'  std::printf("{t} %zu\\n", sizeof({t}));')
    body.append("  return 0;")
    body.append("}")
    src = "\n".join(body) + "\n"
    with tempfile.TemporaryDirectory() as tmp:
        cpp = os.path.join(tmp, "abi.cpp")
        exe = os.path.join(tmp, "abi")
        with open(cpp, "w") as fh:
            fh.write(src)
        cmd = ["c++", "-std=c++17", *flags, cpp, "-o", exe, "-lCoin",
               f"-L{os.path.dirname(lib)}", f"-Wl,-rpath,{os.path.dirname(lib)}"]
        r = subprocess.run(cmd, cwd=cwd or tmp, capture_output=True, text=True)
        if r.returncode != 0:
            return None, f"layout probe failed to compile: {r.stderr.strip()[:300]}"
        run = subprocess.run([exe], capture_output=True, text=True)
        if run.returncode != 0:
            return None, f"layout probe failed to run: {run.stderr.strip()[:300]}"
    sizes = {}
    for line in run.stdout.splitlines():
        name, _, value = line.partition(" ")
        sizes[name] = int(value)
    return sizes, None


def collect(lib_override=None):
    lib = find_libcoin(lib_override)
    result = {
        # Compared: the library identity (basename).
        "lib": lib_identity(lib),
        # Informational only: where it was found (not compared).
        "lib_path": os.path.relpath(lib, REPO) if lib else None,
    }
    if lib:
        result["symbols"] = symbol_fingerprint(lib)
    else:
        result["symbols"] = None
        result["symbols_error"] = "libCoin.so not found (build first)"
    layout, err = layout_fingerprint(lib)
    result["layout"] = layout
    if err:
        result["layout_error"] = err
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", action="store_true", help="write the baseline")
    ap.add_argument("--check", action="store_true", help="compare against baseline")
    ap.add_argument("--lib", metavar="PATH",
                    help="libCoin.so to inspect (default: auto-detect under build/)")
    args = ap.parse_args()
    if not (args.capture or args.check):
        ap.error("pass --capture or --check")

    current = collect(args.lib)
    if args.capture:
        missing = [k for k in ("symbols", "layout") if current.get(k) is None]
        if missing:
            print("refusing to capture an incomplete baseline; fingerprint(s) "
                  "unavailable:", file=sys.stderr)
            for k in missing:
                print(f"  - {k}: {current.get(k + '_error', 'unavailable')}",
                      file=sys.stderr)
            return 2
        with open(BASELINE, "w") as fh:
            json.dump(current, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print(f"captured baseline -> {os.path.relpath(BASELINE, REPO)}")
        print(json.dumps(current, indent=2, sort_keys=True))
        return 0

    if not os.path.exists(BASELINE):
        print("no baseline; run --capture first", file=sys.stderr)
        return 2
    with open(BASELINE) as fh:
        baseline = json.load(fh)

    failures = []

    # The library identity (basename) is part of the pinned surface: a
    # renamed/replaced lib must not pass silently.  The build-dir path is
    # informational only, so the gate runs against any build tree.
    if lib_identity(baseline.get("lib")) != lib_identity(current.get("lib")):
        failures.append(
            f"lib changed: {lib_identity(baseline.get('lib'))} -> "
            f"{lib_identity(current.get('lib'))}")

    for key in ("symbols", "layout"):
        old, new = baseline.get(key), current.get(key)
        if new is None:
            reason = current.get(f"{key}_error", "fingerprint unavailable")
            failures.append(f"{key} fingerprint could not be computed: {reason}")
            continue
        if old is None:
            failures.append(
                f"{key} fingerprint is new (absent from baseline); re-capture "
                "tools/rendering/abi_baseline.json")
            continue
        if old == new:
            continue
        if key == "symbols":
            if old.get("sha256") != new.get("sha256"):
                failures.append(
                    f"symbol set changed: {old.get('count')} -> {new.get('count')} "
                    f"({old.get('sha256', '')[:12]} -> {new.get('sha256', '')[:12]})")
        elif key == "layout":
            for name in sorted(set(old) | set(new)):
                if old.get(name) != new.get(name):
                    failures.append(
                        f"sizeof({name}) changed: {old.get(name)} -> {new.get(name)}")

    if failures:
        print("ABI sentinel FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        print("If the change is intentional, re-run --capture and commit "
              "tools/rendering/abi_baseline.json in the same PR.", file=sys.stderr)
        return 1
    print("ABI sentinel OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
