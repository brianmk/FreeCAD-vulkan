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

#include <limits>


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
#include "Body.h"
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

/// Area of the intersection of two shapes, zero when they do not intersect
double getIntersectionArea(const TopoDS_Shape& shape, const TopoDS_Shape& region)
{
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments;
    TopTools_ListOfShape tools;
    arguments.Append(shape);
    tools.Append(region);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.Build();

    if (!common.IsDone() || common.Shape().IsNull()) {
        return 0.0;
    }

    return getArea(common.Shape());
}

/// True if the whole face lies inside one of the regions
bool isInsideRegions(const TopoDS_Shape& face, const std::vector<TopoDS_Shape>& regions)
{
    const double faceArea = getArea(face);
    if (faceArea <= Precision::Confusion()) {
        return false;
    }

    for (const auto& region : regions) {
        // The face lies inside the region if their intersection is the face itself
        if (std::abs(getIntersectionArea(face, region) - faceArea) <= 1e-6 * faceArea) {
            return true;
        }
    }

    return false;
}

/// True if the object is a 2D shape (a sketch) rather than a solid
bool isProfileObject(const App::DocumentObject* object)
{
    if (!object || !object->isDerivedFrom<Part::Feature>()) {
        return false;
    }

    if (object->isDerivedFrom<Part::Part2DObject>()) {
        return true;
    }

    const auto& shape = static_cast<const Part::Feature*>(object)->Shape.getShape();
    return !shape.isNull() && !shape.hasSubShape(TopAbs_SOLID);
}

