// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QObject>

#include <memory>

QT_BEGIN_NAMESPACE
class QScreen;
QT_END_NAMESPACE

namespace Gui
{

/** Detected HDR luminance of the display a viewport is on.
 *
 *  On a Wayland session whose compositor implements the `wp_color_manager_v1`
 *  color-management protocol this reads the output's reference-white / peak
 *  luminance (cd/m²) from the output's image description.  On every other
 *  platform (or a compositor without the protocol) `hasValue()` stays false and
 *  callers fall back to the built-in reference-white convention.
 *
 *  The value drives the HDR output exposure: presenting at `referenceWhiteNits`
 *  makes an HDR viewport reproduce the same SDR white level the compositor uses
 *  for SDR content.  The probe is asynchronous (the compositor answers over the
 *  Wayland event loop), so `changed()` is emitted on the GUI thread when a value
 *  (or a new one, e.g. after an output reconfiguration) arrives.
 *
 *  A singleton because the protocol objects are global to the connection and
 *  the result is shared by every viewport.
 */
class DisplayLuminance: public QObject
{
    Q_OBJECT

public:
    static DisplayLuminance& instance();

    //! True once a luminance has been read from the compositor.
    bool hasValue() const;
    //! Reference (SDR) white luminance in cd/m².  0 when unknown.
    float referenceWhiteNits() const;
    //! Peak luminance in cd/m² (best effort).  0 when unknown.
    float maxNits() const;
    //! Minimum luminance in cd/m².  0 when unknown.
    float minNits() const;
    //! True when the output the viewport is on is actually driven in an HDR
    //! mode.  The compositor describes an HDR/wide-gamut output with a PQ or
    //! HLG transfer function (or a peak luminance well above SDR white), so
    //! this is the authoritative "is HDR live on this screen" signal.  It is
    //! false until the asynchronous probe answers.
    bool isHdrOutput() const;

    //! Start (or restart) the probe for \a screen.  Safe to call repeatedly
    //! and a no-op off Wayland / without the protocol.
    void query(QScreen* screen);

Q_SIGNALS:
    //! A luminance value was read or updated.
    void changed();

private:
    explicit DisplayLuminance(QObject* parent = nullptr);
    ~DisplayLuminance() override;

    struct Private;
    std::unique_ptr<Private> d;
};

}  // namespace Gui
