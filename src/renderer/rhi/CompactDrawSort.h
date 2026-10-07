#pragma once

// M7R R5c.3: compact (key, index) sorts for DrawPacket queues.
//
// Each helper sorts a small key array with std::sort and a comparator that
// reads the same fields, in the same order, with the same operators as the
// packet comparator it replaces, then moves each 240-byte packet once, in
// place, following the permutation's cycles.
// std::sort's control flow depends only on comparison outcomes and the element
// count, so the permutation, including the order of ties under the non-total
// opaque comparator, is the one the packet sort produced.
// CompactDrawSortTests checks this against the packet comparators.

#include "renderer/rhi/DrawPacket.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    // Replaces the queue comparator
    //   opaqueSortKey, then geometry, then firstIndex (ties stay unordered).
    struct OpaqueDrawSortKey {
        uint64_t opaqueSortKey = 0;
        GeometryHandle geometry;
        uint32_t firstIndex = 0;
        uint32_t packet = 0;
    };
    static_assert(sizeof(OpaqueDrawSortKey) == 24);

    // Replaces transparentWorkLess. The final TransparentWorkIdentity
    // comparison reads the packet through the index.
    struct TransparentWorkSortKey {
        int32_t priority = 0;
        uint32_t workFlags = 0;
        float farDepth = 0.0f;
        float nearDepth = 0.0f;
        uint32_t packet = 0;
    };
    static_assert(sizeof(TransparentWorkSortKey) == 20);

    // Replaces transparentCompatibilityLess.
    struct TransparentCompatibilitySortKey {
        TransparentWorkSortKey work;
        float distanceToCamera = 0.0f;
        PipelineHandle pipeline;
        MaterialHandle material;
        GeometryHandle geometry;
        TransparencyExecutionMode executionMode =
            TransparencyExecutionMode::Classified;
    };
    static_assert(sizeof(TransparentCompatibilitySortKey) == 40);

    // M7R R5c.7: the key builders and key comparators the helpers below use,
    // for callers that sort keys themselves (the parallel transparent sorts in
    // extraction). `packets` is the queue the keys' indices refer to; the
    // final TransparentWorkIdentity comparison reads the packet through it.
    [[nodiscard]] inline TransparentWorkSortKey makeTransparentWorkSortKey(
        const DrawPacket& packet, uint32_t index) noexcept {
        return {
            .priority = packet.transparency.priority,
            .workFlags = packet.transparentWorkFlags,
            .farDepth = packet.transparentFarDepth,
            .nearDepth = packet.transparentNearDepth,
            .packet = index,
        };
    }

    [[nodiscard]] inline TransparentCompatibilitySortKey
        makeTransparentCompatibilitySortKey(const DrawPacket& packet,
            uint32_t index) noexcept {
        return {
            .work = makeTransparentWorkSortKey(packet, index),
            .distanceToCamera = packet.distanceToCamera,
            .pipeline = packet.pipeline,
            .material = packet.material,
            .geometry = packet.geometry,
            .executionMode = packet.transparencyExecutionMode,
        };
    }

    // transparentWorkLess over keys.
    [[nodiscard]] inline bool transparentWorkKeyLess(
        const TransparentWorkSortKey& lhs, const TransparentWorkSortKey& rhs,
        std::span<const DrawPacket> packets) noexcept {
        if (lhs.priority != rhs.priority)
            return lhs.priority < rhs.priority;
        const bool lhsValid = (lhs.workFlags &
            TransparentWorkIntervalValid) != 0;
        const bool rhsValid = (rhs.workFlags &
            TransparentWorkIntervalValid) != 0;
        if (lhsValid != rhsValid) return lhsValid;
        if (lhsValid) {
            const bool lhsIntersects = (lhs.workFlags &
                TransparentWorkCameraIntersecting) != 0;
            const bool rhsIntersects = (rhs.workFlags &
                TransparentWorkCameraIntersecting) != 0;
            if (lhsIntersects != rhsIntersects) return !lhsIntersects;
            if (lhs.farDepth != rhs.farDepth)
                return lhs.farDepth > rhs.farDepth;
            if (lhs.nearDepth != rhs.nearDepth)
                return lhs.nearDepth > rhs.nearDepth;
        }
        return transparentWorkIdentity(packets[lhs.packet]) <
            transparentWorkIdentity(packets[rhs.packet]);
    }

    // transparentCompatibilityLess over keys.
    [[nodiscard]] inline bool transparentCompatibilityKeyLess(
        const TransparentCompatibilitySortKey& lhs,
        const TransparentCompatibilitySortKey& rhs,
        std::span<const DrawPacket> packets) noexcept {
        if (lhs.executionMode != rhs.executionMode)
            return lhs.executionMode < rhs.executionMode;
        if (lhs.executionMode == TransparencyExecutionMode::Classified)
            return transparentWorkKeyLess(lhs.work, rhs.work, packets);
        if (lhs.distanceToCamera != rhs.distanceToCamera)
            return lhs.distanceToCamera > rhs.distanceToCamera;
        if (lhs.pipeline != rhs.pipeline) return lhs.pipeline < rhs.pipeline;
        if (lhs.material != rhs.material) return lhs.material < rhs.material;
        return lhs.geometry < rhs.geometry;
    }

    // M7R R5c.7: sweepAmbiguousTransparentIntervals over the keys of a queue
    // sorted by transparentWorkLess, in that order (the endpoints, groups and
    // arithmetic are those of the sorted packets, so the count is identical).
    [[nodiscard]] inline uint64_t sweepAmbiguousTransparentIntervals(
        std::span<const TransparentWorkSortKey> sortedKeys,
        std::span<TransparentIntervalEndpoint> endpointScratch,
        std::span<float> nearScratch,
        std::span<uint32_t> fenwickScratch) {
        return sweepAmbiguousTransparentIntervalsOf(sortedKeys.size(),
            [sortedKeys](size_t index) {
                const TransparentWorkSortKey& key = sortedKeys[index];
                return TransparentIntervalSample{
                    .priority = key.priority,
                    .valid = (key.workFlags & TransparentWorkIntervalValid) != 0,
                    .nearDepth = key.nearDepth,
                    .farDepth = key.farDepth,
                };
            }, endpointScratch, nearScratch, fenwickScratch);
    }

    // Caller-owned key scratch shared by the sorts of one frame. Its vectors
    // keep their capacity, so steady frames do not allocate.
    struct CompactDrawSortScratch {
        std::vector<OpaqueDrawSortKey> opaqueKeys;
        std::vector<TransparentWorkSortKey> transparentWorkKeys;
        std::vector<TransparentCompatibilitySortKey> transparentCompatibilityKeys;
    };

    // Same order as std::sort(queue, opaque comparator) above.
    void sortOpaqueDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch);
    // M7R R5c.4b: std::sort of keys built by the caller with the opaque
    // comparator; the order of keys (and of their `packet` payloads) is the one
    // the packets they describe would take.
    void sortOpaqueDrawKeys(std::span<OpaqueDrawSortKey> keys);
    // Same order as std::sort(queue, transparentWorkLess).
    void sortTransparentWorkDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch);
    // Same order as std::sort(queue, transparentCompatibilityLess).
    void sortTransparentCompatibilityDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch);

} // namespace Iridium
