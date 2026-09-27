# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(Material_tests_run
        TestMaterialCards.cpp
        TestMaterialFilter.cpp
        TestMaterialProperties.cpp
        TestMaterials.cpp
        TestMaterialValue.cpp
        TestModel.cpp
        TestModelProperties.cpp
        TestRenderIRMaterial.cpp
)

target_include_directories(Material_tests_run SYSTEM PUBLIC
        ${QtCore_INCLUDE_DIRS}
)

# TestRenderIRMaterial exercises the consolidated material mapping in Coin's
# retained render IR (SoRenderIR::packMaterialBlock / effectiveMaterialAmbient),
# which both the raster and ray-tracing backends share.  Linking the Coin target
# supplies its SYSTEM INTERFACE include directory.
target_link_libraries(Material_tests_run Coin)

set(MaterialTestData_Files
        Materials/TestAcrylicLegacy.FCMat
        Materials/TestAluminumAppearance.FCMat
        Materials/TestAluminumMixed.FCMat
        Materials/TestAluminumPhysical.FCMat
        Materials/TestBrassAppearance.FCMat
)

ADD_CUSTOM_TARGET(MaterialTestData ALL
        SOURCES ${MaterialTestData_Files}
)

fc_target_copy_resource(MaterialTestData
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/tests
        ${MaterialTestData_Files})

# INSTALL(
#     FILES ${MaterialTest_Files}
#     DESTINATION Mod/Material/materialtests
# )
