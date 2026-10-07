#include "renderer/graph/RenderGraph.h"

#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <string_view>

namespace {

    using namespace Iridium::RenderGraph;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
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

    bool hasDiagnostic(const CompileResult& result, DiagnosticCode code) {
        for (const GraphDiagnostic& diagnostic : result.diagnostics) {
            if (diagnostic.code == code) {
                return true;
            }
        }
        return false;
    }

    template <typename Function>
    bool throwsBuildError(Function&& function) {
        try {
            function();
        }
        catch (const GraphBuildError&) {
            return true;
        }
        return false;
    }

    bool testStableDagAndCompiledUsages() {
        RenderGraphBuilder builder;
        ResourceHandle scene = builder.createResource("scene", imageDesc());
        const PassHandle produce = builder.addPass("produce");
        const PassHandle sampleA = builder.addPass("sample-a");
        const PassHandle sampleB = builder.addPass("sample-b");
        scene = builder.write(produce, scene, Access::ColorAttachment, LoadOp::Clear);
        builder.read(sampleA, scene, Access::SampledRead);
        builder.read(sampleB, scene, Access::SampledRead);

        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        CHECK(result.graph->passes().size() == 3);
        CHECK(result.graph->passes()[0].name == "produce");
        CHECK(result.graph->passes()[1].name == "sample-a");
        CHECK(result.graph->passes()[2].name == "sample-b");
        CHECK(result.graph->passes()[0].usageCount == 1);
        CHECK(result.graph->usages().size() == 3);
        CHECK(result.graph->usages()[0].write);
        CHECK(result.graph->usages()[0].loadOp == LoadOp::Clear);
        CHECK(!result.graph->transitions().empty());
        return true;
    }

    bool testCycleDiagnostic() {
        RenderGraphBuilder builder;
        const PassHandle first = builder.addPass("first");
        const PassHandle second = builder.addPass("second");
        builder.addDependency(first, second);
        builder.addDependency(second, first);
        const CompileResult result = builder.compile();
        CHECK(!result.succeeded());
        CHECK(hasDiagnostic(result, DiagnosticCode::Cycle));
        return true;
    }

    bool testReadBeforeWriteDiagnostic() {
        RenderGraphBuilder builder;
        const ResourceHandle scene = builder.createResource("scene", imageDesc());
        const PassHandle sample = builder.addPass("sample");
        builder.read(sample, scene, Access::SampledRead);
        const CompileResult result = builder.compile();
        CHECK(!result.succeeded());
        CHECK(hasDiagnostic(result, DiagnosticCode::ReadBeforeWrite));
        return true;
    }

    bool testImportedAndExportedStates() {
        RenderGraphBuilder builder;
        ResourceDesc desc = imageDesc(Format::Bgra8Srgb,
            ResourceLifetime::External);
        desc.imported = true;
        desc.initialAccess = Access::Present;
        const ResourceHandle swapchain = builder.createResource("swapchain", desc);
        const PassHandle copy = builder.addPass("copy-source");
        builder.read(copy, swapchain, Access::TransferSource);
        builder.exportResource(swapchain, Access::Present);

        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        CHECK(result.graph->resources()[0].exported);
        CHECK(result.graph->resources()[0].finalAccess == Access::Present);
        CHECK(result.graph->resources()[0].physicalSlot == InvalidIndex);
        CHECK(result.graph->transitions().size() == 2);
        CHECK(result.graph->transitions().front().before == Access::Present);
        CHECK(result.graph->transitions().back().after == Access::Present);
        return true;
    }

    bool testInvalidHistoryIsExplicit() {
        RenderGraphBuilder builder;
        ResourceDesc desc = imageDesc(Format::Rgba16Float,
            ResourceLifetime::History);
        const ResourceHandle history = builder.createResource("history", desc);
        const PassHandle temporal = builder.addPass("temporal");
        builder.read(temporal, history, Access::SampledRead);

        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        CHECK(result.graph->historyResources().size() == 1);
        HistoryValidityTracker validity;
        validity.resetForGraph(*result.graph);
        CHECK(validity.topologyHash() == result.graph->topologyHash());
        CHECK(!validity.isValid(0));
        validity.setValid(0, true);
        CHECK(validity.isValid(0));
        validity.invalidateAll();
        CHECK(!validity.isValid(0));
        return true;
    }

