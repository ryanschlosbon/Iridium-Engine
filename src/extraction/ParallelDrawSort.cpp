// M7R R5c.7: see ParallelDrawSort.h.
#include "extraction/ParallelDrawSort.h"

#include "core/tasks/TaskSystem.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Iridium {

    // The sorted-surface queue's ambiguous-interval count over its sorted keys
    // (read-only while the gather and the compatibility sort run).
    struct TransparentDrawSorter::IntervalTask final : Tasks::TaskSet {
        IntervalTask() noexcept
            : TaskSet(Tasks::TaskPriority::FrameCritical, 1, 1,
                "cpu.render.transparent.intervals.task") {}

        std::span<const TransparentWorkSortKey> keys;
        std::span<TransparentIntervalEndpoint> endpoints;
        std::span<float> nears;
        std::span<uint32_t> fenwick;
        uint64_t result = 0;
        bool failed = false;

    private:
        void execute(Tasks::TaskRange, uint32_t) override {
            try {
                result = sweepAmbiguousTransparentIntervals(keys, endpoints,
                    nears, fenwick);
            }
            catch (...) {
                failed = true;
            }
        }
    };

    namespace {

        [[nodiscard]] uint32_t packetOf(const TransparentWorkSortKey& key) noexcept {
            return key.packet;
        }

        [[nodiscard]] uint32_t packetOf(
            const TransparentCompatibilitySortKey& key) noexcept {
            return key.work.packet;
        }

        // Runs fn(index) for index in [0, count): one index inline, more as a
        // frame-critical task set the calling (main) thread joins.
        template <class Fn>
        void runIndices(Tasks::TaskSystem& tasks, uint32_t count,
            const char* scope, Fn&& fn) {
            if (count == 0) return;
            if (count == 1) {
                fn(0u);
                return;
            }
            tasks.parallelFor(Tasks::TaskPriority::FrameCritical, count, 1,
                [&fn](Tasks::TaskRange range, uint32_t) {
                    for (uint32_t index = range.begin; index < range.end; ++index)
                        fn(index);
                }, scope);
        }

    } // namespace

    TransparentDrawSorter::TransparentDrawSorter(Tasks::TaskSystem* tasks)
        : tasks_(tasks) {
        if (tasks_) intervalTask_ = std::make_unique<IntervalTask>();
    }

    TransparentDrawSorter::~TransparentDrawSorter() {
        joinIntervals();
    }

    void TransparentDrawSorter::joinIntervals() noexcept {
        if (!intervalsPending_) return;
        tasks_->wait(*intervalTask_);
        intervalsPending_ = false;
    }

    uint32_t TransparentDrawSorter::sliceCount(size_t packets) const noexcept {
        if (!tasks_ || packets < ParallelSortMinimumPackets) return 1;
        return static_cast<uint32_t>(std::clamp<size_t>(
            packets / PacketsPerSlice, 2, MaximumSlices));
    }

    template <class Key, class MakeKey, class Less>
    bool TransparentDrawSorter::sortParallel(std::vector<DrawPacket>& queue,
        std::vector<Key>& keys, std::vector<Key>& merged,
        std::vector<DrawPacket>& gathered, MakeKey makeKey, Less less,
        const Key*& sortedKeys) {
        Tasks::TaskSystem& tasks = *tasks_;
        const size_t count = queue.size();
        if (count >= UINT32_MAX)
            throw std::length_error("draw packet queue exceeds 32-bit indices");
        const uint32_t slices = sliceCount(count);
        const std::span<const DrawPacket> packets = queue;
        const auto keyLess = [packets, less](const Key& lhs, const Key& rhs) {
            return less(lhs, rhs, packets);
        };
        const auto bound = [count, slices](uint32_t slice) {
            return count * slice / slices;
        };
        // Steady frames keep the sizes: no construction, no allocation.
        keys.resize(count);
        merged.resize(count);
        gathered.resize(count);
        sliceTies_.resize(slices);

        // 1. Keys of each slice, built and sorted by one task.
        Key* source = keys.data();
        Key* target = merged.data();
        runIndices(tasks, slices, "cpu.render.sort.transparent.slice",
            [&](uint32_t slice) {
                const size_t begin = bound(slice);
                const size_t end = bound(slice + 1u);
                for (size_t index = begin; index < end; ++index)
                    source[index] = makeKey(packets[index],
                        static_cast<uint32_t>(index));
                std::sort(source + begin, source + end, keyLess);
            });
        // 2. Pairwise merges of sorted runs of `width` slices.
        for (uint32_t width = 1; width < slices; width *= 2u) {
            const uint32_t pairs = (slices + 2u * width - 1u) / (2u * width);
            runIndices(tasks, pairs, "cpu.render.sort.transparent.merge",
                [&](uint32_t pair) {
                    const size_t begin = bound((std::min)(2u * width * pair, slices));
                    const size_t middle = bound((std::min)(
                        2u * width * pair + width, slices));
                    const size_t end = bound((std::min)(
                        2u * width * pair + 2u * width, slices));
                    std::merge(source + begin, source + middle, source + middle,
                        source + end, target + begin, keyLess);
                });
            std::swap(source, target);
        }
        sortedKeys = source;
        return true;
    }

    void TransparentDrawSorter::sort(std::vector<DrawPacket>& compatibilityQueue,
        std::vector<DrawPacket>& sortedSurfaceQueue,
        CompactDrawSortScratch& serialScratch) {
        joinIntervals();
        stats_ = {};
        // 3. Strict order of every adjacent pair and the gather, in one pass
        // over output slices; the result is used only without a tie.
        const auto verifyAndGather = [this](const auto* sortedKeys, auto less,
            std::span<const DrawPacket> packets,
            std::vector<DrawPacket>& gathered) {
            const size_t count = packets.size();
            const uint32_t slices = sliceCount(count);
            runIndices(*tasks_, slices, "cpu.render.sort.transparent.gather",
                [&](uint32_t slice) {
                    const size_t begin = count * slice / slices;
                    const size_t end = count * (slice + 1u) / slices;
                    uint8_t tie = 0;
                    for (size_t index = begin; index < end; ++index) {
                        gathered[index] = packets[packetOf(sortedKeys[index])];
                        if (index + 1u < count && !less(sortedKeys[index],
                                sortedKeys[index + 1u], packets))
                            tie = 1;
                    }
                    sliceTies_[slice] = tie;
                });
            for (uint32_t slice = 0; slice < slices; ++slice)
                if (sliceTies_[slice] != 0) return false;
            return true;
        };

        // Sorted surfaces first, so their interval count overlaps the rest.
        if (sliceCount(sortedSurfaceQueue.size()) > 1) {
            stats_.sortedSurfaceParallel = true;
            const TransparentWorkSortKey* sortedKeys = nullptr;
            (void)sortParallel(sortedSurfaceQueue, workKeys_, workMerged_,
                workGathered_,
                [](const DrawPacket& packet, uint32_t index) {
                    return makeTransparentWorkSortKey(packet, index);
                },
                [](const TransparentWorkSortKey& lhs,
                    const TransparentWorkSortKey& rhs,
                    std::span<const DrawPacket> packets) {
                    return transparentWorkKeyLess(lhs, rhs, packets);
                }, sortedKeys);
            const size_t count = sortedSurfaceQueue.size();
            intervalEndpoints_.resize(count);
            intervalNears_.resize(count);
            intervalFenwick_.resize(count + 1u);
            intervalTask_->keys = { sortedKeys, count };
            intervalTask_->endpoints = intervalEndpoints_;
            intervalTask_->nears = intervalNears_;
            intervalTask_->fenwick = intervalFenwick_;
            intervalTask_->result = 0;
            intervalTask_->failed = false;
            tasks_->submit(*intervalTask_);
            intervalsPending_ = true;
            if (verifyAndGather(sortedKeys,
                    [](const TransparentWorkSortKey& lhs,
                        const TransparentWorkSortKey& rhs,
                        std::span<const DrawPacket> packets) {
                        return transparentWorkKeyLess(lhs, rhs, packets);
                    }, sortedSurfaceQueue, workGathered_)) {
                sortedSurfaceQueue.swap(workGathered_);
            }
            else {
                // A tie: the serial sort decides its order; the interval
                // count is then taken from the sorted packets.
                stats_.sortedSurfaceTieFallback = true;
                joinIntervals();
                sortTransparentWorkDrawPackets(sortedSurfaceQueue, serialScratch);
            }
        }
        else {
            sortTransparentWorkDrawPackets(sortedSurfaceQueue, serialScratch);
        }

        if (sliceCount(compatibilityQueue.size()) > 1) {
            stats_.compatibilityParallel = true;
            const TransparentCompatibilitySortKey* sortedKeys = nullptr;
            const auto less = [](const TransparentCompatibilitySortKey& lhs,
                const TransparentCompatibilitySortKey& rhs,
                std::span<const DrawPacket> packets) {
                return transparentCompatibilityKeyLess(lhs, rhs, packets);
            };
            (void)sortParallel(compatibilityQueue, compatibilityKeys_,
                compatibilityMerged_, compatibilityGathered_,
                [](const DrawPacket& packet, uint32_t index) {
                    return makeTransparentCompatibilitySortKey(packet, index);
                }, less, sortedKeys);
            if (verifyAndGather(sortedKeys, less, compatibilityQueue,
                    compatibilityGathered_)) {
                compatibilityQueue.swap(compatibilityGathered_);
            }
            else {
                stats_.compatibilityTieFallback = true;
                sortTransparentCompatibilityDrawPackets(compatibilityQueue,
                    serialScratch);
            }
        }
        else {
            sortTransparentCompatibilityDrawPackets(compatibilityQueue,
                serialScratch);
        }
    }

    uint64_t TransparentDrawSorter::ambiguousIntervals(
        std::span<const DrawPacket> sortedSurfaceQueue) {
        if (intervalsPending_ && !stats_.sortedSurfaceTieFallback) {
            joinIntervals();
            if (!intervalTask_->failed) return intervalTask_->result;
        }
        joinIntervals();
        const size_t count = sortedSurfaceQueue.size();
        intervalEndpoints_.resize(count);
        intervalNears_.resize(count);
        intervalFenwick_.resize(count + 1u);
        return sweepAmbiguousTransparentIntervals(sortedSurfaceQueue,
            intervalEndpoints_, intervalNears_, intervalFenwick_);
    }

} // namespace Iridium
