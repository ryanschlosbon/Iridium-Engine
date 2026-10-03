#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi/IRenderBackend.h"
#include "../rhi/ResourcePool.h"
#include "../rhi/GpuSceneUploadPlanner.h"
#include "../rhi/GpuSceneIndirect.h"
#include "../rhi/GpuSceneLod.h"

#include "VkContext.h"
#include "VkSwapchain.h"
#include "DescriptorAllocator.h"

// Pipelines & Passes
#include "VkGraphicsPipeline.h"
#include "VkLightingPipeline.h"
#include "VkRenderPass.h"
#include "VkForwardRenderPass.h"
#include "VkUIRenderPass.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceAllocator.h"
#include "VulkanCommandList.h"
#include "VulkanUploadContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanFrameTargets.h"
#include "VulkanSceneDescriptors.h"
#include "VulkanProductionRenderGraph.h"
#include "VulkanBackendExtension.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanIndirectViewCuller.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanTransparencyPyramid.h"
#include "VulkanDepthPyramid.h"
#include "VulkanLayeredInterfaceCapturePass.h"
#include "VulkanLayeredLocalCompositionPass.h"
#include "VulkanLayeredSceneResolvePass.h"
#include "VulkanWeightedOitPass.h"
#include "VulkanOutputPass.h"
#include "VulkanHdrEncodePass.h"
#include "VulkanIndexedTextureTable.h"
#include "VulkanClusteredLightingPipeline.h"
#include "VulkanReflectionProbePipeline.h"
#include "VulkanReflectionProbeCapturePass.h"
#include "VulkanReflectionProbeCaptureTargets.h"
#include "VulkanDirectionalShadowMap.h"
#include "VulkanSpotShadowAtlas.h"
#include "VulkanPointShadowPools.h"
#include "VulkanVirtualShadowResources.h"
#include "renderer/transparency/TransparencyPyramidResidency.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "renderer/transparency/Ordinary2Atlas.h"

#include "utils/DeletionQueue.h"
#include "core/BuildFeatures.h"

namespace Iridium {

    // ==============================================================================
    // THE INTERNAL PAYLOADS
    // These structs only exist inside the Backend. The ECS never sees them.
    // ==============================================================================

    struct VulkanGeometryPayload {
        VulkanBufferResource vertexBuffer;
        VulkanBufferResource indexBuffer;
        VulkanBufferResource arenaUInt16IndexBuffer;
        VulkanBufferResource arenaUInt32IndexBuffer;
        VkDeviceSize vertexOffset = 0;
        uint32_t indexCount = 0;
        IndexFormat indexFormat = IndexFormat::UInt32;
        bool arenaAllocation = false;
        bool ownsArenaBuffers = false;
    };

    struct VulkanTexturePayload {
        VulkanImageResource image;
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t samplerCacheIndex = UINT32_MAX;
        VkDescriptorSet imguiDescriptor = VK_NULL_HANDLE;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        uint32_t width = 0;
        uint32_t height = 0;
        bool retired = false;
    };

    struct VulkanMaterialPayload {
        PipelineHandle pipeline;
        PipelineHandle mirroredPipeline;
        RenderQueue renderQueue = RenderQueue::Opaque;
        PackedGpuMaterial packed{};
        uint64_t packedRevision = 0;
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedPackedRevisions{};
    };

    // ==============================================================================
    // THE CONCRETE BACKEND
    // ==============================================================================