    // M7R R3b.10: createHistory declares a linked previous/current pair on two
    // non-reusable slots outside the per-frame pool.
    bool testHistoryPairDeclaration() {
        RenderGraphBuilder builder;
        const auto history = builder.createHistory("taa", imageDesc());
        CHECK(history.pair == 0);
        ResourceHandle scene = builder.createResource("scene", imageDesc());
        const PassHandle draw = builder.addPass("draw");
        const PassHandle resolve = builder.addPass("resolve");
        const PassHandle post = builder.addPass("post");
        scene = builder.write(draw, scene, Access::ColorAttachment, LoadOp::Clear);
        builder.read(resolve, scene, Access::SampledRead);
        builder.read(resolve, history.previous, Access::SampledRead);
        const ResourceHandle current = builder.write(resolve, history.current,
            Access::ColorAttachment, LoadOp::Clear);
        builder.read(post, current, Access::SampledRead);
        // `previous` is read-only.
        CHECK(throwsBuildError([&] {
            (void)builder.write(post, history.previous, Access::StorageWrite); }));

        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        const CompiledGraph& graph = *result.graph;
        CHECK(graph.historyPairs().size() == 1);
        const CompiledHistoryPair& pair = graph.historyPairs()[0];
        CHECK(pair.name == "taa");
        CHECK(graph.resources()[pair.previousLogical].name == "taa.previous");
        CHECK(graph.resources()[pair.currentLogical].name == "taa.current");
        CHECK(graph.resources()[pair.previousLogical].historyRole == HistoryRole::Previous);
        CHECK(graph.resources()[pair.currentLogical].historyPair == 0);
        CHECK(graph.resources()[pair.previousLogical].desc.lifetime ==
            ResourceLifetime::History);
        // Two history slots, never in the per-frame pool, never shared.
        CHECK(graph.historySlots().size() == 2);
        CHECK(pair.slots[0] == 0 && pair.slots[1] == 1);
        CHECK(graph.resources()[pair.previousLogical].physicalSlot == InvalidIndex);
        CHECK(graph.resources()[pair.currentLogical].physicalSlot == InvalidIndex);
        for (const PhysicalResourceSlot& slot : graph.physicalSlots()) {
            for (const uint32_t logical : slot.logicalResources) {
                CHECK(graph.resources()[logical].historyPair == InvalidIndex);
            }
        }
        for (const PhysicalResourceSlot& slot : graph.historySlots()) {
            CHECK(!slot.transientReusable);
            CHECK((slot.usages & usageBit(Access::ColorAttachment)) != 0);
            CHECK((slot.usages & usageBit(Access::SampledRead)) != 0);
            CHECK(slot.image == imageDesc().image);
        }
        CHECK(graph.historyResources().size() == 2);

        // `current` holds nothing until this frame's writer has run.
        RenderGraphBuilder early;
        const auto earlyHistory = early.createHistory("taa", imageDesc());
        early.read(early.addPass("too-early"), earlyHistory.current, Access::SampledRead);
        CHECK(hasDiagnostic(early.compile(), DiagnosticCode::ReadBeforeWrite));

        // History cannot be imported.
        ResourceDesc imported = imageDesc(Format::Rgba16Float, ResourceLifetime::External);
        imported.imported = true;
        imported.initialAccess = Access::SampledRead;
        CHECK(throwsBuildError([&] { (void)builder.createHistory("bad", imported); }));

        // Pair data is hashed; graphs without pairs keep their former hashes
        // (only pair members hash pair fields).
        RenderGraphBuilder single;
        const ResourceHandle lone = single.createResource("taa.previous",
            imageDesc(Format::Rgba16Float, ResourceLifetime::History));
        single.read(single.addPass("resolve"), lone, Access::SampledRead);
        CHECK(single.compile().graph->topologyHash() != graph.topologyHash());
        return true;
    }

