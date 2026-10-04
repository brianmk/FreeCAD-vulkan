# SPDX-License-Identifier: LGPL-2.1-or-later

# STEP import conversion cache: correctness tests + benchmark.

add_executable(Import_tests_run
    ImportCache.cpp
)
target_link_libraries(Import_tests_run
    GTest::gtest_main
    Import
    ${Python3_LIBRARIES}
)
set_target_properties(Import_tests_run PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME Import_tests_run COMMAND Import_tests_run)

# Standalone benchmark (always passes, reports timings) for the STEP import
# pipeline phases and the STEP conversion cache.
add_executable(ImportBench ImportBench.cpp)
target_link_libraries(ImportBench
    Import
    ${Python3_LIBRARIES}
)
set_target_properties(ImportBench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME ImportBench COMMAND ImportBench)

# Isolate the import cache so the tests never touch the developer's cache.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/import-home)
set_tests_properties(Import_tests_run ImportBench PROPERTIES
    ENVIRONMENT "FREECAD_USER_HOME=${CMAKE_BINARY_DIR}/tests/import-home")
