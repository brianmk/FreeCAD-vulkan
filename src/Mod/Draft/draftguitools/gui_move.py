# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2009, 2010 Yorik van Havre <yorik@uncreated.net>        *
# *   Copyright (c) 2009, 2010 Ken Cline <cline@frii.com>                   *
# *   Copyright (c) 2020 Eliud Cabrera Castillo <e.cabrera-castillo@tum.de> *
# *   Copyright (c) 2024 FreeCAD Project Association                        *
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
"""Provides GUI tools to move objects in the 3D space."""

## @package gui_move
# \ingroup draftguitools
# \brief Provides GUI tools to move objects in the 3D space.

## \addtogroup draftguitools
# @{
from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCAD as App
import FreeCADGui as Gui
import DraftVecUtils
from draftguitools import gui_base_original
from draftguitools import gui_tool_utils
from draftguitools import gui_trackers as trackers
from draftguitools.gui_subelements import SubelementHighlight
from draftutils import params
from draftutils import utils
from draftutils import todo
from draftutils.messages import _msg, _err, _toolmsg
from draftutils.translate import translate


class Move(gui_base_original.Modifier):
    """Gui Command for the Move tool."""

    def GetResources(self):
        """Set icon, menu and tooltip."""
        return {
            "Pixmap": "Draft_Move",
            "Accel": "M, V",
            "MenuText": QT_TRANSLATE_NOOP("Draft_Move", "Move"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Draft_Move",
                'Moves the selected objects.\nIf the "Copy" option is active, it creates displaced copies.',
            ),
        }

    def Activated(self):
        """Execute when the command is called."""
        super().Activated(
            name="Move", is_subtool=isinstance(App.activeDraftCommand, SubelementHighlight)
        )
        if not self.ui:
            return
        self.ghosts = []
        self.get_object_selection()

    def get_object_selection(self):
        """Get the object selection."""
        if Gui.Selection.hasSelection():
            return self.proceed()
        self.ui.selectUi(on_close_call=self.finish)
        _msg(translate("draft", "Select an object to move"))
        self.call = self.view.addEventCallback("SoEvent", gui_tool_utils.selectObject)

    def proceed(self):
        """Continue with the command after a selection has been made."""
        if self.call:
            self.view.removeEventCallback("SoEvent", self.call)
        self.selection = Gui.Selection.getSelectionEx("", 0)
        Gui.doCommand('selection = FreeCADGui.Selection.getSelectionEx("", 0)')
        self.ui.lineUi(title=translate("draft", self.featureName), icon="Draft_Move")
        self.ui.modUi()
        if self.copymode:
            self.ui.isCopy.setChecked(True)

        # State used by the interactive axis gizmo.
        self.gizmo = None
        self._gizmo_cbs = []
        self.gizmo_dragging = False
        self.gizmo_used = False
        self.cancelled = False
        self.gizmo_delta = None
        self.vector = None

        self._setup_gizmo()

        if self.gizmo is not None:
            # Anchor the relative X/Y/Z fields to the gizmo origin so they act
            # as a displacement vector instead of absolute coordinates.
            self._set_local_relative()
            self.node = [self.gizmo_origin]
            self.ui.isRelative.show()
            zero = App.Units.Quantity(0, App.Units.Length).UserString
            self.ui.xValue.setText(zero)
            self.ui.yValue.setText(zero)
            self.ui.zValue.setText(zero)
            self.ui.finishButton.show()

        if not self.ghosts:
            self.set_ghosts()

        self.ui.xValue.setFocus()
        self.ui.xValue.selectAll()
        self.call = self.view.addEventCallback("SoEvent", self.action)
        if self.gizmo is not None:
            _toolmsg(translate("draft", "Drag an axis, or type a distance and press Enter"))
        else:
            _toolmsg(translate("draft", "Pick start point"))
        self.selection_done = True
        self.update_hints()

    def _set_local_relative(self):
        """Force working-plane (local) relative mode for this command only."""
        for cb in (self.ui.isGlobal, self.ui.isRelative):
            cb.blockSignals(True)
        self.ui.globalMode = False
        self.ui.relativeMode = True
        self.ui.isGlobal.setChecked(False)
        self.ui.isRelative.setChecked(True)
        for cb in (self.ui.isGlobal, self.ui.isRelative):
            cb.blockSignals(False)
        self.ui.checkLocal()

    def _get_gizmo_origin(self):
        """Return the point the gizmo is anchored to (usually the object origin)."""
        try:
            objs = [s.Object for s in self.selection if s.Object is not None]
            if objs:
                first = objs[0]
                try:
                    return first.getGlobalPlacement().Base
                except Exception:
                    try:
                        return first.Placement.Base
                    except Exception:
                        return self.wp.position
        except Exception:
            pass
        return self.wp.position

    def _setup_gizmo(self):
        """Create and register the interactive axis gizmo (if enabled)."""
        if not params.get_param("DraftMoveGizmo"):
            return
        try:
            self.gizmo_origin = self._get_gizmo_origin()
            gizmo = trackers.TranslateGizmo(
                origin=self.gizmo_origin, rotation=self.wp.get_placement().Rotation
            )
            gizmo.update_scale_from_camera(self.view, self.gizmo_origin)
            for axis, dragger in gizmo.get_draggers().items():
                for cbtype, cb in (
                    ("addStartCallback", self.gizmo_start),
                    ("addMotionCallback", self.gizmo_motion),
                    ("addFinishCallback", self.gizmo_finish),
                ):
                    self._gizmo_cbs.append(
                        (dragger, cbtype, self.view.addDraggerCallback(dragger, cbtype, cb))
                    )
            self.gizmo = gizmo
            # The axis draggers only receive mouse events when the viewer
            # forwards them to the scene graph; by default events go to the
            # navigation style only, so the handles would be shown but never
            # grabbable.  Restored in finish().
            try:
                self.view.getViewer().setRedirectToSceneGraph(True)
                self._gizmo_redirect = True
            except Exception:
                self._gizmo_redirect = False
            # Remember the moved objects' pickability so it can be toggled while
            # the cursor is over a handle (see _set_gizmo_pick_block) and
            # restored in finish().
            self._gizmo_selectable = []
            for sel in self.selection:
                obj = getattr(sel, "Object", None)
                vp = getattr(obj, "ViewObject", None) if obj is not None else None
                if vp is None or not hasattr(vp, "Selectable"):
                    continue
                try:
                    self._gizmo_selectable.append((vp, bool(vp.Selectable)))
                except Exception:
                    pass
        except Exception as e:
            _err(translate("draft", "Could not create the move gizmo: {}").format(e))
            self.gizmo = None

    def _remove_gizmo_callbacks(self):
        for dragger, cbtype, cb in getattr(self, "_gizmo_cbs", []):
            try:
                self.view.removeDraggerCallback(dragger, cbtype, cb)
            except Exception:
                pass
        self._gizmo_cbs = []

    def _set_gizmo_pick_block(self, block):
        """Toggle pickability of the moved objects while over a gizmo handle.

        The handles extend from the object origin and are usually occluded by
        the object itself, and a Coin dragger only grabs when it is the closest
        pick under the cursor.  So while the cursor is over a handle the moved
        objects are made unpickable (the dragger can win the pick); the rest of
        the time they stay pickable so Draft's own point-picking still snaps to
        them.
        """
        for vp, was in getattr(self, "_gizmo_selectable", []):
            try:
                want = False if block else was
                if bool(vp.Selectable) != want:
                    vp.Selectable = want
            except Exception:
                pass

    def gizmo_start(self, dragger):
        """Called when the user grabs a gizmo axis."""
        if self.gizmo is None or self.gizmo.axis_of(dragger) is None:
            return
        self.gizmo_dragging = True
        if not self.node:
            self.node = [self.gizmo_origin]
        # Snap the ghost back to the origin so it follows the drag from zero.
        zero = App.Vector(0, 0, 0)
        for ghost in self.ghosts:
            ghost.move(zero)
            ghost.on()

    def gizmo_motion(self, dragger):
        """Called while the user drags a gizmo axis."""
        if self.gizmo is None or self.gizmo.axis_of(dragger) is None:
            return
        self.gizmo_used = True
        delta = self.gizmo.total_delta()
        self.vector = delta
        self.gizmo_delta = delta
        for ghost in self.ghosts:
            ghost.move(delta)
            ghost.on()
        if self.ui.isTaskOn:
            self.ui.displayPoint(self.gizmo_origin + delta, self.gizmo_origin)

    def gizmo_finish(self, dragger):
        """Called when the user releases a gizmo axis."""
        if self.gizmo is None or self.gizmo.axis_of(dragger) is None:
            return
        self.gizmo_dragging = False
        self.gizmo_used = True
        for ghost in self.ghosts:
            ghost.on()

    def finish(self, cont=False):
        """Terminate the operation.

        Parameters
        ----------
        cont: bool or None, optional
            Restart (continue) the command if `True`, or if `None` and
            `ui.continueMode` is `True`.
        """
        # Apply a pending gizmo displacement (e.g. the Finish button was
        # pressed) unless the command was cancelled with Esc, or the task panel
        # has already been destroyed (e.g. on workbench deactivation, in which
        # case its widgets raise RuntimeError on access).
        if (
            not getattr(self, "cancelled", False)
            and getattr(self, "gizmo_delta", None) is not None
            and self.gizmo_delta.Length > 1e-9
        ):
            try:
                self.move(self.ui.isCopy.isChecked())
            except RuntimeError:
                # Panel gone: drop the pending displacement instead of crashing.
                self.gizmo_delta = None
        self._remove_gizmo_callbacks()
        if getattr(self, "_gizmo_redirect", False):
            try:
                self.view.getViewer().setRedirectToSceneGraph(False)
            except Exception:
                pass
            self._gizmo_redirect = False
        for vp, was in getattr(self, "_gizmo_selectable", []):
            try:
                vp.Selectable = was
            except Exception:
                pass
        self._gizmo_selectable = []
        if getattr(self, "gizmo", None) is not None:
            self.gizmo.finalize()
            self.gizmo = None
        self.end_callbacks(self.call)
        for ghost in self.ghosts:
            ghost.finalize()
        super().finish()
        if cont or (cont is None and self.ui and self.ui.continueMode):
            todo.ToDo.delayAfter(self.Activated, [])

    def action(self, arg):
        """Handle the 3D scene events.

        This is installed as an EventCallback in the Inventor view.

        Parameters
        ----------
        arg: dict
            Dictionary with strings that indicates the type of event received
            from the 3D view.
        """
        # While the cursor is over a handle, hide the moved objects from the
        # pick so the Coin dragger can win it (see _set_gizmo_pick_block).
        if (
            self.gizmo is not None
            and not self.gizmo_dragging
            and arg.get("Type") in ("SoLocation2Event", "SoMouseButtonEvent")
        ):
            pos = arg.get("Position")
            if pos is not None:
                self._set_gizmo_pick_block(
                    self.gizmo.is_under_cursor(self.view, pos) is not None
                )
        if self.gizmo_dragging:
            return  # the axis dragger owns the mouse
        if arg["Type"] == "SoKeyboardEvent" and arg["Key"] == "ESCAPE":
            self.cancelled = True
            self.finish()
        elif arg["Type"] == "SoLocation2Event":
            if self.gizmo_used:
                return  # keep the dragged displacement, ignore hover
            self.handle_mouse_move_event(arg)
        elif (
            arg["Type"] == "SoMouseButtonEvent"
            and arg["State"] == "DOWN"
            and arg["Button"] == "BUTTON1"
        ):
            if self.gizmo is not None:
                pos = arg.get("Position")
                if pos is not None and self.gizmo.is_under_cursor(self.view, pos):
                    return  # let the axis dragger handle the grab
            self.handle_mouse_click_event(arg)

    def handle_mouse_move_event(self, arg):
        """Handle the mouse when moving."""
        for ghost in self.ghosts:
            ghost.off()
        self.point, ctrlPoint, info = gui_tool_utils.getPoint(self, arg)
        if len(self.node) > 0:
            last = self.node[len(self.node) - 1]
            if self.point:
                self.vector = self.point.sub(last)
            else:
                self.vector = None
            for ghost in self.ghosts:
                if self.vector:
                    ghost.move(self.vector)
                ghost.on()
        if self.extendedCopy:
            if not gui_tool_utils.hasMod(arg, gui_tool_utils.get_mod_alt_key()):
                self.finish()
        gui_tool_utils.redraw3DView()

    def handle_mouse_click_event(self, arg):
        """Handle the mouse when the first button is clicked."""
        if not self.ghosts:
            self.set_ghosts()
        if not self.point:
            return
        # A click away from the gizmo resumes plain point picking; drop any
        # pending gizmo displacement so it is not double-applied.
        self.gizmo_used = False
        self.gizmo_delta = None
        if self.gizmo is not None:
            self.gizmo.reset()
        self.ui.redraw()
        if self.node == []:
            self.node.append(self.point)
            self.ui.isRelative.show()
            for ghost in self.ghosts:
                ghost.on()
            _toolmsg(translate("draft", "Pick end point"))
            self.update_hints()
            if self.planetrack:
                self.planetrack.set(self.point)
        else:
            last = self.node[0]
            self.vector = self.point.sub(last)
            self.move(
                self.ui.isCopy.isChecked()
                or gui_tool_utils.hasMod(arg, gui_tool_utils.get_mod_alt_key())
            )
            if gui_tool_utils.hasMod(arg, gui_tool_utils.get_mod_alt_key()):
                self.extendedCopy = True
            else:
                self.finish(cont=None)

    def set_ghosts(self):
        """Set the ghost to display."""
        for ghost in self.ghosts:
            ghost.remove()
        copy = self.ui.isCopy.isChecked()
        if self.ui.isSubelementMode.isChecked():
            self.ghosts = self.get_subelement_ghosts(self.selection, copy)
            if not self.ghosts:
                _err(translate("draft", "No valid subelements selected"))
        else:
            objs, places, _ = utils._modifiers_process_selection(
                self.selection, copy, add_movable_children=(not copy)
            )
            self.ghosts = [trackers.ghostTracker(objs, parent_places=places)]

    def get_subelement_ghosts(self, selection, copy):
        """Get ghost for the subelements (vertices, edges)."""
        import Part

        ghosts = []
        for sel in selection:
            for sub in sel.SubElementNames if sel.SubElementNames else [""]:
                if (not copy and "Vertex" in sub) or "Edge" in sub:
                    obj = sel.Object.getSubObject(sub, 1)
                    if utils.get_type(obj) != "Wire":
                        continue
                    shape = Part.getShape(sel.Object, sub, needSubElement=True, retType=0)
                    ghosts.append(trackers.ghostTracker(shape))
        return ghosts

    def move(self, copy):
        """Perform the move of the subelement(s) or the entire object(s)."""
        if copy:
            cmd_name = translate("draft", "Copy")
        else:
            cmd_name = translate("draft", "Move")
        Gui.addModule("Draft")
        cmd = "Draft.move(selection, "
        cmd += DraftVecUtils.toString(self.vector) + ", "
        cmd += "copy=" + str(copy) + ", "
        cmd += "subelements=" + str(self.ui.isSubelementMode.isChecked()) + ")"
        cmd_list = [cmd, "FreeCAD.ActiveDocument.recompute()"]
        self.commit(cmd_name, cmd_list)
        # The displacement is now applied; reset the gizmo for the next one.
        self.gizmo_delta = None
        if self.gizmo is not None:
            self.gizmo.reset()
            for ghost in self.ghosts:
                ghost.off()

    def numericInput(self, numx, numy, numz):
        """Validate the entry fields in the user interface.

        This function is called by the toolbar or taskpanel interface
        when valid x, y, and z have been entered in the input fields.
        """
        self.point = App.Vector(numx, numy, numz)
        if not self.node:
            self.node.append(self.point)
            self.ui.isRelative.show()
            self.ui.isCopy.show()
            for ghost in self.ghosts:
                ghost.on()
            _toolmsg(translate("draft", "Pick end point"))
            self.update_hints()
        else:
            last = self.node[-1]
            self.vector = self.point.sub(last)
            self.move(self.ui.isCopy.isChecked())
            self.finish(cont=None)

    def get_action_hints(self):
        if not self.node:
            hints = [
                Gui.InputHint(translate("draft", "%1 pick start point"), Gui.UserInput.MouseLeft)
            ]
        else:
            hints = [
                Gui.InputHint(translate("draft", "%1 pick end point"), Gui.UserInput.MouseLeft)
            ]
        return (
            hints
            + gui_tool_utils._get_hint_xyz_constrain()
            + gui_tool_utils._get_hint_mod_constrain()
            + gui_tool_utils._get_hint_mod_snap()
            + gui_tool_utils._get_hint_mod_copy()
        )


Gui.addCommand("Draft_Move", Move())

## @}