    class VulkanVertexBackend : public IRenderBackend {
    private:
        struct FrameCounters {
            uint64_t drawOpaque = 0;
            uint64_t opaqueIndirectCommands = 0;
            uint64_t opaqueIndirectBins = 0;
            uint64_t opaqueIndirectFallbackPackets = 0;
            uint64_t opaqueIndirectFallbackReason = 0;
            uint64_t depthHistoryEligible = 0;
            uint64_t depthHistoryRejection = 0;
            uint64_t drawSelection = 0;
            uint64_t drawShadowDirectional = 0;
            uint64_t drawShadowDirectionalAlphaMask = 0;
            uint64_t shadowDirectionalCastersTested = 0;
            uint64_t shadowDirectionalCastersCulled = 0;
            uint64_t shadowDirectionalIndirectCommands = 0;
            uint64_t shadowDirectionalIndirectBins = 0;
            uint64_t shadowDirectionalDirectFallback = 0;
            uint64_t shadowDirectionalIndirectFallbackReason = 0;
            uint64_t shadowDirectionalMembershipCacheHit = 0;
            uint64_t drawShadowSpot = 0;
            uint64_t drawShadowSpotAlphaMask = 0;
            uint64_t shadowSpotCastersTested = 0;
            uint64_t shadowSpotCastersCulled = 0;
            uint64_t shadowSpotIndirectCommands = 0;
            uint64_t shadowSpotIndirectBins = 0;
            uint64_t shadowSpotDirectFallback = 0;
            uint64_t shadowSpotIndirectFallbackReason = 0;
            uint64_t shadowSpotMembershipCacheHit = 0;
            uint64_t drawShadowPoint = 0;
            uint64_t drawShadowPointAlphaMask = 0;
            uint64_t shadowPointCastersTested = 0;
            uint64_t shadowPointCastersCulled = 0;
            uint64_t shadowPointIndirectCommands = 0;
            uint64_t shadowPointIndirectBins = 0;
            uint64_t shadowPointDirectFallback = 0;
            uint64_t shadowPointIndirectFallbackReason = 0;
            uint64_t shadowPointMembershipCacheHit = 0;
            uint64_t drawLighting = 0;
            uint64_t drawOutput = 0;
            uint64_t drawTransparentDepth = 0;
            uint64_t drawTransparentForward = 0;
            uint64_t drawStandardForward = 0;
            uint64_t drawComplexForward = 0;
            uint64_t drawUnlitForward = 0;
            std::array<uint64_t, 8> complexLobeDraws{};
            uint64_t drawUi = 0;
            uint64_t dispatchRecorded = 0;
            uint64_t trianglesSubmitted = 0;
            uint64_t materialBinds = 0;
            uint64_t pipelineBinds = 0;
            uint64_t transparentBackgroundPackets = 0;
            uint64_t transparentForegroundPackets = 0;
            uint64_t transparentNonemptyBuckets = 0;
            uint64_t transparentSortedPackets = 0;
            uint64_t weightedOitPackets = 0;
            uint64_t weightedOitSortedFallbackPackets = 0;
            uint64_t weightedOitInstanceCapacityFallbackPackets = 0;
            uint64_t weightedOitInstances = 0;
            uint64_t weightedOitInstanceUploadBytes = 0;
            uint64_t drawWeightedOitAccumulation = 0;
            uint64_t drawWeightedOitResolve = 0;
            uint64_t transparencyPyramidBuilds = 0;
            uint64_t transparencyPyramidMipDispatches = 0;
            uint64_t transparencyPyramidTopologyRebuilds = 0;
            uint64_t transparencyPyramidTopologyRebuildFailures = 0;
            uint64_t transparencyPyramidFallbackFrames = 0;
            uint64_t ordinary2ProbeFrames = 0;
            uint64_t ordinary2CandidatePackets = 0;
            uint64_t ordinary2ProjectedPackets = 0;
            uint64_t ordinary2ProjectionCulledPackets = 0;
            uint64_t ordinary2InvalidBoundsFallbackPackets = 0;
            uint64_t ordinary2NearPlaneFallbackPackets = 0;
            uint64_t ordinary2UnsafeProjectionFallbackPackets = 0;
            uint64_t ordinary2RequestCapacityFallbackPackets = 0;
            uint64_t ordinary2AtlasAcceptedPackets = 0;
            uint64_t ordinary2AtlasAcceptedIslands = 0;
            uint64_t ordinary2AtlasRejectedPackets = 0;
            uint64_t ordinary2AtlasAllocatedTexels = 0;
            uint64_t ordinary2CapturePreparedDraws = 0;
            uint64_t ordinary2CapturePreparationFallbackPackets = 0;
            uint64_t ordinary2CaptureEntryDraws = 0;
            uint64_t ordinary2CaptureExitDraws = 0;
            uint64_t ordinary2LocalCompositionDraws = 0;
            uint64_t ordinary2SceneResolveDraws = 0;
            uint64_t deepLayeredCandidatePackets = 0;
            uint64_t deepLayeredProjectedPackets = 0;
            uint64_t deepLayeredAtlasAcceptedPackets = 0;
            uint64_t deepLayeredAtlasAcceptedIslands = 0;
            uint64_t deepLayeredAtlasRejectedPackets = 0;
            uint64_t deepLayeredCapturePreparedDraws = 0;
            uint64_t deepLayeredCapturePreparationFallbackPackets = 0;
            uint64_t deepLayeredInterfaceDraws = 0;
            uint64_t deepLayeredResidualProbeDraws = 0;
            uint64_t deepLayeredLocalCompositionDraws = 0;
            uint64_t deepLayeredSceneResolveDraws = 0;
            uint64_t uiUntrackedCallbacks = 0;
            uint64_t materialUniqueOverflow = 0;
            uint64_t pipelineUniqueOverflow = 0;
        };

        static constexpr size_t MaxUniqueResourcesPerFrame = 512;

        // --- 1. THE SUBSYSTEMS (Composition) ---
        // We moved all of these pointers out of Application.cpp and into here.
        std::unique_ptr<VkContext> vkContext;
        VulkanResourceAllocator resourceAllocator;
        VulkanUploadContext uploadContext;
        VulkanFrameScheduler scheduler;
        std::unique_ptr<VkSwapchain> vkSwapchain;
        DescriptorAllocator descriptorAllocator;
        VulkanPipelineLibrary pipelineLibrary;
        VulkanMeshLayouts meshLayouts;
        VulkanIndexedTextureTable indexedTextureTable_;

        // G-Buffer Pass (Opaque)
        std::unique_ptr<VkRenderPassWrapper> gBufferPass;
        std::unique_ptr<VkGraphicsPipeline> gBufferPipeline;

