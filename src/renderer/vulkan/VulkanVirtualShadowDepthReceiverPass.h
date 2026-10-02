#pragma once
#include "renderer/rhi/VirtualShadowMap.h"
#include <vulkan/vulkan.h>
#include <filesystem>
#include <span>
#include <vector>

namespace Iridium {
class VulkanVirtualShadowDepthReceiverPass final {
public:
    VulkanVirtualShadowDepthReceiverPass() = default;
    ~VulkanVirtualShadowDepthReceiverPass() { cleanup(); }
    VulkanVirtualShadowDepthReceiverPass(const VulkanVirtualShadowDepthReceiverPass&) = delete;
    VulkanVirtualShadowDepthReceiverPass& operator=(const VulkanVirtualShadowDepthReceiverPass&) = delete;
    void init(VkDevice device, std::span<const VkBuffer> workingSets,
        const VirtualShadowGpuWorkingSetLayout& layout,
        const std::filesystem::path& shaderDirectory);
    // Caller waits for this slot's fence before changing its depth binding, and
    // transitions the supplied image to the declared shader-readable layout.
    void bindDepth(uint32_t slot, VkImageView view, VkSampler sampler,
        VkExtent2D extent, VkImageLayout imageLayout);
    void record(VkCommandBuffer command, uint32_t slot,
        const VirtualShadowDepthReceiverRegion& region,
        const glm::mat4& inverseViewProjection) const;
    void cleanup() noexcept;
private:
    VkDevice device_{};
    VkDescriptorPool pool_{};
    VkDescriptorSetLayout setLayout_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    std::vector<VkDescriptorSet> sets_;
    std::vector<VkExtent2D> extents_;
    uint32_t capacity_ = 0;
};
}
