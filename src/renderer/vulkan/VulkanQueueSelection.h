#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace Iridium {

    // M7R R4d: the queue family uploads use. Selection order (R4 design,
    // section 5): a dedicated transfer family (TRANSFER without GRAPHICS or
    // COMPUTE) whose minImageTransferGranularity is (1,1,1); otherwise a
    // compute family without graphics (compute families always report
    // (1,1,1)); otherwise the graphics family itself.
    enum class VulkanTransferQueueKind : uint8_t {
        DedicatedTransfer,
        AsyncCompute,
        Graphics,
    };

    struct VulkanTransferQueueChoice {
        uint32_t family = 0;
        VulkanTransferQueueKind kind = VulkanTransferQueueKind::Graphics;
    };

    [[nodiscard]] VulkanTransferQueueChoice selectVulkanTransferQueueFamily(
        std::span<const VkQueueFamilyProperties> families,
        uint32_t graphicsFamily) noexcept;

    [[nodiscard]] std::string_view vulkanTransferQueueKindName(
        VulkanTransferQueueKind kind) noexcept;

} // namespace Iridium
