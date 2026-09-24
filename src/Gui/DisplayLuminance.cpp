// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "DisplayLuminance.h"

#include <QGuiApplication>
#include <QScreen>

#ifdef FREECAD_HAVE_WAYLAND_COLOR_MANAGEMENT
# include <functional>

# include <QtGui/qguiapplication_platform.h>
# include <QtGui/qscreen_platform.h>
# include <QtWaylandClient/qwaylandclientextension.h>

// Generated at build time from the wayland-protocols color-management XML by
// qt_generate_wayland_protocol_client_sources() (see src/Gui/CMakeLists.txt).
# include "qwayland-color-management-v1.h"
#endif

namespace Gui
{

#ifdef FREECAD_HAVE_WAYLAND_COLOR_MANAGEMENT

namespace
{

//! Result callback: ok, min cd/m², max cd/m², reference-white cd/m².
using LuminanceResult = std::function<void(bool, double, double, double)>;

//! Registered `wp_color_manager_v1` global.  Bound at protocol version 1: the
//! v1 output image description still carries the luminance events we need, and
//! v1 uses the (non-deprecated) `ready` event, so the code works whether the
//! build generated the bindings from the v1 (Qt-shipped) or v3
//! (wayland-protocols) XML.
class ColorManager: public QWaylandClientExtensionTemplate<ColorManager>,
                    public QtWayland::wp_color_manager_v1
{
public:
    ColorManager()
        : QWaylandClientExtensionTemplate(1)
    {}
};

//! Wrapper for the output's image description.  Owns the info object; both
//! delete themselves once the compositor has answered.
class WlImageDescription: public QObject, public QtWayland::wp_image_description_v1
{
public:
    class Info;

    WlImageDescription(::wp_image_description_v1* desc, LuminanceResult result)
        : QtWayland::wp_image_description_v1(desc)
        , result_(std::move(result))
    {}

    void wp_image_description_v1_ready(uint32_t identity) override;
    void wp_image_description_v1_failed(uint32_t cause, const QString& message) override;

private:
    LuminanceResult result_;
};

//! Info-event sink for one image description.  The `done` event ends the
//! (protocol destructor) info object, at which point the accumulated values are
//! reported and the wrapper deletes itself.
class WlImageDescription::Info: public QObject, public QtWayland::wp_image_description_info_v1
{
public:
    Info(::wp_image_description_info_v1* info, LuminanceResult result)
        : QtWayland::wp_image_description_info_v1(info)
        , result_(std::move(result))
    {}

    void wp_image_description_info_v1_luminances(
        uint32_t minHundredths,
        uint32_t max,
        uint32_t reference
    ) override
    {
        min_ = minHundredths;
        max_ = max;
        reference_ = reference;
    }

    void wp_image_description_info_v1_target_luminance(uint32_t /*min*/, uint32_t max) override
    {
        targetMax_ = max;
    }

    void wp_image_description_info_v1_done() override
    {
        if (result_) {
            const double max = targetMax_ != 0 ? targetMax_ : max_;
            // min_lum is scaled by 10000 for 4 decimals; the others are unscaled.
            result_(true, min_ / 10000.0, max, reference_);
        }
        deleteLater();
    }

private:
    LuminanceResult result_;
    uint32_t min_ = 0;
    uint32_t max_ = 0;
    uint32_t reference_ = 0;
    uint32_t targetMax_ = 0;
};

void WlImageDescription::wp_image_description_v1_ready(uint32_t /*identity*/)
{
    if (auto* info = get_information()) {
        new Info(info, std::move(result_));
    }
    else if (result_) {
        result_(false, 0.0, 0.0, 0.0);
    }
    deleteLater();
}

void WlImageDescription::wp_image_description_v1_failed(uint32_t /*cause*/, const QString& /*message*/)
{
    if (result_) {
        result_(false, 0.0, 0.0, 0.0);
    }
    deleteLater();
}

//! Wrapper for the per-output color-management object.
class WlOutput: public QObject, public QtWayland::wp_color_management_output_v1
{
public:
    explicit WlOutput(::wp_color_management_output_v1* output)
        : QtWayland::wp_color_management_output_v1(output)
    {}
};

}  // namespace

struct DisplayLuminance::Private
{
    DisplayLuminance* q = nullptr;
    std::unique_ptr<ColorManager> manager;
    std::unique_ptr<WlOutput> output;
    QScreen* screen = nullptr;

    bool hasValue = false;
    double referenceWhiteNits = 0.0;
    double maxNits = 0.0;
    double minNits = 0.0;

    void apply(bool ok, double min, double max, double reference)
    {
        if (!ok) {
            return;
        }
        hasValue = true;
        minNits = min;
        maxNits = max;
        referenceWhiteNits = reference;
        Q_EMIT q->changed();
    }

    void query(QScreen* target)
    {
        screen = target;
        if (!screen) {
            return;
        }
        if (!manager) {
            manager = std::make_unique<ColorManager>();
            QObject::connect(manager.get(), &QWaylandClientExtension::activeChanged, q, [this] {
                query(screen);
            });
        }
        if (!manager->isActive()) {
            // The global is not bound yet (or absent); activeChanged retries.
            return;
        }

        auto* native = screen->nativeInterface<QNativeInterface::QWaylandScreen>();
        if (!native || !native->output()) {
            return;
        }
        auto* raw = manager->get_output(native->output());
        if (!raw) {
            return;
        }
        output = std::make_unique<WlOutput>(raw);
        auto* desc = output->get_image_description();
        if (!desc) {
            return;
        }
        // The description is immutable and independent of the output, so the
        // wrapper owns itself from here.
        new WlImageDescription(desc, [this](bool ok, double min, double max, double reference) {
            apply(ok, min, max, reference);
        });
    }
};

DisplayLuminance::DisplayLuminance(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
}

DisplayLuminance::~DisplayLuminance() = default;

void DisplayLuminance::query(QScreen* screen)
{
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        return;
    }
    if (!qApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        return;
    }
    d->query(screen);
}

bool DisplayLuminance::hasValue() const
{
    return d->hasValue;
}

float DisplayLuminance::referenceWhiteNits() const
{
    return static_cast<float>(d->referenceWhiteNits);
}

float DisplayLuminance::maxNits() const
{
    return static_cast<float>(d->maxNits);
}

float DisplayLuminance::minNits() const
{
    return static_cast<float>(d->minNits);
}

#else  // FREECAD_HAVE_WAYLAND_COLOR_MANAGEMENT

struct DisplayLuminance::Private
{
};

DisplayLuminance::DisplayLuminance(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{}

DisplayLuminance::~DisplayLuminance() = default;

void DisplayLuminance::query(QScreen* /*screen*/)
{}

bool DisplayLuminance::hasValue() const
{
    return false;
}

float DisplayLuminance::referenceWhiteNits() const
{
    return 0.0f;
}

float DisplayLuminance::maxNits() const
{
    return 0.0f;
}

float DisplayLuminance::minNits() const
{
    return 0.0f;
}

#endif  // FREECAD_HAVE_WAYLAND_COLOR_MANAGEMENT

DisplayLuminance& DisplayLuminance::instance()
{
    static DisplayLuminance* self = new DisplayLuminance();
    return *self;
}

}  // namespace Gui