        // --- MISSING RAW IMAGE ARRAYS ---
        VulkanFrameTargets frameTargets;
        VulkanRenderGraphExecutor renderGraph_;
        VulkanTransparencyPyramid transparencyPyramid_;
        VulkanDepthPyramid depthPyramid_;
        bool depthPyramidEnabled_ = false;
        VulkanLayeredInterfaceCapturePass layeredInterfaceCapture_;
        VulkanLayeredLocalCompositionPass layeredLocalComposition_;
        VulkanLayeredSceneResolvePass layeredSceneResolve_;
        VulkanWeightedOitPass weightedOit_;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            weightedOitInstanceBuffers_{};
        uint32_t weightedOitInstanceCapacity_ = 0u;
        TransparencyPyramidResidency transparencyPyramidResidency_;
        TransparencyPyramidResidency ordinary2AtlasResidency_;
        TransparencyPyramidResidency hero4AtlasResidency_;
        TransparencyPyramidResidency cinematic8AtlasResidency_;
        TransparencyPyramidResidency weightedOitResidency_;
        VkExtent2D ordinary2AtlasExtent_{};
        VkExtent2D hero4AtlasExtent_{};
        VkExtent2D cinematic8AtlasExtent_{};
        Ordinary2RequestCollector ordinary2RequestCollector_;
        Ordinary2AtlasPlan ordinary2AtlasPlan_;
        Ordinary2CaptureDrawPlan ordinary2CaptureDrawPlan_;
        LayeredRequestCollector deepLayeredRequestCollector_;
        LayeredAtlasPlan deepLayeredAtlasPlan_;
        LayeredCaptureDrawPlan deepLayeredCaptureDrawPlan_;
        std::array<Ordinary2CaptureDraw, kOrdinary2MaximumWorkCount>
            ordinary2ResolvedDraws_{};
        uint32_t ordinary2ResolvedPacketCount_ = 0u;
        std::array<LayeredCaptureDraw, kOrdinary2MaximumWorkCount>
            deepResolvedDraws_{};
        uint32_t deepResolvedPacketCount_ = 0u;
        FrameTopologyPreparation frameTopologyPrewarm_{};

        // G-Buffer Raw Images

        // Lighting Pass Raw Images & Descriptors
        VulkanSceneDescriptors sceneDescriptors;
        VulkanClusteredLightingPipeline clusteredLighting_;
        VulkanReflectionProbePipeline reflectionProbePipeline_;
        VulkanReflectionProbeCapturePass reflectionProbeCapturePass_;
        VulkanReflectionProbeCaptureTargets reflectionProbeCaptureTargets_;
        VulkanDirectionalShadowMap directionalShadow_;
        VulkanSpotShadowAtlas spotShadow_;
        VulkanPointShadowPools pointShadow_;
        VulkanVirtualShadowResources virtualShadowResources_;
        DirectionalVirtualShadowClipPublisher virtualShadowClipPublisher_;
        std::optional<DirectionalVirtualShadowClipPlan> virtualShadowClipPlan_;
        std::array<std::optional<DirectionalVirtualShadowClipPlan>, VulkanFrameScheduler::FramesInFlight>
            virtualShadowFrameClipPlans_;
        std::vector<VirtualShadowCasterBounds> virtualShadowCasterBoundsScratch_;
        uint32_t virtualShadowClipPageSize_ = 128;
        std::array<VkImageView, VulkanFrameScheduler::FramesInFlight> virtualShadowDepthBindings_{};
        std::array<bool, VulkanFrameScheduler::FramesInFlight> virtualShadowReadbackPending_{};
        void collectVirtualShadowRequests(uint32_t slot);
        EnvironmentLightingHandles environmentLighting_;
        EnvironmentLightingSettings environmentLightingSettings_;
        TextureHandle neutralEnvironmentCube_;
        TextureHandle neutralEnvironmentBrdfLut_;

        struct CachedSampler {
            SamplerDesc desc{};
            VkSampler sampler = VK_NULL_HANDLE;
            uint32_t referenceCount = 0;
        };
        std::vector<CachedSampler> samplerCache_;

        // Translucency Pass Raw Images

        // Depth Pass

        // Deferred Lighting Pass
        VkRenderPass lightingRenderPass = VK_NULL_HANDLE;
        std::unique_ptr<VkLightingPipeline> lightingPipeline;

        // Translucency Passes

        std::unique_ptr<VkForwardRenderPass> forwardPass;
        std::unique_ptr<VkForwardRenderPass> transparentPass;

        VulkanOutputPass outputPass;
        VulkanHdrEncodePass hdrEncodePass;

        // UI Pass
        std::unique_ptr<VkUIRenderPass> uiPass;

        // --- IMGUI STATE ---
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> uiSceneTextures;
        std::array<VulkanImageResource, 2> retainedViewImages_{};
        std::array<VkDescriptorSet, 2> retainedViewDescriptors_{};
        VkSampler retainedViewSampler_ = VK_NULL_HANDLE;
        uint32_t retainedRenderView_ = 0;
        bool retainedViewsEnabled_ = false;
        void destroyRetainedViews();
        void publishRetainedView();
        std::vector<VkDescriptorSet> uiDepthTextures;
        std::vector<uint32_t> imguiFragmentShaderCode_;

