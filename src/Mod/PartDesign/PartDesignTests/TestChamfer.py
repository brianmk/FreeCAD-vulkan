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
        # A distance that does not fit the face the edge was selected from makes
        # OCCT fold that face past its boundary (a negative-area face) or fail.
        # The same chamfer is expressible with the distances on the other
        # adjacent face, which is what the feature now falls back to: for the
        # user's model that is TopoShape::makeChamfer(2.99, 17.0, {edge}).
        # Here the wall of the pocket is only `depth` high, so a 17 mm distance
        # cannot be measured on it.
        import Part
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

        # the 20 mm edge between the wall (5 mm high) and the floor of the pocket
        edge = next(
            i + 1
            for i, candidate in enumerate(pocket.Shape.Edges)
            if candidate.Curve.TypeId == "Part::GeomLine"
            and abs(candidate.Length - 20.0) < 1e-6
            and any(abs(f.normalAt(0, 0).z) > 0.9
                    for f in pocket.Shape.ancestorsOfType(candidate, Part.Face))
        )
        baseVolume = pocket.Shape.Volume

        self.Chamfer = self.Doc.addObject("PartDesign::Chamfer", "Chamfer")
        self.Body.addObject(self.Chamfer)
        self.Chamfer.Base = (pocket, ["Edge%d" % edge])
        self.Chamfer.ChamferType = 1  # Two distances
        self.Chamfer.Size = 17.0
        self.Chamfer.Size2 = 2.99
        self.Doc.recompute()

        self.assertNotIn("Invalid", self.Chamfer.State)
        self.assertTrue(self.Chamfer.Shape.isValid())
        otherFace = self.Chamfer.Shape.Volume - baseVolume

        # the same geometry as asking for the distances the other way round
        reference = self.Doc.addObject("PartDesign::Chamfer", "Reference")
        self.Body.addObject(reference)
        reference.Base = (pocket, ["Edge%d" % edge])
        reference.ChamferType = 1
        reference.Size = 2.99
        reference.Size2 = 17.0
        self.Doc.recompute()
        self.assertTrue(reference.Shape.isValid())
        self.assertAlmostEqual(otherFace, reference.Shape.Volume - baseVolume, delta=1e-6)

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestChamfer")
        # print ("omit closing document for debugging")
