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

import os
import unittest

import FreeCAD
import Part


class TestChamfer(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestChamfer")

    def _addPocket(self, pad_size, pocket_size, depth, height=10.0):
        """Add a Pad with a rectangular Pocket (x by y) to the body."""
        import TestSketcherApp

        sketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(sketch)
        TestSketcherApp.CreateRectangleSketch(sketch, (0, 0), (pad_size, pad_size))
        self.Doc.recompute()
        pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(pad)
        pad.Profile = (sketch, [""])
        pad.Length = height
        self.Doc.recompute()

        top = next(
            i + 1
            for i, face in enumerate(pad.Shape.Faces)
            if abs(face.normalAt(0, 0).z - 1) < 1e-9 and abs(face.CenterOfMass.z - height) < 1e-9
        )
        pocket_sketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(pocket_sketch)
        pocket_sketch.AttachmentSupport = [(pad, "Face%d" % top)]
        pocket_sketch.MapMode = "FlatFace"
        self.Doc.recompute()
        inv = pocket_sketch.Placement.inverse()
        size_x, size_y = pocket_size
        half_x = (pad_size - size_x) * 0.5
        half_y = (pad_size - size_y) * 0.5
        corners = [
            inv.multVec(FreeCAD.Vector(x, y, height))
            for x, y in (
                (half_x, half_y),
                (half_x + size_x, half_y),
                (half_x + size_x, half_y + size_y),
                (half_x, half_y + size_y),
            )
        ]
        for a, b in zip(corners, corners[1:] + corners[:1]):
            pocket_sketch.addGeometry(
                Part.LineSegment(FreeCAD.Vector(a.x, a.y, 0), FreeCAD.Vector(b.x, b.y, 0)),
                False,
            )
        pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(pocket)
        pocket.Profile = (pocket_sketch, [""])
        pocket.Length = depth
        self.Doc.recompute()
        return pad, pocket

    def testChamferCubeToOctahedron(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.Body.addObject(self.Box)
        self.Box.Length = 10.00
        self.Box.Width = 10.00
        self.Box.Height = 10.00
        self.Doc.recompute()
        self.Chamfer = self.Doc.addObject("PartDesign::Chamfer", "Chamfer")
        self.Chamfer.Base = (self.Box, ["Face" + str(i + 1) for i in range(6)])
        self.Chamfer.Size = 4.999999
        self.Body.addObject(self.Chamfer)
        self.Doc.recompute()
        self.MajorFaces = [face for face in self.Chamfer.Shape.Faces if face.Area > 1e-3]
        self.assertEqual(len(self.MajorFaces), 8)
        # test UseAllEdges property
        self.Chamfer.UseAllEdges = True
        self.Chamfer.Base = (self.Box, [""])  # no subobjects, should still work
        self.Doc.recompute()
        self.MajorFaces = [face for face in self.Chamfer.Shape.Faces if face.Area > 1e-3]
        self.assertEqual(len(self.MajorFaces), 8)
        self.Chamfer.Base = (self.Box, ["Face50"])  # non-existent face, test topo naming resilience
        self.Doc.recompute()
        self.MajorFaces = [face for face in self.Chamfer.Shape.Faces if face.Area > 1e-3]
        self.assertEqual(len(self.MajorFaces), 8)
        self.Chamfer.UseAllEdges = False
        self.Chamfer.Base = (self.Box, ["Face1"])
        self.Doc.recompute()
        self.MajorFaces = [face for face in self.Chamfer.Shape.Faces if face.Area > 1e-3]
        self.assertEqual(len(self.MajorFaces), 9)

    def testTwoDistancesFitTheOtherFace(self):
        # A 17 mm distance does not fit the 10 mm side of this box: OCCT folds
        # that face past its boundary or fails.  The same chamfer is expressible
        # with the distances on the other adjacent face, which is what the
        # feature falls back to - and it must REMOVE material, never add a wedge.
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.Body.addObject(box)
        box.Length = 20.0
        box.Width = 20.0
        box.Height = 10.0
        self.Doc.recompute()
        baseVolume = box.Shape.Volume
        top = next(
            i + 1 for i, face in enumerate(box.Shape.Faces) if abs(face.normalAt(0, 0).z - 1) < 1e-9
        )
        edge = next(
            i + 1
            for i, candidate in enumerate(box.Shape.Edges)
            if abs(candidate.Length - 20.0) < 1e-6
            and any(
                face.isSame(box.Shape.Faces[top - 1])
                for face in box.Shape.ancestorsOfType(candidate, Part.Face)
            )
        )
        self.Chamfer = self.Doc.addObject("PartDesign::Chamfer", "Chamfer")
        self.Body.addObject(self.Chamfer)
        self.Chamfer.Base = (box, ["Edge%d" % edge])
        self.Chamfer.ChamferType = 1  # Two distances
        self.Chamfer.Size = 17.0
        self.Chamfer.Size2 = 2.99
        self.Doc.recompute()

        self.assertNotIn("Invalid", self.Chamfer.State)
        self.assertTrue(self.Chamfer.Shape.isValid())
        # convex: the chamfer takes the wedge off, exactly 0.5*17*2.99*20
        self.assertLess(self.Chamfer.Shape.Volume, baseVolume)
        removed = baseVolume - self.Chamfer.Shape.Volume
        self.assertAlmostEqual(removed, 0.5 * 17.0 * 2.99 * 20.0, delta=1e-3)

    def testChamferFillsTheValleyOfAConcaveEdge(self):
        # The floor/wall edge of a pocket is concave, so a chamfer there ADDS the
        # wedge that fills the valley - never a groove.  The faces are big enough
        # for both distances (17 and 2.99), so the wedge is legitimate.
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        pad, pocket = self._addPocket(pad_size=40.0, pocket_size=(10.0, 30.0), depth=5.0)
        baseVolume = pocket.Shape.Volume
        floor = next(face for face in pocket.Shape.Faces if abs(face.Area - 300.0) < 1e-3)
        edge = next(
            i + 1
            for i, candidate in enumerate(pocket.Shape.Edges)
            if abs(candidate.Length - 10.0) < 1e-6
            and any(face.isSame(floor) for face in pocket.Shape.ancestorsOfType(candidate, Part.Face))
        )
        # both size orders must produce the same wedge (the user's reference
        # Chamfer001 uses 2.99 / 17 on a concave edge of this shape)
        for index, (size, size2) in enumerate(((17.0, 2.99), (2.99, 17.0))):
            name = "Chamfer%s" % index
            feature = self.Doc.addObject("PartDesign::Chamfer", name)
            self.Body.addObject(feature)
            feature.Base = (pocket, ["Edge%d" % edge])
            feature.ChamferType = 1  # Two distances
            feature.Size = size
            feature.Size2 = size2
            self.Doc.recompute()

            self.assertNotIn("Invalid", feature.State)
            self.assertTrue(feature.Shape.isValid())
            self.assertGreater(feature.Shape.Volume, baseVolume)
            change = feature.Shape.Volume - baseVolume
            self.assertAlmostEqual(change, 0.5 * 17.0 * 2.99 * 10.0, delta=1e-3)

            # the changed region must sit next to the edge, not somewhere else
            changed = feature.Shape.cut(pocket.Shape)
            bounds = changed.BoundBox
            limit = pocket.Shape.Edges[edge - 1].BoundBox
            for low, high, margin in (
                (bounds.XMin, limit.XMin - 17.0, "X"),
                (bounds.YMin, limit.YMin - 17.0, "Y"),
                (bounds.ZMin, limit.ZMin - 17.0, "Z"),
            ):
                self.assertGreaterEqual(low, high - 1e-3)
            for low, high, margin in (
                (bounds.XMax, limit.XMax + 17.0, "X"),
                (bounds.YMax, limit.YMax + 17.0, "Y"),
                (bounds.ZMax, limit.ZMax + 17.0, "Z"),
            ):
                self.assertLessEqual(low, high + 1e-3)

    def testChamferRefusesADistancePastTheFace(self):
        # The user's case: one of the faces next to the edge is only 1 mm wide
        # ("a flat plane that ends at 1 mm").  17 x 1.0 is a chamfer; 17 x 1.01
        # would run off that face and gouge the neighbours, so it must be refused
        # with the extent named instead of being built.
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        pad, pocket = self._addPocket(pad_size=40.0, pocket_size=(20.0, 20.0), depth=1.0)
        top = next(
            face
            for face in pocket.Shape.Faces
            if abs(face.Area - 1200.0) < 1e-3 and abs(face.CenterOfMass.z - 10.0) < 1e-3
        )
        edge = next(
            i + 1
            for i, candidate in enumerate(pocket.Shape.Edges)
            if abs(candidate.Length - 20.0) < 1e-6
            and any(face.isSame(top) for face in pocket.Shape.ancestorsOfType(candidate, Part.Face))
        )

        def chamfer(size2, name):
            feature = self.Doc.addObject("PartDesign::Chamfer", name)
            self.Body.addObject(feature)
            feature.Base = (pocket, ["Edge%d" % edge])
            feature.ChamferType = 1
            feature.Size = 17.0
            feature.Size2 = size2
            self.Doc.recompute()
            return feature

        # This rim edge is not a valley, so 17 x 1.0 is not required to build here
        # (the concave path is covered by testChamferFillsTheValleyOfAConcaveEdge
        # and the wedge path by testChamferOnAThrownEdgeRefusesVisibly).  What
        # matters is that a distance past the 1 mm face is refused with a reason
        # and leaves no shape behind.
        messages = {}
        for size2 in (1.01, 2.99):
            refused = chamfer(size2, "Refused_%s" % size2)
            self.assertIn("Invalid", refused.State)
            self.assertTrue(refused.Shape.isNull())
            messages[size2] = refused.getStatusString()

        # These refusals come from the reach rule, so they keep its wording: the
        # face that is too short and the distance that runs past it are named, and
        # the invalid-result hint must not take their place (the base is clean).
        self.assertIn(
            "the face next to this edge ends 1 mm away, so a 1.01 mm chamfer would cut through "
            "it - reduce the distance or extend the geometry.",
            messages[1.01],
        )
        self.assertIn(
            "the face next to this edge ends 1 mm away, so a 2.99 mm chamfer would cut through "
            "it - reduce the distance or extend the geometry.",
            messages[2.99],
        )
        for text in messages.values():
            self.assertNotIn("produced an invalid shape", text)
            self.assertNotIn("too large", text)

    def testConcaveWedgeMatchesTheUserReference(self):
        # The user's Chamfer001 is the definition of "correct" for the concave
        # wedge path.  Fixtures/local_reference_edge.brep is the 60 mm
        # neighbourhood of that edge (Pocket.Edge125, 34.8281 mm concave, faces
        # reaching 17.0552 / 3.0000): Two distances 2.99 / 17 must add
        # 591.0591 mm3 in the region (-63.720,-57.979,-21.000)-(-47.631,-33.101,1.000),
        # in either order.  This pins the reference without depending on the
        # user's document (whose chain collapses without Chamfer001).
        fixture = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "Fixtures", "local_reference_edge.brep"
        )
        if not os.path.isfile(fixture):
            self.skipTest("local_reference_edge.brep fixture is not available")

        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        part = self.Doc.addObject("Part::Feature", "ReferenceEdge")
        part.Shape = Part.read(fixture)
        self.Body.BaseFeature = part
        self.Doc.recompute()
        base = self.Body.Shape
        self.assertAlmostEqual(base.Volume, 72492.2023, delta=1e-3)
        self.assertTrue(base.isValid())

        # The fixture holds two edges with that signature (the reference edge and
        # its mirror image), so pin whichever one reproduces the reference.
        candidates = [
            i + 1
            for i, candidate in enumerate(base.Edges)
            if abs(candidate.Length - 34.8281) < 1e-3
            and sorted(
                round(face.Area, 2) for face in base.ancestorsOfType(candidate, Part.Face)
            )
            == [104.48, 297.0]
        ]
        self.assertTrue(candidates, "the reference edge is missing from the fixture")

        matched = 0
        for index in candidates:
            for size, size2 in ((2.99, 17.0), (17.0, 2.99)):
                name = "Reference_%s_%s_%s" % (index, size, size2)
                feature = self.Doc.addObject("PartDesign::Chamfer", name)
                self.Body.addObject(feature)
                feature.Base = (self.Body.BaseFeature, ["Edge%d" % index])
                feature.ChamferType = 1  # Two distances
                feature.Size = size
                feature.Size2 = size2
                self.Doc.recompute()
                if feature.Shape.isNull() or "Invalid" in feature.State:
                    continue
                change = feature.Shape.Volume - base.Volume
                if abs(change - 591.0591) >= 1e-3:
                    continue

                self.assertTrue(feature.Shape.isValid())
                self.assertEqual(len(feature.Shape.Solids), 1)
                bounds = feature.Shape.cut(base).BoundBox
                for got, expected in (
                    (bounds.XMin, -63.720),
                    (bounds.YMin, -57.979),
                    (bounds.ZMin, -21.000),
                    (bounds.XMax, -47.631),
                    (bounds.YMax, -33.101),
                    (bounds.ZMax, 1.000),
                ):
                    self.assertAlmostEqual(got, expected, delta=1e-3)
                matched += 1

        # both size orders on the reference edge
        self.assertEqual(matched, 2)

    def testChamferOnAThrownEdgeRefusesVisibly(self):
        # Edge10 in the user's model: a 22 mm concave edge whose one face reaches
        # only 2 mm, with Two distances 17 / 1.0.  OCCT throws on it; the feature
        # used to die with a bare "BRep_API: command not done", no shape and no
        # explanation, and an earlier shape stayed on screen.  Now it must either
        # build the valley wedge or refuse with a reason, and never leave geometry
        # behind.
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        pad, pocket = self._addPocket(pad_size=40.0, pocket_size=(22.0, 28.0), depth=2.0)
        base = pocket.Shape
        edge = next(
            i + 1
            for i, candidate in enumerate(base.Edges)
            if abs(candidate.Length - 22.0) < 1e-6
            and any(
                abs(face.CenterOfMass.z - 8.0) < 1e-6
                for face in base.ancestorsOfType(candidate, Part.Face)
            )
        )
        feature = self.Doc.addObject("PartDesign::Chamfer", "ThrownEdge")
        self.Body.addObject(feature)
        feature.Base = (pocket, ["Edge%d" % edge])
        feature.ChamferType = 1  # Two distances

        # 17 / 1.0 fits both faces (17 <= 28, 1.0 <= 2.0): the valley wedge
        feature.Size = 17.0
        feature.Size2 = 1.0
        self.Doc.recompute()
        self.assertNotIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isValid())
        self.assertAlmostEqual(
            feature.Shape.Volume - base.Volume, 0.5 * 17.0 * 1.0 * 22.0, delta=1e-3
        )

        # the wedge uses plain BRepAlgoAPI booleans, so make sure the element map
        # downstream features rely on still resolves on its result
        self.assertIsNotNone(feature.Shape.getElement("Face1"))
        self.assertIsNotNone(feature.getSubObject("Face1"))

        # the wedge must be on the correct side of the edge: every vertex of the
        # added region lies in front of both adjacent face planes
        wedgeEdge = base.Edges[edge - 1]
        origin = wedgeEdge.Vertexes[0].Point
        added = feature.Shape.cut(base)
        faces = base.ancestorsOfType(wedgeEdge, Part.Face)
        for face in faces:
            normal = face.normalAt(0, 0)  # outward for a solid
            self.assertGreaterEqual(
                min((vertex.Point - origin).dot(normal) for vertex in added.Vertexes), -1e-6
            )

        # ... but "in front of both planes" is not enough: a wedge standing in the
        # air satisfies it too.  A chamfer's changed region must lie ON the faces it
        # joins - it cannot leave the base shape's bounding box, and it has to reach
        # each adjacent face away from the shared edge (at the leg endpoint, where
        # the wedge lies along that face).  The 17 mm leg that used to go up the
        # 2 mm wall reaches 15 mm above the part (z = 25 on a 0..10 pad) and fails
        # both checks; the side-correct wedge (1 mm up the wall, 17 mm across the
        # floor) passes them.
        self.assertTrue(added.Vertexes)
        for vertex in added.Vertexes:
            point = vertex.Point
            for got, limit in (
                (point.x, base.BoundBox.XMax),
                (point.y, base.BoundBox.YMax),
                (point.z, base.BoundBox.ZMax),
            ):
                self.assertLessEqual(got, limit + 1e-6)
            for got, limit in (
                (point.x, base.BoundBox.XMin),
                (point.y, base.BoundBox.YMin),
                (point.z, base.BoundBox.ZMin),
            ):
                self.assertGreaterEqual(got, limit - 1e-6)

        for face in faces:
            onFace = [
                vertex
                for vertex in added.Vertexes
                if Part.Vertex(vertex.Point).distToShape(face)[0] <= 1e-6
                and face.isInside(vertex.Point, 1e-6, True)
                and Part.Vertex(vertex.Point).distToShape(wedgeEdge)[0] > 1e-6
            ]
            self.assertTrue(
                onFace,
                "the wedge does not reach this adjacent face along its surface",
            )

        # 2.5 does not fit the 2 mm face: refuse, and drop the shape built above
        feature.Size2 = 2.5
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isNull())
        self.assertIn(
            "the face next to this edge ends 2 mm away, so a 2.5 mm chamfer would cut through it",
            feature.getStatusString(),
        )

        # a distance beyond every face is refused the same way
        feature.Size = 100.0
        feature.Size2 = 1.0
        self.Doc.recompute()
        self.assertIn("Invalid", feature.State)
        self.assertTrue(feature.Shape.isNull())
        self.assertIn(
            "the face next to this edge ends 28 mm away, so a 100 mm chamfer would cut through it",
            feature.getStatusString(),
        )

    def testChamferRefusalOnASliverBaseNamesTheBase(self):
        # The user's case: Two distances 2 mm / 1 mm on the valley edge whose
        # faces reach 17.0552 and 3.0 mm.  The sizes *fit*, so the reach rule
        # passed, but their Pocket was stale and still carried four sub-micron
        # edges (1.29e-05 .. 2.23e-05 mm; 129 edges instead of the 123 the sliver
        # retry produces), so every chamfer result on that corner folded a face.
        # The refusal used to read "the sizes 2 mm / 1 mm are too large for this
        # edge", which is wrong and sent them hunting the sizes; it has to name
        # the base instead.  Fixtures/local_reference_edge.brep is that geometry
        # (the neighbourhood of Pocket.Edge125) and still holds six edges shorter
        # than 1e-3 mm, so the message is pinned on a real sliver base.
        fixture = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "Fixtures", "local_reference_edge.brep"
        )
        if not os.path.isfile(fixture):
            self.skipTest("local_reference_edge.brep fixture is not available")

        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        part = self.Doc.addObject("Part::Feature", "SliverBase")
        part.Shape = Part.read(fixture)
        self.Body.BaseFeature = part
        self.Doc.recompute()
        base = self.Body.Shape
        slivers = sum(1 for edge in base.Edges if edge.Length < 1e-3)
        self.assertGreater(slivers, 0, "the fixture must carry the slivers of the user's case")

        # two edges have that length (the reference and its mirror); one of them
        # folds a face with the fitting sizes, the other fails in OCCT outright
        candidates = [
            index + 1 for index, edge in enumerate(base.Edges) if abs(edge.Length - 19.5637) < 1e-3
        ]
        self.assertTrue(candidates, "the fixture no longer holds the edge this case is about")

        invalidResult = []
        for index in candidates:
            feature = self.Doc.addObject("PartDesign::Chamfer", "Sliver_%d" % index)
            self.Body.addObject(feature)
            feature.Base = (self.Body.BaseFeature, ["Edge%d" % index])
            feature.ChamferType = 1  # Two distances
            feature.Size = 2.0
            feature.Size2 = 1.0
            self.Doc.recompute()
            self.assertIn("Invalid", feature.State)
            self.assertTrue(feature.Shape.isNull())
            text = feature.getStatusString()
            if "produced an invalid shape" in text:
                invalidResult.append(text)

        self.assertTrue(invalidResult, "the sliver base must refuse the fitting sizes")
        for text in invalidResult:
            # the base is named, with its sliver count...
            self.assertIn(
                "The base still has %d edges shorter than 1e-3 mm" % slivers, text
            )
            # ...the fix is on the model, not on the sizes: the nudge retry that
            # used to be suggested here is gone (it turned the shape's planes into
            # B-splines, see PROFILE_NUDGE_ENABLED)...
            self.assertIn("Move the profile edge that grazes", text)
            self.assertNotIn("sliver retry", text)
            # ...and the sizes are explicitly not the problem
            self.assertIn("sizes 2 mm / 1 mm fit the adjacent faces", text)
            self.assertNotIn("too large", text)

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestChamfer")
        # print ("omit closing document for debugging")
