// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <QVulkanDeviceFunctions>
#include <QVulkanInstance>
#include <QVulkanWindow>

#include <vulkan/vulkan.h>

#include <Inventor/rendering/vulkan/SoVulkanImageCopy.h>

#include <Base/Console.h>
#include <Base/FileInfo.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SIM {
namespace Coin3D {
namespace Quarter {
namespace Detail {

//! Env-gated frame-dump helper (FC_VULKAN_DUMP_FRAME).
//!
//! When disabled every method is a no-op and no Vulkan resources are
//! allocated.  When enabled it keeps a host-visible staging buffer sized to
//! the swapchain, records a copy of the presented color image into it inside
//! the frame's command buffer, and writes the mapped pixels to
//! /tmp/vk_frame_<n>.png after submission.  This captures the exact pixels
//! the backend rasterized (QVulkanWindow::grab() renders its own frame).
//! The dump window is controlled with FC_VULKAN_DUMP_START /
//! FC_VULKAN_DUMP_END (defaults 240-245).
//!
//! Swapchain-side counterpart of Coin's SoVulkanShared::dumpImageToHost()
//! (which the RT storage-image dump uses).  They share the staging-buffer +
//! copy + map + write shape and differ only in command-buffer ownership: this
//! one records into the frame's own command buffer (no submit/wait, to avoid
//! stalling the pipeline), while the Coin helper owns a one-shot submit.
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
class VulkanFrameDumper
{
public:
    VulkanFrameDumper(QVulkanInstance * instance, QVulkanWindow * window)
        : m_instance(instance)
        , m_window(window)
        , m_enabled(qEnvironmentVariableIsSet("FC_VULKAN_DUMP_FRAME"))
    {
        if (m_enabled) {
            m_dumpStart = qEnvironmentVariableIntValue("FC_VULKAN_DUMP_START");
            m_dumpEnd = qEnvironmentVariableIntValue("FC_VULKAN_DUMP_END");
            if (m_dumpStart < 0) {
                m_dumpStart = 240;
            }
            if (m_dumpEnd <= 0) {
                m_dumpEnd = 246;
            }
        }
    }

