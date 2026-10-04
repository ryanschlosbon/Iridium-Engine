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
