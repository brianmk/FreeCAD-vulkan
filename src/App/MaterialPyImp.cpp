// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2010 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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


// inclusion of the generated files (generated out of MaterialPy.xml)
#include "MaterialPy.h"

#include "MaterialPy.cpp"

#include <Base/PyWrapParseTupleAndKeywords.h>

using namespace App;

Base::Color MaterialPy::toColor(PyObject* value)
{
    Base::Color cCol;
    if (PyTuple_Check(value) && (PyTuple_Size(value) == 3 || PyTuple_Size(value) == 4)) {
        PyObject* item {};
        item = PyTuple_GetItem(value, 0);
        if (PyFloat_Check(item)) {
            cCol.r = (float)PyFloat_AsDouble(item);
            item = PyTuple_GetItem(value, 1);
            if (PyFloat_Check(item)) {
                cCol.g = (float)PyFloat_AsDouble(item);
            }
            else {
                throw Base::TypeError("Type in tuple must be consistent (float)");
            }
            item = PyTuple_GetItem(value, 2);
            if (PyFloat_Check(item)) {
                cCol.b = (float)PyFloat_AsDouble(item);
            }
            else {
                throw Base::TypeError("Type in tuple must be consistent (float)");
            }
            if (PyTuple_Size(value) == 4) {
                item = PyTuple_GetItem(value, 3);
                if (PyFloat_Check(item)) {
                    cCol.a = (float)PyFloat_AsDouble(item);
                }
                else {
                    throw Base::TypeError("Type in tuple must be consistent (float)");
                }
            }
        }
        else if (PyLong_Check(item)) {
            cCol.r = static_cast<float>(PyLong_AsLong(item)) / 255.0F;
            item = PyTuple_GetItem(value, 1);
            if (PyLong_Check(item)) {
                cCol.g = static_cast<float>(PyLong_AsLong(item)) / 255.0F;
            }
            else {
                throw Base::TypeError("Type in tuple must be consistent (integer)");
            }
            item = PyTuple_GetItem(value, 2);
            if (PyLong_Check(item)) {
                cCol.b = static_cast<float>(PyLong_AsLong(item)) / 255.0F;
            }
            else {
                throw Base::TypeError("Type in tuple must be consistent (integer)");
            }
            if (PyTuple_Size(value) == 4) {
                item = PyTuple_GetItem(value, 3);
                if (PyLong_Check(item)) {
                    cCol.a = static_cast<float>(PyLong_AsLong(item)) / 255.0F;
                }
                else {
                    throw Base::TypeError("Type in tuple must be consistent (integer)");
                }
            }
        }
        else {
            throw Base::TypeError("Type in tuple must be float or integer");
        }
    }
    else if (PyLong_Check(value)) {
        cCol.setPackedValue(PyLong_AsUnsignedLong(value));
    }
    else {
        std::string error =
            std::string("type must be integer or tuple of float or tuple integer, not ");
        error += value->ob_type->tp_name;
        throw Base::TypeError(error);
    }

    return cCol;
}

PyObject* MaterialPy::PyMake(struct _typeobject*, PyObject*, PyObject*)  // Python wrapper
{
    // create a new instance of MaterialPy and the Twin object
    return new MaterialPy(new Material);
}

// constructor method
int MaterialPy::PyInit(PyObject* args, PyObject* kwds)
{
    PyObject* diffuse = nullptr;
    PyObject* ambient = nullptr;
    PyObject* specular = nullptr;
    PyObject* emissive = nullptr;
    PyObject* shininess = nullptr;
    PyObject* transparency = nullptr;
    static const std::array<const char*, 7> kwds_colors {"DiffuseColor",
                                                         "AmbientColor",
                                                         "SpecularColor",
                                                         "EmissiveColor",
                                                         "Shininess",
                                                         "Transparency",
                                                         nullptr};

    if (!Base::Wrapped_ParseTupleAndKeywords(args,
                                             kwds,
                                             "|OOOOOO",
                                             kwds_colors,
                                             &diffuse,
                                             &ambient,
                                             &specular,
                                             &emissive,
                                             &shininess,
                                             &transparency)) {
        return -1;
    }

    try {
        if (diffuse) {
            setDiffuseColor(Py::Object(diffuse));
        }

        if (ambient) {
            setAmbientColor(Py::Object(ambient));
        }

        if (specular) {
            setSpecularColor(Py::Object(specular));
        }

        if (emissive) {
            setEmissiveColor(Py::Object(emissive));
        }

        if (shininess) {
            setShininess(Py::Float(shininess));
        }

        if (transparency) {
            setTransparency(Py::Float(transparency));
        }

        return 0;
    }
    catch (const Py::Exception&) {
        return -1;
    }
}