    void initSwapChainResources()
    {
        if (!m_enabled || m_buffer != VK_NULL_HANDLE) {
            return;
        }
        // The dumper assumes an 8-bit-per-channel, 4-bytes-per-pixel color
        // format (the QImage formats below are byte-order-specific).
        // Disable it for anything else instead of under-sizing the staging
        // buffer and letting vkCmdCopyImageToBuffer write out of bounds.
        const VkFormat colorFormat = m_window->colorFormat();
        switch (colorFormat) {
            case VK_FORMAT_B8G8R8A8_UNORM:
                m_qimageFormat = QImage::Format_ARGB32;
                m_bytesPerPixel = 4;
                break;
            case VK_FORMAT_R8G8B8A8_UNORM:
                m_qimageFormat = QImage::Format_RGBA8888;
                m_bytesPerPixel = 4;
                break;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                // The HDR (scRGB) swapchain: 4 half-floats, linear light.
                // saveFrame() converts it to an sRGB-encoded 8-bit image so the
                // dump is viewable and directly comparable with the SDR one.
                m_halfFloat = true;
                m_bytesPerPixel = 8;
                break;
            default:
                Base::Console().warning("[Vulkan] frame dump: unsupported "
                                        "color format {}, disabling\n",
                                        static_cast<int>(colorFormat));
                m_enabled = false;
                return;
        }
        const QSize imgSize = m_window->swapChainImageSize();
        if (imgSize.width() <= 0 || imgSize.height() <= 0) {
            return;
        }

        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        VkBufferCreateInfo bci {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = static_cast<VkDeviceSize>(imgSize.width())
            * static_cast<VkDeviceSize>(imgSize.height()) * m_bytesPerPixel;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkdf->vkCreateBuffer(m_window->device(), &bci, nullptr, &buf)
            != VK_SUCCESS) {
            Base::Console().error("[Vulkan] frame dump staging buffer "
                                  "creation failed\n");
            return;
        }

        VkMemoryRequirements memReq {};
        vkdf->vkGetBufferMemoryRequirements(m_window->device(), buf, &memReq);
        VkMemoryAllocateInfo mai {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = findMemoryType(
            memReq,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            Base::Console().error("[Vulkan] frame dump: no host-visible "
                                  "memory type available\n");
            vkdf->vkDestroyBuffer(m_window->device(), buf, nullptr);
            return;
        }
        if (vkdf->vkAllocateMemory(m_window->device(), &mai, nullptr, &mem)
                != VK_SUCCESS
            || vkdf->vkBindBufferMemory(m_window->device(), buf, mem, 0)
                != VK_SUCCESS) {
            Base::Console().error("[Vulkan] frame dump staging buffer alloc "
                                  "failed\n");
            if (mem != VK_NULL_HANDLE) {
                vkdf->vkFreeMemory(m_window->device(), mem, nullptr);
            }
            vkdf->vkDestroyBuffer(m_window->device(), buf, nullptr);
            return;
        }

        m_buffer = buf;
        m_memory = mem;
        m_size = imgSize;
        Base::Console().log("[Vulkan] frame dump staging buffer: %dx%d "
                            "(%llu bytes)\n",
                            imgSize.width(), imgSize.height(),
                            static_cast<unsigned long long>(bci.size));
    }

    void releaseSwapChainResources()
    {
        if (m_buffer == VK_NULL_HANDLE) {
            return;
        }
        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        vkdf->vkDestroyBuffer(m_window->device(), m_buffer, nullptr);
        if (m_memory != VK_NULL_HANDLE) {
            vkdf->vkFreeMemory(m_window->device(), m_memory, nullptr);
        }
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    //! Record the copy of the presented color image into the staging buffer
    //! inside \a cb (no-op when disabled, outside the dump window, or
    //! without a staging buffer).
    void recordFrameCopy(VkCommandBuffer cb, int swapchainIndex, const QSize & size, quint64 frameOrdinal)
    {
        if (!m_enabled || m_buffer == VK_NULL_HANDLE) {
            return;
        }
        m_frameCount++;
        m_frameOrdinal = frameOrdinal;
        if (m_frameCount < m_dumpStart || m_frameCount >= m_dumpEnd) {
            return;
        }
        m_dumpCount++;

        // The swapchain image is in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR here:
        // QVulkanWindow's default render pass ends with the color attachment
        // in the present layout and QVulkanWindow presents it directly after
        // frameReady().  (It is NOT COLOR_ATTACHMENT_OPTIMAL at this point —
        // assuming that triggers VUID-VkImageMemoryBarrier-oldLayout-01197.)
        // So transition PRESENT_SRC_KHR -> TRANSFER_SRC for the copy, then
        // back to PRESENT_SRC_KHR so the present never sees a stray
        // attachment layout (VUID-VkPresentInfoKHR-pImageIndices-01430).
        VkImage srcImage = m_window->swapChainImage(swapchainIndex);
        SoVulkanImageCopy::recordToBuffer(
            cb, srcImage, m_buffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            static_cast<uint32_t>(size.width()),
            static_cast<uint32_t>(size.height()));
    }

    //! Write the staging buffer to a PNG after frame submission (no-op when
    //! disabled, without a buffer, or after the dump window).
    void saveFrame()
    {
        // recordFrameCopy() dumps frames in the half-open window
        // [m_dumpStart, m_dumpEnd), i.e. m_dumpEnd - m_dumpStart frames.
        const int windowFrames = m_dumpEnd - m_dumpStart;
        if (!m_enabled || m_buffer == VK_NULL_HANDLE || m_dumpCount <= 0
            || m_dumpCount > windowFrames) {
            return;
        }
        // Called after QVulkanWindow::frameReady(), which QVulkanWindow
        // only emits once this frame's fence has signaled, so the recorded
        // copy has completed and the staging buffer is safe to read.  A
        // vkQueueWaitIdle here would additionally drain unrelated in-flight
        // frames and stall the whole pipeline.
        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        void * data = nullptr;
        if (vkdf->vkMapMemory(m_window->device(), m_memory, 0,
                              VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS) {
            return;
        }
        QImage img;
        if (m_halfFloat) {
            // FP16 scRGB: linear light.  Encode to sRGB for a viewable 8-bit
            // PNG so the HDR frame can be compared against the SDR dump.
            img = QImage(m_size, QImage::Format_ARGB32);
            const auto * src = static_cast<const quint16 *>(data);
            for (int y = 0; y < m_size.height(); ++y) {
                auto * line = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < m_size.width(); ++x) {
                    const quint16 * p = src
                        + (static_cast<size_t>(y) * m_size.width() + x) * 4;
                    line[x] = qRgba(linearToSrgb8(halfToFloat(p[0])),
                                    linearToSrgb8(halfToFloat(p[1])),
                                    linearToSrgb8(halfToFloat(p[2])), 255);
                }
            }
        }
        else {
            img = QImage(static_cast<const uchar *>(data),
                         m_size.width(), m_size.height(),
                         static_cast<qsizetype>(m_size.width())
                             * m_bytesPerPixel,
                         m_qimageFormat);
        }
        // Use the OS temp dir (Base::FileInfo::getTempPath) rather than a
        // hardcoded /tmp so dumps land somewhere writeable on Windows/macOS.
        const QString path =
            QString::fromStdString(Base::FileInfo::getTempPath())
            + QStringLiteral("vk_frame_%1.png").arg(m_frameOrdinal);
        if (!img.isNull() && img.save(path)) {
            Base::Console().log("[Vulkan] frame dump %d (ordinal %llu): "
                                "%dx%d -> %s\n",
                                m_dumpCount,
                                static_cast<unsigned long long>(m_frameOrdinal),
                                img.width(), img.height(),
                                qPrintable(path));
        }
        else {
            Base::Console().error("[Vulkan] frame dump {} (ordinal {}): "
                                  "image save failed\n",
                                  m_dumpCount,
                                  static_cast<unsigned long long>(m_frameOrdinal));
        }
        vkdf->vkUnmapMemory(m_window->device(), m_memory);
    }

private:
    // IEEE 754 binary16 -> binary32.
    static float halfToFloat(quint16 h)
    {
        const quint32 sign = static_cast<quint32>(h & 0x8000u) << 16;
        quint32 exp = (h >> 10) & 0x1fu;
        quint32 mant = h & 0x3ffu;
        quint32 bits;
        if (exp == 0) {
            if (mant == 0) {
                bits = sign;
            }
            else {
                exp = 127 - 15 + 1;
                while ((mant & 0x400u) == 0) {
                    mant <<= 1;
                    --exp;
                }
                mant &= 0x3ffu;
                bits = sign | (exp << 23) | (mant << 13);
            }
        }
        else if (exp == 31) {
            bits = sign | 0x7f800000u | (mant << 13);
        }
        else {
            bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
        }
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // Linear scRGB -> 8-bit sRGB (clamped).
    static uchar linearToSrgb8(float v)
    {
        v = std::clamp(v, 0.0f, 1.0f);
        const float s = v <= 0.0031308f
            ? v * 12.92f
            : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<uchar>(std::lround(s * 255.0f));
    }

    // Returns UINT32_MAX when no memory type matches, so callers can fail
    // with a diagnostic instead of silently falling back to type 0.  The
    // device memory properties are queried once and cached (they cannot change
    // for a given physical device).
    uint32_t findMemoryType(const VkMemoryRequirements & memReq,
                            uint32_t props)
    {
        if (!m_memPropsValid) {
            m_instance->functions()->vkGetPhysicalDeviceMemoryProperties(
                m_window->physicalDevice(), &m_memProps);
            m_memPropsValid = true;
        }
        for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1u << i))
                && (m_memProps.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    QVulkanInstance * m_instance = nullptr;
    QVulkanWindow * m_window = nullptr;
    VkPhysicalDeviceMemoryProperties m_memProps {};
    bool m_memPropsValid = false;
    bool m_enabled = false;
    int m_dumpStart = 240;
    int m_dumpEnd = 246;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    QSize m_size;
    int m_bytesPerPixel = 0;
    bool m_halfFloat = false;
    QImage::Format m_qimageFormat = QImage::Format_ARGB32;
    int m_dumpCount = 0;
    int m_frameCount = 0;
    quint64 m_frameOrdinal = 0;
};
#else
// Release build without debug hooks: same interface, but no staging buffer,
// no vkCmdCopyImageToBuffer and no PNG write compiled in.  Every method is a
// no-op, so QuarterVulkanWidget's call sites stay unchanged.
class VulkanFrameDumper
{
public:
    VulkanFrameDumper(QVulkanInstance *, QVulkanWindow *) {}
    void initSwapChainResources() {}
    void releaseSwapChainResources() {}
    void recordFrameCopy(VkCommandBuffer, int, const QSize &, quint64) {}
    void saveFrame() {}
};
#endif

} // namespace Detail
} // namespace Quarter
} // namespace Coin3D
} // namespace SIM
