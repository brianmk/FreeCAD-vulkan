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


#pragma once

#include <gp_Pln.hxx>
#include <gp_Dir.hxx>

#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <App/PropertyLinks.h>
#include "FeatureDressUp.h"

namespace PartDesign
{
struct PartDesignExport DraftComputeProps
{
    gp_Dir pullDirection;
    gp_Pln neutralPlane;
};

class PartDesignExport Draft: public DressUp
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::Draft);

public:
    Draft();

    App::PropertyAngle Angle;
    App::PropertyLinkSub NeutralPlane;
    App::PropertyLinkSub PullDirection;
    /** Optional sketch (or any planar profile) that splits the faces listed in
     *  Base. Only the part of a base face that lies inside the profile's
     *  enclosed region is drafted.
     */
    App::PropertyLinkSub Profile;
    App::PropertyBool Reversed;

    /** @name methods override feature */
    //@{
    /// recalculate the feature
    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    /// returns the type name of the view provider
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderDraft";
    }
    //@}

    /**
     * @brief getLastComputedProps: Returns the Pull Direction and Neutral Plane
     * computed during the last call of the execute method.
     * Note: The returned values might be in the default initialized state if
     * they were not computed or computation failed
     */
    DraftComputeProps getLastComputedProps() const
    {
        return computeProps;
    }

private:
    void handleChangedPropertyType(
        Base::XMLReader& reader,
        const char* TypeName,
        App::Property* prop
    ) override;
    static const App::PropertyAngle::Constraints floatAngle;

    /** Split the base faces with the Profile and return the resulting shape.
     *
     *  @param baseShape: the untransformed base shape the faces belong to.
     *  @param faces: the faces selected through Base, in baseShape.
     *  @param draftFaces: replaced with the sub-faces of the split shape that
     *         lie inside the profile's region, i.e. the faces to draft.
     *
     *  @return the split base shape (a copy of baseShape if there is no Profile)
     */
    Part::TopoShape splitBaseFaces(
        const Part::TopoShape& baseShape,
        const std::vector<Part::TopoShape>& faces,
        std::vector<Part::TopoShape>& draftFaces
    ) const;

    DraftComputeProps computeProps;
};

}  // namespace PartDesign
