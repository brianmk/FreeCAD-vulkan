# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(PartGui_tests_run
        VulkanSelectionTest.cpp
)

target_link_libraries(PartGui_tests_run
    GTest::gtest_main
    PartGui
    FreeCADGui
)
