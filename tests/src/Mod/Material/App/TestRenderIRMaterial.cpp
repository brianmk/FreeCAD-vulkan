// SPDX-License-Identifier: LGPL-2.1-or-later

// CPU-level contract tests for the consolidated material mapping in
// SoRenderIR.  The raster Vulkan backend and the ray-tracing backend both fill
// their material staging from SoRenderIR::packMaterialBlock and
// SoRenderIR::effectiveMaterialAmbient, so these tests pin the one definition
// both backends rely on (see the material-consolidation work).

#include <gtest/gtest.h>

#include <cstddef>

#include <Inventor/SbVec.h>
#include <Inventor/rendering/SoRenderIR.h>

namespace
{

SoTextureData makeMap(int width, int height, int components, unsigned char * pixels)
{
    SoTextureData texture;
    texture.pixels = pixels;
    texture.width = width;
    texture.height = height;
    texture.numComponents = components;
    return texture;
}

}  // namespace

TEST(RenderIRMaterial, MaterialBlockLayout)
{
    // std140/std430 mirror: eight tightly packed vec4.
    EXPECT_EQ(sizeof(SoRenderIR::SoMaterialBlock), 128u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, diffuse), 0u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, ambient), 16u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, specular), 32u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, emissive), 48u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, params), 64u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, pbr), 80u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, mapParams), 96u);
    EXPECT_EQ(offsetof(SoRenderIR::SoMaterialBlock, optical), 112u);
}

TEST(RenderIRMaterial, PackMaterialBlockDefaults)
{
    SoMaterialData material;
    SoRenderIR::SoMaterialBlock block;
    SoRenderIR::packMaterialBlock(block, material);

    EXPECT_FLOAT_EQ(block.diffuse[0], 0.8f);
    EXPECT_FLOAT_EQ(block.diffuse[3], 1.0f);
    EXPECT_FLOAT_EQ(block.ambient[0], 0.2f);
    EXPECT_FLOAT_EQ(block.specular[0], 0.0f);
    EXPECT_FLOAT_EQ(block.emissive[0], 0.0f);

    // RGB terms carry colour; alpha is forced opaque.
    EXPECT_FLOAT_EQ(block.ambient[3], 1.0f);
    EXPECT_FLOAT_EQ(block.specular[3], 1.0f);
    EXPECT_FLOAT_EQ(block.emissive[3], 1.0f);

    EXPECT_FLOAT_EQ(block.params[0], 0.2f);
    EXPECT_FLOAT_EQ(block.params[1], 0.0f);  // two-sided off
    EXPECT_FLOAT_EQ(block.params[2], 0.0f);  // light count: caller-filled
    EXPECT_FLOAT_EQ(block.params[3], 0.0f);  // default shading is unlit

    EXPECT_FLOAT_EQ(block.pbr[0], 0.0f);
    EXPECT_FLOAT_EQ(block.pbr[1], 0.5f);
    EXPECT_FLOAT_EQ(block.pbr[2], 0.0f);  // physical material off
    EXPECT_FLOAT_EQ(block.pbr[3], 0.0f);

    EXPECT_FLOAT_EQ(block.mapParams[0], 1.0f);
    EXPECT_FLOAT_EQ(block.mapParams[1], 1.0f);
    EXPECT_FLOAT_EQ(block.mapParams[2], 1.0f);
    EXPECT_FLOAT_EQ(block.mapParams[3], 0.0f);  // no maps present

    EXPECT_FLOAT_EQ(block.optical[0], 1.5f);  // default glass IOR
    EXPECT_FLOAT_EQ(block.optical[1], 0.0f);  // default absorption
    EXPECT_FLOAT_EQ(block.optical[2], 1.0f);  // opaque transmittance
    EXPECT_FLOAT_EQ(block.optical[3], 0.0f);  // optics not authored
}

