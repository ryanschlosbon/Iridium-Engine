#pragma once

// M7R R3c.0: per-frame recording counters and their profiler emission, moved
// out of VulkanVertexBackend. Feature owners count through this sink (the
// VulkanFeatureContext carries it); emission happens once per frame at
// endFrame, in the original counter order.

#include "core/types/RenderHandles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Iridium {

    class CpuProfiler;

    // Identities of the fixed (non-material) pipelines in the pipeline-bind
    // counters. Material pipelines use their PipelineHandle id.
    inline constexpr uint64_t FixedPipelineIdentityMask = uint64_t{ 1 } << 63;

    enum class FixedPipelineIdentity : uint64_t {
        GBufferWireframe = FixedPipelineIdentityMask | 1,
        SelectionMask = FixedPipelineIdentityMask | 2,
        DeferredLighting = FixedPipelineIdentityMask | 3,
        SelectionOutline = FixedPipelineIdentityMask | 4,
        RetiredGlassDepth = FixedPipelineIdentityMask | 5, // reserved; never reuse
        ImGui = FixedPipelineIdentityMask | 6,
        OutputTransform = FixedPipelineIdentityMask | 7,
        LayeredInterfaceCapture = FixedPipelineIdentityMask | 8,
        LayeredLocalComposition = FixedPipelineIdentityMask | 9,
        LayeredSceneResolve = FixedPipelineIdentityMask | 10,
        LayeredResidualComposition = FixedPipelineIdentityMask | 11,
        LayeredTileTermination = FixedPipelineIdentityMask | 12,
        WeightedOitAccumulation = FixedPipelineIdentityMask | 13,
        WeightedOitResolve = FixedPipelineIdentityMask | 14,
    };

    [[nodiscard]] constexpr uint64_t pipelineIdentity(
        FixedPipelineIdentity identity) noexcept {
        return static_cast<uint64_t>(identity);
    }

    struct VulkanFrameCounters {
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

    // Backend-owned state the per-frame counter block reports, sampled when
    // the counters are emitted.
    struct VulkanFrameResidencyCounters {
        bool weightedOitResident = false;
        uint64_t weightedOitOrderSeed = 0;
        bool refractionPyramidsResident = false;
        uint64_t texturesResident = 0;
        uint64_t texturesRetired = 0;
        uint64_t samplersLive = 0;
        uint64_t samplersCached = 0;
        uint64_t materialsResident = 0;
        uint64_t materialTableCapacity = 0;
        uint64_t materialTableMaximumCapacity = 0;
        uint64_t textureViewCapacity = 0;
        uint64_t textureSamplerCapacity = 0;
        uint64_t textureRequiredCapacity = 0;
        uint64_t textureMaximumCapacity = 0;
    };

    class VulkanFrameTelemetry final {
    public:
        static constexpr size_t MaxUniqueResourcesPerFrame = 512;

        // Binds the process profiler (may be null); an enabled profiler
        // reserves the unique-resource sets up front.
        void init(CpuProfiler* profiler);
        void cleanup() noexcept;
        // Starts the frame's counters: collection follows the profiler's
        // open frame; counters and unique sets are reset.
        void beginFrame();

        [[nodiscard]] bool collecting() const noexcept { return collecting_; }
        [[nodiscard]] VulkanFrameCounters& counters() noexcept { return counters_; }
        [[nodiscard]] const VulkanFrameCounters& counters() const noexcept {
            return counters_;
        }

        void recordMaterialBind(MaterialHandle material);
        void recordPipelineBind(uint64_t pipelineIdentity);
        void recordDraw(uint64_t& drawCounter, uint64_t submittedTriangles);

        // Records the frame's counters into the profiler (no-op unless
        // collecting with a profiler).
        void emit(const VulkanFrameResidencyCounters& residency) const;

    private:
        CpuProfiler* profiler_ = nullptr;
        bool collecting_ = false;
        VulkanFrameCounters counters_{};
        std::vector<uint32_t> uniqueMaterialIds_;
        std::vector<uint64_t> uniquePipelineIds_;
    };

} // namespace Iridium
