#pragma once

#include "renderer/rhi/RhiResourceTypes.h"
#include "utils/DeletionQueue.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Iridium {

    class CpuProfiler;
    inline constexpr size_t MaxVulkanGpuRangesPerFrame = 32;

    struct VulkanGpuRangeToken {
        uint32_t frameIndex = 0;
        uint32_t endQuery = 0;
        bool active = false;
    };

    struct VulkanGpuQueryRange {
        const char* name = nullptr;
        uint32_t beginQuery = 0;
        uint32_t endQuery = 0;
        bool ended = false;
    };

    struct VulkanFrameContext {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
        VkQueryPool transparentPipelineStatisticsQueryPool = VK_NULL_HANDLE;
        DeletionQueue deferredDeletes;
        std::array<VulkanGpuQueryRange, MaxVulkanGpuRangesPerFrame> gpuRanges{};
        uint64_t profileFrameId = 0;
        uint32_t timestampQueryCount = 0;
        uint32_t gpuRangeCount = 0;
        uint32_t droppedGpuRanges = 0;
        bool gpuResultsPending = false;
        bool transparentPipelineStatisticsActive = false;
        bool transparentPipelineStatisticsRecorded = false;
        bool transparentPipelineStatisticsResultsPending = false;
        bool fenceInFlight = false;
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
            uint32_t graphicsFamily, uint32_t swapchainImageCount,
            CpuProfiler* cpuProfiler, bool enableGpuProfiling,
            double timestampPeriodNanoseconds, uint32_t timestampValidBits,
            bool enableDebugLabels, bool enableTransparentPipelineStatistics,
            uint64_t transparentTargetPixelCount);
        // Waits the current frame, flushes its deferred deletes, acquires an image,
        // and waits the image's previous frame-fence owner before reuse. The frame
        // command pool resets only after a usable image is acquired. The fence
        // remains signaled until endFrame is ready to submit recorded work.
        [[nodiscard]] VulkanFrameBegin beginFrame(VkSwapchainKHR swapchain);
        // Submits with the acquired image's present-wait semaphore, then presents
        // waiting on that same semaphore. A suboptimal acquire is remembered here
        // and returned as RecreateSwapchain after its semaphore is consumed.
        [[nodiscard]] FrameStatus endFrame(VkSwapchainKHR swapchain, uint32_t imageIndex);
        void defer(std::function<void()> callback);
        void resetSwapchainImages(uint32_t imageCount);
        void waitForAllFrames();
        void cleanup();

        [[nodiscard]] VulkanGpuRangeToken beginGpuRange(const char* name) noexcept;
        void endGpuRange(VulkanGpuRangeToken& token) noexcept;
        [[nodiscard]] bool beginTransparentPipelineStatistics() noexcept;
        void endTransparentPipelineStatistics() noexcept;
        void setTransparentTargetPixelCount(uint64_t pixelCount) noexcept {
            transparentTargetPixelCount_ = pixelCount;
        }
        [[nodiscard]] bool isGpuProfilingEnabled() const noexcept {
            return gpuProfilingEnabled_;
        }

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
        CpuProfiler* cpuProfiler_ = nullptr;
        VulkanGpuRangeToken frameGpuRange_{};
        double timestampPeriodNanoseconds_ = 0.0;
        uint32_t timestampValidBits_ = 0;
        bool gpuProfilingEnabled_ = false;
        bool transparentPipelineStatisticsEnabled_ = false;
        uint64_t transparentTargetPixelCount_ = 0;
        PFN_vkCmdBeginDebugUtilsLabelEXT beginDebugLabel_ = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT endDebugLabel_ = nullptr;

        void collectGpuResults(VulkanFrameContext& frame);
    };

    class VulkanGpuScope final {
    public:
        VulkanGpuScope(VulkanFrameScheduler& scheduler, const char* name) noexcept
            : scheduler_(&scheduler), token_(scheduler.beginGpuRange(name)) {}
        ~VulkanGpuScope() { scheduler_->endGpuRange(token_); }

        VulkanGpuScope(const VulkanGpuScope&) = delete;
        VulkanGpuScope& operator=(const VulkanGpuScope&) = delete;

    private:
        VulkanFrameScheduler* scheduler_ = nullptr;
        VulkanGpuRangeToken token_{};
    };

} // namespace Iridium
