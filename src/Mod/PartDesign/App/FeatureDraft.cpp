// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2012 Jan Rheinländer                                    *
 *                                   <jrheinlaender@users.sourceforge.net> *
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


#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <GeomAPI_IntSS.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>


#include <App/Datums.h>
#include <App/GeoFeature.h>
#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Tools.h>
#include <Mod/Part/App/Part2DObject.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureDraft.h"
#include "DatumLine.h"
#include "DatumPlane.h"


using namespace PartDesign;

namespace
{

/// Surface area of a shape, used to tell whether a split face is covered by the profile
double getArea(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    return props.Mass();
}

/// Returns the plane of a planar face, false for anything else
bool getPlane(const TopoDS_Shape& shape, gp_Pln& plane)
{
    if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE) {
        return false;
    }

    BRepAdaptor_Surface adapt(TopoDS::Face(shape));
    if (adapt.GetType() != GeomAbs_Plane) {
        return false;
    }

    plane = adapt.Plane();
    return true;
}

/// True if the whole face lies inside one of the regions
bool isInsideRegions(const TopoDS_Shape& face, const std::vector<TopoDS_Shape>& regions)
{
    const double faceArea = getArea(face);
    if (faceArea <= Precision::Confusion()) {
        return false;
    }

    for (const auto& region : regions) {
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape arguments;
        TopTools_ListOfShape tools;
        arguments.Append(face);
        tools.Append(region);
        common.SetArguments(arguments);
        common.SetTools(tools);
        common.Build();

        if (!common.IsDone() || common.Shape().IsNull()) {
            continue;
        }

        // The face lies inside the region if their intersection is the face itself
        if (std::abs(getArea(common.Shape()) - faceArea) <= 1e-6 * faceArea) {
            return true;
        }
    }

    return false;
}

}  // namespace


PROPERTY_SOURCE(PartDesign::Draft, PartDesign::DressUp)

const App::PropertyAngle::Constraints Draft::floatAngle
    = {-90.0, 90.0 - Base::toDegrees<double>(Precision::Angular()), 0.1};

Draft::Draft()
{
    ADD_PROPERTY(Angle, (1.5));
    Angle.setConstraints(&floatAngle);
    ADD_PROPERTY_TYPE(NeutralPlane, (nullptr), "Draft", (App::PropertyType)(App::Prop_None), "NeutralPlane");
    ADD_PROPERTY_TYPE(
        PullDirection,
        (nullptr),
        "Draft",
        (App::PropertyType)(App::Prop_None),
        "PullDirection"
    );
    ADD_PROPERTY_TYPE(
        Profile,
        (nullptr),
        "Draft",
        (App::PropertyType)(App::Prop_None),
        "Sketch or planar profile used to split the base face(s).\n"
        "Only the part of a base face lying inside the profile's enclosed region is drafted.\n"
        "Leave empty to draft the whole selected face(s)."
    );
    ADD_PROPERTY(Reversed, (0));
}

void Draft::handleChangedPropertyType(Base::XMLReader& reader, const char* TypeName, App::Property* prop)
{
    Base::Type inputType = Base::Type::fromName(TypeName);
    if (prop == &Angle && inputType == App::PropertyFloatConstraint::getClassTypeId()) {
        App::PropertyFloatConstraint v;
        v.Restore(reader);
        Angle.setValue(v.getValue());
    }
    else {
        DressUp::handleChangedPropertyType(reader, TypeName, prop);
    }
}

short Draft::mustExecute() const
{
    if (Placement.isTouched() || Angle.isTouched() || NeutralPlane.isTouched()
        || PullDirection.isTouched() || Profile.isTouched() || Reversed.isTouched()) {
        return 1;
    }
    return DressUp::mustExecute();
}

