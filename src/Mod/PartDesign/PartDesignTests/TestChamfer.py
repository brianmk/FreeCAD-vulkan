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

import unittest

import FreeCAD
import Part


class TestChamfer(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestChamfer")

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
        self.assertLess(self.Chamfer.Shape.Volume, baseVolume)
        removed = baseVolume - self.Chamfer.Shape.Volume
        self.assertAlmostEqual(removed, 0.5 * 17.0 * 2.99 * 20.0, delta=1e-3)

        # the same geometry as asking for the distances the other way round
        reference = self.Doc.addObject("PartDesign::Chamfer", "Reference")
        self.Body.addObject(reference)
        reference.Base = (box, ["Edge%d" % edge])
        reference.ChamferType = 1
        reference.Size = 2.99
        reference.Size2 = 17.0
        self.Doc.recompute()
        self.assertTrue(reference.Shape.isValid())
        self.assertAlmostEqual(removed, baseVolume - reference.Shape.Volume, delta=1e-6)

    def testChamferNeverAddsMaterial(self):
        # The edge between the 5 mm wall and the floor of a pocket is concave, and
        # with 17/2.99 OCCT can also produce the concave-side chamfer, which ADDS
        # a wedge instead of taking one off (that is how the user's 50 mm edge
        # ended up as Pocket + 1270.75).  Only the chamfer that removes material
        # may be kept.
        import TestSketcherApp

        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        sketch = self.Doc.addObject("Sketcher::SketchObject", "PadSketch")
        self.Body.addObject(sketch)
        TestSketcherApp.CreateRectangleSketch(sketch, (0, 0), (20, 20))
        self.Doc.recompute()
        pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Body.addObject(pad)
        pad.Profile = (sketch, [""])
        pad.Length = 10.0
        self.Doc.recompute()

        top = next(
            i + 1
            for i, face in enumerate(pad.Shape.Faces)
            if abs(face.normalAt(0, 0).z - 1) < 1e-9 and abs(face.CenterOfMass.z - 10.0) < 1e-9
        )
        pocketSketch = self.Doc.addObject("Sketcher::SketchObject", "PocketSketch")
        self.Body.addObject(pocketSketch)
        pocketSketch.AttachmentSupport = [(pad, "Face%d" % top)]
        pocketSketch.MapMode = "FlatFace"
        self.Doc.recompute()
        inv = pocketSketch.Placement.inverse()
        corners = [
            inv.multVec(FreeCAD.Vector(x, y, 10.0)) for x, y in ((5, 5), (15, 5), (15, 15), (5, 15))
        ]
        for a, b in zip(corners, corners[1:] + corners[:1]):
            pocketSketch.addGeometry(
                Part.LineSegment(FreeCAD.Vector(a.x, a.y, 0), FreeCAD.Vector(b.x, b.y, 0)),
                False,
            )
        pocket = self.Doc.addObject("PartDesign::Pocket", "Pocket")
        self.Body.addObject(pocket)
        pocket.Profile = (pocketSketch, [""])
        pocket.Length = 5.0
        self.Doc.recompute()
        baseVolume = pocket.Shape.Volume

        edge = next(
            i + 1
            for i, candidate in enumerate(pocket.Shape.Edges)
            if abs(candidate.Length - 20.0) < 1e-6
            and any(
                abs(face.normalAt(0, 0).z) > 0.9
                for face in pocket.Shape.ancestorsOfType(candidate, Part.Face)
            )
        )

        self.Chamfer = self.Doc.addObject("PartDesign::Chamfer", "Chamfer")
        self.Body.addObject(self.Chamfer)
        self.Chamfer.Base = (pocket, ["Edge%d" % edge])
        self.Chamfer.ChamferType = 1
        self.Chamfer.Size = 17.0
        self.Chamfer.Size2 = 2.99
        self.Doc.recompute()

        self.assertNotIn("Invalid", self.Chamfer.State)
        self.assertTrue(self.Chamfer.Shape.isValid())
        self.assertLess(self.Chamfer.Shape.Volume, baseVolume)
        self.assertAlmostEqual(baseVolume - self.Chamfer.Shape.Volume, 508.3, delta=1e-3)

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestChamfer")
        # print ("omit closing document for debugging")
