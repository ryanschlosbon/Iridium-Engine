#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    class VulkanFrameTargets;

    class VulkanTransparencyPyramid final {
    public:
        void init(VkDevice device, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout globalLayout);
        void rebuild(const VulkanFrameTargets& targets);
        void clearDescriptors() noexcept;
        void cleanup() noexcept;

        [[nodiscard]] uint32_t record(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, VkDescriptorSet globalSet,
            const VulkanFrameTargets& targets) const;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* descriptors_ = nullptr;
        VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        std::vector<std::vector<VkDescriptorSet>> frameDescriptors_;
    };

} // namespace Iridium
