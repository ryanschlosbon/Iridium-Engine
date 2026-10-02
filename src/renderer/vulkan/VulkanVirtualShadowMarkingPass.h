#pragma once

#include "renderer/rhi/VirtualShadowMap.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <span>
#include <vector>

namespace Iridium {

// Descriptor sets are immutable after init. The caller owns frame-slot fence
// reuse and publishes packed clip/receiver inputs before recording this pass.
class VulkanVirtualShadowMarkingPass final {
public:
    VulkanVirtualShadowMarkingPass() = default;
    ~VulkanVirtualShadowMarkingPass() { cleanup(); }
    VulkanVirtualShadowMarkingPass(const VulkanVirtualShadowMarkingPass&) = delete;
    VulkanVirtualShadowMarkingPass& operator=(const VulkanVirtualShadowMarkingPass&) = delete;
    void init(VkDevice device, std::span<const VkBuffer> workingSets,
        const VirtualShadowGpuWorkingSetLayout& layout,
        const VirtualShadowResourceConfig& config,
        const std::filesystem::path& shaderDirectory);
    void cleanup() noexcept;
    void record(VkCommandBuffer command, uint32_t frameIndex,
        uint32_t receiverCount, uint32_t levelCount,
        uint32_t coarsestLevel, uint32_t guardBandPages = 1) const;
    // Consumes already-produced raw marks, including direct full-view page cells.
    void recordCompaction(VkCommandBuffer command, uint32_t frameIndex,
        uint32_t markCount, uint32_t levelCount) const;

private:
    struct Pipeline {
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline handle = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> sets;
    };
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    Pipeline marker_, compactor_;
    VirtualShadowResourceConfig config_{};
};

} // namespace Iridium
