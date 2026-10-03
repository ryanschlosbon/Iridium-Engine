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
        void init(VkContext& context, DescriptorAllocator& allocator,
            VkFormat swapchainFormat);
        void rebuild(const VulkanFrameTargets& targets,
            const std::vector<VkImageView>& swapchainViews, VkExtent2D extent);
        void clearTargets();
        void record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
            uint32_t imageIndex, VkExtent2D extent, float paperWhiteNits,
            float peakNits) const;
        void cleanup();

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
        // M7R R4a: dynamic rendering on the acquired image's view. The
        // executor transitions the swapchain (discarded on first use) and
        // moves it to PRESENT at frame end.
        std::vector<VkImageView> swapchainViews_;
    };
}
