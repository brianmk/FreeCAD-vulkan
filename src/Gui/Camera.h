// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2023 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/

#pragma once

#include <Inventor/SbRotation.h>
#include <Inventor/SbVec3f.h>
#include <Base/Rotation.h>
#include <FCGlobal.h>

#include <string>

namespace Gui
{

class GuiExport Camera
{
public:
    enum Orientation
    {
        Top,
        Bottom,
        Front,
        Rear,
        Right,
        Left,
        Isometric,
        Dimetric,
        Trimetric,
    };

    static SbRotation top();
    static SbRotation bottom();
    static SbRotation front();
    static SbRotation rear();
    static SbRotation right();
    static SbRotation left();
    static SbRotation isometric();
    static SbRotation dimetric();
    static SbRotation trimetric();

    /// Rotation that maps the canonical basis onto the given orthonormal axes
    /// (the axes become the columns of the rotation matrix).
    static SbRotation rotationFromBasis(const SbVec3f& x, const SbVec3f& y, const SbVec3f& z);
    /// Camera orientation that displays the face whose local axes are (x, z)
    /// (y is derived as x cross -z), optionally rolled in-plane by rotZ.
    static SbRotation faceOrientation(SbVec3f x, SbVec3f z, float rotZ = 0.0f);
    /// The camera orientation turned by the smallest angle that brings its
    /// +Z axis onto targetZ. targetZ must be a unit vector.
    static SbRotation alignZAxis(const SbRotation& cameraOrientation, const SbVec3f& targetZ);

    static SbRotation rotation(Orientation view);
    /// Return a named orientation, or the fallback orientation when the name is unknown.
    static SbRotation rotation(const std::string& view, Orientation fallback = Top);
    /// Return the configured new-document orientation, or fallbackView when no preference is set.
    static SbRotation defaultOrientation(const char* fallbackView = "Trimetric");
    static bool rotationsMatch(
        const SbRotation& lhs,
        const SbRotation& rhs,
        float squaredTolerance = 1e-6F
    );
    static Base::Rotation convert(Orientation view);
    static Base::Rotation convert(const SbRotation&);
    static SbRotation convert(const Base::Rotation&);
};

}  // namespace Gui
