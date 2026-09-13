// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2011 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <Geom2d_BSplineCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <Base/GeometryPyCXX.h>
#include <Base/Tools2D.h>
#include <Base/Vector3D.h>
#include <Base/VectorPy.h>

#include "BSplineCurvePy.h"
#include "Geom2d/BSplineCurve2dPy.h"
#include "OCCError.h"


namespace Part
{
namespace BSplineCurvePyHelpers
{

/** Traits mapping the 3D and 2D B-spline Python wrappers onto the shared
 * accessor implementations below.
 */
template<class Self>
struct Traits;

template<>
struct Traits<BSplineCurvePy>
{
    using Curve = Geom_BSplineCurve;
    using Point = gp_Pnt;
    using Vector = Base::Vector3d;
    using ArrayOfPoints = TColgp_Array1OfPnt;

    static Handle(Curve) curve(BSplineCurvePy* self)
    {
        return Handle(Curve)::DownCast(self->getGeometryPtr()->handle());
    }
    static void setCurve(BSplineCurvePy* self, const Handle(Curve)& c)
    {
        self->getGeomBSplineCurvePtr()->setHandle(c);
    }
    static Point toPoint(const Vector& v)
    {
        return Point(v.x, v.y, v.z);
    }
    static Vector vectorFromPy(const Py::Object& o)
    {
        Py::Vector v(o);
        return v.toVector();
    }
    static Vector vectorFromPyObject(PyObject* p)
    {
        return static_cast<Base::VectorPy*>(p)->value();
    }
    static PyTypeObject* vectorType()
    {
        return &Base::VectorPy::Type;
    }
    static void insertKnot(Handle(Curve)& curve, double U, int M, double tol, bool add)
    {
        curve->InsertKnot(U, M, tol, add);
    }
    static PyObject* makePoint(const Point& p)
    {
        return new Base::VectorPy(Base::Vector3d(p.X(), p.Y(), p.Z()));
    }
    static Py::Tuple poleAndWeight(const Point& p, double w)
    {
        Py::Tuple t(4);
        t.setItem(0, Py::Float(p.X()));
        t.setItem(1, Py::Float(p.Y()));
        t.setItem(2, Py::Float(p.Z()));
        t.setItem(3, Py::Float(w));
        return t;
    }
    static Handle(Curve) make(
        const ArrayOfPoints& poles,
        const TColStd_Array1OfReal& knots,
        const TColStd_Array1OfInteger& mults,
        int degree,
        bool periodic
    )
    {
        return new Curve(poles, knots, mults, degree, periodic);
    }
};

template<>
struct Traits<BSplineCurve2dPy>
{
    using Curve = Geom2d_BSplineCurve;
    using Point = gp_Pnt2d;
    using Vector = Base::Vector2d;
    using ArrayOfPoints = TColgp_Array1OfPnt2d;

    static Handle(Curve) curve(BSplineCurve2dPy* self)
    {
        return Handle(Curve)::DownCast(self->getGeometry2dPtr()->handle());
    }
    static void setCurve(BSplineCurve2dPy* self, const Handle(Curve)& c)
    {
        self->getGeom2dBSplineCurvePtr()->setHandle(c);
    }
    static Point toPoint(const Vector& v)
    {
        return Point(v.x, v.y);
    }
    static Vector vectorFromPy(const Py::Object& o)
    {
        return Py::toVector2d(o);
    }
    static Vector vectorFromPyObject(PyObject* p)
    {
        return Py::toVector2d(p);
    }
    static PyTypeObject* vectorType()
    {
        return Base::Vector2dPy::type_object();
    }
    static void insertKnot(Handle(Curve)& curve, double U, int M, double tol, bool /*add*/)
    {
        curve->InsertKnot(U, M, tol);
    }
    static PyObject* makePoint(const Point& p)
    {
        return Py::new_reference_to(Base::Vector2dPy::create(p.X(), p.Y()));
    }
    static Py::Tuple poleAndWeight(const Point& p, double w)
    {
        Py::Tuple t(3);
        t.setItem(0, Py::Float(p.X()));
        t.setItem(1, Py::Float(p.Y()));
        t.setItem(2, Py::Float(w));
        return t;
    }
    static Handle(Curve) make(
        const ArrayOfPoints& poles,
        const TColStd_Array1OfReal& knots,
        const TColStd_Array1OfInteger& mults,
        int degree,
        bool periodic
    )
    {
        return new Curve(poles, knots, mults, degree, periodic);
    }
};

#define BS_CATCH                                    \
    catch (Standard_Failure& e)                     \
    {                                               \
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString()); \
        return nullptr;                             \
    }

template<class Self>
PyObject* isRational(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        return PyBool_FromLong(curve->IsRational() ? 1 : 0);
    }
    BS_CATCH
}

