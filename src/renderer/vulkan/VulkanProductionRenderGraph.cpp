#include "renderer/vulkan/VulkanProductionRenderGraph.h"

#include "renderer/vulkan/VulkanRenderGraphExecutor.h"
#include "renderer/vulkan/VulkanGBufferLayout.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/ShadowTypes.h"
#include "renderer/rhi/DepthPyramid.h"

#include <limits>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Iridium {
namespace {

    RenderGraph::ResourceDesc imageDesc(RenderGraph::Format format,
        VkExtent2D extent,
        RenderGraph::ResourceLifetime lifetime =
            RenderGraph::ResourceLifetime::Transient) {
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Image;
        desc.lifetime = lifetime;
        desc.image.format = format;
        desc.image.extent = { extent.width, extent.height, 1 };
        return desc;
    }

    uint32_t fullMipCount(VkExtent2D extent) noexcept {
        uint32_t maximum = (std::max)(extent.width, extent.height);
        uint32_t levels = 0;
        while (maximum != 0u) {
            ++levels;
            maximum >>= 1u;
        }
        return levels;
    }

    RenderGraph::ResourceDesc bufferDesc(uint64_t size,
        uint32_t alignment = 4) {
        if (size == 0) {
            throw std::invalid_argument(
                "Production render-graph buffers must be non-empty");
        }
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Buffer;
        desc.buffer.size = size;
        desc.buffer.alignment = alignment;
        return desc;
    }

    uint64_t checkedBufferBytes(uint64_t count, uint64_t stride) {
        if (count > (std::numeric_limits<uint64_t>::max)() / stride) {
            throw std::overflow_error(
                "Production render-graph buffer size overflow");
        }
        return count * stride;
    }

    const char* layeredQualityName(TransparencyQuality quality) noexcept {
        switch (quality) {
        case TransparencyQuality::Ordinary2: return "Ordinary2";
        case TransparencyQuality::Hero4: return "Hero4";
        case TransparencyQuality::Cinematic8: return "Cinematic8";
        }
        return "Invalid";
    }

    void validateLayeredTierGraphConfig(VkExtent2D sceneExtent,
        VulkanLayeredGraphConfig layered, TransparencyQuality quality) {
        if (!layered.enabled(quality)) return;
        const VkExtent2D atlasExtent = layered.atlasExtent(quality);
        const std::string tierName = layeredQualityName(quality);
        if (atlasExtent.width == 0u || atlasExtent.height == 0u) {
            throw std::invalid_argument(tierName +
                " atlas dimensions must both be nonzero");
        }
        if ((atlasExtent.width & 15u) != 0u ||
            (atlasExtent.height & 15u) != 0u) {
            throw std::invalid_argument(tierName +
                " atlas dimensions must be 16x16-tile aligned");
        }
        if (atlasExtent.width > sceneExtent.width ||
            atlasExtent.height > sceneExtent.height) {
            throw std::invalid_argument(tierName +
                " atlas must fit within the scene extent");
        }
        const uint64_t atlasPixels = static_cast<uint64_t>(atlasExtent.width) *
            atlasExtent.height;
        if (atlasPixels > layeredAtlasAreaCapPixels(sceneExtent.width,
                sceneExtent.height, quality)) {
            throw std::invalid_argument(tierName +
                " atlas exceeds its screen-area cap");
        }
    }

    void validateLayeredGraphConfig(VkExtent2D sceneExtent,
        bool transparencyPyramids, VulkanLayeredGraphConfig layered) {
        validateLayeredTierGraphConfig(sceneExtent, layered,
            TransparencyQuality::Ordinary2);
        validateLayeredTierGraphConfig(sceneExtent, layered,
            TransparencyQuality::Hero4);
        validateLayeredTierGraphConfig(sceneExtent, layered,
            TransparencyQuality::Cinematic8);
        if (layered.anyEnabled() && !transparencyPyramids) {
            throw std::invalid_argument(
                "Layered transparency requires the shared transparency pyramids");
        }
    }

} // namespace

RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(VkExtent2D extent,
    VkFormat swapchainFormat, VkFormat outputFormat, bool hdr10Composition,
    GBufferLayout gBufferLayout, ClusterGridConfig clusterConfig,
    uint32_t directionalShadowResolution,
    uint32_t spotShadowAtlasResolution,
    bool transparencyPyramids,
    VulkanLayeredGraphConfig layered,
    VulkanProductionGraphFeatures features) {
    return buildVulkanProductionRenderGraph(extent, extent,
        swapchainFormat, outputFormat, hdr10Composition, gBufferLayout,
        clusterConfig, directionalShadowResolution,
        spotShadowAtlasResolution, transparencyPyramids, layered, features);
}

RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
    VkExtent2D sceneExtent, VkExtent2D presentationExtent,
    VkFormat swapchainFormat, VkFormat outputFormat, bool hdr10Composition,
    GBufferLayout gBufferLayout, ClusterGridConfig clusterConfig,
    uint32_t directionalShadowResolution,
    uint32_t spotShadowAtlasResolution,
    bool transparencyPyramids,
    VulkanLayeredGraphConfig layered,
    VulkanProductionGraphFeatures features) {
    const bool depthPyramid = features.depthPyramid;
    const uint64_t virtualShadowWorkingSetBytes =
        features.virtualShadowWorkingSetBytes;
    if (sceneExtent.width == 0 || sceneExtent.height == 0 ||
        presentationExtent.width == 0 || presentationExtent.height == 0) {
        throw std::invalid_argument("Production render graph requires a non-empty extent");
    }
    if (directionalShadowResolution == 0 || spotShadowAtlasResolution == 0)
        throw std::invalid_argument(
            "Production render graph requires nonzero shadow resolutions");
    validateLayeredGraphConfig(sceneExtent, transparencyPyramids, layered);

    RenderGraph::RenderGraphBuilder graph;
    using RenderGraph::Access;
    using RenderGraph::LoadOp;
    using RenderGraph::StoreOp;
    using RenderGraph::ClearValue;
    // M7R R4a: every clearing attachment declares the exact value its render
    // pass clears with today (R4 design inventory), so the executor's
    // dynamic-rendering plans clear bit-identically. Depth clears to 1.0.
    const ClearValue opaqueBlack = ClearValue::color(0.0f, 0.0f, 0.0f, 1.0f);
    const ClearValue transparentBlack = ClearValue::color(0.0f, 0.0f, 0.0f, 0.0f);
    const ClearValue zeroUint = ClearValue::colorUint(0u);
    const ClearValue farDepth = ClearValue::depthStencil(1.0f, 0u);

    if (clusterConfig.tileWidth == 0 || clusterConfig.tileHeight == 0 ||
        clusterConfig.depthSlices == 0) {
        throw std::invalid_argument("Production cluster dimensions must be nonzero");
    }
    const VulkanGBufferFormats formats = vulkanGBufferFormats(gBufferLayout);
    const uint64_t tilesX = (static_cast<uint64_t>(sceneExtent.width) +
        clusterConfig.tileWidth - 1u) / clusterConfig.tileWidth;
    const uint64_t tilesY = (static_cast<uint64_t>(sceneExtent.height) +
        clusterConfig.tileHeight - 1u) / clusterConfig.tileHeight;
    const uint64_t clusterCount = tilesX * tilesY * clusterConfig.depthSlices;
    const uint64_t scanScratchWords =
        clusterScanScratchElementCount(clusterCount);

    RenderGraph::ResourceHandle clusterGlobal = graph.createResource(
        kClusterGlobalResourceName,
        bufferDesc(checkedBufferBytes(clusterConfig.maximumDirectionalLights,
            sizeof(uint32_t))));
    RenderGraph::ResourceHandle clusterHeaders = graph.createResource(
        kClusterHeaderResourceName,
        bufferDesc(checkedBufferBytes(clusterCount,
            sizeof(ClusterLightHeader))));
    RenderGraph::ResourceHandle clusterIndices = graph.createResource(
        kClusterIndexResourceName,
        bufferDesc(checkedBufferBytes(clusterConfig.maximumLightReferences,
            sizeof(uint32_t))));
    RenderGraph::ResourceHandle clusterFallback = graph.createResource(
        kClusterFallbackResourceName,
        bufferDesc(checkedBufferBytes(clusterConfig.maximumFallbackLights,
            sizeof(uint32_t))));
    RenderGraph::ResourceHandle clusterDiagnostics = graph.createResource(
        kClusterDiagnosticResourceName, bufferDesc(64, 16));
    RenderGraph::ResourceHandle clusterReadback{};
    if (features.clusterTelemetryReadback) {
        RenderGraph::ResourceDesc clusterReadbackDesc = bufferDesc(64, 16);
        clusterReadbackDesc.lifetime = RenderGraph::ResourceLifetime::External;
        clusterReadbackDesc.imported = true;
        clusterReadbackDesc.initialAccess = Access::TransferDestination;
        clusterReadback = graph.createResource(
            "lighting.cluster.diagnostics-readback", clusterReadbackDesc);
    }
    RenderGraph::ResourceHandle clusterCounts = graph.createResource(
        kClusterCountResourceName,
        bufferDesc(checkedBufferBytes(clusterCount, sizeof(uint32_t))));
    RenderGraph::ResourceHandle clusterCursors = graph.createResource(
        kClusterCursorResourceName,
        bufferDesc(checkedBufferBytes(clusterCount, sizeof(uint32_t))));
    RenderGraph::ResourceHandle clusterScanScratch = graph.createResource(
        kClusterScanScratchResourceName,
        bufferDesc(checkedBufferBytes(scanScratchWords, sizeof(uint32_t))));
    RenderGraph::ResourceHandle clusterIndirect = graph.createResource(
        kClusterIndirectResourceName, bufferDesc(32, 16));

    RenderGraph::ResourceHandle normal = graph.createResource("gbuffer.normal",
        imageDesc(toGraphFormat(formats.normalF90), sceneExtent));
    RenderGraph::ResourceHandle albedo = graph.createResource("gbuffer.albedo",
        imageDesc(toGraphFormat(formats.diffuseAo), sceneExtent));
    RenderGraph::ResourceHandle emissive = graph.createResource("gbuffer.emissive",
        imageDesc(toGraphFormat(formats.emissive), sceneExtent));
    RenderGraph::ResourceHandle f0Roughness = graph.createResource(
        "gbuffer.f0-roughness", imageDesc(toGraphFormat(formats.f0Roughness), sceneExtent));
    RenderGraph::ResourceHandle materialFlags = graph.createResource(
        "gbuffer.material-flags", imageDesc(toGraphFormat(formats.materialFlags), sceneExtent));
    RenderGraph::ResourceDesc directionalShadowDesc = imageDesc(
        RenderGraph::Format::D32Float,
        { directionalShadowResolution, directionalShadowResolution },
        RenderGraph::ResourceLifetime::External);
    directionalShadowDesc.image.arrayLayers = kDirectionalShadowLayerCount;
    directionalShadowDesc.imported = true;
    directionalShadowDesc.initialAccess = Access::SampledRead;
    RenderGraph::ResourceHandle directionalShadow = graph.createResource(
        "shadow.directional", directionalShadowDesc);
    RenderGraph::ResourceDesc spotShadowDesc = imageDesc(
        RenderGraph::Format::D32Float,
        { spotShadowAtlasResolution, spotShadowAtlasResolution },
        RenderGraph::ResourceLifetime::External);
    spotShadowDesc.imported = true;
    spotShadowDesc.initialAccess = Access::SampledRead;
    RenderGraph::ResourceHandle spotShadow = graph.createResource(
        "shadow.spot", spotShadowDesc);
    std::array<RenderGraph::ResourceHandle, 3> pointShadows{};
    constexpr std::array<uint32_t, 3> PointResolutions{ 256, 512, 1024 };
    const std::array<uint32_t, 3>& PointCapacities =
        features.pointShadowPoolCapacities;
    for (uint32_t tier = 0; tier < pointShadows.size(); ++tier) {
        if (PointCapacities[tier] == 0u)
            throw std::invalid_argument(
                "Production render graph requires nonzero point-shadow pools");
        RenderGraph::ResourceDesc pointDesc = imageDesc(
            RenderGraph::Format::D32Float,
            { PointResolutions[tier], PointResolutions[tier] },
            RenderGraph::ResourceLifetime::External);
        pointDesc.image.arrayLayers = PointCapacities[tier] * 6u;
        pointDesc.imported = true;
        pointDesc.initialAccess = Access::SampledRead;
        pointShadows[tier] = graph.createResource(
            "shadow.point." + std::to_string(PointResolutions[tier]),
            pointDesc);
    }
    RenderGraph::ResourceHandle depth = graph.createResource("depth.opaque",
        imageDesc(RenderGraph::Format::D32Float, sceneExtent));
    RenderGraph::ResourceHandle litScene = graph.createResource("scene.color",
        imageDesc(RenderGraph::Format::Rgba16Float, sceneExtent));
    RenderGraph::ResourceHandle refractionColor{};
    RenderGraph::ResourceHandle refractionDepth{};
    if (depthPyramid) {
        if (sceneExtent.width > 65535 || sceneExtent.height > 65535)
            throw std::invalid_argument("Depth pyramid extent exceeds integer reduction range");
    }
    if (transparencyPyramids) {
        RenderGraph::ResourceDesc colorPyramid = imageDesc(
            RenderGraph::Format::Rgba16Float, sceneExtent);
        colorPyramid.image.mipLevels = fullMipCount(sceneExtent);
        refractionColor = graph.createResource("scene.refraction-color-pyramid",
            colorPyramid);
        RenderGraph::ResourceDesc depthPyramid = imageDesc(
            RenderGraph::Format::R32Float, sceneExtent);
        depthPyramid.image.mipLevels = colorPyramid.image.mipLevels;
        refractionDepth = graph.createResource("depth.refraction-nearest-pyramid",
            depthPyramid);
    }
    RenderGraph::ResourceHandle layeredEntryDepth{};
    RenderGraph::ResourceHandle layeredEntryIdentity{};
    RenderGraph::ResourceHandle layeredExitDepth{};
    RenderGraph::ResourceHandle layeredExitIdentity{};
    RenderGraph::ResourceHandle layeredLocalColor{};
    if (layered.enabled(TransparencyQuality::Ordinary2)) {
        const VkExtent2D atlasExtent = layered.ordinary2AtlasExtent;
        layeredEntryDepth = graph.createResource("depth.layered.entry",
            imageDesc(RenderGraph::Format::D32Float,
                atlasExtent));
        layeredEntryIdentity = graph.createResource(
            "identity.layered.entry",
            imageDesc(RenderGraph::Format::R32Uint,
                atlasExtent));
        layeredExitDepth = graph.createResource("depth.layered.exit",
            imageDesc(RenderGraph::Format::D32Float,
                atlasExtent));
        layeredExitIdentity = graph.createResource(
            "identity.layered.exit",
            imageDesc(RenderGraph::Format::R32Uint,
                atlasExtent));
        layeredLocalColor = graph.createResource(
            "scene.layered.local-color",
            imageDesc(RenderGraph::Format::Rgba16Float,
                atlasExtent));
    }
    struct DeepLayeredTierResources {
        std::array<RenderGraph::ResourceHandle, 8> depth{};
        std::array<RenderGraph::ResourceHandle, 8> identity{};
        std::array<RenderGraph::ResourceHandle, 8> tileTermination{};
        RenderGraph::ResourceHandle localColor{};
        VkExtent2D atlasExtent{};
        uint32_t interfaceCount = 0u;
        std::string name;
    };
    const auto createDeepLayeredTier = [&](TransparencyQuality quality,
            std::string name) {
        DeepLayeredTierResources tier{};
        tier.atlasExtent = layered.atlasExtent(quality);
        if (!layered.enabled(quality)) return tier;
        tier.interfaceCount =
            layeredQualityTierContract(quality).maximumInterfaceCount;
        tier.name = std::move(name);
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < tier.interfaceCount; ++interfaceIndex) {
            const std::string suffix = tier.name + ".interface." +
                std::to_string(interfaceIndex);
            tier.depth[interfaceIndex] = graph.createResource(
                "depth.layered." + suffix,
                imageDesc(RenderGraph::Format::D32Float, tier.atlasExtent));
            tier.identity[interfaceIndex] = graph.createResource(
                "identity.layered." + suffix,
                imageDesc(RenderGraph::Format::R32Uint, tier.atlasExtent));
            if (deepLayeredTerminationInterface(interfaceIndex,
                    tier.interfaceCount)) {
                const VkExtent2D tileExtent{
                    (tier.atlasExtent.width +
                        kDeepLayeredEarlyTerminationTileSize - 1u) /
                        kDeepLayeredEarlyTerminationTileSize,
                    (tier.atlasExtent.height +
                        kDeepLayeredEarlyTerminationTileSize - 1u) /
                        kDeepLayeredEarlyTerminationTileSize };
                tier.tileTermination[interfaceIndex] = graph.createResource(
                    "termination.layered." + suffix,
                    imageDesc(RenderGraph::Format::R32Uint, tileExtent));
            }
        }
        tier.localColor = graph.createResource(
            "scene.layered." + tier.name + ".local-color",
            imageDesc(RenderGraph::Format::Rgba16Float, tier.atlasExtent));
        return tier;
    };
    DeepLayeredTierResources hero4 = createDeepLayeredTier(
        TransparencyQuality::Hero4, "hero4");
    DeepLayeredTierResources cinematic8 = createDeepLayeredTier(
        TransparencyQuality::Cinematic8, "cinematic8");
    RenderGraph::ResourceHandle weightedOitAccumulation{};
    RenderGraph::ResourceHandle weightedOitRevealage{};
    if (layered.weightedOit) {
        weightedOitAccumulation = graph.createResource(
            "transparency.oit.accumulation",
            imageDesc(RenderGraph::Format::Rgba16Float, sceneExtent));
        weightedOitRevealage = graph.createResource(
            "transparency.oit.revealage",
            imageDesc(RenderGraph::Format::R16Float, sceneExtent));
    }
    RenderGraph::ResourceHandle output = graph.createResource("output.display",
        imageDesc(toGraphFormat(outputFormat), sceneExtent));
    RenderGraph::ResourceHandle uiComposition{};
    if (hdr10Composition) {
        uiComposition = graph.createResource("output.ui-composition",
            imageDesc(RenderGraph::Format::Rgba16Float, presentationExtent));
    }

    RenderGraph::ResourceDesc swapchainDesc = imageDesc(
        toGraphFormat(swapchainFormat), presentationExtent,
        RenderGraph::ResourceLifetime::External);
    swapchainDesc.imported = true;
    swapchainDesc.initialAccess = Access::Present;
    RenderGraph::ResourceHandle swapchain = graph.createResource(
        "swapchain", swapchainDesc);

    RenderGraph::ResourceHandle virtualWorking{};
    if (virtualShadowWorkingSetBytes) {
        auto desc = bufferDesc(virtualShadowWorkingSetBytes, 16);
        desc.lifetime = RenderGraph::ResourceLifetime::External;
        desc.imported = true; desc.initialAccess = Access::TransferDestination;
        virtualWorking = graph.createResource("shadow.virtual.working-set", desc);
        const auto upload = graph.addPass("shadow.virtual.clip-upload");
        virtualWorking = graph.write(upload, virtualWorking, Access::TransferDestination);
    }
    // M7R R3b.7: GPU-driven compaction producers. Each view's per-slot
    // indirect command and count buffers are imported (owned by its culler,
    // capacity grows, so variableSize); the compact pass writes them and the
    // drawing pass reads them as indirect arguments, so the compute ->
    // indirect dependency is an executor barrier. Host -> compute barriers stay
    // inside the compact passes. Declared directly before their consumers so
    // the compiled order keeps the previous order as a subsequence.
    const auto indirectBuffer = [&](std::string name) {
        RenderGraph::ResourceDesc desc = bufferDesc(sizeof(uint32_t));
        desc.lifetime = RenderGraph::ResourceLifetime::External;
        desc.imported = true;
        desc.initialAccess = Access::IndirectRead;
        desc.buffer.variableSize = true;
        return graph.createResource(std::move(name), desc);
    };
    const auto addCompaction = [&](std::string view,
        RenderGraph::PassHandle& consumerPass, const char* consumerName) {
        RenderGraph::PassHandle compactPass{};
        RenderGraph::ResourceHandle commands =
            indirectBuffer(view + ".indirect-commands");
        RenderGraph::ResourceHandle counts =
            indirectBuffer(view + ".indirect-counts");
        const RenderGraph::PassHandle compact = graph.addPass(view + ".compact",
            RenderGraph::QueueClass::Compute);
        commands = graph.write(compact, commands, Access::StorageReadWrite);
        counts = graph.write(compact, counts, Access::StorageReadWrite);
        consumerPass = graph.addPass(consumerName);
        graph.read(consumerPass, commands, Access::IndirectRead);
        graph.read(consumerPass, counts, Access::IndirectRead);
        compactPass = compact;
        return compactPass;
    };

    RenderGraph::PassHandle directionalShadowPass{};
    addCompaction("shadow.directional", directionalShadowPass,
        "shadow.directional");
    directionalShadow = graph.write(directionalShadowPass,
        directionalShadow, Access::DepthAttachmentWrite, LoadOp::Clear,
        StoreOp::Store, farDepth);

    RenderGraph::PassHandle spotShadowPass{};
    addCompaction("shadow.spot", spotShadowPass, "shadow.spot");
    spotShadow = graph.write(spotShadowPass, spotShadow,
        Access::DepthAttachmentWrite, LoadOp::Load);

    RenderGraph::PassHandle pointShadowPass{};
    addCompaction("shadow.point", pointShadowPass, "shadow.point");
    for (RenderGraph::ResourceHandle& pointShadow : pointShadows)
        pointShadow = graph.write(pointShadowPass, pointShadow,
            Access::DepthAttachmentWrite, LoadOp::Load);

    // M7R R3b.8: reflection-probe capture lights its faces with this frame's
    // shadow maps. Its staging targets and per-face indirect buffers vary per
    // capture, so they stay owner-managed (barriers inside the pass) rather
    // than graph resources.
    const RenderGraph::PassHandle probeCapture = graph.addPass("probe.capture");
    graph.read(probeCapture, directionalShadow, Access::SampledRead);
    graph.read(probeCapture, spotShadow, Access::SampledRead);
    for (const RenderGraph::ResourceHandle pointShadow : pointShadows)
        graph.read(probeCapture, pointShadow, Access::SampledRead);

    // M7R R3b.9: the Hi-Z depth-pyramid history (one image per retained
    // view, the current view's bound per frame) is an executor-owned global
    // import. The opaque compaction samples last frame's pyramid, the build
    // rewrites it (mip-to-mip barriers stay inside the pass) and the
    // validation hook copies it, so the executor issues the begin (to GENERAL)
    // and ready (to SHADER_READ_ONLY, now at the next reader) barriers.
    RenderGraph::ResourceHandle depthHistory{};
    if (depthPyramid) {
        RenderGraph::ResourceDesc historyDesc = imageDesc(
            RenderGraph::Format::R32Float, sceneExtent,
            RenderGraph::ResourceLifetime::External);
        historyDesc.image.mipLevels = static_cast<uint16_t>(depthPyramidMipCount(
            { sceneExtent.width, sceneExtent.height }));
        historyDesc.imported = true;
        historyDesc.initialAccess = Access::SampledRead;
        depthHistory = graph.createResource("depth.occlusion-pyramid.history",
            historyDesc);
    }

    RenderGraph::PassHandle gbuffer{};
    const RenderGraph::PassHandle opaqueCompact =
        addCompaction("gpu-scene.opaque", gbuffer, "gbuffer");
    if (depthPyramid)
        graph.read(opaqueCompact, depthHistory, Access::SampledRead);
    normal = graph.write(gbuffer, normal, Access::ColorAttachment, LoadOp::Clear,
        StoreOp::Store, opaqueBlack);
    albedo = graph.write(gbuffer, albedo, Access::ColorAttachment, LoadOp::Clear,
        StoreOp::Store, opaqueBlack);
    emissive = graph.write(gbuffer, emissive, Access::ColorAttachment, LoadOp::Clear,
        StoreOp::Store, transparentBlack);
    f0Roughness = graph.write(gbuffer, f0Roughness,
        Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, opaqueBlack);
    materialFlags = graph.write(gbuffer, materialFlags,
        Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, zeroUint);
    depth = graph.write(gbuffer, depth, Access::DepthAttachmentWrite, LoadOp::Clear,
        StoreOp::Store, farDepth);

    // Reflection-probe clustering (R3b.7): per-slot imported header/index
    // buffers written by compute and read by every lit consumer through
    // readClusterProduct; the compute -> fragment dependency is an executor
    // barrier.
    const auto probeClusterBuffer = [&](std::string name) {
        RenderGraph::ResourceDesc desc = bufferDesc(sizeof(uint32_t));
        desc.lifetime = RenderGraph::ResourceLifetime::External;
        desc.imported = true;
        desc.initialAccess = Access::StorageRead;
        desc.buffer.variableSize = true;
        return graph.createResource(std::move(name), desc);
    };
    RenderGraph::ResourceHandle probeClusterHeaders =
        probeClusterBuffer("lighting.probe-cluster.headers");
    RenderGraph::ResourceHandle probeClusterIndices =
        probeClusterBuffer("lighting.probe-cluster.indices");
    const RenderGraph::PassHandle probeCluster = graph.addPass(
        "lighting.probe-cluster", RenderGraph::QueueClass::Compute);
    probeClusterHeaders = graph.write(probeCluster, probeClusterHeaders,
        Access::StorageWrite);
    probeClusterIndices = graph.write(probeCluster, probeClusterIndices,
        Access::StorageWrite);

    const RenderGraph::PassHandle clusterClear = graph.addPass(
        "lighting.cluster.clear", RenderGraph::QueueClass::Compute);
    clusterCounts = graph.write(clusterClear, clusterCounts,
        Access::StorageWrite);
    clusterCursors = graph.write(clusterClear, clusterCursors,
        Access::StorageWrite);
    clusterGlobal = graph.write(clusterClear, clusterGlobal,
        Access::StorageWrite);
    clusterFallback = graph.write(clusterClear, clusterFallback,
        Access::StorageWrite);
    clusterDiagnostics = graph.write(clusterClear, clusterDiagnostics,
        Access::StorageWrite);
    clusterIndirect = graph.write(clusterClear, clusterIndirect,
        Access::StorageWrite);

    const RenderGraph::PassHandle clusterCountPass = graph.addPass(
        "lighting.cluster.count", RenderGraph::QueueClass::Compute);
    clusterCounts = graph.write(clusterCountPass, clusterCounts,
        Access::StorageReadWrite);
    clusterGlobal = graph.write(clusterCountPass, clusterGlobal,
        Access::StorageReadWrite);
    clusterDiagnostics = graph.write(clusterCountPass, clusterDiagnostics,
        Access::StorageReadWrite);

    const RenderGraph::PassHandle clusterScan = graph.addPass(
        "lighting.cluster.scan", RenderGraph::QueueClass::Compute);
    graph.read(clusterScan, clusterCounts, Access::StorageRead);
    clusterHeaders = graph.write(clusterScan, clusterHeaders,
        Access::StorageWrite);
    clusterScanScratch = graph.write(clusterScan, clusterScanScratch,
        Access::StorageWrite);
    clusterDiagnostics = graph.write(clusterScan, clusterDiagnostics,
        Access::StorageReadWrite);

    const RenderGraph::PassHandle clusterFill = graph.addPass(
        "lighting.cluster.fill", RenderGraph::QueueClass::Compute);
    graph.read(clusterFill, clusterHeaders, Access::StorageRead);
    clusterCursors = graph.write(clusterFill, clusterCursors,
        Access::StorageReadWrite);
    clusterIndices = graph.write(clusterFill, clusterIndices,
        Access::StorageWrite);
    clusterCounts = graph.write(clusterFill, clusterCounts,
        Access::StorageWrite);
    clusterIndirect = graph.write(clusterFill, clusterIndirect,
        Access::StorageReadWrite);
    clusterDiagnostics = graph.write(clusterFill, clusterDiagnostics,
        Access::StorageReadWrite);

    const RenderGraph::PassHandle clusterFinalize = graph.addPass(
        "lighting.cluster.finalize", RenderGraph::QueueClass::Compute);
    clusterHeaders = graph.write(clusterFinalize, clusterHeaders,
        Access::StorageReadWrite);
    clusterIndices = graph.write(clusterFinalize, clusterIndices,
        Access::StorageReadWrite);
    clusterFallback = graph.write(clusterFinalize, clusterFallback,
        Access::StorageReadWrite);
    clusterDiagnostics = graph.write(clusterFinalize, clusterDiagnostics,
        Access::StorageReadWrite);
    graph.read(clusterFinalize, clusterCounts, Access::StorageRead);
    graph.read(clusterFinalize, clusterIndirect, Access::IndirectRead);

    if (features.clusterTelemetryReadback) {
        const RenderGraph::PassHandle clusterReadbackPass = graph.addPass(
            "lighting.cluster.readback", RenderGraph::QueueClass::Transfer);
        graph.read(clusterReadbackPass, clusterDiagnostics,
            Access::TransferSource);
        clusterReadback = graph.write(clusterReadbackPass, clusterReadback,
            Access::TransferDestination);
    }

    const auto readClusterProduct = [&](RenderGraph::PassHandle pass) {
        graph.read(pass, directionalShadow, Access::SampledRead);
        graph.read(pass, spotShadow, Access::SampledRead);
        for (const RenderGraph::ResourceHandle pointShadow : pointShadows)
            graph.read(pass, pointShadow, Access::SampledRead);
        graph.read(pass, clusterGlobal, Access::StorageRead);
        graph.read(pass, clusterHeaders, Access::StorageRead);
        graph.read(pass, clusterIndices, Access::StorageRead);
        graph.read(pass, clusterFallback, Access::StorageRead);
        graph.read(pass, clusterDiagnostics, Access::StorageRead);
        graph.read(pass, probeClusterHeaders, Access::StorageRead);
        graph.read(pass, probeClusterIndices, Access::StorageRead);
    };

    const RenderGraph::PassHandle lighting = graph.addPass("lighting");
    graph.read(lighting, normal, Access::SampledRead);
    graph.read(lighting, albedo, Access::SampledRead);
    graph.read(lighting, emissive, Access::SampledRead);
    graph.read(lighting, f0Roughness, Access::SampledRead);
    graph.read(lighting, materialFlags, Access::SampledRead);
    graph.read(lighting, depth, Access::SampledRead);
    readClusterProduct(lighting);
    litScene = graph.write(lighting, litScene, Access::ColorAttachment, LoadOp::Clear,
        StoreOp::Store, opaqueBlack);

    const RenderGraph::PassHandle opaqueForward =
        graph.addPass("forward-opaque");
    readClusterProduct(opaqueForward);
    depth = graph.write(opaqueForward, depth,
        Access::DepthAttachmentWrite, LoadOp::Load);
    litScene = graph.write(opaqueForward, litScene,
        Access::ColorAttachment, LoadOp::Load);

    if (virtualShadowWorkingSetBytes) {
        const auto mark = graph.addPass("shadow.virtual.depth-mark", RenderGraph::QueueClass::Compute);
        graph.read(mark, depth, Access::SampledRead);
        virtualWorking = graph.write(mark, virtualWorking, Access::StorageReadWrite);
        const auto readback = graph.addPass("shadow.virtual.request-readback", RenderGraph::QueueClass::Transfer);
        graph.read(readback, virtualWorking, Access::TransferSource);
        // Explicit qualification can copy the exact depth consumed above.
        if (features.hooks.virtualShadowDepthSnapshot)
            graph.read(readback, depth, Access::TransferSource);
        graph.exportResource(virtualWorking, Access::TransferSource);
    }
    if (transparencyPyramids) {
        const RenderGraph::PassHandle pyramidBuild =
            graph.addPass("transparent.refraction-pyramids",
                RenderGraph::QueueClass::Compute);
        graph.read(pyramidBuild, litScene, Access::SampledRead);
        graph.read(pyramidBuild, depth, Access::SampledRead);
        refractionColor = graph.write(pyramidBuild, refractionColor,
            Access::StorageReadWrite);
        refractionDepth = graph.write(pyramidBuild, refractionDepth,
            Access::StorageReadWrite);
    }

    RenderGraph::PassHandle depthPyramidValidation{};
    if (depthPyramid) {
        const auto build = graph.addPass("depth.occlusion-pyramid.build", RenderGraph::QueueClass::Compute);
        graph.read(build, depth, Access::SampledRead);
        depthHistory = graph.write(build, depthHistory, Access::StorageReadWrite);
        if (features.hooks.depthPyramidValidation) {
            depthPyramidValidation = graph.addPass(
                "depth.occlusion-pyramid.validation-readback-hook",
                RenderGraph::QueueClass::Transfer);
            graph.read(depthPyramidValidation, depth, Access::TransferSource);
            graph.read(depthPyramidValidation, depthHistory, Access::TransferSource);
            graph.addDependency(build, depthPyramidValidation);
        }
    }

    const RenderGraph::PassHandle sortedTransparency =
        graph.addPass("transparent.sorted.forward");
    if (depthPyramidValidation.isValid())
        graph.addDependency(depthPyramidValidation, sortedTransparency);
    readClusterProduct(sortedTransparency);
    graph.read(sortedTransparency, depth, Access::DepthAttachmentRead);
    litScene = graph.write(sortedTransparency, litScene,
        Access::ColorAttachment, LoadOp::Load);

    if (layered.enabled(TransparencyQuality::Ordinary2)) {
        // Entry binds opaque depth and the integer material-flags GBuffer as valid
        // dynamically unused previous-interface descriptors. Exit replaces those
        // bindings with the entry capture products.
        const RenderGraph::PassHandle entryCapture = graph.addPass(
            "transparent.layered.entry.capture");
        graph.read(entryCapture, depth, Access::SampledRead);
        graph.read(entryCapture, materialFlags, Access::SampledRead);
        layeredEntryDepth = graph.write(entryCapture, layeredEntryDepth,
            Access::DepthAttachmentWrite, LoadOp::Clear, StoreOp::Store, farDepth);
        layeredEntryIdentity = graph.write(entryCapture,
            layeredEntryIdentity, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, zeroUint);

        const RenderGraph::PassHandle exitCapture = graph.addPass(
            "transparent.layered.exit.capture");
        graph.read(exitCapture, depth, Access::SampledRead);
        graph.read(exitCapture, layeredEntryDepth, Access::SampledRead);
        graph.read(exitCapture, layeredEntryIdentity, Access::SampledRead);
        layeredExitDepth = graph.write(exitCapture, layeredExitDepth,
            Access::DepthAttachmentWrite, LoadOp::Clear, StoreOp::Store, farDepth);
        layeredExitIdentity = graph.write(exitCapture,
            layeredExitIdentity, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, zeroUint);

        const RenderGraph::PassHandle localComposition = graph.addPass(
            "transparent.layered.local-compose");
        readClusterProduct(localComposition);
        graph.read(localComposition, depth, Access::SampledRead);
        graph.read(localComposition, refractionColor,
            Access::SampledRead);
        graph.read(localComposition, refractionDepth,
            Access::SampledRead);
        graph.read(localComposition, layeredEntryDepth,
            Access::SampledRead);
        graph.read(localComposition, layeredEntryIdentity,
            Access::SampledRead);
        graph.read(localComposition, layeredExitDepth,
            Access::SampledRead);
        graph.read(localComposition, layeredExitIdentity,
            Access::SampledRead);
        layeredLocalColor = graph.write(localComposition, layeredLocalColor,
            Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, transparentBlack);

        // The explicit one-shot diagnostic validates both paired interfaces and
        // their evaluated local AP1 result after composition has completed.
        if (features.hooks.layeredValidation) {
            const RenderGraph::PassHandle validationReadback = graph.addPass(
                "transparent.layered.validation-readback-hook",
                RenderGraph::QueueClass::Transfer);
            graph.read(validationReadback, layeredEntryDepth,
                Access::TransferSource);
            graph.read(validationReadback, layeredEntryIdentity,
                Access::TransferSource);
            graph.read(validationReadback, layeredExitDepth,
                Access::TransferSource);
            graph.read(validationReadback, layeredExitIdentity,
                Access::TransferSource);
            graph.read(validationReadback, layeredLocalColor,
                Access::TransferSource);
        }

        const RenderGraph::PassHandle compositionContract = graph.addPass(
            "transparent.layered.compose-hook");
        graph.read(compositionContract, layeredLocalColor,
            Access::SampledRead);
        graph.read(compositionContract, layeredEntryIdentity,
            Access::SampledRead);
        graph.read(compositionContract, depth,
            Access::DepthAttachmentRead);
        litScene = graph.write(compositionContract, litScene,
            Access::ColorAttachment, LoadOp::Load);
    }

    const auto addDeepLayeredTierPasses = [&](DeepLayeredTierResources& tier) {
        if (tier.interfaceCount == 0u) return;
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < tier.interfaceCount; ++interfaceIndex) {
            const RenderGraph::PassHandle capture = graph.addPass(
                "transparent.layered." + tier.name + ".interface." +
                    std::to_string(interfaceIndex) + ".capture");
            graph.read(capture, depth, Access::SampledRead);
            if (interfaceIndex == 0u) {
                graph.read(capture, materialFlags, Access::SampledRead);
            }
            else {
                graph.read(capture, tier.depth[interfaceIndex - 1u],
                    Access::SampledRead);
                graph.read(capture, tier.identity[interfaceIndex - 1u],
                    Access::SampledRead);
                if (interfaceIndex >= 2u) {
                    graph.read(capture, tier.tileTermination[
                        deepLayeredPriorTerminationInterface(interfaceIndex)],
                        Access::SampledRead);
                }
            }
            tier.depth[interfaceIndex] = graph.write(capture,
                tier.depth[interfaceIndex], Access::DepthAttachmentWrite,
                LoadOp::Clear, StoreOp::Store, farDepth);
            tier.identity[interfaceIndex] = graph.write(capture,
                tier.identity[interfaceIndex], Access::ColorAttachment,
                LoadOp::Clear, StoreOp::Store, zeroUint);

            if (deepLayeredTerminationInterface(interfaceIndex,
                    tier.interfaceCount)) {
                const RenderGraph::PassHandle termination = graph.addPass(
                    "transparent.layered." + tier.name + ".interface." +
                        std::to_string(interfaceIndex) + ".terminate-tiles",
                    RenderGraph::QueueClass::Compute);
                graph.read(termination, tier.identity[interfaceIndex],
                    Access::SampledRead);
                tier.tileTermination[interfaceIndex] = graph.write(
                    termination, tier.tileTermination[interfaceIndex],
                    Access::StorageWrite, LoadOp::DontCare);
            }
        }

        const RenderGraph::PassHandle localComposition = graph.addPass(
            "transparent.layered." + tier.name + ".local-compose");
        readClusterProduct(localComposition);
        graph.read(localComposition, depth, Access::SampledRead);
        graph.read(localComposition, refractionColor, Access::SampledRead);
        graph.read(localComposition, refractionDepth, Access::SampledRead);
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < tier.interfaceCount; ++interfaceIndex) {
            graph.read(localComposition, tier.depth[interfaceIndex],
                Access::SampledRead);
            graph.read(localComposition, tier.identity[interfaceIndex],
                Access::SampledRead);
        }
        tier.localColor = graph.write(localComposition, tier.localColor,
            Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, transparentBlack);

        if (!features.hooks.layeredValidation) return;
        const RenderGraph::PassHandle validationReadback = graph.addPass(
            "transparent.layered." + tier.name +
                ".validation-readback-hook",
            RenderGraph::QueueClass::Transfer);
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < tier.interfaceCount; ++interfaceIndex) {
            graph.read(validationReadback, tier.depth[interfaceIndex],
                Access::TransferSource);
            graph.read(validationReadback, tier.identity[interfaceIndex],
                Access::TransferSource);
            if (tier.tileTermination[interfaceIndex].isValid()) {
                graph.read(validationReadback,
                    tier.tileTermination[interfaceIndex],
                    Access::TransferSource);
            }
        }
        graph.read(validationReadback, tier.localColor,
            Access::TransferSource);

    };
    addDeepLayeredTierPasses(hero4);
    addDeepLayeredTierPasses(cinematic8);

    if (hero4.interfaceCount != 0u || cinematic8.interfaceCount != 0u) {
        const std::string compositionName = hero4.interfaceCount != 0u &&
                cinematic8.interfaceCount != 0u
            ? "transparent.layered.deep.compose-hook"
            : hero4.interfaceCount != 0u
                ? "transparent.layered.hero4.compose-hook"
                : "transparent.layered.cinematic8.compose-hook";
        const RenderGraph::PassHandle composition = graph.addPass(
            compositionName);
        const auto readTierResolveInputs = [&](const DeepLayeredTierResources& tier) {
            if (tier.interfaceCount == 0u) return;
            graph.read(composition, tier.localColor, Access::SampledRead);
            graph.read(composition, tier.identity[0], Access::SampledRead);
        };
        readTierResolveInputs(hero4);
        readTierResolveInputs(cinematic8);
        graph.read(composition, depth, Access::DepthAttachmentRead);
        litScene = graph.write(composition, litScene,
            Access::ColorAttachment, LoadOp::Load);
    }

    // A classified packet which misses a bounded specialist path retains
    // one local forward fallback. It never reconstructs the retired
    // two-bucket glass-depth approximation.
    const RenderGraph::PassHandle compatibilityForward =
        graph.addPass("transparent.compatibility.forward");
    readClusterProduct(compatibilityForward);
    if (transparencyPyramids) {
        graph.read(compatibilityForward, refractionColor,
            Access::SampledRead);
        graph.read(compatibilityForward, refractionDepth,
            Access::SampledRead);
    }
    depth = graph.write(compatibilityForward, depth,
        Access::DepthAttachmentWrite, LoadOp::Load);
    litScene = graph.write(compatibilityForward, litScene,
        Access::ColorAttachment, LoadOp::Load);

    if (layered.weightedOit) {
        // Explicit approximate work accumulates into independent FP16 color
        // and revealage products while testing the opaque depth read-only.
        // Resolve then publishes one premultiplied source-over operator into
        // the shared scene-referred HDR target.
        const RenderGraph::PassHandle accumulation = graph.addPass(
            "transparent.oit.accumulate");
        readClusterProduct(accumulation);
        graph.read(accumulation, depth, Access::DepthAttachmentRead);
        weightedOitAccumulation = graph.write(accumulation,
            weightedOitAccumulation, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, transparentBlack);
        weightedOitRevealage = graph.write(accumulation,
            weightedOitRevealage, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, ClearValue::color(1.0f, 0.0f, 0.0f, 0.0f));

        const RenderGraph::PassHandle resolve = graph.addPass(
            "transparent.oit.resolve");
        graph.read(resolve, weightedOitAccumulation, Access::SampledRead);
        graph.read(resolve, weightedOitRevealage, Access::SampledRead);
        litScene = graph.write(resolve, litScene,
            Access::ColorAttachment, LoadOp::Load);
    }

    // Scene-linear capture copies (R3b.5) read the final scene colour after
    // its last writer and before bloom; output-transform's begin returns it
    // to SampledRead.
    if (features.hooks.sceneColorCapture) {
        const RenderGraph::PassHandle sceneCapture = graph.addPass(
            "scene-color-capture-hook", RenderGraph::QueueClass::Transfer);
        graph.read(sceneCapture, litScene, Access::TransferSource);
    }

    const RenderGraph::PassHandle bloomHook = graph.addPass("bloom-hook");
    graph.read(bloomHook, litScene, Access::SampledRead);

    const RenderGraph::PassHandle outputTransform =
        graph.addPass("output-transform");
    graph.read(outputTransform, litScene, Access::SampledRead);
    graph.read(outputTransform, emissive, Access::SampledRead);
    graph.read(outputTransform, depth, Access::SampledRead);
    output = graph.write(outputTransform, output,
        Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, opaqueBlack);

    const RenderGraph::PassHandle finalCaptureHook =
        graph.addPass("final-capture-hook");
    graph.read(finalCaptureHook, litScene, Access::TransferSource);
    graph.read(finalCaptureHook, output, Access::TransferSource);

    const RenderGraph::PassHandle ui = graph.addPass(
        hdr10Composition ? "ui-compose" : "ui-present");
    graph.read(ui, output, Access::SampledRead);
    if (hdr10Composition) {
        uiComposition = graph.write(ui, uiComposition,
            Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, opaqueBlack);
        const RenderGraph::PassHandle encode = graph.addPass(
            "hdr10-encode-present");
        graph.read(encode, uiComposition, Access::SampledRead);
        swapchain = graph.write(encode, swapchain,
            Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, opaqueBlack);
    }
    else {
        swapchain = graph.write(ui, swapchain,
            Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, opaqueBlack);
    }
    graph.exportResource(swapchain, Access::Present);

    RenderGraph::CompileResult result = graph.compile();
    if (!result.succeeded()) {
        std::ostringstream message;
        message << "Production render graph failed to compile";
        for (const RenderGraph::GraphDiagnostic& diagnostic : result.diagnostics) {
            message << ": " << diagnostic.message;
        }
        throw std::runtime_error(message.str());
    }
    return std::move(*result.graph);
}

