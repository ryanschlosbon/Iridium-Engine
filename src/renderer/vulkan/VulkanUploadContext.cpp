#include "VulkanUploadContext.h"

#include "VulkanCommandList.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {

        void requireInitialized(VkDevice device, VkQueue queue,
            VulkanResourceAllocator* allocator) {
            if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || allocator == nullptr) {
                throw std::logic_error("VulkanUploadContext is not initialized.");
            }
        }

        [[noreturn]] void throwVkError(const char* operation, VkResult result) {
            throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                std::to_string(static_cast<int>(result)) + ".");
        }

        void destroyStagingBuffers(VulkanResourceAllocator& allocator,
            std::vector<VulkanBufferResource>& stagingBuffers) {
            for (VulkanBufferResource& staging : stagingBuffers) {
                allocator.destroy(staging);
            }
            stagingBuffers.clear();
        }

    } // namespace

    void VulkanUploadContext::init(VkDevice device, VkQueue graphicsQueue,
        uint32_t graphicsQueueFamily, VulkanResourceAllocator& allocator) {
        if (device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanUploadContext requires valid Vulkan handles.");
        }
        if (allocator_ != nullptr || device_ != VK_NULL_HANDLE || commandPool_ != VK_NULL_HANDLE ||
            commandBuffer_ != VK_NULL_HANDLE || fence_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanUploadContext was initialized more than once.");
        }

        device_ = device;
        graphicsQueue_ = graphicsQueue;
        graphicsQueueFamily_ = graphicsQueueFamily;
        allocator_ = &allocator;

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueFamily_;

        VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            device_ = VK_NULL_HANDLE;
            graphicsQueue_ = VK_NULL_HANDLE;
            allocator_ = nullptr;
            throwVkError("vkCreateCommandPool", result);
        }

        VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer_);
        if (result != VK_SUCCESS) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            graphicsQueue_ = VK_NULL_HANDLE;
            allocator_ = nullptr;
            throwVkError("vkAllocateCommandBuffers", result);
        }

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = vkCreateFence(device_, &fenceInfo, nullptr, &fence_);
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer_);
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandBuffer_ = VK_NULL_HANDLE;
            commandPool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            graphicsQueue_ = VK_NULL_HANDLE;
            allocator_ = nullptr;
            throwVkError("vkCreateFence", result);
        }
    }

    void VulkanUploadContext::cleanup() {
        if (device_ == VK_NULL_HANDLE) {
            stagingBuffers_.clear();
            batchOpen_ = false;
            return;
        }

        if (batchOpen_ || !stagingBuffers_.empty()) {
            try {
                flush();
            } catch (...) {
                // Flush has already made best-effort cleanup of staging and command state.
                // Continue releasing the context handles so cleanup remains terminal and safe.
            }
        }

        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
        }
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        }

        device_ = VK_NULL_HANDLE;
        graphicsQueue_ = VK_NULL_HANDLE;
        graphicsQueueFamily_ = 0;
        allocator_ = nullptr;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        fence_ = VK_NULL_HANDLE;
        batchOpen_ = false;
        stagingBuffers_.clear();
    }

    bool VulkanUploadContext::hasPendingWork() const noexcept {
        return batchOpen_ || !stagingBuffers_.empty();
    }

    void VulkanUploadContext::flush() {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (!hasPendingWork()) {
            return;
        }

        auto discardPending = [this]() noexcept {
            if (allocator_ != nullptr) {
                destroyStagingBuffers(*allocator_, stagingBuffers_);
            } else {
                stagingBuffers_.clear();
            }
            batchOpen_ = false;
        };

        auto resetCommandPoolAfterFailure = [this]() noexcept -> VkResult {
            if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE) {
                return vkResetCommandPool(device_, commandPool_, 0);
            }
            return VK_SUCCESS;
        };

        if (batchOpen_) {
            VkResult result = vkEndCommandBuffer(commandBuffer_);
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkEndCommandBuffer", result);
            }

            result = vkResetFences(device_, 1, &fence_);
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkResetFences", result);
            }

            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer_;
            result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence_);
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkQueueSubmit", result);
            }

            result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, std::numeric_limits<uint64_t>::max());
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkWaitForFences", result);
            }
        }

        destroyStagingBuffers(*allocator_, stagingBuffers_);
        VkResult result = vkResetCommandPool(device_, commandPool_, 0);
        batchOpen_ = false;
        if (result != VK_SUCCESS) {
            throwVkError("vkResetCommandPool", result);
        }
    }

    void VulkanUploadContext::enqueueBufferUpload(VulkanBufferResource& destination,
        std::span<const std::byte> data, ResourceState finalState) {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (!destination.isValid()) {
            throw std::invalid_argument("Buffer upload destination is invalid.");
        }
        if (data.size_bytes() > destination.size) {
            throw std::out_of_range("Buffer upload data exceeds destination size.");
        }

        if (!batchOpen_) {
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            const VkResult result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
            if (result != VK_SUCCESS) {
                throwVkError("vkBeginCommandBuffer", result);
            }
            batchOpen_ = true;
        }

        VulkanBufferResource staging{};
        try {
            staging = allocator_->createBuffer(data.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
            allocator_->write(staging, 0, data);

            VulkanCommandList commands(commandBuffer_);
            commands.transition(staging, ResourceState::CopySource);
            commands.transition(destination, ResourceState::CopyDestination);
            commands.copyBuffer(staging, destination, data.size_bytes());
            commands.transition(destination, finalState);
            stagingBuffers_.push_back(staging);
            staging = {};
        } catch (...) {
            allocator_->destroy(staging);
            throw;
        }
    }

    void VulkanUploadContext::enqueueImageUpload(VulkanImageResource& destination,
        std::span<const std::byte> data, ResourceState finalState) {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (data.empty()) {
            throw std::invalid_argument("Image upload data must not be empty.");
        }
        if (!destination.isValid()) {
            throw std::invalid_argument("Image upload destination is invalid.");
        }

        if (!batchOpen_) {
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            const VkResult result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
            if (result != VK_SUCCESS) {
                throwVkError("vkBeginCommandBuffer", result);
            }
            batchOpen_ = true;
        }

        VulkanBufferResource staging{};
        try {
            staging = allocator_->createBuffer(data.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
            allocator_->write(staging, 0, data);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = destination.aspect;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { destination.extent.width, destination.extent.height, 1 };

            VulkanCommandList commands(commandBuffer_);
            commands.transition(staging, ResourceState::CopySource);
            commands.transition(destination, ResourceState::CopyDestination);
            commands.copyBufferToImage(staging, destination, region);
            commands.transition(destination, finalState);
            stagingBuffers_.push_back(staging);
            staging = {};
        } catch (...) {
            allocator_->destroy(staging);
            throw;
        }
    }

    void VulkanUploadContext::enqueueTransition(VulkanImageResource& destination, ResourceState state) {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (!destination.isValid()) {
            throw std::invalid_argument("Image transition destination is invalid.");
        }

        if (!batchOpen_) {
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            const VkResult result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
            if (result != VK_SUCCESS) {
                throwVkError("vkBeginCommandBuffer", result);
            }
            batchOpen_ = true;
        }

        VulkanCommandList(commandBuffer_).transition(destination, state);
    }

} // namespace Iridium