/// True if the object provides a solid shape
bool hasSolidShape(const App::DocumentObject* object)
{
    return object && object->isDerivedFrom<Part::Feature>()
        && static_cast<const Part::Feature*>(object)->Shape.getShape().hasSubShape(TopAbs_SOLID);
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
    // The profile is either the Profile property, or - when Base itself holds a
    // 2D object (a sketch picked in the 3D view) - that object.
    App::DocumentObject* baseLink = Base.getValue();
    App::DocumentObject* profileLink = Profile.getValue();

    const bool baseIs2D = isProfileObject(baseLink);
    // "FaceN" sub-elements of a solid feature are what Base normally holds
    const bool baseHasFaces = !Base.getSubValuesStartsWith("Face").empty();

    App::DocumentObject* profileObject = profileLink;
    std::vector<std::string> profileSubs = Profile.getSubValues();
    if (!profileObject && baseIs2D) {
        profileObject = baseLink;
        profileSubs = Base.getSubValues();
    }

    // Without a base face the face below the profile's region is drafted
    const bool profileInBase
        = profileObject && isProfileObject(profileObject) && (baseIs2D || !baseHasFaces);

    // Base shape.  With a profile the draft is applied to the solid below it.
    Part::Feature* baseObject = nullptr;
    Part::TopoShape TopShape;
    if (profileInBase) {
        baseObject = getBaseSolidObject();
        if (!baseObject) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "No solid feature below the profile")
            );
        }
        TopShape = baseObject->Shape.getShape();
        if (!TopShape.hasSubShape(TopAbs_SOLID)) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Base feature's shape is not a solid")
            );
        }
    }
    else {
        try {
            TopShape = getBaseTopoShape();
        }
        catch (Base::Exception& e) {
            return new App::DocumentObjectExecReturn(e.what());
        }
        baseObject = getBaseObject();
    }

    // create an untransformed copy of the base shape
    Part::TopoShape baseShape(TopShape);
    baseShape.setTransform(Base::Matrix4D());

    // Frame in which the base shape is expressed.  When a 2D object in Base acts
    // as the profile, the placement of the solid feature below it is used.
    gp_Trsf invBaseTrsf;
    if (profileInBase && baseObject) {
        invBaseTrsf = TopoShape::convert(baseObject->Placement.getValue().inverse().toMatrix());
    }
    else {
        invBaseTrsf = this->getLocation().Inverted().Transformation();
    }

    // Faces where draft should be applied
    // Note: Cannot be const reference currently because of BRepOffsetAPI_DraftAngle::Remove() bug,
    // see below
    std::vector<std::string> SubVals = Base.getSubValuesStartsWith("Face");

    // If no element is selected, then we use a copy of previous feature.
    if (!profileInBase && SubVals.empty()) {
        this->positionByBaseFeature();
        this->Shape.setValue(TopShape);
        return App::DocumentObject::StdReturn;
    }

    // With a profile the faces to draft are the ones below the profile's region
    Part::TopoShape region;
    std::vector<TopoShape> profileFaces;
    if (profileObject) {
        try {
            region = getProfileRegion(baseObject, profileObject, profileSubs);
            if (profileInBase) {
                profileFaces
                    = getFacesUnderProfile(baseShape, region, profileObject, baseObject);
            }
        }
        catch (Base::Exception& e) {
            return new App::DocumentObjectExecReturn(e.what());
        }
        if (profileInBase && profileFaces.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "No base face found below the profile")
            );
        }
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

        pullDirection.Transform(invBaseTrsf);
    }

    // Reversed pull direction
    bool reversed = Reversed.getValue();
    if (reversed) {
        angle *= -1.0;
    }

    this->positionByBaseFeature();
    if (profileInBase && baseObject) {
        // BaseFeature may still point at the profile in documents saved before
        // the profile was moved out of it, so take the placement from the solid
        this->Placement.setValue(baseObject->Placement.getValue());
    }
    try {
        // Faces to draft: the ones below a profile, or the faces selected in Base
        std::vector<TopoShape> faces = profileInBase ? profileFaces : getFaces(baseShape);
        // Without a profile these are the selected faces themselves
        std::vector<TopoShape> draftFaces = faces;

        // A profile splits the selected faces. Only the resulting sub-faces that
        // lie inside the profile's region are drafted.
        Part::TopoShape splitShape(baseShape);
        if (!region.isNull()) {
            splitShape = splitBaseFaces(baseShape, region, faces, baseObject, draftFaces);
        }

        // Neutral plane.  It comes from the NeutralPlane property, otherwise it is
        // guessed from the face that is drafted (after a split that can be a
        // sub-face).  Only some hinges are accepted by the OCC draft, so the
        // candidates are tried in turn.
        std::vector<gp_Pln> neutralPlanes;
        if (NeutralPlane.getValue()) {
            neutralPlanes.push_back(getExplicitNeutralPlane(refDirection, pullDirection, invBaseTrsf));
        }
        else {
            neutralPlanes = getNeutralPlaneCandidates(draftFaces.front().getShape(), faces);
        }

        TopoShape shape({}, getDocument()->getStringHasher());
        gp_Pln neutralPlane;
        std::string error;
        for (const auto& plane : neutralPlanes) {
            const gp_Dir pull = refDirection ? pullDirection : plane.Axis().Direction();
            TopoShape attempt({}, getDocument()->getStringHasher());
            try {
                attempt.makeElementDraft(splitShape, draftFaces, pull, angle, plane, reversed);
            }
            catch (Standard_Failure& e) {
                error = e.GetMessageString();
                if (error.empty()) {
                    error = e.DynamicType()->Name();
                }
                continue;
            }
            if (attempt.isNull()) {
                continue;
            }

            shape = attempt;
            neutralPlane = plane;
            pullDirection = pull;
            break;
        }

        computeProps = {pullDirection, neutralPlane};

        if (shape.isNull()) {
            if (error.empty()) {
                error = "Failed to draft the selected faces";
            }
            else {
                error = "Failed to draft the selected faces: " + error;
            }
            return new App::DocumentObjectExecReturn(error);
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

Part::Feature* Draft::getBaseSolidObject() const
{
    if (hasSolidShape(BaseFeature.getValue())) {
        return static_cast<Part::Feature*>(BaseFeature.getValue());
    }

    // BaseFeature may point at a profile instead of the feature the draft is
    // applied to, so fall back to the closest solid feature before this one.
    Body* body = getFeatureBody();
    if (!body) {
        return nullptr;
    }

    Part::Feature* previous = nullptr;
    for (auto* feature : body->Group.getValues()) {
        if (feature == this) {
            break;
        }
        if (hasSolidShape(feature)) {
            previous = static_cast<Part::Feature*>(feature);
        }
    }

    return previous;
}

Part::TopoShape Draft::getProfileRegion(
    Part::Feature* baseObject,
    App::DocumentObject* profileObject,
    const std::vector<std::string>& subValues
) const
{
    if (!profileObject || !profileObject->isDerivedFrom<Part::Feature>()) {
        throw Base::TypeError("Profile must be a Part feature, for example a sketch");
    }
    if (!baseObject) {
        throw Base::RuntimeError("No base object for the profile");
    }

    // The profile in global coordinates.  A sub-element name (for example a
    // sketch's InternalFace) is resolved by Part::Feature::getTopoShape().
    const char* sub = (!subValues.empty() && !subValues.front().empty())
        ? subValues.front().c_str()
        : nullptr;
    auto options = Part::ShapeOption::NeedSubElement | Part::ShapeOption::ResolveLink
        | Part::ShapeOption::Transform;
    Part::TopoShape profileShape = Part::Feature::getTopoShape(profileObject, options, sub);
    if (!profileShape.hasSubShape(TopAbs_FACE)) {
        // not a (sub-)face, use the whole profile shape instead
        profileShape = Part::Feature::getTopoShape(
            profileObject,
            Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform
        );
    }
    if (profileShape.isNull()) {
        throw Base::ValueError("Profile shape is empty");
    }

    // A sketch exposes the regions it encloses as internal faces ("InternalFaceN");
    // they are the regions the user sees and can pick.  For wires that
    // Part::FaceMakerBullseye reads differently they are also the only correct
    // region, so prefer them over building faces from the wires.
    if (!sub) {
        std::vector<Part::TopoShape> internalFaces;
        for (int index = 1; index <= 64; ++index) {
            std::string name = "InternalFace" + std::to_string(index);
            Part::TopoShape internal;
            try {
                internal = Part::Feature::getTopoShape(profileObject, options, name.c_str());
            }
            catch (Base::Exception&) {
                break;
            }
            if (internal.isNull() || !internal.hasSubShape(TopAbs_FACE)) {
                break;
            }
            internalFaces.push_back(internal);
        }
        if (!internalFaces.empty()) {
            profileShape = Part::TopoShape({}, getDocument()->getStringHasher())
                               .makeElementCompound(internalFaces);
        }
    }

    // Into the local frame of the base object
    profileShape.transformShape(
        App::GeoFeature::getGlobalPlacement(baseObject).inverse().toMatrix(),
        false,
        true
    );

    // The profile has to enclose at least one closed region
    Part::TopoShape region({}, getDocument()->getStringHasher());
    if (profileShape.hasSubShape(TopAbs_FACE)) {
        region = profileShape;
    }
    else {
        region.makeElementFace(profileShape);
    }

    if (!region.hasSubShape(TopAbs_FACE)) {
        throw Base::ValueError("Profile does not enclose a region, use a closed sketch");
    }

    return region;
}

std::vector<Part::TopoShape> Draft::getFacesUnderProfile(
    const Part::TopoShape& baseShape,
    const Part::TopoShape& region,
    const App::DocumentObject* profileObject,
    const Part::Feature* baseObject
) const
{
    std::vector<Part::TopoShape> result;
    if (region.isNull()) {
        return result;
    }

    // A sketch is normally attached to the face it is drawn on.  Use that face:
    // a purely geometric search can pick a different coplanar face that only
    // partly overlaps the region.
    if (profileObject && profileObject->isDerivedFrom<Part::Part2DObject>()) {
        const auto* part2d = static_cast<const Part::Part2DObject*>(profileObject);
        for (const auto& [object, subs] : part2d->AttachmentSupport.getSubListValues()) {
            if (object != baseObject || subs.empty() || subs.front().empty()) {
                continue;
            }
            // the sub-shape of baseShape itself, so the OCC draft accepts it
            Part::TopoShape face = baseShape.getSubTopoShape(subs.front().c_str(), true);
            if (face.isNull() || face.shapeType() != TopAbs_FACE) {
                // the stored name may only resolve through the element map
                face = Part::Feature::getTopoShape(
                    object,
                    Part::ShapeOption::NeedSubElement | Part::ShapeOption::ResolveLink,
                    subs.front().c_str()
                );
            }
            if (face.isNull() || face.countSubShapes(TopAbs_FACE) != 1) {
                continue;
            }

            // only use it when the region really lies on that face
            auto regionFaces = region.getSubTopoShapes(TopAbs_FACE);
            gp_Pln facePlane;
            gp_Pln regionPlane;
            const bool havePlanes = !regionFaces.empty() && getPlane(face.getShape(), facePlane)
                && getPlane(regionFaces.front().getShape(), regionPlane);
            double area = -1.0;
            if (havePlanes) {
                const double dot = facePlane.Axis().Direction().Dot(regionPlane.Axis().Direction());
                if (std::abs(dot) > 1.0 - 1e-9) {
                    const gp_Dir normal = facePlane.Axis().Direction();
                    const double faceOffset = facePlane.Location().XYZ().Dot(normal.XYZ());
                    const double regionOffset = regionPlane.Location().XYZ().Dot(normal.XYZ());
                    gp_Trsf translation;
                    translation.SetTranslation(gp_Vec(normal.XYZ() * (faceOffset - regionOffset)));
                    TopoDS_Shape moved
                        = BRepBuilderAPI_Transform(region.getShape(), translation, true).Shape();
                    area = getIntersectionArea(face.getShape(), moved);
                }
            }
            if (area > Precision::Confusion()) {
                result.push_back(face);
                return result;
            }
        }
    }

    gp_Pln regionPlane;
    bool haveRegionPlane = false;
    for (const auto& regionFace : region.getSubTopoShapes(TopAbs_FACE)) {
        if (getPlane(regionFace.getShape(), regionPlane)) {
            haveRegionPlane = true;
            break;
        }
    }
    if (!haveRegionPlane) {
        return result;
    }

    const gp_Dir regionNormal = regionPlane.Axis().Direction();
    const double regionOffset = regionPlane.Location().XYZ().Dot(regionNormal.XYZ());

    // Among the faces parallel to the region, the ones closest to it that really
    // overlap it are below the profile.
    double closest = std::numeric_limits<double>::max();
    std::vector<std::pair<double, Part::TopoShape>> candidates;

    for (const auto& face : baseShape.getSubTopoShapes(TopAbs_FACE)) {
        gp_Pln facePlane;
        if (!getPlane(face.getShape(), facePlane)) {
            continue;
        }
        if (std::abs(facePlane.Axis().Direction().Dot(regionNormal)) < 1.0 - 1e-9) {
            continue;
        }

        const double faceOffset = facePlane.Location().XYZ().Dot(regionNormal.XYZ());
        const double distance = std::abs(faceOffset - regionOffset);

        gp_Trsf translation;
        translation.SetTranslation(gp_Vec(regionNormal.XYZ() * (faceOffset - regionOffset)));
        TopoDS_Shape moved
            = BRepBuilderAPI_Transform(region.getShape(), translation, true).Shape();

        if (getIntersectionArea(face.getShape(), moved) <= Precision::Confusion()) {
            continue;
        }

        closest = std::min(closest, distance);
        candidates.emplace_back(distance, face);
    }

    for (const auto& [distance, face] : candidates) {
        if (distance <= closest + Precision::Confusion()) {
            result.push_back(face);
        }
    }

    return result;
}

Part::TopoShape Draft::splitBaseFaces(
    const Part::TopoShape& baseShape,
    const Part::TopoShape& givenRegion,
    const std::vector<Part::TopoShape>& faces,
    Part::Feature* givenBaseObject,
    std::vector<Part::TopoShape>& draftFaces
) const
{
    Part::Feature* baseObject = givenBaseObject;
    if (!baseObject) {
        baseObject = getBaseObject();
    }

    Part::TopoShape region = givenRegion;
    if (region.isNull()) {
        return baseShape;
    }

    auto regionFaces = region.getSubTopoShapes(TopAbs_FACE);
    if (regionFaces.empty()) {
        throw Base::ValueError("Profile does not enclose a region, use a closed sketch");
    }

    // Move the profile's region onto the plane of every base face and use the
    // moved regions as splitting tools.
    TopTools_ListOfShape tools;
    std::vector<std::vector<TopoDS_Shape>> regionsPerFace;
    std::vector<bool> wholeFace;
    regionsPerFace.reserve(faces.size());
    wholeFace.reserve(faces.size());

    for (const auto& face : faces) {
        gp_Pln facePlane;
        if (!getPlane(face.getShape(), facePlane)) {
            throw Base::TypeError("Base face must be planar to be split by a profile");
        }

        const gp_Dir faceNormal = facePlane.Axis().Direction();
        const double faceOffset = facePlane.Location().XYZ().Dot(faceNormal.XYZ());
        const double faceArea = getArea(face.getShape());

        std::vector<TopoDS_Shape> faceRegions;
        double covered = 0.0;
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
            covered += getIntersectionArea(face.getShape(), moved);
        }

        // If the region covers the face completely there is nothing to split:
        // keep the face as it is.  Drafting several coplanar pieces instead makes
        // the OCC draft fail.
        if (!faceRegions.empty() && covered >= faceArea - 1e-6 * faceArea) {
            wholeFace.push_back(true);
            regionsPerFace.push_back(std::move(faceRegions));
            continue;
        }

        for (const auto& moved : faceRegions) {
            tools.Append(moved);
        }
        wholeFace.push_back(false);
        regionsPerFace.push_back(std::move(faceRegions));
    }

    if (tools.IsEmpty()) {
        // every face is covered completely, or the region is not parallel
        bool anyParallel = false;
        for (const auto& faceRegions : regionsPerFace) {
            anyParallel = anyParallel || !faceRegions.empty();
        }
        if (!anyParallel) {
            throw Base::ValueError("Profile is not parallel to any of the selected faces");
        }

        // nothing to split: the region covers the selected faces completely
        draftFaces = faces;
        return baseShape;
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
        if (wholeFace[i]) {
            // the region covers the whole face, the split did not change it
            bool kept = false;
            for (TopTools_ListIteratorOfListOfShape it(splitter.Modified(faces[i].getShape()));
                 it.More();
                 it.Next()) {
                if (it.Value().ShapeType() == TopAbs_FACE) {
                    insideFaces.emplace_back(it.Value());
                    kept = true;
                }
            }
            if (!kept) {
                insideFaces.push_back(faces[i]);
            }
            continue;
        }
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

/** The neutral plane of the draft.
 *
 *  It is either taken from the NeutralPlane property, or guessed from the face
 *  that is actually drafted, which after a split is a sub-face of the selected
 *  face and not necessarily crossed by a plane guessed from the whole face.
 */
gp_Pln Draft::getExplicitNeutralPlane(
    App::DocumentObject* refDirection,
    const gp_Dir& pullDirection,
    const gp_Trsf& invBaseTrsf
) const
{
    gp_Pln neutralPlane;
    App::DocumentObject* refPlane = NeutralPlane.getValue();
    if (!refPlane) {
        throw Base::RuntimeError("No neutral plane specified and none can be guessed");
    }

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

        neutralPlane.Transform(invBaseTrsf);
    return neutralPlane;
}

std::vector<gp_Pln> Draft::getNeutralPlaneCandidates(
    const TopoDS_Shape& draftFace,
    const std::vector<TopoShape>& originalFaces
) const
{
    std::vector<gp_Pln> candidates;

        // Try to guess a neutral plane from the face that is drafted
        TopoDS_Shape face = draftFace;
        TopTools_IndexedMapOfShape mapOfEdges;
        TopExp::MapShapes(face, TopAbs_EDGE, mapOfEdges);

        // Edges created by the split (not part of the face(s) already there) are
        // the best hinge: the other part of the split face then stays in place.
        TopTools_IndexedMapOfShape originalEdges;
        for (const auto& original : originalFaces) {
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(original.getShape(), TopAbs_EDGE, map);
            for (int i = 1; i <= map.Extent(); ++i) {
                originalEdges.Add(map(i));
            }
        }

        std::vector<TopoDS_Edge> edges;
        for (int i = 1; i <= mapOfEdges.Extent(); ++i) {
            if (originalEdges.FindIndex(mapOfEdges(i)) <= 0) {
                edges.push_back(TopoDS::Edge(mapOfEdges(i)));
            }
        }
        for (int i = 1; i <= mapOfEdges.Extent(); ++i) {
            edges.push_back(TopoDS::Edge(mapOfEdges(i)));
        }

        for (const auto& edge : edges) {
            if (candidates.size() >= 12) {
                break;
            }
            // Note: What happens if the edge is the degenerated edge of a cone?
            // But in that case the draft is not possible anyway!
            BRepAdaptor_Curve c(edge);
            gp_Pnt p1 = c.Value(c.FirstParameter());
            gp_Pnt p2 = c.Value(c.LastParameter());

            if (c.IsClosed()) {
                // Edge is a circle or a circular arc (other types are not allowed for drafting)
                if (c.GetType() == GeomAbs_Circle) {
                    candidates.push_back(gp_Pln(p1, c.Circle().Axis().Direction()));
                    continue;
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
                candidates.push_back(gp_Pln(pm, line->Lin().Direction()));
            }
        }

    return candidates;
}
