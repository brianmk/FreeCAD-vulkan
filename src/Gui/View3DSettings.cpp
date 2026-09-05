/***************************************************************************
 *   Copyright (c) 2023 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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


#include <Inventor/fields/SoSFColor.h>
#include <Inventor/nodes/SoDirectionalLight.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>


#include <Base/Builder3D.h>
#include <Base/Color.h>

#include "NaviCube.h"
#include "Navigation/NavigationStyle.h"
#include "Selection/SelectionColors.h"
#include "SoFCSelectionAction.h"
#include "View3DSettings.h"
#include "View3DInventorViewer.h"
#include "VulkanViewSettings.h"

#include <Base/Tools.h>

using namespace Gui;

View3DSettings::View3DSettings(ParameterGrp::handle hGrp, View3DInventorViewer* view)
    : hGrp(hGrp)
    , hLightSourcesGrp(hGrp->GetGroup("LightSources"))
    , _viewers {view}
{
    hGrp->Attach(this);
    hLightSourcesGrp->Attach(this);
}

View3DSettings::View3DSettings(ParameterGrp::handle hGrp, const std::vector<View3DInventorViewer*>& view)
    : hGrp(hGrp)
    , hLightSourcesGrp(hGrp->GetGroup("LightSources"))
    , _viewers(view)
{
    hGrp->Attach(this);
    hLightSourcesGrp->Attach(this);
}

View3DSettings::~View3DSettings()
{
    hGrp->Detach(this);
    hLightSourcesGrp->Detach(this);
}

int View3DSettings::stopAnimatingIfDeactivated() const
{
    long defaultTimeout = 3000;
    return hGrp->GetInt("stopAnimatingIfDeactivated", defaultTimeout);
}

void View3DSettings::applySettings()
{
    // Apply the user settings: dispatch every handled preference so a view
    // opens matching the saved preferences (not the framework default).  This
    // iterates the single preference table, so it can never drift from the
    // runtime dispatch in OnChange().
    ensurePrefTable();
    for (const auto & entry : m_prefTable) {
        const ParameterGrp::handle & group = entry.light ? hLightSourcesGrp : hGrp;
        OnChange(*group, entry.name);
    }
    // The Vulkan display prefs are routed by a "Vulkan*" prefix rule rather
    // than enumerated in the table; one representative kick loads the whole
    // Vulkan preference set via applyVulkanSettings().
    OnChange(*hGrp, "VulkanRenderMode");
}

void View3DSettings::OnChange(ParameterGrp::SubjectType& rCaller, ParameterGrp::MessageType Reason)
{
    // Vulkan display prefs are a single prefix rule (kept out of the table so
    // a new "Vulkan*" pref is picked up automatically and needs no table edit).
    if (VulkanViewSettings::isDisplayPref(Reason)) {
        for (auto _viewer : _viewers) {
            _viewer->applyVulkanSettings();
        }
        return;
    }
    const ParameterGrp& rGrp = static_cast<const ParameterGrp&>(rCaller);
    ensurePrefTable();
    for (const auto & entry : m_prefTable) {
        if (strcmp(entry.name, Reason) == 0) {
            entry.apply(rGrp);
            return;
        }
    }
    // Any unrecognized pref in the view group preserves the old catch-all
    // behavior (apply the background colors).
    applyBackground(rGrp);
}

void View3DSettings::ensurePrefTable()
{
    if (!m_prefTable.empty()) {
        return;
    }
    auto add = [](std::vector<PrefEntry>& out, const char * name, bool light,
                  std::function<void(const ParameterGrp&)> fn) {
        out.push_back(PrefEntry{name, light, std::move(fn)});
    };

    // ---- Light sources (sub-group) -------------------------------------
    add(m_prefTable, "EnableHeadlight", true, [this](const ParameterGrp & rGrp) {
        bool enable = rGrp.GetBool("EnableHeadlight", true);
        for (auto _viewer : _viewers) {
            _viewer->setHeadlightEnabled(enable);
        }
    });
    add(m_prefTable, "HeadlightColor", true, [this](const ParameterGrp & rGrp) {
        unsigned long headlight = rGrp.GetUnsigned("HeadlightColor", 0xFFFFFFFF);
        float transparency;
        SbColor headlightColor;
        headlightColor.setPackedValue((uint32_t)headlight, transparency);
        for (auto _viewer : _viewers) {
            _viewer->getHeadlight()->color.setValue(headlightColor);
        }
    });
    add(m_prefTable, "HeadlightDirection", true, [this](const ParameterGrp & rGrp) {
        try {
            std::string pos = rGrp.GetASCII("HeadlightDirection", defaultHeadLightDirection);
            if (!pos.empty()) {
                Base::Vector3f dir = Base::stringToVector(pos);
                for (auto _viewer : _viewers) {
                    _viewer->getHeadlight()->direction.setValue(dir.x, dir.y, dir.z);
                }
            }
        }
        catch (const std::exception&) {
            // ignore exception
        }
    });
    add(m_prefTable, "HeadlightIntensity", true, [this](const ParameterGrp & rGrp) {
        long value = rGrp.GetInt("HeadlightIntensity", 90);
        for (auto _viewer : _viewers) {
            _viewer->getHeadlight()->intensity.setValue(Base::fromPercent(value));
        }
    });
    add(m_prefTable, "EnableBacklight", true, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setBacklightEnabled(rGrp.GetBool("EnableBacklight", true));
        }
    });
    add(m_prefTable, "BacklightColor", true, [this](const ParameterGrp & rGrp) {
        unsigned long backlight = rGrp.GetUnsigned("BacklightColor", 0xF5F5EEFF);
        float transparency;
        SbColor backlightColor;
        backlightColor.setPackedValue((uint32_t)backlight, transparency);
        for (auto _viewer : _viewers) {
            _viewer->getBacklight()->color.setValue(backlightColor);
        }
    });
    add(m_prefTable, "BacklightDirection", true, [this](const ParameterGrp & rGrp) {
        try {
            std::string pos = rGrp.GetASCII("BacklightDirection", defaultBackLightDirection);
            if (!pos.empty()) {
                Base::Vector3f dir = Base::stringToVector(pos);
                for (auto _viewer : _viewers) {
                    _viewer->getBacklight()->direction.setValue(dir.x, dir.y, dir.z);
                }
            }
        }
        catch (const std::exception&) {
            // ignore exception
        }
    });
    add(m_prefTable, "BacklightIntensity", true, [this](const ParameterGrp & rGrp) {
        long value = rGrp.GetInt("BacklightIntensity", 60);
        for (auto _viewer : _viewers) {
            _viewer->getBacklight()->intensity.setValue(Base::fromPercent(value));
        }
    });
    add(m_prefTable, "EnableFillLight", true, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setFillLightEnabled(rGrp.GetBool("EnableFillLight", true));
        }
    });
    add(m_prefTable, "FillLightColor", true, [this](const ParameterGrp & rGrp) {
        unsigned long fill = rGrp.GetUnsigned("FillLightColor", 0xE6FAFFFF);
        float transparency;
        SbColor fillColor;
        fillColor.setPackedValue((uint32_t)fill, transparency);
        for (auto _viewer : _viewers) {
            _viewer->getFillLight()->color.setValue(fillColor);
        }
    });
    add(m_prefTable, "FillLightDirection", true, [this](const ParameterGrp & rGrp) {
        try {
            std::string pos = rGrp.GetASCII("FillLightDirection", defaultFillLightDirection);
            if (!pos.empty()) {
                Base::Vector3f dir = Base::stringToVector(pos);
                for (auto _viewer : _viewers) {
                    _viewer->getFillLight()->direction.setValue(dir.x, dir.y, dir.z);
                }
            }
        }
        catch (const std::exception&) {
            // ignore exception
        }
    });
    add(m_prefTable, "FillLightIntensity", true, [this](const ParameterGrp & rGrp) {
        long value = rGrp.GetInt("FillLightIntensity", 40);
        for (auto _viewer : _viewers) {
            _viewer->getFillLight()->intensity.setValue(Base::fromPercent(value));
        }
    });
    add(m_prefTable, "AmbientLightColor", true, [this](const ParameterGrp & rGrp) {
        unsigned long color = rGrp.GetUnsigned("AmbientLightColor", 0xFFFFFFFF);
        float transparency;
        SbColor col;
        col.setPackedValue((uint32_t)color, transparency);
        for (auto _viewer : _viewers) {
            _viewer->getEnvironment()->ambientColor.setValue(col);
        }
    });
    add(m_prefTable, "AmbientLightIntensity", true, [this](const ParameterGrp & rGrp) {
        long value = rGrp.GetInt("AmbientLightIntensity", 20);
        for (auto _viewer : _viewers) {
            _viewer->getEnvironment()->ambientIntensity.setValue(Base::fromPercent(value));
        }
    });

    // ---- Selection ----------------------------------------------------
    add(m_prefTable, "EnablePreselection", false, [this](const ParameterGrp & rGrp) {
        SoFCEnablePreselectionAction cAct(rGrp.GetBool("EnablePreselection", true));
        for (auto _viewer : _viewers) {
            cAct.apply(_viewer->getSceneGraph());
        }
    });
    add(m_prefTable, "EnableSelection", false, [this](const ParameterGrp & rGrp) {
        SoFCEnableSelectionAction cAct(rGrp.GetBool("EnableSelection", true));
        for (auto _viewer : _viewers) {
            cAct.apply(_viewer->getSceneGraph());
        }
    });
    add(m_prefTable, "HighlightColor", false, [this](const ParameterGrp &) {
        SoSFColor col;
        col.setValue(SelectionColors::defaultHighlightColor());
        SoFCHighlightColorAction cAct(col);
        for (auto _viewer : _viewers) {
            cAct.apply(_viewer->getSceneGraph());
        }
    });
    add(m_prefTable, "SelectionColor", false, [this](const ParameterGrp &) {
        SoSFColor col;
        col.setValue(SelectionColors::defaultSelectionColor());
        SoFCSelectionColorAction cAct(col);
        for (auto _viewer : _viewers) {
            cAct.apply(_viewer->getSceneGraph());
        }
    });

    // ---- Navigation ----------------------------------------------------
    add(m_prefTable, "NavigationStyle", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreNavigationStyle) {
            std::string model = rGrp.GetASCII(
                "NavigationStyle",
                std::string {CADNavigationStyle::getClassTypeId().getName()}.c_str()
            );
            Base::Type type = Base::Type::fromName(model.c_str());
            for (auto _viewer : _viewers) {
                _viewer->setNavigationType(type);
            }
        }
    });
    add(m_prefTable, "OrbitStyle", false, [this](const ParameterGrp & rGrp) {
        int style = rGrp.GetInt("OrbitStyle", 4);
        for (auto _viewer : _viewers) {
            _viewer->navigationStyle()->setOrbitStyle(NavigationStyle::OrbitStyle(style));
        }
    });
    add(m_prefTable, "Sensitivity", false, [this](const ParameterGrp & rGrp) {
        float val = rGrp.GetFloat("Sensitivity", 2.0f);
        for (auto _viewer : _viewers) {
            _viewer->navigationStyle()->setSensitivity(val);
        }
    });
    add(m_prefTable, "ResetCursorPosition", false, [this](const ParameterGrp & rGrp) {
        bool on = rGrp.GetBool("ResetCursorPosition", false);
        for (auto _viewer : _viewers) {
            _viewer->navigationStyle()->setResetCursorPosition(on);
        }
    });
    add(m_prefTable, "InvertZoom", false, [this](const ParameterGrp & rGrp) {
        bool on = rGrp.GetBool("InvertZoom", true);
        for (auto _viewer : _viewers) {
            _viewer->navigationStyle()->setZoomInverted(on);
        }
    });
    add(m_prefTable, "ZoomAtCursor", false, [this](const ParameterGrp & rGrp) {
        bool on = rGrp.GetBool("ZoomAtCursor", true);
        for (auto _viewer : _viewers) {
            _viewer->navigationStyle()->setZoomAtCursor(on);
        }
    });
    add(m_prefTable, "ZoomStep", false, [this](const ParameterGrp & rGrp) {
        float val = rGrp.GetFloat("ZoomStep", 0.0f);
        for (auto _viewer : _viewers) {
            _viewer->navigationStyle()->setZoomStep(val);
        }
    });
    add(m_prefTable, "RotationMode", false, [this](const ParameterGrp & rGrp) {
        long mode = rGrp.GetInt("RotationMode", 1);
        for (auto _viewer : _viewers) {
            if (mode == 0) {
                _viewer->navigationStyle()->setRotationCenterMode(
                    NavigationStyle::RotationCenterMode::WindowCenter
                );
            }
            else if (mode == 1) {
                _viewer->navigationStyle()->setRotationCenterMode(
                    NavigationStyle::RotationCenterMode::ScenePointAtCursor
                    | NavigationStyle::RotationCenterMode::FocalPointAtCursor
                );
            }
            else if (mode == 2) {
                _viewer->navigationStyle()->setRotationCenterMode(
                    NavigationStyle::RotationCenterMode::ScenePointAtCursor
                    | NavigationStyle::RotationCenterMode::BoundingBoxCenter
                );
            }
        }
    });

    // ---- Main view ------------------------------------------------------
    add(m_prefTable, "EyeDistance", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->getSoRenderManager()->setStereoOffset(rGrp.GetFloat("EyeDistance", 5.0));
        }
    });
    add(m_prefTable, "CornerCoordSystem", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setFeedbackVisibility(rGrp.GetBool("CornerCoordSystem", true));
        }
    });
    add(m_prefTable, "CornerCoordSystemSize", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setFeedbackSize(rGrp.GetInt("CornerCoordSystemSize", 10));
        }
    });
    add(m_prefTable, "AxisLetterColor", false, [this](const ParameterGrp & rGrp) {
        unsigned long color = rGrp.GetUnsigned("AxisLetterColor", 0x00000000);
        float transparency;
        SbColor col;
        col.setPackedValue((uint32_t)color, transparency);
        for (auto _viewer : _viewers) {
            _viewer->setAxisLetterColor(col);
        }
    });
    add(m_prefTable, "ShowAxisCross", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setAxisCross(rGrp.GetBool("ShowAxisCross", false));
        }
    });
    add(m_prefTable, "ShowGroundPlane", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setGroundPlane(rGrp.GetBool("ShowGroundPlane", false));
        }
    });
    add(m_prefTable, "GroundPlaneOpacity", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setGroundPlaneOpacity(static_cast<float>(rGrp.GetFloat("GroundPlaneOpacity", 0.15)));
        }
    });
    add(m_prefTable, "UseNavigationAnimations", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setAnimationEnabled(rGrp.GetBool("UseNavigationAnimations", true));
        }
    });
    add(m_prefTable, "UseSpinningAnimations", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setSpinningAnimationEnabled(rGrp.GetBool("UseSpinningAnimations", false));
        }
    });
    add(m_prefTable, "Gradient", false, [this](const ParameterGrp & rGrp) {
        View3DInventorViewer::Background background = View3DInventorViewer::Background::NoGradient;
        if (rGrp.GetBool("Gradient", true)) {
            background = View3DInventorViewer::Background::LinearGradient;
        }
        else if (rGrp.GetBool("RadialGradient", false)) {
            background = View3DInventorViewer::Background::RadialGradient;
        }
        for (auto _viewer : _viewers) {
            _viewer->setGradientBackground(background);
        }
    });
    add(m_prefTable, "RadialGradient", false, [this](const ParameterGrp & rGrp) {
        View3DInventorViewer::Background background = View3DInventorViewer::Background::NoGradient;
        if (rGrp.GetBool("Gradient", true)) {
            background = View3DInventorViewer::Background::LinearGradient;
        }
        else if (rGrp.GetBool("RadialGradient", false)) {
            background = View3DInventorViewer::Background::RadialGradient;
        }
        for (auto _viewer : _viewers) {
            _viewer->setGradientBackground(background);
        }
    });
    add(m_prefTable, "ShowFPS", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setEnabledFPSCounter(rGrp.GetBool("ShowFPS", false));
        }
    });
    add(m_prefTable, "ShowNaviCube", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setEnabledNaviCube(rGrp.GetBool("ShowNaviCube", true));
        }
    });
    add(m_prefTable, "AxisXColor", false, [this](const ParameterGrp &) {
        for (auto _viewer : _viewers) {
            _viewer->updateColors();
        }
    });
    add(m_prefTable, "AxisYColor", false, [this](const ParameterGrp &) {
        for (auto _viewer : _viewers) {
            _viewer->updateColors();
        }
    });
    add(m_prefTable, "AxisZColor", false, [this](const ParameterGrp &) {
        for (auto _viewer : _viewers) {
            _viewer->updateColors();
        }
    });
    add(m_prefTable, "UseVBO", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreVBO) {
            const auto useVbo = rGrp.GetBool("UseVBO", true);
            for (auto _viewer : _viewers) {
                _viewer->setEnabledVBO(useVbo);
            }
        }
    });
    add(m_prefTable, "RenderCache", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreRenderCache) {
            for (auto _viewer : _viewers) {
                _viewer->setRenderCache(rGrp.GetInt("RenderCache", 0));
            }
        }
    });
    add(m_prefTable, "MaxFrameRate", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setMaxFrameRate(static_cast<int>(rGrp.GetInt("MaxFrameRate", -1)));
        }
    });
    add(m_prefTable, "Orthographic", false, [this](const ParameterGrp & rGrp) {
        if (rGrp.GetBool("Orthographic", true)) {
            for (auto _viewer : _viewers) {
                _viewer->setCameraType(SoOrthographicCamera::getClassTypeId());
            }
        }
        else {
            for (auto _viewer : _viewers) {
                _viewer->setCameraType(SoPerspectiveCamera::getClassTypeId());
            }
        }
    });
    add(m_prefTable, "DimensionsVisible", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreDimensions) {
            if (rGrp.GetBool("DimensionsVisible", true)) {
                for (auto _viewer : _viewers) {
                    _viewer->turnAllDimensionsOn();
                }
            }
            else {
                for (auto _viewer : _viewers) {
                    _viewer->turnAllDimensionsOff();
                }
            }
        }
    });
    add(m_prefTable, "Dimensions3dVisible", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreDimensions) {
            if (rGrp.GetBool("Dimensions3dVisible", true)) {
                for (auto _viewer : _viewers) {
                    _viewer->turn3dDimensionsOn();
                }
            }
            else {
                for (auto _viewer : _viewers) {
                    _viewer->turn3dDimensionsOff();
                }
            }
        }
    });
    add(m_prefTable, "DimensionsDeltaVisible", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreDimensions) {
            if (rGrp.GetBool("DimensionsDeltaVisible", true)) {
                for (auto _viewer : _viewers) {
                    _viewer->turnDeltaDimensionsOn();
                }
            }
            else {
                for (auto _viewer : _viewers) {
                    _viewer->turnDeltaDimensionsOff();
                }
            }
        }
    });
    add(m_prefTable, "PickRadius", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setPickRadius(rGrp.GetFloat("PickRadius", 8.0f));
        }
    });
    add(m_prefTable, "PickRadiusScale", false, [this](const ParameterGrp & rGrp) {
        for (auto _viewer : _viewers) {
            _viewer->setPickRadiusScale(rGrp.GetFloat("PickRadiusScale", 1.0f));
        }
    });
    add(m_prefTable, "TransparentObjectRenderType", false, [this](const ParameterGrp & rGrp) {
        if (!this->ignoreTransparent) {
            long renderType = rGrp.GetInt("TransparentObjectRenderType", 0);
            if (renderType == 0) {
                for (auto _viewer : _viewers) {
                    _viewer->getSoRenderManager()
                        ->getGLRenderAction()
                        ->setTransparentDelayedObjectRenderType(SoGLRenderAction::ONE_PASS);
                }
            }
            else if (renderType == 1) {
                for (auto _viewer : _viewers) {
                    _viewer->getSoRenderManager()->getGLRenderAction()->setTransparentDelayedObjectRenderType(
                        SoGLRenderAction::NONSOLID_SEPARATE_BACKFACE_PASS
                    );
                }
            }
        }
    });

    // ---- No-op backend-choice / throttle keys (kept so the dispatch table is
    // ---- exhaustive; the old if/else ignored them) ----------------------
    add(m_prefTable, "UseVulkanRenderer", false, [](const ParameterGrp &) {});
    add(m_prefTable, "UseVulkanRayTracing", false, [](const ParameterGrp &) {});
    add(m_prefTable, "PreselectionMessageRate", false, [](const ParameterGrp &) {});

    // ---- Background colors (the old OnChange `else` catch-all) ----------
    add(m_prefTable, "BackgroundColor", false,
        [this](const ParameterGrp & rGrp) { this->applyBackground(rGrp); });
    add(m_prefTable, "BackgroundColor2", false,
        [this](const ParameterGrp & rGrp) { this->applyBackground(rGrp); });
    add(m_prefTable, "BackgroundColor3", false,
        [this](const ParameterGrp & rGrp) { this->applyBackground(rGrp); });
    add(m_prefTable, "BackgroundColor4", false,
        [this](const ParameterGrp & rGrp) { this->applyBackground(rGrp); });
    add(m_prefTable, "UseBackgroundColorMid", false,
        [this](const ParameterGrp & rGrp) { this->applyBackground(rGrp); });
}

void View3DSettings::applyBackground(const ParameterGrp& rGrp)
{
    unsigned long col1 = rGrp.GetUnsigned("BackgroundColor", 3940932863UL);
    unsigned long col2 = rGrp.GetUnsigned("BackgroundColor2", 859006463UL);
    unsigned long col3 = rGrp.GetUnsigned("BackgroundColor3", 2880160255UL);
    unsigned long col4 = rGrp.GetUnsigned("BackgroundColor4", 1869583359UL);
    float r1, g1, b1, r2, g2, b2, r3, g3, b3, r4, g4, b4;
    r1 = ((col1 >> 24) & 0xff) / 255.0;
    g1 = ((col1 >> 16) & 0xff) / 255.0;
    b1 = ((col1 >> 8) & 0xff) / 255.0;
    r2 = ((col2 >> 24) & 0xff) / 255.0;
    g2 = ((col2 >> 16) & 0xff) / 255.0;
    b2 = ((col2 >> 8) & 0xff) / 255.0;
    r3 = ((col3 >> 24) & 0xff) / 255.0;
    g3 = ((col3 >> 16) & 0xff) / 255.0;
    b3 = ((col3 >> 8) & 0xff) / 255.0;
    r4 = ((col4 >> 24) & 0xff) / 255.0;
    g4 = ((col4 >> 16) & 0xff) / 255.0;
    b4 = ((col4 >> 8) & 0xff) / 255.0;
    for (auto _viewer : _viewers) {
        _viewer->setBackgroundColor(QColor::fromRgbF(r1, g1, b1));
        if (!rGrp.GetBool("UseBackgroundColorMid", false)) {
            _viewer->setGradientBackgroundColor(SbColor(r2, g2, b2), SbColor(r3, g3, b3));
        }
        else {
            _viewer->setGradientBackgroundColor(
                SbColor(r2, g2, b2),
                SbColor(r3, g3, b3),
                SbColor(r4, g4, b4)
            );
        }
    }
}


// ----------------------------------------------------------------------------

NaviCubeSettings::NaviCubeSettings(ParameterGrp::handle hGrp, View3DInventorViewer* view)
    : hGrp(hGrp)
    , _viewer(view)
{
    connectParameterChanged = hGrp->Manager()->signalParamChanged.connect(
        [this](ParameterGrp*, ParameterGrp::ParamType, const char* Name, const char*) {
            parameterChanged(Name);
        }
    );
}

NaviCubeSettings::~NaviCubeSettings()
{
    connectParameterChanged.disconnect();
}

void NaviCubeSettings::applySettings()
{
    parameterChanged("BaseColor");
    parameterChanged("EmphaseColor");
    parameterChanged("HiliteColor");
    parameterChanged("CornerNaviCube");
    parameterChanged("OffsetX");  // Updates OffsetY too
    parameterChanged("CubeSize");
    parameterChanged("NaviScale");
    parameterChanged("ChamferSize");
    parameterChanged("NaviRotateToNearest");
    parameterChanged("NaviStepByTurn");
    parameterChanged("BorderWidth");
    parameterChanged("FontZoom");
    parameterChanged("FontString");
    parameterChanged("FontWeight");
    parameterChanged("FontStretch");
    parameterChanged("ShowCS");
    parameterChanged("InactiveOpacity");
    parameterChanged("TextFront");  // Updates all labels
}

void NaviCubeSettings::parameterChanged(const char* Name)
{
    if (!Name) {
        return;
    }
    NaviCube* nc = _viewer->getNaviCube();
    if (strcmp(Name, "CornerNaviCube") == 0) {
        nc->setCorner(static_cast<NaviCube::Corner>(hGrp->GetInt("CornerNaviCube", 1)));
    }
    else if (strcmp(Name, "OffsetX") == 0 || strcmp(Name, "OffsetY") == 0) {
        nc->setOffset(hGrp->GetInt("OffsetX", 0), hGrp->GetInt("OffsetY", 0));
    }
    else if (strcmp(Name, "ChamferSize") == 0) {
        nc->setChamfer(hGrp->GetFloat("ChamferSize", 0.12f));
    }
    else if (strcmp(Name, "CubeSize") == 0) {
        nc->setSize(hGrp->GetInt("CubeSize", 132));
    }
    else if (strcmp(Name, "NaviScale") == 0) {
        nc->setScale(hGrp->GetFloat("NaviScale", 1.0f));
    }
    else if (strcmp(Name, "NaviRotateToNearest") == 0) {
        nc->setNaviRotateToNearest(hGrp->GetBool("NaviRotateToNearest", true));
    }
    else if (strcmp(Name, "NaviStepByTurn") == 0) {
        nc->setNaviStepByTurn(hGrp->GetInt("NaviStepByTurn", 8));
    }
    else if (strcmp(Name, "FontZoom") == 0) {
        nc->setFontZoom(hGrp->GetFloat("FontZoom", 0.3));
    }
    else if (strcmp(Name, "FontString") == 0) {
        nc->setFont(hGrp->GetASCII("FontString"));
    }
    else if (strcmp(Name, "FontWeight") == 0) {
        nc->setFontWeight(hGrp->GetInt("FontWeight", 0));
    }
    else if (strcmp(Name, "FontStretch") == 0) {
        nc->setFontStretch(hGrp->GetInt("FontStretch", 0));
    }
    else if (strcmp(Name, "BaseColor") == 0) {
        unsigned long col = hGrp->GetUnsigned("BaseColor", 3806916544);
        nc->setBaseColor(Base::Color::fromPackedRGBA<QColor>(col));
        // update default contrast colors
        parameterChanged("EmphaseColor");
    }
    else if (strcmp(Name, "EmphaseColor") == 0) {
        Base::Color bc((uint32_t)hGrp->GetUnsigned("BaseColor", 3806916544));
        unsigned long d = bc.r + bc.g + bc.b >= 1.5f ? 255 : 4294967295;
        unsigned long col = hGrp->GetUnsigned("EmphaseColor", d);
        nc->setEmphaseColor(Base::Color::fromPackedRGBA<QColor>(col));
    }
    else if (strcmp(Name, "HiliteColor") == 0) {
        unsigned long col = hGrp->GetUnsigned("HiliteColor", 2867003391);
        nc->setHiliteColor(Base::Color::fromPackedRGBA<QColor>(col));
    }
    else if (strcmp(Name, "BorderWidth") == 0) {
        nc->setBorderWidth(hGrp->GetFloat("BorderWidth", 1.1));
    }
    else if (strcmp(Name, "ShowCS") == 0) {
        nc->setShowCS(hGrp->GetBool("ShowCS", true));
    }
    else if (strcmp(Name, "InactiveOpacity") == 0) {
        float opacity = static_cast<float>(hGrp->GetInt("InactiveOpacity", 50)) / 100;
        nc->setInactiveOpacity(opacity);
    }
    else if (
        strcmp(Name, "TextTop") == 0 || strcmp(Name, "TextBottom") == 0
        || strcmp(Name, "TextFront") == 0 || strcmp(Name, "TextRear") == 0
        || strcmp(Name, "TextLeft") == 0 || strcmp(Name, "TextRight") == 0
    ) {
        std::vector<std::string> labels;
        QByteArray frontByteArray = tr("FRONT").toUtf8();
        labels.push_back(hGrp->GetASCII("TextFront", frontByteArray.constData()));
        QByteArray topByteArray = tr("TOP").toUtf8();
        labels.push_back(hGrp->GetASCII("TextTop", topByteArray.constData()));
        QByteArray rightByteArray = tr("RIGHT").toUtf8();
        labels.push_back(hGrp->GetASCII("TextRight", rightByteArray.constData()));
        QByteArray rearByteArray = tr("REAR").toUtf8();
        labels.push_back(hGrp->GetASCII("TextRear", rearByteArray.constData()));
        QByteArray bottomByteArray = tr("BOTTOM").toUtf8();
        labels.push_back(hGrp->GetASCII("TextBottom", bottomByteArray.constData()));
        QByteArray leftByteArray = tr("LEFT").toUtf8();
        labels.push_back(hGrp->GetASCII("TextLeft", leftByteArray.constData()));
        nc->setNaviCubeLabels(labels);
    }
    _viewer->getSoRenderManager()->scheduleRedraw();
}
