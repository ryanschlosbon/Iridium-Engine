#include "VulkanUploadContext.h"

#include "VulkanCommandList.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <limits>
#include <chrono>
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

        VkDeviceSize mipByteSize(
            VkFormat format, uint32_t width, uint32_t height) {
            switch (format) {
            case VK_FORMAT_BC4_UNORM_BLOCK:
                return ((static_cast<VkDeviceSize>(width) + 3) / 4) *
                    ((static_cast<VkDeviceSize>(height) + 3) / 4) * 8;
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
                return ((static_cast<VkDeviceSize>(width) + 3) / 4) *
                    ((static_cast<VkDeviceSize>(height) + 3) / 4) * 16;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                return static_cast<VkDeviceSize>(width) * height * 4;
            case VK_FORMAT_R16G16_SFLOAT:
                return static_cast<VkDeviceSize>(width) * height * 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return static_cast<VkDeviceSize>(width) * height * 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                return static_cast<VkDeviceSize>(width) * height * 16;
            default:
                throw std::invalid_argument("Unsupported Vulkan upload image format");
            }
        }

    } // namespace

    void VulkanUploadContext::init(VkDevice device, VkQueue graphicsQueue,
        uint32_t graphicsQueueFamily, VulkanResourceAllocator& allocator,
        CpuProfiler* cpuProfiler) {
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
        cpuProfiler_ = cpuProfiler;

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueFamily_;

        VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            device_ = VK_NULL_HANDLE;
            graphicsQueue_ = VK_NULL_HANDLE;
            allocator_ = nullptr;
            cpuProfiler_ = nullptr;
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
            cpuProfiler_ = nullptr;
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
            cpuProfiler_ = nullptr;
            throwVkError("vkCreateFence", result);
        }
    }

    void VulkanUploadContext::cleanup() {
        if (device_ == VK_NULL_HANDLE) {
            stagingBuffers_.clear();
            batchOpen_ = false;
            pendingBytes_ = 0;
            cpuProfiler_ = nullptr;
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
        cpuProfiler_ = nullptr;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        fence_ = VK_NULL_HANDLE;
        batchOpen_ = false;
        pendingBytes_ = 0;
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

        CpuScope uploadScope(cpuProfiler_, "cpu.renderer.upload_wait");
        const auto flushStart = std::chrono::steady_clock::now();
        const uint64_t submittedBytes = pendingBytes_;

        auto discardPending = [this]() noexcept {
            if (allocator_ != nullptr) {
                destroyStagingBuffers(*allocator_, stagingBuffers_);
            } else {
                stagingBuffers_.clear();
            }
            batchOpen_ = false;
            pendingBytes_ = 0;
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
        ++totalSubmittedBatches_;
        totalSubmittedBytes_ += submittedBytes;
        totalSubmitAndWaitNanoseconds_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - flushStart).count());
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter("upload.bytes", submittedBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_->recordCounter("upload.batches", 1);
        }
        VkResult result = vkResetCommandPool(device_, commandPool_, 0);
        batchOpen_ = false;
        pendingBytes_ = 0;
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
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::UploadStaging);
            allocator_->write(staging, 0, data);

            VulkanCommandList commands(commandBuffer_);
            commands.transition(staging, ResourceState::CopySource);
            commands.transition(destination, ResourceState::CopyDestination);
            commands.copyBuffer(staging, destination, data.size_bytes());
            commands.transition(destination, finalState);
            stagingBuffers_.push_back(staging);
            pendingBytes_ += static_cast<uint64_t>(data.size_bytes());
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
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::UploadStaging);
            allocator_->write(staging, 0, data);

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(static_cast<size_t>(destination.mipLevels) *
                destination.arrayLayers);
            VkDeviceSize byteOffset = 0;
            // Upload ABI is layer-major, with a complete largest-to-smallest mip
            // chain for each layer. Cube faces use Vulkan layer order +X,-X,+Y,
            // -Y,+Z,-Z.
            for (uint32_t layer = 0; layer < destination.arrayLayers; ++layer) {
                uint32_t width = destination.extent.width;
                uint32_t height = destination.extent.height;
                for (uint32_t level = 0; level < destination.mipLevels; ++level) {
                    VkBufferImageCopy region{};
                    region.bufferOffset = byteOffset;
                    region.imageSubresource.aspectMask = destination.aspect;
                    region.imageSubresource.mipLevel = level;
                    region.imageSubresource.baseArrayLayer = layer;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = { width, height, 1 };
                    regions.push_back(region);
                    byteOffset += mipByteSize(destination.format, width, height);
                    width = width > 1 ? width / 2 : 1;
                    height = height > 1 ? height / 2 : 1;
                }
            }
            if (byteOffset != data.size_bytes())
                throw std::invalid_argument("Image upload byte count does not match mip extents");

            VulkanCommandList commands(commandBuffer_);
            commands.transition(staging, ResourceState::CopySource);
            commands.transition(destination, ResourceState::CopyDestination);
            vkCmdCopyBufferToImage(commandBuffer_, staging.buffer, destination.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<uint32_t>(regions.size()), regions.data());
            commands.transition(destination, finalState);
            stagingBuffers_.push_back(staging);
            pendingBytes_ += static_cast<uint64_t>(data.size_bytes());
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
