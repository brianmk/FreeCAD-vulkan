#!/usr/bin/env python3
"""Static validator for View3DSettings preference coverage.

The View3DSettings dispatch historically enumerated pref keys in two places that
could drift (the OnChange if/else and the applySettings bootstrap).  After the
data-driven rewrite there is one table; this tool checks that the table covers
exactly the original behavior set, so no pref is dropped, mis-routed or
mis-grouped on the rewrite.

Self-contained: the expected key set (the 55 keys the old `strcmp` dispatch
handled + the 5 background keys the old `else` catch-all handled) is embedded
here, so validation works on a fresh checkout without a generated baseline.

Usage:
  vk_viewsettings_keys.py --new     # validate the data-driven table (post-rewrite)
  vk_viewsettings_keys.py --list    # print the expected key set (reference)
"""
import re
import sys

SRC = "src/Gui/View3DSettings.cpp"

# The 14 LightSources sub-group keys (grouped into the sub-grp).
LIGHTS = {
    "EnableHeadlight", "HeadlightColor", "HeadlightDirection", "HeadlightIntensity",
    "EnableBacklight", "BacklightColor", "BacklightDirection", "BacklightIntensity",
    "EnableFillLight", "FillLightColor", "FillLightDirection", "FillLightIntensity",
    "AmbientLightColor", "AmbientLightIntensity",
}
# Keys handled by the background catch-all (`else`) rather than a strcmp branch.
BG = {
    "BackgroundColor", "BackgroundColor2", "BackgroundColor3", "BackgroundColor4",
    "UseBackgroundColorMid",
}
# The keys the original OnChange strcmp dispatch handled (55).
MAIN_STRCMP = {
    "AmbientLightColor", "AmbientLightIntensity", "AxisLetterColor", "AxisXColor",
    "AxisYColor", "AxisZColor", "BacklightColor", "BacklightDirection",
    "BacklightIntensity", "CornerCoordSystem", "CornerCoordSystemSize",
    "Dimensions3dVisible", "DimensionsDeltaVisible", "DimensionsVisible",
    "EnableBacklight", "EnableFillLight", "EnableHeadlight", "EnablePreselection",
    "EnableSelection", "EyeDistance", "FillLightColor", "FillLightDirection",
    "FillLightIntensity", "Gradient", "GroundPlaneOpacity", "HeadlightColor",
    "HeadlightDirection", "HeadlightIntensity", "HighlightColor", "InvertZoom",
    "MaxFrameRate", "NavigationStyle", "OrbitStyle", "Orthographic", "PickRadius",
    "PickRadiusScale", "PreselectionMessageRate", "RadialGradient", "RenderCache",
    "ResetCursorPosition", "RotationMode", "SelectionColor", "Sensitivity",
    "ShowAxisCross", "ShowFPS", "ShowGroundPlane", "ShowNaviCube",
    "TransparentObjectRenderType", "UseNavigationAnimations", "UseSpinningAnimations",
    "UseVBO", "UseVulkanRayTracing", "UseVulkanRenderer", "ZoomAtCursor", "ZoomStep",
}

EXPECTED = MAIN_STRCMP | BG  # 60 explicit keys


def _block(src, sig):
    i = src.index(sig)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return src[i:]


def new_table(src):
    body = _block(src, "View3DSettings::ensurePrefTable()")
    return set(re.findall(r'add\(m_prefTable,\s*"([^"]+)",\s*(true|false),\s*\[', body))


def validate_new():
    src = open(SRC).read()
    if "ensurePrefTable" not in src:
        print("RESULT: FAIL -- no ensurePrefTable() yet (rewrite not applied)")
        return 1
    entries = new_table(src)
    names = {n for n, _ in entries}
    light_entries = {n for n, l in entries if l == "true"}
    missing = EXPECTED - names
    unexpected = names - EXPECTED
    light_ok = light_entries == LIGHTS
    print("table entries:", len(entries))
    print("  missing from table:", sorted(missing) or "NONE")
    print("  unexpected in table:", sorted(unexpected) or "NONE")
    print("  light-group set %s" % ("OK" if light_ok else "MISMATCH (expected the 14 LightSources keys)"))
    bad = missing or unexpected or not light_ok
    print("RESULT: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


def main():
    if "--new" in sys.argv:
        return validate_new()
    if "--list" in sys.argv:
        print("EXPECTED (%d explicit keys):" % len(EXPECTED))
        for k in sorted(EXPECTED):
            print("  %s" % (k + (" [light]" if k in LIGHTS else "")))
        return 0
    print("usage: --new | --list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
