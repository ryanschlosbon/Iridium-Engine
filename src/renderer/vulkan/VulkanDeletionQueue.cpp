#include "VulkanDeletionQueue.h"

#include "DescriptorAllocator.h"

#include <bit>
#include <memory>

namespace Iridium {

    void VulkanDeletionQueue::init(VkDevice device,
        VulkanResourceAllocator* allocator) {
        device_ = device;
        allocator_ = allocator;
        if (entries_.capacity() < InitialCapacity)
            entries_.reserve(InitialCapacity);
    }

    void VulkanDeletionQueue::push(const VulkanDeletionEntry& entry) {
        // Reclaim the collected prefix before the vector would grow (never
        // while collecting: a callback may retire more entries).
        if (!collecting_ && head_ != 0 &&
            entries_.size() == entries_.capacity()) {
            entries_.erase(entries_.begin(),
                entries_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
        entries_.push_back(entry);
    }

    void VulkanDeletionQueue::retire(uint64_t retireValue,
        const VulkanBufferResource& buffer) {
        if (buffer.buffer == VK_NULL_HANDLE &&
            buffer.vmaAllocation == VK_NULL_HANDLE) return;
        VulkanDeletionEntry entry{};
        entry.retireValue = retireValue;
        entry.kind = VulkanDeletionKind::Buffer;
        std::construct_at(&entry.payload.buffer, buffer);
        push(entry);
    }

    void VulkanDeletionQueue::retire(uint64_t retireValue,
        const VulkanImageResource& image) {
        if (image.image == VK_NULL_HANDLE && image.view == VK_NULL_HANDLE &&
            image.vmaAllocation == VK_NULL_HANDLE) return;
        VulkanDeletionEntry entry{};
        entry.retireValue = retireValue;
        entry.kind = VulkanDeletionKind::Image;
        std::construct_at(&entry.payload.image, image);
        push(entry);
    }

    namespace {
        template <class Handle>
        uint64_t handleBits(Handle handle) noexcept {
            if constexpr (sizeof(Handle) == sizeof(uint64_t))
                return std::bit_cast<uint64_t>(handle);
            else
                return static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(handle));
        }
        template <class Handle>
        Handle handleFrom(uint64_t bits) noexcept {
            if constexpr (sizeof(Handle) == sizeof(uint64_t))
                return std::bit_cast<Handle>(bits);
            else
                return reinterpret_cast<Handle>(static_cast<uintptr_t>(bits));
        }
    }

#define IRIDIUM_RETIRE_HANDLE(function, Type, entryKind)                   \
    void VulkanDeletionQueue::function(uint64_t retireValue, Type handle) { \
        if (handle == VK_NULL_HANDLE) return;                               \
        VulkanDeletionEntry entry{};                                        \
        entry.retireValue = retireValue;                                    \
        entry.kind = entryKind;                                             \
        entry.payload.handle = handleBits(handle);                          \
        push(entry);                                                        \
    }

    IRIDIUM_RETIRE_HANDLE(retireImageView, VkImageView, VulkanDeletionKind::ImageView)
    IRIDIUM_RETIRE_HANDLE(retireSampler, VkSampler, VulkanDeletionKind::Sampler)
    IRIDIUM_RETIRE_HANDLE(retirePipeline, VkPipeline, VulkanDeletionKind::Pipeline)
    IRIDIUM_RETIRE_HANDLE(retirePipelineLayout, VkPipelineLayout,
        VulkanDeletionKind::PipelineLayout)
#undef IRIDIUM_RETIRE_HANDLE

    void VulkanDeletionQueue::retireDescriptorSet(uint64_t retireValue,
        ::DescriptorAllocator& allocator, VkDescriptorSet set) {
        if (set == VK_NULL_HANDLE) return;
        VulkanDeletionEntry entry{};
        entry.retireValue = retireValue;
        entry.kind = VulkanDeletionKind::DescriptorSet;
        std::construct_at(&entry.payload.descriptorSet,
            VulkanDeletionDescriptorSet{ &allocator, set });
        push(entry);
    }

    void VulkanDeletionQueue::retireCallback(uint64_t retireValue,
        const VulkanDeletionCallback& callback) {
        if (callback.function == nullptr) return;
        VulkanDeletionEntry entry{};
        entry.retireValue = retireValue;
        entry.kind = VulkanDeletionKind::Callback;
        std::construct_at(&entry.payload.callback, callback);
        push(entry);
    }

    void VulkanDeletionQueue::destroy(VulkanDeletionEntry& entry) noexcept {
        switch (entry.kind) {
        case VulkanDeletionKind::Buffer:
            if (allocator_ != nullptr) allocator_->destroy(entry.payload.buffer);
            break;
        case VulkanDeletionKind::Image:
            if (allocator_ != nullptr) allocator_->destroy(entry.payload.image);
            break;
        case VulkanDeletionKind::ImageView:
            if (device_ != VK_NULL_HANDLE)
                vkDestroyImageView(device_,
                    handleFrom<VkImageView>(entry.payload.handle), nullptr);
            break;
        case VulkanDeletionKind::Sampler:
            if (device_ != VK_NULL_HANDLE)
                vkDestroySampler(device_,
                    handleFrom<VkSampler>(entry.payload.handle), nullptr);
            break;
        case VulkanDeletionKind::Pipeline:
            if (device_ != VK_NULL_HANDLE)
                vkDestroyPipeline(device_,
                    handleFrom<VkPipeline>(entry.payload.handle), nullptr);
            break;
        case VulkanDeletionKind::PipelineLayout:
            if (device_ != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device_,
                    handleFrom<VkPipelineLayout>(entry.payload.handle), nullptr);
            break;
        case VulkanDeletionKind::DescriptorSet:
            try {
                entry.payload.descriptorSet.allocator->free(
                    entry.payload.descriptorSet.set);
            }
            catch (...) {}
            break;
        case VulkanDeletionKind::Callback:
            entry.payload.callback.function(entry.payload.callback.user,
                entry.payload.callback.arguments);
            break;
        }
    }

    size_t VulkanDeletionQueue::collect(uint64_t completedValue) noexcept {
        size_t destroyed = 0;
        collecting_ = true;
        while (head_ < entries_.size() &&
            entries_[head_].retireValue <= completedValue) {
            // A copy: a callback may append (and reallocate) while it runs.
            VulkanDeletionEntry entry = entries_[head_++];
            destroy(entry);
            ++destroyed;
        }
        collecting_ = false;
        if (head_ == entries_.size()) {
            entries_.clear();
            head_ = 0;
        }
        return destroyed;
    }

    size_t VulkanDeletionQueue::flush() noexcept {
        size_t destroyed = 0;
        collecting_ = true;
        while (head_ < entries_.size()) {
            VulkanDeletionEntry entry = entries_[head_++];
            destroy(entry);
            ++destroyed;
        }
        collecting_ = false;
        entries_.clear();
        head_ = 0;
        return destroyed;
    }

    void VulkanDeletionQueue::clear() noexcept {
        entries_.clear();
        head_ = 0;
    }

} // namespace Iridium
