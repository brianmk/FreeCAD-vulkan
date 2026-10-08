# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Regression test for the Vulkan-only right-click context menu warning:
#
#   QWidgetWindow(...) must be a top level window.
#
# Qt derives a popup's transient parent from the parent widget's window.  The
# Vulkan page is created with QWidget::createWindowContainer(), which makes the
# whole MDI chain native.  QMenu(viewer->getGLWidget()) then resolved to the
# viewer's native *child* window (getGLWidget() is the QGraphicsView's
# QOpenGLWidget viewport, whose native parent is the viewer's own
# "Gui::View3DInventorViewerClassWindow"), which QWindow::setTransientParent()
# rejects.  The fix parents the menu to the view's top level window
# (viewer->getGLWidget()->window()).
#
# This is driven through the real FreeCAD binary because the regression needs the
# Vulkan window hierarchy; a process-local test never brings that up.  It exits
# 0 on success, 1 on failure and 77 (CTest SKIP_RETURN_CODE) when no Vulkan
# renderer/page is available, so CTest reports a skip instead of a failure.
#
# Run standalone:
#   xvfb-run -a -s "-screen 0 1280x1024x24" \
#     env HOME=/tmp/fc-tests-a XDG_CONFIG_HOME=/tmp/fc-tests-a/cfg \
#         XDG_DATA_HOME=/tmp/fc-tests-a/data XDG_CACHE_HOME=/tmp/fc-tests-a/cache \
#         QT_QPA_PLATFORM=xcb build-freecad/bin/FreeCAD \
#         tests/src/Gui/RightClickContextMenu.py

import os
import sys
import traceback

SKIP_RETURN_CODE = 77
TOP_LEVEL_WARNING = "must be a top level window"

# FreeCAD redirects sys.stdout to the report view once the GUI is up, so keep a
# handle on the process stream for the CTest log.
_STDOUT = sys.__stdout__


def emit(message):
    try:
        _STDOUT.write(message + "\n")
        _STDOUT.flush()
    except Exception:
        pass


def has_vulkan_device():
    """Best-effort Vulkan device enumeration, so the test skips (77) instead of
    letting the Vulkan widget come up against no driver (which currently
    segfaults inside QuarterVulkanWidget on instance-creation failure)."""
    import ctypes

    class VkApplicationInfo(ctypes.Structure):
        _fields_ = [
            ("sType", ctypes.c_int),
            ("pNext", ctypes.c_void_p),
            ("pApplicationName", ctypes.c_char_p),
            ("applicationVersion", ctypes.c_uint32),
            ("pEngineName", ctypes.c_char_p),
            ("engineVersion", ctypes.c_uint32),
            ("apiVersion", ctypes.c_uint32),
        ]

    class VkInstanceCreateInfo(ctypes.Structure):
        _fields_ = [
            ("sType", ctypes.c_int),
            ("pNext", ctypes.c_void_p),
            ("flags", ctypes.c_uint32),
            ("pApplicationInfo", ctypes.POINTER(VkApplicationInfo)),
            ("enabledLayerCount", ctypes.c_uint32),
            ("ppEnabledLayerNames", ctypes.c_void_p),
            ("enabledExtensionCount", ctypes.c_uint32),
            ("ppEnabledExtensionNames", ctypes.c_void_p),
        ]

    try:
        vk = ctypes.CDLL("libvulkan.so.1")
    except OSError:
        return False

    vk.vkCreateInstance.restype = ctypes.c_int
    vk.vkCreateInstance.argtypes = [
        ctypes.POINTER(VkInstanceCreateInfo),
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    vk.vkEnumeratePhysicalDevices.restype = ctypes.c_int
    vk.vkEnumeratePhysicalDevices.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_void_p,
    ]
    vk.vkDestroyInstance.restype = None
    vk.vkDestroyInstance.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    app_info = VkApplicationInfo()
    app_info.sType = 0  # VK_STRUCTURE_TYPE_APPLICATION_INFO
    app_info.pApplicationName = b"freecad-right-click-regression"
    app_info.apiVersion = (1 << 22) | (0 << 12)  # VK_API_VERSION_1_0
    create_info = VkInstanceCreateInfo()
    create_info.sType = 1  # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    create_info.pApplicationInfo = ctypes.pointer(app_info)

    instance = ctypes.c_void_p()
    if vk.vkCreateInstance(ctypes.byref(create_info), None, ctypes.byref(instance)) != 0:
        return False
    count = ctypes.c_uint32(0)
    result = vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None)
    vk.vkDestroyInstance(instance, None)
    return result == 0 and count.value > 0


def pump(app, rounds=80):
    import FreeCADGui

    for _ in range(rounds):
        app.processEvents()
        try:
            FreeCADGui.updateGui()
        except Exception:
            pass


def find_vulkan_container(main_window):
    """The QWindowContainer of the active Vulkan 3D page, or None."""
    from PySide import QtWidgets

    for widget in main_window.findChildren(QtWidgets.QWidget):
        if "QuarterVulkanWidget" not in widget.metaObject().className():
            continue
        for child in widget.findChildren(QtWidgets.QWidget):
            if child.windowHandle() is not None:
                return child
        return widget
    return None


def send_right_click(widget, position):
    from PySide import QtCore, QtGui, QtWidgets

    for event_type, buttons in (
        (QtCore.QEvent.MouseButtonPress, QtCore.Qt.RightButton),
        (QtCore.QEvent.MouseButtonRelease, QtCore.Qt.NoButton),
    ):
        event = QtGui.QMouseEvent(
            event_type,
            position,
            widget.mapToGlobal(position),
            QtCore.Qt.RightButton,
            buttons,
            QtCore.Qt.NoModifier,
        )
        QtWidgets.QApplication.sendEvent(widget, event)


