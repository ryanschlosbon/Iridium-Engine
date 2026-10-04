#include "VulkanFrameTelemetry.h"

#include "VulkanIndexedTextureTable.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>

namespace Iridium {

    void VulkanFrameTelemetry::init(CpuProfiler* profiler) {
        profiler_ = profiler;
        if (profiler_ != nullptr && profiler_->isEnabled()) {
            uniqueMaterialIds_.reserve(MaxUniqueResourcesPerFrame);
            uniquePipelineIds_.reserve(MaxUniqueResourcesPerFrame);
        }
    }

    void VulkanFrameTelemetry::cleanup() noexcept {
        profiler_ = nullptr;
        collecting_ = false;
        uniqueMaterialIds_.clear();
        uniquePipelineIds_.clear();
    }

    void VulkanFrameTelemetry::beginFrame() {
        collecting_ = profiler_ != nullptr && profiler_->isFrameOpen();
        if (collecting_ && uniqueMaterialIds_.capacity() == 0) {
            uniqueMaterialIds_.reserve(MaxUniqueResourcesPerFrame);
            uniquePipelineIds_.reserve(MaxUniqueResourcesPerFrame);
        }
        counters_ = {};
        uniqueMaterialIds_.clear();
        uniquePipelineIds_.clear();
    }

    void VulkanFrameTelemetry::recordMaterialBind(MaterialHandle material) {
        if (!collecting_) {
            return;
        }
        ++counters_.materialBinds;
        const uint32_t identity = material.id;
        if (std::find(uniqueMaterialIds_.begin(), uniqueMaterialIds_.end(), identity) !=
            uniqueMaterialIds_.end()) {
            return;
        }
        if (uniqueMaterialIds_.size() >= MaxUniqueResourcesPerFrame) {
            ++counters_.materialUniqueOverflow;
            return;
        }
        uniqueMaterialIds_.push_back(identity);
    }

    void VulkanFrameTelemetry::recordPipelineBind(uint64_t pipelineIdentityValue) {
        if (!collecting_) {
            return;
        }
        ++counters_.pipelineBinds;
        if (std::find(uniquePipelineIds_.begin(), uniquePipelineIds_.end(),
            pipelineIdentityValue) != uniquePipelineIds_.end()) {
            return;
        }
        if (uniquePipelineIds_.size() >= MaxUniqueResourcesPerFrame) {
            ++counters_.pipelineUniqueOverflow;
            return;
        }
        uniquePipelineIds_.push_back(pipelineIdentityValue);
    }

    void VulkanFrameTelemetry::recordDraw(uint64_t& drawCounter,
        uint64_t submittedTriangles) {
        if (!collecting_) {
            return;
        }
        ++drawCounter;
        counters_.trianglesSubmitted += submittedTriangles;
    }

