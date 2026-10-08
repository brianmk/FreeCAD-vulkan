# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(Dialogs)
add_subdirectory(propertyeditor)

# Standard C++ GTest tests
add_executable(Gui_tests_run
        Assistant.cpp
        Camera.cpp
        ColorScaleOverlayTest.cpp
        FileDialog.cpp
        StyleParameters/DiagnosticsCapture.h
        StyleParameters/ValueMatchers.h
        StyleParameters/ValueMatchers.cpp
        StyleParameters/StyleParametersApplicationTest.cpp
        StyleParameters/ParserTest.cpp
        StyleParameters/ParserRobustnessTest.cpp
        StyleParameters/ParameterManagerTest.cpp
        StyleParameters/YamlParameterSourceTest.cpp
        StyleParameters/DiagnosticsTest.cpp
        StyleParameters/ValueAccessTest.cpp
        StyleParameters/EdgesTest.cpp
        StyleParameters/CornersTest.cpp
        StyleParameters/GradientTest.cpp
        StyleParameters/ColorShadingTest.cpp
        InputHintTest.cpp
        ViewProviderDoubleClick.cpp
        NativeGesturePinchTest.cpp
        NavigationStylePinchTest.cpp
        NavigationStyleWheelTest.cpp
        SelectionTest.cpp
        SoMouseWheelEventTest.cpp
)

# Qt tests
setup_qt_test(ObjectSelection)
setup_qt_test(TranslatorLocale)
setup_qt_test(InputField)
setup_qt_test(QuantitySpinBox)
setup_qt_test(NumericLocaleConsumers)

target_link_libraries(Gui_tests_run PRIVATE
    GTest::gmock_main
    FreeCADApp
    FreeCADGui
)

if(WIN32)
    target_compile_definitions(Gui_tests_run PRIVATE QUARTER_DLL)
endif()

# Regression for the Vulkan-only right-click context menu warning
# ("QWidgetWindow(...) must be a top level window.").  Driven through the real
# FreeCAD binary: the bug needs the Vulkan page (QWidget::createWindowContainer)
# to make the MDI chain native, which a process-local test never brings up.  It
# skips (exit 77) when no Vulkan device is present.
if(BUILD_GUI AND UNIX AND NOT APPLE AND FREECAD_USE_VULKAN)
    find_program(FREECAD_XVFB_RUN xvfb-run)
    if(FREECAD_XVFB_RUN)
        set(FREECAD_GUI_TEST_HOME "${CMAKE_BINARY_DIR}/Testing/gui-home")
        file(MAKE_DIRECTORY
            "${FREECAD_GUI_TEST_HOME}"
            "${FREECAD_GUI_TEST_HOME}/cfg"
            "${FREECAD_GUI_TEST_HOME}/data"
            "${FREECAD_GUI_TEST_HOME}/cache"
        )
        add_test(NAME Gui_RightClickContextMenu_Vulkan
            COMMAND
                "${FREECAD_XVFB_RUN}" -a -s "-screen 0 1280x1024x24"
                "${CMAKE_COMMAND}" -E env
                    "QT_QPA_PLATFORM=xcb"
                    "HOME=${FREECAD_GUI_TEST_HOME}"
                    "XDG_CONFIG_HOME=${FREECAD_GUI_TEST_HOME}/cfg"
                    "XDG_DATA_HOME=${FREECAD_GUI_TEST_HOME}/data"
                    "XDG_CACHE_HOME=${FREECAD_GUI_TEST_HOME}/cache"
                "${CMAKE_BINARY_DIR}/bin/FreeCAD"
                "${CMAKE_CURRENT_SOURCE_DIR}/RightClickContextMenu.py"
        )
        set_tests_properties(Gui_RightClickContextMenu_Vulkan PROPERTIES
            LABELS "Qt;requires-vulkan"
            SKIP_RETURN_CODE 77
            TIMEOUT 300
        )
    endif()
endif()
