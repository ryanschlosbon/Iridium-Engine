// End-of-run qualification reports: IRIDIUM_RUN_METRICS, the frame-capture
// drain, the readback-validation results and the resize/lifecycle/fallback
// verdicts (RunComplete, backend alive), and probe-resource release
// (ReleaseResources, both shutdown paths).

#include "qualification/harness/QualificationHarness.h"

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "profiling/CpuProfiler.h"
#include "qualification/harness/HarnessDetail.h"
#include "renderer/rhi/Mesh.h"

namespace Iridium {

    void QualificationHarness::reportRunMetrics(
        const AppShutdownContext& context) const {
        const AppRunSnapshot& run = context.run;
        if (!run.measurementStarted) return;
        const uint64_t averageNanoseconds = run.measuredFrameCount != 0
            ? run.measurementWallNanoseconds / run.measuredFrameCount
            : 0;
        std::cout << "IRIDIUM_RUN_METRICS {\"measured_frames\":"
            << run.measuredFrameCount << ",\"wall_ns\":" << run.measurementWallNanoseconds
            << ",\"average_ns\":" << averageNanoseconds
            << ",\"render_width\":" << run.renderExtent.width
            << ",\"render_height\":" << run.renderExtent.height
            << ",\"window_visible\":" << (context.config.windowVisible ? "true" : "false")
            << ",\"window_decorated\":" << (context.config.windowDecorated ? "true" : "false")
            << ",\"validation\":" << (context.config.enableValidation ? "true" : "false")
            << ",\"cpu_profiling\":" << (context.profiler.isEnabled() ? "true" : "false")
            << "}\n";
    }

