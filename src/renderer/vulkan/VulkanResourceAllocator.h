#pragma once

#include "renderer/rhi/RhiResourceTypes.h"
#include "profiling/MemoryProfile.h"

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
        ProfileMemoryAllocation allocation;

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
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        ResourceState state = ResourceState::Undefined;
        ProfileMemoryAllocation allocation;

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

        void init(VkPhysicalDevice physicalDevice, VkDevice device,
            bool memoryBudgetAvailable);
        void cleanup();

        // Newly created resources begin in ResourceState::Undefined.
        [[nodiscard]] VulkanBufferResource createBuffer(
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkMemoryPropertyFlags memoryProperties,
            bool persistentlyMapped = false,
            ProfileMemoryCategory category =
                ProfileMemoryCategory::OtherUnclassified);
        // Images use VK_IMAGE_LAYOUT_UNDEFINED at creation and receive a matching 2D view.
        [[nodiscard]] VulkanImageResource createImage2D(
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage,
            VkImageAspectFlags aspect,
            ProfileMemoryCategory category =
                ProfileMemoryCategory::OtherUnclassified,
            uint32_t mipLevels = 1,
            uint32_t arrayLayers = 1,
            VkImageCreateFlags flags = 0,
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D);

        void destroy(VulkanBufferResource& resource) noexcept;
        void destroy(VulkanImageResource& resource) noexcept;
        void write(VulkanBufferResource& resource, VkDeviceSize offset, std::span<const std::byte> data);
        void reclassify(VulkanImageResource& resource,
            ProfileMemoryCategory category) noexcept;
        [[nodiscard]] FrameMemoryProfile memorySnapshot() const noexcept;

    private:
        [[nodiscard]] uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties memoryProperties_{};
        MemoryProfileAccumulator memoryProfile_;
        bool memoryBudgetAvailable_ = false;
    };

} // namespace Iridium