// returns a string which represents the object e.g. when printed in python
std::string MaterialPy::representation() const
{
    return {"<Material object>"};
}

namespace
{
bool equalMaterialValues(const Material& lhs, const Material& rhs)
{
    return lhs.shininess == rhs.shininess
        && lhs.transparency == rhs.transparency
        && lhs.metallic == rhs.metallic
        && lhs.roughness == rhs.roughness
        && lhs.usePhysicalMaterial == rhs.usePhysicalMaterial
        && lhs.textureSize == rhs.textureSize
        && lhs.textureMapping == rhs.textureMapping
        && lhs.ambientColor == rhs.ambientColor
        && lhs.diffuseColor == rhs.diffuseColor
        && lhs.specularColor == rhs.specularColor
        && lhs.emissiveColor == rhs.emissiveColor
        && lhs.image == rhs.image
        && lhs.imagePath == rhs.imagePath
        && lhs.roughnessStrength == rhs.roughnessStrength
        && lhs.normalStrength == rhs.normalStrength
        && lhs.emissiveIntensity == rhs.emissiveIntensity
        && lhs.transmissionIor == rhs.transmissionIor
        && lhs.transmissionAbsorption == rhs.transmissionAbsorption;
}
}  // namespace

PyObject* MaterialPy::richCompare(PyObject* v, PyObject* w, int op)
{
    if (PyObject_TypeCheck(v, &(MaterialPy::Type)) && PyObject_TypeCheck(w, &(MaterialPy::Type))) {
        const Material* m1 = static_cast<MaterialPy*>(v)->getMaterialPtr();
        const Material* m2 = static_cast<MaterialPy*>(w)->getMaterialPtr();

        const bool equal = equalMaterialValues(*m1, *m2);
        switch (op) {
            case Py_NE:
                return PyBool_FromLong(!equal);
            case Py_EQ:
                return PyBool_FromLong(equal);
            default:
                break;
        }
    }

    Py_RETURN_NOTIMPLEMENTED;
}

PyObject* MaterialPy::set(PyObject* args)
{
    char* pstr {};
    if (!PyArg_ParseTuple(args, "s", &pstr)) {
        return nullptr;
    }

    getMaterialPtr()->set(pstr);

    Py_Return;
}

Py::Object MaterialPy::getAmbientColor() const
{
    Py::Tuple tuple(4);
    tuple.setItem(0, Py::Float(getMaterialPtr()->ambientColor.r));
    tuple.setItem(1, Py::Float(getMaterialPtr()->ambientColor.g));
    tuple.setItem(2, Py::Float(getMaterialPtr()->ambientColor.b));
    tuple.setItem(3, Py::Float(getMaterialPtr()->ambientColor.a));
    return tuple;
}

void MaterialPy::setAmbientColor(Py::Object arg)
{
    try {
        getMaterialPtr()->ambientColor = toColor(*arg);
    }
    catch (const Base::Exception& e) {
        e.setPyException();
        throw Py::Exception();
    }
}

Py::Object MaterialPy::getDiffuseColor() const
{
    Py::Tuple tuple(4);
    tuple.setItem(0, Py::Float(getMaterialPtr()->diffuseColor.r));
    tuple.setItem(1, Py::Float(getMaterialPtr()->diffuseColor.g));
    tuple.setItem(2, Py::Float(getMaterialPtr()->diffuseColor.b));
    tuple.setItem(3, Py::Float(getMaterialPtr()->diffuseColor.a));
    return tuple;
}

