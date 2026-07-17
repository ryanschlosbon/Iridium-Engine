#pragma once

#include "renderer/rhi/RhiResourceTypes.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Iridium {

    struct VulkanBufferResource {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        ResourceState state = ResourceState::Undefined;

        [[nodiscard]] bool isValid() const noexcept {
            return buffer != VK_NULL_HANDLE && memory != VK_NULL_HANDLE;
        }
    };

    struct VulkanImageResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = 0;
        ResourceState state = ResourceState::Undefined;

        [[nodiscard]] bool isValid() const noexcept {
            return image != VK_NULL_HANDLE && memory != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
        }
    };

    // Stage 3 intentionally uses one VkDeviceMemory allocation per resource. Its
    // centralized ownership can later be replaced by a block allocator or VMA.
    class VulkanResourceAllocator final {
    public:
        VulkanResourceAllocator() = default;
        VulkanResourceAllocator(const VulkanResourceAllocator&) = delete;
        VulkanResourceAllocator& operator=(const VulkanResourceAllocator&) = delete;

        void init(VkPhysicalDevice physicalDevice, VkDevice device);
        void cleanup();

        // Newly created resources begin in ResourceState::Undefined.
        [[nodiscard]] VulkanBufferResource createBuffer(
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags memoryProperties,
            bool persistentlyMapped = false);
        // Images use VK_IMAGE_LAYOUT_UNDEFINED at creation and receive a matching 2D view.
        [[nodiscard]] VulkanImageResource createImage2D(
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage,
            VkImageAspectFlags aspect);

        void destroy(VulkanBufferResource& resource);
        void destroy(VulkanImageResource& resource);
        void write(VulkanBufferResource& resource, VkDeviceSize offset, std::span<const std::byte> data);

    private:
        [[nodiscard]] uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
    };

} // namespace Iridium