    void VulkanFrameTelemetry::emit(
        const VulkanFrameResidencyCounters& residency) const {
        if (!collecting_ || profiler_ == nullptr) {
            return;
        }

        const uint64_t transparentDraws = counters_.drawTransparentDepth +
            counters_.drawTransparentForward +
            counters_.drawWeightedOitAccumulation +
            counters_.drawWeightedOitResolve +
            counters_.ordinary2CaptureEntryDraws +
            counters_.ordinary2CaptureExitDraws +
            counters_.ordinary2LocalCompositionDraws +
            counters_.ordinary2SceneResolveDraws +
            counters_.deepLayeredInterfaceDraws +
            counters_.deepLayeredResidualProbeDraws +
            counters_.deepLayeredLocalCompositionDraws +
            counters_.deepLayeredSceneResolveDraws;
        const uint64_t totalDraws = counters_.drawOpaque +
            counters_.drawSelection +
            counters_.drawShadowDirectional +
            counters_.drawShadowSpot +
            counters_.drawShadowPoint +
            counters_.drawLighting +
            counters_.drawOutput +
            transparentDraws + counters_.drawUi;
        const ProfileCounterStatus uiAwareStatus = counters_.uiUntrackedCallbacks == 0
            ? ProfileCounterStatus::Exact
            : ProfileCounterStatus::Estimated;
        const ProfileCounterStatus materialUniqueStatus =
            counters_.materialUniqueOverflow == 0
            ? ProfileCounterStatus::Exact
            : ProfileCounterStatus::Estimated;
        const ProfileCounterStatus pipelineUniqueStatus =
            counters_.pipelineUniqueOverflow == 0
            ? ProfileCounterStatus::Exact
            : ProfileCounterStatus::Estimated;

        profiler_->recordCounter("draw.recorded.opaque", counters_.drawOpaque);
        profiler_->recordCounter("opaque.indirect.command_count",
            counters_.opaqueIndirectCommands);
        profiler_->recordCounter("opaque.indirect.bin_count",
            counters_.opaqueIndirectBins);
        profiler_->recordCounter("opaque.indirect.direct_fallback_packets",
            counters_.opaqueIndirectFallbackPackets);
        profiler_->recordCounter("opaque.indirect.fallback_reason",
            counters_.opaqueIndirectFallbackReason);
        profiler_->recordCounter("depth.occlusion.history_eligible",
            counters_.depthHistoryEligible);
        profiler_->recordCounter("depth.occlusion.history_rejection",
            counters_.depthHistoryRejection);
        profiler_->recordCounter("draw.recorded.selection", counters_.drawSelection);
        profiler_->recordCounter("draw.recorded.shadow.directional",
            counters_.drawShadowDirectional);
        profiler_->recordCounter("draw.recorded.shadow.directional.alpha_mask",
            counters_.drawShadowDirectionalAlphaMask);
        profiler_->recordCounter("shadow.directional.casters.tested",
            counters_.shadowDirectionalCastersTested);
        profiler_->recordCounter("shadow.directional.casters.culled",
            counters_.shadowDirectionalCastersCulled);
        profiler_->recordCounter("shadow.directional.indirect.commands",
            counters_.shadowDirectionalIndirectCommands);
        profiler_->recordCounter("shadow.directional.indirect.bins",
            counters_.shadowDirectionalIndirectBins);
        profiler_->recordCounter(
            "shadow.directional.indirect.direct_fallback",
            counters_.shadowDirectionalDirectFallback);
        profiler_->recordCounter(
            "shadow.directional.indirect.fallback_reason",
            counters_.shadowDirectionalIndirectFallbackReason);
        profiler_->recordCounter(
            "shadow.directional.indirect.membership_cache_hit",
            counters_.shadowDirectionalMembershipCacheHit);
        profiler_->recordCounter("draw.recorded.shadow.spot",
            counters_.drawShadowSpot);
        profiler_->recordCounter("draw.recorded.shadow.spot.alpha_mask",
            counters_.drawShadowSpotAlphaMask);
        profiler_->recordCounter("shadow.spot.casters.tested",
            counters_.shadowSpotCastersTested);
        profiler_->recordCounter("shadow.spot.casters.culled",
            counters_.shadowSpotCastersCulled);
        profiler_->recordCounter("shadow.spot.indirect.commands",
            counters_.shadowSpotIndirectCommands);
        profiler_->recordCounter("shadow.spot.indirect.bins",
            counters_.shadowSpotIndirectBins);
        profiler_->recordCounter("shadow.spot.indirect.direct_fallback",
            counters_.shadowSpotDirectFallback);
        profiler_->recordCounter("shadow.spot.indirect.fallback_reason",
            counters_.shadowSpotIndirectFallbackReason);
        profiler_->recordCounter("shadow.spot.indirect.membership_cache_hit",
            counters_.shadowSpotMembershipCacheHit);
        profiler_->recordCounter("draw.recorded.shadow.point",
            counters_.drawShadowPoint);
        profiler_->recordCounter("draw.recorded.shadow.point.alpha_mask",
            counters_.drawShadowPointAlphaMask);
        profiler_->recordCounter("shadow.point.casters.tested",
            counters_.shadowPointCastersTested);
        profiler_->recordCounter("shadow.point.casters.culled",
            counters_.shadowPointCastersCulled);
        profiler_->recordCounter("shadow.point.indirect.commands",
            counters_.shadowPointIndirectCommands);
        profiler_->recordCounter("shadow.point.indirect.bins",
            counters_.shadowPointIndirectBins);
        profiler_->recordCounter("shadow.point.indirect.direct_fallback",
            counters_.shadowPointDirectFallback);
        profiler_->recordCounter("shadow.point.indirect.fallback_reason",
            counters_.shadowPointIndirectFallbackReason);
        profiler_->recordCounter("shadow.point.indirect.membership_cache_hit",
            counters_.shadowPointMembershipCacheHit);
        profiler_->recordCounter("draw.recorded.lighting", counters_.drawLighting);
        profiler_->recordCounter("draw.recorded.output", counters_.drawOutput);
        profiler_->recordCounter("draw.recorded.transparent.depth",
            counters_.drawTransparentDepth);
        profiler_->recordCounter("draw.recorded.transparent.forward",
            counters_.drawTransparentForward);
        profiler_->recordCounter(
            "draw.recorded.transparent.oit.accumulation",
            counters_.drawWeightedOitAccumulation);
        profiler_->recordCounter("draw.recorded.transparent.oit.resolve",
            counters_.drawWeightedOitResolve);
        profiler_->recordCounter("draw.recorded.forward.standard",
            counters_.drawStandardForward);
        profiler_->recordCounter("draw.recorded.forward.complex",
            counters_.drawComplexForward);
        profiler_->recordCounter("draw.recorded.forward.unlit",
            counters_.drawUnlitForward);
        constexpr std::array<const char*, 8> LobeCounterNames{
            "draw.recorded.lobe.clearcoat", "draw.recorded.lobe.sheen",
            "draw.recorded.lobe.anisotropy", "draw.recorded.lobe.iridescence",
            "draw.recorded.lobe.thin_transmission",
            "draw.recorded.lobe.volume_transmission",
            "draw.recorded.lobe.dispersion",
            "draw.recorded.lobe.diffuse_transmission",
        };
        for (size_t index = 0; index < LobeCounterNames.size(); ++index)
            profiler_->recordCounter(LobeCounterNames[index],
                counters_.complexLobeDraws[index]);
        profiler_->recordCounter("draw.recorded.transparent", transparentDraws);
        profiler_->recordCounter("draw.recorded.ui", counters_.drawUi,
            uiAwareStatus);
        profiler_->recordCounter("draw.recorded.total", totalDraws, uiAwareStatus);
        profiler_->recordCounter("dispatch.recorded",
            counters_.dispatchRecorded);
        profiler_->recordCounter("triangle.submitted", counters_.trianglesSubmitted,
            uiAwareStatus);
        profiler_->recordCounter("material.binds", counters_.materialBinds);
        profiler_->recordCounter("material.unique", uniqueMaterialIds_.size(),
            materialUniqueStatus);
        profiler_->recordCounter("material.unique_overflow",
            counters_.materialUniqueOverflow);
        profiler_->recordCounter("pipeline.binds", counters_.pipelineBinds);
        profiler_->recordCounter("pipeline.unique", uniquePipelineIds_.size(),
            pipelineUniqueStatus);
        profiler_->recordCounter("pipeline.unique_overflow",
            counters_.pipelineUniqueOverflow);
        profiler_->recordCounter("transparent.bucket.background_packets",
            counters_.transparentBackgroundPackets);
        profiler_->recordCounter("transparent.bucket.foreground_packets",
            counters_.transparentForegroundPackets);
        profiler_->recordCounter("transparent.bucket.nonempty",
            counters_.transparentNonemptyBuckets);
        profiler_->recordCounter("transparent.sorted.packets",
            counters_.transparentSortedPackets);
        profiler_->recordCounter("transparent.oit.packets",
            counters_.weightedOitPackets);
        profiler_->recordCounter("transparent.oit.sorted_fallback_packets",
            counters_.weightedOitSortedFallbackPackets);
        profiler_->recordCounter(
            "transparent.oit.instance_capacity_fallback_packets",
            counters_.weightedOitInstanceCapacityFallbackPackets);
        profiler_->recordCounter("transparent.oit.instances",
            counters_.weightedOitInstances);
        profiler_->recordCounter("transparent.oit.instance_upload_bytes",
            counters_.weightedOitInstanceUploadBytes);
        profiler_->recordCounter("transparent.oit.resident",
            static_cast<uint64_t>(residency.weightedOitResident));
        profiler_->recordCounter("transparent.oit.order_seed",
            residency.weightedOitOrderSeed);
        profiler_->recordCounter("transparent.pyramid.builds",
            counters_.transparencyPyramidBuilds);
        profiler_->recordCounter("transparent.pyramid.mip_dispatches",
            counters_.transparencyPyramidMipDispatches);
        profiler_->recordCounter("transparent.pyramid.resident",
            static_cast<uint64_t>(residency.refractionPyramidsResident));
        profiler_->recordCounter("transparent.pyramid.topology_rebuilds",
            counters_.transparencyPyramidTopologyRebuilds);
        profiler_->recordCounter(
            "transparent.pyramid.topology_rebuild_failures",
            counters_.transparencyPyramidTopologyRebuildFailures);
        profiler_->recordCounter("transparent.pyramid.fallback_frames",
            counters_.transparencyPyramidFallbackFrames);
        profiler_->recordCounter("transparent.ordinary2.probe_frames",
            counters_.ordinary2ProbeFrames);
        profiler_->recordCounter("transparent.ordinary2.candidate_packets",
            counters_.ordinary2CandidatePackets);
        profiler_->recordCounter("transparent.ordinary2.projected_packets",
            counters_.ordinary2ProjectedPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.projection_culled_packets",
            counters_.ordinary2ProjectionCulledPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.fallback.invalid_bounds_packets",
            counters_.ordinary2InvalidBoundsFallbackPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.fallback.near_plane_packets",
            counters_.ordinary2NearPlaneFallbackPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.fallback.unsafe_projection_packets",
            counters_.ordinary2UnsafeProjectionFallbackPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.fallback.request_capacity_packets",
            counters_.ordinary2RequestCapacityFallbackPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.atlas.accepted_packets",
            counters_.ordinary2AtlasAcceptedPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.atlas.accepted_islands",
            counters_.ordinary2AtlasAcceptedIslands);
        profiler_->recordCounter(
            "transparent.ordinary2.atlas.rejected_packets",
            counters_.ordinary2AtlasRejectedPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.atlas.allocated_texels",
            counters_.ordinary2AtlasAllocatedTexels);
        profiler_->recordCounter(
            "transparent.ordinary2.capture.prepared_draws",
            counters_.ordinary2CapturePreparedDraws);
        profiler_->recordCounter(
            "transparent.ordinary2.capture.preparation_fallback_packets",
            counters_.ordinary2CapturePreparationFallbackPackets);
        profiler_->recordCounter(
            "transparent.ordinary2.capture.entry_draws",
            counters_.ordinary2CaptureEntryDraws);
        profiler_->recordCounter(
            "transparent.ordinary2.capture.exit_draws",
            counters_.ordinary2CaptureExitDraws);
        profiler_->recordCounter(
            "transparent.ordinary2.local_composition_draws",
            counters_.ordinary2LocalCompositionDraws);
        profiler_->recordCounter(
            "transparent.ordinary2.scene_resolve_draws",
            counters_.ordinary2SceneResolveDraws);
        profiler_->recordCounter(
            "transparent.layered.deep.candidate_packets",
            counters_.deepLayeredCandidatePackets);
        profiler_->recordCounter(
            "transparent.layered.deep.projected_packets",
            counters_.deepLayeredProjectedPackets);
        profiler_->recordCounter(
            "transparent.layered.deep.atlas.accepted_packets",
            counters_.deepLayeredAtlasAcceptedPackets);
        profiler_->recordCounter(
            "transparent.layered.deep.atlas.accepted_islands",
            counters_.deepLayeredAtlasAcceptedIslands);
        profiler_->recordCounter(
            "transparent.layered.deep.atlas.rejected_packets",
            counters_.deepLayeredAtlasRejectedPackets);
        profiler_->recordCounter(
            "transparent.layered.deep.capture.prepared_draws",
            counters_.deepLayeredCapturePreparedDraws);
        profiler_->recordCounter(
            "transparent.layered.deep.capture.preparation_fallback_packets",
            counters_.deepLayeredCapturePreparationFallbackPackets);
        profiler_->recordCounter(
            "transparent.layered.deep.capture.interface_draws",
            counters_.deepLayeredInterfaceDraws);
        profiler_->recordCounter(
            "transparent.layered.deep.residual.probe_draws",
            counters_.deepLayeredResidualProbeDraws);
        profiler_->recordCounter(
            "transparent.layered.deep.local_composition_draws",
            counters_.deepLayeredLocalCompositionDraws);
        profiler_->recordCounter(
            "transparent.layered.deep.scene_resolve_draws",
            counters_.deepLayeredSceneResolveDraws);
        profiler_->recordCounter("ui.untracked_callbacks",
            counters_.uiUntrackedCallbacks);
        profiler_->recordCounter("texture.resident",
            residency.texturesResident);
        profiler_->recordCounter("texture.retired", residency.texturesRetired);
        profiler_->recordCounter("texture.sampler.live", residency.samplersLive);
        profiler_->recordCounter("texture.sampler.cached", residency.samplersCached);
        profiler_->recordCounter("material.resident", residency.materialsResident);
        profiler_->recordCounter("material.descriptor.sets",
            VulkanIndexedTextureTable::FrameSetCount *
                VulkanIndexedTextureTable::SetsPerFrame);
        profiler_->recordCounter("material.descriptor.indexed",
            1);
        profiler_->recordCounter("material.table.capacity",
            residency.materialTableCapacity);
        profiler_->recordCounter("material.table.maximum_capacity",
            residency.materialTableMaximumCapacity);
        profiler_->recordCounter("texture.descriptor.view_capacity",
            residency.textureViewCapacity);
        profiler_->recordCounter("texture.descriptor.sampler_capacity",
            residency.textureSamplerCapacity);
        profiler_->recordCounter("texture.descriptor.required_capacity",
            residency.textureRequiredCapacity);
        profiler_->recordCounter("texture.descriptor.maximum_capacity",
            residency.textureMaximumCapacity);
    }

} // namespace Iridium
