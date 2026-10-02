#pragma once

#include "VulkanResourceAllocator.h"
#include "VulkanVirtualShadowMarkingPass.h"
#include "VulkanVirtualShadowDepthReceiverPass.h"
#include "VulkanVirtualShadowFullViewPass.h"
#include "renderer/rhi/VirtualShadowMap.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Iridium {

    struct VulkanVirtualShadowResourceInfo {
        VirtualShadowAtlasLayout atlasLayout{};
        VkExtent2D physicalAtlasExtent{};
        uint32_t physicalPageCapacity = 0;
        uint32_t pageTableEntryCapacity = 0;
        uint32_t tileFootprintTexels = 0;
        uint64_t requestedPhysicalPoolBytes = 0;
        uint64_t requestedPageTableBytes = 0;
        VirtualShadowGpuWorkingSetLayout workingSetLayout{};
        uint32_t workingSetCount = 0;
        uint64_t requestedWorkingSetBytes = 0;
        uint64_t requestedReadbackBytes = 0;
        uint32_t requestCapacity = 0;
    };

    // Owns persistent storage and immutable marking/compaction descriptors.
    // Frame dispatch requires a qualified receiver producer; sampling is gated.
    class VulkanVirtualShadowResources final {
    public:
        void init(VkDevice device, VulkanResourceAllocator& allocator,
            const VkPhysicalDeviceLimits& limits,
            const VirtualShadowResourceConfig& config,
            uint32_t workingSetCount);
        void cleanup() noexcept;

        [[nodiscard]] bool initialized() const noexcept {
            return physicalPool_.isValid() && pageTable_.isValid() &&
                !workingSets_.empty();
        }
        [[nodiscard]] const VulkanVirtualShadowResourceInfo& info() const noexcept {
            return info_;
        }
        [[nodiscard]] const VulkanBufferResource& workingSet(
            uint32_t frameIndex) const {
            return workingSets_.at(frameIndex);
        }
        [[nodiscard]] const VulkanVirtualShadowMarkingPass& markingPass() const {
            return markingPass_;
        }
        [[nodiscard]] VulkanVirtualShadowDepthReceiverPass& depthReceiverPass() {
            return depthReceiverPass_;
        }
        [[nodiscard]] VulkanVirtualShadowFullViewPass& fullViewPass() {
            return fullViewPass_;
        }
        [[nodiscard]] VkSampler depthSampler() const noexcept { return depthSampler_; }
        [[nodiscard]] const VulkanBufferResource& requestReadback(uint32_t slot) const { return readbacks_.at(slot); }

    private:
        VulkanResourceAllocator* allocator_ = nullptr;
        VulkanImageResource physicalPool_;
        VulkanBufferResource pageTable_;
        std::vector<VulkanBufferResource> workingSets_;
        VulkanVirtualShadowMarkingPass markingPass_;
        VulkanVirtualShadowDepthReceiverPass depthReceiverPass_;
        VulkanVirtualShadowFullViewPass fullViewPass_;
        VulkanVirtualShadowResourceInfo info_{};
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampler depthSampler_ = VK_NULL_HANDLE;
        std::vector<VulkanBufferResource> readbacks_;
    };

} // namespace Iridium
