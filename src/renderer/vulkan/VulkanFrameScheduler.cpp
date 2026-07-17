#include "VulkanFrameScheduler.h"

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
        VkQueue presentQueue, uint32_t graphicsFamily, uint32_t swapchainImageCount) {
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
        VkResult result = vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            throwVkError("vkWaitForFences", result);
        }
        frame.deferredDeletes.flush();

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(device_, swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
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
            result = vkWaitForFences(device_, 1, &imageFence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS) {
                throwVkError("vkWaitForFences(image owner)", result);
            }
        }

        result = vkResetCommandPool(device_, frame.commandPool, 0);
        if (result != VK_SUCCESS) {
            throwVkError("vkResetCommandPool", result);
        }
        result = vkResetFences(device_, 1, &frame.inFlight);
        if (result != VK_SUCCESS) {
            throwVkError("vkResetFences", result);
        }

        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            throwVkError("vkBeginCommandBuffer", result);
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
        VkResult result = vkEndCommandBuffer(frame.commandBuffer);
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
        const VkSemaphore renderFinished = renderFinishedPerImage_[imageIndex];
        submitInfo.pSignalSemaphores = &renderFinished;

        result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.inFlight);
        if (result != VK_SUCCESS) {
            throwVkError("vkQueueSubmit", result);
        }
        imagesInFlight_[imageIndex] = frame.inFlight;

        VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue_, &presentInfo);
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
        for (uint32_t i = 0; i < FramesInFlight; ++i) {
            fences[i] = frames_[i].inFlight;
        }
        const VkResult result = vkWaitForFences(device_, FramesInFlight, fences.data(),
            VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            throwVkError("vkWaitForFences(all frames)", result);
        }
    }

    void VulkanFrameScheduler::cleanup() {
        if (device_ == VK_NULL_HANDLE) {
            imagesInFlight_.clear();
            renderFinishedPerImage_.clear();
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
    }

} // namespace Iridium
