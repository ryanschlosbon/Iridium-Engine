#pragma once

#include "renderer/rhi/RhiResourceTypes.h"

#include <vulkan/vulkan.h>

namespace Iridium {

    struct VulkanStateInfo {
        VkPipelineStageFlags stages;
        VkAccessFlags access;
        VkImageLayout layout;
    };

    // The aspect identifies depth/stencil images at the API boundary; buffer-only
    // states return VK_IMAGE_LAYOUT_UNDEFINED because their layout is unused.
    [[nodiscard]] VulkanStateInfo getVulkanStateInfo(ResourceState state, VkImageAspectFlags aspect);

} // namespace Iridium
