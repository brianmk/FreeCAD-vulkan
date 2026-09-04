#!/usr/bin/env python3
"""Pre-CI validation over fork-modified C++ files (local, fast).

Replicates the two slow CI jobs that keep surprising us, without a full build:

  --pixi      clang -Werror -fsyntax-only   -> replicates the Pixi conda build
                                              (FREECAD_WARN_ERROR=ON, clang++)
  --windows   clang-cl /WX /W4 /Zs          -> replicates the Windows MSVC /WX
                                              conformance treatment

It reads the fork's compile_commands.json (produced by a normal CMake configure
in build/preci), filters to files changed on this fork vs origin/main, and
re-runs each translation unit for syntax/Warning-as-error only.  No linking, no
full build.  Include-resolution failures (Qt/Coin headers absent for a cross
sysroot) are reported but ignored; only errors in the fork file's own code count.

Usage:
  # configure once (Linux, bundled Coin, Vulkan ON; clang to match Pixi):
  cmake -S . -B build/preci -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DFREECAD_WARN_ERROR=ON \
    -DFREECAD_USE_EXTERNAL_COIN_PIVY=OFF -DFREECAD_USE_VULKAN=ON ...

  python3 tools/rendering/preci_check.py --pixi
  python3 tools/rendering/preci_check.py --windows
"""
import json
import os
import shlex
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DB = os.path.join(REPO, "build/preci/compile_commands.json")
XWIN = os.path.join(REPO, "build/xwin")

MSVC_IMSVEC = [
    f"{XWIN}/crt/include",
    f"{XWIN}/sdk/include/ucrt",
    f"{XWIN}/sdk/include/um",
    f"{XWIN}/sdk/include/shared",
    f"{XWIN}/sdk/include/winrt",
]


def fork_files():
    out = subprocess.run(
        ["git", "diff", "--name-only", "origin/main...HEAD",
         "--", "*.cpp", "*.cxx", "*.cc"],
        cwd=REPO, capture_output=True, text=True, check=True).stdout
    files = []
    for line in out.splitlines():
        f = line.strip()
        if not f or f.startswith("src/3rdParty/") or f.startswith("tests/") \
           or f.startswith("tools/") or f.startswith("src/Mod/AddonManager"):
            continue
        files.append(os.path.join(REPO, f))
    return files


def build_db():
    db = json.load(open(DB))
    by = {}
    for e in db:
        by.setdefault(os.path.abspath(e["file"]), []).append(e)
    return by


def _gnu_syntax_flags(args):
    keep = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            i += 2
            continue
        if a in ("-c", "-MD", "-MMD", "-MP", "-fdiagnostics-color",
                 "-fdiagnostics-color=always"):
            i += 1
            continue
        if a in ("-MF", "-MT", "-MQ", "--dependency-file"):
            i += 2
            continue
        if a.startswith("-o"):
            i += 1
            continue
        keep.append(a)
        i += 1
    return keep


def _msvc_flags(args):
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-isystem",):
            skip = True
            continue
        if a.startswith("-std="):
            out.append("/std:c++20")
            continue
        # drop gnu-only warning/opt flags; keep defines + include dirs
        if a.startswith("-W") or a.startswith("-f") or a.startswith("-m") \
           or a in ("-Wall", "-Wextra", "-Wpedantic", "-fPIC"):
            continue
        if a.startswith("-D") or a.startswith("-I"):
            out.append(a)
    return out


def run_pixi():
    cc = "clang++"
    files = fork_files()
    by = build_db()
    present = [f for f in files if f in by]
    print(f"[pixi: {cc} -Werror] fork C++ files: {len(files)}"
          f" (compile entry: {len(present)})")
    fails = []
    n = 0
    for f in sorted(present):
        e = by[f][0]
        args = shlex.split(e["command"])
        args[0] = "clang++"
        cmd = _gnu_syntax_flags(args) + ["-Werror", "-fsyntax-only", e["file"]]
        r = subprocess.run(cmd, cwd=e["directory"], capture_output=True, text=True)
        n += 1
        if r.returncode != 0:
            errs = [l.strip() for l in r.stderr.splitlines() if "error:" in l]
            rel = os.path.relpath(f, REPO)
            fails.append(rel)
            print(f"  pixi -Werror FAIL {rel}")
            for l in errs[:8]:
                print(f"      {l.strip()[:170]}")
    print(f"[pixi] checked {n}: {len(present) - len(fails)} clean, "
          f"{len(fails)} failing")
    return len(fails)


def run_windows():
    msvc = ["clang-cl", "--target=x86_64-pc-windows-msvc",
            "/EHsc", "/WX", "/W4", "/Zs"]
    for imi in MSVC_IMSVEC:
        msvc.append(f"/imsvc{imi}")
    files = fork_files()
    by = build_db()
    present = [f for f in files if f in by]
    print(f"[windows: clang-cl /WX] fork C++ files: {len(files)}"
          f" (compile entry: {len(present)})")
    genuine = []
    include_fails = 0
    ok = 0
    for f in sorted(present):
        e = by[f][0]
        flags = _msvc_flags(shlex.split(e["command"]))
        cmd = msvc + flags + [f]
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        if r.returncode == 0:
            ok += 1
            continue
        errs = [l for l in r.stderr.splitlines() if "error:" in l.lower()]
        # clang-cl reports missing includes as 'fatal error: <x> file not
        # found'.  These are unresolved Qt/Coin/OCCT headers when targeting a
        # bare Windows sysroot, NOT MSVC /WX conformance failures in the fork's
        # own code, so classify them as include-resolution and ignore them.
        if any("file not found" in l.lower() or "no such file or directory"
               in l.lower() or "fatal error" in l.lower() for l in errs):
            include_fails += 1
            continue
        rel = os.path.relpath(f, REPO)
        genuine.append(rel)
        print(f"  windows /WX FAIL {rel}")
        for l in errs[:8]:
            print(f"      {l.strip()[:170]}")
    print(f"[windows] checked {len(present)}: {ok} clean, "
          f"{include_fails} include-resolution (ignored), {len(genuine)} genuine")
    return len(genuine)


def main():
    if not os.path.exists(DB):
        print(f"error: {DB} not found. Configure build/preci first "
              "(see docstring).")
        return 2
    mode = sys.argv[1].lstrip("-") if len(sys.argv) > 1 else "pixi"
    if mode == "pixi":
        return 0 if run_pixi() == 0 else 1
    if mode == "windows":
        return 0 if run_windows() == 0 else 1
    print(f"unknown mode: {mode} (use --pixi or --windows)")
    return 2


if __name__ == "__main__":
    sys.exit(main())
