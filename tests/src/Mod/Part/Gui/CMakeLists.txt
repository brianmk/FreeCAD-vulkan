# SPDX-License-Identifier: LGPL-2.1-or-later

# The Vulkan selection test includes <Inventor/actions/SoIRRenderAction.h>,
# which only exists in the bundled Coin (FREECAD_USE_VULKAN implies it).
if(FREECAD_USE_VULKAN)
    add_executable(PartGui_tests_run
        VulkanSelectionTest.cpp
    )
    # The executable is only defined when FREECAD_USE_VULKAN is ON, so the
    # libraries must link inside the same guard: otherwise targets/CMakeLists
    # appends PartGui_tests_run to TestExecutables (BUILD_PART AND BUILD_GUI
    # hold even with Vulkan off) and configure fails on the missing target.
    target_link_libraries(PartGui_tests_run
        GTest::gtest_main
        PartGui
        FreeCADGui
    )
endif()
