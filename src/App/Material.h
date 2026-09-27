// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2005 Jürgen Riegel <juergen.riegel@web.de>              *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#pragma once

#include <Base/Color.h>

namespace App
{

/** Material class
 */
class AppExport Material
{
public:
    enum MaterialType
    {
        BRASS,
        BRONZE,
        COPPER,
        GOLD,
        PEWTER,
        PLASTER,
        PLASTIC,
        SILVER,
        STEEL,
        STONE,
        SHINY_PLASTIC,
        SATIN,
        METALIZED,
        NEON_GNC,
        CHROME,
        ALUMINIUM,
        OBSIDIAN,
        NEON_PHC,
        JADE,
        RUBY,
        EMERALD,
        DEFAULT,
        USER_DEFINED
    };

    /** Projection used to map the embedded texture image onto a shape. */
    enum TextureMapping
    {
        MappingPlanar,      /**< Project the object X/Y coordinates. */
        MappingBox,         /**< Per-face axis-aligned box projection. */
        MappingSpherical,   /**< Spherical projection around the object Z axis. */
        MappingCylindrical  /**< Cylindrical projection around the object Z axis. */
    };

public:
    /** @name Constructors
     */
    //@{
    /** Sets the USER_DEFINED material type. The user must set the colors afterwards. */
    Material();
    ~Material() = default;
    /** Copy constructor. */
    Material(const Material& other) = default;
    Material(Material&& other) = default;
    /** Defines the colors and shininess for the material \a MatName. If \a MatName isn't defined
     * then USER_DEFINED is set and the user must define the colors itself.
     */
    explicit Material(const char* MatName);
    /** Does basically the same as the constructor above unless that it accepts a MaterialType as
     * argument. */
    explicit Material(MaterialType MatType);
    //@}

    /** Set a material by name
     *  There are some standard materials defined which are:
     *  \li Brass
     *  \li Bronze
     *  \li Copper
     *  \li Gold
     *  \li Pewter
     *  \li Plaster
     *  \li Plastic
     *  \li Silver
     *  \li Steel
     *  \li Stone
     *  \li Shiny plastic
     *  \li Satin
     *  \li Metalized
     *  \li Neon GNC
     *  \li Chrome
     *  \li Aluminium
     *  \li Obsidian
     *  \li Neon PHC
     *  \li Jade
     *  \li Ruby
     *  \li Emerald
     * Furthermore there two additional modes \a Default which defines a kind of grey metallic and
     * user defined that does nothing. The Base::Color and the other properties of the material are
     * defined in the range [0-1]. If \a MatName is an unknown material name then the type
     * USER_DEFINED is set and the material doesn't get changed.
     */
    void set(const char* MatName);
    /**
     * This method is provided for convenience which does basically the same as the method above
     * unless that it accepts a MaterialType as argument.
     */
    void setType(MaterialType MatType);
    /**
     * Returns the currently set material type.
     */
    MaterialType getType() const
    {
        return _matType;
    }

    /** @name Properties */
    //@{
    // NOLINTBEGIN
    Base::Color ambientColor;  /**< Defines the ambient color. */
    Base::Color diffuseColor;  /**< Defines the diffuse color. */
    Base::Color specularColor; /**< Defines the specular color. */
    Base::Color emissiveColor; /**< Defines the emissive color. */
    float shininess;
    float transparency;
    /**
     * Metallic-roughness parameters of a physically based material. These are
     * only meaningful when usePhysicalMaterial is true; otherwise the legacy
     * Blinn-Phong fields above define the appearance.
     */
    float metallic;              /**< 0 = dielectric, 1 = conductor. */
    float roughness;             /**< 0 = mirror, 1 = fully diffuse. */
    bool usePhysicalMaterial;    /**< Enable the physical material path. */
    /**
     * Real-world size, in model units (mm), spanned by one tile of the
     * embedded texture image. Smaller values tile the texture more densely.
     */
    float textureSize;
    /** Projection used to map the embedded texture image onto a shape. */
    TextureMapping textureMapping;
    std::string image;
    std::string imagePath;
    /**
     * Optional secondary physically-based texture maps, embedded exactly like
     * `image` (base64 PNG). An empty string means the map is not used. They
     * are sampled with the same texture coordinates as the base image.
     */
    std::string roughnessImage;
    std::string roughnessImagePath;
    std::string normalImage;
    std::string normalImagePath;
    std::string emissiveImage;
    std::string emissiveImagePath;
    /** Multiplies the sampled roughness-map value (1 = as authored). */
    float roughnessStrength;
    /** Scales the tangent-space normal-map perturbation (1 = as authored). */
    float normalStrength;
    /** Scales the emissive-map contribution (1 = as authored). */
    float emissiveIntensity;
    /**
     * Dielectric optics of a physically based material with a transmissive
     * (glass) response. They are only meaningful when usePhysicalMaterial is
     * true and the material is transparent (transparency > 0); the viewport's
     * global glass settings are the fallback for materials without a physical
     * definition.
     */
    float transmissionIor;         /**< Index of refraction (1.5 = window glass). */
    float transmissionAbsorption;  /**< Beer-Lambert absorption (0 = clear). */
    std::string uuid;
    // NOLINTEND
    //@}

    bool operator==(const Material& m) const
    {
        // clang-format off
        if (!uuid.empty() && uuid == m.uuid) {
            return true;
        }
        return shininess == m.shininess
            && transparency == m.transparency
            && metallic == m.metallic
            && roughness == m.roughness
            && usePhysicalMaterial == m.usePhysicalMaterial
            && textureSize == m.textureSize
            && textureMapping == m.textureMapping
            && ambientColor == m.ambientColor
            && diffuseColor == m.diffuseColor
            && specularColor == m.specularColor
            && emissiveColor == m.emissiveColor
            && image == m.image
            && imagePath == m.imagePath
            && roughnessImage == m.roughnessImage
            && roughnessImagePath == m.roughnessImagePath
            && normalImage == m.normalImage
            && normalImagePath == m.normalImagePath
            && emissiveImage == m.emissiveImage
            && emissiveImagePath == m.emissiveImagePath
            && roughnessStrength == m.roughnessStrength
            && normalStrength == m.normalStrength
            && emissiveIntensity == m.emissiveIntensity
            && transmissionIor == m.transmissionIor
            && transmissionAbsorption == m.transmissionAbsorption;
        // clang-format on
    }
    bool operator!=(const Material& m) const
    {
        return !operator==(m);
    }
    Material& operator=(const Material& other) = default;
    Material& operator=(Material&& other) = default;

    static Material getDefaultAppearance();

private:
    MaterialType _matType;
};

}  // namespace App