    bool testHistoryValidityKeys() {
        const auto build = [](uint32_t width) {
            RenderGraphBuilder builder;
            ResourceDesc desc = imageDesc();
            desc.image.extent.width = width;
            const auto history = builder.createHistory("taa", desc);
            const PassHandle resolve = builder.addPass("resolve");
            builder.read(resolve, history.previous, Access::SampledRead);
            (void)builder.write(resolve, history.current, Access::ColorAttachment,
                LoadOp::Clear);
            return *builder.compile().graph;
        };
        const CompiledGraph graph = build(1920);
        HistoryValidityTracker tracker;
        tracker.resetForGraph(graph);
        CHECK(tracker.pairCount() == 1);
        const uint32_t previous = graph.historyPairs()[0].previousLogical;
        const auto frame = [&](ViewHistoryContext view, bool written) {
            tracker.beginFrame(view);
            const bool valid = tracker.pairValid(0);
            if (written) tracker.markWritten(0);
            tracker.endFrame();
            return valid;
        };
        CHECK(!frame({ 7, 0 }, true));        // first frame
        CHECK(tracker.pairKey(0).extent.width == 1920);
        CHECK(tracker.pairKey(0).format == Format::Rgba16Float);
        CHECK(tracker.pairKey(0).topologyHash == graph.topologyHash());
        CHECK(frame({ 7, 0 }, true));         // written last frame, same key
        CHECK(tracker.isValid(previous));     // pair members report pair validity
        CHECK(!frame({ 7, 1 }, true));        // reset revision
        CHECK(frame({ 7, 1 }, false));        // writer skipped this frame...
        CHECK(!frame({ 7, 1 }, true));        // ...so the next frame is invalid
        CHECK(frame({ 7, 1 }, true));
        CHECK(!frame({ 8, 1 }, true));        // view identity
        CHECK(frame({ 8, 1 }, true));
        tracker.invalidateAll();
        CHECK(!frame({ 8, 1 }, true));
        CHECK(frame({ 8, 1 }, true));
        tracker.setValid(previous, false);
        CHECK(!tracker.pairValid(0));
        tracker.resetForGraph(graph);         // rebuild
        CHECK(!frame({ 8, 1 }, true));
        const CompiledGraph resized = build(1280);
        CHECK(resized.topologyHash() != graph.topologyHash());
        tracker.resetForGraph(resized);       // resize
        CHECK(!frame({ 8, 1 }, true));
        CHECK(tracker.pairKey(0).extent.width == 1280);
        CHECK(frame({ 8, 1 }, true));
        CHECK(throwsBuildError([&] { tracker.markWritten(1); }));
        return true;
    }

