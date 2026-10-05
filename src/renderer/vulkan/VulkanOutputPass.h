#pragma once

#include "DescriptorAllocator.h"
#include "VkContext.h"
#include "renderer/rhi/ViewportGridOverlay.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Iridium {

    class VulkanFrameTargets;

    class VulkanOutputPass final {
    public:
        VulkanOutputPass() = default;
        VulkanOutputPass(const VulkanOutputPass&) = delete;
        VulkanOutputPass& operator=(const VulkanOutputPass&) = delete;
        ~VulkanOutputPass();

        void init(VkContext& context, VkPipelineCache pipelineCache,
            ::DescriptorAllocator& allocator, VkFormat outputFormat);
        void rebuildDescriptors(const VulkanFrameTargets& frameTargets,
            VkImageView lutView = VK_NULL_HANDLE,
            VkSampler lutSampler = VK_NULL_HANDLE);
        void clearDescriptors();
        // Records the transform draw; the caller has begun dynamic rendering
        // on the output target (R4a).
        void record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
            VkExtent2D extent,
            float manualExposureEv, uint32_t outputOperator,
            uint32_t outputTransport, float paperWhiteNits,
            float peakNits, bool selectionActive,
            const ViewportGridOverlay& gridOverlay, bool motionVectorView = false) const;
        void cleanup();

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
    };

} // namespace Iridium
