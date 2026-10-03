#pragma once

#include "renderer/rhi/RhiResourceTypes.h"
#include "profiling/MemoryProfile.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Vulkan Memory Allocator handles (M7R R4b), declared exactly as vk_mem_alloc.h
// declares them so this header does not pull in the library. VMA itself is a
// private dependency of iridium_vulkan (VulkanVma.h).
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Iridium {

    struct VulkanBufferResource {
        VkBuffer buffer = VK_NULL_HANDLE;
        // The VkDeviceMemory block the buffer is bound to (shared by VMA
        // suballocations); the buffer starts at memoryOffset within it.
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VmaAllocation vmaAllocation = VK_NULL_HANDLE;
        VkDeviceSize memoryOffset = 0;
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
        // As for buffers: the bound VkDeviceMemory block and the image's offset.
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VmaAllocation vmaAllocation = VK_NULL_HANDLE;
        VkDeviceSize memoryOffset = 0;
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

    // The memory type the pre-VMA allocator chose: the lowest-indexed type allowed
    // by typeBits whose property flags include every requested flag. R4b keeps
    // memory types identical by restricting each VMA allocation to exactly this
    // type (no silent ReBAR or host-cached change).
    [[nodiscard]] std::optional<uint32_t> legacyMemoryTypeIndex(
        const VkPhysicalDeviceMemoryProperties& properties, uint32_t typeBits,
        VkMemoryPropertyFlags requiredFlags) noexcept;

    // Images whose memory requirement is at least this size (shadow maps and
    // pools, history, probe cubes, 4K render targets) get their own VkDeviceMemory;
    // smaller images and all buffers are suballocated from VMA blocks unless the
    // driver prefers or requires a dedicated allocation.
    inline constexpr VkDeviceSize DedicatedImageAllocationThresholdBytes =
        VkDeviceSize{ 16 } * 1024 * 1024;
    [[nodiscard]] constexpr bool usesDedicatedImageAllocation(
        VkDeviceSize requirementBytes) noexcept {
        return requirementBytes >= DedicatedImageAllocationThresholdBytes;
    }

    // Owns the backend's VmaAllocator (M7R R4b). Resource creation goes through
    // VMA; the 19 engine memory-profile categories stay engine-side (requested
    // bytes as before, committed bytes = the VMA allocation size), and each
    // allocation is named after its category for VMA statistics.
    class VulkanResourceAllocator final {
    public:
        VulkanResourceAllocator() = default;
        VulkanResourceAllocator(const VulkanResourceAllocator&) = delete;
        VulkanResourceAllocator& operator=(const VulkanResourceAllocator&) = delete;

        // The instance must be the one the device was created from (Vulkan 1.3);
        // memoryBudgetAvailable means VK_EXT_memory_budget is enabled on it.
        void init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
            bool memoryBudgetAvailable);
        // Every resource must have been destroyed first.
        void cleanup();
        // Advances VMA's frame index, which also refreshes its cached heap budget
        // when VK_EXT_memory_budget is enabled. Called once per frame.
        void beginFrame() noexcept;

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
        [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept {
            return physicalDevice_;
        }

    private:
        [[nodiscard]] uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VmaAllocator vma_ = VK_NULL_HANDLE;
        uint32_t frameIndex_ = 0;
        VkPhysicalDeviceMemoryProperties memoryProperties_{};
        MemoryProfileAccumulator memoryProfile_;
        bool memoryBudgetAvailable_ = false;
    };

} // namespace Iridium
