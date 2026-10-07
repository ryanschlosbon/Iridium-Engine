#pragma once

// M7R R4b.3: backend-neutral transient-memory aliasing planner.
//
// Places every aliasing-eligible transient image of a compiled graph
// (CompiledResource::aliasEligibility == Eligible) at an offset in one of a
// few alias heaps, so resources whose lifetimes never overlap share memory.
// The backend supplies each resource's memory requirements (size, alignment,
// allowed memory types) before any image exists; the planner never touches a
// device. Planning runs once per graph rebuild and allocates.

#include "renderer/graph/RenderGraph.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Iridium::RenderGraph {

    // The memory one logical resource needs. `typeMask` is a bit set of the
    // memory types the resource may live in (Vulkan memoryTypeBits); resources
    // share a heap only when their masks intersect. Entries of resources that
    // are not eligible are ignored.
    struct TransientMemoryRequirement {
        uint64_t size = 0;
        uint64_t alignment = 1;
        uint32_t typeMask = 0;
    };

    struct AliasingOptions {
        // Lower bound on every placement's alignment (power of two), for
        // example a device granularity the backend wants to respect.
        uint64_t minimumAlignment = 1;
    };

    struct AliasHeap {
        uint64_t size = 0;
        // The largest member alignment: the heap's base must be aligned to it.
        uint64_t alignment = 1;
        // Intersection of every member's typeMask (never zero).
        uint32_t typeMask = 0;
    };

    struct AliasPlacement {
        uint32_t heap = InvalidIndex;
        uint64_t offset = 0;
        uint64_t size = 0;

        [[nodiscard]] constexpr bool placed() const noexcept {
            return heap != InvalidIndex;
        }
    };

    struct AliasPlan {
        std::vector<AliasHeap> heaps;
        // Indexed by logical resource; unplaced (heap == InvalidIndex) for
        // every resource that is not eligible.
        std::vector<AliasPlacement> placements;
        // Logical indices of the placed resources in greedy placement order.
        std::vector<uint32_t> placementOrder;
        // CSR over logical resources: aliasPredecessors(r) lists the placed
        // resources whose memory range overlaps r's in the same heap and whose
        // lifetime ends before r's begins (lastUse < r.firstUse), ordered by
        // (lastUse, logical index). Their final accesses form the source scope
        // of r's first-use (UNDEFINED-layout) barrier. Size resources + 1.
        std::vector<uint32_t> predecessorOffsets;
        std::vector<uint32_t> predecessors;
        // Sum of the placed resources' sizes, without aliasing.
        uint64_t requestedBytes = 0;
        // Sum of the heap sizes.
        uint64_t committedBytes = 0;
        // The largest sum of placed sizes live in one pass (inclusive
        // [firstUse, lastUse]), and the first pass reaching it. A lower bound
        // on committedBytes.
        uint64_t peakLiveBytes = 0;
        uint32_t peakLivePass = InvalidIndex;

        [[nodiscard]] std::span<const uint32_t> aliasPredecessors(
            uint32_t logicalResourceIndex) const noexcept;
    };

    // Interference is the overlap of inclusive [firstUse, lastUse] intervals in
    // compiled order. Placement is greedy and deterministic: resources sorted
    // by size (descending), then firstUse, then logical index; each joins the
    // compatible heap (intersecting typeMask) it grows least (lowest index on
    // ties; a new heap when none is compatible), at the lowest offset among 0
    // and the ends of the interfering resources already placed in that heap,
    // aligned up, that overlaps none of them.
    //
    // `requirements` is indexed by logical resource and must cover every
    // resource of `graph`. Throws std::invalid_argument for a size mismatch, an
    // eligible resource with zero size or typeMask, or a non-power-of-two
    // alignment.
    [[nodiscard]] AliasPlan planTransientAliasing(const CompiledGraph& graph,
        std::span<const TransientMemoryRequirement> requirements,
        const AliasingOptions& options = {});

} // namespace Iridium::RenderGraph