App::DocumentObjectExecReturn* Draft::execute()
{
    // Get parameters
    // Base shape
    Part::TopoShape TopShape;
    try {
        TopShape = getBaseTopoShape();
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }

    // Faces where draft should be applied
    // Note: Cannot be const reference currently because of BRepOffsetAPI_DraftAngle::Remove() bug,
    // see below
    std::vector<std::string> SubVals = Base.getSubValuesStartsWith("Face");

    // If no element is selected, then we use a copy of previous feature.
    if (SubVals.empty()) {
        this->positionByBaseFeature();
        this->Shape.setValue(TopShape);
        return App::DocumentObject::StdReturn;
    }

    // Draft angle
    double angle = Base::toRadians(Angle.getValue());

    // Pull direction
    gp_Dir pullDirection;
    App::DocumentObject* refDirection = PullDirection.getValue();
    if (refDirection) {
        if (refDirection->isDerivedFrom<PartDesign::Line>()) {
            PartDesign::Line* line = static_cast<PartDesign::Line*>(refDirection);
            Base::Vector3d d = line->getDirection();
            pullDirection = gp_Dir(d.x, d.y, d.z);
        }
        else if (refDirection->isDerivedFrom<App::Line>()) {
            App::Line* line = static_cast<App::Line*>(refDirection);
            Base::Vector3d d = line->getDirection();
            pullDirection = gp_Dir(d.x, d.y, d.z);
        }
        else if (refDirection->isDerivedFrom<Part::Feature>()) {
            std::vector<std::string> subStrings = PullDirection.getSubValues();
            if (subStrings.empty() || subStrings[0].empty()) {
                throw Base::ValueError("No pull direction reference specified");
            }

            Part::Feature* refFeature = static_cast<Part::Feature*>(refDirection);
            Part::TopoShape refShape = refFeature->Shape.getShape();
            TopoDS_Shape ref = refShape.getSubShape(subStrings[0].c_str());

            if (ref.ShapeType() == TopAbs_EDGE) {
                TopoDS_Edge refEdge = TopoDS::Edge(ref);
                if (refEdge.IsNull()) {
                    throw Base::ValueError("Failed to extract pull direction reference edge");
                }
                BRepAdaptor_Curve adapt(refEdge);
                if (adapt.GetType() != GeomAbs_Line) {
                    throw Base::TypeError("Pull direction reference edge must be linear");
                }

                pullDirection = adapt.Line().Direction();
            }
            else {
                throw Base::TypeError("Pull direction reference must be an edge or a datum line");
            }
        }
        else {
            throw Base::TypeError(
                "Pull direction reference must be an edge of a feature or a datum line"
            );
        }

        TopLoc_Location invObjLoc = this->getLocation().Inverted();
        pullDirection.Transform(invObjLoc.Transformation());
    }

    // Neutral plane
    gp_Pln neutralPlane;
    App::DocumentObject* refPlane = NeutralPlane.getValue();
    if (!refPlane) {
        // Try to guess a neutral plane from the first selected face
        // Get edges of first selected face
        TopoDS_Shape face = TopShape.getSubShape(SubVals[0].c_str());
        TopTools_IndexedMapOfShape mapOfEdges;
        TopExp::MapShapes(face, TopAbs_EDGE, mapOfEdges);
        bool found = false;

        for (int i = 1; i <= mapOfEdges.Extent(); i++) {
            // Note: What happens if mapOfEdges(i) is the degenerated edge of a cone?
            // But in that case the draft is not possible anyway!
            BRepAdaptor_Curve c(TopoDS::Edge(mapOfEdges(i)));
            gp_Pnt p1 = c.Value(c.FirstParameter());
            gp_Pnt p2 = c.Value(c.LastParameter());

            if (c.IsClosed()) {
                // Edge is a circle or a circular arc (other types are not allowed for drafting)
                if (c.GetType() == GeomAbs_Circle) {
                    neutralPlane = gp_Pln(p1, c.Circle().Axis().Direction());
                    found = true;
                    break;
                }
            }
            else {
                // Edge is linear
                // Find midpoint of edge and create auxiliary plane through midpoint normal to edge
                gp_Pnt pm = c.Value((c.FirstParameter() + c.LastParameter()) / 2.0);
                Handle(Geom_Plane) aux
                    = new Geom_Plane(pm, gp_Dir(p2.X() - p1.X(), p2.Y() - p1.Y(), p2.Z() - p1.Z()));
                // Intersect plane with face. Is there no easier way?
                BRepAdaptor_Surface adapt(TopoDS::Face(face), Standard_False);
                Handle(Geom_Surface) sf = adapt.Surface().Surface();
                GeomAPI_IntSS intersector(aux, sf, Precision::Confusion());
                if (!intersector.IsDone() || intersector.NbLines() < 1) {
                    continue;
                }
                const Handle(Geom_Curve) & icurve = intersector.Line(1);
                if (!icurve->IsKind(STANDARD_TYPE(Geom_Line))) {
                    continue;
                }
                Handle(Geom_Line) line = Handle(Geom_Line)::DownCast(icurve);
                neutralPlane = gp_Pln(pm, line->Lin().Direction());
                found = true;
                break;
            }
        }

        if (!found) {
            throw Base::RuntimeError("No neutral plane specified and none can be guessed");
        }
    }
    else {
        if (refPlane->isDerivedFrom<PartDesign::Plane>()) {
            PartDesign::Plane* plane = static_cast<PartDesign::Plane*>(refPlane);
            Base::Vector3d b = plane->getBasePoint();
            Base::Vector3d n = plane->getNormal();
            neutralPlane = gp_Pln(gp_Pnt(b.x, b.y, b.z), gp_Dir(n.x, n.y, n.z));
        }
        else if (
            refPlane->isDerivedFrom<App::Plane>() || refPlane->isDerivedFrom<Part::Part2DObject>()
        ) {
            neutralPlane = Feature::makePlnFromPlane(refPlane);
        }
        else if (refPlane->isDerivedFrom<Part::Feature>()) {
            std::vector<std::string> subStrings = NeutralPlane.getSubValues();
            if (subStrings.empty() || subStrings[0].empty()) {
                throw Base::ValueError("No neutral plane reference specified");
            }

            Part::Feature* refFeature = static_cast<Part::Feature*>(refPlane);
            Part::TopoShape refShape = refFeature->Shape.getShape();
            TopoDS_Shape ref = refShape.getSubShape(subStrings[0].c_str());

            if (ref.ShapeType() == TopAbs_FACE) {
                TopoDS_Face refFace = TopoDS::Face(ref);
                if (refFace.IsNull()) {
                    throw Base::ValueError("Failed to extract neutral plane reference face");
                }
                BRepAdaptor_Surface adapt(refFace);
                if (adapt.GetType() != GeomAbs_Plane) {
                    throw Base::TypeError("Neutral plane reference face must be planar");
                }

                neutralPlane = adapt.Plane();
            }
            else if (ref.ShapeType() == TopAbs_EDGE) {
                if (refDirection) {
                    // Create neutral plane through edge normal to pull direction
                    TopoDS_Edge refEdge = TopoDS::Edge(ref);
                    if (refEdge.IsNull()) {
                        throw Base::ValueError("Failed to extract neutral plane reference edge");
                    }
                    BRepAdaptor_Curve c(refEdge);
                    if (c.GetType() != GeomAbs_Line) {
                        throw Base::TypeError("Neutral plane reference edge must be linear");
                    }
                    double a = c.Line().Angle(gp_Lin(c.Value(c.FirstParameter()), pullDirection));
                    if (std::fabs(a - std::numbers::pi / 2) > Precision::Confusion()) {
                        throw Base::ValueError(
                            "Neutral plane reference edge must be normal to pull direction"
                        );
                    }
                    neutralPlane = gp_Pln(c.Value(c.FirstParameter()), pullDirection);
                }
                else {
                    throw Base::TypeError(
                        "Neutral plane reference can only be an edge if pull direction is defined"
                    );
                }
            }
            else {
                throw Base::TypeError("Neutral plane reference must be a face");
            }
        }
        else {
            throw Base::TypeError("Neutral plane reference must be face of a feature or a datum plane");
        }

        TopLoc_Location invObjLoc = this->getLocation().Inverted();
        neutralPlane.Transform(invObjLoc.Transformation());
    }

    if (!refDirection) {
        // Choose pull direction normal to neutral plane
        pullDirection = neutralPlane.Axis().Direction();
    }

    // Reversed pull direction
    bool reversed = Reversed.getValue();
    if (reversed) {
        angle *= -1.0;
    }

    computeProps = {pullDirection, neutralPlane};

    this->positionByBaseFeature();
    // create an untransformed copy of the base shape
    Part::TopoShape baseShape(TopShape);
    baseShape.setTransform(Base::Matrix4D());
    try {
        std::vector<TopoShape> faces = getFaces(baseShape);
        // Without a profile these are the selected faces themselves
        std::vector<TopoShape> draftFaces = faces;

        // A profile splits the selected faces. Only the resulting sub-faces that
        // lie inside the profile's region are drafted.
        Part::TopoShape splitShape(baseShape);
        if (Profile.getValue()) {
            splitShape = splitBaseFaces(baseShape, faces, draftFaces);
        }

        TopoShape shape({}, getDocument()->getStringHasher());
        shape.makeElementDraft(
            splitShape,
            draftFaces,
            pullDirection,
            angle,
            neutralPlane,
            reversed
        );

        if (shape.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Resulting shape is null")
            );
        }

        if (!isSingleSolidRuleSatisfied(shape.getShape())) {
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                "Exception",
                "Result has multiple solids: enable 'Allow Compound' in the active body."
            ));
        }

        this->Shape.setValue(getSolid(shape));
        return App::DocumentObject::StdReturn;
    }
    catch (Standard_Failure& e) {
        std::string message = e.GetMessageString();
        if (message.empty()) {
            // several OCC draft failures carry no message at all
            message = e.DynamicType()->Name();
        }
        return new App::DocumentObjectExecReturn(message);
    }
}