template<class Self>
PyObject* isPeriodic(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        return PyBool_FromLong(curve->IsPeriodic() ? 1 : 0);
    }
    BS_CATCH
}

template<class Self>
PyObject* isClosed(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        return PyBool_FromLong(curve->IsClosed() ? 1 : 0);
    }
    BS_CATCH
}

template<class Self>
PyObject* getKnot(Self* self, PyObject* args)
{
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        return Py_BuildValue("d", curve->Knot(index));
    }
    BS_CATCH
}

template<class Self>
PyObject* getKnots(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        TColStd_Array1OfReal w(1, curve->NbKnots());
        curve->Knots(w);
        Py::List knots;
        for (Standard_Integer i = w.Lower(); i <= w.Upper(); i++) {
            knots.append(Py::Float(w(i)));
        }
        return Py::new_reference_to(knots);
    }
    BS_CATCH
}

template<class Self>
PyObject* getPole(Self* self, PyObject* args)
{
    using T = Traits<Self>;
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    try {
        Handle(typename T::Curve) curve = T::curve(self);
        Standard_OutOfRange_Raise_if(index < 1 || index > curve->NbPoles(), "Pole index out of range");
        return T::makePoint(curve->Pole(index));
    }
    BS_CATCH
}

template<class Self>
PyObject* getPoles(Self* self, PyObject* args)
{
    using T = Traits<Self>;
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename T::Curve) curve = T::curve(self);
        typename T::ArrayOfPoints p(1, curve->NbPoles());
        curve->Poles(p);
        Py::List poles;
        for (Standard_Integer i = p.Lower(); i <= p.Upper(); i++) {
            poles.append(Py::asObject(T::makePoint(p(i))));
        }
        return Py::new_reference_to(poles);
    }
    BS_CATCH
}

template<class Self>
PyObject* getPolesAndWeights(Self* self, PyObject* args)
{
    using T = Traits<Self>;
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename T::Curve) curve = T::curve(self);
        typename T::ArrayOfPoints p(1, curve->NbPoles());
        curve->Poles(p);
        TColStd_Array1OfReal w(1, curve->NbPoles());
        curve->Weights(w);

        Py::List poles;
        for (Standard_Integer i = p.Lower(); i <= p.Upper(); i++) {
            poles.append(T::poleAndWeight(p(i), w(i)));
        }
        return Py::new_reference_to(poles);
    }
    BS_CATCH
}

template<class Self>
PyObject* getWeight(Self* self, PyObject* args)
{
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        Standard_OutOfRange_Raise_if(index < 1 || index > curve->NbPoles(), "Weight index out of range");
        return Py_BuildValue("d", curve->Weight(index));
    }
    BS_CATCH
}

template<class Self>
PyObject* getWeights(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        TColStd_Array1OfReal w(1, curve->NbPoles());
        curve->Weights(w);
        Py::List weights;
        for (Standard_Integer i = w.Lower(); i <= w.Upper(); i++) {
            weights.append(Py::Float(w(i)));
        }
        return Py::new_reference_to(weights);
    }
    BS_CATCH
}

template<class Self>
PyObject* getMultiplicity(Self* self, PyObject* args)
{
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        return Py_BuildValue("i", curve->Multiplicity(index));
    }
    BS_CATCH
}

