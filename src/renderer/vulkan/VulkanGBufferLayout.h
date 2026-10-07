#pragma once

#include "renderer/rhi/GBufferLayout.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Iridium {
    struct VulkanGBufferFormats {
        VkFormat diffuseAo = VK_FORMAT_UNDEFINED;
        VkFormat f0Roughness = VK_FORMAT_UNDEFINED;
        VkFormat normalF90 = VK_FORMAT_UNDEFINED;
        VkFormat emissive = VK_FORMAT_UNDEFINED;
        VkFormat materialFlags = VK_FORMAT_UNDEFINED;
        uint32_t colorBytesPerPixel = 0;
        uint32_t colorAttachmentCount = 0;
        uint32_t metadataBits = 0;
        bool preservesScalarF90 = false;
    };

    [[nodiscard]] constexpr VulkanGBufferFormats vulkanGBufferFormats(
        GBufferLayout layout) noexcept {
        switch (layout) {
        case GBufferLayout::CanonicalReference:
            return { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_R32_UINT, 36, 5, 32, true };
        case GBufferLayout::CanonicalQuality:
            return { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_R16G16_SNORM, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                VK_FORMAT_R16_UINT, 26, 5, 16, false };
        case GBufferLayout::CanonicalCompact:
            return { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_R16G16_SNORM, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                VK_FORMAT_R16_UINT, 18, 5, 16, false };
        }
        return {};
    }

    // M9.1: per-pixel motion, current minus previous unjittered UV (both
    // screen-space, so previousUv = uv - velocity). Appended after the
    // surface targets so attachments 0-4 keep their indices.
    inline constexpr VkFormat VulkanVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
    inline constexpr uint32_t VulkanGBufferPassColorAttachmentCount = 6;

    // Colour attachment formats of the G-buffer pass in attachment order
    // (normal/F90, diffuse/AO, emissive, F0/roughness, material flags): the
    // graph's gbuffer usage-declaration order, which the rendering plan and
    // every G-buffer pipeline share (M7R R4a).
    [[nodiscard]] constexpr std::array<VkFormat, 5> vulkanGBufferColorAttachmentFormats(
        GBufferLayout layout) noexcept {
        const VulkanGBufferFormats formats = vulkanGBufferFormats(layout);
        return { formats.normalF90, formats.diffuseAo, formats.emissive,
            formats.f0Roughness, formats.materialFlags };
    }

    // The G-buffer pass's attachments (M9.1): the surface targets, then velocity.
    [[nodiscard]] constexpr std::array<VkFormat, VulkanGBufferPassColorAttachmentCount>
    vulkanGBufferPassColorAttachmentFormats(GBufferLayout layout) noexcept {
        const VulkanGBufferFormats formats = vulkanGBufferFormats(layout);
        return { formats.normalF90, formats.diffuseAo, formats.emissive,
            formats.f0Roughness, formats.materialFlags, VulkanVelocityFormat };
    }
}
