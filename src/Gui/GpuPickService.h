// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <FCGlobal.h>

#include <cstdint>
#include <functional>
#include <mutex>

namespace Gui
{

/** Result of a GPU ray-query pick (Vulkan/RTX viewports only). */
struct GpuPickResult
{
    bool hit = false;
    //! Originating scene-graph shape (SoRenderCommand::userData).
    const void* shape = nullptr;
    //! Triangle index within the hit command's BLAS.
    uint32_t primitiveId = 0;
    //! First triangle of the hit command within the source shape.
    uint32_t primitiveOffset = 0;
    //! World-space hit position.
    float worldPos[3] = {0.0f, 0.0f, 0.0f};
};

/** Process-wide bridge between the Vulkan/RTX viewport and the scene graph.
 *
 *  The Vulkan viewport adapter registers a picker callback while a Vulkan
 *  viewport is displayed; SoBrepFaceSet::rayPick consults it to replace the
 *  O(triangles) CPU traversal with a single GPU ray query against the render
 *  backend's TLAS.  When no picker is registered -- the OpenGL renderer, the
 *  raster Vulkan backend, a viewport without hardware ray tracing, or a view
 *  that is not currently displaying the Vulkan surface -- available() is false
 *  and the CPU SoRayPickAction path runs completely unchanged.
 *
 *  GUI-thread only: the adapter registers/clears and the scene graph queries on
 *  the same thread, but the mutex keeps a stale callback from being invoked
 *  during teardown on another thread.
 */
class GuiExport GpuPickService
{
public:
    using Picker = std::function<bool(const float origin[3],
                                      const float direction[3],
                                      float tMax,
                                      GpuPickResult& out)>;

    static GpuPickService& instance();

    void setPicker(Picker picker);
    void clearPicker();
    bool available() const;
    bool pick(const float origin[3],
              const float direction[3],
              float tMax,
              GpuPickResult& out) const;

private:
    GpuPickService() = default;

    mutable std::mutex mutex;
    Picker picker;
};

}  // namespace Gui
