# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2011 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   This program is distributed in the hope that it will be useful,       *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with this program; if not, write to the Free Software   *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************

import math
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

import FreeCAD
import Part
import TestSketcherApp

# Self-contained grazing pad + pocket case (the task-5 reproducer): the triangle
# cut into the wall of the first pocket has its top corner exactly on the wall's
# boundary edge, so the cut leaves an edge shared by three faces behind.
# BRepCheck still reports the solid as valid; the Pocket must warn about it.
# Run in a child process because Base::Console observers are C++ only.
REPRO_NONMANIFOLD_SCRIPT = textwrap.dedent(
    '''
    import FreeCAD
    import Part


    def build(graze):
        doc = FreeCAD.newDocument("repro")
        body = doc.addObject("PartDesign::Body", "Body")

        pad_sketch = doc.addObject("Sketcher::SketchObject", "PadSketch")
        body.addObject(pad_sketch)
        for a, b in (((0, 0), (60, 0)), ((60, 0), (60, 60)), ((60, 60), (0, 60)), ((0, 60), (0, 0))):
            pad_sketch.addGeometry(
                Part.LineSegment(FreeCAD.Vector(a[0], a[1], 0), FreeCAD.Vector(b[0], b[1], 0)), False
            )
        pad = doc.addObject("PartDesign::Pad", "Pad")
        body.addObject(pad)
        pad.Profile = (pad_sketch, [""])
        pad.Length = 10.0
        doc.recompute()

        top = next(
            i + 1
            for i, f in enumerate(pad.Shape.Faces)
            if abs(f.normalAt(0, 0).z - 1) < 1e-9 and abs(f.CenterOfMass.z - 10) < 1e-9
        )
        pocket1_sketch = doc.addObject("Sketcher::SketchObject", "Pocket1Sketch")
        body.addObject(pocket1_sketch)
        pocket1_sketch.AttachmentSupport = [(pad, "Face%d" % top)]
        pocket1_sketch.MapMode = "FlatFace"
        doc.recompute()
        inv = pocket1_sketch.Placement.inverse()
        corners = [
            inv.multVec(FreeCAD.Vector(x, y, 10)) for x, y in ((15, 15), (45, 15), (45, 45), (15, 45))
        ]
        for a, b in zip(corners, corners[1:] + corners[:1]):
            pocket1_sketch.addGeometry(
                Part.LineSegment(FreeCAD.Vector(a.x, a.y, 0), FreeCAD.Vector(b.x, b.y, 0)), False
            )
        pocket1 = doc.addObject("PartDesign::Pocket", "Pocket1")
        body.addObject(pocket1)
        pocket1.Profile = (pocket1_sketch, [""])
        pocket1.Length = 5.0
        doc.recompute()

        wall = next(
            i + 1
            for i, f in enumerate(pocket1.Shape.Faces)
            if abs(f.normalAt(0, 0).x - 1) < 1e-9 and abs(f.CenterOfMass.x - 15) < 1e-9
        )
        pocket2_sketch = doc.addObject("Sketcher::SketchObject", "Pocket2Sketch")
        body.addObject(pocket2_sketch)
        pocket2_sketch.AttachmentSupport = [(pocket1, "Face%d" % wall)]
        pocket2_sketch.MapMode = "FlatFace"
        doc.recompute()
        inv = pocket2_sketch.Placement.inverse()
        wall_pts = [
            inv.multVec(FreeCAD.Vector(15, y, z)) for y, z in ((15, 5), (45, 5), (45, 10), (15, 10))
        ]
        # the wall lies in the sketch's local x/y plane (local z is ~0)
        xs = [p.x for p in wall_pts]
        ys = [p.y for p in wall_pts]
        y_top = max(ys) if graze else max(ys) - 1.0
        triangle = [
            FreeCAD.Vector(min(xs) + 2.0, min(ys) + 1.0, 0),
            FreeCAD.Vector(max(xs) - 2.0, min(ys) + 1.0, 0),
            FreeCAD.Vector(max(xs) - 2.0, y_top, 0),
        ]
        for a, b in zip(triangle, triangle[1:] + triangle[:1]):
            pocket2_sketch.addGeometry(Part.LineSegment(a, b), False)
        pocket2 = doc.addObject("PartDesign::Pocket", "Pocket2")
        body.addObject(pocket2)
        pocket2.Profile = (pocket2_sketch, ["InternalFace1"])
        pocket2.Length = 3.0
        doc.recompute()
        return pocket2.Shape.countNonManifoldEdges()


    # a clean (inset) profile must not warn ...
    print("CLEAN_NONMANIFOLD=%d" % build(False))
    FreeCAD.closeDocument("repro")
    # ... while the grazing one must.
    print("NONMANIFOLD=%d" % build(True))
    '''
)