void MaterialPy::setDiffuseColor(Py::Object arg)
{
    try {
        getMaterialPtr()->diffuseColor = toColor(*arg);
    }
    catch (const Base::Exception& e) {
        e.setPyException();
        throw Py::Exception();
    }
}

Py::Object MaterialPy::getEmissiveColor() const
{
    Py::Tuple tuple(4);
    tuple.setItem(0, Py::Float(getMaterialPtr()->emissiveColor.r));
    tuple.setItem(1, Py::Float(getMaterialPtr()->emissiveColor.g));
    tuple.setItem(2, Py::Float(getMaterialPtr()->emissiveColor.b));
    tuple.setItem(3, Py::Float(getMaterialPtr()->emissiveColor.a));
    return tuple;
}

void MaterialPy::setEmissiveColor(Py::Object arg)
{
    try {
        getMaterialPtr()->emissiveColor = toColor(*arg);
    }
    catch (const Base::Exception& e) {
        e.setPyException();
        throw Py::Exception();
    }
}

Py::Object MaterialPy::getSpecularColor() const
{
    Py::Tuple tuple(4);
    tuple.setItem(0, Py::Float(getMaterialPtr()->specularColor.r));
    tuple.setItem(1, Py::Float(getMaterialPtr()->specularColor.g));
    tuple.setItem(2, Py::Float(getMaterialPtr()->specularColor.b));
    tuple.setItem(3, Py::Float(getMaterialPtr()->specularColor.a));
    return tuple;
}

void MaterialPy::setSpecularColor(Py::Object arg)
{
    try {
        getMaterialPtr()->specularColor = toColor(*arg);
    }
    catch (const Base::Exception& e) {
        e.setPyException();
        throw Py::Exception();
    }
}

Py::Float MaterialPy::getShininess() const
{
    return Py::Float(getMaterialPtr()->shininess);
}

void MaterialPy::setShininess(Py::Float arg)
{
    getMaterialPtr()->shininess = arg;
}

Py::Float MaterialPy::getTransparency() const
{
    return Py::Float(getMaterialPtr()->transparency);
}

void MaterialPy::setTransparency(Py::Float arg)
{
    getMaterialPtr()->transparency = arg;
}

Py::Float MaterialPy::getMetallic() const
{
    return Py::Float(getMaterialPtr()->metallic);
}

void MaterialPy::setMetallic(Py::Float arg)
{
    getMaterialPtr()->metallic = arg;
}

Py::Float MaterialPy::getRoughness() const
{
    return Py::Float(getMaterialPtr()->roughness);
}

void MaterialPy::setRoughness(Py::Float arg)
{
    getMaterialPtr()->roughness = arg;
}

Py::Boolean MaterialPy::getUsePhysicalMaterial() const
{
    return Py::Boolean(getMaterialPtr()->usePhysicalMaterial);
}

void MaterialPy::setUsePhysicalMaterial(Py::Boolean arg)
{
    getMaterialPtr()->usePhysicalMaterial = arg;
}

Py::String MaterialPy::getImage() const
{
    return Py::String(getMaterialPtr()->image);
}

void MaterialPy::setImage(Py::String arg)
{
    getMaterialPtr()->image = static_cast<std::string>(arg);
}

Py::Float MaterialPy::getTextureSize() const
{
    return Py::Float(getMaterialPtr()->textureSize);
}

void MaterialPy::setTextureSize(Py::Float arg)
{
    getMaterialPtr()->textureSize = arg;
}

namespace
{
std::string textureMappingToString(Material::TextureMapping mapping)
{
    switch (mapping) {
        case Material::MappingBox:
            return "Box";
        case Material::MappingSpherical:
            return "Spherical";
        case Material::MappingCylindrical:
            return "Cylindrical";
        case Material::MappingPlanar:
        default:
            return "Planar";
    }
}

Material::TextureMapping textureMappingFromString(const std::string& value)
{
    if (value == "Box" || value == "box") {
        return Material::MappingBox;
    }
    if (value == "Spherical" || value == "spherical") {
        return Material::MappingSpherical;
    }
    if (value == "Cylindrical" || value == "cylindrical") {
        return Material::MappingCylindrical;
    }
    return Material::MappingPlanar;
}
}  // namespace

