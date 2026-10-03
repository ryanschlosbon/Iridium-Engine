#pragma once
#include "renderer/rhi/VirtualShadowMap.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <span>
#include <vector>

namespace Iridium {
class VulkanVirtualShadowFullViewPass final {
public:
    VulkanVirtualShadowFullViewPass() = default;
    ~VulkanVirtualShadowFullViewPass() { cleanup(); }
    VulkanVirtualShadowFullViewPass(const VulkanVirtualShadowFullViewPass&) = delete;
    VulkanVirtualShadowFullViewPass& operator=(const VulkanVirtualShadowFullViewPass&) = delete;
    void init(VkDevice device, VkPipelineCache pipelineCache, std::span<const VkBuffer> workingSets,
        const VirtualShadowGpuWorkingSetLayout& layout,
        const std::filesystem::path& shaderDirectory);
    // Binding changes require completion of this frame slot. The caller owns
    // depth layout/dependencies and must publish the matching packed clip stack.
    void bindDepth(uint32_t slot, VkImageView view, VkSampler sampler,
        VkExtent2D extent, VkImageLayout imageLayout);
    [[nodiscard]] uint32_t record(VkCommandBuffer command, uint32_t slot,
        const DirectionalVirtualShadowMarkConfig& config,
        std::span<const DirectionalVirtualShadowClipLevel> levels,
        const glm::mat4& inverseViewProjection, bool reverseDepth = false) const;
    void cleanup() noexcept;
private:
    VkDevice device_{};
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_{};
    VkDescriptorSetLayout setLayout_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    std::vector<VkDescriptorSet> sets_;
    std::vector<VkExtent2D> extents_;
    uint32_t capacity_ = 0;
};
}