def run():
    import FreeCAD
    import FreeCADGui
    from PySide import QtCore, QtWidgets, QtGui

    messages = []

    def handler(_mode, _context, message):
        messages.append(str(message))

    QtCore.qInstallMessageHandler(handler)

    def top_level_warnings(since):
        return [m for m in messages[since:] if TOP_LEVEL_WARNING in m]

    if not has_vulkan_device():
        emit("SKIP: no Vulkan physical device")
        return SKIP_RETURN_CODE

    # The Vulkan renderer is opt-in; enable it and ask for the raster Vulkan
    # view mode (1) before any 3D view is created.
    prefs = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
    prefs.SetBool("UseVulkanRenderer", True)
    prefs.SetInt("VulkanRenderMode", 1)

    main_window = FreeCADGui.getMainWindow()
    if main_window is None:
        emit("SKIP: no GUI main window")
        return SKIP_RETURN_CODE
    main_window.show()

    app = QtWidgets.QApplication.instance()
    if app is None:
        emit("SKIP: no QApplication")
        return SKIP_RETURN_CODE

    document = FreeCAD.newDocument("RightClickContextMenuTest")
    pump(app, 100)

    try:
        view = FreeCADGui.ActiveDocument.ActiveView
    except Exception:
        view = None
    if view is None:
        emit("SKIP: no active 3D view")
        return SKIP_RETURN_CODE

    if not hasattr(view, "setRenderMode"):
        emit("SKIP: this build has no Vulkan render modes")
        return SKIP_RETURN_CODE
    view.setRenderMode(1)

    # Wait for the Vulkan page to materialize its native container.
    container = None
    for _ in range(60):
        pump(app, 10)
        container = find_vulkan_container(main_window)
        if container is not None and container.windowHandle() is not None:
            break
    if container is None:
        emit("SKIP: no Vulkan viewport page (no Vulkan device or renderer off)")
        return SKIP_RETURN_CODE

    try:
        graphics_view = view.graphicsView()
        viewport = graphics_view.viewport()
    except Exception:
        graphics_view = None
        viewport = None

    # Sanity-check the harness before asserting on the app path: a deliberate
    # qWarning must reach the handler.
    before = len(messages)
    QtCore.qWarning("right-click-context-menu self test")
    if not messages[before:]:
        emit("FAIL: the Qt message handler captured nothing; detection is broken")
        return 1

    # ---------------------------------------------------------------- app path
    # Synthesize a right-button press+release into the visible Vulkan 3D
    # viewport (falling back to the Coin/GL viewport if the container does not
    # accept synthesized input in this environment).
    popup = None
    warning_before_popup = None
    for target in (container, viewport):
        if target is None:
            continue
        active = QtWidgets.QApplication.activePopupWidget()
        if active is not None:
            active.close()
            pump(app, 20)
        before = len(messages)
        send_right_click(target, target.rect().center())
        pump(app, 80)
        popup = QtWidgets.QApplication.activePopupWidget()
        if popup is not None:
            warning_before_popup = top_level_warnings(before)
            break

    if popup is None or not popup.isVisible():
        emit("FAIL: no visible QMenu popped for a right-click in the 3D viewport")
        return 1

    emit(
        "right-click context menu: popped {} (visible={})".format(
            popup.metaObject().className(), popup.isVisible()
        )
    )

    if warning_before_popup:
        popup.close()
        emit(
            "FAIL: right-click produced the transient-parent warning: "
            + warning_before_popup[0]
        )
        return 1

    popup.close()
    pump(app, 20)

    # --------------------------------------------------- self-check (pre-fix)
    # Reproduce the exact pre-fix parent: QMenu(viewer->getGLWidget()) is
    # QMenu(<QGraphicsView viewport>), whose native parent is the viewer's native
    # child window.  If this no longer warns, the detection above is meaningless.
    if viewport is None:
        emit("FAIL: no QGraphicsView viewport to reproduce the pre-fix parent with")
        return 1

    before = len(messages)
    pre_fix_menu = QtWidgets.QMenu(viewport)
    pre_fix_menu.addAction("pre-fix parent")
    pre_fix_menu.popup(QtGui.QCursor.pos())
    pump(app, 60)
    pre_fix_warnings = top_level_warnings(before)
    pre_fix_menu.close()
    pump(app, 20)

    if not pre_fix_warnings:
        emit(
            "FAIL: the pre-fix parent no longer reproduces the transient-parent "
            "warning; the regression check above cannot detect a revert"
        )
        return 1
    emit("pre-fix parent reproduces the warning: " + pre_fix_warnings[0])

    # And the fixed parent (the top level window) must stay silent.
    before = len(messages)
    fixed_menu = QtWidgets.QMenu(viewport.window())
    fixed_menu.addAction("fixed parent")
    fixed_menu.popup(QtGui.QCursor.pos())
    pump(app, 60)
    fixed_warnings = top_level_warnings(before)
    fixed_menu.close()
    pump(app, 20)
    if fixed_warnings:
        emit("FAIL: the fixed parent still warns: " + fixed_warnings[0])
        return 1

    emit("OK: right-click context menu opens without a transient-parent warning")
    return 0


def main():
    try:
        return run()
    except BaseException:
        traceback.print_exc(file=_STDOUT)
        return 1
    finally:
        try:
            _STDOUT.flush()
        except Exception:
            pass


# FreeCAD executes the macro at top level (there is no __main__ guard), so run
# unconditionally and exit directly with the CTest status code: FreeCAD keeps its
# event loop running after a macro returns.
os._exit(main())
