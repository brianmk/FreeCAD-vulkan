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
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Compound.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>

#include <sstream>


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

//! How far the face reaches in plane away from the edge, measured along \a leg
//! (the in-plane direction from the edge into that face).  A chamfer distance
//! measured on this face must not exceed it: past the reach the tool runs off
//! the face and cuts through whatever lies beyond, which is not a bevel any
//! more.  NB: a boolean can store a geometrically straight edge or a planar face
//! as a B-spline (the user's Pocket002.Edge80 is a GeomBSplineCurve with a 3.6e-15
//! chord deviation), which is why the checks here are geometric and never look
//! at GeomAbs_Line/GeomAbs_Plane.
bool faceReach(const TopoDS_Face& face, const gp_Pnt& start, const gp_Dir& leg, double& reach)
{
    double best = -std::numeric_limits<double>::max();
    bool any = false;
    for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
        const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
        best = std::max(best, std::abs(gp_Vec(start, point).Dot(gp_Vec(leg))));
        any = true;
    }
    for (TopExp_Explorer it(face, TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        const gp_Pnt point = curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
        best = std::max(best, std::abs(gp_Vec(start, point).Dot(gp_Vec(leg))));
        any = true;
    }
    if (!any) {
        return false;
    }
    reach = best;
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

//! In-plane direction from the edge into the interior of a face, taken from the
//! boundary vertex with the largest perpendicular offset from the edge.  A face
//! running parallel to the edge has its centre of mass in the plane through the
//! edge, so the projection collapses and the direction - with the reach and the
//! wedge triangle - becomes unreliable on exactly the valley edges this is for.
bool interiorDirection(const TopoDS_Face& face, const gp_Pnt& origin, const gp_Dir& axis, gp_Dir& out)
{
    double bestOffset = -1.0;
    gp_Vec bestDirection;
    for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
        const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
        gp_Vec dir(origin, point);
        dir -= gp_Vec(axis) * dir.Dot(gp_Vec(axis));
        const double offset = dir.Magnitude();
        if (offset > bestOffset) {
            bestOffset = offset;
            bestDirection = dir;
        }
    }
    if (bestOffset <= Precision::Confusion()) {
        return false;
    }
    out = gp_Dir(bestDirection);
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

//! True when the region the chamfer changed stays inside \a allowed.  A wedge
//! built with the wrong legs or on the wrong side of the edge changes geometry
//! far away from it, which is exactly the artefact the user reported.
bool changedRegionIsLocal(const Part::TopoShape& base,
                          const Part::TopoShape& candidate,
                          double removed,
                          const Base::BoundBox3d& allowed)
{
    const Part::TopoShape changed = removed > 0.0 ? base.makeElementCut(candidate)
                                                  : candidate.makeElementCut(base);
    if (changed.isNull()) {
        return true;  // cannot tell: do not block an otherwise fine chamfer
    }
    const Base::BoundBox3d box = changed.getBoundBox();
    if (!box.IsValid()) {
        return true;
    }
    return box.MinX >= allowed.MinX && box.MaxX <= allowed.MaxX && box.MinY >= allowed.MinY
        && box.MaxY <= allowed.MaxY && box.MinZ >= allowed.MinZ && box.MaxZ <= allowed.MaxZ;
}

//! The two face planes that meet at an edge.
struct EdgePlanes
{
    gp_Pnt origin;
    gp_Dir normal0;
    gp_Dir normal1;
    bool known = false;
};

//! Outward normal of a face at a point on the edge: the surface normal there
//! (exact for any surface type, including a planar face a boolean stored as a
//! B-spline) oriented by the face's orientation in the shell.
bool outwardFaceNormal(const TopoDS_Face& face, const gp_Pnt& origin, gp_Dir& normal)
{
    BRepAdaptor_Surface surface(face, Standard_False);
    GeomAPI_ProjectPointOnSurf projector(origin, surface.Surface().Surface());
    if (!projector.IsDone() || projector.NbPoints() < 1) {
        return false;
    }
    double u = 0.0;
    double v = 0.0;
    projector.LowerDistanceParameters(u, v);
    gp_Pnt point;
    gp_Vec du;
    gp_Vec dv;
    surface.D1(u, v, point, du, dv);
    const gp_Vec cross = du.Crossed(dv);
    if (cross.Magnitude() < Precision::Confusion()) {
        return false;
    }
    normal = gp_Dir(cross);
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    return true;
}

EdgePlanes edgePlanes(const Part::TopoShape& base, const TopoShape& edgeShape)
{
    EdgePlanes planes;
    if (edgeShape.isNull() || edgeShape.getShape().ShapeType() != TopAbs_EDGE) {
        return planes;
    }
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(base.getShape(), TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    const TopoDS_Edge& edge = TopoDS::Edge(edgeShape.getShape());
    if (!edgeFaces.Contains(edge) || edgeFaces.FindFromKey(edge).Extent() != 2) {
        return planes;
    }
    gp_Pnt start;
    gp_Pnt end;
    if (!straightEdgeEnds(edge, start, end)) {
        return planes;
    }
    planes.origin = start;
    planes.known
        = outwardFaceNormal(TopoDS::Face(edgeFaces.FindFromKey(edge).First()), start, planes.normal0)
        && outwardFaceNormal(
            TopoDS::Face(edgeFaces.FindFromKey(edge).Last()),
            start,
            planes.normal1
        );
    return planes;
}

//! The region the chamfer changed: what it took off, or what it added.  A plain
//! cut is used rather than makeElementCut, which returned the whole solid here.
Part::TopoShape changedRegion(const Part::TopoShape& base,
                              const Part::TopoShape& candidate,
                              double removed)
{
    const TopoDS_Shape part = removed > 0.0 ? base.cut(candidate.getShape())
                                            : candidate.cut(base.getShape());
    Part::TopoShape result(0);
    result.setShape(part);
    return result;
}

//! Every vertex of the region the chamfer changed must lie in front of both face
//! planes when material was added (a wedge filling the valley between them) and
//! behind both when material was taken off (a wedge cut out of the solid).  A
//! mirrored wedge crosses a plane and is refused here: that is the upside-down
//! wedge the user saw on inner edges.
bool changedRegionFollowsTheFaces(const Part::TopoShape& changed,
                                  const EdgePlanes& planes,
                                  bool added)
{
    if (!planes.known || changed.isNull()) {
        return true;  // cannot tell: do not block an otherwise fine chamfer
    }
    const double tolerance = 1e-6;
    for (TopExp_Explorer it(changed.getShape(), TopAbs_VERTEX); it.More(); it.Next()) {
        const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
        for (const gp_Dir& normal : {planes.normal0, planes.normal1}) {
            const double offset = gp_Vec(planes.origin, point).Dot(gp_Vec(normal));
            if (added ? offset < -tolerance : offset > tolerance) {
                return false;
            }
        }
    }
    return true;
}

//! How far a wedge's leg endpoint may sit from the face it is supposed to land
//! on.  A boolean lands its vertices within ~1e-7 of each other, so 1e-6 accepts
//! a real endpoint and rejects one that has drifted off the face.
constexpr double wedgeOnFaceTolerance = 1e-6;

//! True when \a point lies on \a face within \a tolerance: on its surface *and*
//! inside its outer wire.  The distance to the bounded face is used rather than a
//! surface projection plus a wire test, because a boolean can store a planar face
//! as a B-spline one, and it is 0 exactly when the point is on the face.
bool pointOnFace(const TopoDS_Face& face, const gp_Pnt& point, double tolerance)
{
    BRepBuilderAPI_MakeVertex makeVertex(point);
    BRepExtrema_DistShapeShape distance(makeVertex.Vertex(), face);
    return distance.IsDone() && distance.Value() <= tolerance;
}

//! The wedge has to lie ON the faces it fills: the region it changed has to
//! reach each of them along its surface, not only at the shared edge.  A vertex
//! on one face that is not on the other is such a reach, and the leg ends are
//! exactly that.  A triangle built from a direction that does not follow the
//! face satisfies the plane-offset rule and still stands in the air ("the wedge
//! is going up in the air, not along the flat surface"): that rule only asks a
//! point to be outside the material.
bool changedRegionLiesOnTheFaces(const Part::TopoShape& changed,
                                 const TopoDS_Face& face0,
                                 const TopoDS_Face& face1,
                                 double tolerance)
{
    if (changed.isNull()) {
        return true;  // cannot tell: do not block an otherwise fine wedge
    }
    bool reached0 = false;
    bool reached1 = false;
    for (TopExp_Explorer it(changed.getShape(), TopAbs_VERTEX); it.More(); it.Next()) {
        const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
        const bool on0 = pointOnFace(face0, point, tolerance);
        const bool on1 = pointOnFace(face1, point, tolerance);
        // A vertex on both faces is on the shared edge, not the wedge reaching
        // along a face; the leg ends are on one face only.
        if (on0 && !on1) {
            reached0 = true;
        }
        if (on1 && !on0) {
            reached1 = true;
        }
        if (reached0 && reached1) {
            return true;
        }
    }
    return false;
}

//! The wedge volume a chamfer of these sizes removes from each straight edge:
//! 0.5 * size0 * size1 * length, summed over the edges, and zero as soon as one
//! of them is not a straight edge.  It only sizes the no-op floor the result has
//! to clear; it is not a magnitude the result has to match (a concave chamfer
//! fills the valley, so it adds less than this).
double chamferWedgeVolume(const std::vector<Part::TopoShape>& edges,
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

//! Start, direction, in-plane legs and sizes of the wedge for one edge.
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

//! The wedge prism over one edge: a triangle at \a base with both legs on the
//! same side (\a legSign), extruded exactly along the edge.  It spans the edge
//! *exactly* - overshooting its ends would cut into whatever lies beyond them.
TopoDS_Shape wedgeTool(const WedgeLegs& legs, int legSign, const gp_Pnt& base, const gp_Dir& direction)
{
    BRepBuilderAPI_MakePolygon polygon;
    polygon.Add(base);
    polygon.Add(base.Translated(gp_Vec(legs.leg0) * (legs.size0 * legSign)));
    polygon.Add(base.Translated(gp_Vec(legs.leg1) * (legs.size1 * legSign)));
    polygon.Close();
    BRepBuilderAPI_MakeFace face(polygon.Wire());
    if (!face.IsDone()) {
        return {};
    }
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(direction) * legs.length);
}

//! Chamfers each edge by subtracting a wedge prism, and returns the cut shape
//! (or a null shape when that is not possible).  \a offTheFaces is set when a
//! candidate was refused because it would have stood off the adjacent faces,
//! so the caller can say why instead of only that it failed.
Part::TopoShape chamferEdgesByWedge(const Part::TopoShape& base,
                                    const std::vector<Part::TopoShape>& edges,
                                    Part::ChamferType type,
                                    double size,
                                    double size2,
                                    bool flip,
                                    double convexity,
                                    double noopFloor,
                                    const EdgePlanes& planes,
                                    bool* offTheFaces)
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
        // Pick the triangle by the side of the edge (see the centroid test below):
        // the distances keep the order the faces are given in (ManualChamfer
        // reference); the reflected triangle is never a candidate.
        //
        // BRepAlgoAPI is called directly: TopoShape::fuse/makeElementFuse run the
        // result through makeShell/element handling and returned only the tool
        // (combined volume = the prism, 885.16 mm3), so the union never happened.
        const bool concave = convexity < 0.0;
        const TopoDS_Face face0 = TopoDS::Face(edgeFaces.FindFromKey(edge).First());
        const TopoDS_Face face1 = TopoDS::Face(edgeFaces.FindFromKey(edge).Last());
        TopoDS_Shape best;
        Part::TopoShape bestResult;
        for (const auto& [size0, size1] : assignments) {
            WedgeLegs legs;
            if (!wedgeLegs(edge, face0, face1, size0, size1, legs)) {
                return Part::TopoShape(0);
            }
            for (int atEnd : {0, 1}) {
                const gp_Pnt baseVertex = atEnd == 0
                    ? legs.start
                    : legs.start.Translated(gp_Vec(legs.direction) * legs.length);
                const gp_Dir direction = atEnd == 0 ? legs.direction
                                                    : gp_Dir(-gp_Vec(legs.direction));
                // The side of the edge the wedge lies on is not a fixed sign: the
                // in-plane leg directions point at the face vertex farthest from the
                // edge, and a face that extends on both sides of the edge can put
                // that vertex on either side.  Measured on Pocket.Edge103 (27 mm
                // convex edge with a 1 mm ledge beside a 99 mm2 face): the reflected
                // triangle removed 0.2045 mm3 where the analytic wedge is 13.5 - and
                // a candidate that cuts air is not a chamfer.  Classify the triangle's
                // centroid instead: a convex chamfer takes material off, so its
                // triangle is inside the solid, a concave one fills the valley, so it
                // is outside.  Neither side right -> no candidate for this end.
                int legSign = 0;
                for (const int sign : {1, -1}) {
                    const gp_Pnt a = baseVertex;
                    const gp_Pnt b
                        = baseVertex.Translated(gp_Vec(legs.leg0) * (legs.size0 * sign));
                    const gp_Pnt c
                        = baseVertex.Translated(gp_Vec(legs.leg1) * (legs.size1 * sign));
                    const gp_Pnt centroid((a.XYZ() + b.XYZ() + c.XYZ()) / 3.0);
                    BRepClass3d_SolidClassifier classifier(base.getShape());
                    classifier.Perform(centroid, Precision::Confusion());
                    const bool inside = classifier.State() == TopAbs_IN;
                    if (inside == !concave) {
                        legSign = sign;
                        break;
                    }
                }
                if (legSign == 0) {
                    continue;
                }
                // The triangle has to sit ON the two faces: its far vertices are
                // where the legs end on them, so each has to be on its own face's
                // surface and inside that face's wire.  The *whole* leg is checked,
                // not only its end: the prism's side face is a straight quad, so a
                // leg that leaves a curved (or short) face between its ends leaves
                // that quad standing in the air - the artifact users see as "a full
                // rectangle clipping in the air".  A face that only reaches part of
                // the leg, or bends away from it, cannot carry the wedge.
                bool legsOnTheFaces = true;
                for (const double along : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                    const gp_Pnt onFace0 = baseVertex.Translated(
                        gp_Vec(legs.leg0) * (legs.size0 * legSign * along)
                    );
                    const gp_Pnt onFace1 = baseVertex.Translated(
                        gp_Vec(legs.leg1) * (legs.size1 * legSign * along)
                    );
                    if (!pointOnFace(face0, onFace0, wedgeOnFaceTolerance)
                        || !pointOnFace(face1, onFace1, wedgeOnFaceTolerance)) {
                        legsOnTheFaces = false;
                        break;
                    }
                }
                if (!legsOnTheFaces) {
                    if (offTheFaces) {
                        *offTheFaces = true;
                    }
                    continue;
                }
                const TopoDS_Shape tool = wedgeTool(legs, legSign, baseVertex, direction);
                if (tool.IsNull()) {
                    continue;
                }
                TopoDS_Shape combined;
                if (concave) {
                    BRepAlgoAPI_Fuse fuseOp(base.getShape(), tool);
                    if (!fuseOp.IsDone()) {
                        continue;
                    }
                    combined = fuseOp.Shape();
                }
                else {
                    BRepAlgoAPI_Cut cutOp(base.getShape(), tool);
                    if (!cutOp.IsDone()) {
                        continue;
                    }
                    combined = cutOp.Shape();
                }
                Part::TopoShape candidate(0);
                candidate.setShape(combined);
                if (candidate.isNull()) {
                    continue;
                }
                const double change = baseVolume - shapeVolume(candidate.getShape());
                if (std::abs(change) < noopFloor) {
                    continue;  // no-op
                }
                if (concave ? change >= 0.0 : change <= 0.0) {
                    continue;  // wrong direction for this edge
                }
                const Part::TopoShape changedPart = changedRegion(base, candidate, change);
                if (!changedRegionFollowsTheFaces(changedPart, planes, change < 0.0)) {
                    continue;  // mirrored: the region crosses a face plane
                }
                if (!changedRegionLiesOnTheFaces(changedPart, face0, face1, wedgeOnFaceTolerance)) {
                    // the legs ended on the faces, but the changed region does not
                    // reach both of them along their surfaces (it only touched one
                    // of them at the shared edge)
                    if (offTheFaces) {
                        *offTheFaces = true;
                    }
                    continue;
                }
                best = tool;
                bestResult = candidate;
                break;
            }
            if (!best.IsNull()) {
                break;
            }
        }
        if (best.IsNull()) {
            return Part::TopoShape(0);  // no side-correct wedge on this edge
        }
        if (edges.size() == 1) {
            // One edge: the per-edge boolean is already the answer.  Re-fusing it
            // as a compound tool invalidated the result that passed the side check.
            return bestResult;
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
        // Clear the shape first: a chamfer that cannot be built must not keep
        // displaying the solid it produced last time (the user's stale, wrong-side
        // wedge stayed on screen after the recompute started failing).
        this->Shape.setValue(Part::TopoShape());
        this->rawShape = Part::TopoShape();

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
        const double expectedWedge = chamferWedgeVolume(edges, size, wedgeSize1);
        // A result that barely moves the volume is not a chamfer, it is a no-op
        // that still gets reported as a success (the user's Edge3/Edge16 changed
        // the solid by 0.0000 and 0.0005 mm3).  A concave chamfer adds whatever
        // the tool covers rather than the analytic wedge, so for it only this
        // floor applies, not a magnitude match.
        const double noopFloor = std::max(1e-6, 0.01 * expectedWedge);
        // The chamfer may only change geometry next to the edge it belongs to;
        // anything further away is the artefact the user sees.
        Base::BoundBox3d allowedRegion;
        for (const auto& edgeShape : edges) {
            const Base::BoundBox3d box = edgeShape.getBoundBox();
            if (box.IsValid()) {
                allowedRegion.Add(box);
            }
        }
        if (allowedRegion.IsValid()) {
            allowedRegion.Enlarge(std::max(size, wedgeSize1));
        }
        // Which side of the edge the chamfer may change: its two face planes.
        // Multi-edge chamfers are not checked this way (a vertex near one edge
        // need not be in front of another edge's faces).
        const bool planesKnown = edges.size() == 1;
        const EdgePlanes planes = planesKnown ? edgePlanes(TopShape, edges.front()) : EdgePlanes();
        // An edge far shorter than the chamfer itself (the user's Footprint has
        // 1.6e-5 mm slivers left by booleans) cannot carry it: any boolean on it
        // is noise, reported as a +0.0005 mm3 "success".
        const double shortest = std::min(size, wedgeSize1);
        for (const auto& edgeShape : edges) {
            if (edgeShape.isNull() || edgeShape.getShape().ShapeType() != TopAbs_EDGE) {
                continue;
            }
            gp_Pnt edgeStart;
            gp_Pnt edgeEnd;
            if (!straightEdgeEnds(TopoDS::Edge(edgeShape.getShape()), edgeStart, edgeEnd)) {
                continue;
            }
            const double edgeLength = edgeStart.Distance(edgeEnd);
            if (edgeLength < 1e-3 * shortest) {
                std::ostringstream message;
                message << "this edge is only " << edgeLength
                        << " mm long, far shorter than the " << Size.getValue() << " mm / "
                        << Size2.getValue() << " mm chamfer - check the edge.";
                return new App::DocumentObjectExecReturn(message.str());
            }
        }
        const double baseVolume = shapeVolume(TopShape.getShape());
        bool sawNoop = false;
        auto removedBy = [&](const TopoShape& candidate) {
            return baseVolume - shapeVolume(candidate.getShape());
        };
        // A distance is measured on one of the two faces next to the edge, so it
        // must fit in that face's in-plane reach; otherwise the result is a
        // gouge across the neighbouring faces, not a chamfer.  Measure every
        // selected edge's two faces once (in the order OCCT picks them: the
        // first ancestor, or the last one with Flip direction) and check each
        // assignment against them.
        std::vector<std::pair<double, double>> reachPairs;
        std::vector<bool> reachKnown;
        {
            TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
            TopExp::MapShapesAndAncestors(
                TopShape.getShape(),
                TopAbs_EDGE,
                TopAbs_FACE,
                edgeFaces
            );
            for (const auto& edgeShape : edges) {
                double firstReach = 0.0;
                double lastReach = 0.0;
                bool known = false;
                if (!edgeShape.isNull() && edgeShape.getShape().ShapeType() == TopAbs_EDGE) {
                    const TopoDS_Edge& edge = TopoDS::Edge(edgeShape.getShape());
                    gp_Pnt start;
                    gp_Pnt end;
                    if (edgeFaces.Contains(edge) && edgeFaces.FindFromKey(edge).Extent() == 2
                        && straightEdgeEnds(edge, start, end)) {
                        const gp_Dir axis(gp_Vec(start, end));
                        gp_Dir legFirst;
                        gp_Dir legLast;
                        known
                            = interiorDirection(
                                  TopoDS::Face(edgeFaces.FindFromKey(edge).First()),
                                  start,
                                  axis,
                                  legFirst
                              )
                            && interiorDirection(
                                TopoDS::Face(edgeFaces.FindFromKey(edge).Last()),
                                start,
                                axis,
                                legLast
                            )
                            && faceReach(
                                TopoDS::Face(edgeFaces.FindFromKey(edge).First()),
                                start,
                                legFirst,
                                firstReach
                            )
                            && faceReach(
                                TopoDS::Face(edgeFaces.FindFromKey(edge).Last()),
                                start,
                                legLast,
                                lastReach
                            );
                    }
                }
                reachPairs.emplace_back(firstReach, lastReach);
                reachKnown.push_back(known);
            }
        }
        constexpr double reachMargin = 1e-3;
        //! Does (sizeA measured on the reference face, sizeB on the other) fit
        //! every selected edge?
        auto assignmentFits = [&](bool flip, bool swap, double sizeA, double sizeB) {
            const double onReference = swap ? sizeB : sizeA;
            const double onOther = swap ? sizeA : sizeB;
            for (std::size_t i = 0; i < reachPairs.size(); ++i) {
                if (!reachKnown[i]) {
                    continue;  // cannot tell: do not block the user
                }
                const double referenceReach = flip ? reachPairs[i].second : reachPairs[i].first;
                const double otherReach = flip ? reachPairs[i].first : reachPairs[i].second;
                if (onReference > referenceReach + reachMargin
                    || onOther > otherReach + reachMargin) {
                    return false;
                }
            }
            return true;
        };
        //! The assignment that misses fitting by the least, for the message.
        std::string reachRefusal;
        {
            double bestExcess = std::numeric_limits<double>::max();
            for (bool flip : {flipDirection, !flipDirection}) {
                for (bool swap : {false, true}) {
                    if (swap && !twoDistances) {
                        continue;
                    }
                    const double onReference = swap ? wedgeSize1 : size;
                    const double onOther = swap ? size : wedgeSize1;
                    bool blocked = false;
                    double worst = 0.0;
                    double worstReach = 0.0;
                    double worstDistance = 0.0;
                    for (std::size_t i = 0; i < reachPairs.size(); ++i) {
                        if (!reachKnown[i]) {
                            continue;
                        }
                        const double referenceReach
                            = flip ? reachPairs[i].second : reachPairs[i].first;
                        const double otherReach
                            = flip ? reachPairs[i].first : reachPairs[i].second;
                        for (const auto& [distance, reach] :
                             {std::pair<double, double> {onReference, referenceReach},
                              std::pair<double, double> {onOther, otherReach}}) {
                            const double excess = distance - reach;
                            if (excess > reachMargin) {
                                blocked = true;
                                if (excess > worst) {
                                    worst = excess;
                                    worstReach = reach;
                                    worstDistance = distance;
                                }
                            }
                        }
                    }
                    if (blocked && worst < bestExcess) {
                        bestExcess = worst;
                        std::ostringstream message;
                        message << "the face next to this edge ends " << worstReach
                                << " mm away, so a " << worstDistance
                                << " mm chamfer would cut through it - reduce the distance or "
                                   "extend the geometry.";
                        reachRefusal = message.str();
                    }
                }
            }
        }

        auto acceptable = [&](const TopoShape& candidate) {
            if (!usable(candidate) || !isSingleSolidRuleSatisfied(candidate.getShape())) {
                return false;
            }
            const double removed = removedBy(candidate);
            if (std::abs(removed) < noopFloor) {
                sawNoop = true;  // changes nothing: not a chamfer
                return false;
            }
            if (convexity < 0.0) {
                if (removed >= -Precision::Confusion()) {
                    return false;  // concave: fills the valley
                }
            }
            else if (removed <= Precision::Confusion()) {
                return false;  // convex, or convexity unknown: never grow the solid
            }
            // The side of an OCCT result is established by the sign check above
            // (a mirrored chamfer moves the volume the other way).  The plane-offset
            // rule is applied to the wedge path, where the side is our own choice -
            // applying it to OCCT results rejected valid chamfers whose changed
            // region was a hair behind a plane after the boolean.
            (void)planesKnown;
            return allowedRegion.IsValid() ? changedRegionIsLocal(TopShape, candidate, removed, allowedRegion)
                                           : true;
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
                    if (!assignmentFits(attempts[i].first, attempts[i].second, size, wedgeSize1)) {
                        continue;  // the distances do not fit the faces: no bevel
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
        // The wedge is built between the two faces in the order OCCT picks them,
        // so it is only legitimate when one of its two assignments fits both
        // faces; otherwise every option would run off a face - refuse instead of
        // gouging the neighbouring geometry.
        const bool wedgeFits = assignmentFits(false, false, size, wedgeSize1)
            || (twoDistances && assignmentFits(false, true, size, wedgeSize1));
        //! Set when a wedge candidate was refused for standing off the adjacent
        //! faces, so the refusal can say that instead of only "could not build".
        bool wedgeOffFaces = false;
        if (!acceptable(shape) && !wedgeFits) {
            if (!reachRefusal.empty()) {
                return new App::DocumentObjectExecReturn(reachRefusal);
            }
        }
        if (!acceptable(shape) && wedgeFits) {
            TopoShape wedged = chamferEdgesByWedge(
                TopShape,
                edges,
                static_cast<Part::ChamferType>(chamferType),
                size,
                size2,
                flipDirection,
                convexity,
                noopFloor,
                planes,
                &wedgeOffFaces
            );
            if (acceptable(wedged)) {
                shape = wedged;
                wedgeRetry = true;
            }
        }

        if (shape.isNull()) {
            // Never hand back a bare kernel message (the user's "no shape and no
            // refusal message"), and never leave the previous shape on screen: an
            // invalid chamfer that keeps displaying the last solid is exactly how
            // a wrong-side wedge stays visible after it stopped being recomputed.
            this->Shape.setValue(Part::TopoShape());
            this->rawShape = Part::TopoShape();
            std::ostringstream message;
            message << "Chamfer could not be built on this edge";
            if (!failure.empty()) {
                message << " (" << failure << ")";
            }
            if (wedgeOffFaces) {
                message << " - OCCT cannot represent the sizes here and the equivalent wedge would "
                           "stand off the adjacent faces instead of lying on them (a face is too "
                           "narrow along this edge for the requested distances); check the edge, "
                           "or reduce the distances.";
            }
            else {
                message << " - check the edge, or reduce the distances.";
            }
            return new App::DocumentObjectExecReturn(message.str());
        }

        // A result that leaves the solid alone must not be reported as a chamfer.
        if (sawNoop && !acceptable(shape)) {
            std::ostringstream message;
            message << "Chamfer does not change the solid on this edge (no-op): the ";
            if (static_cast<Part::ChamferType>(chamferType) == Part::ChamferType::distanceAngle) {
                message << "size " << Size.getValue() << " mm / angle " << Angle.getValue()
                        << " deg leave the geometry as it is - check the edge, or use sizes that "
                           "reach past the faces.";
            }
            else {
                message << "sizes " << Size.getValue() << " mm / " << Size2.getValue()
                        << " mm leave the geometry as it is - check the edge, or use sizes that "
                           "reach past the faces.";
            }
            return new App::DocumentObjectExecReturn(message.str());
        }

        // Neither face assignment nor a wedge cut produced a usable solid: refuse
        // it.  Blaming the sizes is only right when the reach rule actually
        // rejected them.  A base feature that still carries the sub-micron edges a
        // boolean left behind makes every chamfer on the affected corner fold a
        // face, and there the sizes can fit the faces perfectly well: reported on
        // a valley edge whose faces reach 17.0552 and 3.0 mm, refused as "the
        // sizes 2 mm / 1 mm are too large" while the stale Pocket still held four
        // edges of 1.29e-05 .. 2.23e-05 mm (129 edges instead of the 123 the
        // sliver retry produces).  That hint sends the user looking at the sizes
        // instead of at the base.  Say which of the two it is.
        if (!usable(shape)) {
            const std::string reason = invalidChamferReason(shape);
            const int baseSlivers = static_cast<int>(TopShape.getDegenerateEdges().size());
            const bool angleType
                = static_cast<Part::ChamferType>(chamferType) == Part::ChamferType::distanceAngle;
            std::ostringstream sizes;
            if (angleType) {
                sizes << "size " << Size.getValue() << " mm / angle " << Angle.getValue() << " deg";
            }
            else {
                sizes << "sizes " << Size.getValue() << " mm / " << Size2.getValue() << " mm";
            }
            std::ostringstream message;
            message << "Chamfer produced an invalid shape (" << reason << "). ";
            if (baseSlivers > 0) {
                message << "The base still has " << baseSlivers
                        << " edges shorter than 1e-3 mm where a boolean ran into existing "
                           "geometry at the ends of this edge; OCCT folds a face there. Move the "
                           "profile edge that grazes (or its sketch) off the existing geometry, "
                           "or chamfer another edge. The "
                        << sizes.str()
                        << (reachRefusal.empty() ? " fit the adjacent faces."
                                                 : " do not fit every adjacent face either.");
            }
            else if (!reachRefusal.empty()) {
                message << "The " << sizes.str()
                        << " are too large for this edge - reduce them or check the edge.";
            }
            else {
                message << "The " << sizes.str()
                        << " do not give a valid chamfer on this edge - check the edge, or try "
                           "different sizes.";
            }
            if (wedgeOffFaces) {
                message << " An equivalent wedge was tried but would have stood off the adjacent "
                           "faces instead of lying on them.";
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
            // A convex edge has the wedge taken off, a concave one has its valley
            // filled.  Be honest about how it differs from OCCT's chamfer: OCCT
            // trims its chamfer at the ends, while the wedge has sharp ends, so the
            // wedge can be substantially larger - not "a few percent".  Measured at
            // 17 x 2.99 on the user's Pocket002 valley: 873.4 mm3 of wedge against
            // the 591.1 mm3 reference chamfer (+48 %); on the local reference
            // fixture OCCT succeeds and the wedge is not used at all.
            if (convexity < 0.0) {
                Base::Console().warning(
                    "{}: OCCT cannot build this chamfer (the distances fit the adjacent faces); "
                    "filled the valley with an equivalent wedge instead - it has sharp ends "
                    "rather than the corner blend OCCT's chamfer has, so it can be "
                    "substantially larger at the ends.\n",
                    getFullName()
                );
            }
            else {
                Base::Console().warning(
                    "{}: OCCT cannot build this chamfer (the distances fit the adjacent faces); "
                    "built an equivalent wedge cut instead - it has sharp ends rather than the "
                    "corner blend OCCT's chamfer has, so it can be substantially larger at the "
                    "ends.\n",
                    getFullName()
                );
            }
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
