# SPDX-License-Identifier: LGPL-2.1-or-later

# The Vulkan selection test includes <Inventor/actions/SoIRRenderAction.h>,
# which only exists in the bundled Coin (FREECAD_USE_VULKAN implies it).
if(FREECAD_USE_VULKAN)
    add_executable(PartGui_tests_run
        VulkanSelectionTest.cpp
    )
endif()

target_link_libraries(PartGui_tests_run
    GTest::gtest_main
    PartGui
    FreeCADGui
)
