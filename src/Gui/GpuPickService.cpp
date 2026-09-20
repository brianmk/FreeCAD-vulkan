// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "GpuPickService.h"

using namespace Gui;

GpuPickService& GpuPickService::instance()
{
    static GpuPickService service;
    return service;
}

void GpuPickService::setPicker(const void* owner, Picker newPicker)
{
    std::lock_guard<std::mutex> lock(mutex);
    pickerOwner = owner;
    picker = std::move(newPicker);
}

void GpuPickService::clearPicker(const void* owner)
{
    std::lock_guard<std::mutex> lock(mutex);
    // Only the owner may drop the picker: another view switching to Coin (or
    // being torn down) must not disable GPU picking for a live Vulkan view.
    if (pickerOwner == owner) {
        picker = nullptr;
        pickerOwner = nullptr;
    }
}

bool GpuPickService::available() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<bool>(picker);
}

bool GpuPickService::pick(const float origin[3],
                          const float direction[3],
                          float tMax,
                          GpuPickResult& out) const
{
    // Copy the callback out under the lock, then invoke it unlocked: the
    // picker performs a GPU submit and must not hold the service mutex.
    Picker local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!picker) {
            return false;
        }
        local = picker;
    }
    return local(origin, direction, tMax, out);
}
