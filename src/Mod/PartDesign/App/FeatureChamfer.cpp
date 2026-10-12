// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2010 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include <limits>

#include <BRepAlgo.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Compound.hxx>
#include <GProp_GProps.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>

#include <sstream>
#include <Standard_Version.hxx>


#include <Base/Exception.h>
#include <Base/Reader.h>
#include <Base/Tools.h>
#include <Mod/Part/App/SignalException.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureChamfer.h"

#include <Base/ProgramVersion.h>


using namespace PartDesign;


PROPERTY_SOURCE(PartDesign::Chamfer, PartDesign::DressUp)

const char* ChamferTypeEnums[] = {"Equal distance", "Two distances", "Distance and Angle", nullptr};
const App::PropertyQuantityConstraint::Constraints Chamfer::floatSize
    = {0.0, std::numeric_limits<float>::max(), 0.1};
const App::PropertyAngle::Constraints Chamfer::floatAngle = {0.0, 180.0, 1.0};

namespace {

/** Why a chamfer result has to be refused, or an empty string when it is usable.
 *
 * A chamfer with sizes that are too large for the edge can leave a face with a
 * non-positive area (or a duplicated face) in a shape BRepCheck still calls
 * valid.  Accepting that would hand the user a solid that looks up to date but
 * that later operations (fillet, chamfer, export) fail on, so report it here
 * while the sizes that caused it are still known.
 */
std::string invalidChamferReason(const Part::TopoShape& shape)
{
    for (const auto& face : shape.getSubTopoShapes(TopAbs_FACE)) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face.getShape(), props);
        const double area = props.Mass();
        if (area <= Precision::Confusion()) {
            std::ostringstream ss;
            ss << "face with non-positive area " << area;
            return ss.str();
        }
    }
    if (!shape.isValid()) {
        return "the shape is not valid";
    }
    return {};
}


// ---------------------------------------------------------------------------
// Wedge-cut fallback.
//
// OCCT's ChFi3d cannot represent a chamfer whose distance does not fit the face
// it is measured from: it either throws, folds a face past its boundary (a
// negative-area face) or produces a shape BRepCheck rejects.  The equivalent
// solid is a wedge cut, which a user would model by hand: subtract a prism whose
// cross-section is the chamfer triangle, swept along the edge.  A cut does not
// care that the wedge reaches past the adjacent faces.
//
// Only straight edges between two planar faces qualify; anything else keeps the
// original error.
// ---------------------------------------------------------------------------