        // Global Camera Data
        std::vector<VulkanBufferResource> uniformBuffers;
        std::vector<VkDescriptorSet> globalDescriptorSets;
        std::array<VkDescriptorSet, VulkanFrameScheduler::FramesInFlight>
            gpuSceneDescriptorSets_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            canonicalMaterialBuffers_{};
        uint32_t canonicalMaterialCapacity_ = 0;
        uint32_t canonicalMaterialMaximumCapacity_ = 0;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            lightRecordBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            gpuSceneTransformBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            gpuSceneInstanceBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            gpuScenePrimitiveBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            gpuSceneGeometryBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            opaqueIndirectCommandBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            opaqueIndirectCountBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            opaqueIndirectCandidateBuffers_{};
        // GPU-driven shadow/probe caster compaction (M7R R3a). The shared
        // 3-binding indirect set layout is owned here; each culler owns its
        // pipeline, sets, buffers, scratch and validation slots.
        VulkanCullerDevice cullerDevice_{};
        VkDescriptorSetLayout indirectCullerSetLayout_ = VK_NULL_HANDLE;
        VulkanIndirectViewCuller directionalCuller_;
        VulkanIndirectViewCuller spotCuller_;
        VulkanIndirectViewCuller pointCuller_;
        VulkanIndirectViewCuller probeCuller_;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            depthOcclusionQueryBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            depthOcclusionResultBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            depthOcclusionGpuSceneResultBuffers_{};
        std::array<VkDescriptorSet, VulkanFrameScheduler::FramesInFlight>
            gpuSceneCullDescriptorSets_{};
        // Shared across the ordered graphics queue, not replicated per frame.
        VulkanBufferResource mainOpaqueLodHistoryBuffer_{};
        GpuSceneLodHistory mainOpaqueLodHistory_;
        std::vector<uint8_t> opaqueIndirectSeenHistory_;
        VkDescriptorSetLayout gpuSceneCullSetLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout gpuSceneCullPipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline gpuSceneCullPipeline_ = VK_NULL_HANDLE;
        VkPipeline gpuSceneCullFallbackPipeline_ = VK_NULL_HANDLE;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            activeLightSlotBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            fallbackCandidateBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            clusterParameterBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            clusterDiagnosticReadbackBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeRecordBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeActiveSlotBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeParameterBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeClusterHeaderBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeClusterIndexBuffers_{};
        std::array<bool, VulkanFrameScheduler::FramesInFlight>
            clusterDiagnosticReadbackPending_{};
        std::array<uint32_t, VulkanFrameScheduler::FramesInFlight>
            submittedClusterCounts_{};
        ClusteredLightingTelemetry clusterTelemetry_{};
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedLightRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedActiveListRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedSpotShadowMappingRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedPointShadowMappingRevisions_{};
        std::vector<uint32_t> spotShadowDataSlots_;
        std::vector<uint32_t> pointShadowDataSlots_;
        using ResolvedShadowCaster = VulkanResolvedCaster;

        std::vector<uint32_t> spotShadowMappingScratch_;
        std::vector<uint32_t> pointShadowMappingScratch_;
        std::vector<ResolvedShadowCaster> shadowCasterScratch_;
        std::vector<uint8_t> directionalShadowCasterMaskScratch_;

        std::vector<PackedGpuLight> patchedLightRecordsScratch_;
        uint64_t spotShadowMappingRevision_ = 1;
        uint64_t pointShadowMappingRevision_ = 1;
        std::vector<uint32_t> fallbackSelectionScratch_;
        std::vector<LightRecordRange> lightUploadRanges_;
        uint32_t lightRecordCapacity_ = 0;
        uint32_t lightRecordMaximumCapacity_ = 0;
        GpuSceneCapacityRequirements gpuSceneCapacity_{};
        GpuSceneCapacityRequirements gpuSceneMaximumCapacity_{};
        GpuSceneCapacityRequirements gpuScenePublishedCounts_{};
        struct GpuSceneCpuMirror {
            std::vector<GpuSceneAffineTransform> transforms;
            std::vector<GpuSceneInstanceRecord> instances;
            std::vector<GpuScenePrimitiveRecord> primitives;
            std::vector<GpuSceneGeometryRecord> geometries;
            std::vector<GpuScenePrimitiveIdentity> primitiveIdentities;
        };
        [[nodiscard]] bool resolveGpuSceneCaster(uint32_t primitiveIndex,
            uint32_t consumerMask,
            ResolvedShadowCaster& caster) const noexcept;
        // The slot's CPU GPU-scene mirror with the published counts.
        [[nodiscard]] VulkanIndirectScene indirectScene(
            uint32_t frame) const noexcept;
        // Geometry/material/pipeline payload lookups for the indirect cullers.
        [[nodiscard]] VulkanIndirectAssetResolver indirectAssets() const noexcept;
        template<typename Visitor>
        void visitShadowCasters(const ShadowCasterSubmission& submission,
            Visitor&& visitor) const;
        template<typename Visitor>
        void visitReflectionProbeCasters(
            const ReflectionProbeCasterSubmission& submission,
            Visitor&& visitor) const;
        // Upload heaps can be uncached/write-combined. CPU validation must use
        // an owned mirror updated by the exact same per-context dirty ranges.
        std::array<GpuSceneCpuMirror, VulkanFrameScheduler::FramesInFlight> gpuSceneCpuMirrors_;
        std::array<ViewTransportRecord, VulkanFrameScheduler::FramesInFlight> gpuSceneCpuViews_;
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedGpuSceneTransformRevisions_{};
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedGpuSceneInstanceRevisions_{};
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedGpuScenePrimitiveRevisions_{};
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedGpuSceneGeometryRevisions_{};
        std::vector<GpuSceneRecordRange> gpuSceneUploadRanges_;
        GpuSceneUploadTelemetry gpuSceneUploadTelemetry_{};
        GpuSceneIndirectPlan opaqueIndirectPlan_;
        struct OpaqueIndirectBin {
            uint32_t packetBegin = 0;
            uint32_t commandBegin = 0;
            uint32_t commandCount = 0;
            PipelineHandle pipeline;
            MaterialHandle material;
            GeometryHandle geometry;
        };
        std::vector<OpaqueIndirectBin> opaqueIndirectBins_;
        std::vector<GpuSceneIndirectCandidate> opaqueIndirectCandidates_;
        std::vector<DepthPyramidDeviceQuery> depthOcclusionQueries_;
        // Main-view compaction telemetry. The CPU visibility counts are
        // production telemetry; GPU LOD and occlusion qualification state is
        // owned by the oracle (R2.8).
        struct PendingOpaqueIndirectValidation {
            uint64_t profileFrameId = 0;
            std::vector<uint32_t> expectedBinCounts;
            std::vector<uint32_t> binCapacities;
            uint64_t occlusionProfileFrameId = 0;
            uint32_t gpuSceneOcclusionCandidateCount = 0;
            std::vector<uint32_t> occlusionCandidatePrimitiveIndices;
            std::vector<uint32_t> occlusionCandidateBinIndices;
            bool lodQualificationOracle = false;
            bool occlusionQualificationOracle = false;
            bool gpuSceneOcclusionPending = false;
            bool occlusionRejectionApplied = false;
            bool pending = false;
        };
        std::array<PendingOpaqueIndirectValidation,
            VulkanFrameScheduler::FramesInFlight>
            pendingOpaqueIndirectValidations_{};
        uint32_t opaqueIndirectCommandCapacity_ = 0;
        static constexpr uint32_t MaximumOpaqueIndirectCommandCapacity = 65536u;
        uint32_t activeLightCount_ = 0;
        uint64_t lightUploadBytes_ = 0;
        uint32_t lightUploadRangeCount_ = 0;
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedReflectionProbeRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedReflectionProbeActiveListRevisions_{};
        std::vector<ReflectionProbeRecordRange>
            reflectionProbeUploadRanges_;
        std::vector<EnvironmentLightingHandles>
            reflectionProbeEnvironments_;
        uint32_t reflectionProbeRecordCapacity_ = 0;
        uint32_t reflectionProbeRecordMaximumCapacity_ = 0;
        uint32_t reflectionProbeClusterCapacity_ = 0;
        uint32_t reflectionProbeReferenceCapacity_ = 0;
        struct PendingReflectionProbeCapture {
            SceneEntityUuid owner;
            uint64_t captureTicket = 0;
            std::vector<VkDescriptorSet> filterDescriptors;
            VulkanReflectionProbeCaptureReadback bakedReadback;
            uint32_t resolution = 0;
            uint32_t mipLevels = 0;
        };
        std::vector<PendingReflectionProbeCapture>
            pendingReflectionProbeCaptures_;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash>
            capturedReflectionProbeSlots_;
        ReflectionProbeCaptureTelemetry reflectionProbeCaptureTelemetry_{};
        uint32_t reflectionProbePrefilterSampleCount_ = 256;

