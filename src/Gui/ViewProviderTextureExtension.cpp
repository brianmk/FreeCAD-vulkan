// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2024 David Carter <dcarter@david.carter.ca>             *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/


#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Inventor/nodes/SoTexture2.h>
#include <Inventor/nodes/SoTexture3.h>
#include <Inventor/nodes/SoTextureUnit.h>


#include "ViewProviderTextureExtension.h"
#include <Gui/BitmapFactory.h>
#include <App/Material.h>


using namespace Gui;


EXTENSION_PROPERTY_SOURCE(Gui::ViewProviderTextureExtension, Gui::ViewProviderExtension)


ViewProviderTextureExtension::ViewProviderTextureExtension()
{
    initExtensionType(ViewProviderTextureExtension::getExtensionClassTypeId());

    pcSwitchAppearance = new SoSwitch;
    pcSwitchAppearance->ref();
    pcSwitchAppearance->setName("SwitchAppearance");
    pcSwitchTexture = new SoSwitch;
    pcSwitchTexture->ref();
    pcSwitchTexture->setName("SwitchTexture");

    pcTextureGroup2D = new SoGroup;
    pcTextureGroup2D->ref();
    pcTextureGroup2D->setName("TextureGroup2D");

    pcShapeTexture2D = new SoTexture2;
    pcShapeTexture2D->ref();
    pcShapeTexture2D->setName("ShapeTexture2D");

    // Optional PBR maps.  Each map is wrapped in an SoTextureUnit so the
    // subsequent SoTexture2 is bound to unit 1/2/3 rather than the base unit 0.
    pcMapGroup = new SoGroup;
    pcMapGroup->ref();
    pcMapGroup->setName("PhysicalMapGroup");
    for (int i = 0; i < 3; ++i) {
        auto unit = new SoTextureUnit;
        unit->ref();
        unit->unit.setValue(i + 1);
        pcMapUnits[i] = unit;
        auto texture = new SoTexture2;
        texture->ref();
        pcMapTextures[i] = texture;
        pcMapGroup->addChild(unit);
        pcMapGroup->addChild(texture);
    }

    pcTextureGroup3D = new SoGroup;
    pcTextureGroup3D->ref();
    pcTextureGroup3D->setName("TextureGroup3D");
}

void ViewProviderTextureExtension::setup(SoMaterial* pcShapeMaterial)
{
    // Materials go first, with textured faces drawing over them
    pcSwitchAppearance->addChild(pcShapeMaterial);
    pcSwitchAppearance->addChild(pcSwitchTexture);
    pcTextureGroup2D->addChild(pcShapeTexture2D);
    pcTextureGroup2D->addChild(pcMapGroup);
    pcSwitchTexture->addChild(pcTextureGroup2D);
    pcSwitchTexture->addChild(pcTextureGroup3D);
    pcSwitchAppearance->whichChild.setValue(0);
    pcSwitchTexture->whichChild.setValue(SO_SWITCH_NONE);
}

ViewProviderTextureExtension::~ViewProviderTextureExtension()
{
    pcSwitchAppearance->unref();
    pcSwitchTexture->unref();
    pcTextureGroup2D->unref();
    pcShapeTexture2D->unref();
    for (int i = 0; i < 3; ++i) {
        pcMapUnits[i]->unref();
        pcMapTextures[i]->unref();
    }
    pcMapGroup->unref();
    pcTextureGroup3D->unref();
}

SoSwitch* ViewProviderTextureExtension::getAppearance() const
{
    return pcSwitchAppearance;
}

SoGroup* ViewProviderTextureExtension::getTextureGroup3D() const
{
    return pcTextureGroup3D;
}

