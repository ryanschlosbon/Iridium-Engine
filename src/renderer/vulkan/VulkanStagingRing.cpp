#include "VulkanStagingRing.h"

#include <algorithm>

namespace Iridium {

    namespace {

        [[nodiscard]] uint64_t alignUp(uint64_t value, uint64_t alignment) noexcept {
            return alignment <= 1 ? value : (value + alignment - 1) & ~(alignment - 1);
        }

    } // namespace

    void StagingRingAllocator::reset(uint64_t capacity) noexcept {
        capacity_ = capacity;
        head_ = 0;
        tail_ = 0;
        used_ = 0;
        openBytes_ = 0;
        firstBatch_ = 0;
        batchCount_ = 0;
    }

    std::optional<uint64_t> StagingRingAllocator::allocate(uint64_t size,
        uint64_t alignment) noexcept {
        size = (std::max)(size, uint64_t{ 1 });
        if (size > capacity_) return std::nullopt;
        if (used_ == 0) {
            head_ = 0;
            tail_ = 0;
        }
        uint64_t offset = 0;
        uint64_t consumed = 0;
        if (used_ != 0 && head_ == tail_) {
            return std::nullopt; // full: the live bytes wrap onto the tail
        }
        if (head_ >= tail_) {
            // Live bytes are [tail, head): room at the end, else at the start.
            const uint64_t aligned = alignUp(head_, alignment);
            if (aligned <= capacity_ && size <= capacity_ - aligned) {
                offset = aligned;
                consumed = aligned - head_ + size;
            }
            else if (used_ == 0 || size <= tail_) {
                offset = 0;
                consumed = (capacity_ - head_) + size;
                if (used_ != 0) ++wraps_;
            }
            else {
                return std::nullopt;
            }
        }
        else {
            // Wrapped: live bytes are [tail, capacity) and [0, head).
            const uint64_t aligned = alignUp(head_, alignment);
            if (aligned > tail_ || size > tail_ - aligned) return std::nullopt;
            offset = aligned;
            consumed = aligned - head_ + size;
        }
        head_ = offset + size;
        used_ += consumed;
        openBytes_ += consumed;
        return offset;
    }

    void StagingRingAllocator::close(StagingRetireKey key) noexcept {
        if (openBytes_ == 0) return;
        if (batchCount_ == MaxBatches) {
            // Merge into the newest batch: it then retires with the later key.
            Batch& newest = batches_[(firstBatch_ + batchCount_ - 1) % MaxBatches];
            newest.end = head_;
            newest.bytes += openBytes_;
            newest.key.transferValue = (std::max)(newest.key.transferValue,
                key.transferValue);
            newest.key.graphicsValue = (std::max)(newest.key.graphicsValue,
                key.graphicsValue);
        }
        else {
            batches_[(firstBatch_ + batchCount_) % MaxBatches] =
                Batch{ head_, openBytes_, key };
            ++batchCount_;
        }
        openBytes_ = 0;
    }

    size_t StagingRingAllocator::retire(uint64_t transferCompleted,
        uint64_t graphicsCompleted) noexcept {
        size_t retired = 0;
        while (batchCount_ != 0) {
            const Batch& oldest = batches_[firstBatch_];
            if (!oldest.key.completedBy(transferCompleted, graphicsCompleted)) break;
            tail_ = oldest.end;
            used_ -= oldest.bytes;
            firstBatch_ = (firstBatch_ + 1) % MaxBatches;
            --batchCount_;
            ++retired;
        }
        if (used_ == 0) {
            head_ = 0;
            tail_ = 0;
        }
        return retired;
    }

    std::optional<StagingRetireKey> StagingRingAllocator::oldestSealedKey() const noexcept {
        if (batchCount_ == 0) return std::nullopt;
        return batches_[firstBatch_].key;
    }

} // namespace Iridium