        // --- 2. THE MEMORY VAULTS ---
        // This is where the Handles are mapped to the physical Vulkan memory.
        ResourcePool<VulkanGeometryPayload, GeometryHandle> geometryVault;
        ResourcePool<VulkanTexturePayload, TextureHandle> textureVault;
        ResourcePool<VulkanMaterialPayload, MaterialHandle> materialVault;


        // --- 3. RUNTIME STATE ---
        uint32_t currentImageIndex = 0;
        VkCommandBuffer currentCmd = VK_NULL_HANDLE;
        bool initialized_ = false;
        bool cleaned_ = false;
        bool frameOpen_ = false;
        ViewHistoryContext currentViewHistory_{};
        uint64_t currentProjectionRevision_ = 1;
        uint64_t currentDepthContentRevision_ = 1;
        DepthPyramidHistoryDecision currentDepthHistoryDecision_{};
        bool depthHistoryPrepared_ = false;
        uint64_t publishedGpuSceneEpoch_ = 1;
        bool imguiInitialized_ = false;
        CpuProfiler* cpuProfiler_ = nullptr;
        bool collectFrameCounters_ = false;
        FrameCounters frameCounters_{};
        uint64_t weightedOitOrderSeed_ = 0;
        bool forceDirectGBufferReference_ = false;
        bool forceDirectShadowReference_ = false;
        float experimentalShadowLodErrorTexels_ = 0.0f;
        uint32_t shadowLodMaximumLevel_ = 15u;
        bool depthOcclusionQueryEnabled_ = false;
        bool depthOcclusionRejectionEnabled_ = false;
        float experimentalGpuLodErrorPixels_ = 0.0f;
        uint32_t gpuLodMaximumLevel_ = 15u;
        float gpuLodHysteresisFraction_ = 0.15f;
        float experimentalProbeLodErrorPixels_ = 0.0f;
        uint32_t probeLodMaximumLevel_ = 15u;
        glm::mat4 ordinary2ViewProjection_{ 1.0f };
        bool ordinary2ViewProjectionValid_ = false;
        std::vector<uint32_t> uniqueMaterialIds_;
        std::vector<uint64_t> uniquePipelineIds_;
        uint64_t externalSwapchainRequestedPeakBytes_ = 0;
        uint64_t externalSwapchainPeakImageCount_ = 0;
        RenderDebugView debugView_ = RenderDebugView::Final;
        VkExtent2D sceneExtent_{};
        GBufferLayout gBufferLayout_ = GBufferLayout::CanonicalReference;
        ClusterGridConfig clusterConfig_{};
        uint32_t directionalShadowResolution_ = 4096;
        uint32_t spotShadowAtlasResolution_ = 8192;
        std::array<uint32_t, 3> pointShadowCapacities_{
            kPointShadowPool256Capacity, kPointShadowPool512Capacity,
            kPointShadowPool1024Capacity };
        float manualExposureEv_ = 0.0f;
        OutputTransformOperator outputOperator_ = OutputTransformOperator::Aces2;
		Color::OutputTransport outputTransport_ = Color::OutputTransport::SdrSrgb;
		Color::OutputTransport requestedOutputTransport_ =
			Color::OutputTransport::SdrSrgb;
		VkFormat outputTargetFormat_ = VulkanSdrOutputFormat;
        float paperWhiteNits_ = 203.0f;
        float peakNits_ = 1000.0f;
        bool selectionOutlineActive_ = false;
        ViewportGridOverlay viewportGridOverlay_{};
        uint64_t retiredTextureCount_ = 0;
        TextureHandle outputTransformLut_{};
        bool finalCaptureHookRecorded_ = false;

