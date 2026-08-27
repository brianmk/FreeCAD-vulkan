#!/usr/bin/env python3
"""Example consumer: drive a PartDesign Body -> Sketch -> Pad using the unified
Session harness's dependency-orchestration helpers.

Demonstrates that the tool reaches any FreeCAD tool AND that missing
prerequisites are caught (a Pad fired without its closed profile would be
reported as a FAIL, not silently ignored).

Exit: prints `[VERDICT] SKETCHPAD PASS|FAIL` via the harness verdict API.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session

RX0, RY0, RX1, RY1 = 0.0, 0.0, 10.0, 10.0
PAD_DEPTH = 5.0


def run(s: Session) -> None:
    doc = s.new_document("SketchPad")
    s.activate_workbench("PartDesignWorkbench")
    s.recompute(doc)

    # Dependency chain, orchestrated by the harness helpers:
    #   1) a Body must exist            -> ensure_body()
    #   2) a closed profile must exist  -> make_rect_sketch()
    #   3) the profile must be selected -> add_partdesign_feature() does this
    #   4) the Pad feature must result  -> verified by type_id == "Pad"
    body = s.ensure_body("Body")
    if body is None:
        s.verdict(False, "no Body created")
        return

    sketch = s.make_rect_sketch(RX0, RY0, RX1, RY1, name="Sketch")
    if sketch is None:
        s.verdict(False, "no closed sketch created")
        return

    # A sketch with no body membership must fail loudly (dependency check).
    stray = doc.addObject("Sketcher::SketchObject", "Stray")
    if s.add_partdesign_feature("PartDesign_Pad", "Pad", profile=stray) is not None:
        s.verdict(False, "Pad succeeded over a stray (non-body) sketch")

    pad = s.add_partdesign_feature("PartDesign_Pad", "Pad", profile=sketch)
    if pad is None:
        s.verdict(False, "Pad did not produce a feature (dependency chain broke)")
        return

    ok = _check_pad(pad)
    s.emit("result", created=pad.Name, length=PAD_DEPTH, ok=ok)
    s.verdict(ok)


def _check_pad(pad) -> bool:
    bb = pad.Shape.BoundBox
    # The 10x10 rectangle extruded by PAD_DEPTH along +Z.
    return (abs(bb.XMax - bb.XMin - 10.0) < 1e-6
            and abs(bb.YMax - bb.YMin - 10.0) < 1e-6
            and abs(bb.ZMax - bb.ZMin - PAD_DEPTH) < 1e-6)


Session.deferred("SKETCHPAD", run, start_ms=500)