VulkanProductionGraphIds resolveVulkanProductionGraphIds(
    const VulkanRenderGraphExecutor& graph) {
    const auto pass = [&](std::string_view name) { return graph.findPass(name); };
    const auto resource = [&](std::string_view name) {
        return graph.findResource(name);
    };
    VulkanProductionGraphIds ids{};
    const auto producer = [&](const std::string& view) {
        return VulkanIndirectProducerGraphIds{ pass(view + ".compact"),
            resource(view + ".indirect-commands"), resource(view + ".indirect-counts") };
    };
    ids.virtualShadowClipUpload = pass("shadow.virtual.clip-upload");
    ids.directionalIndirect = producer("shadow.directional");
    ids.shadowDirectional = pass("shadow.directional");
    ids.spotIndirect = producer("shadow.spot");
    ids.shadowSpot = pass("shadow.spot");
    ids.pointIndirect = producer("shadow.point");
    ids.shadowPoint = pass("shadow.point");
    ids.probeCapture = pass("probe.capture");
    ids.opaqueIndirect = producer("gpu-scene.opaque");
    ids.gbuffer = pass("gbuffer");
    ids.probeCluster = pass("lighting.probe-cluster");
    ids.probeClusterHeaders = resource("lighting.probe-cluster.headers");
    ids.probeClusterIndices = resource("lighting.probe-cluster.indices");
    ids.cluster = {
        .clear = pass("lighting.cluster.clear"),
        .count = pass("lighting.cluster.count"),
        .scan = pass("lighting.cluster.scan"),
        .fill = pass("lighting.cluster.fill"),
        .finalize = pass("lighting.cluster.finalize"),
        .global = resource(kClusterGlobalResourceName),
        .headers = resource(kClusterHeaderResourceName),
        .indices = resource(kClusterIndexResourceName),
        .fallback = resource(kClusterFallbackResourceName),
        .diagnostics = resource(kClusterDiagnosticResourceName),
        .counts = resource(kClusterCountResourceName),
        .cursors = resource(kClusterCursorResourceName),
        .scanScratch = resource(kClusterScanScratchResourceName),
        .indirect = resource(kClusterIndirectResourceName),
    };
    ids.clusterReadback = pass("lighting.cluster.readback");
    ids.lighting = pass("lighting");
    ids.forwardOpaque = pass("forward-opaque");
    ids.virtualShadowDepthMark = pass("shadow.virtual.depth-mark");
    ids.virtualShadowRequestReadback = pass("shadow.virtual.request-readback");
    ids.refractionPyramids = pass("transparent.refraction-pyramids");
    ids.depthPyramidBuild = pass("depth.occlusion-pyramid.build");
    ids.depthPyramidValidationHook =
        pass("depth.occlusion-pyramid.validation-readback-hook");
    ids.sortedForward = pass("transparent.sorted.forward");
    ids.ordinary2EntryCapture = pass("transparent.layered.entry.capture");
    ids.ordinary2ExitCapture = pass("transparent.layered.exit.capture");
    ids.ordinary2LocalCompose = pass("transparent.layered.local-compose");
    ids.ordinary2ValidationHook =
        pass("transparent.layered.validation-readback-hook");
    ids.ordinary2ComposeHook = pass("transparent.layered.compose-hook");
    const auto deepTier = [&](std::string_view tier) {
        VulkanDeepLayeredGraphIds result{};
        const std::string prefix = "transparent.layered." + std::string(tier);
        for (uint32_t index = 0u;
            index < VulkanDeepLayeredGraphIds::MaximumInterfaces; ++index) {
            const std::string interfaceName = std::string(tier) + ".interface." +
                std::to_string(index);
            const std::string passPrefix = prefix + ".interface." +
                std::to_string(index);
            result.interfaceCapture[index] = pass(passPrefix + ".capture");
            result.terminateTiles[index] = pass(passPrefix + ".terminate-tiles");
            result.interfaceDepth[index] = resource("depth.layered." + interfaceName);
            result.interfaceIdentity[index] =
                resource("identity.layered." + interfaceName);
            result.tileTermination[index] =
                resource("termination.layered." + interfaceName);
        }
        result.localCompose = pass(prefix + ".local-compose");
        result.validationReadbackHook = pass(prefix + ".validation-readback-hook");
        result.localColor = resource("scene.layered." + std::string(tier) +
            ".local-color");
        return result;
    };
    ids.hero4 = deepTier("hero4");
    ids.cinematic8 = deepTier("cinematic8");
    for (const std::string_view name : { "transparent.layered.deep.compose-hook",
            "transparent.layered.hero4.compose-hook",
            "transparent.layered.cinematic8.compose-hook" }) {
        if (const RenderGraph::PassId id = pass(name); id.isValid())
            ids.deepComposeHook = id;
    }
    ids.compatibilityForward = pass("transparent.compatibility.forward");
    ids.oitAccumulate = pass("transparent.oit.accumulate");
    ids.oitResolve = pass("transparent.oit.resolve");
    ids.sceneColorCaptureHook = pass("scene-color-capture-hook");
    ids.bloomHook = pass("bloom-hook");
    ids.outputTransform = pass("output-transform");
    ids.finalCaptureHook = pass("final-capture-hook");
    ids.ui = pass("ui-compose");
    if (!ids.ui.isValid()) ids.ui = pass("ui-present");
    ids.hdr10EncodePresent = pass("hdr10-encode-present");

    ids.swapchain = resource("swapchain");
    ids.shadowDirectionalMap = resource("shadow.directional");
    ids.shadowSpotMap = resource("shadow.spot");
    ids.shadowPointMaps = { resource("shadow.point.256"),
        resource("shadow.point.512"), resource("shadow.point.1024") };
    ids.virtualShadowWorkingSet = resource("shadow.virtual.working-set");
    ids.depthPyramidHistory = resource("depth.occlusion-pyramid.history");
    ids.gbufferNormal = resource("gbuffer.normal");
    ids.gbufferAlbedo = resource("gbuffer.albedo");
    ids.gbufferEmissive = resource("gbuffer.emissive");
    ids.gbufferF0Roughness = resource("gbuffer.f0-roughness");
    ids.gbufferMaterialFlags = resource("gbuffer.material-flags");
    ids.depth = resource("depth.opaque");
    ids.sceneColor = resource("scene.color");
    ids.refractionColorPyramid = resource("scene.refraction-color-pyramid");
    ids.refractionDepthPyramid = resource("depth.refraction-nearest-pyramid");
    ids.ordinary2EntryDepth = resource("depth.layered.entry");
    ids.ordinary2EntryIdentity = resource("identity.layered.entry");
    ids.ordinary2ExitDepth = resource("depth.layered.exit");
    ids.ordinary2ExitIdentity = resource("identity.layered.exit");
    ids.ordinary2LocalColor = resource("scene.layered.local-color");
    ids.oitAccumulation = resource("transparency.oit.accumulation");
    ids.oitRevealage = resource("transparency.oit.revealage");
    ids.output = resource("output.display");
    ids.uiComposition = resource("output.ui-composition");
    return ids;
}

} // namespace Iridium
