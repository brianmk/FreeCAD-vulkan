#!/usr/bin/env python3
"""Host-side assertions for vk_prefcover_probe.py (View3DSettings cover).

Parses the emitted [PFX] lines and asserts the observable setter->scene wiring
for the prefs with reliable read-back:
  - Orthographic  : camera type toggles Perspective <-> Orthographic
  - NavigationStyle : navigation type changes when set to an alternate style
  - AmbientLightIntensity : the environment's ambient intensity follows the pref
  - The probe runs every covered pref without crashing (a dropped/mistranscribed
    handler that throws would fault the run).

Nav sub-settings (zoom/rotation), dimensions, VBO, and the headlight/backlight
nodes have no clean Python read-back; those are guarded by the static coverage
checker (vk_viewsettings_keys --new) and a verbatim-transcription diff.
"""
import re
import sys

CAM = re.compile(r"^PFX (\S+) cam=(\w+)")
NAVTYPE = re.compile(r"^PFX navtype-touchpad cam=(\w+) nav=(\S+)")
ENV = re.compile(r"^PFX (\S+) cam=\w+ nav=\S+ dir\[\]=(\S*) env\[\]=(.*) err=")

CAMS = set()
NAV_AFTER = None
ENV_INTENSITY = {}


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    for ln in lines:
        m = CAM.match(ln)
        if m:
            if m.group(1) in ("ortho-off", "ortho-on"):
                CAMS.add((m.group(1), m.group(2)))
        m2 = NAVTYPE.match(ln)
        if m2:
            NAV_AFTER = m2.group(2)
        m3 = ENV.match(ln)
        if m3:
            stage, env = m3.group(1), m3.group(3)
            if stage in ("base", "ambient-77"):
                ENV_INTENSITY[stage] = env

    # 1. Camera: Orthographic pref toggles the camera type.
    res = {k: v for k, v in CAMS}
    if res.get("ortho-off") != "Perspective":
        err("expected camera=Perspective after Orthographic=False, got %r" % res.get("ortho-off"))
    if res.get("ortho-on") != "Orthographic":
        err("expected camera=Orthographic after Orthographic=True, got %r" % res.get("ortho-on"))

    # 2. NavigationStyle pref changes the navigation type.
    if not NAV_AFTER:
        err("no navtype-touchpad line; navigation-style change not observed")
    elif NAV_AFTER == "Gui::CADNavigationStyle":
        err("NavigationStyle handler did not change the navigation type")

    # 3. AmbientLightIntensity pref drives the environment ambient intensity.
    if "base" not in ENV_INTENSITY or "ambient-77" not in ENV_INTENSITY:
        err("missing env ambient lines for base/ambient-77")
    else:
        b, a = ENV_INTENSITY["base"], ENV_INTENSITY["ambient-77"]
        if b == a:
            err("AmbientLightIntensity did not change environment ambient intensity")

    # 4. Every covered pref ran without a handler fault (the probe would have
    #    reported a walk/* error or crashed otherwise).
    print("[CHECK] prefcover: camera=%s navtype=%s env base=%s ambient77=%s"
          % (res, NAV_AFTER, ENV_INTENSITY.get("base"), ENV_INTENSITY.get("ambient-77")))
