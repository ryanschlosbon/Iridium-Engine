#pragma once

#include "renderer/rhi/GBufferLayout.h"

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
}
