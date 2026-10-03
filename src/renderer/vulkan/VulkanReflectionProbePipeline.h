#pragma once

#include "renderer/vulkan/DescriptorAllocator.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanReflectionProbePipeline final {
    public:
        VulkanReflectionProbePipeline() = default;
        VulkanReflectionProbePipeline(const VulkanReflectionProbePipeline&) = delete;
        VulkanReflectionProbePipeline& operator=(
            const VulkanReflectionProbePipeline&) = delete;
        ~VulkanReflectionProbePipeline();

        void init(VkDevice device, ::DescriptorAllocator& allocator);
        void rebuildDescriptors(
            std::span<const VkDescriptorBufferInfo> records,
            std::span<const VkDescriptorBufferInfo> activeSlots,
            std::span<const VkDescriptorBufferInfo> parameters,
            std::span<const VkDescriptorBufferInfo> headers,
            std::span<const VkDescriptorBufferInfo> indices);
        void clearDescriptors();
        // M7R R4c.2: rewrites one retired slot's five bindings in place
        // (no-op before the sets exist).
        void rewriteDescriptors(uint32_t frameIndex,
            const VkDescriptorBufferInfo& records,
            const VkDescriptorBufferInfo& activeSlots,
            const VkDescriptorBufferInfo& parameters,
            const VkDescriptorBufferInfo& headers,
            const VkDescriptorBufferInfo& indices);
        [[nodiscard]] uint32_t record(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, uint32_t clusterCount);
        void cleanup() noexcept;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
    };

} // namespace Iridium
