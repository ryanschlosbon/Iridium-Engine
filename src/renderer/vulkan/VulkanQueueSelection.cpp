#include "VulkanQueueSelection.h"

namespace Iridium {

    VulkanTransferQueueChoice selectVulkanTransferQueueFamily(
        std::span<const VkQueueFamilyProperties> families,
        uint32_t graphicsFamily) noexcept {
        const auto unitGranularity = [](const VkQueueFamilyProperties& family) {
            const VkExtent3D& granularity = family.minImageTransferGranularity;
            return granularity.width == 1 && granularity.height == 1 &&
                granularity.depth == 1;
        };
        for (uint32_t index = 0; index < families.size(); ++index) {
            const VkQueueFamilyProperties& family = families[index];
            if (index == graphicsFamily || family.queueCount == 0) continue;
            const VkQueueFlags flags = family.queueFlags;
            if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 &&
                (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 &&
                unitGranularity(family))
                return { index, VulkanTransferQueueKind::DedicatedTransfer };
        }
        for (uint32_t index = 0; index < families.size(); ++index) {
            const VkQueueFamilyProperties& family = families[index];
            if (index == graphicsFamily || family.queueCount == 0) continue;
            const VkQueueFlags flags = family.queueFlags;
            // A compute queue also supports transfer commands (the TRANSFER bit
            // is optional to report for graphics/compute families).
            if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                (flags & VK_QUEUE_GRAPHICS_BIT) == 0)
                return { index, VulkanTransferQueueKind::AsyncCompute };
        }
        return { graphicsFamily, VulkanTransferQueueKind::Graphics };
    }

    std::string_view vulkanTransferQueueKindName(
        VulkanTransferQueueKind kind) noexcept {
        switch (kind) {
        case VulkanTransferQueueKind::DedicatedTransfer: return "dedicated-transfer";
        case VulkanTransferQueueKind::AsyncCompute: return "async-compute";
        case VulkanTransferQueueKind::Graphics: return "graphics";
        }
        return "graphics";
    }

} // namespace Iridium
