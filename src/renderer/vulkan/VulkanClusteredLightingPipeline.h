#pragma once

#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanProductionGraphIds.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanClusteredLightingPipeline final {
    public:
        static constexpr uint32_t BindingCount = 14;
        // M7.10.2: the count and fill dispatches split each light's cluster
        // AABB over up to MaximumChunks workgroups (grid-stride), bounded so a
        // many-light frame launches about TargetWorkgroups (never fewer than
        // one per light): a large or camera-enclosing light no longer
        // serializes onto one workgroup.
        static constexpr uint32_t MaximumChunks = 64;
        static constexpr uint32_t TargetWorkgroups = 16'384;
        [[nodiscard]] static constexpr uint32_t chunkCount(
            uint32_t activeLightCount) noexcept {
            if (activeLightCount == 0) return 1;
            return (std::max)(1u, (std::min)(MaximumChunks,
                TargetWorkgroups / activeLightCount));
        }

        VulkanClusteredLightingPipeline() = default;
        VulkanClusteredLightingPipeline(const VulkanClusteredLightingPipeline&) = delete;
        VulkanClusteredLightingPipeline& operator=(
            const VulkanClusteredLightingPipeline&) = delete;
        ~VulkanClusteredLightingPipeline();

        void init(VkDevice device, VkPipelineCache pipelineCache, ::DescriptorAllocator& allocator);
        void rebuildDescriptors(const VulkanRenderGraphExecutor& graph,
            const VulkanClusterGraphIds& ids,
            std::span<const VkDescriptorBufferInfo> lightRecords,
            std::span<const VkDescriptorBufferInfo> activeSlots,
            std::span<const VkDescriptorBufferInfo> fallbackCandidates,
            std::span<const VkDescriptorBufferInfo> parameters);
        void clearDescriptors();
        // M7R R4c.2: rewrites one retired slot's light-record and active-slot
        // bindings in place (no-op before the sets exist).
        void rewriteLightBuffers(uint32_t frameIndex,
            const VkDescriptorBufferInfo& lightRecords,
            const VkDescriptorBufferInfo& activeSlots);
        // One stage per graph pass ("lighting.cluster.{clear,count,scan,fill,
        // finalize}", R3c.1); the executor begins each pass. Each returns its
        // dispatch count. recordCount is two dispatches (M7.10.2): per-light
        // bounds/reservation, then the chunked per-cluster count.
        [[nodiscard]] uint32_t recordClear(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, uint32_t clusterCount);
        [[nodiscard]] uint32_t recordCount(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, uint32_t activeLightCount);
        [[nodiscard]] uint32_t recordScan(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, uint32_t clusterCount);
        [[nodiscard]] uint32_t recordFill(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, uint32_t activeLightCount);
        [[nodiscard]] uint32_t recordFinalize(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, VkBuffer indirect);
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
        VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline clearPipeline_ = VK_NULL_HANDLE;
        VkPipeline boundsPipeline_ = VK_NULL_HANDLE;
        VkPipeline countPipeline_ = VK_NULL_HANDLE;
        VkPipeline scanPipeline_ = VK_NULL_HANDLE;
        VkPipeline fillPipeline_ = VK_NULL_HANDLE;
        VkPipeline sortPreparePipeline_ = VK_NULL_HANDLE;
        VkPipeline sortPipeline_ = VK_NULL_HANDLE;
        VkPipeline denseSortPipeline_ = VK_NULL_HANDLE;
        VkPipeline finalizePipeline_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets_;
        // Records the bound light-bounds buffer holds (the active-light bound).
        uint32_t lightBoundsCapacity_ = 0;
    };

} // namespace Iridium
