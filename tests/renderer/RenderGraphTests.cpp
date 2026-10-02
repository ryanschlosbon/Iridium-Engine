#include "renderer/graph/RenderGraph.h"

#include <exception>
#include <iostream>
#include <iterator>
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
        { "Discarded content and stale export", testDiscardedContentsAndStaleExportAreRejected },
        { "Nonoverlap reuse", testNonoverlappingResourcesReuseSlot },
        { "Overlap and incompatibility", testOverlappingAndIncompatibleResourcesDoNotReuse },
        { "Versioned load ordering", testVersionedLoadOrdering },
        { "Stale handles and capacity", testStaleHandlesAndFixedCapacity },
        { "Repeated compile and cache", testRepeatedCompileHashAndCache },
        { "Layered mip image contract", testLayeredMipImageContract },
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
