#pragma once

#include "VulkanResourceAllocator.h"
#include "VkSwapchain.h"

#include <cstddef>
#include <span>
#include <vector>

namespace Iridium {

    struct VulkanTargetRenderPasses {
        VkRenderPass gBuffer = VK_NULL_HANDLE;
        VkRenderPass lighting = VK_NULL_HANDLE;
        VkRenderPass forward = VK_NULL_HANDLE;
        VkRenderPass glassDepth = VK_NULL_HANDLE;
        VkRenderPass ui = VK_NULL_HANDLE;
    };

    struct VulkanPerImageTargets {
        VulkanImageResource normal;
        VulkanImageResource albedo;
        VulkanImageResource emissive;
        VulkanImageResource depth;
        VulkanImageResource litScene;
        VulkanImageResource opaqueCopy;
        VulkanImageResource glassDepth;
        VkFramebuffer gBufferFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer lightingFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer forwardFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer glassDepthFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer uiFramebuffer = VK_NULL_HANDLE;
    };

    // Owns every swapchain-sized offscreen image and framebuffer, grouped by
    // swapchain image. It never owns swapchain images/views, render passes,
    // descriptors, ImGui IDs, pipelines, or the allocator.
    class VulkanFrameTargets final {
    public:
        VulkanFrameTargets() = default;
        VulkanFrameTargets(const VulkanFrameTargets&) = delete;
        VulkanFrameTargets& operator=(const VulkanFrameTargets&) = delete;
        VulkanFrameTargets(VulkanFrameTargets&& other) noexcept;
        VulkanFrameTargets& operator=(VulkanFrameTargets&& other) noexcept;

        // cleanup() is idempotent. Destruction may call it only while the stored
        // device and non-owning allocator pointer are still valid.
        ~VulkanFrameTargets();

        // All owned images are created in ResourceState::Undefined.
        void init(VkDevice device, VulkanResourceAllocator& allocator, const ::VkSwapchain& swapchain,
            VulkanTargetRenderPasses renderPasses);
        void cleanup();

        [[nodiscard]] size_t size() const noexcept;
        [[nodiscard]] VulkanPerImageTargets& get(size_t index);
        [[nodiscard]] const VulkanPerImageTargets& get(size_t index) const;
        [[nodiscard]] VkSampler sampler() const noexcept;
        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] std::span<VulkanPerImageTargets> targets() noexcept;
        [[nodiscard]] std::span<const VulkanPerImageTargets> targets() const noexcept;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkExtent2D extent_{};
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        std::vector<VulkanPerImageTargets> targets_;
    };

} // namespace Iridium
