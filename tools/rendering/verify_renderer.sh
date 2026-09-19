#!/usr/bin/env bash
# Renderer verification gate for the FreeCAD-vulkan fork.
#
# One command that runs the checks a renderer/Coin change must pass before it
# is considered done.  Every stage is independently selectable so a phase can
# run just the gate that matters for its change.
#
# Usage:
#   tools/rendering/verify_renderer.sh                 # abi + ctest (fast)
#   tools/rendering/verify_renderer.sh --build         # build unit, then gate
#   tools/rendering/verify_renderer.sh --all           # abi ctest preci suite
#   tools/rendering/verify_renderer.sh --preci         # pre-CI warnings gate
#   tools/rendering/verify_renderer.sh --suite         # GUI fcprobe vk_suite
#
# Environment:
#   BUILD_DIR   build tree to test            (default: build/debug)
#   JOBS        parallel build jobs           (default: nproc)
#   LOG_DIR     log/artifact output           (default: /tmp/opencode/verify)
#
# Exit status is non-zero if any selected stage failed.  A stage that cannot
# run (e.g. the Coin testsuite is not configured) is reported as SKIP with the
# command that would enable it -- never silently treated as a pass.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build/debug}"
JOBS="${JOBS:-$(nproc)}"
LOG_DIR="${LOG_DIR:-/tmp/opencode/verify}"
mkdir -p "$LOG_DIR"

STAGES=()
BUILD=0
for arg in "$@"; do
    case "$arg" in
        --build) BUILD=1 ;;
        --abi) STAGES+=(abi) ;;
        --ctest) STAGES+=(ctest) ;;
        --preci) STAGES+=(preci) ;;
        --suite) STAGES+=(suite) ;;
        --all) STAGES=(abi ctest preci suite) ;;
        -h|--help)
            cat <<'EOF'
Usage: verify_renderer.sh [--build] [--abi] [--ctest] [--preci] [--suite] [--all]

Stages (default: abi ctest):
  --build   build Coin FreeCADGui PartGui first (the IR-header ABI unit)
  --abi     ABI/symbol sentinel against tools/rendering/abi_baseline.json
  --ctest   tests registered in the main build (Vulkan/DrawList/Retained/CoinTests)
  --preci   pre-CI clang -Werror gate (pixi + Windows when configured)
  --suite   GUI fcprobe Vulkan suite (tools/fcprobe/vk_suite.json)
  --all     abi ctest preci suite

Environment:
  BUILD_DIR   build tree to test   (default: build/debug)
  JOBS        parallel build jobs  (default: nproc)
  LOG_DIR     log/artifact output  (default: /tmp/opencode/verify)

Exit status is non-zero if any selected stage failed.  A stage that cannot run
(e.g. the Coin testsuite is not configured) is reported as SKIP with the
command that would enable it -- never silently treated as a pass.
EOF
            exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done
if [ "${#STAGES[@]}" -eq 0 ]; then
    STAGES=(abi ctest)
fi

FAILED=()
SKIPPED=()
run_stage() {
    local name="$1"; shift
    echo "=== [$name] $* ==="
    if "$@"; then
        echo "--- [$name] OK"
    else
        echo "--- [$name] FAILED" >&2
        FAILED+=("$name")
    fi
}

stage_build() {
    local log="$LOG_DIR/build.log"
    echo "building Coin FreeCADGui PartGui (the IR-header ABI unit) ..."
    if ! make -C "$REPO/$BUILD_DIR" -j"$JOBS" Coin FreeCADGui PartGui \
            >"$log" 2>&1; then
        grep -iE 'error:|undefined reference|No such file' "$log" | head -40 >&2
        return 1
    fi
    if grep -qiE 'error:|undefined reference' "$log"; then
        grep -iE 'error:|undefined reference' "$log" | head -40 >&2
        return 1
    fi
    return 0
}

stage_abi() {
    python3 "$REPO/tools/rendering/abi_sentinel.py" --check
}

stage_ctest() {
    local cache="$REPO/$BUILD_DIR/CMakeCache.txt"
    local coin_tests=OFF
    [ -f "$cache" ] && coin_tests="$(sed -n 's/^COIN_BUILD_TESTS:BOOL=//p' "$cache")"
    local -a notests=(--no-tests=error)
    if [ "$coin_tests" != "ON" ]; then
        echo "NOTE: Coin testsuite is not built in $BUILD_DIR (COIN_BUILD_TESTS=OFF);"
        echo "      running only the main-build subset below."
        echo "      Enable the full suite with: cmake -S $REPO -B $REPO/$BUILD_DIR -DCOIN_BUILD_TESTS=ON"
        echo "      The 23 Vulkan backend tests + GL drawlist tests live there."
        SKIPPED+=("ctest:coin-testsuite (subset still runs)")
        # A minimal build (e.g. Vulkan off) can legitimately register none of
        # the subset, so an empty match is not a failure of this gate.
        notests=(--no-tests=ignore)
    fi
    # The tests that are registered in the main build (Vulkan selection, etc.).
    ctest --test-dir "$REPO/$BUILD_DIR" -R '^(Vulkan|DrawList|Retained|CoinTests)' \
          --output-on-failure "${notests[@]}"
}

stage_preci() {
    if [ ! -f "$REPO/build/preci/compile_commands.json" ]; then
        echo "SKIP: build/preci not configured (see tools/rendering/preci_check.py)."
        SKIPPED+=("preci")
        return 0
    fi
    local rc=0
    python3 "$REPO/tools/rendering/preci_check.py" --pixi || rc=1
    if [ -d "$REPO/build/xwin" ]; then
        python3 "$REPO/tools/rendering/preci_check.py" --windows || rc=1
    else
        echo "note: build/xwin absent, skipping the Windows /WX gate"
    fi
    return $rc
}

stage_suite() {
    if [ ! -x "$REPO/$BUILD_DIR/bin/FreeCAD" ]; then
        echo "SKIP: $BUILD_DIR/bin/FreeCAD not built."
        SKIPPED+=("suite")
        return 0
    fi
    python3 "$REPO/tools/fcprobe/freecad_probe.py" suite \
        --manifest "$REPO/tools/fcprobe/vk_suite.json" \
        --out "$LOG_DIR/runs"
}

if [ "$BUILD" -eq 1 ]; then
    run_stage build stage_build
fi
for s in "${STAGES[@]}"; do
    run_stage "$s" "stage_$s"
done

echo
echo "================ verification summary ================"
if [ "${#SKIPPED[@]}" -gt 0 ]; then
    printf 'SKIP: %s\n' "${SKIPPED[@]}"
fi
if [ "${#FAILED[@]}" -gt 0 ]; then
    printf 'FAIL: %s\n' "${FAILED[@]}"
    exit 1
fi
echo "PASS: ${STAGES[*]}"
