#include "renderer/vulkan/VulkanProductionRenderGraph.h"

#include "renderer/vulkan/VulkanRenderGraphExecutor.h"
#include "renderer/vulkan/VulkanGBufferLayout.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/ShadowTypes.h"

#include <limits>
#include <array>
#include <string>
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

} // namespace

RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(VkExtent2D extent,
    VkFormat swapchainFormat, VkFormat outputFormat, bool hdr10Composition,
    GBufferLayout gBufferLayout, ClusterGridConfig clusterConfig,
    uint32_t directionalShadowResolution,
    uint32_t spotShadowAtlasResolution) {
    return buildVulkanProductionRenderGraph(extent, extent,
        swapchainFormat, outputFormat, hdr10Composition, gBufferLayout,
        clusterConfig, directionalShadowResolution,
        spotShadowAtlasResolution);
}

RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
    VkExtent2D sceneExtent, VkExtent2D presentationExtent,
    VkFormat swapchainFormat, VkFormat outputFormat, bool hdr10Composition,
    GBufferLayout gBufferLayout, ClusterGridConfig clusterConfig,
    uint32_t directionalShadowResolution,
    uint32_t spotShadowAtlasResolution) {
    if (sceneExtent.width == 0 || sceneExtent.height == 0 ||
        presentationExtent.width == 0 || presentationExtent.height == 0) {
        throw std::invalid_argument("Production render graph requires a non-empty extent");
    }
    if (directionalShadowResolution == 0 || spotShadowAtlasResolution == 0)
        throw std::invalid_argument(
            "Production render graph requires nonzero shadow resolutions");

    RenderGraph::RenderGraphBuilder graph;
    using RenderGraph::Access;
    using RenderGraph::LoadOp;

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
    RenderGraph::ResourceDesc clusterReadbackDesc = bufferDesc(64, 16);
    clusterReadbackDesc.lifetime = RenderGraph::ResourceLifetime::External;
    clusterReadbackDesc.imported = true;
    clusterReadbackDesc.initialAccess = Access::TransferDestination;
    RenderGraph::ResourceHandle clusterReadback = graph.createResource(
        "lighting.cluster.diagnostics-readback", clusterReadbackDesc);
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
    constexpr std::array<uint32_t, 3> PointCapacities{
        kPointShadowPool256Capacity, kPointShadowPool512Capacity,
        kPointShadowPool1024Capacity };
    for (uint32_t tier = 0; tier < pointShadows.size(); ++tier) {
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
    RenderGraph::ResourceHandle opaqueCopy = graph.createResource("scene.opaque-copy",
        imageDesc(RenderGraph::Format::Rgba16Float, sceneExtent));
    RenderGraph::ResourceHandle glassDepth = graph.createResource("depth.glass",
        imageDesc(RenderGraph::Format::D32Float, sceneExtent));
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

    const RenderGraph::PassHandle directionalShadowPass = graph.addPass(
        "shadow.directional");
    directionalShadow = graph.write(directionalShadowPass,
        directionalShadow, Access::DepthAttachmentWrite, LoadOp::Clear);

    const RenderGraph::PassHandle spotShadowPass = graph.addPass(
        "shadow.spot");
    spotShadow = graph.write(spotShadowPass, spotShadow,
        Access::DepthAttachmentWrite, LoadOp::Load);

    const RenderGraph::PassHandle pointShadowPass = graph.addPass(
        "shadow.point");
    for (RenderGraph::ResourceHandle& pointShadow : pointShadows)
        pointShadow = graph.write(pointShadowPass, pointShadow,
            Access::DepthAttachmentWrite, LoadOp::Load);

    const RenderGraph::PassHandle gbuffer = graph.addPass("gbuffer");
    normal = graph.write(gbuffer, normal, Access::ColorAttachment, LoadOp::Clear);
    albedo = graph.write(gbuffer, albedo, Access::ColorAttachment, LoadOp::Clear);
    emissive = graph.write(gbuffer, emissive, Access::ColorAttachment, LoadOp::Clear);
    f0Roughness = graph.write(gbuffer, f0Roughness,
        Access::ColorAttachment, LoadOp::Clear);
    materialFlags = graph.write(gbuffer, materialFlags,
        Access::ColorAttachment, LoadOp::Clear);
    depth = graph.write(gbuffer, depth, Access::DepthAttachmentWrite, LoadOp::Clear);

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

    const RenderGraph::PassHandle clusterReadbackPass = graph.addPass(
        "lighting.cluster.readback", RenderGraph::QueueClass::Transfer);
    graph.read(clusterReadbackPass, clusterDiagnostics, Access::TransferSource);
    clusterReadback = graph.write(clusterReadbackPass, clusterReadback,
        Access::TransferDestination);

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
    };

    const RenderGraph::PassHandle lighting = graph.addPass("lighting");
    graph.read(lighting, normal, Access::SampledRead);
    graph.read(lighting, albedo, Access::SampledRead);
    graph.read(lighting, emissive, Access::SampledRead);
    graph.read(lighting, f0Roughness, Access::SampledRead);
    graph.read(lighting, materialFlags, Access::SampledRead);
    graph.read(lighting, depth, Access::SampledRead);
    readClusterProduct(lighting);
    litScene = graph.write(lighting, litScene, Access::ColorAttachment, LoadOp::Clear);

    const RenderGraph::PassHandle opaqueForward =
        graph.addPass("forward-opaque");
    readClusterProduct(opaqueForward);
    depth = graph.write(opaqueForward, depth,
        Access::DepthAttachmentWrite, LoadOp::Load);
    litScene = graph.write(opaqueForward, litScene,
        Access::ColorAttachment, LoadOp::Load);

    const RenderGraph::PassHandle backgroundCopy =
        graph.addPass("transparent.background.copy");
    graph.read(backgroundCopy, litScene, Access::TransferSource);
    opaqueCopy = graph.write(backgroundCopy, opaqueCopy,
        Access::TransferDestination);

    const RenderGraph::PassHandle backgroundDepth =
        graph.addPass("transparent.background.depth");
    glassDepth = graph.write(backgroundDepth, glassDepth,
        Access::DepthAttachmentWrite, LoadOp::Clear);

    const RenderGraph::PassHandle backgroundForward =
        graph.addPass("transparent.background.forward");
    readClusterProduct(backgroundForward);
    graph.read(backgroundForward, opaqueCopy, Access::SampledRead);
    depth = graph.write(backgroundForward, depth,
        Access::DepthAttachmentWrite, LoadOp::Load);
    graph.read(backgroundForward, glassDepth, Access::SampledRead);
    litScene = graph.write(backgroundForward, litScene,
        Access::ColorAttachment, LoadOp::Load);

    const RenderGraph::PassHandle foregroundCopy =
        graph.addPass("transparent.foreground.copy");
    graph.read(foregroundCopy, litScene, Access::TransferSource);
    opaqueCopy = graph.write(foregroundCopy, opaqueCopy,
        Access::TransferDestination);

    const RenderGraph::PassHandle foregroundDepth =
        graph.addPass("transparent.foreground.depth");
    glassDepth = graph.write(foregroundDepth, glassDepth,
        Access::DepthAttachmentWrite, LoadOp::Clear);

    const RenderGraph::PassHandle foregroundForward =
        graph.addPass("transparent.foreground.forward");
    readClusterProduct(foregroundForward);
    graph.read(foregroundForward, opaqueCopy, Access::SampledRead);
    depth = graph.write(foregroundForward, depth,
        Access::DepthAttachmentWrite, LoadOp::Load);
    graph.read(foregroundForward, glassDepth, Access::SampledRead);
    litScene = graph.write(foregroundForward, litScene,
        Access::ColorAttachment, LoadOp::Load);

    const RenderGraph::PassHandle bloomHook = graph.addPass("bloom-hook");
    graph.read(bloomHook, litScene, Access::SampledRead);

    const RenderGraph::PassHandle outputTransform =
        graph.addPass("output-transform");
    graph.read(outputTransform, litScene, Access::SampledRead);
    graph.read(outputTransform, emissive, Access::SampledRead);
    output = graph.write(outputTransform, output,
        Access::ColorAttachment, LoadOp::Clear);

    const RenderGraph::PassHandle finalCaptureHook =
        graph.addPass("final-capture-hook");
    graph.read(finalCaptureHook, output, Access::TransferSource);

    const RenderGraph::PassHandle ui = graph.addPass(
        hdr10Composition ? "ui-compose" : "ui-present");
    graph.read(ui, output, Access::SampledRead);
    if (hdr10Composition) {
        uiComposition = graph.write(ui, uiComposition,
            Access::ColorAttachment, LoadOp::Clear);
        const RenderGraph::PassHandle encode = graph.addPass(
            "hdr10-encode-present");
        graph.read(encode, uiComposition, Access::SampledRead);
        swapchain = graph.write(encode, swapchain,
            Access::ColorAttachment, LoadOp::Clear);
    }
    else {
        swapchain = graph.write(ui, swapchain,
            Access::ColorAttachment, LoadOp::Clear);
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

} // namespace Iridium
