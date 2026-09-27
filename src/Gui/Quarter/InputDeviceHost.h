// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <Quarter/Basic.h>

#include <Inventor/SbVec2s.h>

#include <QSize>
#include <QtGlobal>

class SoEvent;

namespace SIM
{
namespace Coin3D
{
namespace Quarter
{

/** Minimal host a Qt→Coin input device needs.
 *
 *  `QuarterWidget` implements it for the OpenGL surface.  A Vulkan surface can
 *  implement it too, so the same `Mouse` / `Keyboard` / `EventFilter` stack can
 *  translate events against whichever surface is current — instead of every
 *  event being forwarded to the hidden GL viewer.
 */
class QUARTER_DLL_API InputDeviceHost
{
public:
    virtual ~InputDeviceHost() = default;

    //! Live device pixel ratio of the surface.
    virtual qreal devicePixelRatio() const = 0;
    //! True when the viewport region is in device pixels (Vulkan-driven).
    virtual bool vulkanDevicePixels() const = 0;
    //! The widget's own logical size (resize tracking / diagnostics).
    virtual QSize inputSize() const = 0;
    //! Logical window size the cursor positions are normalized against.
    virtual SbVec2s inputWindowSize() const = 0;
    //! Deliver a translated Coin event into the interaction pipeline.
    virtual bool processSoEvent(const SoEvent* event) = 0;
};

}  // namespace Quarter
}  // namespace Coin3D
}  // namespace SIM
