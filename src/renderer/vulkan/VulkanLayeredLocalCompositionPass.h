#pragma once

#include "renderer/transparency/LayeredGlass.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    class VulkanFrameTargets;

    // Owns the packed-island AP1 material passes. Ordinary2 reads one explicit
    // entry/exit pair. Hero4/Cinematic8 read a bounded per-pixel interface
    // array and composite accepted entries back-to-front. Both variants keep
    // sets 0-3 layout-compatible with the production complex-forward path.
    class VulkanLayeredLocalCompositionPass final {
    public:
        void init(VkDevice device, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout globalLayout,
            VkDescriptorSetLayout materialLayout,
            VkDescriptorSetLayout samplerLayout,
            VkDescriptorSetLayout sceneLayout);
        void rebuildDescriptors(const VulkanFrameTargets& frameTargets);
        void clearDescriptors() noexcept;
        void cleanup() noexcept;

        [[nodiscard]] VkRenderPass renderPass() const noexcept {
            return renderPass_;
        }
        [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept {
            return pipelineLayout_;
        }
        [[nodiscard]] VkPipeline pipeline() const noexcept {
            return pipeline_;
        }
        [[nodiscard]] VkPipelineLayout deepPipelineLayout() const noexcept {
            return deepPipelineLayout_;
        }
        [[nodiscard]] VkPipeline deepPipeline() const noexcept {
            return deepPipeline_;
        }
        [[nodiscard]] VkPipeline deepResidualPipeline() const noexcept {
            return deepResidualPipeline_;
        }
        [[nodiscard]] VkDescriptorSet descriptorSet(
            uint32_t frameIndex) const;
        [[nodiscard]] size_t descriptorFrameCount() const noexcept {
            return descriptorSets_.size();
        }
        [[nodiscard]] VkDescriptorSet deepDescriptorSet(
            uint32_t frameIndex, TransparencyQuality quality) const;
        [[nodiscard]] uint32_t deepDescriptorInterfaceCount(
            uint32_t frameIndex, TransparencyQuality quality) const;

    private:
        [[nodiscard]] VkShaderModule createShaderModule(
            const char* relativePath) const;
        [[nodiscard]] VkPipeline createPipeline(const char* fragmentPath,
            VkPipelineLayout layout, bool premultipliedBlend) const;

        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* descriptors_ = nullptr;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout interfaceLayout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout deepInterfaceLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout deepPipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkPipeline deepPipeline_ = VK_NULL_HANDLE;
        VkPipeline deepResidualPipeline_ = VK_NULL_HANDLE;
        struct FrameDescriptorSets {
            VkDescriptorSet ordinary2 = VK_NULL_HANDLE;
            VkDescriptorSet hero4 = VK_NULL_HANDLE;
            VkDescriptorSet cinematic8 = VK_NULL_HANDLE;
            uint32_t hero4InterfaceCount = 0u;
            uint32_t cinematic8InterfaceCount = 0u;
        };
        std::vector<FrameDescriptorSets> descriptorSets_;
    };

} // namespace Iridium
