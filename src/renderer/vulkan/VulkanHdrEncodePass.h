#pragma once

#include "DescriptorAllocator.h"
#include "VkContext.h"

#include <vulkan/vulkan.h>
#include <vector>

namespace Iridium {
    class VulkanFrameTargets;

    class VulkanHdrEncodePass final {
    public:
        ~VulkanHdrEncodePass();
        void init(VkContext& context, VkPipelineCache pipelineCache,
            DescriptorAllocator& allocator, VkFormat swapchainFormat);
        void rebuild(const VulkanFrameTargets& targets);
        void clearTargets();
        // Records the encode draw; the caller has begun dynamic rendering on
        // the acquired swapchain image (M7R R4a: the executor's rendering plan
        // for "hdr10-encode-present"; the executor transitions the swapchain,
        // discarded on first use, and moves it to PRESENT at frame end).
        void record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
            VkExtent2D extent, float paperWhiteNits, float peakNits) const;
        void cleanup();

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
        DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
    };
}