    // M9 G2: per-view history sets and per-pair reset policy.
    bool testHistoryViewSetsAndResetPolicy() {
        RenderGraphBuilder builder;
        const auto taa = builder.createHistory("taa", imageDesc());
        ResourceDesc exposureDesc{};
        exposureDesc.type = ResourceType::Buffer;
        exposureDesc.buffer.size = 16;
        const auto exposure = builder.createHistory("exposure", exposureDesc,
            HistoryReset::SurviveCut);
        const PassHandle resolve = builder.addPass("resolve");
        builder.read(resolve, taa.previous, Access::SampledRead);
        (void)builder.write(resolve, taa.current, Access::ColorAttachment, LoadOp::Clear);
        builder.read(resolve, exposure.previous, Access::StorageRead);
        (void)builder.write(resolve, exposure.current, Access::StorageWrite);
        const CompiledGraph graph = *builder.compile().graph;
        CHECK(graph.historyPairs()[taa.pair].reset == HistoryReset::OnCut);
        CHECK(graph.historyPairs()[exposure.pair].reset == HistoryReset::SurviveCut);

        // The policy is topology: a graph differing only in policy rehashes.
        RenderGraphBuilder other;
        const auto taaOther = other.createHistory("taa", imageDesc());
        const auto exposureOther = other.createHistory("exposure", exposureDesc);
        const PassHandle resolveOther = other.addPass("resolve");
        other.read(resolveOther, taaOther.previous, Access::SampledRead);
        (void)other.write(resolveOther, taaOther.current, Access::ColorAttachment, LoadOp::Clear);
        other.read(resolveOther, exposureOther.previous, Access::StorageRead);
        (void)other.write(resolveOther, exposureOther.current, Access::StorageWrite);
        CHECK(other.compile().graph->topologyHash() != graph.topologyHash());

        HistoryValidityTracker tracker;
        tracker.resetForGraph(graph);
        struct Validity { bool taa; bool exposure; };
        const auto frame = [&](ViewHistoryContext view) {
            tracker.beginFrame(view);
            const Validity valid{ tracker.pairValid(taa.pair), tracker.pairValid(exposure.pair) };
            tracker.markWritten(taa.pair);
            tracker.markWritten(exposure.pair);
            tracker.endFrame();
            return valid;
        };
        const ViewHistoryContext scene{ 1, 0, 0 };
        const ViewHistoryContext preview{ 5, 1, 1 };
        Validity v = frame(scene);
        CHECK(!v.taa && !v.exposure);
        v = frame(scene);
        CHECK(v.taa && v.exposure);
        // Alternating views: each set is valid on its own next turn.
        v = frame(preview);
        CHECK(!v.taa && !v.exposure);              // the preview's first turn
        CHECK(tracker.activeSet() == 1);
        v = frame(scene);
        CHECK(v.taa && v.exposure);                // the scene set survived the preview
        v = frame(preview);
        CHECK(v.taa && v.exposure);
        v = frame(scene);
        CHECK(v.taa && v.exposure);
        // A cut invalidates OnCut pairs only.
        v = frame({ 1, 1, 0 });
        CHECK(!v.taa && v.exposure);
        v = frame({ 1, 1, 0 });
        CHECK(v.taa && v.exposure);
        // A new preview session (identity) invalidates set 1 only.
        v = frame({ 6, 1, 1 });
        CHECK(!v.taa && !v.exposure);
        v = frame({ 1, 1, 0 });
        CHECK(v.taa && v.exposure);
        // A writer skipped on a view's turn invalidates that view's next turn.
        tracker.beginFrame({ 6, 1, 1 });
        tracker.endFrame();
        v = frame({ 6, 1, 1 });
        CHECK(!v.taa && !v.exposure);
        // Identity 0 is "no view": never valid.
        frame({ 0, 0, 0 });
        v = frame({ 0, 0, 0 });
        CHECK(!v.taa && !v.exposure);
        CHECK(throwsBuildError([&] { tracker.beginFrame({ 1, 0, HistoryViewSetCount }); }));
        return true;
    }

    bool testDiscardedContentsAndStaleExportAreRejected() {
        {
            RenderGraphBuilder builder;
            ResourceHandle scene = builder.createResource("scene", imageDesc());
            const PassHandle produce = builder.addPass("produce");
            const PassHandle sample = builder.addPass("sample");
            scene = builder.write(produce, scene, Access::ColorAttachment,
                LoadOp::Clear, StoreOp::DontCare);
            builder.read(sample, scene, Access::SampledRead);
            const CompileResult result = builder.compile();
            CHECK(!result.succeeded());
            CHECK(hasDiagnostic(result, DiagnosticCode::InvalidUsage));
        }
        {
            RenderGraphBuilder builder;
            ResourceHandle scene = builder.createResource("scene", imageDesc());
            const PassHandle first = builder.addPass("first");
            const PassHandle second = builder.addPass("second");
            scene = builder.write(first, scene, Access::ColorAttachment,
                LoadOp::Clear);
            builder.exportResource(scene, Access::SampledRead);
            scene = builder.write(second, scene, Access::ColorAttachment,
                LoadOp::Load);
            const CompileResult result = builder.compile();
            CHECK(!result.succeeded());
            CHECK(hasDiagnostic(result, DiagnosticCode::InvalidExport));
        }
        {
            RenderGraphBuilder builder;
            ResourceDesc desc{};
            desc.type = ResourceType::Buffer;
            desc.lifetime = ResourceLifetime::Transient;
            desc.buffer.size = 256;
            const ResourceHandle buffer = builder.createResource("buffer", desc);
            const PassHandle pass = builder.addPass("write-buffer");
            CHECK(throwsBuildError([&] {
                (void)builder.write(pass, buffer, Access::StorageWrite, LoadOp::Load);
            }));
        }
        return true;
    }

