# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   (c) 2019 Eliud Cabrera Castillo <e.cabrera-castillo@tum.de>           *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************
"""Provides GUI tools to create polar Array objects."""

## @package gui_polararray
# \ingroup draftguitools
# \brief Provides GUI tools to create polar Array objects.

## \addtogroup draftguitools
# @{
from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCADGui as Gui
from draftguitools import gui_arraybase
from drafttaskpanels import task_polararray


class PolarArray(gui_arraybase._ArrayGuiCommandBase):
    """Gui command for the PolarArray tool."""

    _name = "PolarArray"
    _panelClass = task_polararray.TaskPanelPolarArray

    def GetResources(self):
        """Set icon, menu and tooltip."""
        return {
            "Pixmap": "Draft_PolarArray",
            "MenuText": QT_TRANSLATE_NOOP("Draft_PolarArray", "Polar Array"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Draft_PolarArray", "Creates copies of the selected object in a polar pattern"
            ),
        }


Gui.addCommand("Draft_PolarArray", PolarArray())

## @}
