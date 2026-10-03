#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    class VulkanFrameTargets;

    // Owns the WeightedOIT accumulation/resolve pipelines (dynamic rendering)
    // and per-frame resolve descriptors. Frame-sized images remain
    // graph/frame-target owned and are conditionally resident.
    class VulkanWeightedOitPass final {
    public:
        VulkanWeightedOitPass() = default;
        VulkanWeightedOitPass(const VulkanWeightedOitPass&) = delete;
        VulkanWeightedOitPass& operator=(const VulkanWeightedOitPass&) = delete;

        void init(VkDevice device, VkPipelineCache pipelineCache,
            ::DescriptorAllocator& descriptors,
            VkPipelineLayout forwardPipelineLayout);
        void rebuildDescriptors(const VulkanFrameTargets& frameTargets);
        void clearDescriptors() noexcept;
        void cleanup() noexcept;

        [[nodiscard]] VkPipeline accumulationPipeline() const noexcept {
            return accumulationPipeline_;
        }
        [[nodiscard]] VkPipelineLayout accumulationPipelineLayout() const
            noexcept {
            return accumulationPipelineLayout_;
        }
        [[nodiscard]] VkPipeline resolvePipeline() const noexcept {
            return resolvePipeline_;
        }
        [[nodiscard]] VkPipelineLayout resolvePipelineLayout() const noexcept {
            return resolvePipelineLayout_;
        }
        [[nodiscard]] VkDescriptorSet resolveDescriptorSet(
            uint32_t frameIndex) const;
        [[nodiscard]] size_t descriptorFrameCount() const noexcept {
            return resolveDescriptorSets_.size();
        }

    private:
        [[nodiscard]] VkPipeline createAccumulationPipeline() const;
        [[nodiscard]] VkPipeline createResolvePipeline() const;
        [[nodiscard]] VkShaderModule createShaderModule(
            const char* relativePath) const;

        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* descriptors_ = nullptr;
        VkPipelineLayout accumulationPipelineLayout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout resolveDescriptorLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout resolvePipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline accumulationPipeline_ = VK_NULL_HANDLE;
        VkPipeline resolvePipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> resolveDescriptorSets_;
    };

} // namespace Iridium
