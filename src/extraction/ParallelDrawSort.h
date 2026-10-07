#pragma once

// M7R R5c.7: the transparent queue sorts of render extraction on the task
// system, with exactly the order of the serial compact sorts
// (renderer/rhi/CompactDrawSort.h).
//
// A queue of at least ParallelSortMinimumPackets is sorted as follows:
//   1. Its keys are built and sorted in contiguous slices, in parallel.
//   2. Pairs of sorted runs are merged with std::merge, in parallel rounds,
//      until one run remains.
//   3. Every adjacent pair of the result is checked to be strictly ordered by
//      the comparator. If so, no two keys are equivalent, the sorted order of
//      the keys is unique, and it is the order std::sort gives; the packets
//      are gathered into it in parallel. If any adjacent pair is equivalent
//      (a tie, which the serial std::sort orders by its own control flow),
//      the result is discarded and the serial helper sorts the queue.
// Nothing depends on the schedule: every slice, run and gather range is
// written by one task, and the outcome of step 3 is a property of the keys.
// The sorted-surface queue's ambiguous-interval count (an order statistic of
// the sorted keys) runs as a frame-critical task beside the gather and the
// compatibility sort, and is joined by ambiguousIntervals().
//
// Without a task system, or below the threshold, the serial helpers run.
// Scratch vectors keep their sizes across frames, so steady frames allocate
// and construct nothing.

#include "renderer/rhi/CompactDrawSort.h"
#include "renderer/rhi/DrawPacket.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Iridium {

    namespace Tasks { class TaskSystem; }

    class TransparentDrawSorter final {
    public:
        static constexpr size_t ParallelSortMinimumPackets = 2048;
        static constexpr size_t PacketsPerSlice = 1024;
        static constexpr uint32_t MaximumSlices = 32;

        explicit TransparentDrawSorter(Tasks::TaskSystem* tasks);
        // Joins an interval count still in flight.
        ~TransparentDrawSorter();

        TransparentDrawSorter(const TransparentDrawSorter&) = delete;
        TransparentDrawSorter& operator=(const TransparentDrawSorter&) = delete;

        // Same order as sortTransparentCompatibilityDrawPackets and
        // sortTransparentWorkDrawPackets. The vectors' storage may be
        // exchanged with this object's gather buffers. When the sorted-surface
        // queue sorts in parallel, its interval count starts on a worker.
        void sort(std::vector<DrawPacket>& compatibilityQueue,
            std::vector<DrawPacket>& sortedSurfaceQueue,
            CompactDrawSortScratch& serialScratch);

        // sweepAmbiguousTransparentIntervals of the sorted-surface queue
        // after sort(): the worker's result, or computed here.
        [[nodiscard]] uint64_t ambiguousIntervals(
            std::span<const DrawPacket> sortedSurfaceQueue);

        // Statistics of the last sort() (tests and diagnostics).
        struct Stats {
            bool compatibilityParallel = false;
            bool compatibilityTieFallback = false;
            bool sortedSurfaceParallel = false;
            bool sortedSurfaceTieFallback = false;
        };
        [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    private:
        struct IntervalTask;

        template <class Key, class MakeKey, class Less>
        [[nodiscard]] bool sortParallel(std::vector<DrawPacket>& queue,
            std::vector<Key>& keys, std::vector<Key>& merged,
            std::vector<DrawPacket>& gathered, MakeKey makeKey, Less less,
            const Key*& sortedKeys);
        [[nodiscard]] uint32_t sliceCount(size_t packets) const noexcept;
        void joinIntervals() noexcept;

        Tasks::TaskSystem* tasks_ = nullptr;
        std::vector<TransparentWorkSortKey> workKeys_;
        std::vector<TransparentWorkSortKey> workMerged_;
        std::vector<TransparentCompatibilitySortKey> compatibilityKeys_;
        std::vector<TransparentCompatibilitySortKey> compatibilityMerged_;
        std::vector<DrawPacket> workGathered_;
        std::vector<DrawPacket> compatibilityGathered_;
        std::vector<uint8_t> sliceTies_;
        std::vector<TransparentIntervalEndpoint> intervalEndpoints_;
        std::vector<float> intervalNears_;
        std::vector<uint32_t> intervalFenwick_;
        std::unique_ptr<IntervalTask> intervalTask_;
        bool intervalsPending_ = false;
        Stats stats_{};
    };

} // namespace Iridium