void ViewProviderTextureExtension::setCoinAppearance(
    SoMaterial* pcShapeMaterial,
    const App::Material& source
)
{
    const bool hasBaseImage = !source.image.empty();
    const bool hasMapImage = !source.roughnessImage.empty()
        || !source.normalImage.empty() || !source.emissiveImage.empty();

    if (hasBaseImage) {
        activateTexture2D();

        QByteArray by = QByteArray::fromBase64(QString::fromStdString(source.image).toUtf8());
        auto image = QImage::fromData(by, "PNG");  //.scaled(64, 64, Qt::KeepAspectRatio);

        SoSFImage texture;
        Gui::BitmapFactory().convert(image, texture);
        pcShapeTexture2D->image = texture;
    }
    else {
        // Clear any base texture from a previous appearance so unit 0 stays
        // disabled; the optional PBR maps are still shown when present.
        pcShapeTexture2D->image = SoSFImage();
        if (hasMapImage) {
            activateTexture2D();
        }
        else {
            activateMaterial();
        }
    }

    // Optional PBR maps (roughness/normal/emissive) on texture units 1-3.
    setMapImage(pcMapTextures[0], source.roughnessImage);
    setMapImage(pcMapTextures[1], source.normalImage);
    setMapImage(pcMapTextures[2], source.emissiveImage);

    // Always set the material for items such as lines that don't support textures
    pcShapeMaterial->ambientColor
        .setValue(source.ambientColor.r, source.ambientColor.g, source.ambientColor.b);
    pcShapeMaterial->diffuseColor
        .setValue(source.diffuseColor.r, source.diffuseColor.g, source.diffuseColor.b);
    pcShapeMaterial->specularColor
        .setValue(source.specularColor.r, source.specularColor.g, source.specularColor.b);
    pcShapeMaterial->emissiveColor
        .setValue(source.emissiveColor.r, source.emissiveColor.g, source.emissiveColor.b);
    pcShapeMaterial->shininess.setValue(source.shininess);
    pcShapeMaterial->transparency.setValue(source.transparency);
}

void ViewProviderTextureExtension::setMapImage(SoTexture2* texture,
                                               const std::string& base64Image)
{
    if (base64Image.empty()) {
        texture->image = SoSFImage();
        return;
    }
    QByteArray by = QByteArray::fromBase64(
        QString::fromStdString(base64Image).toUtf8());
    QImage image = QImage::fromData(by, "PNG");
    if (image.isNull()) {
        texture->image = SoSFImage();
        return;
    }
    SoSFImage converted;
    Gui::BitmapFactory().convert(image, converted);
    texture->image = converted;
}

void ViewProviderTextureExtension::activateMaterial()
{
    pcSwitchAppearance->whichChild.setValue(0);
    pcSwitchTexture->whichChild.setValue(SO_SWITCH_NONE);
}

void ViewProviderTextureExtension::activateTexture2D()
{
    pcSwitchAppearance->whichChild.setValue(1);
    pcSwitchTexture->whichChild.setValue(0);
}

void ViewProviderTextureExtension::activateTexture3D()
{
    pcSwitchAppearance->whichChild.setValue(1);
    pcSwitchTexture->whichChild.setValue(1);
}

void ViewProviderTextureExtension::activateMixed3D()
{
    pcSwitchAppearance->whichChild.setValue(SO_SWITCH_ALL);
    pcSwitchTexture->whichChild.setValue(1);
}

// ------------------------------------------------------------------------------------------------

EXTENSION_PROPERTY_SOURCE(Gui::ViewProviderFaceTexture, Gui::ViewProviderTextureExtension)


ViewProviderFaceTexture::ViewProviderFaceTexture()
{
    initExtensionType(ViewProviderFaceTexture::getExtensionClassTypeId());

    // Support for textured faces
    pcShapeTexture3D = new SoTexture3;
    pcShapeTexture3D->ref();
    pcShapeTexture3D->setName("ShapeTexture3D");
    pcShapeCoordinates = new SoCoordinate3;
    pcShapeCoordinates->ref();
    pcShapeCoordinates->setName("ShapeCoordinates");
    pcShapeFaceset = new SoIndexedFaceSet;
    pcShapeFaceset->ref();
    pcShapeFaceset->setName("ShapeFaceset");
}

ViewProviderFaceTexture::~ViewProviderFaceTexture()
{
    pcShapeTexture3D->unref();
    pcShapeCoordinates->unref();
    pcShapeFaceset->unref();
}

void ViewProviderFaceTexture::setup(SoMaterial* mat)
{
    ViewProviderTextureExtension::setup(mat);

    getTextureGroup3D()->addChild(pcShapeTexture3D);
    getTextureGroup3D()->addChild(pcShapeCoordinates);
    getTextureGroup3D()->addChild(pcShapeFaceset);
}
