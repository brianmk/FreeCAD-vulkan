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
"""Provides the shared task panel code for the Draft array tools."""

## @package base_draftarraytaskpanel
# \ingroup drafttaskpanels
# \brief Provides the shared task panel code for the Draft array tools.

## \addtogroup drafttaskpanels
# @{
from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCAD as App
import FreeCADGui as Gui
from FreeCAD import Units as U
from draftguitools.gui_field_locks import InputFieldLockGroup
from draftutils import params
from draftutils.messages import _msg
from draftutils.translate import translate


def _quantity(st):
    return U.Quantity(st).Value


class _TaskPanelArrayBase:
    """Shared behaviour for the Draft array task panels.

    The names of the widgets are defined in the `.ui` file.
    This `.ui` file `must` be loaded into an attribute
    called `self.form` so that it is loaded into the task panel correctly.

    In the subclasses all widgets are automatically created
    as `self.form.<widget_name>`.

    The `.ui` file may use special FreeCAD widgets such as
    `Gui::InputField` (based on `QLineEdit`) and
    `Gui::QuantitySpinBox` (based on `QAbstractSpinBox`).
    See the Doxygen documentation of the corresponding files in `src/Gui/`,
    for example, `InputField.h` and `QuantitySpinBox.h`.

    Subclasses build ``self.form`` from their own ``.ui`` file, set the
    array-specific default values and widgets, then call :meth:`_initCommon`.

    Attributes
    ----------
    source_command: gui_base.GuiCommandBase
        This attribute holds a reference to the calling class
        of this task panel.
        This parent class, which is derived from `gui_base.GuiCommandBase`,
        is responsible for calling this task panel, for installing
        certain callbacks, and for removing them.

        It also delays the execution of the internal creation commands
        by using the `draftutils.todo.ToDo` class.

    See Also
    --------
    * https://forum.freecad.org/viewtopic.php?f=10&t=40007
    * https://forum.freecad.org/viewtopic.php?t=5374#p43038
    """

    def _initCommon(self):
        """Initialise the state and widgets common to every array panel."""
        self.locks = InputFieldLockGroup()
        self.locks.add_field("x", self.form.input_c_x)
        self.locks.add_field("y", self.form.input_c_y)
        self.locks.add_field("z", self.form.input_c_z)

        self.form.input_c_x.setProperty("rawValue", self.center.x)
        self.form.input_c_y.setProperty("rawValue", self.center.y)
        self.form.input_c_z.setProperty("rawValue", self.center.z)

        # Some objects need to be selected before we can execute the function.
        self.selection = None

        # This is used to test the input of the internal function.
        # It should be changed to True before we can execute the function.
        self.valid_input = False

        self.set_widget_callbacks()

        self.tr_true = QT_TRANSLATE_NOOP("Draft", "True")
        self.tr_false = QT_TRANSLATE_NOOP("Draft", "False")

        # The mask is not used at the moment, but could be used in the future
        # by a callback to restrict the coordinates of the pointer.
        self.mask = ""

    def _selectedObject(self):
        """Return the object to array.

        TODO: this should handle multiple objects.
        For example, it could take the shapes of all objects,
        make a compound and then use it as input for the array function.
        """
        return self.selection[0]

    def _applyCheckboxState(self):
        """Read the fuse and link checkboxes into the internal state."""
        self.fuse = self.form.checkbox_fuse.isChecked()
        self.use_link = self.form.checkbox_link.isChecked()

    def _commitArray(self, command, title):
        """Schedule the array creation through the calling command."""
        Gui.addModule("Draft")

        _cmd_list = [
            "_obj_ = " + command,
            "_obj_.Fuse = " + str(self.fuse),
            "Draft.autogroup(_obj_)",
            "App.ActiveDocument.recompute()",
        ]

        # We commit the command list through the parent command
        self.source_command.commit(translate("draft", title), _cmd_list)

    def set_widget_callbacks(self):
        """Set up the callbacks (slots) for the widget signals."""
        # New style for Qt5
        self.form.button_reset.clicked.connect(self.reset_point)

        # When the checkbox changes, change the internal value
        if hasattr(self.form.checkbox_fuse, "checkStateChanged"):  # Qt version >= 6.7.0
            self.form.checkbox_fuse.checkStateChanged.connect(self.set_fuse)
            self.form.checkbox_link.checkStateChanged.connect(self.set_link)
        else:  # Qt version < 6.7.0
            self.form.checkbox_fuse.stateChanged.connect(self.set_fuse)
            self.form.checkbox_link.stateChanged.connect(self.set_link)

    def get_center(self):
        """Get the value of the center from the widgets."""
        c_x_str = self.form.input_c_x.text()
        c_y_str = self.form.input_c_y.text()
        c_z_str = self.form.input_c_z.text()
        center = App.Vector(_quantity(c_x_str), _quantity(c_y_str), _quantity(c_z_str))
        return center

    def constrain_point(self, point, last=None):
        """Apply locked center coordinates to a snapped point."""
        constrained = App.Vector(point)
        for key in ("x", "y", "z"):
            value = self.locks.locked_value(key)
            if value is not None:
                setattr(constrained, key, value)
        return constrained

    def has_point_constraints(self):
        return self.locks.any_locked()

    def get_axis(self):
        """Get the axis that will be used for the array. NOT IMPLEMENTED.

        It should consider a second selection of an edge or wire to use
        as an axis.
        """
        return self.axis

    def reset_point(self):
        """Reset the center point to the original distance."""
        self.locks.unlock_all()
        self.form.input_c_x.setProperty("rawValue", 0)
        self.form.input_c_y.setProperty("rawValue", 0)
        self.form.input_c_z.setProperty("rawValue", 0)

        self.center = self.get_center()

    def print_fuse_state(self, fuse):
        """Print the fuse state translated."""
        if fuse:
            state = self.tr_true
        else:
            state = self.tr_false
        _msg(translate("draft", "Fuse:") + " {}".format(state))

    def set_fuse(self):
        """Execute as a callback when the fuse checkbox changes."""
        self.fuse = self.form.checkbox_fuse.isChecked()
        params.set_param("Draft_array_fuse", self.fuse)

    def print_link_state(self, use_link):
        """Print the link state translated."""
        if use_link:
            state = self.tr_true
        else:
            state = self.tr_false
        _msg(translate("draft", "Create Link array:") + " {}".format(state))

    def set_link(self):
        """Execute as a callback when the link checkbox changes."""
        self.use_link = self.form.checkbox_link.isChecked()
        params.set_param("Draft_array_Link", self.use_link)

    def display_point(self, point=None, plane=None, mask=None):
        """Display the coordinates in the x, y, and z widgets.

        This function should be used in a Coin callback so that
        the coordinate values are automatically updated when the
        mouse pointer moves.
        This was copied from `DraftGui.py` but needs to be improved
        for this particular command.

        point: Base::Vector3
            is a vector that arrives by the callback.
        plane: WorkingPlane.PlaneGui
            is a working plane instance. Not used at the moment.
        mask: str
            is a string that specifies which coordinate is being
            edited. It is used to restrict edition of a single coordinate.
            It is not used at the moment but could be used with a callback.
        """
        # Get the coordinates to display
        d_p = None
        if point:
            d_p = point

        # Set the widgets to the value of the mouse pointer.
        #
        # setProperty() is used if the widget is a FreeCAD widget like
        # Gui::InputField or Gui::QuantitySpinBox, which are based on
        # QLineEdit and QAbstractSpinBox.
        #
        # setText() is used to set the text inside the widget, this may be
        # useful in some circumstances.
        #
        # The mask allows editing only one field, that is, only one coordinate.
        # sbx = self.form.spinbox_c_x
        # sby = self.form.spinbox_c_y
        # sbz = self.form.spinbox_c_z
        if d_p:
            if not self.locks.is_locked("x"):
                self.form.input_c_x.setProperty("rawValue", d_p.x)
            if not self.locks.is_locked("y"):
                self.form.input_c_y.setProperty("rawValue", d_p.y)
            if not self.locks.is_locked("z"):
                self.form.input_c_z.setProperty("rawValue", d_p.z)

        if plane:
            pass

        # Set masks
        if (mask == "x") or (self.mask == "x"):
            self.form.input_c_x.setEnabled(True)
            self.form.input_c_y.setEnabled(False)
            self.form.input_c_z.setEnabled(False)
            self.set_focus("x")
        elif (mask == "y") or (self.mask == "y"):
            self.form.input_c_x.setEnabled(False)
            self.form.input_c_y.setEnabled(True)
            self.form.input_c_z.setEnabled(False)
            self.set_focus("y")
        elif (mask == "z") or (self.mask == "z"):
            self.form.input_c_x.setEnabled(False)
            self.form.input_c_y.setEnabled(False)
            self.form.input_c_z.setEnabled(True)
            self.set_focus("z")
        else:
            self.form.input_c_x.setEnabled(True)
            self.form.input_c_y.setEnabled(True)
            self.form.input_c_z.setEnabled(True)
            self.set_focus()

    def set_focus(self, key=None):
        """Set the focus on the widget that receives the key signal."""
        if key is None or key == "x":
            self.form.input_c_x.setFocus()
            self.form.input_c_x.selectAll()
        elif key == "y":
            self.form.input_c_y.setFocus()
            self.form.input_c_y.selectAll()
        elif key == "z":
            self.form.input_c_z.setFocus()
            self.form.input_c_z.selectAll()

    def reject(self):
        """Execute when clicking the Cancel button or pressing Escape."""
        self.finish()

    def finish(self):
        """Finish the command, after accept or reject.

        It finally calls the parent class to execute
        the delayed functions, and perform cleanup.
        """
        # App.ActiveDocument.commitTransaction()
        if Gui.ActiveDocument is not None:
            Gui.ActiveDocument.resetEdit()
        # Runs the parent command to complete the call
        self.source_command.completed()


## @}