//! Normal of a planar face, whatever surface type it is stored as: a boolean can
//! leave a planar face as a B-spline surface, exactly as it does for edges.
//! False when the face is not planar to within a millionth of its own size.
bool planarFaceNormal(const TopoDS_Face& face, gp_Dir& normal)
{
    BRepAdaptor_Surface surface(face, Standard_False);
    if (surface.GetType() == GeomAbs_Plane) {
        normal = surface.Plane().Axis().Direction();
        return true;
    }
    std::vector<gp_Pnt> points;
    for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
        points.push_back(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
    }
    if (points.size() < 3) {
        return false;
    }
    double size = 0.0;
    bool found = false;
    for (std::size_t i = 1; !found && i + 1 < points.size(); ++i) {
        const gp_Vec a(points[0], points[i]);
        const gp_Vec b(points[0], points[i + 1]);
        const gp_Vec cross = a.Crossed(b);
        if (cross.Magnitude() > Precision::Confusion()) {
            normal = gp_Dir(cross);
            size = std::max(a.Magnitude(), b.Magnitude());
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    const double tolerance = std::max(Precision::Confusion(), size * 1e-4);
    for (const auto& point : points) {
        if (std::abs(gp_Vec(points[0], point).Dot(gp_Vec(normal))) > tolerance) {
            return false;
        }
    }
    // the vertices can be coplanar on a curved face: sample the surface as well
    const double u0 = surface.FirstUParameter();
    const double u1 = surface.LastUParameter();
    const double v0 = surface.FirstVParameter();
    const double v1 = surface.LastVParameter();
    for (double su : {0.25, 0.5, 0.75}) {
        for (double sv : {0.25, 0.5, 0.75}) {
            const gp_Pnt point = surface.Value(u0 + (u1 - u0) * su, v0 + (v1 - v0) * sv);
            if (std::abs(gp_Vec(points[0], point).Dot(gp_Vec(normal))) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

//! End points of a straight edge.  A boolean often leaves an edge that is
//! geometrically a line but stored as a B-spline, so the curve type is not
//! checked; a curve that deviates from its chord by more than a ten-thousandth
//! of its length is rejected (the wedge prism is built on that chord).
bool straightEdgeEnds(const TopoDS_Edge& edge, gp_Pnt& start, gp_Pnt& end)
{
    if (BRep_Tool::Degenerated(edge)) {
        return false;
    }
    BRepAdaptor_Curve curve(edge);
    start = curve.Value(curve.FirstParameter());
    end = curve.Value(curve.LastParameter());
    const double length = start.Distance(end);
    if (length < Precision::Confusion()) {
        return false;
    }
    if (curve.GetType() == GeomAbs_Line) {
        return true;
    }
    const double tolerance = std::max(Precision::Confusion(), length * 1e-4);
    const double first = curve.FirstParameter();
    const double last = curve.LastParameter();
    for (double t : {0.25, 0.5, 0.75}) {
        const gp_Pnt onCurve = curve.Value(first + (last - first) * t);
        const gp_Pnt onChord = start.Translated(gp_Vec(start, end) * t);
        if (onCurve.Distance(onChord) > tolerance) {
            return false;
        }
    }
    return true;
}

//! In-plane direction from the edge into the interior of a face.
bool interiorDirection(const TopoDS_Face& face, const gp_Pnt& origin, const gp_Dir& axis, gp_Dir& out)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    gp_Vec dir(origin, props.CentreOfMass());
    dir -= gp_Vec(axis) * dir.Dot(gp_Vec(axis));
    if (dir.SquareMagnitude() < Precision::SquareConfusion()) {
        return false;
    }
    out = gp_Dir(dir);
    return true;
}

//! +1 for a convex edge (a chamfer takes material off), -1 for a concave one (a
//! chamfer fills the valley) and 0 when that cannot be told.
//!
//! Offset the edge midpoint along the sum of the two in-plane directions towards
//! each adjacent face's interior: inside the solid means the faces bound the
//! material (convex), in the void means they bound a valley (concave).  The
//! face-normal bisector does *not* separate the two - it points into the void
//! either way.
double chamferConvexity(const Part::TopoShape& base, const std::vector<Part::TopoShape>& edges)
{
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(base.getShape(), TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    double convexity = 0.0;
    for (const auto& edgeShape : edges) {
        if (edgeShape.isNull() || edgeShape.getShape().ShapeType() != TopAbs_EDGE) {
            return 0.0;
        }
        const TopoDS_Edge& edge = TopoDS::Edge(edgeShape.getShape());
        if (!edgeFaces.Contains(edge) || edgeFaces.FindFromKey(edge).Extent() != 2) {
            return 0.0;
        }
        gp_Pnt start;
        gp_Pnt end;
        if (!straightEdgeEnds(edge, start, end)) {
            return 0.0;
        }
        const gp_Dir axis(gp_Vec(start, end));
        gp_Dir leg0;
        gp_Dir leg1;
        if (!interiorDirection(TopoDS::Face(edgeFaces.FindFromKey(edge).First()), start, axis, leg0)
            || !interiorDirection(
                TopoDS::Face(edgeFaces.FindFromKey(edge).Last()),
                start,
                axis,
                leg1
            )) {
            return 0.0;
        }
        const gp_Pnt midpoint((start.XYZ() + end.XYZ()) * 0.5);
        const gp_Pnt probe = midpoint.Translated(gp_Vec(leg0) * 1e-4 + gp_Vec(leg1) * 1e-4);
        BRepClass3d_SolidClassifier classifier(base.getShape());
        classifier.Perform(probe, Precision::Confusion());
        double sign = 0.0;
        if (classifier.State() == TopAbs_IN) {
            sign = 1.0;
        }
        else if (classifier.State() == TopAbs_OUT) {
            sign = -1.0;
        }
        else {
            return 0.0;
        }
        if (convexity != 0.0 && convexity != sign) {
            return 0.0;  // the selected edges disagree
        }
        convexity = sign;
    }
    return convexity;
}

//! True when the centroid of the triangle a-b-c lies inside \a base.
bool triangleIsInside(const Part::TopoShape& base, const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c)
{
    const gp_Pnt centroid((a.XYZ() + b.XYZ() + c.XYZ()) / 3.0);
    BRepClass3d_SolidClassifier classifier(base.getShape());
    classifier.Perform(centroid, Precision::Confusion());
    return classifier.State() == TopAbs_IN;
}

//! Volume of the wedge a chamfer of these sizes takes off each straight edge
//! between two planar faces: 0.5 * size0 * size1 * length.
double chamferWedgeVolume(const Part::TopoShape& base,
                          const std::vector<Part::TopoShape>& edges,
                          double size0,
                          double size1)
{
    double volume = 0.0;
    for (const auto& edgeShape : edges) {
        if (edgeShape.isNull() || edgeShape.getShape().ShapeType() != TopAbs_EDGE) {
            return 0.0;
        }
        gp_Pnt start;
        gp_Pnt end;
        if (!straightEdgeEnds(TopoDS::Edge(edgeShape.getShape()), start, end)) {
            return 0.0;
        }
        volume += 0.5 * size0 * size1 * start.Distance(end);
    }
    return volume;
}

struct WedgeLegs
{
    gp_Pnt start;
    gp_Dir direction;
    double length = 0.0;
    gp_Dir leg0;
    gp_Dir leg1;
    double size0 = 0.0;
    double size1 = 0.0;
};

double shapeVolume(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

//! In-plane direction from the edge into the interior of a planar face.

bool wedgeLegs(const TopoDS_Edge& edge,
               const TopoDS_Face& face0,
               const TopoDS_Face& face1,
               double size0,
               double size1,
               WedgeLegs& legs)
{
    gp_Pnt start;
    gp_Pnt end;
    if (!straightEdgeEnds(edge, start, end)) {
        return false;
    }
    for (const TopoDS_Face& face : {face0, face1}) {
        gp_Dir normal;
        if (!planarFaceNormal(face, normal)) {
            return false;
        }
    }
    legs.start = start;
    legs.length = start.Distance(end);
    legs.direction = gp_Dir(gp_Vec(legs.start, end));
    if (!interiorDirection(face0, legs.start, legs.direction, legs.leg0)
        || !interiorDirection(face1, legs.start, legs.direction, legs.leg1)) {
        return false;
    }
    legs.size0 = size0;
    legs.size1 = size1;
    return true;
}

//! The wedge prism for one edge.  It spans the edge *exactly*: overshooting the
//! ends would cut into whatever lies beyond them (a through-all variant removed
//! 2819 mm3 where the chamfer removes 1270.75).
TopoDS_Shape wedgeTool(const WedgeLegs& legs, int sign0, int sign1)
{
    BRepBuilderAPI_MakePolygon polygon;
    polygon.Add(legs.start);
    polygon.Add(legs.start.Translated(gp_Vec(legs.leg0) * (legs.size0 * sign0)));
    polygon.Add(legs.start.Translated(gp_Vec(legs.leg1) * (legs.size1 * sign1)));
    polygon.Close();
    BRepBuilderAPI_MakeFace face(polygon.Wire());
    if (!face.IsDone()) {
        return {};
    }
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(legs.direction) * legs.length);
}

//! Volume of the wedge a chamfer of these sizes takes off each straight edge
//! between two planar faces: 0.5 * size0 * size1 * length.  This is the volume
//! the result must lose - a chamfer only ever takes material off.

//! Chamfers each edge by subtracting a wedge prism, and returns the cut shape
//! (or a null shape when that is not possible).
Part::TopoShape chamferEdgesByWedge(const Part::TopoShape& base,
                                    const std::vector<Part::TopoShape>& edges,
                                    Part::ChamferType type,
                                    double size,
                                    double size2,
                                    bool flip,
                                    double convexity)
{
    // Both assignments of the two distances are tried: OCC measures the first
    // one on the face the caller picked, and for this fallback it does not
    // matter which face that is.
    std::vector<std::pair<double, double>> assignments;
    if (type == Part::ChamferType::twoDistances) {
        assignments = {{size, size2}, {size2, size}};
    }
    else if (type == Part::ChamferType::distanceAngle) {
        // Chamfer::execute() hands the angle (in degrees) over in size2
        const double radians = Base::toRadians(size2);
        if (std::abs(std::tan(radians)) < Precision::Confusion()) {
            return Part::TopoShape(0);
        }
        assignments = {{size, size * std::tan(radians)}};
    }
    else {
        assignments = {{size, size}};
    }
    if (flip && assignments.size() > 1) {
        std::swap(assignments.front(), assignments.back());
    }
    for (const auto& [first, second] : assignments) {
        if (first <= Precision::Confusion() || second <= Precision::Confusion()) {
            return Part::TopoShape(0);
        }
    }
    if (convexity == 0.0) {
        convexity = 1.0;  // unknown: never grow the solid
    }

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(base.getShape(), TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    const double baseVolume = shapeVolume(base.getShape());
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    int used = 0;
    for (const auto& edgeShape : edges) {
        if (edgeShape.isNull() || edgeShape.getShape().ShapeType() != TopAbs_EDGE) {
            return Part::TopoShape(0);
        }
        const TopoDS_Edge& edge = TopoDS::Edge(edgeShape.getShape());
        if (!edgeFaces.Contains(edge) || edgeFaces.FindFromKey(edge).Extent() != 2) {
            return Part::TopoShape(0);
        }
        // Pick the triangle by the side of the edge: a convex edge is chamfered
        // by subtracting the triangle whose centroid lies inside the material, a
        // concave one by fusing the one whose centroid lies in the void.  The
        // analytic volume 0.5*size0*size1*length only breaks ties - for a
        // concave edge most of the tool stands in the open, so the result gains
        // far less than that (0.5*17*2.99*34.83 = 885 mm3 against the measured
        // 186 mm3 on the user's Pocket002.Edge80).
        double bestError = std::numeric_limits<double>::max();
        TopoDS_Shape best;
        for (const auto& [size0, size1] : assignments) {
            WedgeLegs legs;
            if (!wedgeLegs(
                    edge,
                    TopoDS::Face(edgeFaces.FindFromKey(edge).First()),
                    TopoDS::Face(edgeFaces.FindFromKey(edge).Last()),
                    size0,
                    size1,
                    legs
                )) {
                return Part::TopoShape(0);
            }
            const double expected = 0.5 * size0 * size1 * legs.length;
            for (int sign0 : {-1, 1}) {
                for (int sign1 : {-1, 1}) {
                    const gp_Pnt corner0
                        = legs.start.Translated(gp_Vec(legs.leg0) * (legs.size0 * sign0));
                    const gp_Pnt corner1
                        = legs.start.Translated(gp_Vec(legs.leg1) * (legs.size1 * sign1));
                    if (triangleIsInside(base, legs.start, corner0, corner1) != (convexity > 0.0)) {
                        continue;  // wrong side of the edge
                    }
                    const TopoDS_Shape tool = wedgeTool(legs, sign0, sign1);
                    if (tool.IsNull()) {
                        continue;
                    }
                    Part::TopoShape toolShape(0);
                    toolShape.setShape(tool);
                    const Part::TopoShape candidate = convexity > 0.0
                        ? base.makeElementCut(toolShape, Part::OpCodes::Chamfer)
                        : base.makeElementFuse(toolShape, Part::OpCodes::Chamfer);
                    if (candidate.isNull()) {
                        continue;
                    }
                    const double change = baseVolume - shapeVolume(candidate.getShape());
                    const double error = std::abs(change - convexity * expected);
                    if (error < bestError) {
                        bestError = error;
                        best = tool;
                    }
                }
            }
        }
        if (best.IsNull()) {
            return Part::TopoShape(0);  // nothing on the side of the edge the chamfer needs
        }
        builder.Add(compound, best);
        ++used;
    }
    if (used == 0) {
        return Part::TopoShape(0);
    }

    Part::TopoShape toolShape(0);
    toolShape.setShape(compound);
    // a concave edge fills the valley, so the wedge is added rather than cut
    return convexity > 0.0 ? base.makeElementCut(toolShape, Part::OpCodes::Chamfer)
                           : base.makeElementFuse(toolShape, Part::OpCodes::Chamfer);
}

}  // namespace

static App::DocumentObjectExecReturn* validateParameters(
    int chamferType,
    double size,
    double size2,
    double angle
);

Chamfer::Chamfer()
{
    ADD_PROPERTY_TYPE(ChamferType, (0L), "Chamfer", App::Prop_None, "Type of chamfer");
    ChamferType.setEnums(ChamferTypeEnums);

    ADD_PROPERTY_TYPE(Size, (1.0), "Chamfer", App::Prop_None, "Size of chamfer");
    Size.setUnit(Base::Unit::Length);
    Size.setConstraints(&floatSize);

    ADD_PROPERTY_TYPE(Size2, (1.0), "Chamfer", App::Prop_None, "Second size of chamfer");
    Size2.setUnit(Base::Unit::Length);
    Size2.setConstraints(&floatSize);

    ADD_PROPERTY_TYPE(Angle, (45.0), "Chamfer", App::Prop_None, "Angle of chamfer");
    Angle.setUnit(Base::Unit::Angle);
    Angle.setConstraints(&floatAngle);

    ADD_PROPERTY_TYPE(FlipDirection, (false), "Chamfer", App::Prop_None, "Flip direction");
    ADD_PROPERTY_TYPE(
        UseAllEdges,
        (false),
        "Chamfer",
        App::Prop_None,
        "Chamfer all edges if true, else use only those edges in Base property.\n"
        "If true, then this overrides any edge changes made to the Base property or in the "
        "dialog.\n"
    );

    updateProperties();
}

short Chamfer::mustExecute() const
{
    bool touched = false;

    auto chamferType = ChamferType.getValue();

    switch (chamferType) {
        case 0:  // "Equal distance"
            touched = Size.isTouched() || ChamferType.isTouched();
            break;
        case 1:  // "Two distances"
            touched = Size.isTouched() || ChamferType.isTouched() || Size2.isTouched();
            break;
        case 2:  // "Distance and Angle"
            touched = Size.isTouched() || ChamferType.isTouched() || Angle.isTouched();
            break;
    }

    if (Placement.isTouched() || touched) {
        return 1;
    }
    return DressUp::mustExecute();
}

App::DocumentObjectExecReturn* Chamfer::execute()
{
    if (onlyHaveRefined()) {
        return App::DocumentObject::StdReturn;
    }

    // NOTE: Normally the Base property and the BaseFeature property should point to the same object.
    // The only difference is that the Base property also stores the edges that are to be chamfered
    Part::TopoShape TopShape;
    try {
        TopShape = getBaseTopoShape();
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }

    TopShape.setTransform(Base::Matrix4D());

    auto edges = UseAllEdges.getValue() ? TopShape.getSubTopoShapes(TopAbs_EDGE)
                                        : getContinuousEdges(TopShape);

    if (edges.empty()) {
        return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "No edges specified"));
    }
    const int chamferType = ChamferType.getValue();
    const double size = Size.getValue();
    double size2 = Size2.getValue();
    const double angle = Angle.getValue();
    const bool flipDirection = FlipDirection.getValue();

    auto res = validateParameters(chamferType, size, size2, angle);
    if (res != App::DocumentObject::StdReturn) {
        return res;
    }

    this->positionByBaseFeature();

    if (static_cast<Part::ChamferType>(chamferType) == Part::ChamferType::distanceAngle) {
        size2 = angle;
    }
    try {
        //! Builds the chamfer with the distances on the face `flip` selects.
        //! `swapSizes` exchanges the two distances: OCC measures the first one on
        //! the reference face, so handing the distances to the other face means
        //! both flipping the face and swapping them - the same call as
        //! TopoShape::makeChamfer(2.99, 17.0, {edge}) for the user's edge.
        auto buildChamfer = [&](bool flip, bool swapSizes) {
            TopoShape result(0);
            Part::SignalException sig;
            const double radius1 = swapSizes ? size2 : size;
            const double radius2 = swapSizes ? size : size2;
            result.makeElementChamfer(
                TopShape,
                edges,
                static_cast<Part::ChamferType>(chamferType),
                radius1,
                radius2,
                nullptr,
                flip ? Part::Flip::flip : Part::Flip::none
            );
            return result;
        };
        //! A result the feature may keep.  Note this mirrors the refusal below:
        //! a shape BRepCheck accepts is fine even when it has small (but
        //! positive) faces - the octahedron chamfer in TestChamfer relies on
        //! that - and a shape with a non-positive area is only rejected when
        //! BRepCheck rejects it too.
        auto usable = [&](const TopoShape& candidate) {
            if (candidate.isNull()) {
                return false;
            }
            if (invalidChamferReason(candidate).empty()) {
                return true;
            }
            TopTools_ListOfShape larg;
            larg.Append(TopShape.getShape());
            return BRepAlgo::IsValid(larg, candidate.getShape(), Standard_False, Standard_False);
        };

        // 1. Exactly what was asked for: the distances on the face the edge was
        //    selected from.
        TopoShape shape;
        std::string failure;
        try {
            shape = buildChamfer(flipDirection, false);
        }
        catch (Standard_Failure& e) {
            failure = e.GetMessageString();
        }

        TopTools_ListOfShape aLarg;
        aLarg.Append(TopShape.getShape());
        if (!shape.isNull()
            && !BRepAlgo::IsValid(aLarg, shape.getShape(), Standard_False, Standard_False)) {
            ShapeFix_ShapeTolerance aSFT;
            aSFT.LimitTolerance(
                shape.getShape(),
                Precision::Confusion(),
                Precision::Confusion(),
                TopAbs_SHAPE
            );
        }

        const bool twoDistances
            = static_cast<Part::ChamferType>(chamferType) == Part::ChamferType::twoDistances;
        double wedgeSize1 = size;
        if (twoDistances) {
            wedgeSize1 = size2;
        }
        else if (static_cast<Part::ChamferType>(chamferType) == Part::ChamferType::distanceAngle) {
            wedgeSize1 = size * std::tan(Base::toRadians(size2));
        }
        // A chamfer takes material off a convex edge and fills the valley of a
        // concave one (the user's 50 mm edge is concave: 17/2.99 adds 1270.75).
        // A candidate that does the opposite comes from the wrong face
        // assignment and must not be accepted - it would turn "fill the valley"
        // into a groove, or the other way round.
        const double convexity = chamferConvexity(TopShape, edges);
        const double expectedWedge = chamferWedgeVolume(TopShape, edges, size, wedgeSize1);
        const double baseVolume = shapeVolume(TopShape.getShape());
        auto removedBy = [&](const TopoShape& candidate) {
            return baseVolume - shapeVolume(candidate.getShape());
        };
        auto acceptable = [&](const TopoShape& candidate) {
            if (!usable(candidate)) {
                return false;
            }
            const double removed = removedBy(candidate);
            if (convexity < 0.0) {
                return removed < -Precision::Confusion();  // concave: fills the valley
            }
            // convex, or convexity unknown: never grow the solid
            return removed > Precision::Confusion();
        };

        // 2. The same chamfer with the distances on the other adjacent face.
        //    OCCT measures the first distance on the reference face and pins it
        //    there, so a distance that does not fit that face makes OCCT fold the
        //    face past its boundary (a negative-area face) or fail outright - and
        //    the feature's face choice is just "the first ancestor".  The other
        //    face, and for two distances the other distance order (which is what
        //    TopoShape::makeChamfer(2.99, 17.0, {edge}) expresses), are tried in
        //    turn; the first usable result wins.
        bool otherFaceRetry = false;
        if (!acceptable(shape)) {
            std::vector<std::pair<bool, bool>> attempts;
            attempts.emplace_back(flipDirection, false);
            if (twoDistances) {
                attempts.emplace_back(flipDirection, true);
            }
            attempts.emplace_back(!flipDirection, false);
            if (twoDistances) {
                attempts.emplace_back(!flipDirection, true);
            }
            // the first entry is what was asked for and has already been tried;
            // the sign is the direction a chamfer has to move the volume in
            // among the rest, the one whose removed volume is closest to the
            // analytic wedge is the chamfer the user asked for
            const double expected = (convexity < 0.0 ? -1.0 : 1.0) * expectedWedge;
            double bestError = std::numeric_limits<double>::max();
            TopoShape best;
            for (std::size_t i = 1; i < attempts.size(); ++i) {
                try {
                    TopoShape other = buildChamfer(attempts[i].first, attempts[i].second);
                    if (!acceptable(other)) {
                        continue;
                    }
                    const double error = std::abs(removedBy(other) - expected);
                    if (error < bestError) {
                        bestError = error;
                        best = other;
                    }
                }
                catch (Standard_Failure& e) {
                    if (failure.empty()) {
                        failure = e.GetMessageString();
                    }
                }
                catch (Base::Exception& e) {
                    return new App::DocumentObjectExecReturn(e.what());
                }
            }
            if (!best.isNull()) {
                shape = best;
                otherFaceRetry = true;
                failure.clear();
            }
        }

        // 3. Last resort: the equivalent wedge cut.  Only OCCT failures end up
        //    here (the guard above returns early), and it is exact when a
        //    distance fits neither adjacent face - see chamferEdgesByWedge().
        bool wedgeRetry = false;
        if (!acceptable(shape)) {
            TopoShape wedged = chamferEdgesByWedge(
                TopShape,
                edges,
                static_cast<Part::ChamferType>(chamferType),
                size,
                size2,
                flipDirection,
                convexity
            );
            if (acceptable(wedged)) {
                shape = wedged;
                wedgeRetry = true;
            }
        }

        if (shape.isNull()) {
            if (failure.empty()) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Failed to create chamfer")
                );
            }
            return new App::DocumentObjectExecReturn(failure);
        }

        // Neither face assignment nor a wedge cut produced a usable solid: refuse
        // it and name the sizes that caused it.
        if (!usable(shape)) {
            const std::string reason = invalidChamferReason(shape);
            std::ostringstream message;
            message << "Chamfer produced an invalid shape (" << reason << "): the ";
            if (static_cast<Part::ChamferType>(chamferType) == Part::ChamferType::distanceAngle) {
                message << "size " << Size.getValue() << " mm / angle " << Angle.getValue()
                        << " deg are too large for this edge - reduce them or check the edge.";
            }
            else {
                message << "sizes " << Size.getValue() << " mm / " << Size2.getValue()
                        << " mm are too large for this edge - reduce them or check the edge.";
            }
            return new App::DocumentObjectExecReturn(message.str());
        }

        if (otherFaceRetry) {
            Base::Console().warning(
                "{}: the requested distance does not fit the selected face, so the distances were "
                "applied to the other adjacent face; use Flip direction to choose explicitly.\n",
                getFullName()
            );
        }
        if (wedgeRetry) {
            Base::Console().warning(
                "{}: OCCT cannot represent the requested sizes on this edge (a distance runs past "
                "the adjacent face); built the equivalent wedge cut instead - the chamfer has sharp "
                "ends and no corner blend.\n",
                getFullName()
            );
        }

        // store shape before refinement
        this->rawShape = shape;
        shape = refineShapeIfActive(shape);
        if (!isSingleSolidRuleSatisfied(shape.getShape())) {
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                "Exception",
                "Result has multiple solids: enable 'Allow Compound' in the active body."
            ));
        }

        shape = getSolid(shape);
        this->Shape.setValue(shape);
        return App::DocumentObject::StdReturn;
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    catch (...) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "Chamfer failed: OCC kernel error in chamfer computation")
        );
    }
}

