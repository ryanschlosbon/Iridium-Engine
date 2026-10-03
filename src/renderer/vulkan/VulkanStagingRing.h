#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Iridium {

    // M7R R4d.2: when a batch of staging bytes may be reused. Each upload lane
    // (transfer, graphics) signals its own timeline semaphore; 0 means the
    // batch has no work on that lane.
    struct StagingRetireKey {
        uint64_t transferValue = 0;
        uint64_t graphicsValue = 0;

        [[nodiscard]] bool completedBy(uint64_t transferCompleted,
            uint64_t graphicsCompleted) const noexcept {
            return transferValue <= transferCompleted &&
                graphicsValue <= graphicsCompleted;
        }
    };

    // The device-free bookkeeping of the persistently mapped staging ring.
    // Allocations are contiguous (an allocation that would straddle the end
    // skips the tail and starts at 0). Everything allocated since the last
    // close() belongs to the open batch; close(key) seals it, and retire()
    // releases sealed batches oldest first once their key has completed. The
    // batch list is a fixed array, so steady use allocates nothing.
    class StagingRingAllocator final {
    public:
        static constexpr size_t MaxBatches = 64;

        void reset(uint64_t capacity) noexcept;

        // The offset of `size` bytes aligned to `alignment` (a power of two),
        // or nullopt when the live batches leave no contiguous room (or the
        // batch list is full and nothing is open).
        [[nodiscard]] std::optional<uint64_t> allocate(uint64_t size,
            uint64_t alignment) noexcept;
        // Seals the open batch (no-op when nothing was allocated).
        void close(StagingRetireKey key) noexcept;
        // Releases sealed batches whose key has completed, oldest first.
        size_t retire(uint64_t transferCompleted, uint64_t graphicsCompleted) noexcept;

        [[nodiscard]] uint64_t capacity() const noexcept { return capacity_; }
        // Bytes held by sealed and open batches, including alignment and
        // wrap padding.
        [[nodiscard]] uint64_t usedBytes() const noexcept { return used_; }
        [[nodiscard]] bool hasOpenAllocations() const noexcept { return openBytes_ != 0; }
        [[nodiscard]] size_t sealedBatchCount() const noexcept { return batchCount_; }
        [[nodiscard]] std::optional<StagingRetireKey> oldestSealedKey() const noexcept;

    private:
        struct Batch {
            uint64_t end = 0;
            uint64_t bytes = 0;
            StagingRetireKey key{};
        };

        uint64_t capacity_ = 0;
        uint64_t head_ = 0;
        uint64_t tail_ = 0;
        uint64_t used_ = 0;
        uint64_t openBytes_ = 0;
        std::array<Batch, MaxBatches> batches_{};
        size_t firstBatch_ = 0;
        size_t batchCount_ = 0;
    };

} // namespace Iridium
