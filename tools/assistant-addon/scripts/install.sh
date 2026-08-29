#!/usr/bin/env bash
# install.sh - symlink the Assistant addon into FreeCAD's *user* Mod directory.
#
# FreeCAD keeps its user data in a versioned dir (e.g. ~/.local/share/FreeCAD/v26-3)
# on this build, so we resolve the real path from FreeCADCmd when available.
#
# Usage:   scripts/install.sh [--copy]
#          --copy   copy the addon instead of symlinking (frozen snapshot)
#          --bin PATH   use a specific FreeCAD/FreeCADCmd binary for path resolution
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ADDON_SRC="$HERE/../Assistant"

FCCMD="${FREECADCMD_BIN:-}"
if [[ -z "$FCCMD" ]]; then
    for c in "$HOME"/dev/FreeCAD/build/debug/bin/FreeCADCmd; do
        [[ -x "$c" ]] && FCCMD="$c" && break
    done
fi

RESOLVE=""
if [[ -n "$FCCMD" && -x "$FCCMD" ]]; then
    TMPPY="$(mktemp --suffix=.py)"
    printf 'import FreeCAD as App\nprint("APPDATA="+App.getUserAppDataDir())\n' > "$TMPPY"
    RESOLVE="$(LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/tmp/opencode/boost91}" QT_QPA_PLATFORM=offscreen \
        "$FCCMD" "$TMPPY" 2>/dev/null | grep -i APPDATA | tail -1 | cut -d= -f2)" || true
    rm -f "$TMPPY"
fi

if [[ -n "$RESOLVE" ]]; then
    MOD_DIR="$RESOLVE/Mod"
else
    # fallback: newest versioned dir, else the shared dir
    MOD_DIR="$(ls -dt "$HOME"/.local/share/FreeCAD/v*/Mod "$HOME"/.local/share/FreeCAD/Mod 2>/dev/null | head -1)"
    [[ -n "$MOD_DIR" ]] || MOD_DIR="$HOME/.local/share/FreeCAD/Mod"
fi

echo "[install] addon source: $ADDON_SRC"
echo "[install] resolved user Mod dir: $MOD_DIR"
mkdir -p "$MOD_DIR"

TARGET="$MOD_DIR/Assistant"
rm -rf "$TARGET"
if [[ "${1:-}" == "--copy" ]]; then
    cp -r "$ADDON_SRC" "$TARGET"
    echo "[install] copied -> $TARGET"
else
    ln -s "$ADDON_SRC" "$TARGET"
    echo "[install] symlinked -> $TARGET"
fi

if [[ -f "$HERE/sync_guest.py" ]]; then
    echo "[install] refresh vendored tools..."
    python3 "$HERE/sync_guest.py" || true
    python3 "$HERE/gen_tool_schemas.py" 2>/dev/null || true
fi
echo "[install] done. Restart FreeCAD and pick the 'Assistant' workbench."
