#pragma once

#include "VulkanResourceAllocator.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    // Batches uploads on the graphics queue. Commands begin lazily on the first
    // enqueue; flush submits once, waits once on the batch fence, then has the
    // allocator explicitly destroy all retained staging resources.
    class VulkanUploadContext final {
    public:
        VulkanUploadContext() = default;
        VulkanUploadContext(const VulkanUploadContext&) = delete;
        VulkanUploadContext& operator=(const VulkanUploadContext&) = delete;

        void init(VkDevice device, VkQueue graphicsQueue, uint32_t graphicsQueueFamily,
            VulkanResourceAllocator& allocator);
        void cleanup();

        void enqueueBufferUpload(VulkanBufferResource& destination, std::span<const std::byte> data,
            ResourceState finalState);
        void enqueueImageUpload(VulkanImageResource& destination, std::span<const std::byte> data,
            ResourceState finalState);
        void enqueueTransition(VulkanImageResource& destination, ResourceState state);
        void flush();

        [[nodiscard]] bool hasPendingWork() const noexcept;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphicsQueue_ = VK_NULL_HANDLE;
        uint32_t graphicsQueueFamily_ = 0;
        VulkanResourceAllocator* allocator_ = nullptr;

        VkCommandPool commandPool_ = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
        bool batchOpen_ = false;
        std::vector<VulkanBufferResource> stagingBuffers_;
    };

} // namespace Iridium
