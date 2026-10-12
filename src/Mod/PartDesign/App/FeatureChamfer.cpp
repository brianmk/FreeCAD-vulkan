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

#include <cmath>
#include <limits>

#include <BRepAlgo.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_ShapeTolerance.hxx>
#include <Standard_Version.hxx>


#include <Base/Console.h>
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

namespace {

// ---------------------------------------------------------------------------
// Wedge-cut fallback for chamfers that OCCT refuses.
//
// OCCT's ChFi3d corner builder cannot process an edge whose end vertex touches
// a non-manifold edge (one shared by more than two faces).  A boolean cut can
// leave such an edge behind when its boundary coincides exactly with existing
// geometry, and BRepCheck does not flag it, so the solid still looks valid.  In
// that situation ChFi3d raises the opaque "ChFi3d_Builder:only 2 faces" or
// crashes inside ChFi3d_Builder::IntersectMoreCorner.
//
// Such an edge can still be chamfered the way a user would model it by hand:
// subtract a prism whose cross-section is the chamfer triangle.  Only straight
// edges between two planar faces are handled; for anything else the caller keeps
// the original OCCT error.
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

// in-plane direction from the edge into the interior of a planar face
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

TopoDS_Shape wedgeTool(const WedgeLegs& legs, int sign0, int sign1)
{
    // Keep the tool's end faces off the solid's own faces: coincident end faces
    // would introduce exactly the kind of sliver this fallback exists to dodge.
    const double ext = std::max(legs.length, 1.0) * 1e-3;
    const gp_Pnt start = legs.start.Translated(gp_Vec(legs.direction) * -ext);
    BRepBuilderAPI_MakePolygon polygon;
    polygon.Add(start);
    polygon.Add(start.Translated(gp_Vec(legs.leg0) * (legs.size0 * sign0)));
    polygon.Add(start.Translated(gp_Vec(legs.leg1) * (legs.size1 * sign1)));
    polygon.Close();
    BRepBuilderAPI_MakeFace face(polygon.Wire());
    if (!face.IsDone()) {
        return {};
    }
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(legs.direction) * (legs.length + 2 * ext));
}

//! True when an end vertex of one of \a edges touches an edge shared by more than
//! two faces.  ChFi3d cannot chamfer such a corner (it raises "only 2 faces" or
//! crashes), so the wedge fallback is used instead of asking OCCT at all.
bool hasNonManifoldCorner(const Part::TopoShape& shape,
                          const std::vector<Part::TopoShape>& edges)
{
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape.getShape(), TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    TopTools_IndexedDataMapOfShapeListOfShape vertexEdges;
    TopExp::MapShapesAndAncestors(shape.getShape(), TopAbs_VERTEX, TopAbs_EDGE, vertexEdges);

    for (const auto& edgeShape : edges) {
        if (edgeShape.isNull() || edgeShape.getShape().ShapeType() != TopAbs_EDGE) {
            continue;
        }
        for (TopExp_Explorer vx(edgeShape.getShape(), TopAbs_VERTEX); vx.More(); vx.Next()) {
            if (!vertexEdges.Contains(vx.Current())) {
                continue;
            }
            for (TopTools_ListIteratorOfListOfShape it(vertexEdges.FindFromKey(vx.Current()));
                 it.More();
                 it.Next()) {
                if (edgeFaces.Contains(it.Value())
                    && edgeFaces.FindFromKey(it.Value()).Extent() > 2) {
                    return true;
                }
            }
        }
    }
    return false;
}

