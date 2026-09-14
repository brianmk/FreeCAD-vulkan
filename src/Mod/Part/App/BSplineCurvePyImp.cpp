// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <cmath>
#include <Geom_BSplineCurve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomConvert_BSplineCurveToBezierCurve.hxx>
#include <gp_Pnt.hxx>
#include <Precision.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array1OfVec.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_HArray1OfBoolean.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <Standard_Version.hxx>


#include <Base/GeometryPyCXX.h>
#include <Base/PyWrapParseTupleAndKeywords.h>
#include <Base/VectorPy.h>

#include "BSplineCurvePy.h"
#include "BSplineCurvePyHelpers.h"
#include "BSplineCurvePy.cpp"
#include "BezierCurvePy.h"
#include "OCCError.h"


using namespace Part;

// returns a string which represents the object e.g. when printed in python
std::string BSplineCurvePy::representation() const
{
    return "<BSplineCurve object>";
}

PyObject* BSplineCurvePy::PyMake(struct _typeobject*, PyObject*, PyObject*)  // Python wrapper
{
    // create a new instance of BSplineCurvePy and the Twin object
    return new BSplineCurvePy(new GeomBSplineCurve);
}

// constructor method
int BSplineCurvePy::PyInit(PyObject* args, PyObject* kwd)
{
    if (PyArg_ParseTuple(args, "")) {
        return 0;
    }

    PyErr_Clear();
    PyObject* obj;
    // poles, [ periodic, degree, interpolate ]
    // {"poles", "mults", "knots", "periodic", "degree", "weights", "CheckRational", NULL};
    obj = buildFromPolesMultsKnots(args, kwd);

    if (obj) {
        Py_DECREF(obj);
        return 0;
    }
    else if (PyErr_ExceptionMatches(PartExceptionOCCError)) {
        return -1;
    }

    PyErr_SetString(
        PyExc_TypeError,
        "B-spline constructor accepts:\n"
        "-- poles, [ periodic, degree, interpolate ]\n"
        "-- empty parameter list\n"
    );
    return -1;
}

PyObject* BSplineCurvePy::__reduce__(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    Py::Tuple tuple(2);

    // type object to create an instance
    Py::Object type(Base::getTypeAsObject(&BSplineCurvePy::Type));
    tuple.setItem(0, type);

    // create an argument tuple to create a copy
    Py::Object self(const_cast<BSplineCurvePy*>(this));
    Py::Tuple data(7);
    data.setItem(0, Py::Callable(self.getAttr("getPoles")).apply());
    data.setItem(1, Py::Callable(self.getAttr("getMultiplicities")).apply());
    data.setItem(2, Py::Callable(self.getAttr("getKnots")).apply());
    data.setItem(3, Py::Callable(self.getAttr("isPeriodic")).apply());
    data.setItem(4, self.getAttr("Degree"));
    data.setItem(5, Py::Callable(self.getAttr("getWeights")).apply());
    data.setItem(6, Py::Callable(self.getAttr("isRational")).apply());
    tuple.setItem(1, data);

    return Py::new_reference_to(tuple);
}