void Chamfer::Restore(Base::XMLReader& reader)
{
    DressUp::Restore(reader);

    migrateFlippedProperties(reader);
}

void Chamfer::handleChangedPropertyType(Base::XMLReader& reader, const char* TypeName, App::Property* prop)
{
    if (prop && strcmp(TypeName, "App::PropertyFloatConstraint") == 0
        && prop->getTypeId().getName() == "App::PropertyQuantityConstraint") {
        App::PropertyFloatConstraint p;
        p.Restore(reader);
        static_cast<App::PropertyQuantityConstraint*>(prop)->setValue(p.getValue());
    }
    else {
        DressUp::handleChangedPropertyType(reader, TypeName, prop);
    }
}

bool Chamfer::requiresSizeSwapping(const Base::XMLReader& reader) const
{
    return Base::getVersion(reader.ProgramVersion) < Base::Version::v1_0
        && (ChamferType.getValue() == 1 || ChamferType.getValue() == 2);
}

void Chamfer::migrateFlippedProperties(const Base::XMLReader& reader)
{
    if (!requiresSizeSwapping(reader)) {
        return;
    }

    Base::Console().warning(
        "The 'FlipDirection' property of the chamfer of {} is being adjusted to maintain"
        "the same geometry in this FreeCAD version. If the re-saved file is later opened "
        "in FreeCAD 0.21.x the chamfer result may differ due to the changed parameter "
        "interpretation.\n",
        getFullName()
    );

    FlipDirection.setValue(!FlipDirection.getValue());
}

