#pragma once

#include "VulkanResourceAllocator.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/ShadowTypes.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    class VulkanUploadContext;

    struct alignas(16) VulkanPointShadowEntry {
        std::array<glm::mat4, 6> worldToShadowClip{};
        glm::vec4 lightPositionFar{};
        // Light slot, sampleable, pool tier, cube index.
        glm::uvec4 metadata{};
        // Projection A/B, reserved, receiver bias in shadow texels.
        glm::vec4 depthBias{};
        // Local source radius, maximum penumbra texels, reserved, reserved.
        glm::vec4 filterParameters{};
        // Blocker samples, filter samples, contact hardening, reserved.
        glm::uvec4 filterMetadata{};
    };

    struct alignas(16) VulkanPointShadowData {
        std::array<VulkanPointShadowEntry,
            kPointShadowEntryCapacity> entries{};
        // Active entries and three pool capacities.
        glm::uvec4 metadata{};
    };

    static_assert(sizeof(VulkanPointShadowEntry) == 464);
    static_assert(sizeof(VulkanPointShadowData) == 26000);

    class VulkanPointShadowPools final {
    public:
        VulkanPointShadowPools() = default;
        VulkanPointShadowPools(const VulkanPointShadowPools&) = delete;
        VulkanPointShadowPools& operator=(const VulkanPointShadowPools&) = delete;

        void init(VkDevice device, VulkanResourceAllocator& allocator,
            VulkanUploadContext& uploads, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout materialLayout,
            VkDescriptorSetLayout samplerLayout,
            std::array<uint32_t, 3> capacities);
        void cleanup() noexcept;

        void updateFrame(uint32_t frameIndex,
            std::span<const PointShadowFramePacket> packets);
        void beginFace(VkCommandBuffer commandBuffer,
            const PointShadowFramePacket& packet, uint32_t face) const;
        void endFace(VkCommandBuffer commandBuffer) const;

        [[nodiscard]] VkPipeline pipeline(bool alphaMasked,
            bool doubleSided) const noexcept;
        [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept {
            return pipelineLayout_;
        }
        [[nodiscard]] VkDescriptorSet renderDescriptor(
            uint32_t frameIndex) const;
        [[nodiscard]] std::array<VkDescriptorImageInfo, 3>
            sampleImages() const noexcept;
        [[nodiscard]] VkDescriptorBufferInfo sampleBuffer(
            uint32_t frameIndex) const noexcept;
        [[nodiscard]] std::array<uint32_t, 3> capacities() const noexcept {
            return capacities_;
        }

    private:
        struct Pool {
            uint32_t resolution = 0;
            uint32_t capacity = 0;
            VulkanImageResource image;
            std::vector<VkImageView> layerViews;
            std::vector<VkFramebuffer> framebuffers;
        };

        [[nodiscard]] static uint32_t poolIndex(uint32_t resolution);
        VkPipeline createPipeline(bool alphaMasked, bool doubleSided);
        VkShaderModule createShaderModule(const char* relativePath) const;

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        std::array<Pool, 3> pools_{};
        std::array<uint32_t, 3> capacities_{};
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout renderSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        std::array<VkPipeline, 4> pipelines_{};
        std::array<VulkanBufferResource, 2> frameBuffers_{};
        std::array<VkDescriptorSet, 2> renderSets_{};
        ::DescriptorAllocator* descriptors_ = nullptr;
    };

} // namespace Iridium