        // --- 4. EXTENSIONS (M7R R2.7) ---
        // Attached by the factory before init(); not owned (each must outlive
        // the backend). None attached is the null object (no hook passes, no
        // oracle work).
        std::vector<IVulkanBackendExtension*> extensions_;
        IVulkanIndirectOracle* indirectOracle_ = nullptr;
        IVulkanIndirectStreamObserver* indirectStreamObserver_ = nullptr;
        VulkanGraphHooks graphHooks_ = VulkanGraphHooks::none();

        // Private helpers that Application.cpp no longer needs to worry about
        void createUniformBuffers();
        void createCanonicalMaterialBuffers(uint32_t capacity);
        void ensureCanonicalMaterialCapacity(uint32_t requiredCapacity);
        void uploadCanonicalMaterialsForFrame(uint32_t frameIndex);
        void createLightRecordBuffers(uint32_t capacity);
        void createGpuSceneBuffers(
            const GpuSceneCapacityRequirements& capacity);
        void bindGpuSceneBuffers();
        void createOpaqueIndirectBuffers(uint32_t capacity);
        void createGpuSceneCullPipeline();
        [[nodiscard]] VulkanCullerServices cullerServices();
        void createDirectionalShadowIndirectPipeline();
        [[nodiscard]] bool prepareDirectionalShadowIndirectSubmission(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows);
        void createSpotShadowIndirectPipeline();
        [[nodiscard]] bool prepareSpotShadowIndirectSubmission(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const SpotShadowFramePacket> shadows);
        void createPointShadowIndirectPipeline();
        [[nodiscard]] bool preparePointShadowIndirectSubmission(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const PointShadowFramePacket> shadows);
        void createReflectionProbeIndirectPipeline();
        // The view cullers in collection order: directional, spot, point,
        // reflection probe.
        [[nodiscard]] std::array<VulkanIndirectViewCuller*, kIndirectViewKindCount>
            indirectViewCullers() noexcept {
            return { &directionalCuller_, &spotCuller_, &pointCuller_,
                &probeCuller_ };
        }
        // Device telemetry (+ oracle verdict) of every view's retired slot.
        void collectIndirectViewValidations(uint32_t frameIndex);
        // Draw counters for the GPU-scene casters an oracle-checked shadow
        // work item drew (visibility[i] & visibilityBit).
        void recordIndirectOracleDraws(std::span<const uint8_t> visibility,
            uint8_t visibilityBit, uint64_t& drawCounter,
            uint64_t& commandCounter, uint64_t& alphaMaskCounter);
        [[nodiscard]] bool prepareReflectionProbeIndirectSubmission(
            const ReflectionProbeCasterSubmission& probeCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures);
        void recordReflectionProbeIndirectDispatch(uint32_t frameIndex,
            uint32_t faceRecord, uint32_t excludedInstanceIndex);
        void bindOpaqueIndirectBuffers();
        void collectOpaqueIndirectValidation(uint32_t frameIndex);
        [[nodiscard]] bool prepareOpaqueIndirectSubmission(
            std::span<const DrawPacket> opaqueQueue);
        void bindLightRecordBuffers();
        void bindClusterBuffers();
        void bindSceneClusterBuffers();
        void createNeutralEnvironmentProducts();
        void bindEnvironmentProducts(uint32_t frame = UINT32_MAX);
        std::array<EnvironmentLightingHandles, VulkanFrameScheduler::FramesInFlight> frameEnvironments_{};
        void bindDirectionalShadowDescriptors();
        void bindSpotShadowDescriptors();
        void bindPointShadowDescriptors();
        void createReflectionProbeBuffers(uint32_t recordCapacity,
            uint32_t clusterCapacity, uint32_t referenceCapacity);
        void bindReflectionProbeBuffers();
        void bindReflectionProbeEnvironments();
        void uploadReflectionProbesForFrame(uint32_t frameIndex,
            const ReflectionProbeGpuFramePacket& probes);
        void updateReflectionProbeParameters(uint32_t frameIndex,
            const glm::mat4& view, const glm::mat4& projection,
            float nearPlane, float farPlane, uint32_t activeProbeCount);
        void uploadLightsForFrame(uint32_t frameIndex,
            const LightingFramePacket& lights);
        void updateClusterParameters(uint32_t frameIndex,
            const glm::mat4& view, const glm::mat4& projection,
            float nearPlane, float farPlane, uint32_t activeLightCount);
        void updateClusterFallbackCandidates(uint32_t frameIndex,
            const glm::mat4& view, const LightingFramePacket& lights);
        void collectClusterDiagnostics(uint32_t frameIndex) noexcept;
        void initFrameTargets();
        void rebuildRenderGraphAfterDeviceIdle();
        [[nodiscard]] VulkanProductionGraphFeatures
            productionGraphFeatures() const noexcept;
        void applyTransparencyPyramidTopologyChange(
            std::optional<VkExtent2D> requestedOrdinary2AtlasExtent =
                std::nullopt,
            std::optional<VkExtent2D> requestedHero4AtlasExtent =
                std::nullopt,
            std::optional<VkExtent2D> requestedCinematic8AtlasExtent =
                std::nullopt);
        void setWeightedOitInstanceCapacity(uint32_t capacity);
        void updateUniformBuffer(const glm::mat4& view, const glm::mat4& proj);
        void createLightingRenderPass();
        void resetFrameCounters();
        void recordMaterialBind(MaterialHandle material);
        void recordPipelineBind(uint64_t pipelineIdentity);
        void recordDraw(uint64_t& drawCounter, uint64_t submittedTriangles);
        void emitFrameCounters();
        void bindMaterialDescriptors(VkPipelineLayout layout);
        void recordOrdinary2InterfaceCapture(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws,
            bool exitCapture);
        void recordOrdinary2Captures(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws);
        void recordDeepLayeredInterfaceCapture(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality, uint32_t interfaceIndex);
        void recordDeepLayeredTileTermination(
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality, uint32_t interfaceIndex);
        void recordDeepLayeredCaptures(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality);
        void recordDeepLayeredLocalComposition(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality);
        void recordDeepLayeredSceneResolve(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws);
        void recordOrdinary2LocalComposition(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws);
        void recordOrdinary2SceneResolve(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws);
        void prepareOrdinary2ResolvedPacketIndices(
            std::span<const Ordinary2CaptureDraw> draws);
        [[nodiscard]] bool isOrdinary2PacketResolved(
            uint32_t packetIndex) const noexcept;
        void prepareDeepResolvedPacketIndices(
            std::span<const LayeredCaptureDraw> draws);
        [[nodiscard]] bool isDeepPacketResolved(
            uint32_t packetIndex) const noexcept;
        [[nodiscard]] bool isLayeredPacketResolved(
            uint32_t packetIndex) const noexcept;
        [[nodiscard]] uint32_t acquireSampler(const SamplerDesc& desc);
        void releaseSampler(uint32_t cacheIndex) noexcept;
        void cleanupSamplerCache() noexcept;
        [[nodiscard]] uint64_t liveSamplerCount() const noexcept;
        void recordDeepLayeredValidationHook(
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality);
        // Extension hooks (R2.7). A pass hook does nothing when undeclared,
        // skips its pass when no extension wants it, or begins the GPU range
        // and pass and calls every extension that wants it.
        [[nodiscard]] bool anyExtensionWants(
            const VulkanHookContext& context) const;
        void runPassHook(const VulkanHookContext& context, bool declared,
            std::string_view passName, const char* gpuRangeName);
        void notifyHook(const VulkanHookContext& context);
        // Brackets a capture copy: scene-linear transitions scene.color to
        // TransferSource and back; final output runs in final-capture-hook.
        template<typename Record>
        void recordCaptureCopy(FrameCapturePoint point, Record&& record);
        void runCaptureHook(VulkanHookPoint point, FrameCapturePoint capturePoint);
        [[nodiscard]] VulkanCaptureHookPayload captureSource(
            FrameCapturePoint point);
        [[nodiscard]] VulkanBackendServices backendServices() noexcept;
        // Oracle access for expectation-emission sites: null unless this is a
        // qualification build with an attached oracle enabled for the view.
        [[nodiscard]] IVulkanIndirectOracle* activeIndirectOracle(
            VulkanIndirectOracleView view) const noexcept {
            if constexpr (kQualificationBuild) {
                if (indirectOracle_ != nullptr && indirectOracle_->enabled(view))
                    return indirectOracle_;
            }
            return nullptr;
        }
        // Command-stream digest observer (R3a.0): null unless this is a
        // qualification build with a digest requested.
        [[nodiscard]] IVulkanIndirectStreamObserver*
            activeIndirectStreamObserver() const noexcept {
            if constexpr (kQualificationBuild)
                return indirectStreamObserver_;
            return nullptr;
        }
        [[nodiscard]] FrameMemoryProfile memorySnapshot();

