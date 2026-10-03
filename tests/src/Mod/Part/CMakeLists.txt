# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(App)

if(BUILD_GUI)
    target_sources(Part_tests_run PRIVATE
        Gui/SoBrepEdgeSet.cpp
        Gui/SoBrepFaceSet.cpp
    )
    target_link_libraries(Part_tests_run PartGui)

    # Standalone benchmark (always passes, reports timings) for the
    # SoBrepFaceSet ray-pick used by hover/preselection.
    add_executable(PartPickBench Gui/PickBench.cpp)
    target_link_libraries(PartPickBench PartGui)
    set_target_properties(PartPickBench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    add_test(NAME PartPickBench COMMAND PartPickBench)
endif()

target_link_libraries(Part_tests_run
    GTest::gtest_main
    ${Python3_LIBRARIES}
    Part
)