Part::TopoShape chamferEdgesByWedge(const Part::TopoShape& base,
                                    const std::vector<Part::TopoShape>& edges,
                                    Part::ChamferType type,
                                    double size,
                                    double size2,
                                    bool flip)
{
    double size0 = size;
    double size1 = size;
    if (type == Part::ChamferType::twoDistances) {
        size1 = size2;
        if (flip) {
            std::swap(size0, size1);
        }
    }
    else if (type == Part::ChamferType::distanceAngle) {
        // Chamfer::execute() hands the angle (in degrees) over in size2
        const double radians = Base::toRadians(size2);
        if (std::abs(std::tan(radians)) < Precision::Confusion()) {
            return Part::TopoShape(0);
        }
        size1 = size * std::tan(radians);
        if (flip) {
            std::swap(size0, size1);
        }
    }
    if (size0 <= Precision::Confusion() || size1 <= Precision::Confusion()) {
        return Part::TopoShape(0);
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
        // Pick the triangle orientation that really removes a chamfer wedge: the
        // faces' interior directions give the two candidate legs, the material
        // side decides their signs.
        const double expected = 0.5 * size0 * size1 * legs.length;
        double bestError = std::numeric_limits<double>::max();
        TopoDS_Shape best;
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
                    best = tool;
                }
            }
        }
        if (best.IsNull() || bestError > expected * 0.5) {
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
    // Shared post-processing of a candidate chamfer shape, and the fallback used
    // when OCCT's ChFi3d refuses the edges.
    auto finish = [&](TopoShape shape) -> App::DocumentObjectExecReturn* {
        if (shape.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Failed to create chamfer")
            );
        }

        TopTools_ListOfShape aLarg;
        aLarg.Append(TopShape.getShape());
        if (!BRepAlgo::IsValid(aLarg, shape.getShape(), Standard_False, Standard_False)) {
            ShapeFix_ShapeTolerance aSFT;
            aSFT.LimitTolerance(
                shape.getShape(),
                Precision::Confusion(),
                Precision::Confusion(),
                TopAbs_SHAPE
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
    };
    // Returns true when the wedge fallback produced the result.  Note that
    // App::DocumentObject::StdReturn is a null pointer, so success has to be
    // tested against that sentinel, never by testing the pointer itself.
    auto tryWedge = [&](App::DocumentObjectExecReturn*& result) -> bool {
        TopoShape wedged = chamferEdgesByWedge(
            TopShape,
            edges,
            static_cast<Part::ChamferType>(chamferType),
            size,
            size2,
            flipDirection
        );
        if (wedged.isNull()) {
            return false;  // not a case the fallback can handle: keep the OCC error
        }
        Base::Console().warning(
            "{}: a corner of the chamfered edge touches a non-manifold edge, which OCCT's "
            "fillet/chamfer cannot process; subtracted a wedge instead. The chamfer has sharp "
            "ends and no corner blend.\n",
            this->getFullName()
        );
        result = finish(wedged);
        return true;
    };
    // Detect the non-manifold corner up front: asking OCCT is pointless there (it
    // only fails, and for some edges it crashes).
    if (hasNonManifoldCorner(TopShape, edges)) {
        App::DocumentObjectExecReturn* wedged = App::DocumentObject::StdReturn;
        if (tryWedge(wedged)) {
            return wedged;
        }
    }
    try {
        TopoShape shape(0);
        Part::SignalException sig;
        shape.makeElementChamfer(
            TopShape,
            edges,
            static_cast<Part::ChamferType>(chamferType),
            size,
            size2,
            nullptr,
            flipDirection ? Part::Flip::flip : Part::Flip::none
        );
        if (shape.isNull()) {
            App::DocumentObjectExecReturn* wedged = App::DocumentObject::StdReturn;
            if (tryWedge(wedged)) {
                return wedged;
            }
        }
        return finish(shape);
    }
    catch (Base::Exception& e) {
        App::DocumentObjectExecReturn* wedged = App::DocumentObject::StdReturn;
        if (tryWedge(wedged)) {
            return wedged;
        }
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (Standard_Failure& e) {
        App::DocumentObjectExecReturn* wedged = App::DocumentObject::StdReturn;
        if (tryWedge(wedged)) {
            return wedged;
        }
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    catch (...) {
        App::DocumentObjectExecReturn* wedged = App::DocumentObject::StdReturn;
        if (tryWedge(wedged)) {
            return wedged;
        }
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
