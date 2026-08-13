#pragma once

#include "VulkanResourceAllocator.h"
#include "VkSwapchain.h"
#include "renderer/rhi/GBufferLayout.h"

#include <cstddef>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr VkFormat VulkanSceneColorFormat =
        VK_FORMAT_R16G16B16A16_SFLOAT;
    inline constexpr VkFormat VulkanSdrOutputFormat =
        VK_FORMAT_B8G8R8A8_SRGB;

    class VulkanRenderGraphExecutor;

    struct VulkanTargetRenderPasses {
        VkRenderPass gBuffer = VK_NULL_HANDLE;
        VkRenderPass lighting = VK_NULL_HANDLE;
        VkRenderPass forward = VK_NULL_HANDLE;
        VkRenderPass glassDepth = VK_NULL_HANDLE;
        VkRenderPass output = VK_NULL_HANDLE;
        VkRenderPass ui = VK_NULL_HANDLE;
    };

    struct VulkanFrameContextTargets {
        VulkanImageResource normal;
        VulkanImageResource albedo;
        VulkanImageResource emissive;
        VulkanImageResource f0Roughness;
        VulkanImageResource materialFlags;
        VulkanImageResource depth;
        VulkanImageResource litScene;
        VulkanImageResource opaqueCopy;
        VulkanImageResource glassDepth;
        VulkanImageResource output;
        VulkanImageResource uiComposition;
        VkFramebuffer gBufferFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer lightingFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer forwardFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer glassDepthFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer outputFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer uiCompositionFramebuffer = VK_NULL_HANDLE;
    };

    // Owns scene-sized offscreen images and framebuffers by frame context.
    // Swapchain UI framebuffers are tracked separately by acquired image. It never
    // owns swapchain images/views, render passes, descriptors, ImGui IDs, pipelines,
    // or the allocator.
    class VulkanFrameTargets final {
    public:
        VulkanFrameTargets() = default;
        VulkanFrameTargets(const VulkanFrameTargets&) = delete;
        VulkanFrameTargets& operator=(const VulkanFrameTargets&) = delete;
        VulkanFrameTargets(VulkanFrameTargets&& other) noexcept;
        VulkanFrameTargets& operator=(VulkanFrameTargets&& other) noexcept;

        // cleanup() is idempotent. Destruction may call it only while the stored
        // device is still valid.
        ~VulkanFrameTargets();

        // All owned images are created in ResourceState::Undefined.
        void init(VkDevice device, const ::VkSwapchain& swapchain,
            VkExtent2D sceneExtent,
            VulkanTargetRenderPasses renderPasses, uint32_t frameContextCount,
            bool hdr10Composition,
            const VulkanRenderGraphExecutor& graphResources);
        void cleanup();

        [[nodiscard]] size_t size() const noexcept;
        [[nodiscard]] VulkanFrameContextTargets& get(size_t index);
        [[nodiscard]] const VulkanFrameContextTargets& get(size_t index) const;
        [[nodiscard]] VkFramebuffer uiFramebuffer(size_t swapchainImageIndex) const;
        [[nodiscard]] size_t uiFramebufferCount() const noexcept;
        [[nodiscard]] VkSampler sampler() const noexcept;
        [[nodiscard]] VkSampler integerSampler() const noexcept;
        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] std::span<VulkanFrameContextTargets> targets() noexcept;
        [[nodiscard]] std::span<const VulkanFrameContextTargets> targets() const noexcept;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkSampler integerSampler_ = VK_NULL_HANDLE;
        VkExtent2D extent_{};
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        std::vector<VulkanFrameContextTargets> targets_;
        std::vector<VkFramebuffer> uiFramebuffers_;
    };

} // namespace Iridium