Part::TopoShape Draft::splitBaseFaces(
    const Part::TopoShape& baseShape,
    const std::vector<Part::TopoShape>& faces,
    std::vector<Part::TopoShape>& draftFaces
) const
{
    App::DocumentObject* profileObject = Profile.getValue();
    if (!profileObject) {
        return baseShape;
    }

    if (!profileObject->isDerivedFrom<Part::Feature>()) {
        throw Base::TypeError("Profile must be a Part feature, for example a sketch");
    }

    auto baseObject = getBaseObject();
    if (!baseObject) {
        throw Base::RuntimeError("No base object for the profile");
    }

    // Bring the profile into the coordinate system of the base shape. The shape
    // returned by Part::Feature::Shape carries the object's own placement, so it
    // is removed first and the placement relative to the base object is applied.
    Part::TopoShape profileShape = static_cast<Part::Feature*>(profileObject)->Shape.getShape();
    const auto& subValues = Profile.getSubValues();
    if (!subValues.empty() && !subValues.front().empty()) {
        profileShape = profileShape.getSubTopoShape(subValues.front().c_str());
    }
    if (profileShape.isNull()) {
        throw Base::ValueError("Profile shape is empty");
    }

    profileShape.setTransform(Base::Matrix4D());
    {
        Base::Matrix4D placement
            = App::GeoFeature::getGlobalPlacement(baseObject).inverse().toMatrix();
        placement *= App::GeoFeature::getGlobalPlacement(profileObject).toMatrix();
        profileShape.transformShape(placement, false, true);
    }

    // The profile has to enclose at least one closed region
    Part::TopoShape region({}, getDocument()->getStringHasher());
    if (profileShape.hasSubShape(TopAbs_FACE)) {
        region = profileShape;
    }
    else {
        region.makeElementFace(profileShape);
    }

    auto regionFaces = region.getSubTopoShapes(TopAbs_FACE);
    if (regionFaces.empty()) {
        throw Base::ValueError("Profile does not enclose a region, use a closed sketch");
    }

    // Move the profile's region onto the plane of every base face and use the
    // moved regions as splitting tools.
    TopTools_ListOfShape tools;
    std::vector<std::vector<TopoDS_Shape>> regionsPerFace;
    regionsPerFace.reserve(faces.size());

    for (const auto& face : faces) {
        gp_Pln facePlane;
        if (!getPlane(face.getShape(), facePlane)) {
            throw Base::TypeError("Base face must be planar to be split by a profile");
        }

        const gp_Dir faceNormal = facePlane.Axis().Direction();
        const double faceOffset = facePlane.Location().XYZ().Dot(faceNormal.XYZ());

        std::vector<TopoDS_Shape> faceRegions;
        for (const auto& regionFace : regionFaces) {
            gp_Pln regionPlane;
            if (!getPlane(regionFace.getShape(), regionPlane)) {
                // only planar regions can be moved onto a planar base face
                continue;
            }

            if (std::abs(faceNormal.Dot(regionPlane.Axis().Direction())) < 1.0 - 1e-9) {
                // the region is not parallel to this face and cannot split it
                continue;
            }

            const double regionOffset
                = regionPlane.Location().XYZ().Dot(faceNormal.XYZ());
            gp_Trsf translation;
            translation.SetTranslation(gp_Vec(faceNormal.XYZ() * (faceOffset - regionOffset)));

            TopoDS_Shape moved
                = BRepBuilderAPI_Transform(regionFace.getShape(), translation, true).Shape();
            faceRegions.push_back(moved);
            tools.Append(moved);
        }
        regionsPerFace.push_back(std::move(faceRegions));
    }

    if (tools.IsEmpty()) {
        throw Base::ValueError("Profile is not parallel to any of the selected faces");
    }

    // Split the base shape with the profile regions
    BRepAlgoAPI_Splitter splitter;
    TopTools_ListOfShape arguments;
    arguments.Append(baseShape.getShape());
    splitter.SetArguments(arguments);
    splitter.SetTools(tools);
    splitter.Build();

    if (!splitter.IsDone() || splitter.Shape().IsNull()) {
        throw Base::CADKernelError("Failed to split the base face with the profile");
    }

    Part::TopoShape result({}, getDocument()->getStringHasher());
    result.makeElementShape(splitter, baseShape, Part::OpCodes::Split);

    // Keep only the sub-faces of the split that lie inside the profile's region
    std::vector<Part::TopoShape> insideFaces;
    for (size_t i = 0; i < faces.size(); ++i) {
        for (TopTools_ListIteratorOfListOfShape it(splitter.Modified(faces[i].getShape())); it.More();
             it.Next()) {
            if (it.Value().ShapeType() != TopAbs_FACE) {
                continue;
            }
            if (isInsideRegions(it.Value(), regionsPerFace[i])) {
                insideFaces.emplace_back(it.Value());
            }
        }
    }

    if (insideFaces.empty()) {
        throw Base::ValueError("Profile does not cover any part of the base face");
    }

    draftFaces = std::move(insideFaces);

    return result;
}
