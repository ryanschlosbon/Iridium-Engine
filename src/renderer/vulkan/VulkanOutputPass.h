#pragma once

#include "DescriptorAllocator.h"
#include "VkContext.h"

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

        void init(VkContext& context, ::DescriptorAllocator& allocator,
            VkFormat outputFormat);
        void rebuildDescriptors(const VulkanFrameTargets& frameTargets,
            VkImageView lutView = VK_NULL_HANDLE,
            VkSampler lutSampler = VK_NULL_HANDLE);
        void clearDescriptors();
        void record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
            VkFramebuffer framebuffer, VkExtent2D extent,
            float manualExposureEv, uint32_t outputOperator,
            uint32_t outputTransport, float paperWhiteNits,
            float peakNits, bool selectionActive) const;
        void cleanup();

        [[nodiscard]] VkRenderPass renderPass() const noexcept {
            return renderPass_;
        }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
    };

} // namespace Iridium
