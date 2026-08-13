#pragma once

#include "VulkanResourceAllocator.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/ShadowTypes.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>

class DescriptorAllocator;

namespace Iridium {

    class VulkanUploadContext;

    struct alignas(16) VulkanDirectionalShadowData {
        std::array<glm::mat4, kDirectionalShadowLayerCount>
            worldToShadowClip{};
        std::array<glm::vec4, kDirectionalShadowLightCapacity> splitFar{};
        // Per owner: light slot, sampleable mask, first array layer, enabled.
        std::array<glm::uvec4, kDirectionalShadowLightCapacity> metadata{};
        std::array<glm::vec4, kDirectionalShadowLightCapacity>
            texelWorldSize{};
        std::array<glm::vec4, kDirectionalShadowLightCapacity>
            depthSpanMeters{};
        // tan(source angular radius), maximum penumbra texels, reserved.
        std::array<glm::vec4, kDirectionalShadowLightCapacity>
            filterParameters{};
        // Blocker samples, filter samples, contact hardening, reserved.
        std::array<glm::uvec4, kDirectionalShadowLightCapacity>
            filterMetadata{};
        // Raster constant/slope, receiver bias in shadow texels, reserved.
        glm::vec4 biasParameters{ 0.5f, 1.0f, 1.25f, 0.0f };
    };

    static_assert(sizeof(VulkanDirectionalShadowData) == 720);

    class VulkanDirectionalShadowMap final {
    public:
        void init(VkDevice device, VulkanResourceAllocator& allocator,
            VulkanUploadContext& uploads, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout materialLayout,
            VkDescriptorSetLayout samplerLayout, uint32_t resolution);
        void cleanup() noexcept;

        void updateFrame(uint32_t frameIndex,
            std::span<const DirectionalShadowFramePacket> packets);
        void beginCascade(VkCommandBuffer commandBuffer,
            uint32_t shadowIndex, uint32_t cascadeIndex) const;
        void endCascade(VkCommandBuffer commandBuffer) const;

        [[nodiscard]] VkPipeline pipeline(bool alphaMasked,
            bool doubleSided) const noexcept;
        [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept {
            return pipelineLayout_;
        }
        [[nodiscard]] VkDescriptorSet renderDescriptor(uint32_t frameIndex) const;
        [[nodiscard]] VkDescriptorImageInfo sampleImage() const noexcept;
        [[nodiscard]] VkDescriptorBufferInfo sampleBuffer(
            uint32_t frameIndex) const noexcept;
        [[nodiscard]] uint32_t resolution() const noexcept { return resolution_; }

    private:
        VkPipeline createPipeline(bool alphaMasked, bool doubleSided);
        VkShaderModule createShaderModule(const char* relativePath) const;

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        VulkanImageResource image_;
        std::array<VkImageView, kDirectionalShadowLayerCount> layerViews_{};
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        std::array<VkFramebuffer, kDirectionalShadowLayerCount> framebuffers_{};
        VkDescriptorSetLayout renderSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        std::array<VkPipeline, 4> pipelines_{};
        std::array<VulkanBufferResource, 2> frameBuffers_{};
        std::array<VkDescriptorSet, 2> renderSets_{};
        ::DescriptorAllocator* descriptors_ = nullptr;
        uint32_t resolution_ = 0;
    };

} // namespace Iridium
