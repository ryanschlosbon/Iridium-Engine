#pragma once

#include "renderer/transparency/LayeredGlass.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    class VulkanFrameTargets;

    // Prewarmed geometry-addressed resolve for the packed Ordinary2 AP1 atlas.
    // Local colors are premultiplied and blended over scene color only where the
    // accepted entry-facing shell geometry maps back to a populated atlas pixel.
    class VulkanLayeredSceneResolvePass final {
    public:
        VulkanLayeredSceneResolvePass() = default;
        VulkanLayeredSceneResolvePass(
            const VulkanLayeredSceneResolvePass&) = delete;
        VulkanLayeredSceneResolvePass& operator=(
            const VulkanLayeredSceneResolvePass&) = delete;

        void init(VkDevice device, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout globalLayout, VkRenderPass sceneRenderPass);
        void rebuildDescriptors(const VulkanFrameTargets& frameTargets);
        void clearDescriptors() noexcept;
        void cleanup() noexcept;

        [[nodiscard]] VkPipeline pipeline() const noexcept {
            return pipeline_;
        }
        [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept {
            return pipelineLayout_;
        }
        [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t frameIndex,
            TransparencyQuality quality = TransparencyQuality::Ordinary2)
            const;
        [[nodiscard]] size_t descriptorFrameCount(
            TransparencyQuality quality = TransparencyQuality::Ordinary2)
            const noexcept;

    private:
        [[nodiscard]] VkShaderModule createShaderModule(
            const char* relativePath) const;
        [[nodiscard]] VkPipeline createPipeline(
            VkRenderPass sceneRenderPass) const;

        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* descriptors_ = nullptr;
        VkDescriptorSetLayout localColorLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
        std::vector<VkDescriptorSet> hero4DescriptorSets_;
        std::vector<VkDescriptorSet> cinematic8DescriptorSets_;
    };

} // namespace Iridium
