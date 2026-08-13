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

    struct alignas(16) VulkanSpotShadowEntry {
        glm::mat4 worldToShadowClip{ 1.0f };
        // Inner, guard-free atlas region: scale.xy, bias.xy.
        glm::vec4 atlasScaleBias{};
        // Light slot, sampleable, tile resolution, stale age.
        glm::uvec4 metadata{};
        // Receiver bias in shadow texels, reserved, reserved, reserved.
        glm::vec4 biasParameters{ 1.0f, 0.0f, 0.0f, 0.0f };
        // Near/far planes, local source radius, tangent of the outer cone.
        glm::vec4 projectionParameters{};
        // Blocker samples, filter samples, contact hardening, max penumbra.
        glm::uvec4 filterMetadata{};
    };

    struct alignas(16) VulkanSpotShadowData {
        std::array<VulkanSpotShadowEntry, kSpotShadowEntryCapacity> entries{};
        // Atlas resolution, active entry count, reserved, reserved.
        glm::uvec4 metadata{};
    };

    static_assert(sizeof(VulkanSpotShadowEntry) == 144);
    static_assert(sizeof(VulkanSpotShadowData) == 36880);

    class VulkanSpotShadowAtlas final {
    public:
        VulkanSpotShadowAtlas() = default;
        VulkanSpotShadowAtlas(const VulkanSpotShadowAtlas&) = delete;
        VulkanSpotShadowAtlas& operator=(const VulkanSpotShadowAtlas&) = delete;
        void init(VkDevice device, VulkanResourceAllocator& allocator,
            VulkanUploadContext& uploads, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout materialLayout,
            VkDescriptorSetLayout samplerLayout, uint32_t resolution);
        void cleanup() noexcept;

        void updateFrame(uint32_t frameIndex,
            std::span<const SpotShadowFramePacket> packets);
        void beginTile(VkCommandBuffer commandBuffer,
            const SpotShadowFramePacket& packet) const;
        void endTile(VkCommandBuffer commandBuffer) const;

        [[nodiscard]] VkPipeline pipeline(bool alphaMasked,
            bool doubleSided) const noexcept;
        [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept {
            return pipelineLayout_;
        }
        [[nodiscard]] VkDescriptorSet renderDescriptor(
            uint32_t frameIndex) const;
        [[nodiscard]] VkDescriptorImageInfo sampleImage() const noexcept;
        [[nodiscard]] VkDescriptorBufferInfo sampleBuffer(
            uint32_t frameIndex) const noexcept;
        [[nodiscard]] uint32_t resolution() const noexcept {
            return resolution_;
        }

    private:
        VkPipeline createPipeline(bool alphaMasked, bool doubleSided);
        VkShaderModule createShaderModule(const char* relativePath) const;

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        VulkanImageResource image_;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout renderSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        std::array<VkPipeline, 4> pipelines_{};
        std::array<VulkanBufferResource, 2> frameBuffers_{};
        std::array<VkDescriptorSet, 2> renderSets_{};
        ::DescriptorAllocator* descriptors_ = nullptr;
        uint32_t resolution_ = 0;
    };

} // namespace Iridium
