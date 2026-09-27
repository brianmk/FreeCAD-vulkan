// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2006 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <App/PropertyStandard.h>

#include "Dialogs/DlgMaterialPropertiesImp.h"
#include "ui_DlgMaterialProperties.h"
#include "ViewProvider.h"

#include <Base/Tools.h>

#include <QCheckBox>
#include <QDoubleSpinBox>


using namespace Gui::Dialog;


/* TRANSLATOR Gui::Dialog::DlgMaterialPropertiesImp */

DlgMaterialPropertiesImp::DlgMaterialPropertiesImp(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui_DlgMaterialProperties)
{
    ui->setupUi(this);
    setupConnections();

    ui->ambientColor->setAutoChangeColor(true);
    ui->diffuseColor->setAutoChangeColor(true);
    ui->emissiveColor->setAutoChangeColor(true);
    ui->specularColor->setAutoChangeColor(true);
}

DlgMaterialPropertiesImp::~DlgMaterialPropertiesImp() = default;

void DlgMaterialPropertiesImp::setupConnections()
{
    // clang-format off
    connect(ui->ambientColor, &ColorButton::changed,
            this, &DlgMaterialPropertiesImp::onAmbientColorChanged);
    connect(ui->diffuseColor, &ColorButton::changed,
            this, &DlgMaterialPropertiesImp::onDiffuseColorChanged);
    connect(ui->emissiveColor, &ColorButton::clicked,
            this, &DlgMaterialPropertiesImp::onEmissiveColorChanged);
    connect(ui->specularColor, &ColorButton::clicked,
            this, &DlgMaterialPropertiesImp::onSpecularColorChanged);
    connect(ui->shininess, qOverload<int>(&QSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onShininessValueChanged);
    connect(ui->transparency, qOverload<int>(&QSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onTransparencyValueChanged);
    connect(ui->usePhysicalMaterial, &QCheckBox::toggled,
            this, &DlgMaterialPropertiesImp::onPhysicalMaterialToggled);
    connect(ui->metallic, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onMetallicChanged);
    connect(ui->roughness, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onRoughnessChanged);
    connect(ui->transmissionIor, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onTransmissionIorChanged);
    connect(ui->transmissionAbsorption, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onTransmissionAbsorptionChanged);
    connect(ui->textureSize, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &DlgMaterialPropertiesImp::onTextureSizeChanged);
    connect(ui->textureMapping, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DlgMaterialPropertiesImp::onTextureMappingChanged);
    connect(ui->buttonReset, &QPushButton::clicked,
            this, &DlgMaterialPropertiesImp::onButtonReset);
    connect(ui->buttonDefault, &QPushButton::clicked,
            this, &DlgMaterialPropertiesImp::onButtonDefault);
    // clang-format on
}

void DlgMaterialPropertiesImp::setCustomMaterial(const App::Material& mat)
{
    customMaterial = mat;
    setButtonColors(customMaterial);
}

App::Material DlgMaterialPropertiesImp::getCustomMaterial() const
{
    return customMaterial;
}

void DlgMaterialPropertiesImp::setDefaultMaterial(const App::Material& mat)
{
    defaultMaterial = mat;
}

App::Material DlgMaterialPropertiesImp::getDefaultMaterial() const
{
    return defaultMaterial;
}

/**
 * Sets the ambient color.
 */
void DlgMaterialPropertiesImp::onAmbientColorChanged()
{
    customMaterial.ambientColor.setValue(ui->ambientColor->color());
}

/**
 * Sets the diffuse color.
 */
void DlgMaterialPropertiesImp::onDiffuseColorChanged()
{
    customMaterial.diffuseColor.setValue(ui->diffuseColor->color());
}

/**
 * Sets the emissive color.
 */
void DlgMaterialPropertiesImp::onEmissiveColorChanged()
{
    customMaterial.emissiveColor.setValue(ui->emissiveColor->color());
}

/**
 * Sets the specular color.
 */
void DlgMaterialPropertiesImp::onSpecularColorChanged()
{
    customMaterial.specularColor.setValue(ui->specularColor->color());
}

/**
 * Sets the current shininess.
 */
void DlgMaterialPropertiesImp::onShininessValueChanged(int sh)
{
    customMaterial.shininess = Base::fromPercent(sh);
}

/**
 * Sets the current transparency.
 */
void DlgMaterialPropertiesImp::onTransparencyValueChanged(int sh)
{
    customMaterial.transparency = Base::fromPercent(sh);
}

/**
 * Enables or disables the physical (metallic-roughness) appearance path.
 */
void DlgMaterialPropertiesImp::onPhysicalMaterialToggled(bool on)
{
    customMaterial.usePhysicalMaterial = on;
    ui->metallic->setEnabled(on);
    ui->roughness->setEnabled(on);
    ui->transmissionIor->setEnabled(on);
    ui->transmissionAbsorption->setEnabled(on);
}

/**
 * Sets the metallic amount of the physical material.
 */
void DlgMaterialPropertiesImp::onMetallicChanged(double value)
{
    customMaterial.metallic = static_cast<float>(value);
}

/**
 * Sets the roughness (surface finish) of the physical material.
 */
void DlgMaterialPropertiesImp::onRoughnessChanged(double value)
{
    customMaterial.roughness = static_cast<float>(value);
}

/**
 * Sets the index of refraction of the dielectric (glass) response.
 */
void DlgMaterialPropertiesImp::onTransmissionIorChanged(double value)
{
    customMaterial.transmissionIor = static_cast<float>(value);
}

/**
 * Sets the Beer-Lambert absorption strength of the dielectric response.
 */
void DlgMaterialPropertiesImp::onTransmissionAbsorptionChanged(double value)
{
    customMaterial.transmissionAbsorption = static_cast<float>(value);
}

/**
 * Sets the real-world size spanned by one tile of the texture image.
 */
void DlgMaterialPropertiesImp::onTextureSizeChanged(double value)
{
    customMaterial.textureSize = static_cast<float>(value);
}

/**
 * Sets the projection used to map the texture image onto the shape.
 */
void DlgMaterialPropertiesImp::onTextureMappingChanged(int index)
{
    customMaterial.textureMapping = static_cast<App::Material::TextureMapping>(index);
}

/**
 * Reset the colors to the Coin3D defaults
 */
void DlgMaterialPropertiesImp::onButtonReset()
{
    setCustomMaterial(getDefaultMaterial());
}

/**
 * Reset the colors to the current default
 */
void DlgMaterialPropertiesImp::onButtonDefault()
{
    App::Material mat = App::Material::getDefaultAppearance();
    setCustomMaterial(mat);
}

/**
 * Sets the button colors to match the current material settings.
 */
void DlgMaterialPropertiesImp::setButtonColors(const App::Material& mat)
{
    ui->ambientColor->setColor(mat.ambientColor.asValue<QColor>());
    ui->diffuseColor->setColor(mat.diffuseColor.asValue<QColor>());
    ui->emissiveColor->setColor(mat.emissiveColor.asValue<QColor>());
    ui->specularColor->setColor(mat.specularColor.asValue<QColor>());
    ui->shininess->blockSignals(true);
    ui->shininess->setValue((int)(100.0F * (mat.shininess + 0.001F)));
    ui->shininess->blockSignals(false);
    ui->transparency->blockSignals(true);
    ui->transparency->setValue((int)(100.0F * (mat.transparency + 0.001F)));
    ui->transparency->blockSignals(false);
    ui->usePhysicalMaterial->blockSignals(true);
    ui->usePhysicalMaterial->setChecked(mat.usePhysicalMaterial);
    ui->usePhysicalMaterial->blockSignals(false);
    ui->metallic->blockSignals(true);
    ui->metallic->setValue(static_cast<double>(mat.metallic));
    ui->metallic->blockSignals(false);
    ui->roughness->blockSignals(true);
    ui->roughness->setValue(static_cast<double>(mat.roughness));
    ui->roughness->blockSignals(false);
    ui->transmissionIor->blockSignals(true);
    ui->transmissionIor->setValue(static_cast<double>(mat.transmissionIor));
    ui->transmissionIor->blockSignals(false);
    ui->transmissionAbsorption->blockSignals(true);
    ui->transmissionAbsorption->setValue(static_cast<double>(mat.transmissionAbsorption));
    ui->transmissionAbsorption->blockSignals(false);
    ui->metallic->setEnabled(mat.usePhysicalMaterial);
    ui->roughness->setEnabled(mat.usePhysicalMaterial);
    ui->transmissionIor->setEnabled(mat.usePhysicalMaterial);
    ui->transmissionAbsorption->setEnabled(mat.usePhysicalMaterial);
    ui->textureSize->blockSignals(true);
    ui->textureSize->setValue(static_cast<double>(mat.textureSize));
    ui->textureSize->blockSignals(false);
    ui->textureMapping->blockSignals(true);
    ui->textureMapping->setCurrentIndex(static_cast<int>(mat.textureMapping));
    ui->textureMapping->blockSignals(false);
}

#include "moc_DlgMaterialPropertiesImp.cpp"
