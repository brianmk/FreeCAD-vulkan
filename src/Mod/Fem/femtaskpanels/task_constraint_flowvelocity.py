# ***************************************************************************
# *   Copyright (c) 2017 Markus Hovorka <m.hovorka@live.de>                 *
# *   Copyright (c) 2023 Uwe Stöhr <uwestoehr@lyx.org>                      *
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

__title__ = "FreeCAD FEM constraint flow velocity task panel for the document object"
__author__ = "Markus Hovorka, Bernd Hahnebach, Uwe Stöhr"
__url__ = "https://www.freecad.org"

## @package task_constraint_flowvelocity
#  \ingroup FEM
#  \brief task panel for constraint flow velocity object

from . import base_femflowvelocitytaskpanel


class _TaskPanel(base_femflowvelocitytaskpanel.BaseFlowVelocityTaskPanel):

    _uiFile = "FlowVelocity.ui"
    _selectionTypes = ["Solid", "Face", "Edge", "Vertex"]

    def _initParamWidget(self):
        super()._initParamWidget()
        self._paramWidget.normalBox.setChecked(self.obj.NormalToBoundary)

    def _applyWidgetChanges(self):
        super()._applyWidgetChanges()
        self.obj.NormalToBoundary = self._paramWidget.normalBox.isChecked()