void Chamfer::onChanged(const App::Property* prop)
{
    if (prop == &ChamferType) {
        updateProperties();
    }

    DressUp::onChanged(prop);
}

void Chamfer::updateProperties()
{
    auto chamferType = ChamferType.getValue();

    auto disableproperty = [](App::Property* prop, bool on) {
        prop->setStatus(App::Property::ReadOnly, on);
    };

    switch (chamferType) {
        case 0:  // "Equal distance"
            disableproperty(&this->Angle, true);
            disableproperty(&this->Size2, true);
            break;
        case 1:  // "Two distances"
            disableproperty(&this->Angle, true);
            disableproperty(&this->Size2, false);
            break;
        case 2:  // "Distance and Angle"
            disableproperty(&this->Angle, false);
            disableproperty(&this->Size2, true);
            break;
    }
}

static App::DocumentObjectExecReturn* validateParameters(
    int chamferType,
    double size,
    double size2,
    double angle
)
{
    // Size is common to all chamfer types.
    if (size <= 0) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "Size must be greater than zero")
        );
    }

    switch (chamferType) {
        case 0:  // Equal distance
            // Nothing to do.
            break;
        case 1:  // Two distances
            if (size2 <= 0) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Size2 must be greater than zero")
                );
            }
            break;
        case 2:  // Distance and angle
            if (angle <= 0 || angle >= 180.0) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Angle must be greater than 0 and less than 180")
                );
            }
            break;
    }

    return App::DocumentObject::StdReturn;
}
