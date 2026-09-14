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

#include "PrimitiveShapes.h"

#include <cmath>

#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrim_Cylinder.hxx>
#include <BRepPrim_Wedge.hxx>
#include <Precision.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <Base/Matrix.h>
#include <Base/Tools.h>
#include <Base/Vector3D.h>
#include <Mod/Part/App/TopoShape.h>


using namespace Part;

TopoDS_Shape PrimitiveShapes::makeSphere(double radius, double angle1, double angle2, double angle3)
{
    BRepPrimAPI_MakeSphere mkSphere(
        radius,
        Base::toRadians<double>(angle1),
        Base::toRadians<double>(angle2),
        Base::toRadians<double>(angle3)
    );
    return mkSphere.Shape();
}

TopoDS_Shape PrimitiveShapes::makeEllipsoid(
    double radius1,
    double radius2,
    double radius3,
    double angle1,
    double angle2,
    double angle3
)
{
    gp_Pnt pnt(0.0, 0.0, 0.0);
    gp_Dir dir(0.0, 0.0, 1.0);
    gp_Ax2 ax2(pnt, dir);
    BRepPrimAPI_MakeSphere mkSphere(
        ax2,
        radius2,
        Base::toRadians<double>(angle1),
        Base::toRadians<double>(angle2),
        Base::toRadians<double>(angle3)
    );
    Standard_Real scaleX = 1.0;
    Standard_Real scaleZ = radius1 / radius2;
    // issue #1798: A third radius has been introduced. To be backward
    // compatible if radius3 is 0.0 (default) it's handled to be the same
    // as radius2
    Standard_Real scaleY = 1.0;
    if (radius3 >= Precision::Confusion()) {
        scaleY = radius3 / radius2;
    }
    gp_GTrsf mat;
    mat.SetValue(1, 1, scaleX);
    mat.SetValue(2, 1, 0.0);
    mat.SetValue(3, 1, 0.0);
    mat.SetValue(1, 2, 0.0);
    mat.SetValue(2, 2, scaleY);
    mat.SetValue(3, 2, 0.0);
    mat.SetValue(1, 3, 0.0);
    mat.SetValue(2, 3, 0.0);
    mat.SetValue(3, 3, scaleZ);
    BRepBuilderAPI_GTransform mkTrsf(mkSphere.Shape(), mat);
    return mkTrsf.Shape();
}

TopoDS_Shape PrimitiveShapes::extrudePrism(
    const TopoDS_Face& face,
    double height,
    double firstAngle,
    double secondAngle
)
{
    // the direction vector for the prism is the height for z and the given angle
    BRepPrimAPI_MakePrism mkPrism(
        face,
        gp_Vec(
            height * tan(Base::toRadians<double>(firstAngle)),
            height * tan(Base::toRadians<double>(secondAngle)),
            height
        )
    );
    return mkPrism.Shape();
}

TopoDS_Shape PrimitiveShapes::makeCylinder(
    double radius,
    double height,
    double angle,
    double firstAngle,
    double secondAngle
)
{
    BRepPrimAPI_MakeCylinder mkCylr(radius, height, Base::toRadians<double>(angle));
    BRepPrim_Cylinder prim = mkCylr.Cylinder();
    return extrudePrism(prim.BottomFace(), height, firstAngle, secondAngle);
}

TopoDS_Shape PrimitiveShapes::makeCone(double radius1, double radius2, double height, double angle)
{
    if (std::abs(radius1 - radius2) < Precision::Confusion()) {
        // Build a cylinder
        BRepPrimAPI_MakeCylinder mkCylr(radius1, height, Base::toRadians<double>(angle));
        return mkCylr.Shape();
    }
    // Build a cone
    BRepPrimAPI_MakeCone mkCone(radius1, radius2, height, Base::toRadians<double>(angle));
    return mkCone.Shape();
}

TopoDS_Shape PrimitiveShapes::makeTorus(
    double radius1,
    double radius2,
    double angle1,
    double angle2,
    double angle3
)
{
    TopoShape shape;
    return shape.makeTorus(radius1, radius2, angle1, angle2, angle3);
}

TopoDS_Shape PrimitiveShapes::makePrism(
    long polygon,
    double circumradius,
    double height,
    double firstAngle,
    double secondAngle
)
{
    Base::Matrix4D mat;
    mat.rotZ(Base::toRadians(360.0 / polygon));

    // create polygon
    BRepBuilderAPI_MakePolygon mkPoly;
    Base::Vector3d v(circumradius, 0, 0);
    for (long i = 0; i < polygon; i++) {
        mkPoly.Add(gp_Pnt(v.x, v.y, v.z));
        v = mat * v;
    }
    mkPoly.Add(gp_Pnt(v.x, v.y, v.z));
    BRepBuilderAPI_MakeFace mkFace(mkPoly.Wire());
    return extrudePrism(mkFace.Face(), height, firstAngle, secondAngle);
}

TopoDS_Shape PrimitiveShapes::makeWedge(
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
)
{
    gp_Pnt pnt(0.0, 0.0, 0.0);
    gp_Dir dir(0.0, 0.0, 1.0);
    BRepPrim_Wedge
        mkWedge(gp_Ax2(pnt, dir), xmin, ymin, zmin, z2min, x2min, xmax, ymax, zmax, z2max, x2max);
    BRepBuilderAPI_MakeSolid mkSolid;
    mkSolid.Add(mkWedge.Shell());
    return mkSolid.Solid();
}
