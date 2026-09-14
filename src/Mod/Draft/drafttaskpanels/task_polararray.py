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
"""Provides the task panel code for the Draft PolarArray tool."""

## @package task_polararray
# \ingroup drafttaskpanels
# \brief Provides the task panel code for the Draft PolarArray tool.

## \addtogroup drafttaskpanels
# @{
import PySide.QtGui as QtGui

import FreeCAD as App
import FreeCADGui as Gui
import WorkingPlane
import Draft_rc  # include resources, icons, ui files
import DraftVecUtils
from draftutils import params
from draftutils.messages import _err, _msg, _wrn
from draftutils.translate import translate
from drafttaskpanels import base_draftarraytaskpanel
from drafttaskpanels.base_draftarraytaskpanel import _quantity

# The module is used to prevent complaints from code checkers (flake8)
bool(Draft_rc.__name__)


class TaskPanelPolarArray(base_draftarraytaskpanel._TaskPanelArrayBase):
    """TaskPanel code for the PolarArray command.

    See `_TaskPanelArrayBase` for the shared widget handling.
    """

    def __init__(self):

        self.form = Gui.PySideUic.loadUi(":/ui/TaskPanel_PolarArray.ui")
        self.form.setWindowTitle(translate("draft", "Polar Array"))
        self.form.setWindowIcon(QtGui.QIcon(":/icons/Draft_PolarArray.svg"))

        # -------------------------------------------------------------------
        # Default values for the internal function, and for the task panel interface
        self.center = App.Vector()
        # TODO: the axis is currently fixed, it should be editable
        # or selectable from the task panel
        self.axis = WorkingPlane.get_working_plane(update=False).axis
        self.angle = 360
        self.number = 5
        self.fuse = params.get_param("Draft_array_fuse")
        self.use_link = params.get_param("Draft_array_Link")

        self.form.spinbox_angle.setProperty("rawValue", self.angle)
        self.form.spinbox_number.setValue(self.number)
        self.form.checkbox_fuse.setChecked(self.fuse)
        self.form.checkbox_link.setChecked(self.use_link)
        # -------------------------------------------------------------------

        self._initCommon()

    def accept(self):
        """Execute when clicking the OK button or Enter key."""
        self.selection = Gui.Selection.getSelection()

        self.number, self.angle = self.get_number_angle()

        self.axis = self.get_axis()
        self.center = self.get_center()

        self.valid_input = self.validate_input(
            self.selection, self.number, self.angle, self.axis, self.center
        )
        if self.valid_input:
            self.create_object()
            # The internal function already displays messages
            # self.print_messages()
            self.finish()

    def validate_input(self, selection, number, angle, axis, center):
        """Check that the input is valid.

        Some values may not need to be checked because
        the interface may not allow one to input wrong data.
        """
        if not selection:
            _err(translate("draft", "At least 1 element must be selected"))
            return False

        # TODO: this should handle multiple objects.
        # Each of the elements of the selection should be tested.
        obj = selection[0]
        if obj.isDerivedFrom("App::FeaturePython"):
            _err(translate("draft", "Selection is not suitable for array"))
            _err(translate("draft", "Object:") + " {}".format(selection[0].Label))
            return False

        if number < 2:
            _err(translate("draft", "Number of elements must be at least 2"))
            return False

        if angle > 360:
            _wrn(
                translate(
                    "draft", "The angle is above 360 degrees. It is set to this value to proceed."
                )
            )
            self.angle = 360
        elif angle < -360:
            _wrn(
                translate(
                    "draft", "The angle is below -360 degrees. It is set to this value to proceed."
                )
            )
            self.angle = -360

        # The other arguments are not tested but they should be present.
        if axis and center:
            pass

        self._applyCheckboxState()
        return True

    def create_object(self):
        """Create the new object.

        At this stage we already tested that the input is correct
        so the necessary attributes are already set.
        Then we proceed with the internal function to create the new object.
        """
        sel_obj = self._selectedObject()

        # This creates the object immediately
        # obj = Draft.make_polar_array(sel_obj,
        #                              self.number, self.angle, self.center,
        #                              self.axis, self.use_link)

        # Instead, we build the commands to execute through the caller
        # of this class, the GuiCommand.
        # This is needed to schedule geometry manipulation
        # that would crash Coin3D if done in the event callback.
        _cmd = "Draft.make_polar_array"
        _cmd += "("
        _cmd += "App.ActiveDocument." + sel_obj.Name + ", "
        _cmd += "number=" + str(self.number) + ", "
        _cmd += "angle=" + str(self.angle) + ", "
        _cmd += "center=" + DraftVecUtils.toString(self.center) + ", "
        _cmd += "axis=" + DraftVecUtils.toString(self.axis) + ", "
        _cmd += "use_link=" + str(self.use_link)
        _cmd += ")"

        self._commitArray(_cmd, "Create Polar Array")

    def get_number_angle(self):
        """Get the number and angle parameters from the widgets."""
        number = self.form.spinbox_number.value()

        angle_str = self.form.spinbox_angle.text()
        angle = _quantity(angle_str)
        return number, angle

    def print_messages(self):
        """Print messages about the operation."""
        if len(self.selection) == 1:
            sel_obj = self.selection[0]
        else:
            # TODO: this should handle multiple objects.
            # For example, it could take the shapes of all objects,
            # make a compound and then use it as input for the array function.
            sel_obj = self.selection[0]
        _msg(translate("draft", "Object:") + " {}".format(sel_obj.Label))
        _msg(translate("draft", "Number of elements:") + " {}".format(self.number))
        _msg(translate("draft", "Polar angle:") + " {}".format(self.angle))
        _msg(
            translate("draft", "Center of rotation:")
            + " ({0}, {1}, {2})".format(self.center.x, self.center.y, self.center.z)
        )
        self.print_fuse_state(self.fuse)
        self.print_link_state(self.use_link)


## @}
