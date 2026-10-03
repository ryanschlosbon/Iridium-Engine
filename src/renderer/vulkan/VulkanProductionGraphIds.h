#pragma once

// M7R R3b.4: execution-time identities of the production render graph.
// Resolved once per graph rebuild (resolveVulkanProductionGraphIds, next to the
// declarations in VulkanProductionRenderGraph.cpp) and cached by the backend and
// its passes; recording addresses passes and resources only through these ids.
// A pass or resource the current topology does not declare has an invalid id.

#include "renderer/graph/RenderGraph.h"

#include <array>
#include <cstdint>

namespace Iridium {

    // The clustered-light build (VulkanClusteredLightingPipeline).
    struct VulkanClusterGraphIds {
        RenderGraph::PassId clear;
        RenderGraph::PassId count;
        RenderGraph::PassId scan;
        RenderGraph::PassId fill;
        RenderGraph::PassId finalize;
        RenderGraph::GraphResourceId global;
        RenderGraph::GraphResourceId headers;
        RenderGraph::GraphResourceId indices;
        RenderGraph::GraphResourceId fallback;
        RenderGraph::GraphResourceId diagnostics;
        RenderGraph::GraphResourceId counts;
        RenderGraph::GraphResourceId cursors;
        RenderGraph::GraphResourceId scanScratch;
        RenderGraph::GraphResourceId indirect;
    };

    // One deep layered tier (Hero4: 4 interfaces, Cinematic8: 8).
    struct VulkanDeepLayeredGraphIds {
        static constexpr uint32_t MaximumInterfaces = 8u;
        std::array<RenderGraph::PassId, MaximumInterfaces> interfaceCapture{};
        std::array<RenderGraph::PassId, MaximumInterfaces> terminateTiles{};
        RenderGraph::PassId localCompose;
        RenderGraph::PassId validationReadbackHook;
        std::array<RenderGraph::GraphResourceId, MaximumInterfaces> interfaceDepth{};
        std::array<RenderGraph::GraphResourceId, MaximumInterfaces> interfaceIdentity{};
        std::array<RenderGraph::GraphResourceId, MaximumInterfaces> tileTermination{};
        RenderGraph::GraphResourceId localColor;
    };

    // A GPU-driven compaction producer (R3b.7): its compute pass and the
    // per-slot imported indirect command/count buffers its consumer reads.
    struct VulkanIndirectProducerGraphIds {
        RenderGraph::PassId compact;
        RenderGraph::GraphResourceId commands;
        RenderGraph::GraphResourceId counts;
    };

    struct VulkanProductionGraphIds {
        // Passes, in declaration order.
        RenderGraph::PassId virtualShadowClipUpload;
        VulkanIndirectProducerGraphIds directionalIndirect;
        RenderGraph::PassId shadowDirectional;
        VulkanIndirectProducerGraphIds spotIndirect;
        RenderGraph::PassId shadowSpot;
        VulkanIndirectProducerGraphIds pointIndirect;
        RenderGraph::PassId shadowPoint;
        RenderGraph::PassId probeCapture;
        VulkanIndirectProducerGraphIds opaqueIndirect;
        RenderGraph::PassId gbuffer;
        RenderGraph::PassId probeCluster;
        RenderGraph::GraphResourceId probeClusterHeaders;
        RenderGraph::GraphResourceId probeClusterIndices;
        VulkanClusterGraphIds cluster;
        RenderGraph::PassId clusterReadback;
        RenderGraph::PassId lighting;
        RenderGraph::PassId forwardOpaque;
        RenderGraph::PassId virtualShadowDepthMark;
        RenderGraph::PassId virtualShadowRequestReadback;
        RenderGraph::PassId refractionPyramids;
        RenderGraph::PassId depthPyramidBuild;
        RenderGraph::PassId depthPyramidValidationHook;
        RenderGraph::PassId sortedForward;
        RenderGraph::PassId ordinary2EntryCapture;
        RenderGraph::PassId ordinary2ExitCapture;
        RenderGraph::PassId ordinary2LocalCompose;
        RenderGraph::PassId ordinary2ValidationHook;
        RenderGraph::PassId ordinary2ComposeHook;
        VulkanDeepLayeredGraphIds hero4;
        VulkanDeepLayeredGraphIds cinematic8;
        // "transparent.layered.{deep,hero4,cinematic8}.compose-hook": the one
        // the resident tier set declares.
        RenderGraph::PassId deepComposeHook;
        RenderGraph::PassId compatibilityForward;
        RenderGraph::PassId oitAccumulate;
        RenderGraph::PassId oitResolve;
        RenderGraph::PassId sceneColorCaptureHook;
        RenderGraph::PassId bloomHook;
        RenderGraph::PassId outputTransform;
        RenderGraph::PassId finalCaptureHook;
        // "ui-compose" (HDR10 composition) or "ui-present".
        RenderGraph::PassId ui;
        RenderGraph::PassId hdr10EncodePresent;

        // Imported and per-frame resources the backend binds or reads.
        RenderGraph::GraphResourceId swapchain;
        RenderGraph::GraphResourceId shadowDirectionalMap;
        RenderGraph::GraphResourceId shadowSpotMap;
        // 256, 512 and 1024 pools.
        std::array<RenderGraph::GraphResourceId, 3> shadowPointMaps{};
        RenderGraph::GraphResourceId virtualShadowWorkingSet;
        RenderGraph::GraphResourceId gbufferNormal;
        RenderGraph::GraphResourceId gbufferAlbedo;
        RenderGraph::GraphResourceId gbufferEmissive;
        RenderGraph::GraphResourceId gbufferF0Roughness;
        RenderGraph::GraphResourceId gbufferMaterialFlags;
        RenderGraph::GraphResourceId depth;
        RenderGraph::GraphResourceId sceneColor;
        RenderGraph::GraphResourceId refractionColorPyramid;
        RenderGraph::GraphResourceId refractionDepthPyramid;
        RenderGraph::GraphResourceId ordinary2EntryDepth;
        RenderGraph::GraphResourceId ordinary2EntryIdentity;
        RenderGraph::GraphResourceId ordinary2ExitDepth;
        RenderGraph::GraphResourceId ordinary2ExitIdentity;
        RenderGraph::GraphResourceId ordinary2LocalColor;
        RenderGraph::GraphResourceId oitAccumulation;
        RenderGraph::GraphResourceId oitRevealage;
        RenderGraph::GraphResourceId output;
        RenderGraph::GraphResourceId uiComposition;
    };

} // namespace Iridium
