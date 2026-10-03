#include "renderer/graph/RenderGraphAliasing.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace Iridium::RenderGraph {
namespace {

    bool isPowerOfTwo(uint64_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    uint64_t alignUp(uint64_t value, uint64_t alignment) {
        const uint64_t mask = alignment - 1;
        if (value > (std::numeric_limits<uint64_t>::max)() - mask) {
            throw std::overflow_error("Transient aliasing offset overflow");
        }
        return (value + mask) & ~mask;
    }

    bool interferes(const CompiledResource& left, const CompiledResource& right) noexcept {
        return left.firstUse <= right.lastUse && right.firstUse <= left.lastUse;
    }

    bool rangesOverlap(const AliasPlacement& left, const AliasPlacement& right) noexcept {
        return left.offset < right.offset + right.size &&
            right.offset < left.offset + left.size;
    }

} // namespace

std::span<const uint32_t> AliasPlan::aliasPredecessors(
    uint32_t logicalResourceIndex) const noexcept {
    if (logicalResourceIndex + 1u >= predecessorOffsets.size()) return {};
    const uint32_t begin = predecessorOffsets[logicalResourceIndex];
    const uint32_t end = predecessorOffsets[logicalResourceIndex + 1u];
    return std::span<const uint32_t>(predecessors).subspan(begin, end - begin);
}

AliasPlan planTransientAliasing(const CompiledGraph& graph,
    std::span<const TransientMemoryRequirement> requirements,
    const AliasingOptions& options) {
    const auto& resources = graph.resources();
    const uint32_t resourceCount = static_cast<uint32_t>(resources.size());
    if (requirements.size() != resources.size()) {
        throw std::invalid_argument(
            "Transient aliasing requirements must cover every logical resource");
    }
    if (!isPowerOfTwo(options.minimumAlignment)) {
        throw std::invalid_argument(
            "Transient aliasing minimum alignment must be a power of two");
    }

    AliasPlan plan;
    plan.placements.assign(resourceCount, AliasPlacement{});

    std::vector<uint32_t> eligible;
    eligible.reserve(resourceCount);
    std::vector<uint64_t> alignments(resourceCount, 1);
    for (uint32_t logical = 0; logical < resourceCount; ++logical) {
        if (resources[logical].aliasEligibility != AliasEligibility::Eligible) continue;
        const TransientMemoryRequirement& requirement = requirements[logical];
        if (requirement.size == 0 || requirement.typeMask == 0) {
            throw std::invalid_argument("Eligible transient resource '" +
                resources[logical].name + "' needs a nonzero size and memory-type mask");
        }
        if (!isPowerOfTwo(requirement.alignment)) {
            throw std::invalid_argument("Transient resource '" +
                resources[logical].name + "' alignment must be a power of two");
        }
        alignments[logical] = (std::max)(requirement.alignment, options.minimumAlignment);
        eligible.push_back(logical);
    }

    // Deterministic greedy order: size descending, then firstUse, then index.
    plan.placementOrder = eligible;
    std::sort(plan.placementOrder.begin(), plan.placementOrder.end(),
        [&](uint32_t left, uint32_t right) {
            const uint64_t leftSize = requirements[left].size;
            const uint64_t rightSize = requirements[right].size;
            if (leftSize != rightSize) return leftSize > rightSize;
            if (resources[left].firstUse != resources[right].firstUse)
                return resources[left].firstUse < resources[right].firstUse;
            return left < right;
        });

    // Placed resources per heap, in placement order.
    std::vector<std::vector<uint32_t>> heapMembers;
    std::vector<uint64_t> candidates;
    candidates.reserve(eligible.size() + 1);

    // The lowest feasible offset of `logical` in `heap`.
    const auto lowestOffset = [&](uint32_t logical, uint32_t heap) {
        const CompiledResource& resource = resources[logical];
        const uint64_t size = requirements[logical].size;
        const uint64_t alignment = alignments[logical];
        candidates.clear();
        candidates.push_back(0);
        for (const uint32_t member : heapMembers[heap]) {
            if (!interferes(resource, resources[member])) continue;
            const AliasPlacement& placed = plan.placements[member];
            candidates.push_back(alignUp(placed.offset + placed.size, alignment));
        }
        std::sort(candidates.begin(), candidates.end());
        for (const uint64_t candidate : candidates) {
            const AliasPlacement probe{ heap, candidate, size };
            bool overlaps = false;
            for (const uint32_t member : heapMembers[heap]) {
                if (interferes(resource, resources[member]) &&
                    rangesOverlap(probe, plan.placements[member])) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) return candidate;
        }
        // Unreachable: the largest candidate clears every interfering member.
        throw std::logic_error("Transient aliasing found no feasible offset");
    };

    for (const uint32_t logical : plan.placementOrder) {
        const TransientMemoryRequirement& requirement = requirements[logical];
        uint32_t selectedHeap = InvalidIndex;
        uint64_t selectedOffset = 0;
        uint64_t selectedGrowth = 0;
        for (uint32_t heap = 0; heap < plan.heaps.size(); ++heap) {
            if ((plan.heaps[heap].typeMask & requirement.typeMask) == 0) continue;
            const uint64_t offset = lowestOffset(logical, heap);
            const uint64_t end = offset + requirement.size;
            const uint64_t growth = end > plan.heaps[heap].size
                ? end - plan.heaps[heap].size : 0;
            if (selectedHeap == InvalidIndex || growth < selectedGrowth) {
                selectedHeap = heap;
                selectedOffset = offset;
                selectedGrowth = growth;
            }
        }
        if (selectedHeap == InvalidIndex) {
            selectedHeap = static_cast<uint32_t>(plan.heaps.size());
            plan.heaps.push_back({ 0, 1, requirement.typeMask });
            heapMembers.emplace_back();
            selectedOffset = 0;
        }
        AliasHeap& heap = plan.heaps[selectedHeap];
        heap.size = (std::max)(heap.size, selectedOffset + requirement.size);
        heap.alignment = (std::max)(heap.alignment, alignments[logical]);
        heap.typeMask &= requirement.typeMask;
        plan.placements[logical] = { selectedHeap, selectedOffset, requirement.size };
        heapMembers[selectedHeap].push_back(logical);
        plan.requestedBytes += requirement.size;
    }
    for (const AliasHeap& heap : plan.heaps) plan.committedBytes += heap.size;

    // Alias predecessors (CSR by logical index).
    plan.predecessorOffsets.assign(resourceCount + 1u, 0);
    std::vector<uint32_t> scratch;
    for (uint32_t logical = 0; logical < resourceCount; ++logical) {
        plan.predecessorOffsets[logical] = static_cast<uint32_t>(plan.predecessors.size());
        const AliasPlacement& placement = plan.placements[logical];
        if (!placement.placed()) continue;
        scratch.clear();
        for (const uint32_t member : heapMembers[placement.heap]) {
            if (member == logical) continue;
            if (resources[member].lastUse < resources[logical].firstUse &&
                rangesOverlap(plan.placements[member], placement)) {
                scratch.push_back(member);
            }
        }
        std::sort(scratch.begin(), scratch.end(), [&](uint32_t left, uint32_t right) {
            return resources[left].lastUse != resources[right].lastUse
                ? resources[left].lastUse < resources[right].lastUse
                : left < right;
        });
        plan.predecessors.insert(plan.predecessors.end(), scratch.begin(), scratch.end());
    }
    plan.predecessorOffsets[resourceCount] = static_cast<uint32_t>(plan.predecessors.size());

    // Peak live bytes over the compiled pass order.
    const uint32_t passCount = static_cast<uint32_t>(graph.passes().size());
    for (uint32_t pass = 0; pass < passCount; ++pass) {
        uint64_t live = 0;
        for (const uint32_t logical : eligible) {
            if (resources[logical].firstUse <= pass && pass <= resources[logical].lastUse)
                live += requirements[logical].size;
        }
        if (live > plan.peakLiveBytes) {
            plan.peakLiveBytes = live;
            plan.peakLivePass = pass;
        }
    }
    return plan;
}

} // namespace Iridium::RenderGraph
