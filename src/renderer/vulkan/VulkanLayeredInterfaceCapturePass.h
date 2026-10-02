#pragma once

#include "renderer/transparency/LayeredGlass.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    class VulkanFrameTargets;

    // Owns the shared material-aware layered capture render pass, pipeline,
    // set-3 image contract, and fence-safe per-frame/per-interface descriptor
    // sets for the conditional Ordinary2/Hero4/Cinematic8 target chains.
    // The graphics pipeline is created during renderer initialization so
    // enabling an already prepared atlas never compiles a pipeline mid-frame.
    class VulkanLayeredInterfaceCapturePass final {
    public:
        void init(VkDevice device, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout globalLayout,
            VkDescriptorSetLayout materialLayout,
            VkDescriptorSetLayout samplerLayout);
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
        [[nodiscard]] VkDescriptorSet descriptorSet(
            uint32_t frameIndex, bool exitCapture) const;
        [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t frameIndex,
            TransparencyQuality quality, uint32_t interfaceIndex) const;
        [[nodiscard]] uint32_t descriptorInterfaceCount(
            uint32_t frameIndex, TransparencyQuality quality) const;
        void recordTileTermination(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, TransparencyQuality quality,
            uint32_t interfaceIndex, VkExtent2D atlasExtent) const;
        [[nodiscard]] VkPipeline tileTerminationPipeline() const noexcept {
            return tileTerminationPipeline_;
        }
        [[nodiscard]] size_t descriptorFrameCount() const noexcept {
            return descriptorSets_.size();
        }

    private:
        [[nodiscard]] VkShaderModule createShaderModule(
            const char* relativePath) const;
        [[nodiscard]] VkPipeline createPipeline() const;

        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* descriptors_ = nullptr;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout captureLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout tileTerminationLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout tileTerminationPipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline tileTerminationPipeline_ = VK_NULL_HANDLE;
        struct FrameDescriptorSets {
            std::array<VkDescriptorSet, 2> ordinary2{};
            std::array<VkDescriptorSet, 4> hero4{};
            std::array<VkDescriptorSet, kMaximumLayeredInterfaceCount>
                cinematic8{};
            std::array<VkDescriptorSet, 4> hero4Termination{};
            std::array<VkDescriptorSet, kMaximumLayeredInterfaceCount>
                cinematic8Termination{};
            uint32_t ordinary2Count = 0u;
            uint32_t hero4Count = 0u;
            uint32_t cinematic8Count = 0u;
        };

        std::vector<FrameDescriptorSets> descriptorSets_;
    };

} // namespace Iridium