    bool testNonoverlappingResourcesReuseSlot() {
        RenderGraphBuilder builder;
        ResourceHandle first = builder.createResource("first", imageDesc());
        ResourceHandle second = builder.createResource("second", imageDesc());
        const PassHandle writeFirst = builder.addPass("write-first");
        const PassHandle readFirst = builder.addPass("read-first");
        const PassHandle writeSecond = builder.addPass("write-second");
        const PassHandle readSecond = builder.addPass("read-second");
        first = builder.write(writeFirst, first, Access::ColorAttachment,
            LoadOp::Clear);
        builder.read(readFirst, first, Access::SampledRead);
        second = builder.write(writeSecond, second, Access::ColorAttachment,
            LoadOp::Clear);
        builder.read(readSecond, second, Access::SampledRead);

        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        CHECK(result.graph->physicalSlots().size() == 1);
        CHECK(result.graph->resources()[0].physicalSlot ==
            result.graph->resources()[1].physicalSlot);
        CHECK(result.graph->physicalSlots()[0].logicalResources.size() == 2);
        return true;
    }

    bool testOverlappingAndIncompatibleResourcesDoNotReuse() {
        {
            RenderGraphBuilder builder;
            ResourceHandle first = builder.createResource("first", imageDesc());
            ResourceHandle second = builder.createResource("second", imageDesc());
            const PassHandle writeFirst = builder.addPass("write-first");
            const PassHandle writeSecond = builder.addPass("write-second");
            const PassHandle readBoth = builder.addPass("read-both");
            first = builder.write(writeFirst, first, Access::ColorAttachment,
                LoadOp::Clear);
            second = builder.write(writeSecond, second, Access::ColorAttachment,
                LoadOp::Clear);
            builder.read(readBoth, first, Access::SampledRead);
            builder.read(readBoth, second, Access::SampledRead);
            const CompileResult result = builder.compile();
            CHECK(result.succeeded());
            CHECK(result.graph->physicalSlots().size() == 2);
        }
        {
            RenderGraphBuilder builder;
            ResourceHandle first = builder.createResource("first",
                imageDesc(Format::Rgba16Float));
            ResourceHandle second = builder.createResource("second",
                imageDesc(Format::Rgba8Unorm));
            const PassHandle writeFirst = builder.addPass("write-first");
            const PassHandle readFirst = builder.addPass("read-first");
            const PassHandle writeSecond = builder.addPass("write-second");
            first = builder.write(writeFirst, first, Access::ColorAttachment,
                LoadOp::Clear);
            builder.read(readFirst, first, Access::SampledRead);
            second = builder.write(writeSecond, second, Access::ColorAttachment,
                LoadOp::Clear);
            const CompileResult result = builder.compile();
            CHECK(result.succeeded());
            CHECK(result.graph->physicalSlots().size() == 2);
        }
        return true;
    }

