#pragma once

#include "renderer/rhi/RhiResourceTypes.h"
#include "utils/DeletionQueue.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace Iridium {

    struct VulkanFrameContext {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        DeletionQueue deferredDeletes;
    };

    struct VulkanFrameBegin {
        FrameStatus status = FrameStatus::Ready;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        uint32_t imageIndex = 0;
    };

    // Acquire semaphores and fences are frame-context-owned. Present-wait
    // semaphores are indexed by swapchain image because a frame fence only
    // proves rendering completion; reacquiring the image proves presentation
    // has consumed its semaphore. imagesInFlight_ maps each acquired image to
    // the fence of the frame context that most recently submitted work for it.
    class VulkanFrameScheduler final {
    public:
        static constexpr uint32_t FramesInFlight = 2;

        VulkanFrameScheduler() = default;
        VulkanFrameScheduler(const VulkanFrameScheduler&) = delete;
        VulkanFrameScheduler& operator=(const VulkanFrameScheduler&) = delete;

        void init(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue,
            uint32_t graphicsFamily, uint32_t swapchainImageCount);
        // Waits the current frame, flushes its deferred deletes, acquires an image,
        // and waits the image's previous frame-fence owner before reuse. The frame
        // fence and command pool reset only after a usable image is acquired.
        [[nodiscard]] VulkanFrameBegin beginFrame(VkSwapchainKHR swapchain);
        // Submits with the acquired image's present-wait semaphore, then presents
        // waiting on that same semaphore. A suboptimal acquire is remembered here
        // and returned as RecreateSwapchain after its semaphore is consumed.
        [[nodiscard]] FrameStatus endFrame(VkSwapchainKHR swapchain, uint32_t imageIndex);
        void defer(std::function<void()> callback);
        void resetSwapchainImages(uint32_t imageCount);
        void waitForAllFrames();
        void cleanup();

        [[nodiscard]] uint32_t currentFrameIndex() const noexcept { return currentFrame_; }
        [[nodiscard]] VkCommandBuffer currentCommandBuffer() const noexcept {
            return frames_[currentFrame_].commandBuffer;
        }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphicsQueue_ = VK_NULL_HANDLE;
        VkQueue presentQueue_ = VK_NULL_HANDLE;
        uint32_t graphicsFamily_ = 0;

        std::array<VulkanFrameContext, FramesInFlight> frames_{};
        std::vector<VkFence> imagesInFlight_;
        std::vector<VkSemaphore> renderFinishedPerImage_;
        uint32_t currentFrame_ = 0;
        bool acquireSuboptimal_ = false;
    };

} // namespace Iridium
