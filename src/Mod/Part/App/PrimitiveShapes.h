// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2007 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/Part/PartGlobal.h>

namespace Part
{

/** Shared geometry construction of the Part and PartDesign primitives.
 *
 * The helpers only build the shape. Validation of the input values and the
 * module specific error handling stay in the calling feature.
 */
namespace PrimitiveShapes
{

PartExport TopoDS_Shape makeSphere(double radius, double angle1, double angle2, double angle3);

PartExport TopoDS_Shape makeEllipsoid(
    double radius1,
    double radius2,
    double radius3,
    double angle1,
    double angle2,
    double angle3
);

PartExport TopoDS_Shape makeCylinder(
    double radius,
    double height,
    double angle,
    double firstAngle,
    double secondAngle
);

PartExport TopoDS_Shape makeCone(double radius1, double radius2, double height, double angle);

PartExport TopoDS_Shape makeTorus(
    double radius1,
    double radius2,
    double angle1,
    double angle2,
    double angle3
);

PartExport TopoDS_Shape makePrism(
    long polygon,
    double circumradius,
    double height,
    double firstAngle,
    double secondAngle
);

PartExport TopoDS_Shape makeWedge(
    double xmin,
    double ymin,
    double zmin,
    double z2min,
    double x2min,
    double xmax,
    double ymax,
    double zmax,
    double z2max,
    double x2max
);

PartExport TopoDS_Shape extrudePrism(
    const TopoDS_Face& face,
    double height,
    double firstAngle,
    double secondAngle
);

}  // namespace PrimitiveShapes
}  // namespace Part