    void QualificationHarness::collectEndOfRunValidations(
        AppShutdownContext& context) {
        const ApplicationConfig& config = context.config;
        IRenderBackend& backend = *context.backend;
        const RenderExtent renderExtent = context.run.renderExtent;
        if (config.captureFrameIndex) {
            std::vector<FrameCapture> captures =
                backend.collectFrameCaptures(true);
            if (captures.size() != 1 ||
                captures.front().captureId != *config.captureFrameIndex) {
                throw std::runtime_error(
                    "The requested measured frame did not produce exactly one capture.");
            }
            completedCapture_ = std::move(captures.front());
        }
        if (config.validateDepthPyramidCapture) {
            const auto validations =
                backend.collectDepthPyramidCaptureValidations(true);
            if (validations.size() != 1u ||
                validations.front().validationId != 0u) {
                throw std::runtime_error(
                    "The requested depth-pyramid validation did not produce exactly one GPU readback result.");
            }
            const DepthPyramidCaptureValidationResult& validation =
                validations.front();
            std::cout
                << "IRIDIUM_DEPTH_PYRAMID_CAPTURE_VALIDATION {\"validation_id\":"
                << validation.validationId
                << ",\"extent\":[" << validation.extent.width << ','
                << validation.extent.height << ']'
                << ",\"mip_count\":" << validation.mipCount
                << ",\"source_texels\":" << validation.sourceTexelCount
                << ",\"pyramid_texels\":" << validation.pyramidTexelCount
                << ",\"invalid_source_texels\":"
                << validation.invalidSourceTexelCount
                << ",\"mismatch_texels\":" << validation.mismatchTexelCount
                << ",\"history_mismatch_texels\":"
                << validation.historyMismatchTexelCount
                << ",\"first_mismatch_mip\":" << validation.firstMismatchMip
                << ",\"first_mismatch_texel\":"
                << validation.firstMismatchTexel
                << ",\"maximum_absolute_error\":"
                << validation.maximumAbsoluteError
                << ",\"passed\":" << (validation.passed() ? "true" : "false")
                << "}\n" << std::flush;
            if (!validation.passed()) {
                throw std::runtime_error(
                    "Live scene-depth pyramid readback did not match the CPU oracle.");
            }
        }
        if (config.validateOrdinary2Capture ||
            config.validateOrdinary2Resize) {
            const std::vector<Ordinary2CaptureValidationResult> validations =
                backend.collectOrdinary2CaptureValidations(true);
            if (validations.size() != 1u ||
                validations.front().validationId != 0u) {
                throw std::runtime_error(
                    "The requested Ordinary2 validation did not produce exactly one GPU readback result.");
            }
            const Ordinary2CaptureValidationResult& validation =
                validations.front();
            std::cout
                << "IRIDIUM_ORDINARY2_CAPTURE_VALIDATION {\"validation_id\":"
                << validation.validationId
                << ",\"atlas_extent\":[" << validation.atlasWidth << ','
                << validation.atlasHeight << ']'
                << ",\"expected_draws\":" << validation.expectedDrawCount
                << ",\"work_items\":" << validation.workItemCount
                << ",\"inspected_pixels\":" << validation.inspectedPixelCount
                << ",\"entry_pixels\":" << validation.entryPixelCount
                << ",\"exit_pixels\":" << validation.exitPixelCount
                << ",\"paired_pixels\":" << validation.pairedPixelCount
                << ",\"entry_only_pixels\":" << validation.entryOnlyPixelCount
                << ",\"invalid_work_index_pixels\":"
                << validation.invalidWorkIndexPixelCount
                << ",\"invalid_orientation_pixels\":"
                << validation.invalidOrientationPixelCount
                << ",\"unpaired_exit_pixels\":"
                << validation.unpairedExitPixelCount
                << ",\"work_mismatch_pixels\":"
                << validation.workMismatchPixelCount
                << ",\"invalid_depth_pixels\":"
                << validation.invalidDepthPixelCount
                << ",\"non_increasing_depth_pixels\":"
                << validation.nonIncreasingDepthPixelCount
                << ",\"minimum_paired_depth_delta\":"
                << validation.minimumPairedDepthDelta
                << ",\"maximum_paired_depth_delta\":"
                << validation.maximumPairedDepthDelta
                << ",\"local_color_pixels\":"
                << validation.localColorPixelCount
                << ",\"local_color_invalid_pixels\":"
                << validation.localColorInvalidPixelCount
                << ",\"minimum_local_alpha\":"
                << validation.minimumLocalAlpha
                << ",\"maximum_local_alpha\":"
                << validation.maximumLocalAlpha
                << ",\"passed\":"
                << (validation.passed() ? "true" : "false")
                << "}\n" << std::flush;
            if (!validation.passed()) {
                throw std::runtime_error(
                    "Ordinary2 GPU capture/local-color validation failed.");
            }
        }
        if (config.validateDeepLayeredCapture ||
            config.validateDeepLayeredLifecycle) {
            const std::vector<DeepLayeredCaptureValidationResult>
                validations = backend.
                    collectDeepLayeredCaptureValidations(true);
            if (validations.size() != 1u ||
                validations.front().validationId != 0u) {
                throw std::runtime_error(
                    "The requested deep layered validation did not produce exactly one GPU readback result.");
            }
            const DeepLayeredCaptureValidationResult& validation =
                validations.front();
            std::cout
                << "IRIDIUM_DEEP_LAYERED_CAPTURE_VALIDATION {\"validation_id\":"
                << validation.validationId
                << ",\"quality\":\""
                << transparencyQualityName(validation.quality) << '\"'
                << ",\"atlas_extent\":[" << validation.atlasWidth << ','
                << validation.atlasHeight << ']'
                << ",\"interface_count\":" << validation.interfaceCount
                << ",\"expected_draws\":" << validation.expectedDrawCount
                << ",\"scene_resolve_draws\":"
                << validation.sceneResolveDrawCount
                << ",\"compatibility_forward_draws\":"
                << validation.compatibilityForwardDrawCount
                << ",\"work_items\":" << validation.workItemCount
                << ",\"inspected_pixels\":" << validation.inspectedPixelCount
                << ",\"maximum_observed_interfaces\":"
                << validation.maximumObservedInterfaceCount
                << ",\"interface_pixels\":[";
            for (uint32_t interfaceIndex = 0u;
                interfaceIndex < validation.interfaceCount;
                ++interfaceIndex) {
                if (interfaceIndex != 0u) std::cout << ',';
                std::cout << validation.interfacePixelCounts[
                    interfaceIndex];
            }
            std::cout
                << "]"
                << ",\"paired_pixels\":" << validation.pairedPixelCount
                << ",\"nested_four_interface_pixels\":"
                << validation.nestedFourInterfacePixelCount
                << ",\"crossing_pair_pixels\":"
                << validation.crossingPairPixelCount
                << ",\"early_terminated_pixels\":"
                << validation.earlyTerminatedPixelCount
                << ",\"terminated_occupied_tiles\":"
                << validation.terminatedOccupiedTileCount
                << ",\"terminated_occupied_tiles_by_interface\":[";
            for (uint32_t interfaceIndex = 0u;
                interfaceIndex < validation.interfaceCount;
                ++interfaceIndex) {
                if (interfaceIndex != 0u) std::cout << ',';
                std::cout << validation.terminatedOccupiedTileCounts[
                    interfaceIndex];
            }
            std::cout
                << "]"
                << ",\"invalid_work_index_pixels\":"
                << validation.invalidWorkIndexPixelCount
                << ",\"invalid_orientation_pixels\":"
                << validation.invalidOrientationPixelCount
                << ",\"invalid_depth_pixels\":"
                << validation.invalidDepthPixelCount
                << ",\"non_increasing_depth_pixels\":"
                << validation.nonIncreasingDepthPixelCount
                << ",\"interface_gap_pixels\":"
                << validation.interfaceGapPixelCount
                << ",\"duplicate_entry_pixels\":"
                << validation.duplicateEntryPixelCount
                << ",\"unmatched_exit_pixels\":"
                << validation.unmatchedExitPixelCount
                << ",\"unclosed_entry_pixels\":"
                << validation.unclosedEntryPixelCount
                << ",\"saturated_residual_pixels\":"
                << validation.saturatedResidualPixelCount
                << ",\"minimum_depth_delta\":"
                << validation.minimumDepthDelta
                << ",\"maximum_depth_delta\":"
                << validation.maximumDepthDelta
                << ",\"local_color_pixels\":"
                << validation.localColorPixelCount
                << ",\"local_color_invalid_pixels\":"
                << validation.localColorInvalidPixelCount
                << ",\"minimum_local_alpha\":"
                << validation.minimumLocalAlpha
                << ",\"maximum_local_alpha\":"
                << validation.maximumLocalAlpha
                << ",\"passed\":"
                << (validation.passed() ? "true" : "false")
                << "}\n" << std::flush;
            if (!validation.passed()) {
                throw std::runtime_error(
                    "Deep layered GPU capture/local-color validation failed.");
            }
        }
        runtimeInfo_ = backend.getRuntimeInfo();
        if (config.validateDeepLayeredLifecycle) {
            const bool selectedTierResident =
                config.deepLayeredCaptureQuality ==
                        TransparencyQuality::Hero4
                    ? runtimeInfo_.hero4AtlasResident &&
                        runtimeInfo_.hero4AtlasWidth != 0u &&
                        runtimeInfo_.hero4AtlasHeight != 0u
                    : runtimeInfo_.cinematic8AtlasResident &&
                        runtimeInfo_.cinematic8AtlasWidth != 0u &&
                        runtimeInfo_.cinematic8AtlasHeight != 0u;
            const bool passed =
                deepLayeredLifecycleValidation_.phase ==
                    DeepLayeredLifecycleValidationState::Phase::Complete &&
                deepLayeredLifecycleValidation_.retirements == 2u &&
                deepLayeredLifecycleValidation_.reactivations == 2u &&
                deepLayeredLifecycleValidation_.visibilityChanges == 4u &&
                selectedTierResident &&
                runtimeInfo_.refractionPyramidsResident;
            std::cout
                << "IRIDIUM_DEEP_LAYERED_LIFECYCLE_VALIDATION {\"quality\":\""
                << transparencyQualityName(
                    config.deepLayeredCaptureQuality)
                << "\",\"retirements\":"
                << deepLayeredLifecycleValidation_.retirements
                << ",\"reactivations\":"
                << deepLayeredLifecycleValidation_.reactivations
                << ",\"visibility_changes\":"
                << deepLayeredLifecycleValidation_.visibilityChanges
                << ",\"completion_measured_frame\":"
                << deepLayeredLifecycleValidation_.completionMeasuredFrame
                << ",\"tier_resident\":"
                << (selectedTierResident ? "true" : "false")
                << ",\"refraction_pyramids_resident\":"
                << (runtimeInfo_.refractionPyramidsResident
                    ? "true" : "false")
                << ",\"passed\":" << (passed ? "true" : "false")
                << "}\n" << std::flush;
            if (!passed) {
                throw std::runtime_error(
                    "Deep layered tier lifecycle validation failed.");
            }
        }
        if (config.validateOrdinary2Resize) {
            const uint64_t rebuildDelta =
                runtimeInfo_.renderGraphRebuildCount >=
                    ordinary2ResizeValidation_.initialRenderGraphRebuildCount
                ? runtimeInfo_.renderGraphRebuildCount -
                    ordinary2ResizeValidation_.initialRenderGraphRebuildCount
                : 0u;
            const bool extentRestored =
                renderExtent.width ==
                    ordinary2ResizeValidation_.originalExtent.width &&
                renderExtent.height ==
                    ordinary2ResizeValidation_.originalExtent.height;
            const bool passed =
                ordinary2ResizeValidation_.requests == 3u &&
                ordinary2ResizeValidation_.successes == 3u &&
                ordinary2ResizeValidation_.failures == 0u &&
                extentRestored && rebuildDelta >= 3u &&
                runtimeInfo_.ordinary2AtlasResident &&
                runtimeInfo_.ordinary2AtlasWidth > 0u &&
                runtimeInfo_.ordinary2AtlasHeight > 0u &&
                runtimeInfo_.refractionPyramidsResident;
            std::cout
                << "IRIDIUM_ORDINARY2_RESIZE_VALIDATION {\"sequence\":[[960,540],[1600,900],[1280,720]]"
                << ",\"requests\":"
                << ordinary2ResizeValidation_.requests
                << ",\"successes\":"
                << ordinary2ResizeValidation_.successes
                << ",\"failures\":"
                << ordinary2ResizeValidation_.failures
                << ",\"render_graph_rebuild_delta\":" << rebuildDelta
                << ",\"final_render_extent\":[" << renderExtent.width
                << ',' << renderExtent.height << ']'
                << ",\"ordinary2_atlas_resident\":"
                << (runtimeInfo_.ordinary2AtlasResident
                    ? "true" : "false")
                << ",\"ordinary2_atlas_extent\":["
                << runtimeInfo_.ordinary2AtlasWidth << ','
                << runtimeInfo_.ordinary2AtlasHeight << ']'
                << ",\"refraction_pyramids_resident\":"
                << (runtimeInfo_.refractionPyramidsResident
                    ? "true" : "false")
                << ",\"passed\":" << (passed ? "true" : "false")
                << "}\n" << std::flush;
            if (!passed) {
                throw std::runtime_error(
                    "Populated Ordinary2 resize/lifecycle validation failed: " +
                    ordinary2ResizeValidation_.lastDiagnostic);
            }
        }
        if (config.validateWeightedOitResize) {
            const uint64_t rebuildDelta =
                runtimeInfo_.renderGraphRebuildCount >=
                    weightedOitResizeValidation_.initialRenderGraphRebuildCount
                ? runtimeInfo_.renderGraphRebuildCount -
                    weightedOitResizeValidation_.initialRenderGraphRebuildCount
                : 0u;
            const bool extentRestored = renderExtent.width ==
                    weightedOitResizeValidation_.originalExtent.width &&
                renderExtent.height ==
                    weightedOitResizeValidation_.originalExtent.height;
            const bool passed =
                weightedOitResizeValidation_.requests == 3u &&
                weightedOitResizeValidation_.successes == 3u &&
                weightedOitResizeValidation_.failures == 0u &&
                extentRestored && rebuildDelta >= 3u &&
                runtimeInfo_.weightedOitResident &&
                !runtimeInfo_.refractionPyramidsResident;
            std::cout
                << "IRIDIUM_WEIGHTED_OIT_RESIZE_VALIDATION {\"sequence\":[[960,540],[1600,900],[1280,720]]"
                << ",\"requests\":" << weightedOitResizeValidation_.requests
                << ",\"successes\":" << weightedOitResizeValidation_.successes
                << ",\"failures\":" << weightedOitResizeValidation_.failures
                << ",\"render_graph_rebuild_delta\":" << rebuildDelta
                << ",\"final_render_extent\":[" << renderExtent.width
                << ',' << renderExtent.height << ']'
                << ",\"weighted_oit_resident\":"
                << (runtimeInfo_.weightedOitResident
                    ? "true" : "false")
                << ",\"refraction_pyramids_resident\":"
                << (runtimeInfo_.refractionPyramidsResident
                    ? "true" : "false")
                << ",\"passed\":" << (passed ? "true" : "false")
                << "}\n" << std::flush;
            if (!passed) {
                throw std::runtime_error(
                    "Populated WeightedOIT resize validation failed: " +
                    weightedOitResizeValidation_.lastDiagnostic);
            }
        }
        if (config.validateDepthPyramidResize) {
            runtimeInfo_ = backend.getRuntimeInfo();
            const uint64_t rebuildDelta =
                runtimeInfo_.renderGraphRebuildCount >=
                    depthPyramidResizeValidation_.initialRenderGraphRebuildCount
                ? runtimeInfo_.renderGraphRebuildCount -
                    depthPyramidResizeValidation_.initialRenderGraphRebuildCount
                : 0u;
            const bool extentRestored = renderExtent.width ==
                    depthPyramidResizeValidation_.originalExtent.width &&
                renderExtent.height ==
                    depthPyramidResizeValidation_.originalExtent.height;
            const bool passed =
                depthPyramidResizeValidation_.requests == 3u &&
                depthPyramidResizeValidation_.successes == 3u &&
                depthPyramidResizeValidation_.failures == 0u &&
                extentRestored && rebuildDelta >= 3u;
            std::cout
                << "IRIDIUM_DEPTH_PYRAMID_RESIZE_VALIDATION {\"sequence\":[[960,540],[1600,900],[1280,720]]"
                << ",\"requests\":"
                << depthPyramidResizeValidation_.requests
                << ",\"successes\":"
                << depthPyramidResizeValidation_.successes
                << ",\"failures\":"
                << depthPyramidResizeValidation_.failures
                << ",\"render_graph_rebuild_delta\":" << rebuildDelta
                << ",\"final_render_extent\":[" << renderExtent.width
                << ',' << renderExtent.height << ']'
                << ",\"passed\":" << (passed ? "true" : "false")
                << "}\n" << std::flush;
            if (!passed) {
                throw std::runtime_error(
                    "Depth-pyramid resize/lifecycle validation failed: " +
                    depthPyramidResizeValidation_.lastDiagnostic);
            }
        }
        if (config.validateOrdinary2Fallback) {
            const Ordinary2FallbackModelStats stats =
                ordinary2FallbackModelStats(*context.run.mainModel);
            const bool passed = stats.transparentSubmeshes > 0u &&
                stats.requestedLayeredCandidates ==
                    stats.transparentSubmeshes &&
                stats.fallbackThinGlassSubmeshes ==
                    stats.transparentSubmeshes &&
                stats.fallbackFlaggedSubmeshes ==
                    stats.transparentSubmeshes &&
                stats.topologyRequiredSubmeshes ==
                    stats.transparentSubmeshes &&
                stats.layeredGlassSubmeshes == 0u &&
                !runtimeInfo_.ordinary2AtlasResident &&
                runtimeInfo_.ordinary2AtlasWidth == 0u &&
                runtimeInfo_.ordinary2AtlasHeight == 0u &&
                runtimeInfo_.refractionPyramidsResident;
            std::cout
                << "IRIDIUM_ORDINARY2_FALLBACK_VALIDATION {\"transparent_submeshes\":"
                << stats.transparentSubmeshes
                << ",\"requested_layered_candidates\":"
                << stats.requestedLayeredCandidates
                << ",\"fallback_thin_glass_submeshes\":"
                << stats.fallbackThinGlassSubmeshes
                << ",\"fallback_flagged_submeshes\":"
                << stats.fallbackFlaggedSubmeshes
                << ",\"topology_required_submeshes\":"
                << stats.topologyRequiredSubmeshes
                << ",\"layered_glass_submeshes\":"
                << stats.layeredGlassSubmeshes
                << ",\"ordinary2_atlas_resident\":"
                << (runtimeInfo_.ordinary2AtlasResident
                    ? "true" : "false")
                << ",\"ordinary2_atlas_extent\":["
                << runtimeInfo_.ordinary2AtlasWidth << ','
                << runtimeInfo_.ordinary2AtlasHeight << ']'
                << ",\"refraction_pyramids_resident\":"
                << (runtimeInfo_.refractionPyramidsResident
                    ? "true" : "false")
                << ",\"passed\":" << (passed ? "true" : "false")
                << "}\n" << std::flush;
            if (!passed) {
                throw std::runtime_error(
                    "Invalid-topology Ordinary2 fallback validation failed.");
            }
        }
    }

    void QualificationHarness::releaseProbeResources(
        AppShutdownContext& context) {
        IRenderBackend* backend = context.backend;
        if (backend) {
            for (TextureHandle texture : textureScaleProbeTextures_) {
                backend->freeTexture(texture);
            }
            textureScaleProbeTextures_.clear();
            for (MaterialHandle material : materialScaleProbeMaterials_) {
                backend->freeMaterial(material);
            }
            materialScaleProbeMaterials_.clear();
            if (materialScaleProbeTexture_.isValid()) {
                backend->freeTexture(materialScaleProbeTexture_);
                materialScaleProbeTexture_ = {};
            }
        }
        if (backend && residencyProbeTexture_.isValid()) {
            backend->freeTexture(residencyProbeTexture_);
            residencyProbeTexture_ = {};
        }
        if (backend && residencyReplacementTexture_.isValid()) {
            backend->freeTexture(residencyReplacementTexture_);
            residencyReplacementTexture_ = {};
        }
        residencyProbePixels_.clear();
    }

} // namespace Iridium
