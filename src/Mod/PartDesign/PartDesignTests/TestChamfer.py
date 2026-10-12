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

    def testChamferNonManifoldCornerUsesWedgeFallback(self):
        # A boolean can leave an edge shared by more than two faces behind when
        # its boundary coincides exactly with existing geometry, and BRepCheck
        # does not flag it.  OCCT's ChFi3d then refuses any edge whose end vertex
        # touches that edge ("ChFi3d_Builder:only 2 faces"), and for some of them
        # it crashes.  Such edges must still be chamferable: the feature has to
        # fall back to subtracting a wedge prism.
        a = Part.makeBox(10, 10, 10)
        b = Part.makeBox(10, 10, 10).translate(FreeCAD.Vector(10, 10, 0))
        c = Part.makeBox(10, 10, 10).translate(FreeCAD.Vector(0, 0, 10))
        fused = a.fuse(b).fuse(c)
        self.assertTrue(fused.isValid())

        nonManifold = [
            edge for edge in fused.Edges if len(fused.ancestorsOfType(edge, Part.Face)) > 2
        ]
        self.assertEqual(len(nonManifold), 1)
        touching = [v.Point for edge in nonManifold for v in edge.Vertexes]
        candidates = [
            index + 1
            for index, edge in enumerate(fused.Edges)
            if len(fused.ancestorsOfType(edge, Part.Face)) == 2
            and any((v.Point - p).Length < 1e-6 for v in edge.Vertexes for p in touching)
        ]
        self.assertNotEqual(candidates, [])

        base = self.Doc.addObject("Part::Feature", "Base")
        base.Shape = fused
        body = self.Doc.addObject("PartDesign::Body", "Body")
        body.BaseFeature = base
        self.Doc.recompute()

        edgeName = "Edge%d" % candidates[0]
        edge = fused.getElement(edgeName)
        size = 0.5
        chamfer = body.newObject("PartDesign::Chamfer", "Chamfer")
        chamfer.Base = (base, [edgeName])
        chamfer.Size = size
        self.Doc.recompute()

        self.assertNotIn("Invalid", chamfer.State)
        self.assertTrue(chamfer.Shape.isValid())
        removed = fused.Volume - chamfer.Shape.Volume
        expected = 0.5 * size * size * edge.Length
        self.assertAlmostEqual(removed, expected, delta=expected * 0.05)

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

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestChamfer")
        # print ("omit closing document for debugging")