    public:
        VulkanVertexBackend() = default;
        ~VulkanVertexBackend() override { cleanup(); }

        // Factory-only, before init(). Does not take ownership.
        void attachExtension(IRenderBackendExtension* extension);

        // --- IRenderBackend Interface Implementation ---
        void init(GLFWwindow* window, const RenderBackendConfig& config) override;
        void cleanup() override;
        void recreateSwapchain(GLFWwindow* window) override;
        void setOutputTransport(GLFWwindow* window,
            Color::OutputTransport requestedTransport) override;
        [[nodiscard]] RenderExtent getRenderExtent() const override;
        [[nodiscard]] bool resizeSceneRenderExtent(
            RenderExtent extent, std::string& diagnostic) override;
        [[nodiscard]] RenderBackendCapabilities getCapabilities() const override;
        [[nodiscard]] RenderBackendRuntimeInfo getRuntimeInfo() const override;
        [[nodiscard]] FrameTopologyPreparation prepareFrameTopology(
            const FrameTopologyRequirements& requirements) override;
        void prepareLighting(uint32_t requiredCapacity) override;
        void prepareGpuScene(
            const GpuSceneCapacityRequirements& requirements) override;
        void publishGpuScene(const GpuScenePackedTables& scene) override;
        [[nodiscard]] GpuSceneFrameSerials getGpuSceneFrameSerials()
            const noexcept override {
            return { scheduler.lastSubmittedSerial(),
                scheduler.completedSerial() };
        }
        [[nodiscard]] GpuSceneUploadTelemetry getGpuSceneUploadTelemetry()
            const noexcept override { return gpuSceneUploadTelemetry_; }
        void prepareReflectionProbes(uint32_t requiredCapacity,
            std::span<const EnvironmentLightingHandles> environments) override;
        [[nodiscard]] std::vector<ReflectionProbeCaptureCompletion>
            finalizeReflectionProbeCaptures() override;
        [[nodiscard]] std::optional<uint32_t>
            capturedReflectionProbeEnvironmentSlot(
                SceneEntityUuid owner) const noexcept override;
        void synchronizeReflectionProbeCaptureOwners(
            std::span<const SceneEntityUuid> owners) override;
        void configureReflectionProbeCaptures(
            const ProjectReflectionProbeSettings& settings) override;
        FrameStatus beginFrame() override;
        void updateCamera(const ViewTransportRecord& view,
            ViewHistoryContext history = {}) override;
        void setDebugView(RenderDebugView view) override { debugView_ = view; }
        void setOutputSettings(float manualExposureEv, float paperWhiteNits,
            float peakNits) override;
        void setViewportGridOverlay(
            const ViewportGridOverlay& overlay) override {
            viewportGridOverlay_ = overlay;
        }
        void submitDirectionalShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows) override;
        void submitSpotShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const SpotShadowFramePacket> shadows) override;
        void submitPointShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const PointShadowFramePacket> shadows) override;
        void submitReflectionProbeCaptures(
            const ReflectionProbeCasterSubmission& probeCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights) override;
        [[nodiscard]] ReflectionProbeCaptureTelemetry
            getReflectionProbeCaptureTelemetry() const noexcept override {
            return reflectionProbeCaptureTelemetry_;
        }
        [[nodiscard]] uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission& shadowCasters) const noexcept override;
        [[nodiscard]] std::array<uint64_t, kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(
                const ShadowCasterSubmission& shadowCasters,
                const DirectionalShadowCascadePlan& plan) const noexcept override;
        void prepareDepthPyramidHistory(
            std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> opaqueForwardQueue) override;

        void submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> selectionQueue, bool isWireframe) override;
        void submitLightingPass(const glm::vec3& cameraPos,
            const glm::mat4& view, const glm::mat4& proj,
            float nearPlane, float farPlane,
            const LightingFramePacket& lights,
            const ReflectionProbeGpuFramePacket& reflectionProbes) override;
        [[nodiscard]] LightingUploadTelemetry
            getLightingUploadTelemetry() const noexcept override {
            return { lightUploadBytes_, lightUploadRangeCount_,
                activeLightCount_, lightRecordCapacity_ };
        }
        [[nodiscard]] ClusteredLightingTelemetry
            getClusteredLightingTelemetry() const noexcept override {
            return clusterTelemetry_;
        }
        void submitForwardQueues(std::span<const DrawPacket> opaqueForwardQueue,
            std::span<const DrawPacket> sortedSurfaceQueue,
            std::span<const DrawPacket> compatibilityTransparentQueue,
            std::span<const glm::mat4> instanceTransforms = {}) override;
        void submitOutputPass() override;
        void submitUIPass() override;

        void beginUI() override;
        void* getLitSceneTextureID() override;
        void* getGlassDepthTextureID() override;
        void* getEditorTextureID(TextureHandle texture) override;

        FrameStatus endFrame() override;

        // Resource Allocation
        GeometryHandle allocateGeometry(const GeometryDesc& desc,
            std::span<const std::byte> vertexBytes,
            std::span<const std::byte> indexBytes) override;
        void freeGeometry(GeometryHandle handle) override;
        GeometryArenaAllocation allocateGeometryArena(
            uint32_t vertexStride,
            std::span<const std::byte> vertexBytes,
            const GeometryArenaData& arena) override;
        void freeGeometryArena(
            std::span<const GeometryHandle> primitiveGeometry) override;

        TextureHandle allocateTexture(const TextureDesc& desc,
            std::span<const std::byte> pixelBytes) override;
        void freeTexture(TextureHandle handle) override;

        MaterialBinding allocateCanonicalMaterial(
            const CanonicalMaterialAsset& desc) override;
        void updateCanonicalMaterial(MaterialHandle handle,
            const PackedGpuMaterial& material) override;
        void freeMaterial(MaterialHandle handle) override;

        void setEnvironmentLighting(
            const EnvironmentLightingHandles& environment) override;
        [[nodiscard]] EnvironmentLightingHandles getEnvironmentLighting() const override { return environmentLighting_; }
        void prepareRetainedViews(bool enabled, uint32_t renderView) override;
        [[nodiscard]] void* getRetainedViewTextureID(uint32_t view) override;
        void setEnvironmentLightingSettings(
            const EnvironmentLightingSettings& settings) override;
        void setOutputTransformLut(TextureHandle lutHandle) override;
    };

} // namespace Iridium
