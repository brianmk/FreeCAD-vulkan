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
import unittest

import FreeCAD
import Part

App = FreeCAD


class TestDraft(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestDraft")

    def testSimpleDraft(self):
        # fix: create datum plane on YZ. create datum line on Z + 10i
        # find top face by first making list comprehension of Z-normal faces
        # and then find which has the higher center of mass Z-value
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.Body.addObject(self.Box)
        self.Box.Length = 10.00
        self.Box.Width = 10.00
        self.Box.Height = 10.00
        self.Doc.recompute()
        self.DatumPlane = self.Doc.addObject("PartDesign::Plane", "DatumPlane")
        self.DatumPlane.AttachmentSupport = [(self.Doc.YZ_Plane, "")]
        self.DatumPlane.MapMode = "FlatFace"
        self.Body.addObject(self.DatumPlane)
        self.Doc.recompute()
        self.DatumLine = self.Doc.addObject("PartDesign::Line", "DatumLine")
        self.DatumLine.AttachmentSupport = [(self.Doc.X_Axis, "")]
        self.DatumLine.MapMode = "TwoPointLine"
        self.Body.addObject(self.DatumLine)
        self.Doc.recompute()
        self.Draft = self.Doc.addObject("PartDesign::Draft", "Draft")
        # Draft.Base needs to be top face
        self.Faces = self.Box.Shape.Faces
        # Grab the two faces with Z-normals and find the higher one
        self.ZFaceIndexes = [
            i for i in range(len(self.Faces)) if self.Faces[i].Surface.Axis == App.Vector(0, 0, 1)
        ]
        if (
            self.Faces[self.ZFaceIndexes[0]].CenterOfMass.z
            > self.Faces[self.ZFaceIndexes[1]].CenterOfMass.z
        ):
            self.TopFaceIndex = self.ZFaceIndexes[0]
        else:
            self.TopFaceIndex = self.ZFaceIndexes[1]
        self.Draft.Base = (self.Box, ["Face" + str(self.TopFaceIndex + 1)])
        self.Draft.NeutralPlane = (self.DatumPlane, [""])
        self.Draft.PullDirection = (self.DatumLine, [""])
        self.Draft.Angle = 45.0
        self.Draft.Reversed = 1
        self.Body.addObject(self.Draft)
        self.Doc.recompute()
        if "Invalid" in self.Draft.State:
            self.Draft.Reversed = 0
            self.Doc.recompute()
        self.assertAlmostEqual(self.Draft.Shape.Volume, 1500)

    def testDraftOnFaceSplitBySketch(self):
        # A sketch lying on the +X face of the box splits that face; only the
        # part of the face covered by the sketch is drafted.  The lower half of
        # the +X face (z from 0 to 10) is sketched and drafted about the split
        # line (z = 10), which removes/adds a wedge of 0.5 * 10 * tan(10) * 10 * 20.
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.Body.addObject(self.Box)
        self.Box.Length = 20.0
        self.Box.Width = 20.0
        self.Box.Height = 20.0
        self.Doc.recompute()

        self.Faces = self.Box.Shape.Faces
        # the +X face (at x = 20)
        self.XFaceIndexes = [
            i
            for i in range(len(self.Faces))
            if self.Faces[i].Surface.Axis == App.Vector(1, 0, 0)
            and abs(self.Faces[i].CenterOfMass.x - 20.0) < 1e-9
        ]
        self.assertEqual(len(self.XFaceIndexes), 1)
        self.XFace = "Face" + str(self.XFaceIndexes[0] + 1)

        # datum plane at z = 10, i.e. through the split line, used as neutral
        # plane so that the drafted half hinges on the split line
        self.Plane = self.Doc.addObject("PartDesign::Plane", "NeutralPlane")
        self.Body.addObject(self.Plane)
        self.Plane.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        self.Plane.MapMode = "FlatFace"
        self.Plane.AttachmentOffset = App.Placement(App.Vector(0, 0, 10), App.Rotation())
        self.Doc.recompute()

        # Sketch on the +X face, covering its lower half.  The outline is
        # expressed in the sketch's own (attached) coordinate system, so the
        # corners are mapped from the global face corners.
        self.Sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        self.Body.addObject(self.Sketch)
        self.Sketch.AttachmentSupport = [(self.Box, self.XFace)]
        self.Sketch.MapMode = "FlatFace"
        self.Doc.recompute()
        toLocal = self.Sketch.getGlobalPlacement().inverse()
        corners = [
            toLocal.multVec(App.Vector(20, 0, 0)),
            toLocal.multVec(App.Vector(20, 20, 0)),
            toLocal.multVec(App.Vector(20, 20, 10)),
            toLocal.multVec(App.Vector(20, 0, 10)),
        ]
        for i in range(4):
            self.Sketch.addGeometry(
                Part.LineSegment(corners[i], corners[(i + 1) % 4]), False
            )
        self.Doc.recompute()

        self.Draft = self.Doc.addObject("PartDesign::Draft", "Draft")
        self.Draft.Base = (self.Box, [self.XFace])
        self.Draft.Profile = (self.Sketch, [""])
        self.Draft.NeutralPlane = (self.Plane, [""])
        self.Draft.Angle = 10.0
        self.Body.addObject(self.Draft)
        self.Doc.recompute()

        self.assertNotIn("Invalid", self.Draft.State)
        self.assertTrue(self.Draft.Shape.isValid())

        # the base face was split in two, so there is one face more than before
        self.assertEqual(len(self.Draft.Shape.Faces), 7)

        # the upper half of the +X face stays untouched
        upper = [
            face
            for face in self.Draft.Shape.Faces
            if abs(face.Surface.Axis.x - 1.0) < 1e-9
            and abs(face.Surface.Axis.y) < 1e-9
            and abs(face.Surface.Axis.z) < 1e-9
            and abs(face.Area - 200.0) < 1e-6
        ]
        self.assertEqual(len(upper), 1)

        # the lower half is tilted by the draft angle
        tilted = [
            face
            for face in self.Draft.Shape.Faces
            if abs(face.Surface.Axis.x) > 0.9 and abs(face.Surface.Axis.z) > 0.1
        ]
        self.assertEqual(len(tilted), 1)
        self.assertAlmostEqual(
            abs(tilted[0].Surface.Axis.z), math.sin(math.radians(10.0)), places=6
        )

        # volume of the drafted wedge, the sign depends on the pull direction
        wedge = 1000.0 * math.tan(math.radians(10.0))
        self.assertAlmostEqual(abs(self.Draft.Shape.Volume - 8000.0), wedge, places=3)

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestDraft")
        # print ("omit closing document for debugging")
