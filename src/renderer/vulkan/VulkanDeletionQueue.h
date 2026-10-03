#pragma once

// M7R R4c.1: fence-keyed deferred deletion. Every entry carries the frame
// serial after which the GPU can no longer reference it:
//   retireValue = frameOpen ? lastSubmittedSerial + 1 : lastSubmittedSerial
// (VulkanFrameScheduler::retireValue). VulkanFrameScheduler::beginFrame
// collects every entry whose value the completed serial has reached, right
// after the slot's fence wait; cleanup flushes the rest after the device is
// idle. Keys are non-decreasing in retire order, so collection is FIFO; an
// out-of-order key only delays the entries behind it (never early).
//
// Entries are plain data (no std::function): Vulkan handles, allocator
// resources, a descriptor set with its allocator, or a function-pointer
// callback with four 64-bit arguments. Capacity is reserved up front; the
// vector grows only on a frame that retires more than it holds.

#include "VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    enum class VulkanDeletionKind : uint8_t {
        Buffer,
        Image,
        ImageView,
        Sampler,
        Pipeline,
        PipelineLayout,
        DescriptorSet,
        Callback,
    };

    // A callback entry: `function(user, arguments)` runs once at collection.
    // It must not throw.
    using VulkanDeletionArguments = std::array<uint64_t, 4>;
    using VulkanDeletionFunction = void (*)(void* user,
        const VulkanDeletionArguments& arguments);

    struct VulkanDeletionCallback {
        VulkanDeletionFunction function = nullptr;
        void* user = nullptr;
        VulkanDeletionArguments arguments{};
    };

    struct VulkanDeletionDescriptorSet {
        ::DescriptorAllocator* allocator = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    struct VulkanDeletionEntry {
        uint64_t retireValue = 0;
        VulkanDeletionKind kind = VulkanDeletionKind::Callback;
        // Every member is trivially copyable; `kind` selects the live one.
        union Payload {
            VulkanBufferResource buffer;
            VulkanImageResource image;
            uint64_t handle; // view, sampler, pipeline, pipeline layout
            VulkanDeletionDescriptorSet descriptorSet;
            VulkanDeletionCallback callback;
            Payload() noexcept : handle(0) {}
        } payload;
    };

    class VulkanDeletionQueue final {
    public:
        static constexpr size_t InitialCapacity = 256;

        VulkanDeletionQueue() = default;
        VulkanDeletionQueue(const VulkanDeletionQueue&) = delete;
        VulkanDeletionQueue& operator=(const VulkanDeletionQueue&) = delete;

        // `allocator` destroys buffer and image entries; `device` destroys
        // the raw handles. Either may be set later (attachAllocator).
        void init(VkDevice device, VulkanResourceAllocator* allocator);
        void attachAllocator(VulkanResourceAllocator* allocator) noexcept {
            allocator_ = allocator;
        }

        // Invalid (null) resources and handles are not queued.
        void retire(uint64_t retireValue, const VulkanBufferResource& buffer);
        void retire(uint64_t retireValue, const VulkanImageResource& image);
        void retireImageView(uint64_t retireValue, VkImageView view);
        void retireSampler(uint64_t retireValue, VkSampler sampler);
        void retirePipeline(uint64_t retireValue, VkPipeline pipeline);
        void retirePipelineLayout(uint64_t retireValue, VkPipelineLayout layout);
        void retireDescriptorSet(uint64_t retireValue,
            ::DescriptorAllocator& allocator, VkDescriptorSet set);
        void retireCallback(uint64_t retireValue,
            const VulkanDeletionCallback& callback);

        // Destroys, in retire order, every entry whose value is at most
        // `completedValue`. Returns the number destroyed.
        size_t collect(uint64_t completedValue) noexcept;
        // Destroys every entry (device idle). Returns the number destroyed.
        size_t flush() noexcept;
        // Drops every entry without destroying it (failed initialization).
        void clear() noexcept;

        [[nodiscard]] size_t size() const noexcept { return entries_.size() - head_; }
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }
        [[nodiscard]] size_t capacity() const noexcept { return entries_.capacity(); }
        // The oldest pending key (0 when empty).
        [[nodiscard]] uint64_t oldestRetireValue() const noexcept {
            return empty() ? 0 : entries_[head_].retireValue;
        }

    private:
        void push(const VulkanDeletionEntry& entry);
        void destroy(VulkanDeletionEntry& entry) noexcept;

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        std::vector<VulkanDeletionEntry> entries_;
        size_t head_ = 0;
        bool collecting_ = false;
    };

} // namespace Iridium