    bool testVersionedLoadOrdering() {
        RenderGraphBuilder builder;
        ResourceHandle scene = builder.createResource("scene", imageDesc());
        const PassHandle opaque = builder.addPass("opaque");
        const PassHandle transparent = builder.addPass("transparent");
        const PassHandle sample = builder.addPass("sample");
        scene = builder.write(opaque, scene, Access::ColorAttachment, LoadOp::Clear);
        scene = builder.write(transparent, scene, Access::ColorAttachment,
            LoadOp::Load);
        builder.read(sample, scene, Access::SampledRead);

        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        CHECK(result.graph->passes()[0].name == "opaque");
        CHECK(result.graph->passes()[1].name == "transparent");
        CHECK(result.graph->passes()[2].name == "sample");
        const CompiledPass& transparentPass = result.graph->passes()[1];
        CHECK(transparentPass.usageCount == 1);
        CHECK(result.graph->usages()[transparentPass.firstUsage].loadOp == LoadOp::Load);
        CHECK(result.graph->resources().size() == 1);
        return true;
    }

    bool testStaleHandlesAndFixedCapacity() {
        GraphCapacity capacity{};
        capacity.maxPasses = 1;
        capacity.maxLogicalResources = 1;
        capacity.maxResourceVersions = 2;
        capacity.maxUsages = 1;
        capacity.maxDependencies = 1;
        RenderGraphBuilder builder(capacity);
        const ResourceHandle oldResource = builder.createResource("old", imageDesc());
        const PassHandle oldPass = builder.addPass("old-pass");
        CHECK(throwsBuildError([&] { (void)builder.addPass("overflow"); }));
        builder.reset();
        CHECK(throwsBuildError([&] {
            builder.read(oldPass, oldResource, Access::SampledRead);
        }));
        return true;
    }

    bool testRepeatedCompileHashAndCache() {
        RenderGraphBuilder builder;
        ResourceHandle resource = builder.createResource("scene", imageDesc());
        const PassHandle pass = builder.addPass("produce");
        resource = builder.write(pass, resource, Access::ColorAttachment,
            LoadOp::Clear);
        CompileResult first = builder.compile();
        CompileResult second = builder.compile();
        CHECK(first.succeeded());
        CHECK(second.succeeded());
        CHECK(first.graph->topologyHash() == second.graph->topologyHash());

        const uint64_t hash = first.graph->topologyHash();
        CompiledGraphCache cache;
        cache.store(std::move(*first.graph));
        CHECK(cache.size() == 1);
        CHECK(cache.find(hash) != nullptr);
        CHECK(cache.find(hash + 1) == nullptr);
        cache.store(std::move(*second.graph));
        CHECK(cache.size() == 1);
        CHECK(cache.find(hash) != nullptr);

        builder.reset();
        ResourceHandle rebuilt = builder.createResource("scene", imageDesc());
        const PassHandle rebuiltPass = builder.addPass("produce");
        rebuilt = builder.write(rebuiltPass, rebuilt, Access::ColorAttachment,
            LoadOp::Clear);
        const CompileResult rebuiltResult = builder.compile();
        CHECK(rebuiltResult.succeeded());
        CHECK(rebuiltResult.graph->topologyHash() == hash);
        return true;
    }

    bool testLayeredMipImageContract() {
        RenderGraphBuilder builder;
        ResourceDesc desc = imageDesc();
        desc.image.extent = { 256, 256, 1 };
        desc.image.mipLevels = 9;
        desc.image.arrayLayers = 6;
        ResourceHandle cubeFaces = builder.createResource(
            "environment.cube-faces", desc);
        const PassHandle write = builder.addPass("environment.capture");
        const PassHandle read = builder.addPass("environment.prefilter");
        cubeFaces = builder.write(write, cubeFaces, Access::StorageWrite);
        builder.read(read, cubeFaces, Access::SampledRead);
        const CompileResult result = builder.compile();
        CHECK(result.succeeded());
        CHECK(result.graph->physicalSlots().size() == 1);
        CHECK(result.graph->physicalSlots()[0].image.mipLevels == 9);
        CHECK(result.graph->physicalSlots()[0].image.arrayLayers == 6);
        CHECK(result.graph->topologyHash() != 0);
        return true;
    }

    // ---- M7R R4a: clear values and store operations ---------------------------

