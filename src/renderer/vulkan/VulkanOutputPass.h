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
            const ViewportGridOverlay& gridOverlay, bool motionVectorView = false,
            bool autoExposure = false) const;
        // M9.5: binding 5, the exposure state the shader reads in auto mode.
        // The fallback is bound by rebuildDescriptors (set it before); a
        // frame with an adapted state rebinds its slot's set (not in flight).
        void setExposureFallback(VkBuffer buffer) noexcept { exposureFallback_ = buffer; }
        void setExposureBuffer(uint32_t frameIndex, VkBuffer buffer) const;
        // M9.2: points binding 0 at this frame's resolved scene colour (the
        // TAA history slot changes per frame). The slot's set is not in flight.
        void setSceneView(uint32_t frameIndex, VkImageView view, VkSampler sampler) const;
        void cleanup();

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkBuffer exposureFallback_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
    };

} // namespace Iridium