class TestPocket(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestPocket")

    def testPocketDimensionCase(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (0, 0), (10, 10))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 1
        self.Pad.Reversed = 1
        self.Doc.recompute()
        self.PocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch)
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch, (2.5, 2.5), (5, 5))
        self.Doc.recompute()
        self.Pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(self.Pocket)
        self.Pocket.Profile = self.PocketSketch
        self.Pocket.Length = 1
        self.Doc.recompute()
        self.assertAlmostEqual(self.Pocket.Shape.Volume, 75.0)

    def testPocketPreservesTaperDirection(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (-10, -10), (20, 20))

        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 10
        self.Doc.recompute()

        self.PocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch)
        self.PocketSketch.Placement = FreeCAD.Placement(
            FreeCAD.Vector(), FreeCAD.Rotation(FreeCAD.Vector(1, 0, 0), 180)
        )
        self.PocketSketch.MakeInternals = False
        radius = 2.0
        self.PocketSketch.addGeometry(
            Part.Circle(FreeCAD.Vector(), FreeCAD.Vector(0, 0, 1), radius), False
        )

        self.Pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(self.Pocket)
        self.Pocket.Profile = self.PocketSketch
        self.Pocket.Length = 5
        self.Pocket.TaperAngle = 20
        self.assertFalse(self.Pocket.UseLegacyTaperDirection)
        self.Doc.recompute()

        oneSidedVolume = self.Pocket.Shape.Volume
        expectedRadius = radius + self.Pocket.Length.Value * math.tan(math.radians(20))
        self.assertAlmostEqual(self.Pocket.AddSubShape.BoundBox.XMax, expectedRadius)

        self.Pocket.StartType = "Offset"
        self.Pocket.StartOffset = 1
        self.Doc.recompute()

        self.assertAlmostEqual(self.Pocket.AddSubShape.BoundBox.XMax, expectedRadius)

        self.Pocket.StartType = "Profile plane"
        self.Pocket.SideType = "Two sides"
        self.Pocket.Length2 = 5
        self.Doc.recompute()

        self.assertAlmostEqual(self.Pocket.AddSubShape.BoundBox.XMax, expectedRadius)
        self.assertAlmostEqual(self.Pocket.Shape.Volume, oneSidedVolume)

        self.Pocket.StartType = "Offset"
        self.Doc.recompute()

        self.assertAlmostEqual(self.Pocket.AddSubShape.BoundBox.XMax, expectedRadius)

        self.Pocket.UseLegacyTaperDirection = True
        self.Pocket.SideType = "One side"
        self.Doc.recompute()

        self.assertAlmostEqual(self.Pocket.AddSubShape.BoundBox.XMax, radius)

        self.Pocket.StartType = "Profile plane"
        self.Pocket.SideType = "Two sides"
        self.Doc.recompute()

        self.assertAlmostEqual(self.Pocket.AddSubShape.BoundBox.XMax, radius)

    def testStartOffset(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (0, 0), (10, 10))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 1
        self.Pad.Reversed = 1
        self.Doc.recompute()

        self.PocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch)
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch, (2.5, 2.5), (5, 5))
        self.Doc.recompute()
        self.Pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(self.Pocket)
        self.Pocket.Profile = self.PocketSketch
        self.Pocket.StartType = "Offset"
        self.Pocket.StartOffset = 0.5
        self.Pocket.Length = 0.25
        self.Doc.recompute()
        self.assertAlmostEqual(self.Pocket.Shape.Volume, 93.75)

    def testPocketThroughAllCase(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (0, 0), (10, 10))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 1
        self.Pad.Reversed = 1
        self.Doc.recompute()
        self.PocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch)
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch, (2.5, 2.5), (5, 5))
        self.Doc.recompute()
        self.Pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(self.Pocket)
        self.Pocket.Profile = self.PocketSketch
        self.Pocket.Length = 1
        self.Doc.recompute()
        self.PocketSketch1 = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch1)
        self.PocketSketch1.MapMode = "FlatFace"
        self.PocketSketch1.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        self.Doc.recompute()
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch1, (2.5, -0.75), (5, 0.50))
        self.Doc.recompute()
        self.Pocket001 = self.Doc.addObject("PartDesign::Pocket", "Pocket001")
        self.Body.addObject(self.Pocket001)
        self.Pocket001.Profile = self.PocketSketch1
        self.Pocket001.Type = 1
        self.Doc.recompute()
        self.assertAlmostEqual(self.Pocket001.Shape.Volume, 62.5)

    def testPocketToFirstCase(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (0, 0), (10, 10))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 1
        self.Pad.Reversed = 1
        self.Doc.recompute()
        self.PocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch)
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch, (2.5, 2.5), (5, 5))
        self.Doc.recompute()
        self.Pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(self.Pocket)
        self.Pocket.Profile = self.PocketSketch
        self.Pocket.Length = 1
        self.Doc.recompute()
        self.PocketSketch1 = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch1)
        self.PocketSketch1.MapMode = "FlatFace"
        self.PocketSketch1.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        self.Doc.recompute()
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch1, (2.5, -1), (5, 1))
        self.Doc.recompute()
        self.Pocket001 = self.Doc.addObject("PartDesign::Pocket", "Pocket001")
        self.Body.addObject(self.Pocket001)
        self.Pocket001.Profile = self.PocketSketch1
        self.Pocket001.Type = 2
        self.Doc.recompute()
        self.assertAlmostEqual(self.Pocket001.Shape.Volume, 62.5)

    def testPocketToFaceCase(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.PadSketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(self.PadSketch)
        TestSketcherApp.CreateRectangleSketch(self.PadSketch, (0, 0), (10, 10))
        self.Doc.recompute()
        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(self.Pad)
        self.Pad.Profile = self.PadSketch
        self.Pad.Length = 1
        self.Pad.Reversed = 1
        self.Doc.recompute()
        self.PocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch)
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch, (2.5, 2.5), (5, 5))
        self.Doc.recompute()
        self.Pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(self.Pocket)
        self.Pocket.Profile = self.PocketSketch
        self.Pocket.Length = 1
        self.Doc.recompute()
        self.PocketSketch1 = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(self.PocketSketch1)
        self.PocketSketch1.MapMode = "FlatFace"
        self.PocketSketch1.AttachmentSupport = (self.Doc.XZ_Plane, [""])
        self.Doc.recompute()
        TestSketcherApp.CreateRectangleSketch(self.PocketSketch1, (0, -1), (10, 1))
        self.Doc.recompute()
        self.Pocket001 = self.Doc.addObject("PartDesign::Pocket", "Pocket001")
        self.Body.addObject(self.Pocket001)
        self.Pocket001.Profile = self.PocketSketch1
        self.Pocket001.Type = 3
        # Handle face-naming inconsistency in OCC < 7
        self.FaceNumber = 7
        self.Pocket001.UpToFace = (self.Pocket, ["Face" + str(self.FaceNumber)])
        self.Doc.recompute()
        while (
            "Invalid" in self.Pocket001.State or round(self.Pocket001.Shape.Volume, 7) != 50.0
        ) and self.FaceNumber < 11:
            self.FaceNumber += 1
            self.Pocket001.UpToFace = (self.Pocket, ["Face" + str(self.FaceNumber)])
            self.Doc.recompute()
        self.assertAlmostEqual(self.Pocket001.Shape.Volume, 50.0)

    def _buildGrazingPocket(self, graze):
        """Pad + two pockets, returning the second Pocket.

        The triangle is cut 3 mm into the wall of the first pocket with its top
        corner exactly on the wall's boundary edge when ``graze`` is true - the
        task-5 reproducer for a cut boundary that coincides with existing
        geometry - or 1 mm inside it otherwise.
        """
        doc = self.Doc
        self.Body = doc.addObject("PartDesign::Body", "Body")

        pad_sketch = doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(pad_sketch)
        TestSketcherApp.CreateRectangleSketch(pad_sketch, (0, 0), (60, 60))
        doc.recompute()
        pad = doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(pad)
        pad.Profile = (pad_sketch, [""])
        pad.Length = 10.0
        doc.recompute()

        top = next(
            i + 1
            for i, f in enumerate(pad.Shape.Faces)
            if abs(f.normalAt(0, 0).z - 1) < 1e-9 and abs(f.CenterOfMass.z - 10) < 1e-9
        )
        pocket1_sketch = doc.addObject("Sketcher::SketchObject", "Pocket1Sketch")
        self.Body.addObject(pocket1_sketch)
        pocket1_sketch.AttachmentSupport = [(pad, "Face%d" % top)]
        pocket1_sketch.MapMode = "FlatFace"
        doc.recompute()
        inv = pocket1_sketch.Placement.inverse()
        corners = [
            inv.multVec(FreeCAD.Vector(x, y, 10))
            for x, y in ((15, 15), (45, 15), (45, 45), (15, 45))
        ]
        for a, b in zip(corners, corners[1:] + corners[:1]):
            pocket1_sketch.addGeometry(
                Part.LineSegment(FreeCAD.Vector(a.x, a.y, 0), FreeCAD.Vector(b.x, b.y, 0)),
                False,
            )
        pocket1 = doc.addObject("PartDesign::Pocket", "Pocket1")
        self.Body.addObject(pocket1)
        pocket1.Profile = (pocket1_sketch, [""])
        pocket1.Length = 5.0
        doc.recompute()

        wall = next(
            i + 1
            for i, f in enumerate(pocket1.Shape.Faces)
            if abs(f.normalAt(0, 0).x - 1) < 1e-9 and abs(f.CenterOfMass.x - 15) < 1e-9
        )
        pocket2_sketch = doc.addObject("Sketcher::SketchObject", "Pocket2Sketch")
        self.Body.addObject(pocket2_sketch)
        pocket2_sketch.AttachmentSupport = [(pocket1, "Face%d" % wall)]
        pocket2_sketch.MapMode = "FlatFace"
        doc.recompute()
        inv = pocket2_sketch.Placement.inverse()
        wall_pts = [
            inv.multVec(FreeCAD.Vector(15, y, z))
            for y, z in ((15, 5), (45, 5), (45, 10), (15, 10))
        ]
        ys = [p.y for p in wall_pts]
        zs = [p.z for p in wall_pts]
        z_top = max(zs) if graze else max(zs) - 1.0
        triangle = [
            FreeCAD.Vector(min(ys) + 2.0, min(zs) + 1.0, 0),
            FreeCAD.Vector(max(ys) - 2.0, min(zs) + 1.0, 0),
            FreeCAD.Vector(max(ys) - 2.0, z_top, 0),
        ]
        for a, b in zip(triangle, triangle[1:] + triangle[:1]):
            pocket2_sketch.addGeometry(Part.LineSegment(a, b), False)
        pocket2 = doc.addObject("PartDesign::Pocket", "Pocket2")
        self.Body.addObject(pocket2)
        pocket2.Profile = (pocket2_sketch, ["InternalFace1"])
        pocket2.Length = 3.0
        doc.recompute()
        return pocket2

    def testPocketGrazingProfileIsNudgedToManifold(self):
        # The profile's corner lies exactly on an existing edge, which used to make
        # the cut leave an edge shared by three faces (BRepCheck does not flag it).
        # The feature retries the boolean once with the profile nudged inside the
        # sketch plane, so the result is manifold again.  AddSubShape holds the
        # tool the retry started from, so cutting it from the base reproduces the
        # pre-nudge result: it must still show the defect, and the nudged result
        # must not have moved any measurable amount.
        pocket = self._buildGrazingPocket(graze=True)
        self.assertTrue(pocket.Shape.isValid())
        self.assertIsNone(pocket.Shape.check(True))
        self.assertEqual(pocket.Shape.countNonManifoldEdges(), 0)

        base = self.Doc.getObject("Pocket1").Shape
        preNudge = base.cut(pocket.AddSubShape)
        self.assertEqual(preNudge.countNonManifoldEdges(), 1)

        # The nudge is grow-only (the profile is scaled inside its plane), so the
        # tool removes a hair more material: perimeter x depth x nudge ~ 2.4e-3
        # mm3 here, 8e-8 of the part.  What it must never do is leave a ledge -
        # a translation used to, as faces below 1e-3 mm2.
        self.assertLess(abs(pocket.Shape.Volume - preNudge.Volume), 5e-3)
        self.assertEqual([f for f in pocket.Shape.Faces if f.Area < 1e-3], [])

    def testPocketInsetProfileStaysManifold(self):
        # A profile that does not lie on existing geometry must be left alone: no
        # retry, no nudge, and exactly the volume of the unmodified boolean.
        pocket = self._buildGrazingPocket(graze=False)
        self.assertTrue(pocket.Shape.isValid())
        self.assertEqual(pocket.Shape.countNonManifoldEdges(), 0)

        base = self.Doc.getObject("Pocket1").Shape
        unmodified = base.cut(pocket.AddSubShape)
        self.assertEqual(unmodified.countNonManifoldEdges(), 0)
        self.assertAlmostEqual(pocket.Shape.Volume, unmodified.Volume, delta=1e-6)

    def testPocketNonManifoldWarningIsPrinted(self):
        # Base::Console observers are C++ only, so run the same reproducer in a
        # child FreeCADCmd and capture the warning from its output.
        with tempfile.TemporaryDirectory() as home:
            env = dict(os.environ)
            env["HOME"] = home
            # NOTE: do not set QT_QPA_PLATFORM here - FreeCADCmd aborts with
            # "Application unexpectedly terminated" when the plugin is missing.
            env.pop("QT_QPA_PLATFORM", None)
            script = os.path.join(home, "repro_nonmanifold.py")
            with open(script, "w") as handle:
                handle.write(REPRO_NONMANIFOLD_SCRIPT)
            # FreeCADCmd aborts with "Application unexpectedly terminated" when a
            # multi-line script is passed through -c; run it from a file instead.
            proc = subprocess.run(
                [sys.executable, script], capture_output=True, text=True, timeout=300, env=env
            )
        output = proc.stdout + proc.stderr
        # both cases end manifold: the grazing one because the profile was nudged,
        # the inset one because it never needed it
        self.assertIn("CLEAN_NONMANIFOLD=0", output, output)
        self.assertIn("NONMANIFOLD=0", output, output)
        # the nudge warning fired for the grazing profile only
        warning = "the profile lay exactly on existing geometry"
        self.assertEqual(output.count(warning), 1, output)
        self.assertIn("the cut was retried with the profile nudged by", output, output)
        self.assertIn("This is a sub-micron change", output, output)
        # the old "left it non-manifold" warning must not appear
        self.assertNotIn("the result has", output, output)
    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestPocket")
        # print ("omit closing document for debugging")