    bool testClearValueBits() {
        constexpr ClearValue black = ClearValue::color(0.0f, 0.0f, 0.0f, 1.0f);
        static_assert(black.colorBits[3] == 0x3F80'0000u);
        static_assert(ClearValue::color(1.0f, 0.0f, 0.0f, 0.0f).colorBits[0] == 0x3F80'0000u);
        static_assert(ClearValue::colorUint(7u).colorBits[0] == 7u);
        static_assert(ClearValue{}.depth == 1.0f && ClearValue{}.stencil == 0u);
        static_assert(ClearValue::depthStencil(0.5f, 3u).depth == 0.5f);
        // Equality is bit-exact: signed zeros differ, a NaN equals itself.
        CHECK(ClearValue::color(0.0f, 0.0f, 0.0f, 0.0f) == ClearValue::colorUint(0u));
        CHECK(!(ClearValue::color(-0.0f, 0.0f, 0.0f, 0.0f) == ClearValue::colorUint(0u)));
        CHECK(!(ClearValue::depthStencil(-0.0f) == ClearValue::depthStencil(0.0f)));
        const ClearValue nan = ClearValue::depthStencil(
            std::numeric_limits<float>::quiet_NaN());
        CHECK(nan == nan);
        return true;
    }

    // One pass clearing colour and depth with `color`/`depth`, then a reader.
    CompileResult clearGraph(LoadOp load, const ClearValue& color, const ClearValue& depth,
        bool explicitOverload = true) {
        RenderGraphBuilder builder;
        ResourceHandle scene = builder.createResource("scene", imageDesc());
        ResourceHandle sceneDepth = builder.createResource("depth", imageDesc(Format::D32Float));
        const PassHandle draw = builder.addPass("draw");
        const PassHandle sample = builder.addPass("sample");
        if (explicitOverload) {
            scene = builder.write(draw, scene, Access::ColorAttachment, load,
                StoreOp::Store, color);
            sceneDepth = builder.write(draw, sceneDepth, Access::DepthAttachmentWrite, load,
                StoreOp::Store, depth);
        }
        else {
            scene = builder.write(draw, scene, Access::ColorAttachment, load);
            sceneDepth = builder.write(draw, sceneDepth, Access::DepthAttachmentWrite, load);
        }
        builder.read(sample, scene, Access::SampledRead);
        builder.read(sample, sceneDepth, Access::DepthAttachmentRead);
        return builder.compile();
    }

    bool testClearValuesCompileAndHash() {
        const ClearValue grey = ClearValue::color(0.5f, 0.5f, 0.5f, 1.0f);
        const ClearValue nearDepth = ClearValue::depthStencil(0.0f);
        const CompileResult clear = clearGraph(LoadOp::Clear, grey, nearDepth);
        CHECK(clear.succeeded());
        const auto& usages = clear.graph->usages();
        CHECK(usages[0].loadOp == LoadOp::Clear && usages[0].clearValue == grey);
        CHECK(usages[1].loadOp == LoadOp::Clear && usages[1].clearValue == nearDepth);
        CHECK(usages[0].storeOp == StoreOp::Store && usages[1].storeOp == StoreOp::Store);

        // Clear values are topology only when the usage clears.
        CHECK(clearGraph(LoadOp::Clear, grey, nearDepth).graph->topologyHash() ==
            clear.graph->topologyHash());
        CHECK(clearGraph(LoadOp::Clear, ClearValue::color(0.5f, 0.5f, 0.5f, 0.0f), nearDepth)
            .graph->topologyHash() != clear.graph->topologyHash());
        CHECK(clearGraph(LoadOp::Clear, grey, ClearValue::depthStencil(1.0f))
            .graph->topologyHash() != clear.graph->topologyHash());
        CHECK(clearGraph(LoadOp::Clear, grey, ClearValue::depthStencil(0.0f, 1u))
            .graph->topologyHash() != clear.graph->topologyHash());
        // The default value equals the overload without one.
        CHECK(clearGraph(LoadOp::Clear, ClearValue{}, ClearValue{}).graph->topologyHash() ==
            clearGraph(LoadOp::Clear, {}, {}, false).graph->topologyHash());
        // A non-clearing usage drops its value: identical usage and hash.
        const CompileResult dontCare = clearGraph(LoadOp::DontCare, grey, nearDepth);
        CHECK(dontCare.succeeded());
        CHECK(dontCare.graph->usages()[0].clearValue == ClearValue{});
        CHECK(dontCare.graph->topologyHash() ==
            clearGraph(LoadOp::DontCare, ClearValue{}, ClearValue{}).graph->topologyHash());
        CHECK(dontCare.graph->topologyHash() != clear.graph->topologyHash());
        return true;
    }

    bool testStoreOperations() {
        // DepthAttachmentRead implies LOAD + NONE; other reads are unchanged.
        const CompileResult clear = clearGraph(LoadOp::Clear, {}, {});
        CHECK(clear.succeeded());
        const CompiledPass& sample = clear.graph->passes()[1];
        const CompiledUsage& sampled = clear.graph->usages()[sample.firstUsage];
        const CompiledUsage& depthRead = clear.graph->usages()[sample.firstUsage + 1];
        CHECK(sampled.access == Access::SampledRead && sampled.loadOp == LoadOp::DontCare &&
            sampled.storeOp == StoreOp::Store);
        CHECK(depthRead.access == Access::DepthAttachmentRead && !depthRead.write);
        CHECK(depthRead.loadOp == LoadOp::Load && depthRead.storeOp == StoreOp::None);

        // A write that stores NONE leaves nothing to read.
        {
            RenderGraphBuilder builder;
            ResourceHandle scene = builder.createResource("scene", imageDesc());
            const PassHandle draw = builder.addPass("draw");
            const PassHandle sampleScene = builder.addPass("sample");
            scene = builder.write(draw, scene, Access::ColorAttachment, LoadOp::Clear,
                StoreOp::None, ClearValue{});
            builder.read(sampleScene, scene, Access::SampledRead);
            const CompileResult result = builder.compile();
            CHECK(!result.succeeded());
            CHECK(hasDiagnostic(result, DiagnosticCode::InvalidUsage));
        }
        // NONE and clear values are attachment-only.
        RenderGraphBuilder builder;
        ResourceDesc buffer{};
        buffer.type = ResourceType::Buffer;
        buffer.buffer.size = 64;
        const ResourceHandle data = builder.createResource("data", buffer);
        const PassHandle compute = builder.addPass("compute", QueueClass::Compute);
        CHECK(throwsBuildError([&] {
            (void)builder.write(compute, data, Access::StorageWrite, LoadOp::DontCare,
                StoreOp::None, ClearValue{}); }));
        CHECK(throwsBuildError([&] {
            (void)builder.write(compute, data, Access::StorageWrite, LoadOp::Clear,
                StoreOp::Store, ClearValue::colorUint(1u)); }));
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        std::string_view name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Stable DAG and compiled usages", testStableDagAndCompiledUsages },
        { "Cycle diagnostic", testCycleDiagnostic },
        { "Read-before-write diagnostic", testReadBeforeWriteDiagnostic },
        { "Imported/exported states", testImportedAndExportedStates },
        { "History invalidation", testInvalidHistoryIsExplicit },
        { "History pair declaration", testHistoryPairDeclaration },
        { "History validity keys", testHistoryValidityKeys },
        { "History view sets and reset policy", testHistoryViewSetsAndResetPolicy },
        { "Discarded content and stale export", testDiscardedContentsAndStaleExportAreRejected },
        { "Nonoverlap reuse", testNonoverlappingResourcesReuseSlot },
        { "Overlap and incompatibility", testOverlappingAndIncompatibleResourcesDoNotReuse },
        { "Versioned load ordering", testVersionedLoadOrdering },
        { "Stale handles and capacity", testStaleHandlesAndFixedCapacity },
        { "Repeated compile and cache", testRepeatedCompileHashAndCache },
        { "Layered mip image contract", testLayeredMipImageContract },
        { "Clear value bits", testClearValueBits },
        { "Clear values compile and hash", testClearValuesCompileAndHash },
        { "Store operations", testStoreOperations },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
