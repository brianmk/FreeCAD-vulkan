# ***************************************************************************
# *   Copyright (c) 2017 Markus Hovorka <m.hovorka@live.de>                 *
# *   Copyright (c) 2020 Bernd Hahnebach <bernd@bimstatik.org>              *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   This program is distributed in the hope that it will be useful,       *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with this program; if not, write to the Free Software   *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************

__title__ = "FreeCAD FEM flow velocity base document object"
__author__ = "Markus Hovorka, Bernd Hahnebach"
__url__ = "https://www.freecad.org"

## @package base_femflowvelocity
#  \ingroup FEM
#  \brief base object for flow velocity constraints

from . import base_fempythonobject


class BaseFemFlowVelocity(base_fempythonobject.BaseFemPythonObject):
    """Base object for the flow velocity constraints.

    Adds the per-axis velocity, formula and unspecified properties shared by
    ``Fem::ConstraintFlowVelocity`` and ``Fem::ConstraintInitialFlowVelocity``.
    """

    def __init__(self, obj):
        super().__init__(obj)
        for axis in ("X", "Y", "Z"):
            obj.addProperty(
                "App::PropertyVelocity",
                f"Velocity{axis}",
                "Parameter",
                f"Velocity in {axis}-direction",
            )
            obj.setPropertyStatus(f"Velocity{axis}", "LockDynamic")
            obj.addProperty(
                "App::PropertyString",
                f"Velocity{axis}Formula",
                "Parameter",
                f"Velocity formula in {axis}-direction",
            )
            obj.setPropertyStatus(f"Velocity{axis}Formula", "LockDynamic")
            obj.addProperty(
                "App::PropertyBool",
                f"Velocity{axis}Unspecified",
                "Parameter",
                f"Use velocity in {axis}-direction",
            )
            obj.setPropertyStatus(f"Velocity{axis}Unspecified", "LockDynamic")
            setattr(obj, f"Velocity{axis}Unspecified", True)
            obj.addProperty(
                "App::PropertyBool",
                f"Velocity{axis}HasFormula",
                "Parameter",
                f"Use formula for velocity in {axis}-direction",
            )
            obj.setPropertyStatus(f"Velocity{axis}HasFormula", "LockDynamic")
