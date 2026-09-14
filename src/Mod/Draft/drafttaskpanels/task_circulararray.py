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
"""Provides the task panel code for the Draft CircularArray tool."""

## @package task_circulararray
# \ingroup drafttaskpanels
# \brief Provides the task panel code for the Draft CircularArray tool.

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


class TaskPanelCircularArray(base_draftarraytaskpanel._TaskPanelArrayBase):
    """TaskPanel code for the CircularArray command.

    See `_TaskPanelArrayBase` for the shared widget handling.
    """

    def __init__(self):

        self.form = Gui.PySideUic.loadUi(":/ui/TaskPanel_CircularArray.ui")
        self.form.setWindowTitle(translate("draft", "Circular Array"))
        self.form.setWindowIcon(QtGui.QIcon(":/icons/Draft_CircularArray.svg"))

        # -------------------------------------------------------------------
        # Default values for the internal function, and for the task panel interface
        self.center = App.Vector()
        # TODO: the axis is currently fixed, it should be editable
        # or selectable from the task panel
        self.axis = WorkingPlane.get_working_plane(update=False).axis
        self.r_distance = 100
        self.tan_distance = 50
        self.number = 3
        self.symmetry = 1
        self.fuse = params.get_param("Draft_array_fuse")
        self.use_link = params.get_param("Draft_array_Link")

        self.form.spinbox_r_distance.setProperty("rawValue", self.r_distance)
        self.form.spinbox_tan_distance.setProperty("rawValue", self.tan_distance)
        self.form.spinbox_number.setValue(self.number)
        self.form.spinbox_symmetry.setValue(self.symmetry)
        self.form.checkbox_fuse.setChecked(self.fuse)
        self.form.checkbox_link.setChecked(self.use_link)
        # -------------------------------------------------------------------

        self._initCommon()

    def accept(self):
        """Execute when clicking the OK button or Enter key."""
        self.selection = Gui.Selection.getSelection()

        self.r_distance, self.tan_distance = self.get_distances()

        self.number, self.symmetry = self.get_number_symmetry()

        self.axis = self.get_axis()
        self.center = self.get_center()

        self.valid_input = self.validate_input(
            self.selection,
            self.r_distance,
            self.tan_distance,
            self.number,
            self.symmetry,
            self.axis,
            self.center,
        )
        if self.valid_input:
            self.create_object()
            # The internal function already displays messages
            self.finish()

    def validate_input(self, selection, r_distance, tan_distance, number, symmetry, axis, center):
        """Check that the input is valid.

        Some values may not need to be checked because
        the interface may not allow one to input wrong data.
        """
        if not selection:
            _err(translate("draft", "At least 1 element must be selected"))
            return False

        if number < 2:
            _err(translate("draft", "Number of layers must be at least 2"))
            return False

        # TODO: this should handle multiple objects.
        # Each of the elements of the selection should be tested.
        obj = selection[0]
        if obj.isDerivedFrom("App::FeaturePython"):
            _err(translate("draft", "Selection is not suitable for array"))
            _err(translate("draft", "Object:") + " {}".format(selection[0].Label))
            return False

        if r_distance == 0:
            _wrn(
                translate("draft", "Radial distance is zero. Resulting array may not look correct.")
            )
        elif r_distance < 0:
            _wrn(translate("draft", "Radial distance is negative. It is made positive to proceed."))
            self.r_distance = abs(r_distance)

        if tan_distance == 0:
            _err(translate("draft", "Tangential distance cannot be 0"))
            return False
        elif tan_distance < 0:
            _wrn(
                translate(
                    "draft", "Tangential distance is negative. It is made positive to proceed."
                )
            )
            self.tan_distance = abs(tan_distance)

        # The other arguments are not tested but they should be present.
        if symmetry and axis and center:
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
        # obj = Draft.make_circular_array(sel_obj,
        #                                 self.r_distance, self.tan_distance,
        #                                 self.number, self.symmetry,
        #                                 self.axis, self.center,
        #                                 self.use_link)

        # Instead, we build the commands to execute through the caller
        # of this class, the GuiCommand.
        # This is needed to schedule geometry manipulation
        # that would crash Coin3D if done in the event callback.
        _cmd = "Draft.make_circular_array"
        _cmd += "("
        _cmd += "App.ActiveDocument." + sel_obj.Name + ", "
        _cmd += "r_distance=" + str(self.r_distance) + ", "
        _cmd += "tan_distance=" + str(self.tan_distance) + ", "
        _cmd += "number=" + str(self.number) + ", "
        _cmd += "symmetry=" + str(self.symmetry) + ", "
        _cmd += "axis=" + DraftVecUtils.toString(self.axis) + ", "
        _cmd += "center=" + DraftVecUtils.toString(self.center) + ", "
        _cmd += "use_link=" + str(self.use_link)
        _cmd += ")"

        self._commitArray(_cmd, "Create Circular Array")

    def get_distances(self):
        """Get the distance parameters from the widgets."""
        r_d_str = self.form.spinbox_r_distance.text()
        tan_d_str = self.form.spinbox_tan_distance.text()
        return _quantity(r_d_str), _quantity(tan_d_str)

    def get_number_symmetry(self):
        """Get the number and symmetry parameters from the widgets."""
        number = self.form.spinbox_number.value()
        symmetry = self.form.spinbox_symmetry.value()
        return number, symmetry

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
        _msg(translate("draft", "Radial distance:") + " {}".format(self.r_distance))
        _msg(translate("draft", "Tangential distance:") + " {}".format(self.tan_distance))
        _msg(translate("draft", "Number of concentric circles:") + " {}".format(self.number))
        _msg(translate("draft", "Symmetry parameter:") + " {}".format(self.symmetry))
        _msg(
            translate("draft", "Center of rotation:")
            + " ({0}, {1}, {2})".format(self.center.x, self.center.y, self.center.z)
        )
        self.print_fuse_state(self.fuse)
        self.print_link_state(self.use_link)


## @}
