# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(Part_tests_run
        Attacher.cpp
        AttachExtension.cpp
        BRepMesh.cpp
        FaceMakerBullseye.cpp
        FaceMakerUnified.cpp
        FeatureChamfer.cpp
        FeatureCompound.cpp
        FeatureExtrusion.cpp
        FeatureFillet.cpp
        FeatureMirroring.cpp
        FeatureOffset.cpp
        FeaturePartBoolean.cpp
        FeaturePartCommon.cpp
        FeaturePartCut.cpp
        FeaturePartFuse.cpp
        FeatureRevolution.cpp
        FuzzyBoolean.cpp
        Geometry.cpp
        LinkArrayPolar.cpp
        PartFeature.cpp
        PartFeatures.cpp
        PartTestHelpers.cpp
        PreviewExtension.cpp
        PropertyTopoShape.cpp
        TopoDS_Shape.cpp
        TopoShape.cpp
        TopoShapeCache.cpp
        TopoShapeExpansion.cpp
        TopoShapeMakeElementRefine.cpp
        TopoShapeMakeShapeWithElementMap.cpp
        TopoShapeMapper.cpp
        TopoShapeMakeShape.cpp
        WireJoiner.cpp
)

set(PartTestData_Files
        brepfiles/cylinder1.brep
        brepfiles/helix1.brep
        brepfiles/unorientableSolid.brep
)

ADD_CUSTOM_TARGET(PartTestData ALL
        SOURCES ${PartTestData_Files}
)

fc_target_copy_resource(PartTestData
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/tests
        ${PartTestData_Files})

# Standalone benchmark (always passes, reports timings) comparing the
# OCCT triangulation -> vertex-array conversion done by
# ViewProviderPartExt::setupCoinGeometry against a direct interleaved pack.
add_executable(PartTessellationBench TessellationBench.cpp)
target_link_libraries(PartTessellationBench Part)
if(WIN32)
    set_target_properties(PartTessellationBench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
else()
    set_target_properties(PartTessellationBench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()
add_test(NAME PartTessellationBench COMMAND PartTessellationBench)