template<class Self>
PyObject* getMultiplicities(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        TColStd_Array1OfInteger m(1, curve->NbKnots());
        curve->Multiplicities(m);
        Py::List mults;
        for (Standard_Integer i = m.Lower(); i <= m.Upper(); i++) {
            mults.append(Py::Long(m(i)));
        }
        return Py::new_reference_to(mults);
    }
    BS_CATCH
}

template<class Self>
PyObject* getResolution(Self* self, PyObject* args)
{
    double tol;
    if (!PyArg_ParseTuple(args, "d", &tol)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        double utol;
        curve->Resolution(tol, utol);
        return Py_BuildValue("d", utol);
    }
    BS_CATCH
}

template<class Self>
PyObject* buildFromPoles(Self* self, PyObject* args)
{
    using T = Traits<Self>;
    PyObject* obj;
    int degree = 3;
    PyObject* periodic = Py_False;
    PyObject* interpolate = Py_False;
    if (
        !PyArg_ParseTuple(
            args,
            "O|O!iO!",
            &obj,
            &PyBool_Type,
            &periodic,
            &degree,
            &PyBool_Type,
            interpolate
        )
    ) {
        return nullptr;
    }
    try {
        Py::Sequence list(obj);
        typename T::ArrayOfPoints poles(1, list.size());
        Standard_Integer index = 1;
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            poles(index++) = T::toPoint(T::vectorFromPy(*it));
        }

        if (poles.Length() <= degree) {
            degree = poles.Length() - 1;
        }

        Handle(typename T::Curve) spline;
        if (Base::asBoolean(periodic)) {
            int mult;
            int len;
            if (Base::asBoolean(interpolate)) {
                mult = degree;
                len = poles.Length() - mult + 2;
            }
            else {
                mult = 1;
                len = poles.Length() + 1;
            }
            TColStd_Array1OfReal knots(1, len);
            TColStd_Array1OfInteger mults(1, len);
            for (int i = 1; i <= knots.Length(); i++) {
                knots.SetValue(i, (double)(i - 1) / (knots.Length() - 1));
                mults.SetValue(i, 1);
            }
            mults.SetValue(1, mult);
            mults.SetValue(knots.Length(), mult);

            spline = T::make(poles, knots, mults, degree, true);
        }
        else {
            TColStd_Array1OfReal knots(1, poles.Length() + degree + 1 - 2 * (degree));
            TColStd_Array1OfInteger mults(1, poles.Length() + degree + 1 - 2 * (degree));
            for (int i = 1; i <= knots.Length(); i++) {
                knots.SetValue(i, (double)(i - 1) / (knots.Length() - 1));
                mults.SetValue(i, 1);
            }
            mults.SetValue(1, degree + 1);
            mults.SetValue(knots.Length(), degree + 1);

            spline = T::make(poles, knots, mults, degree, false);
        }

        if (spline.IsNull()) {
            throw Standard_Failure("failed to create spline");
        }
        T::setCurve(self, spline);
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* increaseDegree(Self* self, PyObject* args)
{
    int degree;
    if (!PyArg_ParseTuple(args, "i", &degree)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->IncreaseDegree(degree);
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* increaseMultiplicity(Self* self, PyObject* args)
{
    int mult = -1;
    int start, end;
    if (!PyArg_ParseTuple(args, "ii|i", &start, &end, &mult)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        if (mult == -1) {
            mult = end;
            curve->IncreaseMultiplicity(start, mult);
        }
        else {
            curve->IncreaseMultiplicity(start, end, mult);
        }
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* incrementMultiplicity(Self* self, PyObject* args)
{
    int start, end, mult;
    if (!PyArg_ParseTuple(args, "iii", &start, &end, &mult)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->IncrementMultiplicity(start, end, mult);
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* insertKnot(Self* self, PyObject* args)
{
    double U, tol = 0.0;
    int M = 1;
    PyObject* add = Py_True;
    if (!PyArg_ParseTuple(args, "d|idO!", &U, &M, &tol, &PyBool_Type, &add)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        Traits<Self>::insertKnot(curve, U, M, tol, Base::asBoolean(add));
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* insertKnots(Self* self, PyObject* args)
{
    double tol = 0.0;
    PyObject* add = Py_True;
    PyObject* obj1;
    PyObject* obj2;
    if (!PyArg_ParseTuple(args, "OO|dO!", &obj1, &obj2, &tol, &PyBool_Type, &add)) {
        return nullptr;
    }
    try {
        Py::Sequence knots(obj1);
        TColStd_Array1OfReal k(1, knots.size());
        int index = 1;
        for (Py::Sequence::iterator it = knots.begin(); it != knots.end(); ++it) {
            Py::Float val(*it);
            k(index++) = (double)val;
        }
        Py::Sequence mults(obj2);
        TColStd_Array1OfInteger m(1, mults.size());
        index = 1;
        for (Py::Sequence::iterator it = mults.begin(); it != mults.end(); ++it) {
            Py::Long val(*it);
            m(index++) = (int)val;
        }

        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->InsertKnots(k, m, tol, Base::asBoolean(add));
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* removeKnot(Self* self, PyObject* args)
{
    double tol;
    int index, M;
    if (!PyArg_ParseTuple(args, "iid", &index, &M, &tol)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        Standard_Boolean ok = curve->RemoveKnot(index, M, tol);
        return PyBool_FromLong(ok ? 1 : 0);
    }
    BS_CATCH
}

template<class Self>
PyObject* setKnot(Self* self, PyObject* args)
{
    int index, M = -1;
    double K;
    if (!PyArg_ParseTuple(args, "id|i", &index, &K, &M)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        if (M == -1) {
            curve->SetKnot(index, K);
        }
        else {
            curve->SetKnot(index, K, M);
        }
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* setKnots(Self* self, PyObject* args)
{
    PyObject* obj;
    if (!PyArg_ParseTuple(args, "O", &obj)) {
        return nullptr;
    }
    try {
        Py::Sequence list(obj);
        TColStd_Array1OfReal k(1, list.size());
        int index = 1;
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            Py::Float val(*it);
            k(index++) = (double)val;
        }

        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->SetKnots(k);
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* setPole(Self* self, PyObject* args)
{
    using T = Traits<Self>;
    int index;
    double weight = -1.0;
    PyObject* p;
    if (!PyArg_ParseTuple(args, "iO!|d", &index, T::vectorType(), &p, &weight)) {
        return nullptr;
    }
    try {
        typename T::Point pnt = T::toPoint(T::vectorFromPyObject(p));
        Handle(typename T::Curve) curve = T::curve(self);
        if (weight < 0.0) {
            curve->SetPole(index, pnt);
        }
        else {
            curve->SetPole(index, pnt, weight);
        }
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* setWeight(Self* self, PyObject* args)
{
    int index;
    double weight;
    if (!PyArg_ParseTuple(args, "id", &index, &weight)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->SetWeight(index, weight);
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* movePoint(Self* self, PyObject* args)
{
    using T = Traits<Self>;
    double U;
    int index1, index2;
    PyObject* pnt;
    if (!PyArg_ParseTuple(args, "dO!ii", &U, T::vectorType(), &pnt, &index1, &index2)) {
        return nullptr;
    }
    try {
        typename T::Vector p = T::vectorFromPyObject(pnt);
        Handle(typename T::Curve) curve = T::curve(self);
        int first, last;
        curve->MovePoint(U, T::toPoint(p), index1, index2, first, last);
        return Py_BuildValue("(ii)", first, last);
    }
    BS_CATCH
}

template<class Self>
PyObject* setNotPeriodic(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->SetNotPeriodic();
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* setPeriodic(Self* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->SetPeriodic();
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

template<class Self>
PyObject* setOrigin(Self* self, PyObject* args)
{
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    try {
        Handle(typename Traits<Self>::Curve) curve = Traits<Self>::curve(self);
        curve->SetOrigin(index);
        return Py::new_reference_to(Py::None());
    }
    BS_CATCH
}

#undef BS_CATCH

}  // namespace BSplineCurvePyHelpers
}  // namespace Part