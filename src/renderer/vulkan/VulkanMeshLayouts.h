#pragma once

#include <vulkan/vulkan.h>

namespace Iridium {

    // Owns mesh descriptor-set and pipeline-layout identity. init() throws if called twice.
    class VulkanMeshLayouts final {
    public:
        VulkanMeshLayouts() = default;
        ~VulkanMeshLayouts() = default;

        VulkanMeshLayouts(const VulkanMeshLayouts&) = delete;
        VulkanMeshLayouts& operator=(const VulkanMeshLayouts&) = delete;
        VulkanMeshLayouts(VulkanMeshLayouts&&) = delete;
        VulkanMeshLayouts& operator=(VulkanMeshLayouts&&) = delete;

        void init(VkDevice device, VkDescriptorSetLayout lightingSetLayout,
            VkDescriptorSetLayout indexedMaterialSetLayout,
            VkDescriptorSetLayout indexedSamplerSetLayout);
        void cleanup() noexcept;

        VkDescriptorSetLayout getGlobalSetLayout() const noexcept { return globalSetLayout_; }
        VkDescriptorSetLayout getMaterialSetLayout() const noexcept { return materialSetLayout_; }
        VkPipelineLayout getGBufferPipelineLayout() const noexcept { return gBufferPipelineLayout_; }
        VkPipelineLayout getForwardPipelineLayout() const noexcept { return forwardPipelineLayout_; }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout globalSetLayout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout samplerSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout gBufferPipelineLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout forwardPipelineLayout_ = VK_NULL_HANDLE;
    };

} // namespace Iridium
