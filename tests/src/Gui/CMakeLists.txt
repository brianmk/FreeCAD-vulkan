# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(Dialogs)
add_subdirectory(propertyeditor)

# Standard C++ GTest tests
add_executable(Gui_tests_run
        Assistant.cpp
        Camera.cpp
        ColorScaleOverlayTest.cpp
        FileDialog.cpp
        StyleParameters/StyleParametersApplicationTest.cpp
        StyleParameters/ParserTest.cpp
        StyleParameters/ParameterManagerTest.cpp
        StyleParameters/YamlParameterSourceTest.cpp
        InputHintTest.cpp
        SelectionTest.cpp
        SoRayPickActionTest.cpp
)

# The Vulkan viewport test includes <Inventor/actions/SoIRRenderAction.h>,
# which only exists in the bundled Coin (FREECAD_USE_VULKAN implies it).
if(FREECAD_USE_VULKAN)
    target_sources(Gui_tests_run PRIVATE
        VulkanSelectionTest.cpp
    )
endif()

# Qt tests
setup_qt_test(ObjectSelection)
setup_qt_test(QuantitySpinBox)

target_link_libraries(Gui_tests_run PRIVATE
    GTest::gmock_main
    FreeCADApp
    FreeCADGui
)
