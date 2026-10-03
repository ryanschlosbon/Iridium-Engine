// Compiled production render-graph contracts (pure CPU). Replaces the
// VulkanProductionRenderGraph.cpp / backend source-text checks of
// Stage3ArchitectureTests groups 2 and 4 with assertions on the graph the
// executor actually receives: pass order, read/write wiring, load operations and
// the usage of qualification-only TransferSource reads. Also encodes the ADR
// invariants that are visible in the graph: one clustered-light product read by
// every lit consumer, and scene-linear AP1 until the single output transform.

#include "GraphQuery.h"
#include "TestHarness.h"

#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/vulkan/VulkanProductionRenderGraph.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;
    using RenderGraph::Access;
    using RenderGraph::LoadOp;
    using Iridium::Test::GraphQuery;

    constexpr VkExtent2D SceneExtent{ 1920, 1080 };
    constexpr VkExtent2D Ordinary2Atlas{ 960, 528 };
    constexpr VkExtent2D Hero4Atlas{ 1920, 528 };
    constexpr VkExtent2D Cinematic8Atlas{ 1920, 1072 };

    RenderGraph::CompiledGraph layeredGraph(VulkanLayeredGraphConfig layered,
        VulkanProductionGraphFeatures features = {}, bool hdr10 = false) {
        return buildVulkanProductionRenderGraph(SceneExtent,
            hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_SRGB,
            hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_SRGB, hdr10,
            GBufferLayout::CanonicalReference, {}, 4096, 8192, true, layered,
            features);
    }

    // Every optional pass enabled at once.
    RenderGraph::CompiledGraph fullGraph(bool hdr10) {
        return layeredGraph(VulkanLayeredGraphConfig{
                Ordinary2Atlas, Hero4Atlas, Cinematic8Atlas, true },
            VulkanProductionGraphFeatures{ .depthPyramid = true,
                .virtualShadowWorkingSetBytes = 1u << 20 }, hdr10);
    }

    bool testOrdinary2CompositionWiring() {
        const RenderGraph::CompiledGraph compiled =
            layeredGraph(VulkanLayeredGraphConfig{ Ordinary2Atlas });
        const GraphQuery graph(compiled);
        IRIDIUM_CHECK(graph.ordered({ "transparent.sorted.forward",
            "transparent.layered.entry.capture", "transparent.layered.exit.capture",
            "transparent.layered.local-compose",
            "transparent.layered.validation-readback-hook",
            "transparent.layered.compose-hook", "transparent.compatibility.forward",
            "bloom-hook", "output-transform" }));

        // Paired capture: exit reads the entry products it peels behind.
        IRIDIUM_CHECK(graph.writes("transparent.layered.entry.capture",
            "depth.layered.entry", Access::DepthAttachmentWrite, LoadOp::Clear));
        IRIDIUM_CHECK(graph.writes("transparent.layered.entry.capture",
            "identity.layered.entry", Access::ColorAttachment, LoadOp::Clear));
        IRIDIUM_CHECK(graph.reads("transparent.layered.entry.capture",
            "gbuffer.material-flags", Access::SampledRead));
        IRIDIUM_CHECK(graph.reads("transparent.layered.exit.capture",
            "depth.layered.entry", Access::SampledRead));
        IRIDIUM_CHECK(graph.reads("transparent.layered.exit.capture",
            "identity.layered.entry", Access::SampledRead));
        IRIDIUM_CHECK(graph.writes("transparent.layered.exit.capture",
            "identity.layered.exit", Access::ColorAttachment, LoadOp::Clear));

        // Local composition clears its atlas product and consumes both
        // interfaces plus the refraction pyramids.
        IRIDIUM_CHECK(graph.writes("transparent.layered.local-compose",
            "scene.layered.local-color", Access::ColorAttachment, LoadOp::Clear));
        for (const char* input : { "depth.layered.entry", "identity.layered.entry",
                "depth.layered.exit", "identity.layered.exit",
                "scene.refraction-color-pyramid", "depth.refraction-nearest-pyramid" })
            IRIDIUM_CHECK_MSG(graph.reads("transparent.layered.local-compose", input,
                Access::SampledRead), input);

        // Scene resolve: samples local colour and entry identity, tests opaque
        // depth read-only, and loads (never clears) the HDR scene colour.
        IRIDIUM_CHECK(graph.reads("transparent.layered.compose-hook",
            "scene.layered.local-color", Access::SampledRead));
        IRIDIUM_CHECK(graph.reads("transparent.layered.compose-hook",
            "identity.layered.entry", Access::SampledRead));
        IRIDIUM_CHECK(graph.reads("transparent.layered.compose-hook", "depth.opaque",
            Access::DepthAttachmentRead));
        IRIDIUM_CHECK(graph.writes("transparent.layered.compose-hook", "scene.color",
            Access::ColorAttachment, LoadOp::Load));
        IRIDIUM_CHECK(!graph.writes("transparent.layered.compose-hook", "depth.opaque",
            Access::DepthAttachmentWrite));
        return true;
    }

    bool testDeepTierWiring() {
        const RenderGraph::CompiledGraph compiled = layeredGraph(
            VulkanLayeredGraphConfig{ {}, Hero4Atlas, Cinematic8Atlas });
        const GraphQuery graph(compiled);
        for (const auto& [tier, count] : { std::pair{ std::string("hero4"), 4u },
                std::pair{ std::string("cinematic8"), 8u } }) {
            const std::string prefix = "transparent.layered." + tier;
            std::vector<std::string> captures;
            for (uint32_t index = 0; index < count; ++index) {
                const std::string suffix = tier + ".interface." + std::to_string(index);
                const std::string capture = prefix + ".interface." +
                    std::to_string(index) + ".capture";
                captures.push_back(capture);
                IRIDIUM_CHECK_MSG(graph.writes(capture, "depth.layered." + suffix,
                    Access::DepthAttachmentWrite, LoadOp::Clear), capture);
                IRIDIUM_CHECK_MSG(graph.writes(capture, "identity.layered." + suffix,
                    Access::ColorAttachment, LoadOp::Clear), capture);
                if (index > 0) {
                    const std::string previous = tier + ".interface." +
                        std::to_string(index - 1);
                    IRIDIUM_CHECK_MSG(graph.reads(capture, "depth.layered." + previous,
                        Access::SampledRead), capture);
                    IRIDIUM_CHECK_MSG(graph.reads(capture, "identity.layered." + previous,
                        Access::SampledRead), capture);
                    IRIDIUM_CHECK(graph.passOrder(captures[index - 1]) <
                        graph.passOrder(capture));
                }
                if (deepLayeredTerminationInterface(index, count)) {
                    const std::string terminate = prefix + ".interface." +
                        std::to_string(index) + ".terminate-tiles";
                    IRIDIUM_CHECK_MSG(graph.writes(terminate,
                        "termination.layered." + suffix, Access::StorageWrite), terminate);
                    IRIDIUM_CHECK(graph.passOrder(capture) < graph.passOrder(terminate));
                    IRIDIUM_CHECK(graph.pass(terminate)->queue ==
                        RenderGraph::QueueClass::Compute);
                }
                IRIDIUM_CHECK_MSG(graph.reads(prefix + ".local-compose",
                    "depth.layered." + suffix, Access::SampledRead), suffix);
                IRIDIUM_CHECK_MSG(graph.reads(prefix + ".local-compose",
                    "identity.layered." + suffix, Access::SampledRead), suffix);
            }
            IRIDIUM_CHECK(graph.writes(prefix + ".local-compose",
                "scene.layered." + tier + ".local-color", Access::ColorAttachment,
                LoadOp::Clear));
            IRIDIUM_CHECK(graph.passOrder(captures.back()) <
                graph.passOrder(prefix + ".local-compose"));
            IRIDIUM_CHECK(graph.passOrder(prefix + ".local-compose") <
                graph.passOrder(prefix + ".validation-readback-hook"));
            // The combined resolve reads each tier's first interface identity
            // and its local colour.
            IRIDIUM_CHECK(graph.reads("transparent.layered.deep.compose-hook",
                "identity.layered." + tier + ".interface.0", Access::SampledRead));
            IRIDIUM_CHECK(graph.reads("transparent.layered.deep.compose-hook",
                "scene.layered." + tier + ".local-color", Access::SampledRead));
            IRIDIUM_CHECK(graph.passOrder(prefix + ".local-compose") <
                graph.passOrder("transparent.layered.deep.compose-hook"));
        }
        IRIDIUM_CHECK(graph.writes("transparent.layered.deep.compose-hook", "scene.color",
            Access::ColorAttachment, LoadOp::Load));
        IRIDIUM_CHECK(graph.reads("transparent.layered.deep.compose-hook", "depth.opaque",
            Access::DepthAttachmentRead));
        IRIDIUM_CHECK(graph.ordered({ "transparent.layered.deep.compose-hook",
            "transparent.compatibility.forward" }));
        return true;
    }

    bool testWeightedOitWiring() {
        const RenderGraph::CompiledGraph compiled =
            layeredGraph(VulkanLayeredGraphConfig{ {}, {}, {}, true });
        const GraphQuery graph(compiled);
        IRIDIUM_CHECK(graph.ordered({ "transparent.compatibility.forward",
            "transparent.oit.accumulate", "transparent.oit.resolve", "bloom-hook" }));
        IRIDIUM_CHECK(graph.writes("transparent.oit.accumulate",
            "transparency.oit.accumulation", Access::ColorAttachment, LoadOp::Clear));
        IRIDIUM_CHECK(graph.writes("transparent.oit.accumulate",
            "transparency.oit.revealage", Access::ColorAttachment, LoadOp::Clear));
        IRIDIUM_CHECK(graph.reads("transparent.oit.accumulate", "depth.opaque",
            Access::DepthAttachmentRead));
        IRIDIUM_CHECK(graph.writers("depth.opaque").back() ==
            std::string_view("transparent.compatibility.forward"));
        IRIDIUM_CHECK(graph.reads("transparent.oit.resolve",
            "transparency.oit.accumulation", Access::SampledRead));
        IRIDIUM_CHECK(graph.reads("transparent.oit.resolve",
            "transparency.oit.revealage", Access::SampledRead));
        IRIDIUM_CHECK(graph.writes("transparent.oit.resolve", "scene.color",
            Access::ColorAttachment, LoadOp::Load));
        IRIDIUM_CHECK(graph.users("transparency.oit.accumulation").size() == 2u);
        return true;
    }

    // Qualification readbacks are graph-visible: TransferSource only appears on
    // readback/capture hooks, which never write scene-visible images.
    bool testTransferSourceOnlyOnHooks() {
        for (const bool hdr10 : { false, true }) {
            const RenderGraph::CompiledGraph compiled = fullGraph(hdr10);
            const GraphQuery graph(compiled);
            uint32_t hookCount = 0;
            for (const RenderGraph::CompiledPass& pass : compiled.passes()) {
                bool readsTransferSource = false;
                for (const auto& usage : graph.usages(pass))
                    readsTransferSource |= !usage.write &&
                        usage.access == Access::TransferSource;
                if (!readsTransferSource) continue;
                ++hookCount;
                const std::string_view name = pass.name;
                IRIDIUM_CHECK_MSG(name.find("readback") != std::string_view::npos ||
                    name == "final-capture-hook" ||
                    name == "scene-color-capture-hook", name);
                for (const auto& usage : graph.usages(pass)) {
                    if (!usage.write) continue;
                    const auto* resource = graph.resourceAt(usage.logicalResourceIndex);
                    IRIDIUM_CHECK(resource != nullptr);
                    IRIDIUM_CHECK_MSG(resource->desc.lifetime ==
                        RenderGraph::ResourceLifetime::External &&
                        resource->desc.type == RenderGraph::ResourceType::Buffer,
                        name << " writes " << resource->name);
                }
            }
            // Ordinary2, both deep tiers, depth pyramid, the scene-linear and
            // the final capture.
            IRIDIUM_CHECK(hookCount >= 6u);
            IRIDIUM_CHECK(graph.reads("scene-color-capture-hook", "scene.color",
                Access::TransferSource));
            IRIDIUM_CHECK(graph.pass("scene-color-capture-hook")->queue ==
                RenderGraph::QueueClass::Transfer);
            IRIDIUM_CHECK(graph.hasPass("final-capture-hook"));
            IRIDIUM_CHECK(graph.reads("final-capture-hook", "scene.color",
                Access::TransferSource));
            IRIDIUM_CHECK(graph.reads("final-capture-hook", "output.display",
                Access::TransferSource));
        }
        return true;
    }

    // ADR: scene-linear AP1 until the single output transform.
    bool testSingleOutputTransform() {
        for (const bool hdr10 : { false, true }) {
            const RenderGraph::CompiledGraph compiled = fullGraph(hdr10);
            const GraphQuery graph(compiled);
            const auto outputWriters = graph.writers("output.display");
            IRIDIUM_CHECK(outputWriters.size() == 1u &&
                outputWriters.front() == "output-transform");
            IRIDIUM_CHECK(graph.reads("output-transform", "scene.color",
                Access::SampledRead));
            const auto transform = *graph.passOrder("output-transform");
            for (const std::string_view writer : graph.writers("scene.color"))
                IRIDIUM_CHECK_MSG(*graph.passOrder(writer) < transform, writer);
            for (const std::string_view user : graph.users("scene.color")) {
                if (*graph.passOrder(user) <= transform) continue;
                IRIDIUM_CHECK_MSG(user == "final-capture-hook", user);
            }
            // The scene-linear capture reads the last scene writer's output.
            for (const std::string_view writer : graph.writers("scene.color"))
                IRIDIUM_CHECK_MSG(*graph.passOrder(writer) <
                    *graph.passOrder("scene-color-capture-hook"), writer);
            const auto* sceneColor = graph.resource("scene.color");
            IRIDIUM_CHECK(sceneColor != nullptr &&
                sceneColor->desc.image.format == RenderGraph::Format::Rgba16Float);
            // Scene-linear capture, output transform, final capture, then UI.
            IRIDIUM_CHECK(graph.ordered({ "scene-color-capture-hook", "bloom-hook",
                "output-transform", "final-capture-hook",
                hdr10 ? "ui-compose" : "ui-present" }));
            if (hdr10) {
                IRIDIUM_CHECK(graph.ordered({ "output-transform", "ui-compose",
                    "hdr10-encode-present" }));
                IRIDIUM_CHECK(graph.writers("swapchain").size() == 1u &&
                    graph.writers("swapchain").front() == "hdr10-encode-present");
            } else {
                IRIDIUM_CHECK(graph.ordered({ "output-transform", "ui-present" }));
                IRIDIUM_CHECK(graph.writers("swapchain").front() == "ui-present");
            }
        }
        return true;
    }

    // ADR: one clustered-light representation for deferred and forward.
    bool testOneClusteredLightProduct() {
        const RenderGraph::CompiledGraph compiled = fullGraph(false);
        const GraphQuery graph(compiled);
        const std::array product{ kClusterGlobalResourceName, kClusterHeaderResourceName,
            kClusterIndexResourceName, kClusterFallbackResourceName };
        for (const char* name : product) IRIDIUM_CHECK(graph.resource(name) != nullptr);
        const std::array consumers{ "lighting", "forward-opaque",
            "transparent.sorted.forward", "transparent.layered.local-compose",
            "transparent.layered.hero4.local-compose",
            "transparent.layered.cinematic8.local-compose",
            "transparent.compatibility.forward", "transparent.oit.accumulate" };
        for (const char* consumer : consumers) {
            IRIDIUM_CHECK_MSG(graph.hasPass(consumer), consumer);
            for (const char* name : product)
                IRIDIUM_CHECK_MSG(graph.reads(consumer, name, Access::StorageRead),
                    consumer << " does not read " << name);
            for (const char* shadow : { "shadow.directional", "shadow.spot",
                    "shadow.point.256", "shadow.point.512", "shadow.point.1024" })
                IRIDIUM_CHECK_MSG(graph.reads(consumer, shadow, Access::SampledRead),
                    consumer << " does not read " << shadow);
        }
        const auto finalize = *graph.passOrder("lighting.cluster.finalize");
        for (const char* name : product) {
            for (const std::string_view writer : graph.writers(name)) {
                IRIDIUM_CHECK_MSG(writer.starts_with("lighting.cluster."), writer);
                IRIDIUM_CHECK(*graph.passOrder(writer) <= finalize);
            }
        }
        IRIDIUM_CHECK(finalize < *graph.passOrder("lighting"));
        // Exactly one set of cluster resources exists in the graph.
        std::set<std::string> clusterResources;
        for (const auto& resource : compiled.resources())
            if (resource.name.starts_with("lighting.cluster."))
                clusterResources.insert(resource.name);
        for (const char* name : product) IRIDIUM_CHECK(clusterResources.count(name) == 1u);
        return true;
    }

    // M7R R2.9: a backend with no extension attached (every production run,
    // and every IRIDIUM_QUALIFICATION=OFF build) declares no hook passes.
    // The graph then differs from the qualification graph only by the
    // validation readback hooks and the scene-linear capture hook (R3b.5);
    // final-capture-hook stays (retained editor views use it). The VSM depth
    // snapshot changes a usage, not a pass.
    bool testNullExtensionGraphDiffersOnlyByHooks() {
        const auto passNames = [](const RenderGraph::CompiledGraph& compiled) {
            std::vector<std::string> names;
            for (const RenderGraph::CompiledPass& pass : compiled.passes())
                names.emplace_back(pass.name);
            return names;
        };
        // What VulkanQualificationExtension declares without the VSM oracle.
        constexpr VulkanGraphHooks qualificationHooks{ .depthPyramidValidation = true,
            .layeredValidation = true, .virtualShadowDepthSnapshot = false,
            .sceneColorCapture = true };
        const std::array<VulkanLayeredGraphConfig, 2> layeredConfigs{
            VulkanLayeredGraphConfig{},
            VulkanLayeredGraphConfig{ Ordinary2Atlas, Hero4Atlas, Cinematic8Atlas, true } };
        for (const bool hdr10 : { false, true }) {
            for (const VulkanLayeredGraphConfig& layered : layeredConfigs) {
                for (const bool depthPyramid : { false, true }) {
                    VulkanProductionGraphFeatures features{
                        .depthPyramid = depthPyramid,
                        .virtualShadowWorkingSetBytes = depthPyramid ? 1u << 20 : 0u };
                    features.hooks = qualificationHooks;
                    const std::vector<std::string> on =
                        passNames(layeredGraph(layered, features, hdr10));
                    features.hooks = VulkanGraphHooks::none();
                    const std::vector<std::string> off =
                        passNames(layeredGraph(layered, features, hdr10));
                    std::vector<std::string> onWithoutHooks;
                    for (const std::string& name : on)
                        if (!name.ends_with("validation-readback-hook") &&
                            name != "scene-color-capture-hook")
                            onWithoutHooks.push_back(name);
                    IRIDIUM_CHECK(off == onWithoutHooks);
                    IRIDIUM_CHECK(std::ranges::find(off, std::string("final-capture-hook"))
                        != off.end());
                    IRIDIUM_CHECK(std::ranges::find(on,
                        std::string("scene-color-capture-hook")) != on.end());
                    IRIDIUM_CHECK(std::ranges::find(off,
                        std::string("scene-color-capture-hook")) == off.end());
                    // The default (empty-scene) graph differs only by the
                    // scene-linear capture hook.
                    const bool optionalProducts = depthPyramid ||
                        layered.ordinary2AtlasExtent.width != 0u;
                    IRIDIUM_CHECK(optionalProducts || on.size() == off.size() + 1u);
                }
            }
        }
        // VSM depth snapshot: same passes; only the request readback's scene
        // depth TransferSource read differs.
        VulkanProductionGraphFeatures vsm{ .virtualShadowWorkingSetBytes = 1u << 20 };
        vsm.hooks = VulkanGraphHooks::none();
        const RenderGraph::CompiledGraph withoutSnapshot =
            layeredGraph(VulkanLayeredGraphConfig{}, vsm);
        vsm.hooks.virtualShadowDepthSnapshot = true;
        const RenderGraph::CompiledGraph withSnapshot =
            layeredGraph(VulkanLayeredGraphConfig{}, vsm);
        IRIDIUM_CHECK(passNames(withSnapshot) == passNames(withoutSnapshot));
        IRIDIUM_CHECK(GraphQuery(withSnapshot).reads("shadow.virtual.request-readback",
            "depth.opaque", Access::TransferSource));
        IRIDIUM_CHECK(!GraphQuery(withoutSnapshot).reads("shadow.virtual.request-readback",
            "depth.opaque", Access::TransferSource));
        return true;
    }

    // M7R R3b.7/R3b.8: previously undeclared GPU work becomes graph passes.
    // Declaring them at matching positions must keep the R3b.6 compiled order
    // as a subsequence (the passes below removed, the rest is unchanged) and
    // leave every physical slot's membership unchanged (their resources are
    // imported). The golden was generated from the R3b.6 graph.
    struct GoldenTopology {
        const char* name;
        std::vector<std::string_view> passes;
        std::vector<std::vector<std::string_view>> slots;
    };
    const std::vector<GoldenTopology>& r3b6Golden() {
        static const std::vector<GoldenTopology> golden{
#include "fixtures/ProductionGraphR3b6Golden.inc"
        };
        return golden;
    }
    constexpr std::array<std::string_view, 5> DeclaredSinceR3b6{
        "shadow.directional.compact", "shadow.spot.compact", "shadow.point.compact",
        "gpu-scene.opaque.compact", "lighting.probe-cluster" };

    bool testDeclaredWorkKeepsOrderAndSlots() {
        const VulkanLayeredGraphConfig all{ Ordinary2Atlas, Hero4Atlas, Cinematic8Atlas, true };
        const VulkanProductionGraphFeatures full{ .depthPyramid = true,
            .virtualShadowWorkingSetBytes = 1u << 20 };
        VulkanProductionGraphFeatures fullNone = full;
        fullNone.hooks = VulkanGraphHooks::none();
        VulkanProductionGraphFeatures lean{ .depthPyramid = true,
            .clusterTelemetryReadback = false };
        lean.hooks = VulkanGraphHooks::none();
        const std::array<std::pair<const char*, RenderGraph::CompiledGraph>, 6> topologies{ {
            { "default-sdr", layeredGraph({}) },
            { "default-hdr10", layeredGraph({}, {}, true) },
            { "full-sdr", layeredGraph(all, full) },
            { "full-hdr10", layeredGraph(all, full, true) },
            { "full-sdr-no-hooks", layeredGraph(all, fullNone) },
            { "lean-depth-pyramid", layeredGraph({}, lean) },
        } };
        IRIDIUM_CHECK(r3b6Golden().size() == topologies.size());
        for (size_t index = 0; index < topologies.size(); ++index) {
            const auto& [name, compiled] = topologies[index];
            const GoldenTopology& golden = r3b6Golden()[index];
            IRIDIUM_CHECK_MSG(std::string_view(golden.name) == name, name);
            std::vector<std::string_view> previous;
            for (const RenderGraph::CompiledPass& pass : compiled.passes())
                if (std::ranges::find(DeclaredSinceR3b6, pass.name) ==
                    DeclaredSinceR3b6.end())
                    previous.push_back(pass.name);
            IRIDIUM_CHECK_MSG(previous == golden.passes, name);
            IRIDIUM_CHECK_MSG(compiled.physicalSlots().size() == golden.slots.size(), name);
            for (size_t slot = 0; slot < golden.slots.size(); ++slot) {
                std::vector<std::string_view> members;
                for (const uint32_t logical : compiled.physicalSlots()[slot].logicalResources)
                    members.push_back(compiled.resources()[logical].name);
                IRIDIUM_CHECK_MSG(members == golden.slots[slot], name << " slot " << slot);
            }

            // Producers run directly before their consumers (shadows, gbuffer).
            const GraphQuery graph(compiled);
            for (const auto& [producer, consumer] : {
                    std::pair{ "shadow.directional.compact", "shadow.directional" },
                    std::pair{ "shadow.spot.compact", "shadow.spot" },
                    std::pair{ "shadow.point.compact", "shadow.point" },
                    std::pair{ "gpu-scene.opaque.compact", "gbuffer" } }) {
                IRIDIUM_CHECK_MSG(*graph.passOrder(producer) + 1u ==
                    *graph.passOrder(consumer), name << ' ' << producer);
                IRIDIUM_CHECK(graph.pass(producer)->queue == RenderGraph::QueueClass::Compute);
                const std::string view = std::string(producer).substr(0,
                    std::string_view(producer).size() - std::string_view(".compact").size());
                for (const char* suffix : { ".indirect-commands", ".indirect-counts" }) {
                    const std::string buffer = view + suffix;
                    IRIDIUM_CHECK_MSG(graph.writes(producer, buffer,
                        Access::StorageReadWrite), buffer);
                    IRIDIUM_CHECK_MSG(graph.reads(consumer, buffer, Access::IndirectRead),
                        buffer);
                    const auto* resource = graph.resource(buffer);
                    IRIDIUM_CHECK(resource != nullptr && resource->desc.imported &&
                        resource->desc.buffer.variableSize &&
                        resource->physicalSlot == RenderGraph::InvalidIndex);
                }
            }
            IRIDIUM_CHECK(graph.ordered({ "gbuffer", "lighting.probe-cluster",
                "lighting.cluster.clear" }));
            for (const char* buffer : { "lighting.probe-cluster.headers",
                    "lighting.probe-cluster.indices" }) {
                IRIDIUM_CHECK(graph.writes("lighting.probe-cluster", buffer,
                    Access::StorageWrite));
                IRIDIUM_CHECK_MSG(graph.reads("lighting", buffer, Access::StorageRead),
                    buffer);
            }
        }
        return true;
    }

} // namespace

int main() {
    constexpr Iridium::Test::TestCase tests[] = {
        { "Ordinary2 composition wiring", testOrdinary2CompositionWiring },
        { "deep-tier capture and composition wiring", testDeepTierWiring },
        { "WeightedOIT accumulate/resolve wiring", testWeightedOitWiring },
        { "TransferSource only on qualification hooks", testTransferSourceOnlyOnHooks },
        { "single output transform", testSingleOutputTransform },
        { "one clustered-light product", testOneClusteredLightProduct },
        { "null-extension graph differs only by hooks",
            testNullExtensionGraphDiffersOnlyByHooks },
        { "declared work keeps the R3b.6 order and slots",
            testDeclaredWorkKeepsOrderAndSlots },
    };
    return Iridium::Test::runTests(tests);
}
