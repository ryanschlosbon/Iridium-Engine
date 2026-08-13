#pragma once

#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"

#include <vulkan/vulkan.h>

#include <array>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanClusteredLightingPipeline final {
    public:
        static constexpr uint32_t BindingCount = 13;

        VulkanClusteredLightingPipeline() = default;
        VulkanClusteredLightingPipeline(const VulkanClusteredLightingPipeline&) = delete;
        VulkanClusteredLightingPipeline& operator=(
            const VulkanClusteredLightingPipeline&) = delete;
        ~VulkanClusteredLightingPipeline();

        void init(VkDevice device, ::DescriptorAllocator& allocator);
        void rebuildDescriptors(const VulkanRenderGraphExecutor& graph,
            std::span<const VkDescriptorBufferInfo> lightRecords,
            std::span<const VkDescriptorBufferInfo> activeSlots,
            std::span<const VkDescriptorBufferInfo> fallbackCandidates,
            std::span<const VkDescriptorBufferInfo> parameters);
        void clearDescriptors();
        [[nodiscard]] uint32_t record(VkCommandBuffer commandBuffer,
            VulkanRenderGraphExecutor& graph, uint32_t frameIndex,
            uint32_t clusterCount, uint32_t activeLightCount);
        void cleanup() noexcept;

    private:
        struct ScanPushConstants {
            uint32_t mode = 0;
            uint32_t inputOffset = 0;
            uint32_t outputOffset = 0;
            uint32_t elementCount = 0;
        };

        [[nodiscard]] VkPipeline createPipeline(const char* shaderPath);
        void bindAndDispatch(VkCommandBuffer commandBuffer, VkPipeline pipeline,
            uint32_t frameIndex, uint32_t groupsX, uint32_t groupsY = 1);
        static void computeBarrier(VkCommandBuffer commandBuffer);

        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline clearPipeline_ = VK_NULL_HANDLE;
        VkPipeline countPipeline_ = VK_NULL_HANDLE;
        VkPipeline scanPipeline_ = VK_NULL_HANDLE;
        VkPipeline fillPipeline_ = VK_NULL_HANDLE;
        VkPipeline sortPreparePipeline_ = VK_NULL_HANDLE;
        VkPipeline sortPipeline_ = VK_NULL_HANDLE;
        VkPipeline denseSortPipeline_ = VK_NULL_HANDLE;
        VkPipeline finalizePipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
    };

} // namespace Iridium