Py::String MaterialPy::getTextureMapping() const
{
    return Py::String(textureMappingToString(getMaterialPtr()->textureMapping));
}

void MaterialPy::setTextureMapping(Py::String arg)
{
    getMaterialPtr()->textureMapping = textureMappingFromString(static_cast<std::string>(arg));
}

Py::String MaterialPy::getImagePath() const
{
    return Py::String(getMaterialPtr()->imagePath);
}

void MaterialPy::setImagePath(Py::String arg)
{
    getMaterialPtr()->imagePath = static_cast<std::string>(arg);
}

Py::String MaterialPy::getRoughnessImage() const
{
    return Py::String(getMaterialPtr()->roughnessImage);
}

void MaterialPy::setRoughnessImage(Py::String arg)
{
    getMaterialPtr()->roughnessImage = static_cast<std::string>(arg);
}

Py::String MaterialPy::getRoughnessImagePath() const
{
    return Py::String(getMaterialPtr()->roughnessImagePath);
}

void MaterialPy::setRoughnessImagePath(Py::String arg)
{
    getMaterialPtr()->roughnessImagePath = static_cast<std::string>(arg);
}

Py::String MaterialPy::getNormalImage() const
{
    return Py::String(getMaterialPtr()->normalImage);
}

void MaterialPy::setNormalImage(Py::String arg)
{
    getMaterialPtr()->normalImage = static_cast<std::string>(arg);
}

Py::String MaterialPy::getNormalImagePath() const
{
    return Py::String(getMaterialPtr()->normalImagePath);
}

void MaterialPy::setNormalImagePath(Py::String arg)
{
    getMaterialPtr()->normalImagePath = static_cast<std::string>(arg);
}

Py::String MaterialPy::getEmissiveImage() const
{
    return Py::String(getMaterialPtr()->emissiveImage);
}

void MaterialPy::setEmissiveImage(Py::String arg)
{
    getMaterialPtr()->emissiveImage = static_cast<std::string>(arg);
}

Py::String MaterialPy::getEmissiveImagePath() const
{
    return Py::String(getMaterialPtr()->emissiveImagePath);
}

void MaterialPy::setEmissiveImagePath(Py::String arg)
{
    getMaterialPtr()->emissiveImagePath = static_cast<std::string>(arg);
}

Py::Float MaterialPy::getRoughnessStrength() const
{
    return Py::Float(getMaterialPtr()->roughnessStrength);
}

void MaterialPy::setRoughnessStrength(Py::Float arg)
{
    getMaterialPtr()->roughnessStrength = arg;
}

Py::Float MaterialPy::getNormalStrength() const
{
    return Py::Float(getMaterialPtr()->normalStrength);
}

void MaterialPy::setNormalStrength(Py::Float arg)
{
    getMaterialPtr()->normalStrength = arg;
}

Py::Float MaterialPy::getEmissiveIntensity() const
{
    return Py::Float(getMaterialPtr()->emissiveIntensity);
}

void MaterialPy::setEmissiveIntensity(Py::Float arg)
{
    getMaterialPtr()->emissiveIntensity = arg;
}

Py::Float MaterialPy::getTransmissionIor() const
{
    return Py::Float(getMaterialPtr()->transmissionIor);
}

void MaterialPy::setTransmissionIor(Py::Float arg)
{
    getMaterialPtr()->transmissionIor = arg;
}

Py::Float MaterialPy::getTransmissionAbsorption() const
{
    return Py::Float(getMaterialPtr()->transmissionAbsorption);
}

void MaterialPy::setTransmissionAbsorption(Py::Float arg)
{
    getMaterialPtr()->transmissionAbsorption = arg;
}

PyObject* MaterialPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int MaterialPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