PyObject* BSplineCurvePy::isRational(PyObject* args) const
{
    return BSplineCurvePyHelpers::isRational(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::isPeriodic(PyObject* args) const
{
    return BSplineCurvePyHelpers::isPeriodic(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::isClosed(PyObject* args) const
{
    return BSplineCurvePyHelpers::isClosed(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::increaseDegree(PyObject* args){
    return BSplineCurvePyHelpers::increaseDegree(this, args);
}

PyObject* BSplineCurvePy::increaseMultiplicity(PyObject* args){
    return BSplineCurvePyHelpers::increaseMultiplicity(this, args);
}

PyObject* BSplineCurvePy::incrementMultiplicity(PyObject* args){
    return BSplineCurvePyHelpers::incrementMultiplicity(this, args);
}

PyObject* BSplineCurvePy::insertKnot(PyObject* args){
    return BSplineCurvePyHelpers::insertKnot(this, args);
}

PyObject* BSplineCurvePy::insertKnots(PyObject* args){
    return BSplineCurvePyHelpers::insertKnots(this, args);
}

PyObject* BSplineCurvePy::removeKnot(PyObject* args){
    return BSplineCurvePyHelpers::removeKnot(this, args);
}

PyObject* BSplineCurvePy::segment(PyObject* args)
{
    double u1, u2;
    if (!PyArg_ParseTuple(args, "dd", &u1, &u2)) {
        return nullptr;
    }
    try {
        Handle(Geom_BSplineCurve)
            curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
        Handle(Geom_BSplineCurve) tempCurve = Handle(Geom_BSplineCurve)::DownCast(curve->Copy());
        tempCurve->Segment(u1, u2);
        if (std::abs(tempCurve->FirstParameter() - u1) > Precision::Approximation()
            || std::abs(tempCurve->LastParameter() - u2) > Precision::Approximation()) {
            throw Standard_Failure("Failed to segment BSpline curve");
            return nullptr;
        }
        else {
            curve->Segment(u1, u2);
        }
        Py_Return;
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::split(PyObject* args) const
{
    double u {};
    double tolerance = 0.0;
    if (!PyArg_ParseTuple(args, "d|d", &u, &tolerance)) {
        return nullptr;
    }
    try {
        auto curves = getGeomBSplineCurvePtr()->split(u, tolerance);
        Py::Tuple tuple(2);
        tuple.setItem(0, Py::asObject(std::get<0>(curves)->getPyObject()));
        tuple.setItem(1, Py::asObject(std::get<1>(curves)->getPyObject()));
        return Py::new_reference_to(tuple);
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::setKnot(PyObject* args){
    return BSplineCurvePyHelpers::setKnot(this, args);
}

PyObject* BSplineCurvePy::getKnot(PyObject* args) const
{
    return BSplineCurvePyHelpers::getKnot(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::setKnots(PyObject* args){
    return BSplineCurvePyHelpers::setKnots(this, args);
}

PyObject* BSplineCurvePy::getKnots(PyObject* args) const
{
    return BSplineCurvePyHelpers::getKnots(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::setPole(PyObject* args){
    return BSplineCurvePyHelpers::setPole(this, args);
}

PyObject* BSplineCurvePy::getPole(PyObject* args) const
{
    return BSplineCurvePyHelpers::getPole(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::getPoles(PyObject* args) const
{
    return BSplineCurvePyHelpers::getPoles(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::getPolesAndWeights(PyObject* args) const
{
    return BSplineCurvePyHelpers::getPolesAndWeights(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::setWeight(PyObject* args){
    return BSplineCurvePyHelpers::setWeight(this, args);
}

PyObject* BSplineCurvePy::getWeight(PyObject* args) const
{
    return BSplineCurvePyHelpers::getWeight(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::getWeights(PyObject* args) const
{
    return BSplineCurvePyHelpers::getWeights(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::getResolution(PyObject* args) const
{
    return BSplineCurvePyHelpers::getResolution(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::movePoint(PyObject* args){
    return BSplineCurvePyHelpers::movePoint(this, args);
}

PyObject* BSplineCurvePy::setNotPeriodic(PyObject* args){
    return BSplineCurvePyHelpers::setNotPeriodic(this, args);
}

PyObject* BSplineCurvePy::setPeriodic(PyObject* args){
    return BSplineCurvePyHelpers::setPeriodic(this, args);
}

PyObject* BSplineCurvePy::setOrigin(PyObject* args){
    return BSplineCurvePyHelpers::setOrigin(this, args);
}

PyObject* BSplineCurvePy::getMultiplicity(PyObject* args) const
{
    return BSplineCurvePyHelpers::getMultiplicity(const_cast<BSplineCurvePy*>(this), args);
}

PyObject* BSplineCurvePy::getMultiplicities(PyObject* args) const
{
    return BSplineCurvePyHelpers::getMultiplicities(const_cast<BSplineCurvePy*>(this), args);
}
Py::Long BSplineCurvePy::getDegree() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    return Py::Long(curve->Degree());
}

Py::Long BSplineCurvePy::getMaxDegree() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    return Py::Long(curve->MaxDegree());
}

Py::Long BSplineCurvePy::getNbPoles() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    return Py::Long(curve->NbPoles());
}

Py::Long BSplineCurvePy::getNbKnots() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    return Py::Long(curve->NbKnots());
}

Py::Object BSplineCurvePy::getStartPoint() const
{
    Handle(Geom_BSplineCurve) c = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    gp_Pnt pnt = c->StartPoint();
    return Py::Vector(Base::Vector3d(pnt.X(), pnt.Y(), pnt.Z()));
}

Py::Object BSplineCurvePy::getEndPoint() const
{
    Handle(Geom_BSplineCurve) c = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    gp_Pnt pnt = c->EndPoint();
    return Py::Vector(Base::Vector3d(pnt.X(), pnt.Y(), pnt.Z()));
}

Py::Long BSplineCurvePy::getFirstUKnotIndex() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    return Py::Long(curve->FirstUKnotIndex());
}

Py::Long BSplineCurvePy::getLastUKnotIndex() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    return Py::Long(curve->LastUKnotIndex());
}

Py::List BSplineCurvePy::getKnotSequence() const
{
    Handle(Geom_BSplineCurve) curve = Handle(Geom_BSplineCurve)::DownCast(getGeometryPtr()->handle());
    Standard_Integer m = 0;
    if (curve->IsPeriodic()) {
        // knots=poles+2*degree-mult(1)+2
        m = curve->NbPoles() + 2 * curve->Degree() - curve->Multiplicity(1) + 2;
    }
    else {
        // knots=poles+degree+1
        for (int i = 1; i <= curve->NbKnots(); i++) {
            m += curve->Multiplicity(i);
        }
    }

    TColStd_Array1OfReal k(1, m);
    curve->KnotSequence(k);
    Py::List list;
    for (Standard_Integer i = k.Lower(); i <= k.Upper(); i++) {
        list.append(Py::Float(k(i)));
    }
    return list;
}

PyObject* BSplineCurvePy::toBiArcs(PyObject* args) const
{
    double tolerance = 0.001;
    if (!PyArg_ParseTuple(args, "d", &tolerance)) {
        return nullptr;
    }
    try {
        GeomBSplineCurve* curve = getGeomBSplineCurvePtr();
        std::list<Geometry*> arcs;
        arcs = curve->toBiArcs(tolerance);

        Py::List list;
        for (auto arc : arcs) {
            list.append(Py::asObject(arc->getPyObject()));
            delete arc;
        }

        return Py::new_reference_to(list);
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::approximate(PyObject* args, PyObject* kwds)
{
    PyObject* obj;
    Standard_Integer degMin = 3;
    Standard_Integer degMax = 8;
    Standard_Integer segMax = 8;
    const char* continuity = "C2";
    double tol3d = 1e-3;
    const char* parType = "ChordLength";
    PyObject* par = nullptr;
    double weight1 = 0;
    double weight2 = 0;
    double weight3 = 0;

    // Approximate this curve with a given continuity and degree
    static const std::array<const char*, 5>
        kwds_reapprox {"MaxDegree", "MaxSegments", "Continuity", "Tolerance", nullptr};
    if (Base::Wrapped_ParseTupleAndKeywords(
            args,
            kwds,
            "i|isd",
            kwds_reapprox,
            &tol3d,
            &degMax,
            &segMax,
            &continuity
        )) {

        GeomAbs_Shape c;
        std::string str = continuity;
        if (str == "C0") {
            c = GeomAbs_C0;
        }
        else if (str == "G1") {
            c = GeomAbs_G1;
        }
        else if (str == "C1") {
            c = GeomAbs_C1;
        }
        else if (str == "G2") {
            c = GeomAbs_G2;
        }
        else if (str == "C2") {
            c = GeomAbs_C2;
        }
        else if (str == "C3") {
            c = GeomAbs_C3;
        }
        else if (str == "CN") {
            c = GeomAbs_CN;
        }
        else {
            c = GeomAbs_C2;
        }

        this->getGeomBSplineCurvePtr()->approximate(tol3d, segMax, degMax, c);
        Py_Return;
    }

    // Approximate a list of points
    //
    static const std::array<const char*, 11> kwds_interp {
        "Points",
        "DegMax",
        "Continuity",
        "Tolerance",
        "DegMin",
        "ParamType",
        "Parameters",
        "LengthWeight",
        "CurvatureWeight",
        "TorsionWeight",
        nullptr
    };

    PyErr_Clear();
    if (!Base::Wrapped_ParseTupleAndKeywords(
            args,
            kwds,
            "O|isdisOddd",
            kwds_interp,
            &obj,
            &degMax,
            &continuity,
            &tol3d,
            &degMin,
            &parType,
            &par,
            &weight1,
            &weight2,
            &weight3
        )) {
        return nullptr;
    }

    try {
        Py::Sequence list(obj);
        TColgp_Array1OfPnt pnts(1, list.size());
        Standard_Integer index = 1;
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            Base::Vector3d vec = Py::Vector(*it).toVector();
            pnts(index++) = gp_Pnt(vec.x, vec.y, vec.z);
        }

        if (degMin > degMax) {
            throw Standard_Failure("DegMin must be lower or equal to DegMax");
        }

        GeomAbs_Shape c;
        std::string str = continuity;
        if (str == "C0") {
            c = GeomAbs_C0;
        }
        else if (str == "G1") {
            c = GeomAbs_G1;
        }
        else if (str == "C1") {
            c = GeomAbs_C1;
        }
        else if (str == "G2") {
            c = GeomAbs_G2;
        }
        else if (str == "C2") {
            c = GeomAbs_C2;
        }
        else if (str == "C3") {
            c = GeomAbs_C3;
        }
        else if (str == "CN") {
            c = GeomAbs_CN;
        }
        else {
            c = GeomAbs_C2;
        }

        if (weight1 || weight2 || weight3) {
            // It seems that this function only works with Continuity = C0, C1 or C2
            GeomAPI_PointsToBSpline fit(pnts, weight1, weight2, weight3, degMax, c, tol3d);
            Handle(Geom_BSplineCurve) spline = fit.Curve();
            if (!spline.IsNull()) {
                this->getGeomBSplineCurvePtr()->setHandle(spline);
                Py_Return;
            }
            else {
                throw Standard_Failure("Smoothing approximation failed");
                return nullptr;  // goes to the catch block
            }
        }

        if (par) {
            Py::Sequence plist(par);
            TColStd_Array1OfReal parameters(1, plist.size());
            Standard_Integer index = 1;
            for (Py::Sequence::iterator it = plist.begin(); it != plist.end(); ++it) {
                Py::Float f(*it);
                parameters(index++) = static_cast<double>(f);
            }

            GeomAPI_PointsToBSpline fit(pnts, parameters, degMin, degMax, c, tol3d);
            Handle(Geom_BSplineCurve) spline = fit.Curve();
            if (!spline.IsNull()) {
                this->getGeomBSplineCurvePtr()->setHandle(spline);
                Py_Return;
            }
            else {
                throw Standard_Failure("Approximation with parameters failed");
                return nullptr;  // goes to the catch block
            }
        }

        Approx_ParametrizationType pt;
        std::string pstr = parType;
        if (pstr == "Uniform") {
            pt = Approx_IsoParametric;
        }
        else if (pstr == "Centripetal") {
            pt = Approx_Centripetal;
        }
        else {
            pt = Approx_ChordLength;
        }

        GeomAPI_PointsToBSpline fit(pnts, pt, degMin, degMax, c, tol3d);
        Handle(Geom_BSplineCurve) spline = fit.Curve();
        if (!spline.IsNull()) {
            this->getGeomBSplineCurvePtr()->setHandle(spline);
            Py_Return;
        }
        else {
            throw Standard_Failure("failed to approximate points");
            return nullptr;  // goes to the catch block
        }
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::getCardinalSplineTangents(PyObject* args, PyObject* kwds) const
{
    PyObject* pts;
    PyObject* tgs;
    double parameter;

    static const std::array<const char*, 3> kwds_interp1 {"Points", "Parameter", nullptr};
    if (Base::Wrapped_ParseTupleAndKeywords(args, kwds, "Od", kwds_interp1, &pts, &parameter)) {
        Py::Sequence list(pts);
        std::vector<gp_Pnt> interpPoints;
        interpPoints.reserve(list.size());
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            Py::Vector v(*it);
            Base::Vector3d pnt = v.toVector();
            interpPoints.emplace_back(pnt.x, pnt.y, pnt.z);
        }

        GeomBSplineCurve* bspline = this->getGeomBSplineCurvePtr();
        std::vector<gp_Vec> tangents;
        bspline->getCardinalSplineTangents(interpPoints, parameter, tangents);

        Py::List vec;
        for (gp_Vec it : tangents) {
            vec.append(Py::Vector(Base::Vector3d(it.X(), it.Y(), it.Z())));
        }
        return Py::new_reference_to(vec);
    }

    PyErr_Clear();
    static const std::array<const char*, 3> kwds_interp2 {"Points", "Parameters", nullptr};
    if (Base::Wrapped_ParseTupleAndKeywords(args, kwds, "OO", kwds_interp2, &pts, &tgs)) {
        Py::Sequence list(pts);
        std::vector<gp_Pnt> interpPoints;
        interpPoints.reserve(list.size());
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            Py::Vector v(*it);
            Base::Vector3d pnt = v.toVector();
            interpPoints.emplace_back(pnt.x, pnt.y, pnt.z);
        }

        Py::Sequence list2(tgs);
        std::vector<double> parameters;
        parameters.reserve(list2.size());
        for (Py::Sequence::iterator it = list2.begin(); it != list2.end(); ++it) {
            Py::Float p(*it);
            parameters.push_back(static_cast<double>(p));
        }

        GeomBSplineCurve* bspline = this->getGeomBSplineCurvePtr();
        std::vector<gp_Vec> tangents;
        bspline->getCardinalSplineTangents(interpPoints, parameters, tangents);

        Py::List vec;
        for (gp_Vec it : tangents) {
            vec.append(Py::Vector(Base::Vector3d(it.X(), it.Y(), it.Z())));
        }
        return Py::new_reference_to(vec);
    }

    return nullptr;
}

PyObject* BSplineCurvePy::interpolate(PyObject* args, PyObject* kwds)
{
    PyObject* obj;
    PyObject* par = nullptr;
    double tol3d = Precision::Approximation();
    PyObject* periodic = Py_False;
    PyObject* t1 = nullptr;
    PyObject* t2 = nullptr;
    PyObject* ts = nullptr;
    PyObject* fl = nullptr;
    PyObject* scale = Py_True;

    static const std::array<const char*, 10> kwds_interp {
        "Points",
        "PeriodicFlag",
        "Tolerance",
        "InitialTangent",
        "FinalTangent",
        "Tangents",
        "TangentFlags",
        "Parameters",
        "Scale",
        nullptr
    };

    if (!Base::Wrapped_ParseTupleAndKeywords(
            args,
            kwds,
            "O|O!dO!O!OOOO!",
            kwds_interp,
            &obj,
            &PyBool_Type,
            &periodic,
            &tol3d,
            &Base::VectorPy::Type,
            &t1,
            &Base::VectorPy::Type,
            &t2,
            &ts,
            &fl,
            &par,
            &PyBool_Type,
            &scale
        )) {
        return nullptr;
    }

    try {
        Py::Sequence list(obj);
        Handle(TColgp_HArray1OfPnt) interpolationPoints = new TColgp_HArray1OfPnt(1, list.size());
        Standard_Integer index = 1;
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            Py::Vector v(*it);
            Base::Vector3d pnt = v.toVector();
            interpolationPoints->SetValue(index++, gp_Pnt(pnt.x, pnt.y, pnt.z));
        }

        if (interpolationPoints->Length() < 2) {
            throw Standard_Failure("not enough points given");
        }

        Handle(TColStd_HArray1OfReal) parameters;
        if (par) {
            Py::Sequence plist(par);
            parameters = new TColStd_HArray1OfReal(1, plist.size());
            Standard_Integer pindex = 1;
            for (Py::Sequence::iterator it = plist.begin(); it != plist.end(); ++it) {
                Py::Float f(*it);
                parameters->SetValue(pindex++, static_cast<double>(f));
            }
        }

        std::unique_ptr<GeomAPI_Interpolate> aBSplineInterpolation;
        if (parameters.IsNull()) {
            aBSplineInterpolation = std::make_unique<GeomAPI_Interpolate>(
                interpolationPoints,
                Base::asBoolean(periodic),
                tol3d
            );
        }
        else {
            aBSplineInterpolation = std::make_unique<GeomAPI_Interpolate>(
                interpolationPoints,
                parameters,
                Base::asBoolean(periodic),
                tol3d
            );
        }

        if (t1 && t2) {
            Base::Vector3d v1 = Py::Vector(t1, false).toVector();
            Base::Vector3d v2 = Py::Vector(t2, false).toVector();
            gp_Vec initTangent(v1.x, v1.y, v1.z), finalTangent(v2.x, v2.y, v2.z);
            aBSplineInterpolation->Load(initTangent, finalTangent, Base::asBoolean(scale));
        }
        else if (ts && fl) {
            Py::Sequence tlist(ts);
            TColgp_Array1OfVec tangents(1, tlist.size());
            Standard_Integer index = 1;
            for (Py::Sequence::iterator it = tlist.begin(); it != tlist.end(); ++it) {
                Py::Vector v(*it);
                Base::Vector3d vec = v.toVector();
                tangents.SetValue(index++, gp_Vec(vec.x, vec.y, vec.z));
            }

            Py::Sequence flist(fl);
            Handle(TColStd_HArray1OfBoolean)
                tangentFlags = new TColStd_HArray1OfBoolean(1, flist.size());
            Standard_Integer findex = 1;
            for (Py::Sequence::iterator it = flist.begin(); it != flist.end(); ++it) {
                Py::Boolean flag(*it);
                tangentFlags->SetValue(
                    findex++,
                    static_cast<bool>(flag) ? Standard_True : Standard_False
                );
            }

            aBSplineInterpolation->Load(tangents, tangentFlags, Base::asBoolean(scale));
        }

        aBSplineInterpolation->Perform();
        if (aBSplineInterpolation->IsDone()) {
            Handle(Geom_BSplineCurve) aBSplineCurve(aBSplineInterpolation->Curve());
            this->getGeomBSplineCurvePtr()->setHandle(aBSplineCurve);
            Py_Return;
        }
        else {
            throw Standard_Failure("failed to interpolate points");
            return nullptr;  // goes to the catch block
        }
    }
    catch (Standard_Failure& e) {
        std::string err = e.GetMessageString();
        if (err.empty()) {
#if OCC_VERSION_HEX >= 0x080000
            err = e.ExceptionType();
#else
            err = e.DynamicType()->Name();
#endif
        }
        PyErr_SetString(PartExceptionOCCError, err.c_str());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::buildFromPoles(PyObject* args){
    return BSplineCurvePyHelpers::buildFromPoles(this, args);
}

PyObject* BSplineCurvePy::buildFromPolesMultsKnots(PyObject* args, PyObject* keywds)
{
    static const std::array<const char*, 8>
        kwlist {"poles", "mults", "knots", "periodic", "degree", "weights", "CheckRational", nullptr};
    PyObject* periodic = Py_False;      // NOLINT
    PyObject* CheckRational = Py_True;  // NOLINT
    PyObject* poles = Py_None;
    PyObject* mults = Py_None;
    PyObject* knots = Py_None;
    PyObject* weights = Py_None;
    int degree = 3;
    int number_of_poles = 0;
    int number_of_knots = 0;
    int sum_of_mults = 0;
    if (!Base::Wrapped_ParseTupleAndKeywords(
            args,
            keywds,
            "O|OOO!iOO!",
            kwlist,
            &poles,
            &mults,
            &knots,
            &PyBool_Type,
            &periodic,
            &degree,
            &weights,
            &PyBool_Type,
            &CheckRational
        )) {
        return nullptr;
    }
    try {
        // poles have to be present
        Py::Sequence list(poles);

        number_of_poles = list.size();
        if ((number_of_poles) < 2) {
            throw Standard_Failure("need two or more poles");
            return nullptr;
        }
        TColgp_Array1OfPnt occpoles(1, number_of_poles);
        Standard_Integer index = 1;
        for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
            Py::Vector v(*it);
            Base::Vector3d pnt = v.toVector();
            occpoles(index++) = gp_Pnt(pnt.x, pnt.y, pnt.z);
        }
        // Calculate the number of knots
        if (mults != Py_None && knots != Py_None) {
            number_of_knots = PyObject_Length(mults);
            if (PyObject_Length(knots) != number_of_knots) {
                throw Standard_Failure("number of knots and mults mismatch");
                return nullptr;
            }
        }
        else {
            if (mults != Py_None) {
                number_of_knots = PyObject_Length(mults);
            }
            else {
                if (knots != Py_None) {
                    number_of_knots = PyObject_Length(knots);
                }
                else {  // guess number of knots
                    if (Base::asBoolean(periodic)) {
                        if (number_of_poles < degree) {
                            degree = number_of_poles;
                        }
                        number_of_knots = number_of_poles + 1;
                    }
                    else {
                        if (number_of_poles <= degree) {
                            degree = number_of_poles - 1;
                        }
                        number_of_knots = number_of_poles - degree + 1;
                    }
                }
            }
        }
        TColStd_Array1OfInteger occmults(1, number_of_knots);
        TColStd_Array1OfReal occknots(1, number_of_knots);
        TColStd_Array1OfReal occweights(1, number_of_poles);
        if (mults != Py_None) {  // mults are given
            Py::Sequence multssq(mults);
            Standard_Integer index = 1;
            for (Py::Sequence::iterator it = multssq.begin();
                 it != multssq.end() && index <= occmults.Length();
                 ++it) {
                Py::Long mult(*it);
                if (index < occmults.Length() || !Base::asBoolean(periodic)) {
                    sum_of_mults += static_cast<int>(mult);  // sum up the mults to compare them
                                                             // against the number of poles later
                }
                occmults(index++) = static_cast<int>(mult);
            }
        }
        else {  // mults are 1 or degree+1 at the ends
            for (int i = 1; i <= occmults.Length(); i++) {
                occmults.SetValue(i, 1);
            }
            if (!Base::asBoolean(periodic) && occmults.Length() > 0) {
                occmults.SetValue(1, degree + 1);
                occmults.SetValue(occmults.Length(), degree + 1);
                sum_of_mults = occmults.Length() + 2 * degree;
            }
            else {
                sum_of_mults = occmults.Length() - 1;
            }
        }
        // check multiplicity of inner knots
        for (Standard_Integer i = 2; i < occmults.Length(); i++) {
            if (occmults(i) > degree) {
                throw Standard_Failure("multiplicity of inner knot higher than degree");
            }
        }
        if (knots != Py_None) {  // knots are given
            Py::Sequence knotssq(knots);
            index = 1;
            for (Py::Sequence::iterator it = knotssq.begin();
                 it != knotssq.end() && index <= occknots.Length();
                 ++it) {
                Py::Float knot(*it);
                occknots(index++) = knot;
            }
        }
        else {  // knotes are uniformly spaced 0..1 if not given
            for (int i = 1; i <= occknots.Length(); i++) {
                occknots.SetValue(i, (double)(i - 1) / (occknots.Length() - 1));
            }
        }
        if (weights != Py_None) {  // weights are given
            if (PyObject_Length(weights) != number_of_poles) {
                throw Standard_Failure("number of poles and weights mismatch");
                return nullptr;
            }  // complain about mismatch
            Py::Sequence weightssq(weights);
            Standard_Integer index = 1;
            for (Py::Sequence::iterator it = weightssq.begin(); it != weightssq.end(); ++it) {
                Py::Float weight(*it);
                occweights(index++) = weight;
            }
        }
        else {  // weights are 1.0
            for (int i = 1; i <= occweights.Length(); i++) {
                occweights.SetValue(i, 1.0);
            }
        }
        // check if the number of poles matches the sum of mults
        if ((Base::asBoolean(periodic) && sum_of_mults != number_of_poles)
            || (!Base::asBoolean(periodic) && sum_of_mults - degree - 1 != number_of_poles)) {
            throw Standard_Failure("number of poles and sum of mults mismatch");
            return (nullptr);
        }

        Handle(Geom_BSplineCurve) spline = new Geom_BSplineCurve(
            occpoles,
            occweights,
            occknots,
            occmults,
            degree,
            Base::asBoolean(periodic),
            Base::asBoolean(CheckRational)
        );
        if (!spline.IsNull()) {
            this->getGeomBSplineCurvePtr()->setHandle(spline);
            Py_Return;
        }
        else {
            throw Standard_Failure("failed to create spline");
            return nullptr;  // goes to the catch block
        }
    }
    catch (const Standard_Failure& e) {
        Standard_CString msg = e.GetMessageString();
        PyErr_SetString(PartExceptionOCCError, msg ? msg : "");
        return nullptr;
    }
}


PyObject* BSplineCurvePy::toBezier(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    try {
        Handle(Geom_BSplineCurve)
            spline = Handle(Geom_BSplineCurve)::DownCast(this->getGeomBSplineCurvePtr()->handle());
        GeomConvert_BSplineCurveToBezierCurve crt(spline);

        Py::List list;
        Standard_Integer arcs = crt.NbArcs();
        for (Standard_Integer i = 1; i <= arcs; i++) {
            Handle(Geom_BezierCurve) bezier = crt.Arc(i);
            list.append(Py::asObject(new BezierCurvePy(new GeomBezierCurve(bezier))));
        }

        return Py::new_reference_to(list);
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::join(PyObject* args)
{
    PyObject* c;
    if (!PyArg_ParseTuple(args, "O!", &BSplineCurvePy::Type, &c)) {
        return nullptr;
    }

    try {
        GeomBSplineCurve* curve1 = this->getGeomBSplineCurvePtr();
        BSplineCurvePy* curve2 = static_cast<BSplineCurvePy*>(c);
        Handle(Geom_BSplineCurve)
            spline = Handle(Geom_BSplineCurve)::DownCast(curve2->getGeomBSplineCurvePtr()->handle());

        bool ok = curve1->join(spline);

        return PyBool_FromLong(ok ? 1 : 0);
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::makeC1Continuous(PyObject* args)
{
    double tol = Precision::Approximation();
    double ang_tol = 1.0e-7;
    if (!PyArg_ParseTuple(args, "|dd", &tol, &ang_tol)) {
        return nullptr;
    }

    try {
        GeomBSplineCurve* spline = this->getGeomBSplineCurvePtr();
        spline->makeC1Continuous(tol, ang_tol);
        Py_Return;
    }
    catch (Standard_Failure& e) {
        std::string err = e.GetMessageString();
        if (err.empty()) {
#if OCC_VERSION_HEX >= 0x080000
            err = e.ExceptionType();
#else
            err = e.DynamicType()->Name();
#endif
        }
        PyErr_SetString(PartExceptionOCCError, err.c_str());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::scaleKnotsToBounds(PyObject* args)
{
    double u0 = 0.0;
    double u1 = 1.0;
    if (!PyArg_ParseTuple(args, "|dd", &u0, &u1)) {
        return nullptr;
    }
    try {
        if (u0 >= u1) {
            throw Standard_Failure("Bad parameter range");
            return nullptr;
        }
        GeomBSplineCurve* curve = getGeomBSplineCurvePtr();
        curve->scaleKnotsToBounds(u0, u1);
        Py_Return;
    }
    catch (Standard_Failure& e) {
        std::string err = e.GetMessageString();
        if (err.empty()) {
#if OCC_VERSION_HEX >= 0x080000
            err = e.ExceptionType();
#else
            err = e.DynamicType()->Name();
#endif
        }
        PyErr_SetString(PartExceptionOCCError, err.c_str());
        return nullptr;
    }
}

PyObject* BSplineCurvePy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int BSplineCurvePy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