TEST(RenderIRMaterial, PackMaterialBlockValues)
{
    SoMaterialData material;
    material.diffuse.setValue(0.1f, 0.2f, 0.3f, 0.4f);
    material.ambient.setValue(0.4f, 0.5f, 0.6f, 0.7f);
    material.specular.setValue(0.7f, 0.8f, 0.9f, 0.1f);
    material.emissive.setValue(0.2f, 0.3f, 0.4f, 0.5f);
    material.shininess = 0.75f;
    material.twoSidedLighting = true;
    material.shadingModel = SO_SHADING_LEGACY_GOURAUD;
    material.metalness = 0.9f;
    material.roughness = 0.15f;
    material.physicalMaterial = true;
    material.roughnessStrength = 0.6f;
    material.normalStrength = 0.3f;
    material.emissiveIntensity = 2.5f;
    material.transmissionIor = 1.7f;
    material.transmissionAbsorption = 0.4f;
    material.opacity = 0.6f;
    material.transmissionAuthored = true;

    SoRenderIR::SoMaterialBlock block;
    SoRenderIR::packMaterialBlock(block, material);

    // Diffuse keeps its authored alpha; the colour terms force alpha opaque.
    EXPECT_FLOAT_EQ(block.diffuse[0], 0.1f);
    EXPECT_FLOAT_EQ(block.diffuse[3], 0.4f);
    EXPECT_FLOAT_EQ(block.ambient[1], 0.5f);
    EXPECT_FLOAT_EQ(block.ambient[3], 1.0f);
    EXPECT_FLOAT_EQ(block.specular[2], 0.9f);
    EXPECT_FLOAT_EQ(block.specular[3], 1.0f);
    EXPECT_FLOAT_EQ(block.emissive[0], 0.2f);
    EXPECT_FLOAT_EQ(block.emissive[3], 1.0f);

    EXPECT_FLOAT_EQ(block.params[0], 0.75f);
    EXPECT_FLOAT_EQ(block.params[1], 1.0f);
    EXPECT_FLOAT_EQ(block.params[3], 1.0f);  // gouraud

    EXPECT_FLOAT_EQ(block.pbr[0], 0.9f);
    EXPECT_FLOAT_EQ(block.pbr[1], 0.15f);
    EXPECT_FLOAT_EQ(block.pbr[2], 1.0f);

    EXPECT_FLOAT_EQ(block.mapParams[0], 0.6f);
    EXPECT_FLOAT_EQ(block.mapParams[1], 0.3f);
    EXPECT_FLOAT_EQ(block.mapParams[2], 2.5f);

    EXPECT_FLOAT_EQ(block.optical[0], 1.7f);
    EXPECT_FLOAT_EQ(block.optical[1], 0.4f);
    EXPECT_FLOAT_EQ(block.optical[2], 0.6f);  // transmittance == opacity
    EXPECT_FLOAT_EQ(block.optical[3], 1.0f);  // optics authored
}

TEST(RenderIRMaterial, PackMaterialBlockMapPresenceBitmask)
{
    unsigned char pixels[4] = {1, 2, 3, 255};
    SoMaterialData material;
    material.roughnessTexture = makeMap(2, 2, 1, pixels);
    material.normalTexture = makeMap(2, 2, 3, pixels);
    material.emissiveTexture = makeMap(2, 2, 3, pixels);

    SoRenderIR::SoMaterialBlock block;
    SoRenderIR::packMaterialBlock(block, material);
    EXPECT_FLOAT_EQ(block.mapParams[3], 7.0f);  // all three present

    // Dropping the normal map clears bit 1 only.
    material.normalTexture = SoTextureData {};
    SoRenderIR::packMaterialBlock(block, material);
    EXPECT_FLOAT_EQ(block.mapParams[3], 5.0f);

    // A zero-sized map counts as absent even with a pixel pointer.
    material.roughnessTexture = makeMap(0, 0, 1, pixels);
    SoRenderIR::packMaterialBlock(block, material);
    EXPECT_FLOAT_EQ(block.mapParams[3], 4.0f);
}

TEST(RenderIRMaterial, EffectiveMaterialAmbient)
{
    const SbVec3f scene(0.5f, 0.25f, 2.0f);
    const SbVec4f material(0.4f, 0.8f, 0.1f, 0.9f);
    const SbVec3f ambient = SoRenderIR::effectiveMaterialAmbient(scene, material);
    EXPECT_FLOAT_EQ(ambient[0], 0.2f);
    EXPECT_FLOAT_EQ(ambient[1], 0.2f);
    EXPECT_FLOAT_EQ(ambient[2], 0.2f);
}
