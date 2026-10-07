// M7R R4b.3: the backend-neutral transient-memory aliasing planner
// (renderer/graph/RenderGraphAliasing.h) and the compiler's aliasing slot mode.
// Pure CPU: synthetic graphs plus the production topologies, which are only
// built through buildVulkanProductionRenderGraph (no device).

#include "renderer/graph/RenderGraph.h"
#include "renderer/graph/RenderGraphAliasing.h"
#include "renderer/vulkan/VulkanProductionRenderGraph.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;
    using namespace Iridium::RenderGraph;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    #define CHECK_MSG(condition, message) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " [" << message << "] (line " \
                    << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    ResourceDesc imageDesc(Format format = Format::Rgba16Float,
        ResourceLifetime lifetime = ResourceLifetime::Transient) {
        ResourceDesc desc{};
        desc.type = ResourceType::Image;
        desc.lifetime = lifetime;
        desc.image.format = format;
        desc.image.extent = { 1920, 1080, 1 };
        return desc;
    }

    template <typename Function>
    bool throws(Function&& function) {
        try { function(); }
        catch (const std::exception&) { return true; }
        return false;
    }

    const CompiledResource* findResource(const CompiledGraph& graph, std::string_view name) {
        for (const CompiledResource& resource : graph.resources())
            if (resource.name == name) return &resource;
        return nullptr;
    }

    // ---- synthetic graphs ------------------------------------------------------

    // A resource used over the inclusive pass interval [first, last]: cleared as
    // a colour attachment in `first`, sampled in `last` (when later).
    struct IntervalSpec {
        uint32_t first = 0;
        uint32_t last = 0;
    };

    // Passes are added in index order and every edge runs forward, so the
    // compiled order is the pass index order.
    CompiledGraph intervalGraph(std::span<const IntervalSpec> specs, uint32_t passCount,
        const CompileOptions& options = {}) {
        RenderGraphBuilder builder;
        std::vector<ResourceHandle> handles;
        for (size_t index = 0; index < specs.size(); ++index)
            handles.push_back(builder.createResource("r" + std::to_string(index), imageDesc()));
        std::vector<PassHandle> passes;
        for (uint32_t pass = 0; pass < passCount; ++pass)
            passes.push_back(builder.addPass("p" + std::to_string(pass)));
        for (uint32_t pass = 0; pass < passCount; ++pass) {
            for (size_t index = 0; index < specs.size(); ++index) {
                if (specs[index].first == pass)
                    handles[index] = builder.write(passes[pass], handles[index],
                        Access::ColorAttachment, LoadOp::Clear);
                else if (specs[index].last == pass)
                    builder.read(passes[pass], handles[index], Access::SampledRead);
            }
        }
        CompileResult result = builder.compile(options);
        if (!result.succeeded()) throw std::runtime_error("interval graph failed to compile");
        return std::move(*result.graph);
    }

    bool interferes(const CompiledResource& left, const CompiledResource& right) {
        return left.firstUse <= right.lastUse && right.firstUse <= left.lastUse;
    }

    bool overlaps(const AliasPlacement& left, const AliasPlacement& right) {
        return left.offset < right.offset + right.size &&
            right.offset < left.offset + left.size;
    }

    // Every structural property a plan must have, whatever the input.
    bool checkPlanInvariants(const CompiledGraph& graph,
        std::span<const TransientMemoryRequirement> requirements, const AliasPlan& plan,
        const AliasingOptions& options = {}) {
        const auto& resources = graph.resources();
        CHECK(plan.placements.size() == resources.size());
        CHECK(plan.predecessorOffsets.size() == resources.size() + 1);
        uint64_t requested = 0;
        uint64_t padding = 0;
        uint64_t commonAlignment = 0;
        bool uniformAlignment = true;
        uint32_t placedCount = 0;
        for (uint32_t logical = 0; logical < resources.size(); ++logical) {
            const AliasPlacement& placement = plan.placements[logical];
            const bool eligible = resources[logical].aliasEligibility == AliasEligibility::Eligible;
            CHECK_MSG(placement.placed() == eligible, resources[logical].name);
            if (!eligible) {
                CHECK(plan.aliasPredecessors(logical).empty());
                continue;
            }
            ++placedCount;
            requested += requirements[logical].size;
            CHECK(placement.heap < plan.heaps.size());
            const AliasHeap& heap = plan.heaps[placement.heap];
            CHECK(placement.size == requirements[logical].size);
            CHECK(placement.offset + placement.size <= heap.size);
            const uint64_t alignment = (std::max)(requirements[logical].alignment,
                options.minimumAlignment);
            CHECK_MSG(placement.offset % alignment == 0, resources[logical].name);
            padding += alignment - 1;
            if (commonAlignment == 0) commonAlignment = alignment;
            uniformAlignment = uniformAlignment && alignment == commonAlignment &&
                requirements[logical].size % alignment == 0;
            CHECK(heap.alignment % alignment == 0);
            CHECK(heap.typeMask != 0);
            CHECK((heap.typeMask & requirements[logical].typeMask) == heap.typeMask);
        }
        CHECK(plan.placementOrder.size() == placedCount);
        CHECK(plan.requestedBytes == requested);

        uint64_t committed = 0;
        for (const AliasHeap& heap : plan.heaps) committed += heap.size;
        CHECK(plan.committedBytes == committed);
        // Peak bound: never less than what one pass keeps live, and never more
        // than no aliasing. With one alignment that divides every size (as
        // real image requirements are) nothing pads; otherwise each placement
        // can pad by up to its alignment - 1.
        CHECK(plan.committedBytes >= plan.peakLiveBytes);
        if (uniformAlignment) CHECK(plan.committedBytes <= plan.requestedBytes);
        else CHECK(plan.committedBytes <= plan.requestedBytes + padding);

        for (uint32_t left = 0; left < resources.size(); ++left) {
            const AliasPlacement& a = plan.placements[left];
            if (!a.placed()) continue;
            for (uint32_t right = 0; right < resources.size(); ++right) {
                const AliasPlacement& b = plan.placements[right];
                if (right == left || !b.placed() || a.heap != b.heap) continue;
                const bool memory = overlaps(a, b);
                // No overlap among interfering resources.
                if (interferes(resources[left], resources[right]))
                    CHECK_MSG(!memory, resources[left].name << " / " << resources[right].name);
                // Predecessors: exactly the overlapping, earlier-ending ones.
                const auto predecessors = plan.aliasPredecessors(left);
                const bool listed = std::ranges::find(predecessors, right) != predecessors.end();
                const bool expected = memory &&
                    resources[right].lastUse < resources[left].firstUse;
                CHECK_MSG(listed == expected, resources[left].name << " <- " << resources[right].name);
            }
            const auto predecessors = plan.aliasPredecessors(left);
            CHECK(std::ranges::is_sorted(predecessors, [&](uint32_t x, uint32_t y) {
                return resources[x].lastUse != resources[y].lastUse
                    ? resources[x].lastUse < resources[y].lastUse : x < y;
            }));
        }
        return true;
    }

    bool samePlan(const AliasPlan& left, const AliasPlan& right) {
        if (left.heaps.size() != right.heaps.size()) return false;
        for (size_t index = 0; index < left.heaps.size(); ++index) {
            if (left.heaps[index].size != right.heaps[index].size ||
                left.heaps[index].alignment != right.heaps[index].alignment ||
                left.heaps[index].typeMask != right.heaps[index].typeMask) return false;
        }
        if (left.placements.size() != right.placements.size()) return false;
        for (size_t index = 0; index < left.placements.size(); ++index) {
            if (left.placements[index].heap != right.placements[index].heap ||
                left.placements[index].offset != right.placements[index].offset ||
                left.placements[index].size != right.placements[index].size) return false;
        }
        return left.placementOrder == right.placementOrder &&
            left.predecessorOffsets == right.predecessorOffsets &&
            left.predecessors == right.predecessors &&
            left.requestedBytes == right.requestedBytes &&
            left.committedBytes == right.committedBytes &&
            left.peakLiveBytes == right.peakLiveBytes &&
            left.peakLivePass == right.peakLivePass;
    }

    struct Lcg {
        uint64_t state;
        uint32_t next() {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<uint32_t>(state >> 33);
        }
    };

    // A pseudo-random interval graph and requirements (mixed sizes, alignments
    // and memory-type masks).
    struct RandomCase {
        CompiledGraph graph;
        std::vector<TransientMemoryRequirement> requirements;
    };
    RandomCase randomCase(uint64_t seed, uint32_t resourceCount, uint32_t passCount) {
        Lcg random{ seed };
        std::vector<IntervalSpec> specs;
        for (uint32_t index = 0; index < resourceCount; ++index) {
            const uint32_t first = random.next() % passCount;
            const uint32_t length = random.next() % (passCount / 3 + 1);
            specs.push_back({ first, (std::min)(passCount - 1, first + length) });
        }
        RandomCase result{ intervalGraph(specs, passCount), {} };
        constexpr uint64_t Alignments[] = { 256, 4096, 65536, 1u << 20 };
        constexpr uint32_t Masks[] = { 0b0011u, 0b0110u, 0b0100u, 0b0001u, 0b1111u };
        for (uint32_t index = 0; index < resourceCount; ++index) {
            // 64 KiB .. 4 MiB, a third of them not a multiple of any alignment.
            const uint64_t size = static_cast<uint64_t>(1 + random.next() % 64) * 65536 +
                (random.next() % 3 == 0 ? 1 + random.next() % 977 : 0);
            result.requirements.push_back({ size, Alignments[random.next() % 4],
                Masks[random.next() % 5] });
        }
        return result;
    }

    // ---- tests -----------------------------------------------------------------

    bool testEligibilityRules() {
        RenderGraphBuilder builder;
        ResourceHandle clear = builder.createResource("clear", imageDesc());
        ResourceHandle dontCare = builder.createResource("attachment-dontcare", imageDesc());
        ResourceHandle attachmentWhole = builder.createResource("attachment-whole", imageDesc());
        ResourceHandle storage = builder.createResource("storage", imageDesc());
        ResourceHandle storageWhole = builder.createResource("storage-whole", imageDesc());
        ResourceHandle readWriteWhole = builder.createResource("storage-rw-whole", imageDesc());
        ResourceHandle transferWhole = builder.createResource("transfer-whole", imageDesc());
        ResourceHandle lateWhole = builder.createResource("late-whole", imageDesc());
        ResourceHandle persistent = builder.createResource("persistent",
            imageDesc(Format::Rgba16Float, ResourceLifetime::Persistent));
        ResourceDesc importedDesc = imageDesc(Format::Rgba16Float, ResourceLifetime::External);
        importedDesc.imported = true;
        importedDesc.initialAccess = Access::SampledRead;
        const ResourceHandle imported = builder.createResource("imported", importedDesc);
        ResourceHandle exported = builder.createResource("exported", imageDesc());
        const auto history = builder.createHistory("history", imageDesc());
        ResourceHandle historyCurrent = history.current;
        ResourceDesc bufferDesc{};
        bufferDesc.type = ResourceType::Buffer;
        bufferDesc.buffer.size = 4096;
        bufferDesc.buffer.alignment = 16;
        ResourceHandle buffer = builder.createResource("buffer", bufferDesc);
        (void)builder.createResource("unused", imageDesc());

        const PassHandle first = builder.addPass("first");
        const PassHandle second = builder.addPass("second", QueueClass::Compute);
        const PassHandle consume = builder.addPass("consume");

        clear = builder.write(first, clear, Access::ColorAttachment, LoadOp::Clear);
        dontCare = builder.write(first, dontCare, Access::ColorAttachment, LoadOp::DontCare);
        attachmentWhole = builder.write(first, attachmentWhole, Access::ColorAttachment,
            LoadOp::DontCare);
        builder.declareWholeResourceWrite(attachmentWhole);
        storage = builder.write(second, storage, Access::StorageWrite);
        storageWhole = builder.write(second, storageWhole, Access::StorageWrite);
        builder.declareWholeResourceWrite(storageWhole);
        readWriteWhole = builder.write(second, readWriteWhole, Access::StorageReadWrite);
        builder.declareWholeResourceWrite(readWriteWhole);
        transferWhole = builder.write(first, transferWhole, Access::TransferDestination);
        builder.declareWholeResourceWrite(transferWhole);
        // A whole-resource declaration on a later write does not cover the first.
        lateWhole = builder.write(first, lateWhole, Access::StorageWrite);
        lateWhole = builder.write(second, lateWhole, Access::StorageWrite);
        builder.declareWholeResourceWrite(lateWhole);
        persistent = builder.write(first, persistent, Access::ColorAttachment, LoadOp::Clear);
        exported = builder.write(first, exported, Access::ColorAttachment, LoadOp::Clear);
        builder.exportResource(exported, Access::SampledRead);
        historyCurrent = builder.write(first, historyCurrent, Access::ColorAttachment,
            LoadOp::Clear);
        buffer = builder.write(second, buffer, Access::StorageWrite);
        builder.declareWholeResourceWrite(buffer);
        for (const ResourceHandle handle : { clear, dontCare, attachmentWhole, storage,
                storageWhole, readWriteWhole, transferWhole, lateWhole, persistent, imported,
                history.previous, historyCurrent, buffer })
            builder.read(consume, handle, Access::SampledRead);

        // The declaration needs a written, non-loading version.
        CHECK(throws([&] { builder.declareWholeResourceWrite(imported); }));
        {
            RenderGraphBuilder other;
            ResourceHandle target = other.createResource("target", imageDesc());
            const PassHandle clearPass = other.addPass("clear");
            const PassHandle loadPass = other.addPass("load");
            target = other.write(clearPass, target, Access::ColorAttachment, LoadOp::Clear);
            const ResourceHandle loaded = other.write(loadPass, target,
                Access::ColorAttachment, LoadOp::Load);
            CHECK(throws([&] { other.declareWholeResourceWrite(loaded); }));
            CHECK(!throws([&] { other.declareWholeResourceWrite(target); }));
        }

        const CompileResult result = builder.compile();
        for (const auto& diagnostic : result.diagnostics) std::cerr << "  " << diagnostic.message << '\n';
        CHECK(result.succeeded());
        const CompiledGraph& graph = *result.graph;
        const auto expect = [&](std::string_view name, AliasEligibility eligibility) {
            const CompiledResource* resource = findResource(graph, name);
            if (resource == nullptr || resource->aliasEligibility != eligibility) {
                std::cerr << "  " << name << ": "
                    << (resource ? aliasEligibilityName(resource->aliasEligibility) : "missing")
                    << " != " << aliasEligibilityName(eligibility) << '\n';
                return false;
            }
            return true;
        };
        CHECK(expect("clear", AliasEligibility::Eligible));
        CHECK(expect("attachment-dontcare", AliasEligibility::FirstUseNotDiscard));
        CHECK(expect("attachment-whole", AliasEligibility::Eligible));
        CHECK(expect("storage", AliasEligibility::FirstUseNotDiscard));
        CHECK(expect("storage-whole", AliasEligibility::Eligible));
        CHECK(expect("storage-rw-whole", AliasEligibility::Eligible));
        CHECK(expect("transfer-whole", AliasEligibility::Eligible));
        CHECK(expect("late-whole", AliasEligibility::FirstUseNotDiscard));
        CHECK(expect("persistent", AliasEligibility::NotTransient));
        CHECK(expect("imported", AliasEligibility::Imported));
        CHECK(expect("exported", AliasEligibility::Exported));
        CHECK(expect("history.previous", AliasEligibility::History));
        CHECK(expect("history.current", AliasEligibility::History));
        CHECK(expect("buffer", AliasEligibility::Buffer));
        CHECK(expect("unused", AliasEligibility::Unused));

        // Only eligible resources are placed; the others' entries are ignored
        // (zero requirements are fine for them).
        std::vector<TransientMemoryRequirement> requirements(graph.resources().size());
        for (const CompiledResource& resource : graph.resources())
            if (resource.aliasEligibility == AliasEligibility::Eligible)
                requirements[resource.logicalResourceIndex] = { 1 << 20, 4096, 1 };
        const AliasPlan plan = planTransientAliasing(graph, requirements);
        CHECK(plan.placementOrder.size() == 5);
        CHECK(checkPlanInvariants(graph, requirements, plan));
        return true;
    }

    bool testDeclarationHashing() {
        const auto build = [](bool declare) {
            RenderGraphBuilder builder;
            ResourceHandle target = builder.createResource("target", imageDesc());
            const PassHandle write = builder.addPass("write", QueueClass::Compute);
            const PassHandle read = builder.addPass("read");
            target = builder.write(write, target, Access::StorageWrite);
            if (declare) builder.declareWholeResourceWrite(target);
            builder.read(read, target, Access::SampledRead);
            return builder.compile();
        };
        const CompileResult plain = build(false);
        const CompileResult declared = build(true);
        CHECK(plain.succeeded() && declared.succeeded());
        CHECK(plain.graph->topologyHash() != declared.graph->topologyHash());
        CHECK(plain.graph->resources()[0].aliasEligibility == AliasEligibility::FirstUseNotDiscard);
        CHECK(declared.graph->resources()[0].aliasEligibility == AliasEligibility::Eligible);
        CHECK(build(true).graph->topologyHash() == declared.graph->topologyHash());

        // R4b.5 exclusion: overrides a clearing first use, hashed only when
        // declared, any version names the resource.
        const auto excluded = [](bool exclude) {
            RenderGraphBuilder builder;
            ResourceHandle first = builder.createResource("target", imageDesc());
            const PassHandle write = builder.addPass("write");
            const PassHandle read = builder.addPass("read");
            const ResourceHandle written = builder.write(write, first, Access::ColorAttachment,
                LoadOp::Clear);
            builder.read(read, written, Access::SampledRead);
            if (exclude) builder.excludeFromAliasing(first);
            return builder.compile(CompileOptions{ .transientAliasing = true });
        };
        const CompileResult kept = excluded(false);
        const CompileResult dropped = excluded(true);
        CHECK(kept.succeeded() && dropped.succeeded());
        CHECK(kept.graph->resources()[0].aliasEligibility == AliasEligibility::Eligible);
        CHECK(dropped.graph->resources()[0].aliasEligibility == AliasEligibility::Excluded);
        CHECK(std::string_view(aliasEligibilityName(AliasEligibility::Excluded)) == "excluded");
        CHECK(!dropped.graph->physicalSlots()[dropped.graph->resources()[0].physicalSlot].aliased);
        CHECK(kept.graph->topologyHash() != dropped.graph->topologyHash());
        CHECK(excluded(true).graph->topologyHash() == dropped.graph->topologyHash());
        return true;
    }

    bool testAliasingSlotMode() {
        // a and b share a descriptor and never overlap, so exact-descriptor reuse
        // puts them in one slot; c and d (storage writes, not eligible) too.
        const auto build = [](const CompileOptions& options) {
            RenderGraphBuilder builder;
            ResourceHandle a = builder.createResource("a", imageDesc());
            ResourceHandle b = builder.createResource("b", imageDesc());
            ResourceHandle c = builder.createResource("c", imageDesc(Format::R32Float));
            ResourceHandle d = builder.createResource("d", imageDesc(Format::R32Float));
            ResourceHandle persistent = builder.createResource("persistent",
                imageDesc(Format::Rgba16Float, ResourceLifetime::Persistent));
            const PassHandle p0 = builder.addPass("p0");
            const PassHandle p1 = builder.addPass("p1");
            const PassHandle p2 = builder.addPass("p2");
            const PassHandle p3 = builder.addPass("p3");
            a = builder.write(p0, a, Access::ColorAttachment, LoadOp::Clear);
            c = builder.write(p0, c, Access::StorageWrite);
            persistent = builder.write(p0, persistent, Access::ColorAttachment, LoadOp::Clear);
            builder.read(p1, a, Access::SampledRead);
            builder.read(p1, c, Access::SampledRead);
            b = builder.write(p2, b, Access::ColorAttachment, LoadOp::Clear);
            d = builder.write(p2, d, Access::StorageWrite);
            builder.read(p3, b, Access::SampledRead);
            builder.read(p3, d, Access::SampledRead);
            builder.read(p3, persistent, Access::SampledRead);
            return builder.compile(options);
        };
        const CompileResult implicit = build(CompileOptions{});
        const CompileResult off = build(CompileOptions{ .transientAliasing = false });
        const CompileResult on = build(CompileOptions{ .transientAliasing = true });
        CHECK(implicit.succeeded() && off.succeeded() && on.succeeded());
        const CompiledGraph& offGraph = *off.graph;
        const CompiledGraph& onGraph = *on.graph;
        CHECK(implicit.graph->topologyHash() == offGraph.topologyHash());
        CHECK(onGraph.topologyHash() != offGraph.topologyHash());

        const auto slotOf = [](const CompiledGraph& graph, std::string_view name) {
            return findResource(graph, name)->physicalSlot;
        };
        // Off: today's reuse.
        CHECK(offGraph.physicalSlots().size() == 3);
        CHECK(slotOf(offGraph, "a") == slotOf(offGraph, "b"));
        CHECK(slotOf(offGraph, "c") == slotOf(offGraph, "d"));
        for (const PhysicalResourceSlot& slot : offGraph.physicalSlots()) CHECK(!slot.aliased);
        // On: a and b get their own aliased slots; c/d reuse as before.
        CHECK(onGraph.physicalSlots().size() == 4);
        CHECK(slotOf(onGraph, "a") != slotOf(onGraph, "b"));
        CHECK(slotOf(onGraph, "c") == slotOf(onGraph, "d"));
        for (const PhysicalResourceSlot& slot : onGraph.physicalSlots()) {
            const bool eligibleMember = std::ranges::any_of(slot.logicalResources,
                [&](uint32_t logical) {
                    return onGraph.resources()[logical].aliasEligibility ==
                        AliasEligibility::Eligible;
                });
            CHECK(slot.aliased == eligibleMember);
            if (slot.aliased) {
                CHECK(slot.logicalResources.size() == 1);
                CHECK(!slot.transientReusable);
            }
        }
        // Eligibility does not depend on the option.
        for (size_t index = 0; index < onGraph.resources().size(); ++index)
            CHECK(onGraph.resources()[index].aliasEligibility ==
                offGraph.resources()[index].aliasEligibility);
        return true;
    }

    bool testExactPackingAndPredecessors() {
        // Equal sizes; a [0,1], b [1,2], c [2,3], d [3,4]: a/c and b/d can share.
        const IntervalSpec specs[] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } };
        const CompiledGraph graph = intervalGraph(specs, 5);
        const std::vector<TransientMemoryRequirement> requirements(4, { 1000, 256, 1 });
        const AliasPlan plan = planTransientAliasing(graph, requirements);
        CHECK(checkPlanInvariants(graph, requirements, plan));
        CHECK(plan.heaps.size() == 1);
        CHECK(plan.requestedBytes == 4000);
        CHECK(plan.peakLiveBytes == 2000);
        CHECK(plan.committedBytes == 2024);
        // Greedy: same size, so firstUse order a, b, c, d.
        CHECK((plan.placementOrder == std::vector<uint32_t>{ 0, 1, 2, 3 }));
        CHECK(plan.placements[0].offset == 0);
        CHECK(plan.placements[1].offset == 1024);  // aligned end of a
        CHECK(plan.placements[2].offset == 0);
        CHECK(plan.placements[3].offset == 1024);
        CHECK(plan.heaps[0].size == 2024);  // aligned offset 1024 + 1000
        CHECK(plan.aliasPredecessors(0).empty());
        CHECK(plan.aliasPredecessors(1).empty());
        CHECK((std::vector<uint32_t>(plan.aliasPredecessors(2).begin(),
            plan.aliasPredecessors(2).end()) == std::vector<uint32_t>{ 0 }));
        CHECK((std::vector<uint32_t>(plan.aliasPredecessors(3).begin(),
            plan.aliasPredecessors(3).end()) == std::vector<uint32_t>{ 1 }));
        return true;
    }

    bool testAlignment() {
        // Odd sizes and large alignments: every offset is aligned, and the
        // option's minimum applies to every placement.
        const IntervalSpec specs[] = { { 0, 2 }, { 0, 2 }, { 1, 3 }, { 3, 4 }, { 4, 5 } };
        const CompiledGraph graph = intervalGraph(specs, 6);
        const std::vector<TransientMemoryRequirement> requirements{
            { 1001, 1 << 16, 1 }, { 3, 512, 1 }, { 70001, 4096, 1 },
            { 10, 1 << 20, 1 }, { 999, 2, 1 } };
        const AliasPlan plan = planTransientAliasing(graph, requirements);
        CHECK(checkPlanInvariants(graph, requirements, plan));
        CHECK(plan.heaps.size() == 1 && plan.heaps[0].alignment == (1u << 20));
        const AliasingOptions coarse{ .minimumAlignment = 1u << 16 };
        const AliasPlan coarsePlan = planTransientAliasing(graph, requirements, coarse);
        CHECK(checkPlanInvariants(graph, requirements, coarsePlan, coarse));
        for (const AliasPlacement& placement : coarsePlan.placements)
            CHECK(placement.offset % (1u << 16) == 0);
        const auto rejects = [&](std::vector<TransientMemoryRequirement> bad,
            AliasingOptions options = {}) {
            return throws([&] { (void)planTransientAliasing(graph, bad, options); });
        };
        auto badAlignment = requirements;
        badAlignment[2].alignment = 3000;
        CHECK(rejects(badAlignment));
        CHECK(rejects(requirements, AliasingOptions{ .minimumAlignment = 0 }));
        auto zeroSize = requirements;
        zeroSize[1].size = 0;
        CHECK(rejects(zeroSize));
        auto zeroMask = requirements;
        zeroMask[1].typeMask = 0;
        CHECK(rejects(zeroMask));
        CHECK(rejects(std::vector<TransientMemoryRequirement>(requirements.begin(),
            requirements.end() - 1)));
        return true;
    }

    bool testTypeMaskGrouping() {
        // a [0,1] and b [2,3] could share memory, but their memory types are
        // disjoint; c [2,3] intersects a's mask and takes a's dead range.
        const IntervalSpec specs[] = { { 0, 1 }, { 2, 3 }, { 2, 3 } };
        const CompiledGraph graph = intervalGraph(specs, 4);
        const std::vector<TransientMemoryRequirement> requirements{
            { 4096, 256, 0b0011 }, { 4096, 256, 0b0100 }, { 2048, 256, 0b0110 } };
        const AliasPlan plan = planTransientAliasing(graph, requirements);
        CHECK(checkPlanInvariants(graph, requirements, plan));
        CHECK(plan.heaps.size() == 2);
        CHECK(plan.placements[0].heap != plan.placements[1].heap);
        CHECK(plan.placements[2].heap == plan.placements[0].heap);
        CHECK(plan.placements[2].offset == 0);
        CHECK(plan.heaps[plan.placements[0].heap].typeMask == 0b0010);
        CHECK(plan.heaps[plan.placements[1].heap].typeMask == 0b0100);
        CHECK(plan.committedBytes == 8192);
        return true;
    }

    bool testRandomGraphsAndDeterminism() {
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            const RandomCase input = randomCase(seed * 7919, 48, 30);
            const AliasPlan plan = planTransientAliasing(input.graph, input.requirements);
            CHECK_MSG(checkPlanInvariants(input.graph, input.requirements, plan), "seed " << seed);
            CHECK(plan.committedBytes < plan.requestedBytes);
            // Determinism: same input, same plan; a recompiled graph too.
            CHECK(samePlan(plan, planTransientAliasing(input.graph, input.requirements)));
            const RandomCase again = randomCase(seed * 7919, 48, 30);
            CHECK(samePlan(plan, planTransientAliasing(again.graph, again.requirements)));
            const AliasingOptions coarse{ .minimumAlignment = 1u << 16 };
            CHECK(checkPlanInvariants(input.graph, input.requirements,
                planTransientAliasing(input.graph, input.requirements, coarse), coarse));
        }
        return true;
    }

    // ---- production topologies -------------------------------------------------

    struct NamedTopology {
        std::string name;
        CompiledGraph graph;
    };

    // Every production topology the backend can build (the R3b equivalence set).
    std::vector<NamedTopology> productionTopologies() {
        constexpr VkExtent2D Scene{ 1920, 1080 };
        constexpr VkExtent2D Ordinary2{ 960, 528 };
        constexpr VkExtent2D Hero4{ 1920, 528 };
        constexpr VkExtent2D Cinematic8{ 1920, 1072 };
        const auto build = [&](bool hdr10, VulkanLayeredGraphConfig layered,
            VulkanProductionGraphFeatures features, bool pyramids = true) {
            return buildVulkanProductionRenderGraph(Scene, Scene,
                hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_SRGB,
                hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_SRGB,
                hdr10, GBufferLayout::CanonicalReference, {}, 4096, 8192, pyramids,
                layered, features);
        };
        std::vector<NamedTopology> result;
        result.push_back({ "base SDR", build(false, {}, {}) });
        result.push_back({ "base SDR, no pyramids, no telemetry",
            build(false, {}, { .clusterTelemetryReadback = false }, false) });
        result.push_back({ "HDR10", build(true, {}, {}) });
        result.push_back({ "VSM", build(false, {}, { .virtualShadowWorkingSetBytes = 8'192 }) });
        result.push_back({ "Hi-Z", build(false, {}, { .depthPyramid = true }) });
        result.push_back({ "Ordinary2", build(false, { Ordinary2, {}, {} }, {}) });
        result.push_back({ "Hero4", build(false, { {}, Hero4, {} }, {}) });
        result.push_back({ "Cinematic8", build(false, { {}, {}, Cinematic8 }, {}) });
        result.push_back({ "OIT", build(false, { {}, {}, {}, true }, {}) });
        result.push_back({ "all features", build(true,
            { Ordinary2, Hero4, Cinematic8, true },
            { .depthPyramid = true, .virtualShadowWorkingSetBytes = 8'192 }) });
        result.push_back({ "all features, no hooks", build(false,
            { Ordinary2, Hero4, Cinematic8, true },
            { .depthPyramid = true, .virtualShadowWorkingSetBytes = 8'192,
              .hooks = VulkanGraphHooks::none() }) });
        return result;
    }

    void hashBytes(uint64_t& hash, const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    }
    template <typename T> void hashValue(uint64_t& hash, const T& value) {
        hashBytes(hash, &value, sizeof(value));
    }

    // Digest of the per-frame physical slots and every resource's slot.
    uint64_t slotDigest(const CompiledGraph& graph) {
        uint64_t hash = 14695981039346656037ull;
        hashValue(hash, graph.physicalSlots().size());
        for (const PhysicalResourceSlot& slot : graph.physicalSlots()) {
            hashValue(hash, slot.slotIndex);
            hashValue(hash, slot.type);
            hashValue(hash, slot.image.format);
            hashValue(hash, slot.image.extent.width);
            hashValue(hash, slot.image.extent.height);
            hashValue(hash, slot.image.extent.depth);
            hashValue(hash, slot.image.mipLevels);
            hashValue(hash, slot.image.arrayLayers);
            hashValue(hash, slot.image.samples);
            hashValue(hash, slot.buffer.size);
            hashValue(hash, slot.buffer.alignment);
            hashValue(hash, slot.usages);
            hashValue(hash, slot.lastUse);
            const uint8_t reusable = slot.transientReusable ? 1 : 0;
            hashValue(hash, reusable);
            hashValue(hash, slot.logicalResources.size());
            for (const uint32_t logical : slot.logicalResources) {
                hashValue(hash, logical);
                const std::string& name = graph.resources()[logical].name;
                hashBytes(hash, name.data(), name.size());
            }
        }
        for (const CompiledResource& resource : graph.resources())
            hashValue(hash, resource.physicalSlot);
        return hash;
    }

    // Generated by this digest from the 6f8b282 compiler (before R4b.3).
    struct GoldenSlots {
        const char* name;
        size_t slotCount;
        uint64_t digest;
    };
    // M9.1 (2026-10-05): regenerated; every topology gains the gbuffer.velocity
    // slot (alias-eligible, cleared in gbuffer, read by output-transform).
    // M7.10.2 (2026-10-07): regenerated; every topology gains the
    // lighting.cluster.light-bounds slot (a 2 MiB buffer written by
    // lighting.cluster.count and read by lighting.cluster.fill).
    constexpr GoldenSlots R4b3Golden[] = {
        { "base SDR", 21, 0xa3329f3a6cde3761ull },
        { "base SDR, no pyramids, no telemetry", 19, 0xaff8a3969b14d12bull },
        { "HDR10", 20, 0xf9d399e22e35f3f8ull },
        { "VSM", 21, 0x1459ed6d6c9f1997ull },
        { "Hi-Z", 21, 0xee1e44a0514ca26ull },
        { "Ordinary2", 26, 0xcf1489ab537967caull },
        { "Hero4", 31, 0x7480ceba86c5d7d4ull },
        { "Cinematic8", 41, 0x2f189c1be9670fcdull },
        { "OIT", 22, 0x9102b7387d0e9e0full },
        { "all features", 56, 0x56d47f1c2f3ce72aull },
        { "all features, no hooks", 55, 0xcbf77b0eeae9f429ull },
    };

    // Rebuilds a compiled graph through the builder in compiled order. The
    // compiled order is a topological order and the compiler emits the lowest
    // ready pass first, so the replay keeps the order and every lifetime.
    // `wholeResourceFirstWrites` names resources whose first write is declared
    // whole-resource (what a backend would declare in the production graph).
    // The source's own declarations are reproduced from its eligibility: an
    // eligible resource whose first write does not clear was declared
    // whole-resource, an Excluded one was excluded (R4b.5).
    CompileResult replay(const CompiledGraph& source,
        std::span<const std::string_view> wholeResourceFirstWrites,
        const CompileOptions& options) {
        RenderGraphBuilder builder;
        const auto& resources = source.resources();
        std::vector<ResourceHandle> handles(resources.size());
        for (const CompiledResource& resource : resources) {
            if (resource.historyRole == HistoryRole::Current) continue;
            if (resource.historyRole == HistoryRole::Previous) {
                const CompiledHistoryPair& pair = source.historyPairs()[resource.historyPair];
                const auto created = builder.createHistory(pair.name, resource.desc);
                handles[pair.previousLogical] = created.previous;
                handles[pair.currentLogical] = created.current;
                continue;
            }
            handles[resource.logicalResourceIndex] =
                builder.createResource(resource.name, resource.desc);
            if (resource.aliasEligibility == AliasEligibility::Excluded)
                builder.excludeFromAliasing(handles[resource.logicalResourceIndex]);
        }
        std::vector<bool> used(resources.size(), false);
        for (const CompiledPass& pass : source.passes()) {
            const PassHandle handle = builder.addPass(pass.name, pass.queue);
            for (uint32_t index = 0; index < pass.usageCount; ++index) {
                const CompiledUsage& usage = source.usages()[pass.firstUsage + index];
                const uint32_t logical = usage.logicalResourceIndex;
                if (usage.write) {
                    handles[logical] = builder.write(handle, handles[logical], usage.access,
                        usage.loadOp, usage.storeOp, usage.clearValue);
                    const bool declaredInSource = resources[logical].aliasEligibility ==
                        AliasEligibility::Eligible && usage.loadOp != LoadOp::Clear;
                    if (!used[logical] && (declaredInSource ||
                            std::ranges::find(wholeResourceFirstWrites,
                                resources[logical].name) != wholeResourceFirstWrites.end()))
                        builder.declareWholeResourceWrite(handles[logical]);
                }
                else {
                    builder.read(handle, handles[logical], usage.access);
                }
                used[logical] = true;
            }
        }
        for (const CompiledResource& resource : resources)
            if (resource.exported)
                builder.exportResource(handles[resource.logicalResourceIndex],
                    resource.finalAccess);
        return builder.compile(options);
    }

    uint64_t bytesPerPixel(Format format) {
        switch (format) {
        case Format::Rgba16Float: return 8;
        case Format::R16Float:
        case Format::R16Uint: return 2;
        case Format::Undefined: return 0;
        default: return 4;  // RGBA8, BGRA8, RGB10A2, RG16, R11G11B10, R32*, D32
        }
    }

    // The R4 design's size model: bytes per pixel x pixels x layers, a 4/3
    // factor for mip pyramids, sizes rounded to the 64 KiB alignment (as
    // Vulkan image requirements are), one memory type.
    std::vector<TransientMemoryRequirement> estimateRequirements(const CompiledGraph& graph) {
        constexpr uint64_t Alignment = 1u << 16;
        std::vector<TransientMemoryRequirement> requirements;
        for (const CompiledResource& resource : graph.resources()) {
            TransientMemoryRequirement requirement{ 0, Alignment, 1 };
            if (resource.desc.type == ResourceType::Image) {
                const ImageDesc& image = resource.desc.image;
                uint64_t bytes = uint64_t{ image.extent.width } * image.extent.height *
                    image.extent.depth * image.arrayLayers * image.samples *
                    bytesPerPixel(image.format);
                if (image.mipLevels > 1) bytes = bytes * 4 / 3;
                requirement.size = (bytes + Alignment - 1) / Alignment * Alignment;
            }
            else {
                requirement.size = resource.desc.buffer.size;
            }
            requirements.push_back(requirement);
        }
        return requirements;
    }

    bool testAliasingOffReproducesGoldenSlots() {
        const std::vector<NamedTopology> topologies = productionTopologies();
        CHECK(topologies.size() == std::size(R4b3Golden));
        // On a mismatch, print every topology's actual values (regenerating
        // the golden needs an explanation in the milestone log).
        bool mismatch = false;
        for (size_t index = 0; index < topologies.size(); ++index)
            mismatch = mismatch ||
                topologies[index].graph.physicalSlots().size() != R4b3Golden[index].slotCount ||
                slotDigest(topologies[index].graph) != R4b3Golden[index].digest;
        if (mismatch)
            for (const NamedTopology& topology : topologies)
                std::cerr << "    { \"" << topology.name << "\", "
                    << topology.graph.physicalSlots().size() << ", 0x" << std::hex
                    << slotDigest(topology.graph) << std::dec << "ull },\n";
        for (size_t index = 0; index < topologies.size(); ++index) {
            const NamedTopology& topology = topologies[index];
            CHECK_MSG(topology.name == R4b3Golden[index].name, topology.name);
            CHECK_MSG(topology.graph.physicalSlots().size() == R4b3Golden[index].slotCount,
                topology.name << " slots " << topology.graph.physicalSlots().size());
            CHECK_MSG(slotDigest(topology.graph) == R4b3Golden[index].digest,
                topology.name << " digest 0x" << std::hex << slotDigest(topology.graph) << std::dec);
            for (const PhysicalResourceSlot& slot : topology.graph.physicalSlots())
                CHECK(!slot.aliased);

            // The replay helper reproduces the production compile exactly.
            const CompileResult off = replay(topology.graph, {}, CompileOptions{});
            CHECK_MSG(off.succeeded(), topology.name);
            CHECK_MSG(slotDigest(*off.graph) == R4b3Golden[index].digest, topology.name);
            for (size_t logical = 0; logical < off.graph->resources().size(); ++logical) {
                const CompiledResource& left = off.graph->resources()[logical];
                const CompiledResource& right = topology.graph.resources()[logical];
                CHECK(left.name == right.name && left.firstUse == right.firstUse &&
                    left.lastUse == right.lastUse &&
                    left.aliasEligibility == right.aliasEligibility);
            }
        }
        return true;
    }

    bool testAliasingOnProductionTopologies() {
        for (const NamedTopology& topology : productionTopologies()) {
            const CompileResult on = replay(topology.graph, {},
                CompileOptions{ .transientAliasing = true });
            CHECK_MSG(on.succeeded(), topology.name);
            const CompiledGraph& graph = *on.graph;
            CHECK(graph.topologyHash() != topology.graph.topologyHash());
            uint32_t eligible = 0;
            for (const CompiledResource& resource : graph.resources()) {
                CHECK(resource.aliasEligibility ==
                    topology.graph.resources()[resource.logicalResourceIndex].aliasEligibility);
                if (resource.aliasEligibility != AliasEligibility::Eligible) continue;
                ++eligible;
                CHECK(resource.desc.type == ResourceType::Image);
                const PhysicalResourceSlot& slot = graph.physicalSlots()[resource.physicalSlot];
                CHECK_MSG(slot.aliased && slot.logicalResources.size() == 1, resource.name);
            }
            CHECK_MSG(eligible > 0, topology.name);
            uint32_t aliasedSlots = 0;
            for (const PhysicalResourceSlot& slot : graph.physicalSlots()) {
                if (slot.aliased) { ++aliasedSlots; continue; }
                for (const uint32_t logical : slot.logicalResources)
                    CHECK(graph.resources()[logical].aliasEligibility !=
                        AliasEligibility::Eligible);
            }
            CHECK(aliasedSlots == eligible);

            // The plan is a pure function of eligibility and lifetimes, so it is
            // the same over the production (aliasing-off) compile.
            const auto requirements = estimateRequirements(graph);
            const AliasPlan plan = planTransientAliasing(graph, requirements);
            CHECK_MSG(checkPlanInvariants(graph, requirements, plan), topology.name);
            CHECK(samePlan(plan, planTransientAliasing(topology.graph, requirements)));
        }
        return true;
    }

    // Default SDR graph at 4K, CanonicalReference, refraction pyramids on: the
    // design's ~564 MB requested and ~398 MB peak live per frame slot.
    constexpr std::string_view RefractionPyramids[] = {
        "scene.refraction-color-pyramid", "depth.refraction-nearest-pyramid" };

    bool testReport4kDefaultSdr() {
        const CompiledGraph production = buildVulkanProductionRenderGraph(
            VkExtent2D{ 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB);
        const auto requirements = estimateRequirements(production);
        const auto megabytes = [](uint64_t bytes) { return static_cast<double>(bytes) / 1.0e6; };

        uint64_t transientImageBytes = 0;
        uint64_t ineligibleImageBytes = 0;
        for (const CompiledResource& resource : production.resources()) {
            if (resource.desc.type != ResourceType::Image ||
                resource.desc.lifetime != ResourceLifetime::Transient ||
                resource.firstUse == InvalidIndex) continue;
            transientImageBytes += requirements[resource.logicalResourceIndex].size;
            if (resource.aliasEligibility != AliasEligibility::Eligible)
                ineligibleImageBytes += requirements[resource.logicalResourceIndex].size;
        }

        const auto report = [&](const char* title, const CompiledGraph& graph,
            const AliasPlan& plan, uint64_t dedicated) {
            std::printf("  %s\n", title);
            for (const CompiledResource& resource : graph.resources()) {
                if (resource.desc.type != ResourceType::Image ||
                    resource.desc.lifetime != ResourceLifetime::Transient) continue;
                const AliasPlacement& placement = plan.placements[resource.logicalResourceIndex];
                std::printf("    %-36s %8.1f MB  passes [%2u,%2u]  %-22s",
                    resource.name.c_str(),
                    megabytes(requirements[resource.logicalResourceIndex].size),
                    resource.firstUse, resource.lastUse,
                    aliasEligibilityName(resource.aliasEligibility));
                if (placement.placed())
                    std::printf(" heap %u @ %8.1f MB", placement.heap, megabytes(placement.offset));
                std::printf("\n");
            }
            const uint64_t perSlot = plan.committedBytes + dedicated;
            std::printf("    transient images requested %.1f MB; aliased %.1f MB -> committed "
                "%.1f MB (peak live %.1f MB at '%s'); dedicated %.1f MB; per slot %.1f MB, "
                "saves %.1f MB per slot, %.1f MB over 2 slots (%.0f%%)\n",
                megabytes(transientImageBytes), megabytes(plan.requestedBytes),
                megabytes(plan.committedBytes), megabytes(plan.peakLiveBytes),
                plan.peakLivePass == InvalidIndex ? "-"
                    : graph.passes()[plan.peakLivePass].name.c_str(),
                megabytes(dedicated), megabytes(perSlot),
                megabytes(transientImageBytes - perSlot),
                megabytes(2 * (transientImageBytes - perSlot)),
                100.0 * static_cast<double>(transientImageBytes - perSlot) /
                    static_cast<double>(transientImageBytes));
        };

        // As declared since R4b.5: the pyramid build is declared whole-resource
        // (it writes every mip before reading it), so every transient image of
        // the default graph is eligible.
        const AliasPlan declared = planTransientAliasing(production, requirements);
        CHECK(checkPlanInvariants(production, requirements, declared));
        report("as declared (production graph since R4b.5)", production, declared,
            ineligibleImageBytes);
        for (const std::string_view name : RefractionPyramids)
            CHECK(findResource(production, name)->aliasEligibility ==
                AliasEligibility::Eligible);
        CHECK(ineligibleImageBytes == 0);

        // The aliasing compile (what --render-graph-aliasing on builds).
        const CompileResult projected = replay(production, {},
            CompileOptions{ .transientAliasing = true });
        CHECK(projected.succeeded());
        const AliasPlan full = planTransientAliasing(*projected.graph, requirements);
        CHECK(checkPlanInvariants(*projected.graph, requirements, full));
        uint64_t projectedDedicated = 0;
        for (const CompiledResource& resource : projected.graph->resources())
            if (resource.desc.type == ResourceType::Image &&
                resource.desc.lifetime == ResourceLifetime::Transient &&
                resource.firstUse != InvalidIndex &&
                resource.aliasEligibility != AliasEligibility::Eligible)
                projectedDedicated += requirements[resource.logicalResourceIndex].size;
        report("aliasing compile", *projected.graph, full, projectedDedicated);

        // The design's estimate: ~564 MB requested, ~398 MB peak (at lighting),
        // and the greedy plan reaches the peak. M9.1 adds the 4K RG16F velocity
        // target (33.2 MB, live from gbuffer through lighting): ~597.6 MB
        // requested, ~431.6 MB peak.
        CHECK(projectedDedicated == 0);
        CHECK(full.requestedBytes == transientImageBytes);
        CHECK(transientImageBytes > 597'000'000ull && transientImageBytes < 599'000'000ull);
        CHECK(full.peakLiveBytes > 431'000'000ull && full.peakLiveBytes < 433'000'000ull);
        CHECK(projected.graph->passes()[full.peakLivePass].name == "lighting");
        CHECK(full.committedBytes == full.peakLiveBytes);
        CHECK(declared.committedBytes == declared.peakLiveBytes);
        CHECK(declared.committedBytes == full.committedBytes);
        CHECK(declared.committedBytes + ineligibleImageBytes < transientImageBytes);
        return true;
    }

    // R4b.5 production declarations: whole-resource storage first writes
    // (refraction pyramids, deep-tier tile masks), the deep resolve's inputs
    // excluded only when both deep tiers share it, and the editor's depth
    // read in the UI pass.
    bool testProductionAliasingDeclarations() {
        constexpr VkExtent2D Scene{ 1920, 1080 };
        constexpr VkExtent2D Ordinary2{ 960, 528 };
        constexpr VkExtent2D Hero4{ 1920, 528 };
        constexpr VkExtent2D Cinematic8{ 1920, 1072 };
        const auto build = [&](VulkanLayeredGraphConfig layered,
            VulkanProductionGraphFeatures features) {
            return buildVulkanProductionRenderGraph(Scene, Scene,
                VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true, layered, features);
        };
        const auto eligibility = [](const CompiledGraph& graph, std::string_view name) {
            const CompiledResource* resource = findResource(graph, name);
            return resource == nullptr ? AliasEligibility::Unused : resource->aliasEligibility;
        };
        const CompiledGraph both = build({ Ordinary2, Hero4, Cinematic8, true }, {});
        for (const CompiledResource& resource : both.resources()) {
            if (resource.desc.type != ResourceType::Image ||
                resource.desc.lifetime != ResourceLifetime::Transient ||
                resource.firstUse == InvalidIndex) continue;
            const bool resolveInput =
                resource.name == "scene.layered.hero4.local-color" ||
                resource.name == "identity.layered.hero4.interface.0" ||
                resource.name == "scene.layered.cinematic8.local-color" ||
                resource.name == "identity.layered.cinematic8.interface.0";
            CHECK_MSG(resource.aliasEligibility == (resolveInput
                ? AliasEligibility::Excluded : AliasEligibility::Eligible), resource.name);
        }
        CHECK(eligibility(both, "termination.layered.cinematic8.interface.5") ==
            AliasEligibility::Eligible);
        CHECK(eligibility(both, "depth.refraction-nearest-pyramid") ==
            AliasEligibility::Eligible);
        for (const VulkanLayeredGraphConfig& single : {
                VulkanLayeredGraphConfig{ {}, Hero4, {} },
                VulkanLayeredGraphConfig{ {}, {}, Cinematic8 } }) {
            const CompiledGraph graph = build(single, {});
            for (const CompiledResource& resource : graph.resources())
                CHECK_MSG(resource.aliasEligibility != AliasEligibility::Excluded,
                    resource.name);
        }

        VulkanProductionGraphFeatures editor{};
        editor.hooks.editorDepthSample = true;
        const CompiledGraph plain = build({}, {});
        const CompiledGraph withEditor = build({}, editor);
        const CompiledResource* plainDepth = findResource(plain, "depth.opaque");
        const CompiledResource* editorDepth = findResource(withEditor, "depth.opaque");
        CHECK(plainDepth != nullptr && editorDepth != nullptr);
        CHECK(plain.passes()[plainDepth->lastUse].name == "output-transform");
        CHECK(withEditor.passes()[editorDepth->lastUse].name == "ui-present");
        CHECK(plain.topologyHash() != withEditor.topologyHash());
        // The read keeps depth's SampledRead state: no new transition.
        CHECK(plain.transitions().size() == withEditor.transitions().size());
        CHECK(!VulkanGraphHooks::none().editorDepthSample);
        CHECK(!VulkanGraphHooks{}.editorDepthSample);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        std::string_view name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Eligibility rules", testEligibilityRules },
        { "Whole-resource declaration hashing", testDeclarationHashing },
        { "Aliasing slot mode", testAliasingSlotMode },
        { "Exact packing and predecessors", testExactPackingAndPredecessors },
        { "Alignment", testAlignment },
        { "Type-mask grouping", testTypeMaskGrouping },
        { "Random graphs and determinism", testRandomGraphsAndDeterminism },
        { "Aliasing off reproduces golden slots", testAliasingOffReproducesGoldenSlots },
        { "Aliasing on, production topologies", testAliasingOnProductionTopologies },
        { "4K default SDR report", testReport4kDefaultSdr },
        { "Production aliasing declarations", testProductionAliasingDeclarations },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
                continue;
            }
            std::cout << "[FAIL] " << test.name << '\n';
        }
        catch (const std::exception& exception) {
            std::cout << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
        ++failures;
    }
    if (failures != 0) {
        std::cerr << failures << " render-graph aliasing test(s) failed\n";
        return 1;
    }
    std::cout << "All render-graph aliasing tests passed\n";
    return 0;
}
