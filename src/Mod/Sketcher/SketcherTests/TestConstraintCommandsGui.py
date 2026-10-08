# SPDX-License-Identifier: LGPL-2.1-or-later

import FreeCAD as App
import Part
import Sketcher
from PySide import QtCore
from SketcherTests.GuiTestCase import FreeCADGui as Gui, SketcherGuiTestCase


class TestConstraintCommandsGui(SketcherGuiTestCase):
    def setUp(self):
        super().setUp()
        Gui.activateWorkbench("SketcherWorkbench")
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Sketcher")
        self.saved_params = {
            key: self.params.GetBool(key, True)
            for key in ("ContinuousConstraintMode", "ShowDialogOnDistanceConstraint")
        }
        self.params.SetBool("ContinuousConstraintMode", True)
        self.params.SetBool("ShowDialogOnDistanceConstraint", False)
        self.doc = App.newDocument("ConstraintCommands")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(50, 45, 0), App.Vector(10, 15, 0)), False
        )
        self.doc.recompute()
        Gui.activeDocument().setEdit(self.sketch.Name)
        self.flush_gui(50)
        self.view = Gui.activeDocument().activeView()
        self.view.viewTop()
        self.flush_gui(50)
        self.view.fitAll()
        self.flush_gui(150)
        self.viewport = self.view.graphicsView().viewport()

    def tearDown(self):
        try:
            super().tearDown()
        finally:
            for key, value in getattr(self, "saved_params", {}).items():
                self.params.SetBool(key, value)

    def select(self, *names):
        Gui.Selection.clearSelection()
        for name in names:
            Gui.Selection.addSelection(self.sketch, name)

    def assert_distance(self, axis, value, driving=True):
        self.assertEqual(self.sketch.ConstraintCount, 1)
        constraint = self.sketch.Constraints[0]
        self.assertEqual(constraint.Type, "Distance" + axis)
        self.assertAlmostEqual(constraint.Value, value, places=6)
        self.assertEqual(self.sketch.getDriving(0), driving)
        self.assertEqual(self.sketch.solve(), 0)
        self.assertEqual(Gui.Selection.getSelectionEx(), [])
        self.doc.undo()
        self.assertEqual(self.sketch.ConstraintCount, 0)
        self.doc.redo()
        self.assertEqual(self.sketch.ConstraintCount, 1)
        self.assertEqual(self.sketch.getDriving(0), driving)
        self.doc.undo()

    def click_world(self, point):
        pos = self.viewport_to_qpoint(self.view, self.viewport, self.view.getPointOnScreen(point))
        self.move(self.viewport, pos)
        self.click(self.viewport, pos)

    def test_coordinate_distances_from_selection(self):
        for axis, value in (("X", 40), ("Y", 30)):
            for selection in (("Edge1",), ("Vertex1", "Vertex2")):
                with self.subTest(axis=axis, selection=selection):
                    self.select(*selection)
                    Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                    self.assert_distance(axis, value)

    def test_coordinate_distances_from_continuous_picking(self):
        for axis, value in (("X", 40), ("Y", 30)):
            for points in (
                (App.Vector(30, 30, 0),),
                (App.Vector(50, 45, 0), App.Vector(10, 15, 0)),
            ):
                with self.subTest(axis=axis, points=points):
                    Gui.Selection.clearSelection()
                    Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                    for point in points:
                        self.click_world(point)
                    self.assert_distance(axis, value)

    def test_reference_coordinate_distances(self):
        Gui.runCommand("Sketcher_ToggleDrivingConstraint")
        try:
            for axis, value in (("X", 40), ("Y", 30)):
                with self.subTest(axis=axis):
                    self.select("Edge1")
                    Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                    self.assert_distance(axis, value, driving=False)
        finally:
            Gui.runCommand("Sketcher_ToggleDrivingConstraint")

    def test_fixed_geometry_produces_reference_datum(self):
        self.sketch.addConstraint(Sketcher.Constraint("Block", 0))
        self.select("Edge1")
        Gui.runCommand("Sketcher_ConstrainDistanceX")
        self.assertEqual(self.sketch.ConstraintCount, 2)
        self.assertFalse(self.sketch.getDriving(1))
        self.assertEqual(self.sketch.solve(), 0)

    def test_single_vertex_coordinates(self):
        for axis, value in (("X", 50), ("Y", 45)):
            with self.subTest(axis=axis):
                self.select("Vertex1")
                Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                self.assert_distance(axis, value)

    def test_fixed_vertex_coordinates_are_reference(self):
        self.sketch.addConstraint(Sketcher.Constraint("Block", 0))
        for axis, value in (("X", 50), ("Y", 45)):
            with self.subTest(axis=axis):
                self.select("Vertex1")
                Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                self.assertEqual(self.sketch.ConstraintCount, 2)
                self.assertEqual(self.sketch.Constraints[1].Type, "Distance" + axis)
                self.assertAlmostEqual(self.sketch.Constraints[1].Value, value)
                self.assertFalse(self.sketch.getDriving(1))
                self.assertEqual(self.sketch.solve(), 0)
                self.doc.undo()
                self.assertEqual(self.sketch.ConstraintCount, 1)

    def test_coordinate_distances_to_axes(self):
        for axis, name, value in (("X", "V_Axis", 50), ("Y", "H_Axis", 45)):
            for selection in ((name, "Vertex1"), ("Vertex1", name)):
                with self.subTest(axis=axis, selection=selection):
                    self.select(*selection)
                    Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                    self.assert_distance(axis, value)

    def test_external_vertex_coordinates(self):
        source = self.doc.addObject("Part::Feature", "ExternalLine")
        source.Shape = Part.makeLine(App.Vector(-20, -25, 0), App.Vector(-10, -15, 0))
        self.doc.recompute()
        self.sketch.addExternal(source.Name, "Edge1")
        self.doc.recompute()
        self.assertEqual(self.sketch.getGeoVertexIndex(2)[0], -3)
        for axis, value in (("X", -20), ("Y", -25)):
            with self.subTest(axis=axis):
                self.select("Vertex3")
                Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                self.assert_distance(axis, value, driving=False)
                # One external point must not force a movable point's distance to be reference.
                for selection in (("Vertex3", "Vertex1"), ("Vertex1", "Vertex3")):
                    self.select(*selection)
                    Gui.runCommand("Sketcher_ConstrainDistance" + axis)
                    self.assert_distance(axis, 70, driving=True)

    def test_radial_dimensions(self):
        circle = Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 10)
        self.sketch.addGeometry(circle, False)
        self.sketch.addGeometry(Part.ArcOfCircle(circle, 0.2, 2.0), False)
        self.doc.recompute()
        for command in ("Radius", "Diameter", "Radiam"):
            for edge, kind, value in (
                (
                    "Edge2",
                    "Radius" if command == "Radius" else "Diameter",
                    10 if command == "Radius" else 20,
                ),
                (
                    "Edge3",
                    "Diameter" if command == "Diameter" else "Radius",
                    20 if command == "Diameter" else 10,
                ),
            ):
                for reference in (False, True):
                    with self.subTest(command=command, edge=edge, reference=reference):
                        if reference:
                            Gui.runCommand("Sketcher_ToggleDrivingConstraint")
                        try:
                            self.select(edge)
                            Gui.runCommand("Sketcher_Constrain" + command)
                            self.assertEqual(self.sketch.ConstraintCount, 1)
                            self.assertEqual(self.sketch.Constraints[0].Type, kind)
                            self.assertAlmostEqual(self.sketch.Constraints[0].Value, value)
                            self.assertEqual(self.sketch.getDriving(0), not reference)
                            self.assertEqual(self.sketch.solve(), 0)
                            self.doc.undo()
                            self.assertEqual(self.sketch.ConstraintCount, 0)
                            self.doc.redo()
                            self.assertEqual(self.sketch.Constraints[0].Type, kind)
                            self.doc.undo()
                        finally:
                            if reference:
                                Gui.runCommand("Sketcher_ToggleDrivingConstraint")

    def test_radial_dimensions_from_continuous_picking(self):
        self.sketch.addGeometry(Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 10), False)
        self.doc.recompute()
        self.view.fitAll()
        self.flush_gui(150)
        for command in ("Radius", "Diameter", "Radiam"):
            with self.subTest(command=command):
                Gui.Selection.clearSelection()
                Gui.runCommand("Sketcher_Constrain" + command)
                # Pick away from the sketch axes so the circle is unambiguous.
                self.click_world(App.Vector(-8, 6, 0))
                self.assertEqual(self.sketch.ConstraintCount, 1)
                expected = "Radius" if command == "Radius" else "Diameter"
                self.assertEqual(self.sketch.Constraints[0].Type, expected)
                self.assertAlmostEqual(
                    self.sketch.Constraints[0].Value, 10 if command == "Radius" else 20
                )
                self.assertTrue(self.sketch.getDriving(0))
                self.assertEqual(self.sketch.solve(), 0)
                self.doc.undo()
                self.assertEqual(self.sketch.ConstraintCount, 0)

    def test_multiple_radial_dimensions(self):
        circle = Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 10)
        self.sketch.addGeometry(circle, False)
        self.sketch.addGeometry(Part.ArcOfCircle(circle, 0.2, 2.0), False)
        self.doc.recompute()
        for command in ("Radius", "Diameter", "Radiam"):
            for edges in (("Edge2", "Edge3"), ("Edge3", "Edge2")):
                for reference in (False, True):
                    with self.subTest(command=command, edges=edges, reference=reference):
                        if reference:
                            Gui.runCommand("Sketcher_ToggleDrivingConstraint")
                        try:
                            self.select(*edges)
                            Gui.runCommand("Sketcher_Constrain" + command)
                            expected = [
                                (
                                    "Diameter"
                                    if command == "Diameter"
                                    or (command == "Radiam" and edge == "Edge2")
                                    else "Radius"
                                )
                                for edge in edges
                            ]
                            if not reference:
                                expected = ["Equal", expected[0]]
                            self.assertEqual([c.Type for c in self.sketch.Constraints], expected)
                            self.assertEqual(self.sketch.solve(), 0)
                            for index in (range(2) if reference else (1,)):
                                self.assertEqual(self.sketch.getDriving(index), not reference)
                            self.doc.undo()
                            self.assertEqual(self.sketch.ConstraintCount, 0)
                        finally:
                            if reference:
                                Gui.runCommand("Sketcher_ToggleDrivingConstraint")

    def test_fixed_and_external_radial_dimensions(self):
        circle = Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 10)
        self.sketch.addGeometry(circle, False)
        self.sketch.addConstraint(Sketcher.Constraint("Block", 1))
        source = self.doc.addObject("Part::Feature", "ExternalCircle")
        source.Shape = circle.toShape()
        self.doc.recompute()
        self.sketch.addExternal(source.Name, "Edge1")
        self.doc.recompute()
        for command in ("Radius", "Diameter", "Radiam"):
            for edge in ("Edge2", "ExternalEdge1"):
                with self.subTest(command=command, edge=edge):
                    self.select(edge)
                    Gui.runCommand("Sketcher_Constrain" + command)
                    self.assertEqual(self.sketch.ConstraintCount, 2)
                    expected = "Radius" if command == "Radius" else "Diameter"
                    self.assertEqual(self.sketch.Constraints[1].Type, expected)
                    self.assertFalse(self.sketch.getDriving(1))
                    self.assertEqual(self.sketch.solve(), 0)
                    self.doc.undo()
                    self.assertEqual(self.sketch.ConstraintCount, 1)

    def test_radial_dimensions_on_bspline_weights(self):
        spline = Part.BSplineCurve()
        spline.interpolate([App.Vector(0, 0, 0), App.Vector(10, 20, 0), App.Vector(30, 0, 0)])
        self.sketch.addGeometry(spline, False)
        self.sketch.exposeInternalGeometry(1)
        self.doc.recompute()
        pole = next(
            index for index, geo in enumerate(self.sketch.Geometry) if isinstance(geo, Part.Circle)
        )
        # Exposing a non-rational spline fixes its first weight automatically.
        weight = next(
            index
            for index, constraint in enumerate(self.sketch.Constraints)
            if constraint.Type == "Weight" and constraint.First == pole
        )
        self.sketch.delConstraint(weight)
        count = self.sketch.ConstraintCount
        for command in ("Radius", "Radiam"):
            with self.subTest(command=command):
                self.select("Edge" + str(pole + 1))
                Gui.runCommand("Sketcher_Constrain" + command)
                self.assertEqual(self.sketch.ConstraintCount, count + 1)
                self.assertEqual(self.sketch.Constraints[-1].Type, "Weight")
                self.assertTrue(self.sketch.getDriving(count))
                self.assertEqual(self.sketch.solve(), 0)
                self.doc.undo()
                self.assertEqual(self.sketch.ConstraintCount, count)

    def test_disabled_continuous_mode_releases_previous_handler(self):
        notifications = App.ParamGet("User parameter:BaseApp/Preferences/NotificationArea")
        saved = notifications.GetBool("NonIntrusiveNotificationsEnabled", True)
        notifications.SetBool("NonIntrusiveNotificationsEnabled", True)
        try:
            for name in ("DistanceX", "Perpendicular", "Tangent", "Equal", "Symmetric"):
                with self.subTest(command=name):
                    self.params.SetBool("ContinuousConstraintMode", True)
                    Gui.Selection.clearSelection()
                    Gui.runCommand("Sketcher_ConstrainDistanceX")
                    self.params.SetBool("ContinuousConstraintMode", False)
                    Gui.runCommand("Sketcher_Constrain" + name)
                    self.flush_gui()
                    self.assertEqual(self.sketch.ConstraintCount, 0)
                    self.assertEqual(self.viewport.cursor().shape(), QtCore.Qt.ArrowCursor)
        finally:
            notifications.SetBool("NonIntrusiveNotificationsEnabled", saved)

    def test_switching_continuous_commands_preserves_sketch(self):
        for name in (
            "Horizontal",
            "Vertical",
            "HorVer",
            "Lock",
            "Block",
            "Coincident",
            "PointOnObject",
            "Distance",
            "DistanceX",
            "DistanceY",
            "Parallel",
            "Perpendicular",
            "Tangent",
            "Radius",
            "Diameter",
            "Radiam",
            "Angle",
            "Equal",
            "Symmetric",
        ):
            with self.subTest(command=name):
                Gui.Selection.clearSelection()
                Gui.runCommand("Sketcher_Constrain" + name)
                self.flush_gui()
                self.assertEqual(self.sketch.ConstraintCount, 0)
                self.assertIsNotNone(Gui.activeDocument().getInEdit())
                self.assertEqual(self.viewport.cursor().shape(), QtCore.Qt.BitmapCursor)

    # --- Symmetric constraint: two elements and a center edge -----------------

    @staticmethod
    def mirror_point(point, axis_start, axis_end):
        direction = axis_end - axis_start
        offset = point - axis_start
        projection = axis_start + direction * (offset.dot(direction) / direction.dot(direction))
        return projection * 2 - point

    def axis_points(self, geo_id):
        # GeoId -1 is the horizontal axis, -2 the vertical axis. They are not
        # part of sketch.Geometry, so their points are given explicitly.
        if geo_id == -2:
            return App.Vector(0, 0, 0), App.Vector(0, 1, 0)
        if geo_id == -1:
            return App.Vector(0, 0, 0), App.Vector(1, 0, 0)
        return self.sketch.getPoint(geo_id, 1), self.sketch.getPoint(geo_id, 2)

    def assert_mirrored(self, geo1, geo2, axis_geo):
        axis_start, axis_end = self.axis_points(axis_geo)
        for pos, name in ((1, "start"), (2, "end")):
            first = self.sketch.getPoint(geo1, pos)
            second = self.sketch.getPoint(geo2, pos)
            expected = self.mirror_point(first, axis_start, axis_end)
            self.assertAlmostEqual(second.x, expected.x, places=6, msg=name)
            self.assertAlmostEqual(second.y, expected.y, places=6, msg=name)

    def add_symmetric_test_geometry(self):
        # Edge1 is (50, 45) -> (10, 15). Edge2 deliberately is *not* its mirror,
        # so that the tests prove the solver enforces the symmetry. Edge3 is a
        # center (symmetry) line.
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(-40, 30, 0), App.Vector(-5, 10, 0)), False
        )
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(20, -80, 0), App.Vector(20, 80, 0)), False
        )
        self.doc.recompute()

    def test_symmetric_two_edges_and_center_edge(self):
        self.add_symmetric_test_geometry()
        self.select("Edge1", "Edge2", "Edge3")
        Gui.runCommand("Sketcher_ConstrainSymmetric")

        # One Symmetric constraint per corresponding endpoint pair.
        self.assertEqual(self.sketch.ConstraintCount, 2)
        for constraint in self.sketch.Constraints:
            self.assertEqual(constraint.Type, "Symmetric")
            self.assertEqual((constraint.First, constraint.Second, constraint.Third), (0, 1, 2))
            self.assertEqual(constraint.ThirdPos, 0)
        self.assertEqual(
            (self.sketch.Constraints[0].FirstPos, self.sketch.Constraints[0].SecondPos), (1, 1)
        )
        self.assertEqual(
            (self.sketch.Constraints[1].FirstPos, self.sketch.Constraints[1].SecondPos), (2, 2)
        )
        self.assertEqual(self.sketch.solve(), 0)
        self.assert_mirrored(0, 1, 2)

        self.doc.undo()
        self.assertEqual(self.sketch.ConstraintCount, 0)

    def test_symmetric_center_edge_any_position_with_axis(self):
        self.add_symmetric_test_geometry()
        for selection in (
            ("V_Axis", "Edge1", "Edge2"),
            ("Edge1", "V_Axis", "Edge2"),
            ("Edge1", "Edge2", "V_Axis"),
        ):
            with self.subTest(selection=selection):
                self.select(*selection)
                Gui.runCommand("Sketcher_ConstrainSymmetric")

                self.assertEqual(self.sketch.ConstraintCount, 2)
                for constraint in self.sketch.Constraints:
                    self.assertEqual(constraint.Type, "Symmetric")
                    self.assertEqual(constraint.Third, -2)
                    self.assertEqual(constraint.ThirdPos, 0)
                self.assertEqual(self.sketch.solve(), 0)
                self.assert_mirrored(0, 1, -2)

                self.doc.undo()
                self.assertEqual(self.sketch.ConstraintCount, 0)

    def test_symmetric_center_edge_any_position_when_unique_line(self):
        # The two elements are arcs, so the only line among the three selections
        # is the center edge and must be used as the axis whatever its position.
        first = Part.Circle(App.Vector(30, 5, 0), App.Vector(0, 0, 1), 3)
        second = Part.Circle(App.Vector(50, -3, 0), App.Vector(0, 0, 1), 2)
        self.sketch.addGeometry(Part.ArcOfCircle(first, 0.0, 1.0), False)
        self.sketch.addGeometry(Part.ArcOfCircle(second, 2.0, 3.0), False)
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(20, -80, 0), App.Vector(20, 80, 0)), False
        )
        self.doc.recompute()

        for selection in (
            ("Edge4", "Edge2", "Edge3"),
            ("Edge2", "Edge4", "Edge3"),
            ("Edge2", "Edge3", "Edge4"),
        ):
            with self.subTest(selection=selection):
                self.select(*selection)
                Gui.runCommand("Sketcher_ConstrainSymmetric")

                self.assertEqual(self.sketch.ConstraintCount, 2)
                for constraint in self.sketch.Constraints:
                    self.assertEqual(constraint.Type, "Symmetric")
                    self.assertEqual(constraint.Third, 3)
                    self.assertEqual(constraint.ThirdPos, 0)
                self.assertEqual(self.sketch.solve(), 0)
                self.assert_mirrored(1, 2, 3)

                self.doc.undo()
                self.assertEqual(self.sketch.ConstraintCount, 0)

    def test_symmetric_two_edges_and_external_center_edge(self):
        source = self.doc.addObject("Part::Feature", "ExternalCenterLine")
        source.Shape = Part.makeLine(App.Vector(20, -80, 0), App.Vector(20, 80, 0))
        self.doc.recompute()
        self.sketch.addExternal(source.Name, "Edge1")
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(-40, 30, 0), App.Vector(-5, 10, 0)), False
        )
        self.doc.recompute()

        self.select("Edge1", "Edge2", "ExternalEdge1")
        Gui.runCommand("Sketcher_ConstrainSymmetric")

        self.assertEqual(self.sketch.ConstraintCount, 2)
        for constraint in self.sketch.Constraints:
            self.assertEqual(constraint.Type, "Symmetric")
            self.assertEqual(constraint.Third, -3)
            self.assertEqual(constraint.ThirdPos, 0)
        self.assertEqual(self.sketch.solve(), 0)
        self.assert_mirrored(0, 1, -3)

    def test_symmetric_non_line_center_edge_is_rejected(self):
        circle = Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 20)
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(-40, 30, 0), App.Vector(-5, 10, 0)), False
        )
        self.sketch.addGeometry(Part.ArcOfCircle(circle, 0.1, 1.0), False)
        self.doc.recompute()

        # Edge3 (an arc) is selected as the center edge and is not a line.
        self.select("Edge1", "Edge2", "Edge3")
        Gui.runCommand("Sketcher_ConstrainSymmetric")
        self.assertEqual(self.sketch.ConstraintCount, 0)

    def test_symmetric_element_without_endpoints_is_rejected(self):
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(-40, 30, 0), App.Vector(-5, 10, 0)), False
        )
        self.sketch.addGeometry(Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 20), False)
        self.doc.recompute()

        # Edge2 is a circle, so it has no endpoints to make symmetric.
        self.select("Edge1", "Edge2", "Edge3")
        Gui.runCommand("Sketcher_ConstrainSymmetric")
        self.assertEqual(self.sketch.ConstraintCount, 0)

    def test_symmetric_all_fixed_is_rejected(self):
        self.add_symmetric_test_geometry()
        for geo in range(3):
            self.sketch.addConstraint(Sketcher.Constraint("Block", geo))
        self.doc.recompute()

        self.select("Edge1", "Edge2", "Edge3")
        Gui.runCommand("Sketcher_ConstrainSymmetric")
        # Only the three Block constraints remain.
        self.assertEqual(self.sketch.ConstraintCount, 3)

    def test_symmetric_two_points_and_line_still_works(self):
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(20, -80, 0), App.Vector(20, 80, 0)), False
        )
        self.doc.recompute()

        self.select("Vertex1", "Vertex2", "Edge2")
        Gui.runCommand("Sketcher_ConstrainSymmetric")

        self.assertEqual(self.sketch.ConstraintCount, 1)
        constraint = self.sketch.Constraints[0]
        self.assertEqual(constraint.Type, "Symmetric")
        self.assertEqual(
            (constraint.First, constraint.FirstPos, constraint.Second, constraint.SecondPos),
            (0, 1, 0, 2),
        )
        self.assertEqual((constraint.Third, constraint.ThirdPos), (1, 0))
        self.assertEqual(self.sketch.solve(), 0)

    def test_symmetric_two_points_and_point_still_works(self):
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(-50, 45, 0), App.Vector(-10, 15, 0)), False
        )
        self.doc.recompute()

        self.select("Vertex1", "Vertex4", "Vertex2")
        Gui.runCommand("Sketcher_ConstrainSymmetric")

        self.assertEqual(self.sketch.ConstraintCount, 1)
        constraint = self.sketch.Constraints[0]
        self.assertEqual(constraint.Type, "Symmetric")
        self.assertEqual(
            (constraint.First, constraint.FirstPos, constraint.Second, constraint.SecondPos),
            (0, 1, 1, 1),
        )
        self.assertEqual((constraint.Third, constraint.ThirdPos), (0, 2))
        self.assertEqual(self.sketch.solve(), 0)

    def test_symmetric_element_and_point_still_works(self):
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(-50, 45, 0), App.Vector(-10, 15, 0)), False
        )
        self.doc.recompute()

        self.select("Edge1", "Vertex4")
        Gui.runCommand("Sketcher_ConstrainSymmetric")

        self.assertEqual(self.sketch.ConstraintCount, 1)
        constraint = self.sketch.Constraints[0]
        self.assertEqual(constraint.Type, "Symmetric")
        self.assertEqual(
            (
                constraint.First,
                constraint.FirstPos,
                constraint.Second,
                constraint.SecondPos,
                constraint.Third,
                constraint.ThirdPos,
            ),
            (0, 1, 0, 2, 1, 1),
        )
        self.assertEqual(self.sketch.solve(), 0)

    def test_symmetric_element_and_line_still_works(self):
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(20, -80, 0), App.Vector(20, 80, 0)), False
        )
        self.doc.recompute()

        # The first selected element is the one made symmetric, the second one is
        # the symmetry line (unchanged behaviour).
        for selection, element, line in (
            (("Edge1", "Edge2"), 0, 1),
            (("Edge2", "Edge1"), 1, 0),
        ):
            with self.subTest(selection=selection):
                self.select(*selection)
                Gui.runCommand("Sketcher_ConstrainSymmetric")

                self.assertEqual(self.sketch.ConstraintCount, 1)
                constraint = self.sketch.Constraints[0]
                self.assertEqual(constraint.Type, "Symmetric")
                self.assertEqual(
                    (
                        constraint.First,
                        constraint.FirstPos,
                        constraint.Second,
                        constraint.SecondPos,
                    ),
                    (element, 1, element, 2),
                )
                self.assertEqual((constraint.Third, constraint.ThirdPos), (line, 0))
                self.assertEqual(self.sketch.solve(), 0)

                self.doc.undo()
                self.assertEqual(self.sketch.ConstraintCount, 0)

