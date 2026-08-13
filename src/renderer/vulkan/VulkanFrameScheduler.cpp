#include "VulkanFrameScheduler.h"

#include "profiling/CpuProfiler.h"
#include "profiling/GpuTimestamp.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace Iridium {

    namespace {

        [[noreturn]] void throwVkError(const char* operation, VkResult result) {
            throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                std::to_string(static_cast<int>(result)) + ".");
        }

        void destroyFrameObjects(VkDevice device, VulkanFrameContext& frame) noexcept {
            if (frame.timestampQueryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device, frame.timestampQueryPool, nullptr);
                frame.timestampQueryPool = VK_NULL_HANDLE;
            }
            if (frame.transparentPipelineStatisticsQueryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device,
                    frame.transparentPipelineStatisticsQueryPool, nullptr);
                frame.transparentPipelineStatisticsQueryPool = VK_NULL_HANDLE;
            }
            if (frame.inFlight != VK_NULL_HANDLE) {
                vkDestroyFence(device, frame.inFlight, nullptr);
                frame.inFlight = VK_NULL_HANDLE;
            }
            if (frame.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, frame.imageAvailable, nullptr);
                frame.imageAvailable = VK_NULL_HANDLE;
            }
            if (frame.commandBuffer != VK_NULL_HANDLE && frame.commandPool != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device, frame.commandPool, 1, &frame.commandBuffer);
                frame.commandBuffer = VK_NULL_HANDLE;
            }
            if (frame.commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, frame.commandPool, nullptr);
                frame.commandPool = VK_NULL_HANDLE;
            }
        }

        void destroyPresentSemaphores(VkDevice device,
            std::vector<VkSemaphore>& semaphores) noexcept {
            for (VkSemaphore semaphore : semaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, semaphore, nullptr);
                }
            }
            semaphores.clear();
        }

        std::vector<VkSemaphore> createPresentSemaphores(VkDevice device, uint32_t imageCount) {
            std::vector<VkSemaphore> semaphores(imageCount, VK_NULL_HANDLE);
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            try {
                for (VkSemaphore& semaphore : semaphores) {
                    const VkResult result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
                    if (result != VK_SUCCESS) {
                        throwVkError("vkCreateSemaphore(renderFinished)", result);
                    }
                }
            } catch (...) {
                destroyPresentSemaphores(device, semaphores);
                throw;
            }
            return semaphores;
        }

    } // namespace

    void VulkanFrameScheduler::init(VkDevice device, VkQueue graphicsQueue,
        VkQueue presentQueue, uint32_t graphicsFamily, uint32_t swapchainImageCount,
        CpuProfiler* cpuProfiler, bool enableGpuProfiling,
        double timestampPeriodNanoseconds, uint32_t timestampValidBits,
        bool enableDebugLabels, bool enableTransparentPipelineStatistics,
        uint64_t transparentTargetPixelCount) {
        if (device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE ||
            presentQueue == VK_NULL_HANDLE || swapchainImageCount == 0) {
            throw std::invalid_argument("VulkanFrameScheduler requires valid handles and image count.");
        }
        if (device_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanFrameScheduler was initialized more than once.");
        }

        device_ = device;
        graphicsQueue_ = graphicsQueue;
        presentQueue_ = presentQueue;
        graphicsFamily_ = graphicsFamily;
        cpuProfiler_ = cpuProfiler;
        gpuProfilingEnabled_ = enableGpuProfiling && cpuProfiler_ != nullptr &&
            timestampPeriodNanoseconds > 0.0 && timestampValidBits > 0 &&
            timestampValidBits <= 64;
        timestampPeriodNanoseconds_ = gpuProfilingEnabled_
            ? timestampPeriodNanoseconds
            : 0.0;
        timestampValidBits_ = gpuProfilingEnabled_ ? timestampValidBits : 0;
        transparentPipelineStatisticsEnabled_ =
            enableTransparentPipelineStatistics && cpuProfiler_ != nullptr;
        transparentTargetPixelCount_ = transparentTargetPixelCount;
        if (gpuProfilingEnabled_ && enableDebugLabels) {
            beginDebugLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
            endDebugLabel_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
            if (beginDebugLabel_ == nullptr || endDebugLabel_ == nullptr) {
                beginDebugLabel_ = nullptr;
                endDebugLabel_ = nullptr;
            }
        }
        imagesInFlight_.assign(swapchainImageCount, VK_NULL_HANDLE);

        try {
            for (VulkanFrameContext& frame : frames_) {
                VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolInfo.queueFamilyIndex = graphicsFamily_;
                VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.commandPool);
                if (result != VK_SUCCESS) {
                    throwVkError("vkCreateCommandPool", result);
                }

                VkCommandBufferAllocateInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                commandInfo.commandPool = frame.commandPool;
                commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                commandInfo.commandBufferCount = 1;
                result = vkAllocateCommandBuffers(device_, &commandInfo, &frame.commandBuffer);
                if (result != VK_SUCCESS) {
                    throwVkError("vkAllocateCommandBuffers", result);
                }

                VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable);
                if (result != VK_SUCCESS) {
                    throwVkError("vkCreateSemaphore(imageAvailable)", result);
                }

                VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                result = vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight);
                if (result != VK_SUCCESS) {
                    throwVkError("vkCreateFence", result);
                }

                if (gpuProfilingEnabled_) {
                    VkQueryPoolCreateInfo queryPoolInfo{
                        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                    queryPoolInfo.queryCount = static_cast<uint32_t>(
                        frame.gpuRanges.size() * 2);
                    result = vkCreateQueryPool(device_, &queryPoolInfo, nullptr,
                        &frame.timestampQueryPool);
                    if (result != VK_SUCCESS) {
                        throwVkError("vkCreateQueryPool(timestamp)", result);
                    }
                }
                if (transparentPipelineStatisticsEnabled_) {
                    VkQueryPoolCreateInfo queryPoolInfo{
                        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                    queryPoolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                    queryPoolInfo.queryCount = 1;
                    queryPoolInfo.pipelineStatistics =
                        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
                    result = vkCreateQueryPool(device_, &queryPoolInfo, nullptr,
                        &frame.transparentPipelineStatisticsQueryPool);
                    if (result != VK_SUCCESS) {
                        throwVkError(
                            "vkCreateQueryPool(transparent pipeline statistics)",
                            result);
                    }
                }
            }
            renderFinishedPerImage_ = createPresentSemaphores(device_, swapchainImageCount);
        }
        catch (...) {
            for (VulkanFrameContext& frame : frames_) {
                frame.deferredDeletes.deletors.clear();
                destroyFrameObjects(device_, frame);
            }
            imagesInFlight_.clear();
            destroyPresentSemaphores(device_, renderFinishedPerImage_);
            device_ = VK_NULL_HANDLE;
            graphicsQueue_ = VK_NULL_HANDLE;
            presentQueue_ = VK_NULL_HANDLE;
            graphicsFamily_ = 0;
            currentFrame_ = 0;
            acquireSuboptimal_ = false;
            cpuProfiler_ = nullptr;
            frameGpuRange_ = {};
            timestampPeriodNanoseconds_ = 0.0;
            timestampValidBits_ = 0;
            gpuProfilingEnabled_ = false;
            transparentPipelineStatisticsEnabled_ = false;
            transparentTargetPixelCount_ = 0;
            beginDebugLabel_ = nullptr;
            endDebugLabel_ = nullptr;
            throw;
        }
    }

    VulkanFrameBegin VulkanFrameScheduler::beginFrame(VkSwapchainKHR swapchain) {
        if (device_ == VK_NULL_HANDLE || graphicsQueue_ == VK_NULL_HANDLE ||
            presentQueue_ == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
            throw std::logic_error("VulkanFrameScheduler is not initialized.");
        }
        if (imagesInFlight_.empty()) {
            throw std::logic_error("VulkanFrameScheduler has no swapchain images.");
        }

        VulkanFrameContext& frame = frames_[currentFrame_];
        VkResult result = VK_SUCCESS;
        {
            CpuScope waitScope(cpuProfiler_, "cpu.renderer.frame_fence_wait");
            if (frame.fenceInFlight) {
                result = vkWaitForFences(device_, 1, &frame.inFlight,
                    VK_TRUE, UINT64_MAX);
            }
        }
        if (result != VK_SUCCESS) {
            throwVkError("vkWaitForFences", result);
        }
        frame.fenceInFlight = false;
        collectGpuResults(frame);
        frame.deferredDeletes.flush();

        uint32_t imageIndex = 0;
        {
            CpuScope acquireScope(cpuProfiler_, "cpu.renderer.acquire");
            result = vkAcquireNextImageKHR(device_, swapchain, UINT64_MAX,
                frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return { FrameStatus::RecreateSwapchain, VK_NULL_HANDLE, 0 };
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            acquireSuboptimal_ = true;
        }
        else if (result != VK_SUCCESS) {
            throwVkError("vkAcquireNextImageKHR", result);
        }
        if (imageIndex >= imagesInFlight_.size()) {
            throw std::runtime_error("vkAcquireNextImageKHR returned an invalid image index.");
        }
        if (imageIndex >= renderFinishedPerImage_.size()) {
            throw std::runtime_error("vkAcquireNextImageKHR returned an image without a present semaphore.");
        }

        VkFence imageFence = imagesInFlight_[imageIndex];
        if (imageFence != VK_NULL_HANDLE && imageFence != frame.inFlight) {
            CpuScope waitScope(cpuProfiler_, "cpu.renderer.frame_fence_wait");
            result = vkWaitForFences(device_, 1, &imageFence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS) {
                throwVkError("vkWaitForFences(image owner)", result);
            }
        }

        result = vkResetCommandPool(device_, frame.commandPool, 0);
        if (result != VK_SUCCESS) {
            throwVkError("vkResetCommandPool", result);
        }
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            throwVkError("vkBeginCommandBuffer", result);
        }

        frame.profileFrameId = 0;
        frame.timestampQueryCount = 0;
        frame.gpuRangeCount = 0;
        frame.droppedGpuRanges = 0;
        frame.gpuResultsPending = false;
        frame.transparentPipelineStatisticsActive = false;
        frame.transparentPipelineStatisticsRecorded = false;
        frame.transparentPipelineStatisticsResultsPending = false;
        frameGpuRange_ = {};
        if ((gpuProfilingEnabled_ || transparentPipelineStatisticsEnabled_) &&
            cpuProfiler_ != nullptr && cpuProfiler_->isFrameOpen()) {
            frame.profileFrameId = cpuProfiler_->currentFrameId();
            if (gpuProfilingEnabled_) {
                vkCmdResetQueryPool(frame.commandBuffer, frame.timestampQueryPool, 0,
                    static_cast<uint32_t>(frame.gpuRanges.size() * 2));
                frameGpuRange_ = beginGpuRange("gpu.frame");
            }
            if (transparentPipelineStatisticsEnabled_) {
                vkCmdResetQueryPool(frame.commandBuffer,
                    frame.transparentPipelineStatisticsQueryPool, 0, 1);
            }
        }

        return { FrameStatus::Ready, frame.commandBuffer, imageIndex };
    }

    FrameStatus VulkanFrameScheduler::endFrame(VkSwapchainKHR swapchain, uint32_t imageIndex) {
        if (device_ == VK_NULL_HANDLE || graphicsQueue_ == VK_NULL_HANDLE ||
            presentQueue_ == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
            throw std::logic_error("VulkanFrameScheduler is not initialized.");
        }
        if (imageIndex >= imagesInFlight_.size()) {
            throw std::out_of_range("VulkanFrameScheduler image index is out of range.");
        }
        if (imageIndex >= renderFinishedPerImage_.size()) {
            throw std::out_of_range("VulkanFrameScheduler present semaphore index is out of range.");
        }

        VulkanFrameContext& frame = frames_[currentFrame_];
        endTransparentPipelineStatistics();
        endGpuRange(frameGpuRange_);
        const VkSemaphore renderFinished = renderFinishedPerImage_[imageIndex];
        VkResult result = VK_SUCCESS;
        {
            CpuScope submitScope(cpuProfiler_, "cpu.renderer.submit");
            result = vkEndCommandBuffer(frame.commandBuffer);
            if (result != VK_SUCCESS) {
                throwVkError("vkEndCommandBuffer", result);
            }

            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &frame.imageAvailable;
            submitInfo.pWaitDstStageMask = &waitStage;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &frame.commandBuffer;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &renderFinished;

            result = vkResetFences(device_, 1, &frame.inFlight);
            if (result != VK_SUCCESS) {
                throwVkError("vkResetFences", result);
            }
            result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.inFlight);
            if (result != VK_SUCCESS) {
                throwVkError("vkQueueSubmit", result);
            }
            frame.fenceInFlight = true;
            imagesInFlight_[imageIndex] = frame.inFlight;
            frame.gpuResultsPending = frame.profileFrameId != 0 &&
                frame.timestampQueryCount != 0;
            frame.transparentPipelineStatisticsResultsPending =
                frame.profileFrameId != 0 &&
                frame.transparentPipelineStatisticsRecorded;
        }

        VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        {
            CpuScope presentScope(cpuProfiler_, "cpu.renderer.present");
            result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        }
        const bool recreate = acquireSuboptimal_ ||
            result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
        if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR &&
            result != VK_SUBOPTIMAL_KHR) {
            throwVkError("vkQueuePresentKHR", result);
        }

        acquireSuboptimal_ = false;
        currentFrame_ = (currentFrame_ + 1) % FramesInFlight;
        return recreate ? FrameStatus::RecreateSwapchain : FrameStatus::Ready;
    }

    void VulkanFrameScheduler::defer(std::function<void()> callback) {
        if (device_ == VK_NULL_HANDLE) {
            throw std::logic_error("VulkanFrameScheduler is not initialized.");
        }
        frames_[currentFrame_].deferredDeletes.push_function(std::move(callback));
    }

    void VulkanFrameScheduler::resetSwapchainImages(uint32_t imageCount) {
        if (device_ == VK_NULL_HANDLE) {
            throw std::logic_error("VulkanFrameScheduler is not initialized.");
        }
        if (imageCount == 0) {
            throw std::invalid_argument("VulkanFrameScheduler requires at least one swapchain image.");
        }

        std::vector<VkSemaphore> replacementSemaphores =
            createPresentSemaphores(device_, imageCount);
        destroyPresentSemaphores(device_, renderFinishedPerImage_);
        renderFinishedPerImage_ = std::move(replacementSemaphores);
        imagesInFlight_.assign(imageCount, VK_NULL_HANDLE);
    }

    void VulkanFrameScheduler::waitForAllFrames() {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        std::array<VkFence, FramesInFlight> fences{};
        uint32_t fenceCount = 0;
        for (VulkanFrameContext& frame : frames_) {
            if (frame.fenceInFlight) {
                fences[fenceCount++] = frame.inFlight;
            }
        }
        if (fenceCount != 0) {
            const VkResult result = vkWaitForFences(device_, fenceCount,
                fences.data(), VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS) {
                throwVkError("vkWaitForFences(all frames)", result);
            }
        }
        for (VulkanFrameContext& frame : frames_) {
            frame.fenceInFlight = false;
            collectGpuResults(frame);
        }
    }

    VulkanGpuRangeToken VulkanFrameScheduler::beginGpuRange(
        const char* name) noexcept {
        VulkanGpuRangeToken token{};
        if (!gpuProfilingEnabled_ || name == nullptr || device_ == VK_NULL_HANDLE) {
            return token;
        }

        VulkanFrameContext& frame = frames_[currentFrame_];
        if (frame.profileFrameId == 0) {
            return token;
        }
        if (frame.gpuRangeCount >= frame.gpuRanges.size() ||
            frame.timestampQueryCount + 2 > frame.gpuRanges.size() * 2) {
            ++frame.droppedGpuRanges;
            return token;
        }

        const uint32_t rangeIndex = frame.gpuRangeCount++;
        VulkanGpuQueryRange& range = frame.gpuRanges[rangeIndex];
        range.name = name;
        range.beginQuery = frame.timestampQueryCount++;
        range.endQuery = frame.timestampQueryCount++;
        range.ended = false;
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            frame.timestampQueryPool, range.beginQuery);
        if (beginDebugLabel_ != nullptr) {
            VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
            label.pLabelName = name;
            label.color[0] = 0.20f;
            label.color[1] = 0.55f;
            label.color[2] = 0.90f;
            label.color[3] = 1.0f;
            beginDebugLabel_(frame.commandBuffer, &label);
        }

        token.frameIndex = currentFrame_;
        token.endQuery = range.endQuery;
        token.active = true;
        return token;
    }

    void VulkanFrameScheduler::endGpuRange(VulkanGpuRangeToken& token) noexcept {
        if (!token.active) {
            return;
        }
        token.active = false;
        if (!gpuProfilingEnabled_ || token.frameIndex != currentFrame_) {
            return;
        }

        VulkanFrameContext& frame = frames_[currentFrame_];
        const auto range = std::find_if(frame.gpuRanges.begin(),
            frame.gpuRanges.begin() + frame.gpuRangeCount,
            [&token](const VulkanGpuQueryRange& candidate) {
                return candidate.endQuery == token.endQuery;
            });
        if (range == frame.gpuRanges.begin() + frame.gpuRangeCount || range->ended) {
            ++frame.droppedGpuRanges;
            return;
        }
        if (endDebugLabel_ != nullptr) {
            endDebugLabel_(frame.commandBuffer);
        }
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            frame.timestampQueryPool, range->endQuery);
        range->ended = true;
    }

    bool VulkanFrameScheduler::beginTransparentPipelineStatistics() noexcept {
        if (!transparentPipelineStatisticsEnabled_ || device_ == VK_NULL_HANDLE) {
            return false;
        }
        VulkanFrameContext& frame = frames_[currentFrame_];
        if (frame.profileFrameId == 0 ||
            frame.transparentPipelineStatisticsActive ||
            frame.transparentPipelineStatisticsRecorded) {
            return false;
        }
        vkCmdBeginQuery(frame.commandBuffer,
            frame.transparentPipelineStatisticsQueryPool, 0, 0);
        frame.transparentPipelineStatisticsActive = true;
        return true;
    }

    void VulkanFrameScheduler::endTransparentPipelineStatistics() noexcept {
        if (!transparentPipelineStatisticsEnabled_ || device_ == VK_NULL_HANDLE) {
            return;
        }
        VulkanFrameContext& frame = frames_[currentFrame_];
        if (!frame.transparentPipelineStatisticsActive) {
            return;
        }
        vkCmdEndQuery(frame.commandBuffer,
            frame.transparentPipelineStatisticsQueryPool, 0);
        frame.transparentPipelineStatisticsActive = false;
        frame.transparentPipelineStatisticsRecorded = true;
    }

    void VulkanFrameScheduler::collectGpuResults(VulkanFrameContext& frame) {
        if (frame.profileFrameId == 0) {
            return;
        }

        if (transparentPipelineStatisticsEnabled_ &&
            frame.transparentPipelineStatisticsResultsPending) {
            struct PipelineStatisticsResult {
                uint64_t fragmentShaderInvocations = 0;
                uint64_t available = 0;
            } statistics;
            const VkResult statisticsResult = vkGetQueryPoolResults(device_,
                frame.transparentPipelineStatisticsQueryPool, 0, 1,
                sizeof(statistics), &statistics, sizeof(statistics),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            const bool available = statisticsResult == VK_SUCCESS &&
                statistics.available != 0;
            const ProfileCounterStatus status = available
                ? ProfileCounterStatus::Exact
                : ProfileCounterStatus::Unavailable;
            const uint64_t invocations = available
                ? statistics.fragmentShaderInvocations : 0;
            (void)cpuProfiler_->attachCounter(frame.profileFrameId,
                "transparent.fragment_invocations", invocations, status,
                ProfileCounterUnit::Count);

            uint64_t fullscreenEquivalentMillionths = 0;
            if (available && transparentTargetPixelCount_ != 0) {
                const long double scaled =
                    static_cast<long double>(invocations) * 1'000'000.0L /
                    static_cast<long double>(transparentTargetPixelCount_);
                fullscreenEquivalentMillionths = scaled >=
                    static_cast<long double>(std::numeric_limits<uint64_t>::max())
                    ? std::numeric_limits<uint64_t>::max()
                    : static_cast<uint64_t>(scaled + 0.5L);
            }
            (void)cpuProfiler_->attachCounter(frame.profileFrameId,
                "transparent.fullscreen_equivalents",
                fullscreenEquivalentMillionths, status,
                ProfileCounterUnit::Millionths);
            frame.transparentPipelineStatisticsResultsPending = false;
        }

        if (!gpuProfilingEnabled_ || !frame.gpuResultsPending ||
            frame.timestampQueryCount == 0) {
            if (!frame.transparentPipelineStatisticsResultsPending) {
                frame.profileFrameId = 0;
            }
            return;
        }

        struct QueryResult {
            uint64_t value = 0;
            uint64_t available = 0;
        };
        std::array<QueryResult, MaxVulkanGpuRangesPerFrame * 2> results{};
        const VkResult result = vkGetQueryPoolResults(device_, frame.timestampQueryPool,
            0, frame.timestampQueryCount,
            sizeof(QueryResult) * frame.timestampQueryCount, results.data(),
            sizeof(QueryResult), VK_QUERY_RESULT_64_BIT |
            VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

        std::array<GpuProfileRange, CpuProfiler::MaxGpuRangesPerFrame> ranges{};
        uint32_t rangeCount = 0;
        uint32_t unavailableCount = 0;
        uint64_t frameStartTicks = 0;
        bool frameStartAvailable = false;
        if (frame.gpuRangeCount > 0) {
            const VulkanGpuQueryRange& root = frame.gpuRanges[0];
            frameStartAvailable = root.beginQuery < frame.timestampQueryCount &&
                results[root.beginQuery].available != 0;
            if (frameStartAvailable) {
                frameStartTicks = results[root.beginQuery].value;
            }
        }

        for (uint32_t index = 0; index < frame.gpuRangeCount; ++index) {
            const VulkanGpuQueryRange& queryRange = frame.gpuRanges[index];
            GpuProfileRange& range = ranges[rangeCount++];
            range.name = queryRange.name;
            const bool available = result == VK_SUCCESS && queryRange.ended &&
                queryRange.beginQuery < frame.timestampQueryCount &&
                queryRange.endQuery < frame.timestampQueryCount &&
                results[queryRange.beginQuery].available != 0 &&
                results[queryRange.endQuery].available != 0;
            range.available = available;
            if (!available) {
                ++unavailableCount;
                continue;
            }
            range.durationNanoseconds = calculateGpuTimestampDurationNanoseconds(
                results[queryRange.beginQuery].value,
                results[queryRange.endQuery].value,
                timestampValidBits_, timestampPeriodNanoseconds_);
            if (frameStartAvailable) {
                range.startNanoseconds = calculateGpuTimestampDurationNanoseconds(
                    frameStartTicks, results[queryRange.beginQuery].value,
                    timestampValidBits_, timestampPeriodNanoseconds_);
            }
        }

        (void)cpuProfiler_->attachGpuRanges(frame.profileFrameId,
            std::span<const GpuProfileRange>(ranges.data(), rangeCount),
            frame.droppedGpuRanges + unavailableCount);
        frame.gpuResultsPending = false;
        frame.profileFrameId = 0;
        frame.timestampQueryCount = 0;
        frame.gpuRangeCount = 0;
        frame.droppedGpuRanges = 0;
        if (!frame.transparentPipelineStatisticsResultsPending) {
            frame.profileFrameId = 0;
        }
    }

    void VulkanFrameScheduler::cleanup() {
        if (device_ == VK_NULL_HANDLE) {
            imagesInFlight_.clear();
            renderFinishedPerImage_.clear();
            cpuProfiler_ = nullptr;
            frameGpuRange_ = {};
            timestampPeriodNanoseconds_ = 0.0;
            timestampValidBits_ = 0;
            gpuProfilingEnabled_ = false;
            transparentPipelineStatisticsEnabled_ = false;
            transparentTargetPixelCount_ = 0;
            beginDebugLabel_ = nullptr;
            endDebugLabel_ = nullptr;
            return;
        }

        waitForAllFrames();
        for (VulkanFrameContext& frame : frames_) {
            frame.deferredDeletes.flush();
        }
        for (VulkanFrameContext& frame : frames_) {
            destroyFrameObjects(device_, frame);
        }

        imagesInFlight_.clear();
        destroyPresentSemaphores(device_, renderFinishedPerImage_);
        device_ = VK_NULL_HANDLE;
        graphicsQueue_ = VK_NULL_HANDLE;
        presentQueue_ = VK_NULL_HANDLE;
        graphicsFamily_ = 0;
        currentFrame_ = 0;
        acquireSuboptimal_ = false;
        cpuProfiler_ = nullptr;
        frameGpuRange_ = {};
        timestampPeriodNanoseconds_ = 0.0;
        timestampValidBits_ = 0;
        gpuProfilingEnabled_ = false;
        transparentPipelineStatisticsEnabled_ = false;
        transparentTargetPixelCount_ = 0;
        beginDebugLabel_ = nullptr;
        endDebugLabel_ = nullptr;
    }

} // namespace Iridium
