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

bool wedgeLegs(const TopoDS_Edge& edge,
               const TopoDS_Face& face0,
               const TopoDS_Face& face1,
               double size0,
               double size1,
               WedgeLegs& legs)
{
    if (BRep_Tool::Degenerated(edge)) {
        return false;
    }
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Line) {
        return false;
    }
    for (const TopoDS_Face& face : {face0, face1}) {
        BRepAdaptor_Surface surface(face, Standard_False);
        if (surface.GetType() != GeomAbs_Plane) {
            return false;
        }
    }
    legs.start = curve.Value(curve.FirstParameter());
    const gp_Pnt end = curve.Value(curve.LastParameter());
    legs.length = legs.start.Distance(end);
    if (legs.length < Precision::Confusion()) {
        return false;
    }
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
        BRepAdaptor_Curve curve(TopoDS::Edge(edgeShape.getShape()));
        if (curve.GetType() != GeomAbs_Line) {
            return 0.0;
        }
        const double length = curve.Value(curve.FirstParameter())
            .Distance(curve.Value(curve.LastParameter()));
        if (length < Precision::Confusion()) {
            return 0.0;
        }
        volume += 0.5 * size0 * size1 * length;
    }
    return volume;
}

//! Chamfers each edge by subtracting a wedge prism, and returns the cut shape
//! (or a null shape when that is not possible).
Part::TopoShape chamferEdgesByWedge(const Part::TopoShape& base,
                                    const std::vector<Part::TopoShape>& edges,
                                    Part::ChamferType type,
                                    double size,
                                    double size2,
                                    bool flip)
{
    // Both assignments of the two distances are tried: OCC measures the first
    // one on the face the caller picked, and for this fallback it does not matter
    // which face that is - the orientation search below keeps whichever really
    // removes a wedge (0.5 * size0 * size1 * length is the same either way).
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
        double bestError = std::numeric_limits<double>::max();
        double bestExpected = 0.0;
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
            // Pick the triangle orientation that really removes a chamfer wedge:
            // the faces' interior directions give the two candidate legs, the
            // material side decides their signs.
            const double expected = 0.5 * size0 * size1 * legs.length;
            for (int sign0 : {-1, 1}) {
                for (int sign1 : {-1, 1}) {
                    const TopoDS_Shape tool = wedgeTool(legs, sign0, sign1);
                    if (tool.IsNull()) {
                        continue;
                    }
                    Part::TopoShape toolShape(0);
                    toolShape.setShape(tool);
                    const Part::TopoShape candidate = base.makeElementCut(
                        toolShape,
                        Part::OpCodes::Chamfer
                    );
                    if (candidate.isNull()) {
                        continue;
                    }
                    const double removed = baseVolume - shapeVolume(candidate.getShape());
                    const double error = std::abs(removed - expected);
                    if (error < bestError) {
                        bestError = error;
                        bestExpected = expected;
                        best = tool;
                    }
                }
            }
        }
        if (best.IsNull() || bestError > bestExpected * 0.5) {
            return Part::TopoShape(0);  // no orientation removes a chamfer wedge
        }
        builder.Add(compound, best);
        ++used;
    }
    if (used == 0) {
        return Part::TopoShape(0);
    }

    Part::TopoShape toolShape(0);
    toolShape.setShape(compound);
    return base.makeElementCut(toolShape, Part::OpCodes::Chamfer);
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
        // A chamfer only ever takes material off.  A candidate that adds it is
        // the concave-side chamfer OCCT produces from the wrong face assignment
        // - it silently grows the solid by a whole wedge (the user's 50 mm edge:
        // Pocket + 1270.75) - so it is not a chamfer the user asked for.
        const double expectedWedge = chamferWedgeVolume(TopShape, edges, size, wedgeSize1);
        const double baseVolume = shapeVolume(TopShape.getShape());
        auto removedBy = [&](const TopoShape& candidate) {
            return baseVolume - shapeVolume(candidate.getShape());
        };
        auto acceptable = [&](const TopoShape& candidate) {
            return usable(candidate) && removedBy(candidate) > Precision::Confusion();
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
            // among the rest, the one whose removed volume is closest to the
            // analytic wedge is the chamfer the user asked for
            double bestError = std::numeric_limits<double>::max();
            TopoShape best;
            for (std::size_t i = 1; i < attempts.size(); ++i) {
                try {
                    TopoShape other = buildChamfer(attempts[i].first, attempts[i].second);
                    if (!acceptable(other)) {
                        continue;
                    }
                    const double error = std::abs(removedBy(other) - expectedWedge);
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
                flipDirection
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
