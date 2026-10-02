#include "app/Application.h"
#include "renderer/rhi/TransparencyQualityOverride.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <bit>

#include "profiling/CpuProfileExport.h"
#include "profiling/CpuAllocationProfile.h"
#include "renderer/rhi/RenderBackendFactory.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/RenderInstanceBatchComponent.h"
#include "scene/components/LightComponent.h"
#include "scene/components/NameComponent.h"
#include "scene/components/SkyComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/transparency/WeightedOit.h"
#include "imgui.h"
#include "utils/Sha256.h"
#include "capture/CaptureArtifact.h"
#include "platform/SystemProfile.h"
#include "renderer/color/AcesOutputLut.h"
#include "assets/AssetDiscovery.h"
#include "assets/SqliteAssetCatalog.h"
#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookKey.h"
#include "assets/cooker/CookReceipt.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/cooker/TextFixtureImporter.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/texture/TextureImporter.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "editor/EditorPreviewImageFit.h"
#include "assets/environment/EnvironmentProduct.h"
#include "editor/EditorSceneActions.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#ifndef IRIDIUM_BUILD_CONFIGURATION
#define IRIDIUM_BUILD_CONFIGURATION "unknown"
#endif

#ifndef IRIDIUM_SOURCE_COMMIT
#define IRIDIUM_SOURCE_COMMIT "unknown"
#endif

#ifndef IRIDIUM_SOURCE_BRANCH
#define IRIDIUM_SOURCE_BRANCH "unknown"
#endif

#ifndef IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE
#define IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE 0
#endif

#ifndef IRIDIUM_COMPILER
#define IRIDIUM_COMPILER "unavailable"
#endif

#ifndef IRIDIUM_SHADER_COMPILER
#define IRIDIUM_SHADER_COMPILER "unavailable"
#endif

#ifndef IRIDIUM_VULKAN_SDK
#define IRIDIUM_VULKAN_SDK "unavailable"
#endif

namespace Iridium {

    namespace {
        constexpr uint64_t
            EditorRuntimeUploadBudgetBytes =
                128ull * 1024ull * 1024ull;
        // Model publication is still atomic today, but the per-frame admission
        // budget is not a product-size limit. Permit one oversized model when
        // the queue is otherwise idle while retaining a high-end-editor safety
        // cap until progressive geometry/texture residency lands.
        constexpr uint64_t EditorModelPublicationLimitBytes =
            1024ull * 1024ull * 1024ull;
        // One atomic Cinematic 2048/2048 environment is about 512 MiB. Keep a
        // bounded margin for product metadata without silently allowing
        // arbitrary oversized model/texture publications.
        constexpr uint64_t EditorEnvironmentPublicationLimitBytes =
            640ull * 1024ull * 1024ull;
        constexpr std::string_view Aces2TransformId =
            "urn:ampas:aces:transformId:v2.0:Output.Academy.Rec709-D65_100nit_in_Rec709-D65_sRGB-Piecewise.a2.v1";
        constexpr std::string_view Aces2HdrTransformId =
            "urn:ampas:aces:transformId:v2.0:Output.Academy.P3-D65_1000nit_in_Rec2100-D65_ST2084.a2.v1";

        struct Ordinary2FallbackModelStats {
            uint32_t transparentSubmeshes = 0;
            uint32_t requestedLayeredCandidates = 0;
            uint32_t fallbackThinGlassSubmeshes = 0;
            uint32_t fallbackFlaggedSubmeshes = 0;
            uint32_t topologyRequiredSubmeshes = 0;
            uint32_t layeredGlassSubmeshes = 0;
        };

        Ordinary2FallbackModelStats ordinary2FallbackModelStats(
            const ModelAsset& model) noexcept {
            Ordinary2FallbackModelStats result{};
            for (const SubMesh& subMesh : model.subMeshes) {
                if (subMesh.materialIndex < 0 ||
                    static_cast<size_t>(subMesh.materialIndex) >=
                        model.materials.size() ||
                    model.materials[static_cast<size_t>(
                        subMesh.materialIndex)].renderQueue !=
                        RenderQueue::Transparent) {
                    continue;
                }
                ++result.transparentSubmeshes;
                if (subMesh.transparency.requestedClass ==
                        TransparencyClass::Auto ||
                    subMesh.transparency.requestedClass ==
                        TransparencyClass::LayeredGlass) {
                    ++result.requestedLayeredCandidates;
                }
                if (subMesh.transparency.resolvedClass ==
                        TransparencyClass::ThinGlass) {
                    ++result.fallbackThinGlassSubmeshes;
                }
                if ((subMesh.transparency.flags &
                        CompiledTransparencyFallbackApplied) != 0u) {
                    ++result.fallbackFlaggedSubmeshes;
                }
                if ((subMesh.transparency.flags &
                        CompiledTransparencyTopologyRequired) != 0u) {
                    ++result.topologyRequiredSubmeshes;
                }
                if (subMesh.transparency.resolvedClass ==
                        TransparencyClass::LayeredGlass) {
                    ++result.layeredGlassSubmeshes;
                }
            }
            return result;
        }

        std::string_view outputOperatorName(OutputTransformOperator value) {
            switch (value) {
            case OutputTransformOperator::Aces2: return "aces2";
            case OutputTransformOperator::AcesFittedLegacy:
                return "aces_fitted_legacy";
            case OutputTransformOperator::IdentityClampDiagnostic:
                return "identity_clamp_diagnostic";
            }
            return "unknown";
        }

        std::string_view gamutMappingName(OutputTransformOperator value) {
            switch (value) {
            case OutputTransformOperator::Aces2:
                return "aces2_jmh_chroma_and_gamut_compression_lut128_tetrahedral";
            case OutputTransformOperator::AcesFittedLegacy:
                return "ap1_to_rec709_matrix_then_clip_negative";
            case OutputTransformOperator::IdentityClampDiagnostic:
                return "ap1_to_rec709_matrix_then_clamp_diagnostic";
            }
            return "unknown";
        }

        std::string_view transformId(OutputTransformOperator value,
            Color::OutputTransport transport) {
            return value == OutputTransformOperator::Aces2
                ? (transport == Color::OutputTransport::SdrSrgb
                    ? Aces2TransformId : Aces2HdrTransformId)
                : (value == OutputTransformOperator::AcesFittedLegacy
                    ? "legacy_fitted_compatibility" : "identity_clamp_diagnostic");
        }

        struct ModelSourceReimportContext {
            std::filesystem::path assetRoot;
            std::filesystem::path sourceRelativePath;
            std::filesystem::path metadataPath;
            ImporterRegistry importers;
            std::shared_ptr<LocalDerivedDataCache>
                cache;
            CookTarget target;
        };

        std::string cookFailureMessage(
            std::string_view prefix,
            std::span<const CookDiagnostic>
                diagnostics) {
            std::string message(prefix);
            for (const CookDiagnostic& diagnostic :
                diagnostics) {
                if (diagnostic.severity ==
                    CookDiagnosticSeverity::Error) {
                    message += ": " +
                        diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            return message;
        }

        template <typename T>
        uint64_t shadowRevision(const T& value) noexcept {
            uint64_t hash = 1469598103934665603ull;
            const auto bytes = std::as_bytes(std::span{ &value, size_t{ 1 } });
            for (const std::byte byte : bytes) {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash == 0u ? 1u : hash;
        }

        template <typename T>
        void appendCaptureRevision(uint64_t& hash, const T& value) noexcept {
            const auto bytes = std::as_bytes(std::span{ &value, size_t{ 1 } });
            for (const std::byte byte : bytes) {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
        }

        uint64_t reflectionProbeSettingsRevision(
            const ReflectionProbeCandidate& candidate) noexcept {
            uint64_t hash = 1469598103934665603ull;
            appendCaptureRevision(hash, candidate.probeToWorld);
            appendCaptureRevision(hash, candidate.probe.captureResolution);
            appendCaptureRevision(hash, candidate.probe.captureNearMeters);
            appendCaptureRevision(hash, candidate.probe.captureFarMeters);
            appendCaptureRevision(hash, candidate.probe.captureSky);
            appendCaptureRevision(hash, candidate.probe.updateMode);
            return hash == 0u ? 1u : hash;
        }

        uint64_t reflectionProbeLightingRevision(
            const LightingFramePacket& lights) noexcept {
            uint64_t hash = 1469598103934665603ull;
            for (uint64_t revision : lights.recordRevisions)
                appendCaptureRevision(hash, revision);
            appendCaptureRevision(hash, lights.activeListRevision);
            return hash == 0u ? 1u : hash;
        }

        bool writeBinaryAtomic(const std::filesystem::path& destination,
            std::span<const std::byte> bytes, std::string& error) {
            error.clear();
            std::error_code filesystemError;
            std::filesystem::create_directories(destination.parent_path(),
                filesystemError);
            if (filesystemError) {
                error = "Could not create baked-probe directory: " +
                    filesystemError.message();
                return false;
            }
            std::filesystem::path temporary = destination;
            temporary += ".tmp";
            {
                std::ofstream output(temporary,
                    std::ios::binary | std::ios::trunc);
                if (!output) {
                    error = "Could not open the temporary baked-probe product.";
                    return false;
                }
                output.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                output.flush();
                if (!output) {
                    error = "Could not write the temporary baked-probe product.";
                    output.close();
                    std::filesystem::remove(temporary, filesystemError);
                    return false;
                }
            }
#if defined(_WIN32)
            if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
                return true;
            error = "Could not atomically publish the baked-probe product (Windows " +
                std::to_string(GetLastError()) + ").";
#else
            if (std::rename(temporary.c_str(), destination.c_str()) == 0)
                return true;
            error = "Could not atomically publish the baked-probe product.";
#endif
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
    }

    Application::Application(ApplicationConfig config)
        : config_(std::move(config)),
          cpuProfiler_(config_.enableCpuProfiling),
          reflectionProbeCaptureScheduler_({
              .maximumRenderedTexels = config_.reflectionProbeSettings.
                  maximumRenderedTexelsPerFrame,
              .maximumFacesPerProbePerFrame = config_.reflectionProbeSettings.
                  maximumFacesPerProbePerFrame,
              .maximumCapturesInFlight = config_.reflectionProbeSettings.
                  maximumCapturesInFlight,
              .minimumRealtimeFramesBetweenCaptures =
                  config_.reflectionProbeSettings.
                      minimumRealtimeFramesBetweenCaptures }),
          spotShadowAtlas_({
              .atlasResolution = config_.shadowSettings.spotAtlasResolution,
              .minimumTileResolution = 512,
              .guardTexels = 4 }),
          spotShadowCache_({
              .maximumRenderedTexels = config_.shadowSettings.
                  maximumSpotRenderedTexelsPerFrame,
              .maximumCompatibleStaleFrames = config_.shadowSettings.
                  maximumCompatibleSpotStaleFrames }),
          pointShadowPools_({ .cubeCapacity = {
              config_.shadowSettings.pointPool256Capacity,
              config_.shadowSettings.pointPool512Capacity,
              config_.shadowSettings.pointPool1024Capacity } }),
          pointShadowCache_({
              .maximumRenderedTexels = config_.shadowSettings.
                  maximumPointRenderedTexelsPerFrame,
              .maximumCompatibleStaleFrames = config_.shadowSettings.
                  maximumCompatiblePointStaleFrames }),
          sceneDocumentService_(sceneWorld_),
          transactionService_(sceneDocumentService_),
          registry(sceneWorld_.registry()) {}

    void Application::run() {
        std::optional<CaptureArtifactPaths> captureArtifact;
        try {
            const auto startupStart = std::chrono::steady_clock::now();
            const auto windowStart = std::chrono::steady_clock::now();
            initWindow();
            startupProfile_.windowNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - windowStart).count());
            initRenderer();
            startupProfile_.totalNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - startupStart).count());
            mainLoop();
            if (config_.captureFrameIndex) {
                std::vector<FrameCapture> captures =
                    renderBackend->collectFrameCaptures(true);
                if (captures.size() != 1 ||
                    captures.front().captureId != *config_.captureFrameIndex) {
                    throw std::runtime_error(
                        "The requested measured frame did not produce exactly one capture.");
                }
                completedCapture_ = std::move(captures.front());
            }
            if (config_.validateDepthPyramidCapture) {
                const auto validations =
                    renderBackend->collectDepthPyramidCaptureValidations(true);
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
            if (config_.validateOrdinary2Capture ||
                config_.validateOrdinary2Resize) {
                const std::vector<Ordinary2CaptureValidationResult> validations =
                    renderBackend->collectOrdinary2CaptureValidations(true);
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
            if (config_.validateDeepLayeredCapture ||
                config_.validateDeepLayeredLifecycle) {
                const std::vector<DeepLayeredCaptureValidationResult>
                    validations = renderBackend->
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
            renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
            if (config_.validateDeepLayeredLifecycle) {
                const bool selectedTierResident =
                    config_.deepLayeredCaptureQuality ==
                            TransparencyQuality::Hero4
                        ? renderRuntimeInfo_.hero4AtlasResident &&
                            renderRuntimeInfo_.hero4AtlasWidth != 0u &&
                            renderRuntimeInfo_.hero4AtlasHeight != 0u
                        : renderRuntimeInfo_.cinematic8AtlasResident &&
                            renderRuntimeInfo_.cinematic8AtlasWidth != 0u &&
                            renderRuntimeInfo_.cinematic8AtlasHeight != 0u;
                const bool passed =
                    deepLayeredLifecycleValidation_.phase ==
                        DeepLayeredLifecycleValidationState::Phase::Complete &&
                    deepLayeredLifecycleValidation_.retirements == 2u &&
                    deepLayeredLifecycleValidation_.reactivations == 2u &&
                    deepLayeredLifecycleValidation_.visibilityChanges == 4u &&
                    selectedTierResident &&
                    renderRuntimeInfo_.refractionPyramidsResident;
                std::cout
                    << "IRIDIUM_DEEP_LAYERED_LIFECYCLE_VALIDATION {\"quality\":\""
                    << transparencyQualityName(
                        config_.deepLayeredCaptureQuality)
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
                    << (renderRuntimeInfo_.refractionPyramidsResident
                        ? "true" : "false")
                    << ",\"passed\":" << (passed ? "true" : "false")
                    << "}\n" << std::flush;
                if (!passed) {
                    throw std::runtime_error(
                        "Deep layered tier lifecycle validation failed.");
                }
            }
            if (config_.validateOrdinary2Resize) {
                const uint64_t rebuildDelta =
                    renderRuntimeInfo_.renderGraphRebuildCount >=
                        ordinary2ResizeValidation_.initialRenderGraphRebuildCount
                    ? renderRuntimeInfo_.renderGraphRebuildCount -
                        ordinary2ResizeValidation_.initialRenderGraphRebuildCount
                    : 0u;
                const bool extentRestored =
                    renderExtent_.width ==
                        ordinary2ResizeValidation_.originalExtent.width &&
                    renderExtent_.height ==
                        ordinary2ResizeValidation_.originalExtent.height;
                const bool passed =
                    ordinary2ResizeValidation_.requests == 3u &&
                    ordinary2ResizeValidation_.successes == 3u &&
                    ordinary2ResizeValidation_.failures == 0u &&
                    extentRestored && rebuildDelta >= 3u &&
                    renderRuntimeInfo_.ordinary2AtlasResident &&
                    renderRuntimeInfo_.ordinary2AtlasWidth > 0u &&
                    renderRuntimeInfo_.ordinary2AtlasHeight > 0u &&
                    renderRuntimeInfo_.refractionPyramidsResident;
                std::cout
                    << "IRIDIUM_ORDINARY2_RESIZE_VALIDATION {\"sequence\":[[960,540],[1600,900],[1280,720]]"
                    << ",\"requests\":"
                    << ordinary2ResizeValidation_.requests
                    << ",\"successes\":"
                    << ordinary2ResizeValidation_.successes
                    << ",\"failures\":"
                    << ordinary2ResizeValidation_.failures
                    << ",\"render_graph_rebuild_delta\":" << rebuildDelta
                    << ",\"final_render_extent\":[" << renderExtent_.width
                    << ',' << renderExtent_.height << ']'
                    << ",\"ordinary2_atlas_resident\":"
                    << (renderRuntimeInfo_.ordinary2AtlasResident
                        ? "true" : "false")
                    << ",\"ordinary2_atlas_extent\":["
                    << renderRuntimeInfo_.ordinary2AtlasWidth << ','
                    << renderRuntimeInfo_.ordinary2AtlasHeight << ']'
                    << ",\"refraction_pyramids_resident\":"
                    << (renderRuntimeInfo_.refractionPyramidsResident
                        ? "true" : "false")
                    << ",\"passed\":" << (passed ? "true" : "false")
                    << "}\n" << std::flush;
                if (!passed) {
                    throw std::runtime_error(
                        "Populated Ordinary2 resize/lifecycle validation failed: " +
                        ordinary2ResizeValidation_.lastDiagnostic);
                }
            }
            if (config_.validateWeightedOitResize) {
                const uint64_t rebuildDelta =
                    renderRuntimeInfo_.renderGraphRebuildCount >=
                        weightedOitResizeValidation_.initialRenderGraphRebuildCount
                    ? renderRuntimeInfo_.renderGraphRebuildCount -
                        weightedOitResizeValidation_.initialRenderGraphRebuildCount
                    : 0u;
                const bool extentRestored = renderExtent_.width ==
                        weightedOitResizeValidation_.originalExtent.width &&
                    renderExtent_.height ==
                        weightedOitResizeValidation_.originalExtent.height;
                const bool passed =
                    weightedOitResizeValidation_.requests == 3u &&
                    weightedOitResizeValidation_.successes == 3u &&
                    weightedOitResizeValidation_.failures == 0u &&
                    extentRestored && rebuildDelta >= 3u &&
                    renderRuntimeInfo_.weightedOitResident &&
                    !renderRuntimeInfo_.refractionPyramidsResident;
                std::cout
                    << "IRIDIUM_WEIGHTED_OIT_RESIZE_VALIDATION {\"sequence\":[[960,540],[1600,900],[1280,720]]"
                    << ",\"requests\":" << weightedOitResizeValidation_.requests
                    << ",\"successes\":" << weightedOitResizeValidation_.successes
                    << ",\"failures\":" << weightedOitResizeValidation_.failures
                    << ",\"render_graph_rebuild_delta\":" << rebuildDelta
                    << ",\"final_render_extent\":[" << renderExtent_.width
                    << ',' << renderExtent_.height << ']'
                    << ",\"weighted_oit_resident\":"
                    << (renderRuntimeInfo_.weightedOitResident
                        ? "true" : "false")
                    << ",\"refraction_pyramids_resident\":"
                    << (renderRuntimeInfo_.refractionPyramidsResident
                        ? "true" : "false")
                    << ",\"passed\":" << (passed ? "true" : "false")
                    << "}\n" << std::flush;
                if (!passed) {
                    throw std::runtime_error(
                        "Populated WeightedOIT resize validation failed: " +
                        weightedOitResizeValidation_.lastDiagnostic);
                }
            }
            if (config_.validateDepthPyramidResize) {
                renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
                const uint64_t rebuildDelta =
                    renderRuntimeInfo_.renderGraphRebuildCount >=
                        depthPyramidResizeValidation_.initialRenderGraphRebuildCount
                    ? renderRuntimeInfo_.renderGraphRebuildCount -
                        depthPyramidResizeValidation_.initialRenderGraphRebuildCount
                    : 0u;
                const bool extentRestored = renderExtent_.width ==
                        depthPyramidResizeValidation_.originalExtent.width &&
                    renderExtent_.height ==
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
                    << ",\"final_render_extent\":[" << renderExtent_.width
                    << ',' << renderExtent_.height << ']'
                    << ",\"passed\":" << (passed ? "true" : "false")
                    << "}\n" << std::flush;
                if (!passed) {
                    throw std::runtime_error(
                        "Depth-pyramid resize/lifecycle validation failed: " +
                        depthPyramidResizeValidation_.lastDiagnostic);
                }
            }
            if (config_.validateOrdinary2Fallback) {
                const Ordinary2FallbackModelStats stats =
                    ordinary2FallbackModelStats(*mainModel);
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
                    !renderRuntimeInfo_.ordinary2AtlasResident &&
                    renderRuntimeInfo_.ordinary2AtlasWidth == 0u &&
                    renderRuntimeInfo_.ordinary2AtlasHeight == 0u &&
                    renderRuntimeInfo_.refractionPyramidsResident;
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
                    << (renderRuntimeInfo_.ordinary2AtlasResident
                        ? "true" : "false")
                    << ",\"ordinary2_atlas_extent\":["
                    << renderRuntimeInfo_.ordinary2AtlasWidth << ','
                    << renderRuntimeInfo_.ordinary2AtlasHeight << ']'
                    << ",\"refraction_pyramids_resident\":"
                    << (renderRuntimeInfo_.refractionPyramidsResident
                        ? "true" : "false")
                    << ",\"passed\":" << (passed ? "true" : "false")
                    << "}\n" << std::flush;
                if (!passed) {
                    throw std::runtime_error(
                        "Invalid-topology Ordinary2 fallback validation failed.");
                }
            }
        }
        catch (...) {
            cleanup();
            throw;
        }
        cleanup();

        const SystemProfile systemProfile = querySystemProfile();

        if (completedCapture_) {
            CaptureArtifactMetadata captureMetadata{};
            captureMetadata.requireSpatialSignal = config_.requireCaptureSignal;
            captureMetadata.buildConfiguration = IRIDIUM_BUILD_CONFIGURATION;
            captureMetadata.sourceCommit = IRIDIUM_SOURCE_COMMIT;
            captureMetadata.sourceBranch = IRIDIUM_SOURCE_BRANCH;
            captureMetadata.sourceDirtyAtConfigure =
                IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE != 0;
            captureMetadata.validationEnabled = config_.enableValidation;
            captureMetadata.cpuProfilingEnabled = cpuProfiler_.isEnabled();
            captureMetadata.gpuProfilingRequested = config_.enableGpuProfiling;
            captureMetadata.gpuProfilingAvailable = config_.enableGpuProfiling &&
                renderCapabilities_.gpuTimestampProfiling;
            captureMetadata.windowVisible = config_.windowVisible;
            captureMetadata.windowDecorated = config_.windowDecorated;
            captureMetadata.compiler = IRIDIUM_COMPILER;
            captureMetadata.shaderCompiler = IRIDIUM_SHADER_COMPILER;
            captureMetadata.operatingSystem = systemProfile.operatingSystem;
            captureMetadata.cpuName = systemProfile.cpuName;
            captureMetadata.systemMemoryBytes = systemProfile.physicalMemoryBytes;
            captureMetadata.gpuName = renderRuntimeInfo_.gpuName;
            captureMetadata.gpuUuid = renderRuntimeInfo_.gpuUuid;
            captureMetadata.gpuDriver = renderRuntimeInfo_.driverName + " " +
                renderRuntimeInfo_.driverVersion;
            captureMetadata.vulkanDeviceApiVersion =
                renderRuntimeInfo_.vulkanDeviceApiVersion;
            captureMetadata.vulkanLoaderApiVersion =
                renderRuntimeInfo_.vulkanLoaderApiVersion;
            captureMetadata.vulkanSdkVersion = IRIDIUM_VULKAN_SDK;
            captureMetadata.applicationEnabledLayers =
                renderRuntimeInfo_.applicationEnabledLayers;
            captureMetadata.activeTools = renderRuntimeInfo_.activeTools;
            captureMetadata.swapchainFormat = renderRuntimeInfo_.swapchainFormat;
            captureMetadata.swapchainColorSpace =
                renderRuntimeInfo_.swapchainColorSpace;
            captureMetadata.presentMode = renderRuntimeInfo_.presentMode;
            captureMetadata.outputMode = renderRuntimeInfo_.outputMode;
            captureMetadata.reconstructionMode =
                renderRuntimeInfo_.reconstructionMode;
            captureMetadata.qualitySettings =
                "m5_directional_shadow_" + std::to_string(
                    config_.shadowSettings.directionalResolution) +
                "_d32_4c_" + std::to_string(
                    config_.shadowSettings.maximumDirectionalLights) +
                "l_5x5_tent";
            captureMetadata.qualitySettings += config_.forceDirectGBufferReference
                ? "_gbuffer_direct_unculled_reference" : "_gbuffer_automatic";
            captureMetadata.qualitySettings +=
                (config_.forceDirectGBufferReference ||
                    config_.forceDirectShadowReference)
                ? "_shadow_direct_reference" : "_shadow_automatic";
            captureMetadata.qualitySettings +=
                (config_.forceDirectGBufferReference ||
                    config_.forceDirectProbeCaptureReference)
                ? "_probe_capture_direct_reference"
                : "_probe_capture_automatic";
            captureMetadata.qualitySettings += "_shadow_indirect_oracle_" +
                std::to_string(config_.shadowIndirectQualificationOracle);
            captureMetadata.qualitySettings += "_shadow_lod_texels_" +
                std::to_string(config_.experimentalShadowLodErrorTexels) +
                "_max_" + std::to_string(config_.shadowLodMaximumLevel);
            captureMetadata.qualitySettings += "_experimental_lod_px_" +
                std::to_string(config_.experimentalGpuLodErrorPixels) + "_max_" +
                std::to_string(config_.gpuLodMaximumLevel) + "_hysteresis_" +
                std::to_string(config_.gpuLodHysteresisFraction) + "_oracle_" +
                std::to_string(config_.gpuLodQualificationOracle) + "_resident_floor_" +
                std::to_string(config_.gpuLodMinimumResidentLevel);
            captureMetadata.qualitySettings += "_probe_lod_px_" +
                std::to_string(config_.experimentalProbeLodErrorPixels) +
                "_max_" + std::to_string(config_.probeLodMaximumLevel) +
                "_oracle_" + std::to_string(config_.probeLodQualificationOracle);
            captureMetadata.qualitySettings += "_depth_pyramid_" +
                std::to_string(config_.experimentalDepthPyramid) +
                "_occlusion_query_" +
                std::to_string(config_.experimentalDepthOcclusionQuery) +
                "_occlusion_rejection_" +
                std::to_string(config_.experimentalDepthOcclusionRejection);
            captureMetadata.qualitySettings += "_occlusion_oracle_" +
                std::to_string(config_.depthOcclusionQualificationOracle);
            captureMetadata.qualitySettings += "_fixture_lights_" +
                std::to_string(activeBenchmark_
                    ? activeBenchmark_->lights.size() : 0u);
            captureMetadata.cacheState = config_.cacheState;
            captureMetadata.outputOperator = outputOperatorName(config_.outputOperator);
            captureMetadata.manualExposureEv = config_.manualExposureEv;
            captureMetadata.gamutMapping = gamutMappingName(config_.outputOperator);
            const Color::OutputTransport effectiveOutputTransport =
                renderRuntimeInfo_.effectiveOutputTransportMode;
            if (effectiveOutputTransport == Color::OutputTransport::SdrSrgb) {
                captureMetadata.displayProfile = "windows_sdr_rec709_srgb";
                captureMetadata.outputTransfer = "iec_61966_2_1_srgb";
                captureMetadata.paperWhiteNits = 100.0;
                captureMetadata.peakNits = 100.0;
            }
            else {
                captureMetadata.displayProfile = effectiveOutputTransport ==
                    Color::OutputTransport::ScRgb
                    ? "windows_scrgb_extended_srgb_linear"
                    : "windows_hdr10_rec2100_pq";
                captureMetadata.outputTransfer = effectiveOutputTransport ==
                    Color::OutputTransport::ScRgb ? "linear" : "st2084_pq";
                captureMetadata.paperWhiteNits = config_.paperWhiteNits;
                captureMetadata.peakNits = config_.peakNits;
            }
            captureMetadata.acesPackageVersion = "v2.0.0+2025.04.04";
            captureMetadata.acesTransformId = transformId(config_.outputOperator,
                effectiveOutputTransport);
            captureMetadata.measuredFrameIndex = *config_.captureFrameIndex;
            captureMetadata.applicationFrameIndex =
                capturedApplicationFrameIndex_.value_or(0);
            captureMetadata.benchmarkStateFrameIndex =
                captureMetadata.applicationFrameIndex;
            captureMetadata.warmupFrameCount = config_.warmupFrameCount;
            captureMetadata.debugView = config_.forceWireframe
                ? "wireframe"
                : std::string(renderDebugViewName(config_.debugView));
            captureMetadata.debugViewSemantics = config_.forceWireframe
                ? "editor opaque geometry in wireframe with normal forward composition"
                : std::string(renderDebugViewDescription(config_.debugView));
            if (activeBenchmark_) {
                captureMetadata.fixtureId = activeBenchmark_->id;
                captureMetadata.fixtureRevision = activeBenchmark_->revision;
                captureMetadata.cameraId = activeBenchmark_->camera.id;
                captureMetadata.manifestPath = benchmarkManifestPath_;
                captureMetadata.manifestSha256 = benchmarkManifestSha256_;
                for (const BenchmarkContentFile& file :
                    activeBenchmark_->contentFiles) {
                    captureMetadata.contentHashes.emplace_back(
                        file.relativePath.generic_string(), file.sha256);
                }
            }
            captureMetadata.modelLoadMode =
                config_.cookedModelArtifact.empty()
                ? "source-import"
                : "self-contained-cooked-artifact";
            if (mainModel) {
                captureMetadata.modelLocation =
                    mainModel->filePath;
                captureMetadata.modelAssetGuid =
                    mainModel->assetGuid.isNil()
                    ? "" : mainModel->assetGuid.toString();
                captureMetadata.modelArtifactCookKey =
                    mainModel->artifactCookKey;
            }
            captureMetadata.environmentLoadMode =
                activeCookedEnvironmentArtifact_.empty()
                // Metadata is emitted after cleanup retires GPU handles; use
                // retained publication identity, not the cleared lighting handle.
                ? (activeBenchmark_ && !activeEnvironmentAssetGuid_.isNil()
                    ? "benchmark-procedural-constant" : "neutral-black-fallback")
                : "self-contained-cooked-artifact";
            captureMetadata.environmentLocation =
                activeCookedEnvironmentArtifact_.generic_string();
            captureMetadata.environmentAssetGuid =
                activeEnvironmentAssetGuid_.isNil()
                ? "" : activeEnvironmentAssetGuid_.toString();
            captureMetadata.environmentArtifactCookKey =
                activeEnvironmentCookKey_;
            captureMetadata.environmentSourceTextureGuid =
                activeEnvironmentSourceGuid_.isNil()
                ? "" : activeEnvironmentSourceGuid_.toString();
            captureMetadata.environmentSourcePrimaries =
                activeEnvironmentSourcePrimaries_;
            captureMetadata.environmentRadianceScale =
                activeEnvironmentRadianceScale_;
            captureMetadata.directionalShadowActive =
                activeDirectionalShadowSelection_.has_value() &&
                activeDirectionalShadowSampleableMask_ != 0;
            captureMetadata.directionalShadowOwnerCount =
                activeDirectionalShadowOwnerCount_;
            if (activeDirectionalShadowSelection_) {
                captureMetadata.directionalShadowOwner =
                    activeDirectionalShadowSelection_->owner.toString();
                captureMetadata.directionalShadowLightSlot =
                    activeDirectionalShadowSelection_->lightSlot;
                captureMetadata.omittedShadowDirectionalLights =
                    activeDirectionalShadowSelection_->
                        omittedShadowDirectionalLights;
            }
            captureMetadata.directionalShadowResolution =
                config_.shadowSettings.directionalResolution;
            captureMetadata.directionalShadowCascadeCount =
                kDirectionalShadowCascadeCount;
            captureMetadata.directionalShadowSampleableMask =
                activeDirectionalShadowSampleableMask_;
            captureMetadata.directionalShadowFormat = "D32_SFLOAT";
            const uint32_t selectedShadowQuality =
                activeDirectionalShadowSelection_
                ? activeDirectionalShadowSelection_->quality
                : static_cast<uint32_t>(ShadowQualityProfile::Ultra);
            const ShadowFilterProfile captureShadowFilter =
                effectiveShadowFilterProfile(config_.shadowSettings,
                    selectedShadowQuality);
            captureMetadata.directionalShadowFilter =
                captureShadowFilter.contactHardening
                ? "bounded_spatial_pcss" : "fixed_5x5_pcf";
            captureMetadata.directionalShadowSourceAngularDiameterDegrees =
                config_.shadowSettings.directionalSourceAngularDiameterDegrees;
            captureMetadata.directionalShadowMaximumPenumbraTexels =
                captureShadowFilter.maximumPenumbraTexels;
            captureMetadata.directionalShadowReceiverDepthBiasTexels =
                config_.shadowSettings.directionalReceiverDepthBiasTexels;
            captureMetadata.directionalShadowReceiverPlaneClampTexels =
                config_.shadowSettings.directionalReceiverPlaneClampTexels;
            captureMetadata.directionalShadowNormalOffsetTexels =
                config_.shadowSettings.directionalNormalOffsetTexels;
            captureMetadata.directionalShadowBlockerSearchSamples =
                captureShadowFilter.blockerSearchSamples;
            captureMetadata.directionalShadowFilterSamples =
                captureShadowFilter.filterSamples;
            captureMetadata.unavailableFields = {};
            captureArtifact = writeCaptureArtifact(
                config_.captureDirectory, *completedCapture_, captureMetadata);
            std::cout << "IRIDIUM_CAPTURE {\"image\":\""
                << captureArtifact->image.generic_string() << "\",\"metadata\":\""
                << captureArtifact->metadata.generic_string() << "\",\"sha256\":\""
                << captureArtifact->imageSha256 << "\"}\n";
        }

        if (!config_.cpuProfileOutput.empty()) {
            CpuProfileRunMetadata metadata{};
            metadata.runId = "cpu-" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            metadata.buildConfiguration = IRIDIUM_BUILD_CONFIGURATION;
            metadata.sourceCommit = IRIDIUM_SOURCE_COMMIT;
            metadata.sourceBranch = IRIDIUM_SOURCE_BRANCH;
            metadata.compiler = IRIDIUM_COMPILER;
            metadata.shaderCompiler = IRIDIUM_SHADER_COMPILER;
            metadata.vulkanSdkVersion = IRIDIUM_VULKAN_SDK;
            metadata.operatingSystem = systemProfile.operatingSystem;
            metadata.cpuName = systemProfile.cpuName;
            metadata.systemMemoryBytes = systemProfile.physicalMemoryBytes;
            metadata.gpuName = renderRuntimeInfo_.gpuName;
            metadata.gpuUuid = renderRuntimeInfo_.gpuUuid;
            metadata.gpuVendorId = renderRuntimeInfo_.gpuVendorId;
            metadata.gpuDeviceId = renderRuntimeInfo_.gpuDeviceId;
            metadata.gpuDriverName = renderRuntimeInfo_.driverName;
            metadata.gpuDriverVersion = renderRuntimeInfo_.driverVersion;
            metadata.gpuDriverInfo = renderRuntimeInfo_.driverInfo;
            metadata.vulkanDeviceApiVersion =
                renderRuntimeInfo_.vulkanDeviceApiVersion;
            metadata.vulkanLoaderApiVersion =
                renderRuntimeInfo_.vulkanLoaderApiVersion;
            metadata.applicationEnabledLayers =
                renderRuntimeInfo_.applicationEnabledLayers;
            metadata.activeVulkanTools = renderRuntimeInfo_.activeTools;
            metadata.sourceDirtyAtConfigure = IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE != 0;
            metadata.validationEnabled = config_.enableValidation;
            metadata.windowVisible = config_.windowVisible;
            metadata.windowDecorated = config_.windowDecorated;
            metadata.requestedWindowWidth = config_.windowWidth;
            metadata.requestedWindowHeight = config_.windowHeight;
            metadata.renderWidth = renderExtent_.width;
            metadata.renderHeight = renderExtent_.height;
            metadata.warmupFrameCount = config_.warmupFrameCount;
            metadata.frameLimit = config_.frameLimit;
            metadata.measuredFrameCount = measuredFrameCount_;
            metadata.measurementWallNanoseconds = measurementWallNanoseconds_;
            metadata.cpuProfilingEnabled = cpuProfiler_.isEnabled();
            metadata.gpuProfilingRequested = config_.enableGpuProfiling;
            metadata.gpuProfilingAvailable = config_.enableGpuProfiling &&
                renderCapabilities_.gpuTimestampProfiling;
            metadata.gpuTimestampPeriodNanoseconds =
                renderCapabilities_.gpuTimestampPeriodNanoseconds;
            metadata.gpuTimestampValidBits =
                renderCapabilities_.gpuTimestampValidBits;
            metadata.engineAllocationTrackingAvailable =
                renderCapabilities_.engineAllocationTracking;
            metadata.driverMemoryBudgetAvailable =
                renderCapabilities_.driverMemoryBudget;
            metadata.cppAllocationTrackingAvailable = true;
            metadata.transparentPipelineStatisticsRequested =
                config_.enableTransparentPipelineStatistics;
            metadata.transparentPipelineStatisticsAvailable =
                config_.enableTransparentPipelineStatistics &&
                renderCapabilities_.transparentPipelineStatistics;
            metadata.swapchainFormat = renderRuntimeInfo_.swapchainFormat;
            metadata.swapchainColorSpace = renderRuntimeInfo_.swapchainColorSpace;
            metadata.presentMode = renderRuntimeInfo_.presentMode;
            metadata.swapchainImageCount = renderRuntimeInfo_.swapchainImageCount;
			metadata.supportedOutputTransports =
				renderRuntimeInfo_.supportedOutputTransports;
			metadata.requestedOutputTransport =
				renderRuntimeInfo_.requestedOutputTransport;
			metadata.effectiveOutputTransport =
				renderRuntimeInfo_.effectiveOutputTransport;
			metadata.outputTransportDiagnostic =
				renderRuntimeInfo_.outputTransportDiagnostic;
			metadata.swapchainColorspaceExtensionEnabled =
				renderRuntimeInfo_.swapchainColorspaceExtensionEnabled;
			metadata.hdrMetadataExtensionEnabled =
				renderRuntimeInfo_.hdrMetadataExtensionEnabled;
            const bool effectiveSdr = renderRuntimeInfo_.effectiveOutputTransport ==
                "sdr_srgb";
            const bool effectiveScRgb =
                renderRuntimeInfo_.effectiveOutputTransport == "scrgb_linear";
            metadata.hdrMetadataApplied =
                renderRuntimeInfo_.hdrMetadataExtensionEnabled &&
                renderRuntimeInfo_.effectiveOutputTransport == "hdr10_pq";
            metadata.displayProfile = effectiveSdr
                ? "windows_sdr_rec709_srgb"
                : (effectiveScRgb ? "windows_scrgb_extended_srgb_linear"
                    : "windows_hdr10_rec2100_pq");
            metadata.outputTransfer = effectiveSdr ? "iec_61966_2_1_srgb"
                : (effectiveScRgb ? "linear" : "st2084_pq");
            metadata.paperWhiteNits = effectiveSdr ? 100.0 :
                config_.paperWhiteNits;
            metadata.peakNits = effectiveSdr ? 100.0 : config_.peakNits;
            metadata.baseWidth = renderRuntimeInfo_.baseWidth;
            metadata.baseHeight = renderRuntimeInfo_.baseHeight;
            metadata.reconstructionMode = renderRuntimeInfo_.reconstructionMode;
            metadata.outputMode = renderRuntimeInfo_.outputMode;
            metadata.qualitySettings = "m5_cluster_" +
                std::to_string(config_.clusterTileSize) + "x" +
                std::to_string(config_.clusterTileSize) + "x" +
                std::to_string(config_.clusterDepthSlices) +
                "_scene_linear_fixed_quality";
            metadata.qualitySettings += config_.forceDirectGBufferReference
                ? "_gbuffer_direct_unculled_reference" : "_gbuffer_automatic";
            metadata.qualitySettings +=
                (config_.forceDirectGBufferReference ||
                    config_.forceDirectShadowReference)
                ? "_shadow_direct_reference" : "_shadow_automatic";
            metadata.qualitySettings +=
                (config_.forceDirectGBufferReference ||
                    config_.forceDirectProbeCaptureReference)
                ? "_probe_capture_direct_reference"
                : "_probe_capture_automatic";
            metadata.qualitySettings += "_shadow_indirect_oracle_" +
                std::to_string(config_.shadowIndirectQualificationOracle);
            metadata.qualitySettings += "_shadow_lod_texels_" +
                std::to_string(config_.experimentalShadowLodErrorTexels) +
                "_max_" + std::to_string(config_.shadowLodMaximumLevel);
            metadata.qualitySettings += "_experimental_lod_px_" +
                std::to_string(config_.experimentalGpuLodErrorPixels) + "_max_" +
                std::to_string(config_.gpuLodMaximumLevel) + "_hysteresis_" +
                std::to_string(config_.gpuLodHysteresisFraction) + "_oracle_" +
                std::to_string(config_.gpuLodQualificationOracle) + "_resident_floor_" +
                std::to_string(config_.gpuLodMinimumResidentLevel);
            metadata.qualitySettings += "_probe_lod_px_" +
                std::to_string(config_.experimentalProbeLodErrorPixels) +
                "_max_" + std::to_string(config_.probeLodMaximumLevel) +
                "_oracle_" + std::to_string(config_.probeLodQualificationOracle);
            metadata.qualitySettings += "_depth_pyramid_" +
                std::to_string(config_.experimentalDepthPyramid) +
                "_occlusion_query_" +
                std::to_string(config_.experimentalDepthOcclusionQuery) +
                "_occlusion_rejection_" +
                std::to_string(config_.experimentalDepthOcclusionRejection);
            metadata.qualitySettings += "_occlusion_oracle_" +
                std::to_string(config_.depthOcclusionQualificationOracle);
            metadata.qualitySettings += "_fixture_lights_" +
                std::to_string(activeBenchmark_
                    ? activeBenchmark_->lights.size() : 0u);
            metadata.renderMode = "graph_deferred_plus_forward_scene_linear_canonical_" +
                std::string(gBufferLayoutName(config_.gBufferLayout)) +
                "_" + renderRuntimeInfo_.textureBindingMode +
                (config_.forceWireframe ? "_opaque_wireframe" : "");
            metadata.cacheState = config_.cacheState;
            metadata.outputOperator = std::string(outputOperatorName(
                config_.outputOperator)) + "_final_output";
            metadata.exposureState = "manual_ev_" +
                std::to_string(config_.manualExposureEv) +
                "_applied_in_final_output; cooked_ap1_environment_scale_in_manifest";
            metadata.colorDomain = "scene_linear_acescg_ap1_pre_output";
            metadata.renderGraphEnabled = renderRuntimeInfo_.renderGraphEnabled;
            metadata.renderGraphTopologyHash =
                renderRuntimeInfo_.renderGraphTopologyHash;
            metadata.renderGraphPassCount = renderRuntimeInfo_.renderGraphPassCount;
            metadata.renderGraphLogicalResourceCount =
                renderRuntimeInfo_.renderGraphLogicalResourceCount;
            metadata.renderGraphPhysicalSlotCount =
                renderRuntimeInfo_.renderGraphPhysicalSlotCount;
            metadata.renderGraphBarrierCount =
                renderRuntimeInfo_.renderGraphBarrierCount;
            metadata.renderGraphFrameCount = renderRuntimeInfo_.renderGraphFrameCount;
            metadata.renderGraphRequestedBytes =
                renderRuntimeInfo_.renderGraphRequestedBytes;
            metadata.renderGraphCommittedBytes =
                renderRuntimeInfo_.renderGraphCommittedBytes;
            metadata.renderGraphRebuildCount =
                renderRuntimeInfo_.renderGraphRebuildCount;
            metadata.renderGraphCacheMissCount =
                renderRuntimeInfo_.renderGraphCacheMissCount;
            metadata.ordinary2AtlasResident =
                renderRuntimeInfo_.ordinary2AtlasResident;
            metadata.ordinary2AtlasWidth =
                renderRuntimeInfo_.ordinary2AtlasWidth;
            metadata.ordinary2AtlasHeight =
                renderRuntimeInfo_.ordinary2AtlasHeight;
            metadata.gpuLightRecordsAvailable =
                renderCapabilities_.gpuLightRecords;
            metadata.maxGpuLightRecords =
                renderCapabilities_.maxGpuLightRecords;
            metadata.multiDrawIndirectAvailable =
                renderCapabilities_.multiDrawIndirect;
            metadata.drawIndirectFirstInstanceAvailable =
                renderCapabilities_.drawIndirectFirstInstance;
            metadata.drawIndirectCountAvailable =
                renderCapabilities_.drawIndirectCount;
            metadata.maxDrawIndirectCount =
                renderCapabilities_.maxDrawIndirectCount;
            metadata.gpuLightCapacity = renderRuntimeInfo_.gpuLightCapacity;
            metadata.gpuLightActiveCount =
                renderRuntimeInfo_.gpuLightActiveCount;
            metadata.gpuLightUploadBytes =
                renderRuntimeInfo_.gpuLightUploadBytes;
            metadata.gpuLightUploadRanges =
                renderRuntimeInfo_.gpuLightUploadRanges;
            metadata.startupTotalNanoseconds = startupProfile_.totalNanoseconds;
            metadata.windowInitNanoseconds = startupProfile_.windowNanoseconds;
            metadata.backendInitNanoseconds = startupProfile_.backendNanoseconds;
            metadata.editorInitNanoseconds = startupProfile_.editorNanoseconds;
            metadata.manifestVerificationNanoseconds =
                startupProfile_.manifestVerificationNanoseconds;
            // Source import is no longer a production startup path. Preserve the
            // legacy field as zero for profile-schema compatibility and report the
            // cooked artifact load separately.
            metadata.sourceImportNanoseconds = 0;
            metadata.modelLoadNanoseconds = startupProfile_.modelLoadNanoseconds;
            metadata.environmentCreationNanoseconds =
                startupProfile_.environmentCreationNanoseconds;
            metadata.sceneConstructionNanoseconds =
                startupProfile_.sceneConstructionNanoseconds;
            metadata.frameTopologyPrewarmNanoseconds =
                startupProfile_.frameTopologyPrewarmNanoseconds;
            metadata.frameTopologyPrewarmRequested =
                renderRuntimeInfo_.frameTopologyPrewarmRequested;
            metadata.frameTopologyPrewarmChanged =
                renderRuntimeInfo_.frameTopologyPrewarmChanged;
            metadata.refractionPyramidsResident =
                renderRuntimeInfo_.refractionPyramidsResident;
            metadata.modelLoadMode =
                config_.cookedModelArtifact.empty()
                ? "source-import"
                : "self-contained-cooked-artifact";
            if (mainModel) {
                metadata.modelLocation = mainModel->filePath;
                metadata.modelAssetGuid =
                    mainModel->assetGuid.isNil()
                    ? "" : mainModel->assetGuid.toString();
                metadata.modelArtifactCookKey =
                    mainModel->artifactCookKey;
            }
            metadata.environmentLoadMode =
                activeCookedEnvironmentArtifact_.empty()
                ? (activeBenchmark_ && !activeEnvironmentAssetGuid_.isNil()
                    ? "benchmark-procedural-constant" : "neutral-black-fallback")
                : "self-contained-cooked-artifact";
            metadata.environmentLocation =
                activeCookedEnvironmentArtifact_.generic_string();
            metadata.environmentAssetGuid =
                activeEnvironmentAssetGuid_.isNil()
                ? "" : activeEnvironmentAssetGuid_.toString();
            metadata.environmentArtifactCookKey =
                activeEnvironmentCookKey_;
            metadata.environmentSourceTextureGuid =
                activeEnvironmentSourceGuid_.isNil()
                ? "" : activeEnvironmentSourceGuid_.toString();
            metadata.environmentSourcePrimaries =
                activeEnvironmentSourcePrimaries_;
            metadata.environmentRadianceScale =
                activeEnvironmentRadianceScale_;
            metadata.uploadSubmittedBytes =
                renderRuntimeInfo_.uploads.submittedBytes;
            metadata.uploadSubmittedBatches =
                renderRuntimeInfo_.uploads.submittedBatches;
            metadata.uploadSubmitAndWaitNanoseconds =
                renderRuntimeInfo_.uploads.submitAndWaitNanoseconds;
            metadata.renderDebugView = config_.forceWireframe
                ? "wireframe"
                : std::string(renderDebugViewName(editor.getDebugView()));
            metadata.renderDebugViewSemantics = config_.forceWireframe
                ? "editor opaque geometry in wireframe with normal forward composition"
                : std::string(renderDebugViewDescription(editor.getDebugView()));
            if (activeBenchmark_) {
                metadata.benchmarkFixtureId = activeBenchmark_->id;
                metadata.benchmarkFixtureRevision = activeBenchmark_->revision;
                metadata.benchmarkCameraId = activeBenchmark_->camera.id;
                metadata.benchmarkManifestPath = benchmarkManifestPath_;
                metadata.benchmarkManifestSha256 = benchmarkManifestSha256_;
                for (const BenchmarkContentFile& file : activeBenchmark_->contentFiles) {
                    metadata.benchmarkContentHashes.emplace_back(
                        file.relativePath.generic_string(), file.sha256);
                }
            }
            metadata.unavailableFields = {
                "gpu_clocks_power_behavior"
            };
            if (captureArtifact) {
                metadata.captureOutputs.emplace_back(
                    captureArtifact->image.generic_string(),
                    captureArtifact->imageSha256);
            }
            else {
                metadata.unavailableFields.push_back("capture_outputs");
            }
            if (!activeBenchmark_) {
                metadata.unavailableFields.push_back("benchmark_fixture");
                metadata.unavailableFields.push_back("benchmark_camera");
            }
            if (!metadata.gpuProfilingAvailable) {
                metadata.unavailableFields.push_back("gpu_ranges");
            }
            if (!metadata.engineAllocationTrackingAvailable) {
                metadata.unavailableFields.push_back("allocation_totals");
            }
            if (!metadata.driverMemoryBudgetAvailable) {
                metadata.unavailableFields.push_back("driver_heap");
            }
            if (metadata.transparentPipelineStatisticsRequested &&
                !metadata.transparentPipelineStatisticsAvailable) {
                metadata.unavailableFields.push_back(
                    "transparent.fragment_invocations");
                metadata.unavailableFields.push_back(
                    "transparent.fullscreen_equivalents");
            }
            writeCpuProfileJsonLines(config_.cpuProfileOutput, cpuProfiler_, metadata);
        }
    }

    void Application::initWindow() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
        glfwInitialized_ = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Tell GLFW not to create an OpenGL context
        glfwWindowHint(GLFW_VISIBLE, config_.windowVisible ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_DECORATED, config_.windowDecorated ? GLFW_TRUE : GLFW_FALSE);
        window = glfwCreateWindow(static_cast<int>(config_.windowWidth),
            static_cast<int>(config_.windowHeight), "Iridium Engine", nullptr, nullptr);
        if (window == nullptr) {
            throw std::runtime_error("Failed to create the Iridium window");
        }

        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
    }

    void Application::initRenderer() {
        // 1. Instantiate the RHI (The Strategy Pattern in action)
        const auto backendStart = std::chrono::steady_clock::now();
        renderBackend = createRenderBackend(RenderBackendApi::Vulkan);
        renderBackend->init(window, {
            .enableValidation = config_.enableValidation,
            .experimentalDepthPyramid = config_.experimentalDepthPyramid,
            .experimentalVirtualShadowResources =
                config_.experimentalVirtualShadowResources,
            .virtualShadowDepthQualificationOracle =
                config_.virtualShadowDepthQualificationOracle,
            .experimentalDepthOcclusionQuery =
                config_.experimentalDepthOcclusionQuery,
            .experimentalDepthOcclusionRejection =
                config_.experimentalDepthOcclusionRejection,
            .enableDepthOcclusionQualificationOracle =
                config_.depthOcclusionQualificationOracle,
            .cpuProfiler = &cpuProfiler_,
            .enableGpuProfiling = config_.enableGpuProfiling,
            .enableTransparentPipelineStatistics =
                config_.enableTransparentPipelineStatistics,
            .validateReflectionProbeCaptureTargets =
                config_.validateReflectionProbes,
            .forceDirectGBufferReference = config_.forceDirectGBufferReference,
            .forceDirectShadowReference = config_.forceDirectShadowReference,
            .enableShadowIndirectQualificationOracle =
                config_.shadowIndirectQualificationOracle,
            .experimentalShadowLodErrorTexels =
                config_.experimentalShadowLodErrorTexels,
            .shadowLodMaximumLevel = config_.shadowLodMaximumLevel,
            .experimentalGpuLodErrorPixels = config_.experimentalGpuLodErrorPixels,
            .gpuLodMaximumLevel = config_.gpuLodMaximumLevel,
            .gpuLodHysteresisFraction = config_.gpuLodHysteresisFraction,
            .enableGpuLodQualificationOracle = config_.gpuLodQualificationOracle,
            .experimentalProbeLodErrorPixels =
                config_.experimentalProbeLodErrorPixels,
            .probeLodMaximumLevel = config_.probeLodMaximumLevel,
            .enableProbeLodQualificationOracle = config_.probeLodQualificationOracle,
            .weightedOitOrderSeed = config_.weightedOitOrderSeed,
            .gBufferLayout = config_.gBufferLayout,
            .clusterTileSize = config_.clusterTileSize,
            .clusterDepthSlices = config_.clusterDepthSlices,
            .directionalShadowResolution =
                config_.shadowSettings.directionalResolution,
            .spotShadowAtlasResolution =
                config_.shadowSettings.spotAtlasResolution,
            .pointShadowPool256Capacity =
                config_.shadowSettings.pointPool256Capacity,
            .pointShadowPool512Capacity =
                config_.shadowSettings.pointPool512Capacity,
            .pointShadowPool1024Capacity =
                config_.shadowSettings.pointPool1024Capacity,
            .reflectionProbeSettings = config_.reflectionProbeSettings,
            .manualExposureEv = config_.manualExposureEv,
            .outputOperator = config_.outputOperator,
			.outputTransport = config_.outputTransport,
            .paperWhiteNits = config_.paperWhiteNits,
            .peakNits = config_.peakNits,
        });
        renderExtent_ = renderBackend->getRenderExtent();
        renderCapabilities_ = renderBackend->getCapabilities();
        gpuScenePublisher_ = std::make_unique<GpuScenePublisher>(
            GpuSceneCapacity{
                .maximumInstances = 262'144u,
                .maximumPrimitives = 1'048'576u,
                .maximumGeometries = 1'048'576u,
                .maximumTransforms = 524'288u,
            });
        startupProfile_.backendNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - backendStart).count());

        // Production runtime always executes classified transparency (ADR-0012);
        // serialized LegacyTwoBucket values remain readable but never select it.
        const TransparencyExecutionMode runtimeTransparencyExecutionMode =
            TransparencyExecutionMode::Classified;
        assetManager = std::make_unique<AssetManager>(renderBackend.get(),
            runtimeTransparencyExecutionMode,
            config_.gpuLodMinimumResidentLevel);
        std::cout << "IRIDIUM_TRANSPARENCY_EXECUTION {\"mode\":\""
            << transparencyExecutionModeName(
                runtimeTransparencyExecutionMode)
            << "\",\"developer_override\":false}\n";
        if (config_.validateTextureTableScale != 0) {
            constexpr std::array<std::byte, 4>
                texturePixel{
                    std::byte{ 0x3f },
                    std::byte{ 0x7f },
                    std::byte{ 0xbf },
                    std::byte{ 0xff },
                };
            const TextureDesc probe{
                .width = 1,
                .height = 1,
                .format = TextureFormat::RGBA8_UNorm,
            };
            textureScaleProbeTextures_.reserve(
                config_.validateTextureTableScale);
            for (uint32_t index = 0;
                index <
                    config_.validateTextureTableScale;
                ++index) {
                textureScaleProbeTextures_.push_back(
                    renderBackend->allocateTexture(
                        probe, texturePixel));
            }
            std::cout
                << "IRIDIUM_TEXTURE_TABLE_SCALE "
                << "{\"resident_views\":"
                << textureScaleProbeTextures_.size()
                << ",\"resident_samplers\":"
                << textureScaleProbeTextures_.size()
                << ",\"indexed\":true}\n";
        }
        if (config_.validateMaterialTableScale != 0) {
            constexpr std::array<std::byte, 4>
                whitePixel{
                    std::byte{ 0xff },
                    std::byte{ 0xff },
                    std::byte{ 0xff },
                    std::byte{ 0xff },
                };
            materialScaleProbeTexture_ =
                renderBackend->allocateTexture(
                    TextureDesc{
                        .width = 1,
                        .height = 1,
                        .format =
                            TextureFormat::RGBA8_UNorm,
                    },
                    whitePixel);
            CanonicalMaterialAsset probe{};
            probe.name =
                "m3.7-material-scale-probe";
            probe.packed.closureClass =
                static_cast<uint32_t>(
                    MaterialClosureClass::
                        StandardDeferred);
            probe.packed.baseColorFactor = {
                1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed
                .metallicRoughnessIorSpecular = {
                    0.0f, 1.0f, 1.5f, 1.0f };
            probe.packed
                .specularColorNormalScale = {
                    1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed.diffuseFactor = {
                1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed
                .specularGlossinessFactorGloss = {
                    1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed
                .emissiveFactorStrength = {
                    0.0f, 0.0f, 0.0f, 1.0f };
            probe.packed.surfaceParameters = {
                1.0f, 0.5f, 0.0f, 0.0f };
            probe.textures.fill(
                materialScaleProbeTexture_);
            probe.packed.textureIndices.fill(
                materialScaleProbeTexture_
                    .getIndex());
            materialScaleProbeMaterials_.reserve(
                config_.validateMaterialTableScale);
            for (uint32_t index = 0;
                index <
                    config_
                        .validateMaterialTableScale;
                ++index) {
                materialScaleProbeMaterials_
                    .push_back(
                        renderBackend->
                            allocateCanonicalMaterial(
                                probe)
                            .material);
            }
            std::cout
                << "IRIDIUM_MATERIAL_TABLE_SCALE "
                << "{\"resident_records\":"
                << materialScaleProbeMaterials_
                       .size()
                << ",\"indexed\":true}\n";
        }
        assetRuntimeService_ =
            std::make_unique<AssetRuntimeService>(
                AssetRuntimeServiceConfig{
                    .uploadBudgetBytes =
                        EditorRuntimeUploadBudgetBytes,
                    .startSourceWorkers =
                        config_.benchmarkId.empty(),
                });
        const std::filesystem::path assetRoot =
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets";
        assetCatalog_ = createSqliteAssetCatalog(
            config_.benchmarkId.empty()
                ? std::filesystem::path(PROJECT_ROOT_DIR) / "out" /
                    "editor" / "asset-catalog.sqlite"
                : std::filesystem::path(":memory:"));
        if (config_.benchmarkId.empty()) {
            const AssetDiscoveryResult discovery =
                discoverAssetRoots(std::array{
                    AssetRoot{ "project", assetRoot },
                });
            assetCatalog_->rebuild(
                discovery.records,
                discovery.sourceDirectories);
            for (const AssetDiscoveryDiagnostic& diagnostic :
                discovery.diagnostics) {
                std::cerr << "Asset catalog " << diagnostic.code << " at "
                    << diagnostic.path << ": " << diagnostic.message << '\n';
            }
            assetCatalogService_ =
                std::make_unique<AssetCatalogService>(
                    assetCatalog_.get(),
                    std::vector<AssetRoot>{
                        AssetRoot{ "project", assetRoot },
                    },
                    &engineLog_);
            editorModelDdc_ =
                std::make_shared<
                    LocalDerivedDataCache>(
                        std::filesystem::path(
                            PROJECT_ROOT_DIR) /
                        "out" / "editor" /
                        "model-ddc");
            assetModelPreparationService_ =
                std::make_unique<AssetModelPreparationService>(
                    assetRoot,
                    editorModelDdc_,
                    CookTarget{
                        .platform = "windows-x64",
                        .profile = "editor",
                        .qualityPolicy = "reference",
                        .artifactContainerVersion =
                            kCookedArtifactContainerVersion,
                        .materialSchemaVersion = 2,
                    },
                    &engineLog_);
            assetEnvironmentPreparationService_ =
                std::make_unique<AssetEnvironmentPreparationService>(
                    assetRoot,
                    editorModelDdc_,
                    CookTarget{
                        .platform = "windows-x64",
                        .profile = "editor",
                        .qualityPolicy = "reference",
                        .artifactContainerVersion =
                            kCookedArtifactContainerVersion,
                        .materialSchemaVersion = 2,
                    },
                    &engineLog_);
            assetThumbnailService_ =
                std::make_unique<AssetThumbnailService>(
                    assetRoot,
                    editorModelDdc_,
                    CookTarget{
                        .platform = "windows-x64",
                        .profile = "editor",
                        .qualityPolicy = "reference",
                        .artifactContainerVersion =
                            kCookedArtifactContainerVersion,
                        .materialSchemaVersion = 2,
                    },
                    &engineLog_);
        }

        const auto editorStart = std::chrono::steady_clock::now();
        editor.init(window, &cpuProfiler_, config_.showProfiler,
            config_.showMaterialDiagnostics,
            config_.outputTransport, static_cast<float>(config_.manualExposureEv),
            static_cast<float>(config_.paperWhiteNits),
            static_cast<float>(config_.peakNits), config_.shadowSettings,
            config_.reflectionProbeSettings,
            assetCatalog_.get(),
            assetCatalogService_.get(),
            assetModelPreparationService_.get(),
            assetThumbnailService_.get(),
            assetRuntimeService_.get(),
            &engineLog_, &sceneDocumentService_, &transactionService_);
        renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        publishOutputTransportStatus();
        renderBackend->setOutputSettings(static_cast<float>(config_.manualExposureEv),
            static_cast<float>(config_.paperWhiteNits),
            static_cast<float>(config_.peakNits));
        editor.setDebugView(config_.debugView);
        if (config_.editorAssetViewerGuid) {
            const std::vector<AssetCatalogRecord> records =
                assetCatalog_->recordsForGuid(
                    *config_.editorAssetViewerGuid);
            const auto record = std::ranges::find_if(
                records,
                [](const AssetCatalogRecord& candidate) {
                    return candidate.status == AssetCatalogStatus::Ready &&
                        (candidate.assetType == "iridium.model" ||
                            candidate.assetType == "iridium.material");
                });
            if (record == records.end()) {
                throw std::runtime_error(
                    "--open-asset-viewer GUID is not a ready model or material asset");
            }
            const EditorAssetOpenResult opened =
                editor.assetDocuments().open({
                    .assetGuid = record->guid,
                    .parentAssetGuid = record->parentGuid,
                    .assetType = record->assetType,
                    .displayName = record->displayName,
                });
            if (!opened) {
                throw std::runtime_error(
                    "Could not open configured asset viewer: " +
                    opened.diagnostic);
            }
        }
        AssetGuid startupModelGuid;
        if (!config_.benchmarkId.empty()) {
            ImGui::GetIO().IniFilename = nullptr;
        }
        startupProfile_.editorNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - editorStart).count());

        replaceOutputTransformLut(
            renderRuntimeInfo_.effectiveOutputTransportMode);

        if (!config_.benchmarkId.empty()) {
            const auto manifestStart = std::chrono::steady_clock::now();
            const std::filesystem::path manifestPath = config_.benchmarkManifest.empty()
                ? std::filesystem::path(PROJECT_ROOT_DIR) /
                    "assets" / "benchmarks" / "m0" / "manifest.v1.json"
                : config_.benchmarkManifest;
            const BenchmarkManifest manifest = loadBenchmarkManifest(manifestPath);
            const std::filesystem::path projectRoot =
                std::filesystem::weakly_canonical(PROJECT_ROOT_DIR);
            benchmarkManifestPath_ = std::filesystem::relative(
                manifest.sourcePath, projectRoot).generic_string();
            benchmarkManifestSha256_ = sha256File(manifest.sourcePath);
            activeBenchmark_ = findBenchmarkFixture(manifest, config_.benchmarkId);
            startupProfile_.manifestVerificationNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - manifestStart).count());
            const auto importStart = std::chrono::steady_clock::now();
            if (config_.cookedModelArtifact.empty()) {
                throw std::invalid_argument(
                    "Benchmark runtime requires --cooked-model-artifact after the M3 production cutover.");
            }
            const std::filesystem::path artifactPath =
                config_.cookedModelArtifact.is_absolute()
                ? config_.cookedModelArtifact
                : std::filesystem::path(PROJECT_ROOT_DIR) /
                    config_.cookedModelArtifact;
            activeCookedModelArtifact_ =
                artifactPath.lexically_normal();
            mainModel =
                assetManager->
                    loadSelfContainedModelFromCookedArtifactFile(
                        artifactPath);
            startupModelGuid = mainModel->assetGuid;
            startupProfile_.modelLoadNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - importStart).count());
            const auto environmentStart = std::chrono::steady_clock::now();
            if (!config_.cookedEnvironmentArtifact.empty()) {
                const std::filesystem::path environmentPath =
                    config_.cookedEnvironmentArtifact.is_absolute()
                    ? config_.cookedEnvironmentArtifact
                    : std::filesystem::path(PROJECT_ROOT_DIR) /
                        config_.cookedEnvironmentArtifact;
                activeCookedEnvironmentArtifact_ =
                    environmentPath.lexically_normal();
                LoadedEnvironmentAsset environment = assetManager->
                    loadEnvironmentFromCookedArtifactFile(environmentPath);
                environmentLighting_ = environment.lighting;
                activeEnvironmentAssetGuid_ = environment.assetGuid;
                activeEnvironmentSourceGuid_ =
                    environment.manifest.sourceTextureGuid;
                activeEnvironmentCookKey_ = environment.cookKey;
                activeEnvironmentSourcePrimaries_ =
                    environment.manifest.sourcePrimaries;
                activeEnvironmentRadianceScale_ =
                    environment.manifest.sourceRadianceScale;
                loadedEnvironments_.insert_or_assign(
                    environment.assetGuid, std::move(environment));
            }
            else {
                // Honor the frozen fixture's declared illumination. Leaving the
                // backend's neutral black fallback here made no-light fixtures
                // produce identical all-black captures despite drawing geometry.
                const AssetGuid fixtureEnvironmentGuid = *AssetGuid::parse(
                    "019fc681-2110-7000-8000-000000000001");
                const glm::vec3 color = activeBenchmark_->constantEnvironmentLinear;
                const CookProduct product = makeConstantEnvironmentProduct(
                    fixtureEnvironmentGuid, color);
                if (hasCookErrors(product.diagnostics))
                    throw std::runtime_error("Constant benchmark environment is invalid.");
                const std::string recipe = "iridium.benchmark.constant_environment.v1/" +
                    std::to_string(std::bit_cast<uint32_t>(color.x)) + "/" +
                    std::to_string(std::bit_cast<uint32_t>(color.y)) + "/" +
                    std::to_string(std::bit_cast<uint32_t>(color.z));
                LoadedEnvironmentAsset environment = assetManager->
                    loadEnvironmentFromCookedArtifact({
                        .assetGuid = fixtureEnvironmentGuid,
                        .artifactType = product.artifactType,
                        .artifactSchemaVersion = product.artifactSchemaVersion,
                        .cookKey = sha256(std::as_bytes(std::span(recipe))),
                        .sections = product.sections,
                    });
                environmentLighting_ = environment.lighting;
                activeEnvironmentAssetGuid_ = environment.assetGuid;
                activeEnvironmentSourceGuid_ = environment.manifest.sourceTextureGuid;
                activeEnvironmentCookKey_ = environment.cookKey;
                activeEnvironmentSourcePrimaries_ = environment.manifest.sourcePrimaries;
                activeEnvironmentRadianceScale_ = environment.manifest.sourceRadianceScale;
                loadedEnvironments_.insert_or_assign(
                    environment.assetGuid, std::move(environment));
            }
            startupProfile_.environmentCreationNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - environmentStart).count());
            cameraPos = activeBenchmark_->camera.position;
            cameraFront = glm::normalize(activeBenchmark_->camera.target - cameraPos);
            cameraUp = glm::normalize(activeBenchmark_->camera.up);
            verticalFovDegrees_ = activeBenchmark_->camera.verticalFovDegrees;
            cameraNearPlane_ = activeBenchmark_->camera.nearPlane;
            cameraFarPlane_ = activeBenchmark_->camera.farPlane;
            if (!config_.warmupFrameCountSpecified) {
                config_.warmupFrameCount = activeBenchmark_->warmupFrames;
            }
            if (!config_.frameLimitSpecified) {
                config_.frameLimit = activeBenchmark_->measuredFrames;
            }
        }
        else {
            // Interactive startup is asset-independent. An explicit cooked model
            // still overrides the built-in primitive for diagnostics and captures,
            // while the cached cube remains available to editor create commands.
            const auto importStart = std::chrono::steady_clock::now();
            const std::shared_ptr<ModelAsset> builtInCube =
                assetManager->loadBuiltInCubeModel();
            if (config_.cookedModelArtifact.empty()) {
                mainModel = builtInCube;
                startupModelGuid = mainModel->assetGuid;
            } else {
                const std::filesystem::path artifactPath =
                    config_.cookedModelArtifact.is_absolute()
                    ? config_.cookedModelArtifact
                    : std::filesystem::path(PROJECT_ROOT_DIR) /
                        config_.cookedModelArtifact;
                activeCookedModelArtifact_ =
                    artifactPath.lexically_normal();
                mainModel =
                    assetManager->
                        loadSelfContainedModelFromCookedArtifactFile(
                            artifactPath);
                startupModelGuid =
                    mainModel->assetGuid;
            }
            startupProfile_.modelLoadNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - importStart).count());
            configureCookedModelHotReload();
            const auto environmentStart = std::chrono::steady_clock::now();
            if (!config_.cookedEnvironmentArtifact.empty()) {
                const std::filesystem::path environmentPath =
                    config_.cookedEnvironmentArtifact.is_absolute()
                    ? config_.cookedEnvironmentArtifact
                    : std::filesystem::path(PROJECT_ROOT_DIR) /
                        config_.cookedEnvironmentArtifact;
                activeCookedEnvironmentArtifact_ =
                    environmentPath.lexically_normal();
                LoadedEnvironmentAsset environment = assetManager->
                    loadEnvironmentFromCookedArtifactFile(environmentPath);
                environmentLighting_ = environment.lighting;
                activeEnvironmentAssetGuid_ = environment.assetGuid;
                activeEnvironmentSourceGuid_ =
                    environment.manifest.sourceTextureGuid;
                activeEnvironmentCookKey_ = environment.cookKey;
                activeEnvironmentSourcePrimaries_ =
                    environment.manifest.sourcePrimaries;
                activeEnvironmentRadianceScale_ =
                    environment.manifest.sourceRadianceScale;
                loadedEnvironments_.insert_or_assign(
                    environment.assetGuid, std::move(environment));
            }
            startupProfile_.environmentCreationNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - environmentStart).count());
            configureCookedEnvironmentHotReload();
        }
        if (config_.captureFrameIndex) {
            if (!activeBenchmark_ && !config_.editorAssetViewerGuid) {
                throw std::invalid_argument(
                    "Deterministic frame capture requires --benchmark or "
                    "--open-asset-viewer.");
            }
            if (config_.frameLimit != 0 &&
                *config_.captureFrameIndex >= config_.frameLimit) {
                throw std::invalid_argument(
                    "--capture-frame must be lower than the measured frame limit.");
            }
        }
        if (config_.validateDepthPyramidCapture && config_.frameLimit == 0u) {
            throw std::invalid_argument(
                "--validate-depth-pyramid-capture requires a bounded measured frame limit.");
        }
        if (config_.validateDepthPyramidResize) {
            if (config_.frameLimit < 10u) {
                throw std::invalid_argument(
                    "--validate-depth-pyramid-resize requires at least ten measured frames.");
            }
            if (!activeBenchmark_) {
                throw std::invalid_argument(
                    "--validate-depth-pyramid-resize requires a benchmark fixture.");
            }
            if (renderExtent_.width != 1280u || renderExtent_.height != 720u) {
                throw std::invalid_argument(
                    "--validate-depth-pyramid-resize requires the deterministic 1280x720 base extent.");
            }
            depthPyramidResizeValidation_.originalExtent = renderExtent_;
        }
        const uint32_t layeredValidationModeCount =
            (config_.validateOrdinary2Capture ? 1u : 0u) +
            (config_.validateOrdinary2Fallback ? 1u : 0u) +
            (config_.validateOrdinary2Resize ? 1u : 0u) +
            (config_.validateWeightedOitResize ? 1u : 0u) +
            (config_.validateDeepLayeredCapture ? 1u : 0u) +
            (config_.validateDeepLayeredLifecycle ? 1u : 0u);
        if (layeredValidationModeCount > 1u) {
            throw std::invalid_argument(
                "Transparency capture, fallback, resize, and deep validations are mutually exclusive.");
        }
        if (config_.validateOrdinary2Capture) {
            if (config_.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-capture requires a bounded measured frame limit.");
            }
            if (!mainModel ||
                !modelRequiresOrdinary2LayeredInterfaces(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-ordinary2-capture requires a startup model with Ordinary2 layered interfaces.");
            }
        }
        if (config_.validateOrdinary2Fallback) {
            if (config_.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-fallback requires a bounded measured frame limit.");
            }
            if (!mainModel) {
                throw std::invalid_argument(
                    "--validate-ordinary2-fallback requires a startup model.");
            }
            const Ordinary2FallbackModelStats stats =
                ordinary2FallbackModelStats(*mainModel);
            if (stats.transparentSubmeshes == 0u ||
                stats.requestedLayeredCandidates !=
                    stats.transparentSubmeshes ||
                stats.fallbackThinGlassSubmeshes !=
                    stats.transparentSubmeshes ||
                stats.fallbackFlaggedSubmeshes !=
                    stats.transparentSubmeshes ||
                stats.topologyRequiredSubmeshes !=
                    stats.transparentSubmeshes ||
                stats.layeredGlassSubmeshes != 0u ||
                modelRequiresOrdinary2LayeredInterfaces(*mainModel) ||
                !modelRequiresRefractionPyramids(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-ordinary2-fallback requires only topology-rejected LayeredGlass candidates resolved to ThinGlass.");
            }
        }
        if (config_.validateOrdinary2Resize) {
            if (config_.frameLimit < 6u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-resize requires at least six measured frames.");
            }
            if (!activeBenchmark_ || !mainModel ||
                !modelRequiresOrdinary2LayeredInterfaces(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-ordinary2-resize requires a benchmark startup model with Ordinary2 layered interfaces.");
            }
            if (renderExtent_.width != 1280u ||
                renderExtent_.height != 720u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-resize requires the deterministic 1280x720 base extent.");
            }
            ordinary2ResizeValidation_.originalExtent = renderExtent_;
        }
        if (config_.validateWeightedOitResize) {
            if (config_.frameLimit < 6u) {
                throw std::invalid_argument(
                    "--validate-weighted-oit-resize requires at least six measured frames.");
            }
            if (!activeBenchmark_ || !mainModel ||
                !modelRequiresWeightedOit(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-weighted-oit-resize requires a benchmark startup model with WeightedOIT work.");
            }
            if (renderExtent_.width != 1280u ||
                renderExtent_.height != 720u) {
                throw std::invalid_argument(
                    "--validate-weighted-oit-resize requires the deterministic 1280x720 base extent.");
            }
            weightedOitResizeValidation_.originalExtent = renderExtent_;
        }
        if (config_.validateDeepLayeredCapture) {
            if (config_.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-deep-layered-capture requires a bounded measured frame limit.");
            }
            const bool hasRequestedTier = mainModel &&
                (config_.deepLayeredCaptureQuality ==
                        TransparencyQuality::Hero4
                    ? modelRequiresHero4LayeredInterfaces(*mainModel)
                    : modelRequiresCinematic8LayeredInterfaces(*mainModel));
            if (!activeBenchmark_ || !hasRequestedTier) {
                throw std::invalid_argument(
                    "--validate-deep-layered-capture requires a benchmark startup model with the selected deep layered quality.");
            }
        }
        if (config_.validateDeepLayeredLifecycle) {
            if (config_.frameLimit < 250u) {
                throw std::invalid_argument(
                    "--validate-deep-layered-lifecycle requires at least 250 measured frames.");
            }
            const bool hasRequestedTier = mainModel &&
                (config_.deepLayeredCaptureQuality ==
                        TransparencyQuality::Hero4
                    ? modelRequiresHero4LayeredInterfaces(*mainModel)
                    : modelRequiresCinematic8LayeredInterfaces(*mainModel));
            if (!activeBenchmark_ || !hasRequestedTier) {
                throw std::invalid_argument(
                    "--validate-deep-layered-lifecycle requires a benchmark startup model with the selected deep layered quality.");
            }
        }
        const FrameTopologyPreparation topologyPreparation =
            renderBackend->prepareFrameTopology({
                .refractionPyramids = mainModel &&
                    modelRequiresRefractionPyramids(*mainModel),
                .ordinary2LayeredInterfaces = mainModel &&
                    modelRequiresOrdinary2LayeredInterfaces(*mainModel),
                .hero4LayeredInterfaces = mainModel &&
                    modelRequiresHero4LayeredInterfaces(*mainModel),
                .cinematic8LayeredInterfaces = mainModel &&
                    modelRequiresCinematic8LayeredInterfaces(*mainModel),
                .weightedOit = mainModel &&
                    modelRequiresWeightedOit(*mainModel)
            });
        startupProfile_.frameTopologyPrewarmNanoseconds =
            topologyPreparation.durationNanoseconds;
        std::cout << "IRIDIUM_FRAME_TOPOLOGY_PREWARM {\"requested\":"
            << (topologyPreparation.requested ? "true" : "false")
            << ",\"changed\":"
            << (topologyPreparation.changed ? "true" : "false")
            << ",\"duration_ns\":"
            << topologyPreparation.durationNanoseconds << "}\n" << std::flush;
        if (config_.validateOrdinary2Resize) {
            ordinary2ResizeValidation_.initialRenderGraphRebuildCount =
                renderBackend->getRuntimeInfo().renderGraphRebuildCount;
        }
        if (config_.validateWeightedOitResize) {
            weightedOitResizeValidation_.initialRenderGraphRebuildCount =
                renderBackend->getRuntimeInfo().renderGraphRebuildCount;
        }
        if (config_.validateDepthPyramidResize) {
            depthPyramidResizeValidation_.initialRenderGraphRebuildCount =
                renderBackend->getRuntimeInfo().renderGraphRebuildCount;
        }
        const auto sceneStart = std::chrono::steady_clock::now();
        if (environmentLighting_.isValid())
            renderBackend->setEnvironmentLighting(environmentLighting_);

        const glm::uvec3 grid = activeBenchmark_
            ? activeBenchmark_->sceneFactory.instanceGrid
            : glm::uvec3(1, 1, 1);
        const glm::vec3 spacing = activeBenchmark_
            ? activeBenchmark_->sceneFactory.instanceSpacing
            : glm::vec3(0.0f);
        const glm::vec3 gridCenter = (glm::vec3(grid) - glm::vec3(1.0f)) * 0.5f;
        const uint64_t gridInstanceCount = benchmarkInstanceCount(grid);
        const bool renderInstanceBatch = activeBenchmark_ &&
            activeBenchmark_->sceneFactory.renderInstanceBatch;
        const glm::uvec3 constructionGrid = renderInstanceBatch
            ? glm::uvec3(1u) : grid;
        if (activeBenchmark_) {
            benchmarkInstances_.reserve(benchmarkInstances_.size() +
                static_cast<size_t>(renderInstanceBatch
                    ? 1u : gridInstanceCount));
        }
        Entity firstEntity = NULL_ENTITY;
        uint32_t benchmarkInstanceOrdinal = 0u;
        for (uint32_t z = 0; z < constructionGrid.z; ++z) {
            for (uint32_t y = 0; y < constructionGrid.y; ++y) {
                for (uint32_t x = 0; x < constructionGrid.x; ++x) {
                    const glm::vec3 position = activeBenchmark_ &&
                            !renderInstanceBatch
                        ? (glm::vec3(x, y, z) - gridCenter) * spacing
                        : glm::vec3(0.0f);
                    Entity entity = NULL_ENTITY;
                    if (activeBenchmark_) {
                        // The ordinary editor helper deliberately scans existing
                        // names and sibling order. A deterministic benchmark grid
                        // already owns both values, so repeating those scans would
                        // turn fixture construction into O(n^2) editor-only work.
                        entity = registry.createEntity();
                        auto& name = registry.addComponent<NameComponent>(entity);
                        name.name = benchmarkInstanceOrdinal == 0u
                            ? "Benchmark Model"
                            : "Benchmark Model (" +
                                std::to_string(benchmarkInstanceOrdinal + 1u) + ")";
                        auto& transform =
                            registry.addComponent<TransformComponent>(entity);
                        transform.position = position;
                        transform.isDirty = true;
                        auto& relationship =
                            registry.addComponent<RelationshipComponent>(entity);
                        relationship.siblingOrder = static_cast<int32_t>(
                            benchmarkInstanceOrdinal);
                        auto& mesh = registry.addComponent<MeshComponent>(entity);
                        mesh.assetGuid = startupModelGuid;
                        mesh.requestedAssetGuid = startupModelGuid;
                    }
                    else {
                        entity = createModelEditorEntity(registry, startupModelGuid,
                            config_.cookedModelArtifact.empty() ? "Cube" : "Model",
                            position);
                    }
                    if (firstEntity == NULL_ENTITY) firstEntity = entity;
                    auto& transform = registry.getComponent<TransformComponent>(entity);
                    transform.rotation = glm::vec3(0.0f);
                    transform.scale = glm::vec3(1.0f);
                    if (activeBenchmark_ && !renderInstanceBatch) {
                        const BenchmarkSceneFactory& factory =
                            activeBenchmark_->sceneFactory;
                        transform.scale =
                            factory.instanceScaleOverrideEnabled &&
                                factory.instanceScaleOverrideIndex ==
                                    benchmarkInstanceOrdinal
                            ? factory.instanceScaleOverride
                            : factory.instanceScale;
                    }
                    transform.worldMatrix = glm::mat4(1.0f);
                    transform.isDirty = true;

                    auto& meshComp = registry.getComponent<MeshComponent>(entity);
                    meshComp.model = mainModel;
                    meshComp.assetGuid =
                        startupModelGuid;
                    if (!mainModel) {
                        meshComp.requestedAssetGuid =
                            startupModelGuid;
                    }
                    meshComp.enabled = true;
                    if (activeBenchmark_) {
                        benchmarkInstances_.push_back({ entity, transform.position });
                    }
                    ++benchmarkInstanceOrdinal;
                }
            }
        }
        if (renderInstanceBatch) {
            if (firstEntity == NULL_ENTITY || !mainModel) {
                throw std::logic_error(
                    "Render-batch benchmark requires a loaded model entity");
            }
            auto& batch = registry.addComponent<
                RenderInstanceBatchComponent>(firstEntity);
            batch.localTransforms.reserve(static_cast<size_t>(
                gridInstanceCount));
            for (uint32_t z = 0; z < grid.z; ++z) {
                for (uint32_t y = 0; y < grid.y; ++y) {
                    for (uint32_t x = 0; x < grid.x; ++x) {
                        const glm::vec3 position =
                            (glm::vec3(x, y, z) - gridCenter) * spacing;
                        glm::mat4 local = glm::translate(
                            glm::mat4(1.0f), position);
                        local = glm::scale(local,
                            activeBenchmark_->sceneFactory.instanceScale);
                        batch.localTransforms.push_back(local);
                    }
                }
            }
            batch.subMeshBounds.resize(mainModel->subMeshes.size());
            for (size_t subMeshIndex = 0u;
                subMeshIndex < mainModel->subMeshes.size(); ++subMeshIndex) {
                const SubMesh& subMesh = mainModel->subMeshes[subMeshIndex];
                glm::vec3 minimum((std::numeric_limits<float>::max)());
                glm::vec3 maximum((std::numeric_limits<float>::lowest)());
                for (const glm::mat4& local : batch.localTransforms) {
                    for (uint32_t corner = 0u; corner < 8u; ++corner) {
                        const glm::vec3 point{
                            (corner & 1u) != 0u ? subMesh.boundsMax.x :
                                subMesh.boundsMin.x,
                            (corner & 2u) != 0u ? subMesh.boundsMax.y :
                                subMesh.boundsMin.y,
                            (corner & 4u) != 0u ? subMesh.boundsMax.z :
                                subMesh.boundsMin.z,
                        };
                        const glm::vec4 transformed = local *
                            glm::vec4(point, 1.0f);
                        minimum = glm::min(minimum,
                            glm::vec3(transformed));
                        maximum = glm::max(maximum,
                            glm::vec3(transformed));
                    }
                }
                batch.subMeshBounds[subMeshIndex] = {
                    .minimum = minimum,
                    .maximum = maximum,
                    .valid = !batch.localTransforms.empty(),
                };
            }
        }
        editor.setSelectedEntity(activeBenchmark_ && !config_.selectBenchmarkEntity
            ? NULL_ENTITY : firstEntity);
        const bool sampleCarLightingFixture = activeBenchmark_ &&
            activeBenchmark_->id == "sample_car_lighting_local_v1";
        const bool spotShadowContactFixture = activeBenchmark_ &&
            (activeBenchmark_->id == "spot_shadow_contact_v1" ||
                activeBenchmark_->id == "spot_shadow_contact_forward_v1");
        const bool pointShadowContactFixture = activeBenchmark_ &&
            (activeBenchmark_->id == "point_shadow_contact_v1" ||
                activeBenchmark_->id == "point_shadow_contact_forward_v1");
        const bool directionalShadowFixture = activeBenchmark_ &&
            (activeBenchmark_->id == "directional_shadow_contact_v1" ||
                activeBenchmark_->id == "directional_shadow_motion_v1");
        const bool explicitFixtureLights = activeBenchmark_ &&
            !activeBenchmark_->lights.empty() &&
            config_.clusterStressLightCount == 0 &&
            config_.validateLightTableScale == 0;
        const uint32_t fixtureLightCount = explicitFixtureLights
            ? static_cast<uint32_t>(activeBenchmark_->lights.size())
            : directionalShadowFixture
            ? 1u : (spotShadowContactFixture || pointShadowContactFixture)
            ? 2u : sampleCarLightingFixture ? 3u : 0u;
        const uint32_t generatedLightCount = config_.clusterStressLightCount != 0
            ? config_.clusterStressLightCount
            : config_.validateLightTableScale != 0
                ? config_.validateLightTableScale
                : fixtureLightCount;
        if (generatedLightCount != 0) {
            for (uint32_t index = 0;
                index < generatedLightCount; ++index) {
                std::array<uint8_t, 10> random{};
                const uint64_t value = static_cast<uint64_t>(index) + 1;
                for (size_t byte = 0; byte < sizeof(value); ++byte) {
                    random[byte] = static_cast<uint8_t>(value >> (byte * 8u));
                }
                const Entity lightEntity = sceneWorld_.createEntity(
                    SceneEntityUuid::fromUuidV7Fields(
                        1'775'000'300'000ull + index, random));
                auto& transform = registry.addComponent<TransformComponent>(
                    lightEntity);
                const BenchmarkLight* fixtureLight = explicitFixtureLights
                    ? &activeBenchmark_->lights[index] : nullptr;
                if (fixtureLight) {
                    transform.position = fixtureLight->position;
                    transform.rotation = fixtureLight->rotationDegrees;
                }
                else if (sampleCarLightingFixture) {
                    constexpr std::array<glm::vec3, 3> kRigPositions{
                        glm::vec3(-3.0f, 4.0f, 3.0f),
                        glm::vec3(3.0f, 2.25f, 1.5f),
                        glm::vec3(0.0f, 5.0f, -2.0f),
                    };
                    transform.position = kRigPositions[index];
                    if (index != 1u) {
                        const glm::vec3 emissionDirection = glm::normalize(
                            glm::vec3(0.0f, 1.0f, 0.0f) - transform.position);
                        transform.rotation.x = -glm::degrees(
                            std::asin(emissionDirection.y));
                        transform.rotation.y = glm::degrees(std::atan2(
                            emissionDirection.x, emissionDirection.z));
                    }
                }
                else if (spotShadowContactFixture || pointShadowContactFixture) {
                    const float lightHeight = spotShadowContactFixture
                        ? 4.0f : 3.0f;
                    transform.position = index % 2u == 0u
                        ? glm::vec3(-3.0f, lightHeight, 3.0f)
                        : glm::vec3(3.0f, lightHeight, 3.0f);
                    if (spotShadowContactFixture) {
                        const glm::vec3 emissionDirection = glm::normalize(
                            -transform.position);
                        transform.rotation.x = -glm::degrees(
                            std::asin(emissionDirection.y));
                        transform.rotation.y = glm::degrees(std::atan2(
                            emissionDirection.x, emissionDirection.z));
                    }
                }
                else if (config_.clusterStressLightCount != 0) {
                    transform.position = {
                        (static_cast<float>(index % 32u) - 15.5f) * 2.0f,
                        (static_cast<float>((index / 32u) % 16u) - 7.5f) * 2.0f,
                        -10.0f - static_cast<float>(index % 16u) * 4.0f,
                    };
                }
                else {
                    transform.position = {
                        static_cast<float>(index % 64u) - 31.5f,
                        static_cast<float>((index / 64u) % 64u) - 31.5f,
                        static_cast<float>(index / 4'096u),
                    };
                }
                if (config_.clusterStressLightCount != 0 && index < 4u) {
                    // Spread global stress lights across opposing azimuths so
                    // multi-owner shadow composition is exercised, not merely
                    // duplicate projections from coincident directions.
                    transform.rotation.y = 135.0f +
                        static_cast<float>(index) * 90.0f;
                }
                else if (!fixtureLight && activeBenchmark_ &&
                    (activeBenchmark_->id == "directional_shadow_contact_v1" ||
                        activeBenchmark_->id == "directional_shadow_motion_v1") &&
                    index == 0u) {
                    // Preserve the fixture's incoming-light direction after +Z
                    // became the authored emission axis.
                    transform.rotation.y = 210.0f;
                }
                registry.addComponent<RelationshipComponent>(lightEntity)
                    .siblingOrder = static_cast<int32_t>(index);
                auto& light = registry.addComponent<LightComponent>(lightEntity);
                if (fixtureLight) {
                    switch (fixtureLight->type) {
                    case BenchmarkLightType::Directional:
                        light.type = LightType::Directional;
                        break;
                    case BenchmarkLightType::Point:
                        light.type = LightType::Point;
                        break;
                    case BenchmarkLightType::Spot:
                        light.type = LightType::Spot;
                        break;
                    }
                    light.colorLinearRec709 = fixtureLight->colorLinearRec709;
                    light.illuminanceLux = fixtureLight->illuminanceLux;
                    light.luminousIntensityCandela =
                        fixtureLight->luminousIntensityCandela;
                    light.rangeMeters = fixtureLight->rangeMeters;
                    light.sourceRadiusMeters = fixtureLight->sourceRadiusMeters;
                    light.innerConeDegrees = fixtureLight->innerConeDegrees;
                    light.outerConeDegrees = fixtureLight->outerConeDegrees;
                    light.castsShadows = fixtureLight->castsShadows &&
                        !config_.disableBenchmarkLocalShadows;
                    light.shadowQuality = static_cast<LightShadowQuality>(
                        fixtureLight->shadowQuality);
                    light.priority = fixtureLight->priority;
                    continue;
                }
                light.type = sampleCarLightingFixture
                    ? (index == 0u ? LightType::Spot :
                        index == 1u ? LightType::Point : LightType::Directional)
                    : directionalShadowFixture
                    ? LightType::Directional
                    : spotShadowContactFixture
                    ? LightType::Spot
                    : pointShadowContactFixture ? LightType::Point
                    : config_.clusterStressLightCount == 0
                    ? static_cast<LightType>(index % 3u)
                    : (index < 4u ? LightType::Directional :
                        (index % 2u == 0u ? LightType::Point : LightType::Spot));
                if (directionalShadowFixture || spotShadowContactFixture ||
                    pointShadowContactFixture) {
                    light.castsShadows =
                        !config_.disableBenchmarkLocalShadows;
                }
                else if (sampleCarLightingFixture) {
                    light.castsShadows = true;
                }
                if (!spotShadowContactFixture && !pointShadowContactFixture &&
                    config_.clusterStressLightCount != 0 &&
                    light.type == LightType::Spot) {
                    // The stress volume is centered in front of these negative-Z
                    // lights, so the authored +Z emission axis already aims back
                    // through the volume.
                    transform.rotation.y = 0.0f;
                }
                light.colorLinearRec709 = { 1.0f, 0.5f, 0.25f };
                if (sampleCarLightingFixture || directionalShadowFixture ||
                    spotShadowContactFixture || pointShadowContactFixture) {
                    light.shadowQuality = LightShadowQuality::Ultra;
                }
                light.illuminanceLux = 100'000.0f;
                light.luminousIntensityCandela = 1'250.0f;
                light.rangeMeters = config_.clusterStressLightCount == 0
                    ? 25.0f : 4.0f;
                if (sampleCarLightingFixture) {
                    constexpr std::array<glm::vec3, 3> kRigColors{
                        glm::vec3(1.0f, 0.82f, 0.64f),
                        glm::vec3(0.32f, 0.5f, 1.0f),
                        glm::vec3(1.0f, 0.96f, 0.9f),
                    };
                    light.colorLinearRec709 = kRigColors[index];
                    light.luminousIntensityCandela = index == 0u
                        ? 45'000.0f : 18'000.0f;
                    light.illuminanceLux = 35'000.0f;
                    light.rangeMeters = 12.0f;
                    light.innerConeDegrees = 22.0f;
                    light.outerConeDegrees = 38.0f;
                    light.priority = static_cast<int32_t>(3u - index);
                }
                else if (spotShadowContactFixture) {
                    light.colorLinearRec709 = index % 2u == 0u
                        ? glm::vec3(1.0f, 0.35f, 0.12f)
                        : glm::vec3(0.12f, 0.35f, 1.0f);
                    light.luminousIntensityCandela = 1'000'000.0f;
                    light.rangeMeters = 15.0f;
                    light.innerConeDegrees = 20.0f;
                    light.outerConeDegrees = 35.0f;
                    light.priority = static_cast<int32_t>(
                        generatedLightCount - index);
                }
                else if (pointShadowContactFixture) {
                    light.colorLinearRec709 = index % 2u == 0u
                        ? glm::vec3(1.0f, 0.3f, 0.08f)
                        : glm::vec3(0.08f, 0.3f, 1.0f);
                    light.luminousIntensityCandela = 500'000.0f;
                    light.rangeMeters = 15.0f;
                    light.priority = static_cast<int32_t>(
                        generatedLightCount - index);
                }
            }
        }
        if (!activeBenchmark_) {
            (void)createEditorEntityPreset(
                registry, EditorEntityPreset::DirectionalLight);
            const Entity skyEntity = createEditorEntityPreset(
                registry, EditorEntityPreset::HdriSky);
            auto& sky = registry.getComponent<SkyComponent>(skyEntity);
            if (!activeEnvironmentAssetGuid_.isNil()) {
                sky.hdri.environmentAssetGuid = activeEnvironmentAssetGuid_;
                sky.resolvedEnvironmentAssetGuid =
                    activeEnvironmentAssetGuid_;
                sky.requestedEnvironmentAssetGuid = {};
            }
        }
        if (config_.validateReflectionProbes) {
            if (activeEnvironmentAssetGuid_.isNil())
                throw std::invalid_argument(
                    "--validate-reflection-probes requires --cooked-environment-artifact");
            const Entity probeEntity = sceneWorld_.createEntity(
                SceneEntityUuid::fromUuidV7Fields(
                    1'775'000'410'000ull,
                    std::array<uint8_t, 10>{ 0x49, 0x52, 0x49, 0x44, 0x49,
                        0x55, 0x4d, 0x50, 0x52, 0x42 }));
            registry.addComponent<NameComponent>(probeEntity,
                "Reflection Probe Validation");
            registry.addComponent<TransformComponent>(probeEntity);
            registry.addComponent<RelationshipComponent>(probeEntity)
                .siblingOrder = static_cast<int32_t>(
                    generatedLightCount + 2u);
            auto& probe = registry.addComponent<ReflectionProbeComponent>(
                probeEntity);
            probe.shape = ReflectionProbeShape::Sphere;
            probe.sphereRadiusMeters = 1'000.0f;
            probe.blendDistanceMeters = 0.0f;
            probe.parallaxMode = ReflectionProbeParallaxMode::None;
            probe.environmentAssetGuid = activeEnvironmentAssetGuid_;
            probe.resolvedEnvironmentAssetGuid = activeEnvironmentAssetGuid_;

            const BenchmarkReflectionProbeCapture* fixtureCapture =
                activeBenchmark_ && activeBenchmark_->reflectionProbeCapture
                ? &*activeBenchmark_->reflectionProbeCapture : nullptr;

            // The default route keeps the self-capture exclusion proof. An
            // explicit benchmark capture instead owns no renderable geometry,
            // so every fixture primitive contributes to all six faces and the
            // published cubemap can be judged in visible surface reflections.
            Entity captureProbeEntity = fixtureCapture
                ? NULL_ENTITY : firstEntity;
            if (captureProbeEntity == NULL_ENTITY) {
                captureProbeEntity = sceneWorld_.createEntity(
                    SceneEntityUuid::fromUuidV7Fields(
                        1'775'000'410'001ull,
                        std::array<uint8_t, 10>{ 0x49, 0x52, 0x49, 0x44,
                            0x49, 0x55, 0x4d, 0x43, 0x41, 0x50 }));
                registry.addComponent<NameComponent>(captureProbeEntity,
                    "Runtime Reflection Capture Validation");
                auto& captureTransform =
                    registry.addComponent<TransformComponent>(
                        captureProbeEntity);
                if (fixtureCapture)
                    captureTransform.position = fixtureCapture->position;
                registry.addComponent<RelationshipComponent>(captureProbeEntity)
                    .siblingOrder = static_cast<int32_t>(
                        generatedLightCount + 3u);
            }
            auto& captureProbe =
                registry.addComponent<ReflectionProbeComponent>(
                    captureProbeEntity);
            captureProbe.shape = ReflectionProbeShape::Sphere;
            captureProbe.sphereRadiusMeters = 1'000.0f;
            captureProbe.blendDistanceMeters = 0.0f;
            captureProbe.parallaxMode = ReflectionProbeParallaxMode::None;
            captureProbe.priority = fixtureCapture
                ? fixtureCapture->priority : 1;
            if (fixtureCapture) {
                captureProbe.sphereRadiusMeters =
                    fixtureCapture->influenceRadiusMeters;
                captureProbe.updateMode = fixtureCapture->updateMode ==
                        BenchmarkReflectionProbeUpdateMode::Realtime
                    ? ReflectionProbeUpdateMode::Realtime
                    : ReflectionProbeUpdateMode::OnDemand;
                captureProbe.captureResolution = static_cast<int32_t>(
                    fixtureCapture->resolution);
                captureProbe.captureNearMeters = fixtureCapture->nearPlane;
                captureProbe.captureFarMeters = fixtureCapture->farPlane;
                captureProbe.captureSky = fixtureCapture->captureSky;
            }
        }
        startupProfile_.sceneConstructionNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sceneStart).count());

        if (config_.validateTextureResidencyChurn) {
            if (config_.frameLimit != 0 &&
                config_.warmupFrameCount + config_.frameLimit < 3) {
                throw std::invalid_argument(
                    "Texture residency churn validation requires at least three frames");
            }
            const TextureDesc probeDesc{
                .width = 512,
                .height = 512,
                .format = TextureFormat::RGBA8_UNorm,
                .usageClass = TextureUsageClass::Sampled2D,
            };
            constexpr size_t ProbeUploadBudgetBytes =
                1u * 1024u * 1024u;
            residencyProbePixels_.resize(ProbeUploadBudgetBytes);
            for (size_t offset = 0; offset < ProbeUploadBudgetBytes;
                offset += 4) {
                residencyProbePixels_[offset] = std::byte{ 0x3f };
                residencyProbePixels_[offset + 1] = std::byte{ 0x7f };
                residencyProbePixels_[offset + 2] = std::byte{ 0xbf };
                residencyProbePixels_[offset + 3] = std::byte{ 0xff };
            }
            residencyProbeTexture_ = renderBackend->allocateTexture(
                probeDesc, residencyProbePixels_);
        }

        // The command-line viewer path is used for deterministic captures and
        // profiling. Interactive opens stay fully asynchronous, but a bounded
        // command-line run must not begin its measured frames before the
        // requested cooked revision is resident.
        if (config_.editorAssetViewerGuid) {
            constexpr auto ViewerReadyTimeout = std::chrono::seconds(30);
            const auto deadline = std::chrono::steady_clock::now() +
                ViewerReadyTimeout;
            while (!resolveEditorAssetPreview()) {
                ProcessMeshSwaps();
                if (assetRuntimeService_) {
                    (void)assetRuntimeService_->tick();
                    const EditorAssetDocument* document =
                        editor.assetDocuments().active();
                    const auto snapshot = document
                        ? assetRuntimeService_->snapshot(
                            document->presentationAssetGuid)
                        : std::nullopt;
                    if (snapshot && snapshot->state ==
                            RuntimeAssetState::Failed) {
                        throw std::runtime_error(
                            "Configured asset viewer preparation failed: " +
                            snapshot->diagnostic);
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error(
                        "Timed out preparing configured asset viewer");
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
            }
        }
    }

    void Application::updateTextureResidencyChurn(uint64_t frameIndex) {
        if (!config_.validateTextureResidencyChurn) return;

        const TextureDesc probeDesc{
            .width = 512,
            .height = 512,
            .format = TextureFormat::RGBA8_UNorm,
            .usageClass = TextureUsageClass::Sampled2D,
        };

        if (frameIndex == 0) {
            if (!residencyProbeTexture_.isValid()) {
                throw std::runtime_error(
                    "Texture residency churn probe was not initialized");
            }
            residencyRetiredIndex_ = residencyProbeTexture_.getIndex();
            renderBackend->freeTexture(residencyProbeTexture_);
            residencyProbeTexture_ = {};
            residencyReplacementTexture_ = renderBackend->allocateTexture(
                probeDesc, residencyProbePixels_);
            if (residencyReplacementTexture_.getIndex() ==
                residencyRetiredIndex_) {
                throw std::runtime_error(
                    "Texture descriptor index was reused before fence retirement");
            }
        }
        else if (frameIndex == 2) {
            TextureHandle collected = renderBackend->allocateTexture(
                probeDesc, residencyProbePixels_);
            if (collected.getIndex() != residencyRetiredIndex_) {
                renderBackend->freeTexture(collected);
                throw std::runtime_error(
                    "Retired texture descriptor index was not reclaimed");
            }
            if (residencyReplacementTexture_.isValid()) {
                renderBackend->freeTexture(residencyReplacementTexture_);
                residencyReplacementTexture_ = {};
            }
            renderBackend->freeTexture(collected);
            std::cout << "IRIDIUM_TEXTURE_RESIDENCY_CHURN "
                "{\"fallback_before_destroy\":true,"
                "\"immediate_reuse\":false,"
                "\"reuse_after_fence\":true,"
                "\"retired_index\":" << residencyRetiredIndex_ << "}\n";
            residencyRetiredIndex_ = UINT32_MAX;
        }
    }

    void Application::mainLoop() {
        float lastFrameTime = 0.0f;
        uint64_t applicationFrameCount = 0;
        std::chrono::steady_clock::time_point measurementStart{};
        bool measurementStarted = false;

        // --- Added for FPS Tracking ---
        int frameCount = 0;
        float timeAccumulator = 0.0f;

        while (!glfwWindowShouldClose(window)) {
            const bool isMeasuredFrame = applicationFrameCount >= config_.warmupFrameCount;
            if (isMeasuredFrame && !measurementStarted) {
                measurementStart = std::chrono::steady_clock::now();
                measurementStarted = true;
            }
            const bool profileFrame = isMeasuredFrame &&
                cpuProfiler_.beginFrame(applicationFrameCount + 1);
            if (profileFrame) {
                beginCpuAllocationFrame();
            }
            {
                CpuScope frameScope(cpuProfiler_, "cpu.frame.total");
                {
                    CpuScope eventScope(cpuProfiler_, "cpu.platform.events");
                    glfwPollEvents();
                }

                // 1. Time & Input
                float currentFrameTime = static_cast<float>(glfwGetTime());
                deltaTime = currentFrameTime - lastFrameTime;
                lastFrameTime = currentFrameTime;

                // --- FPS CALCULATION ---
                frameCount++;
                timeAccumulator += deltaTime;

                // Update the window title once every second
                if (timeAccumulator >= 1.0f) {
                    char title[96]{};
                    const float millisecondsPerFrame = frameCount != 0
                        ? 1000.0f / static_cast<float>(frameCount)
                        : 0.0f;
                    std::snprintf(title, sizeof(title),
                        "Iridium Engine - FPS: %u (%.2f ms/frame)",
                        frameCount, millisecondsPerFrame);
                    glfwSetWindowTitle(window, title);

                    frameCount = 0;
                    timeAccumulator -= 1.0f;
                }

                if (!activeBenchmark_) {
                    processInput(window);
                }

                updateBenchmarkState(applicationFrameCount);

                // 2. Process delayed ECS events (like swapping meshes on the main thread)
                ProcessMeshSwaps();

                {
                    CpuScope assetScope(
                        cpuProfiler_,
                        "cpu.asset_runtime.tick");
                    const AssetRuntimeServiceTick
                        assetTick =
                            assetRuntimeService_
                                ->tick();
                    const AssetRuntimeServiceStats
                        assetStats =
                            assetRuntimeService_
                                ->stats();
                    cpuProfiler_.recordCounter(
                        "asset.change_batches",
                        assetTick.changeBatches);
                    cpuProfiler_.recordCounter(
                        "asset.rebuilds_requested",
                        assetTick.rebuildsRequested);
                    cpuProfiler_.recordCounter(
                        "asset.rebuilds.total",
                        assetStats.reimport.enqueued);
                    cpuProfiler_.recordCounter(
                        "asset.source.events",
                        assetStats.source.watcher
                            .changes);
                    cpuProfiler_.recordCounter(
                        "asset.source.same_content",
                        assetStats.source.tracker
                            .sameContent);
                    cpuProfiler_.recordCounter(
                        "asset.reimport.cancel_requests",
                        assetStats.reimport
                            .cancellationRequests);
                    cpuProfiler_.recordCounter(
                        "asset.publish.count",
                        assetTick.publication.published);
                    cpuProfiler_.recordCounter(
                        "asset.publish.total",
                        assetStats.publisher.published);
                    cpuProfiler_.recordCounter(
                        "asset.publish.failed",
                        assetTick.publication.failed +
                            assetTick.reimport.failed);
                    cpuProfiler_.recordCounter(
                        "asset.publish.failures_total",
                        assetStats.publisher.failed);
                    cpuProfiler_.recordCounter(
                        "asset.upload.bytes",
                        assetTick.publication
                            .scheduledUploadBytes);
                    cpuProfiler_.recordCounter(
                        "asset.upload.bytes_total",
                        assetStats.publisher
                            .scheduledUploadBytes);
                    cpuProfiler_.recordCounter(
                        "asset.publish.queued",
                        assetStats.publisher.queued);
                    cpuProfiler_.recordCounter(
                        "asset.resident.count",
                        assetStats.publisher.resident);
                    cpuProfiler_.recordCounter(
                        "asset.resident.cpu_bytes",
                        assetStats.publisher
                            .cpuResidentBytes);
                    cpuProfiler_.recordCounter(
                        "asset.resident.gpu_bytes",
                        assetStats.publisher
                            .gpuResidentBytes);
                    cpuProfiler_.recordCounter(
                        "asset.evictions",
                        assetStats.publisher.evicted);
                    cpuProfiler_.recordCounter(
                        "asset.retirements",
                        assetStats.publisher.retired);
                }

                // 3. Update ECS Systems (Physics, Transforms, Animations)
                // This recalculates all local/world matrices before we extract them.
                {
                    CpuScope transformScope(cpuProfiler_, "cpu.scene.transforms");
                    changedTransformsThisFrame_ = transformSystem.update(
                        registry, &changedTransformEntities_);
                }

                if (isMeasuredFrame && config_.validateOrdinary2Resize) {
                    updateOrdinary2ResizeValidation(measuredFrameCount_);
                }
                if (isMeasuredFrame && config_.validateWeightedOitResize) {
                    updateWeightedOitResizeValidation(measuredFrameCount_);
                }
                if (isMeasuredFrame && config_.validateDepthPyramidResize) {
                    updateDepthPyramidResizeValidation(measuredFrameCount_);
                }
                const bool validateDeepLayeredLifecycle = isMeasuredFrame &&
                    config_.validateDeepLayeredLifecycle &&
                    updateDeepLayeredLifecycleValidation(
                        measuredFrameCount_);

                // 4. The frame acquisition must precede UI construction so the
                // viewport texture IDs correspond to the image acquired this frame.
                const std::optional<uint64_t> captureFrameIndex =
                    isMeasuredFrame && config_.captureFrameIndex == measuredFrameCount_
                    ? config_.captureFrameIndex
                    : std::nullopt;
                const bool validateOrdinary2Capture = isMeasuredFrame &&
                    ((config_.validateOrdinary2Capture &&
                        measuredFrameCount_ == 0u) ||
                    (config_.validateOrdinary2Resize &&
                        measuredFrameCount_ == 5u));
                const bool validateDeepLayeredCapture = isMeasuredFrame &&
                    ((config_.validateDeepLayeredCapture &&
                        measuredFrameCount_ == 0u) ||
                    validateDeepLayeredLifecycle);
                const bool validateDepthPyramidCapture = isMeasuredFrame &&
                    config_.validateDepthPyramidCapture &&
                    measuredFrameCount_ == 0u;
                drawFrame(captureFrameIndex, applicationFrameCount,
                    validateOrdinary2Capture,
                    validateDeepLayeredCapture,
                    validateDepthPyramidCapture);
            }
            if (profileFrame) {
                const CpuAllocationFrameSample allocationSample =
                    endCpuAllocationFrame();
                cpuProfiler_.recordCounter("allocation.cpp.calls",
                    allocationSample.allocationCount);
                cpuProfiler_.recordCounter("allocation.cpp.bytes",
                    allocationSample.requestedBytes);
                (void)cpuProfiler_.endFrame();
            }

            ++applicationFrameCount;
            if (isMeasuredFrame) {
                ++measuredFrameCount_;
            }
            if (config_.frameLimit != 0 && measuredFrameCount_ >= config_.frameLimit) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        if (measurementStarted) {
            measurementWallNanoseconds_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - measurementStart).count());
            const uint64_t averageNanoseconds = measuredFrameCount_ != 0
                ? measurementWallNanoseconds_ / measuredFrameCount_
                : 0;
            std::cout << "IRIDIUM_RUN_METRICS {\"measured_frames\":"
                << measuredFrameCount_ << ",\"wall_ns\":" << measurementWallNanoseconds_
                << ",\"average_ns\":" << averageNanoseconds
                << ",\"render_width\":" << renderExtent_.width
                << ",\"render_height\":" << renderExtent_.height
                << ",\"window_visible\":" << (config_.windowVisible ? "true" : "false")
                << ",\"window_decorated\":" << (config_.windowDecorated ? "true" : "false")
                << ",\"validation\":" << (config_.enableValidation ? "true" : "false")
                << ",\"cpu_profiling\":" << (cpuProfiler_.isEnabled() ? "true" : "false")
                << "}\n";
        }
    }

    void Application::updateOrdinary2ResizeValidation(
        uint64_t measuredFrameIndex) {
        if (measuredFrameIndex >= 3u) {
            cpuProfiler_.recordCounter(
                "ordinary2.lifecycle.resize.requests", 0u);
            cpuProfiler_.recordCounter(
                "ordinary2.lifecycle.resize.successes", 0u);
            cpuProfiler_.recordCounter(
                "ordinary2.lifecycle.resize.failures", 0u);
            return;
        }

        const std::array<RenderExtent, 3> sequence{{
            { 960u, 540u },
            { 1600u, 900u },
            ordinary2ResizeValidation_.originalExtent,
        }};
        const RenderExtent requested = sequence[measuredFrameIndex];
        ++ordinary2ResizeValidation_.requests;

        std::string diagnostic;
        bool resized = false;
        {
            CpuScope resizeScope(
                cpuProfiler_, "cpu.ordinary2.lifecycle.resize");
            resized = renderBackend->resizeSceneRenderExtent(
                requested, diagnostic);
        }
        if (resized) {
            ++ordinary2ResizeValidation_.successes;
            renderExtent_ = renderBackend->getRenderExtent();
            ordinary2ResizeValidation_.lastDiagnostic.clear();
        }
        else {
            ++ordinary2ResizeValidation_.failures;
            ordinary2ResizeValidation_.lastDiagnostic = diagnostic.empty()
                ? "scene target resize failed without a diagnostic"
                : std::move(diagnostic);
        }
        cpuProfiler_.recordCounter(
            "ordinary2.lifecycle.resize.requests", 1u);
        cpuProfiler_.recordCounter(
            "ordinary2.lifecycle.resize.successes", resized ? 1u : 0u);
        cpuProfiler_.recordCounter(
            "ordinary2.lifecycle.resize.failures", resized ? 0u : 1u);
        std::cout << "IRIDIUM_ORDINARY2_RESIZE_EVENT {\"measured_frame\":"
            << measuredFrameIndex << ",\"requested\":["
            << requested.width << ',' << requested.height
            << "],\"effective\":[" << renderExtent_.width << ','
            << renderExtent_.height << "],\"success\":"
            << (resized ? "true" : "false") << "}\n" << std::flush;
    }

    void Application::updateWeightedOitResizeValidation(
        uint64_t measuredFrameIndex) {
        if (measuredFrameIndex >= 3u) {
            cpuProfiler_.recordCounter(
                "weighted_oit.lifecycle.resize.requests", 0u);
            cpuProfiler_.recordCounter(
                "weighted_oit.lifecycle.resize.successes", 0u);
            cpuProfiler_.recordCounter(
                "weighted_oit.lifecycle.resize.failures", 0u);
            return;
        }

        const std::array<RenderExtent, 3> sequence{{
            { 960u, 540u },
            { 1600u, 900u },
            weightedOitResizeValidation_.originalExtent,
        }};
        const RenderExtent requested = sequence[measuredFrameIndex];
        ++weightedOitResizeValidation_.requests;

        std::string diagnostic;
        bool resized = false;
        {
            CpuScope resizeScope(
                cpuProfiler_, "cpu.weighted_oit.lifecycle.resize");
            resized = renderBackend->resizeSceneRenderExtent(
                requested, diagnostic);
        }
        if (resized) {
            ++weightedOitResizeValidation_.successes;
            renderExtent_ = renderBackend->getRenderExtent();
            weightedOitResizeValidation_.lastDiagnostic.clear();
        }
        else {
            ++weightedOitResizeValidation_.failures;
            weightedOitResizeValidation_.lastDiagnostic = diagnostic.empty()
                ? "scene target resize failed without a diagnostic"
                : std::move(diagnostic);
        }
        cpuProfiler_.recordCounter(
            "weighted_oit.lifecycle.resize.requests", 1u);
        cpuProfiler_.recordCounter(
            "weighted_oit.lifecycle.resize.successes", resized ? 1u : 0u);
        cpuProfiler_.recordCounter(
            "weighted_oit.lifecycle.resize.failures", resized ? 0u : 1u);
        std::cout << "IRIDIUM_WEIGHTED_OIT_RESIZE_EVENT {\"measured_frame\":"
            << measuredFrameIndex << ",\"requested\":["
            << requested.width << ',' << requested.height
            << "],\"effective\":[" << renderExtent_.width << ','
            << renderExtent_.height << "],\"success\":"
            << (resized ? "true" : "false") << "}\n" << std::flush;
    }

    void Application::updateDepthPyramidResizeValidation(
        uint64_t measuredFrameIndex) {
        std::optional<RenderExtent> requested;
        if (measuredFrameIndex == 0u) requested = RenderExtent{ 960u, 540u };
        else if (measuredFrameIndex == 3u)
            requested = RenderExtent{ 1600u, 900u };
        else if (measuredFrameIndex == 6u)
            requested = depthPyramidResizeValidation_.originalExtent;

        if (!requested) {
            cpuProfiler_.recordCounter(
                "depth.occlusion.lifecycle.resize.requests", 0u);
            cpuProfiler_.recordCounter(
                "depth.occlusion.lifecycle.resize.successes", 0u);
            cpuProfiler_.recordCounter(
                "depth.occlusion.lifecycle.resize.failures", 0u);
            return;
        }

        ++depthPyramidResizeValidation_.requests;
        std::string diagnostic;
        bool resized = false;
        {
            CpuScope resizeScope(
                cpuProfiler_, "cpu.depth.occlusion.lifecycle.resize");
            resized = renderBackend->resizeSceneRenderExtent(
                *requested, diagnostic);
        }
        if (resized) {
            ++depthPyramidResizeValidation_.successes;
            renderExtent_ = renderBackend->getRenderExtent();
            depthPyramidResizeValidation_.lastDiagnostic.clear();
        }
        else {
            ++depthPyramidResizeValidation_.failures;
            depthPyramidResizeValidation_.lastDiagnostic = diagnostic.empty()
                ? "scene target resize failed without a diagnostic"
                : std::move(diagnostic);
        }
        cpuProfiler_.recordCounter(
            "depth.occlusion.lifecycle.resize.requests", 1u);
        cpuProfiler_.recordCounter(
            "depth.occlusion.lifecycle.resize.successes", resized ? 1u : 0u);
        cpuProfiler_.recordCounter(
            "depth.occlusion.lifecycle.resize.failures", resized ? 0u : 1u);
        std::cout << "IRIDIUM_DEPTH_PYRAMID_RESIZE_EVENT {\"measured_frame\":"
            << measuredFrameIndex << ",\"requested\":["
            << requested->width << ',' << requested->height
            << "],\"effective\":[" << renderExtent_.width << ','
            << renderExtent_.height << "],\"success\":"
            << (resized ? "true" : "false") << "}\n" << std::flush;
    }

    bool Application::updateDeepLayeredLifecycleValidation(
        uint64_t measuredFrameIndex) {
        using State = DeepLayeredLifecycleValidationState;
        const RenderBackendRuntimeInfo runtimeInfo =
            renderBackend->getRuntimeInfo();
        const bool selectedTierResident =
            config_.deepLayeredCaptureQuality == TransparencyQuality::Hero4
                ? runtimeInfo.hero4AtlasResident
                : runtimeInfo.cinematic8AtlasResident;
        const bool topologyResident = selectedTierResident &&
            runtimeInfo.refractionPyramidsResident;

        const auto setBenchmarkModelEnabled = [&](bool enabled) {
            auto* meshes = registry.getPool<MeshComponent>();
            uint32_t changed = 0u;
            if (meshes != nullptr) {
                for (const BenchmarkInstanceState& instance :
                        benchmarkInstances_) {
                    if (!meshes->has(instance.entity)) continue;
                    MeshComponent& mesh = meshes->get(instance.entity);
                    if (!mesh.model || mesh.model != mainModel ||
                        mesh.enabled == enabled) {
                        continue;
                    }
                    mesh.enabled = enabled;
                    ++changed;
                }
            }
            if (changed == 0u) {
                throw std::runtime_error(
                    "Deep layered lifecycle validation could not change the benchmark model visibility.");
            }
            ++deepLayeredLifecycleValidation_.visibilityChanges;
        };
        const auto logEvent = [&](std::string_view event) {
            std::cout
                << "IRIDIUM_DEEP_LAYERED_LIFECYCLE_EVENT {\"measured_frame\":"
                << measuredFrameIndex << ",\"event\":\"" << event
                << "\",\"quality\":\""
                << transparencyQualityName(
                    config_.deepLayeredCaptureQuality)
                << "\",\"tier_resident\":"
                << (selectedTierResident ? "true" : "false")
                << ",\"refraction_pyramids_resident\":"
                << (runtimeInfo.refractionPyramidsResident
                    ? "true" : "false")
                << "}\n" << std::flush;
        };

        switch (deepLayeredLifecycleValidation_.phase) {
        case State::Phase::Initial:
            if (!topologyResident) {
                throw std::runtime_error(
                    "Deep layered lifecycle validation did not start with a resident selected tier.");
            }
            deepLayeredLifecycleValidation_.firstMeasuredFrame =
                measuredFrameIndex;
            setBenchmarkModelEnabled(false);
            deepLayeredLifecycleValidation_.phase =
                State::Phase::WaitingForRetirement;
            logEvent("hide_for_retirement");
            return false;
        case State::Phase::WaitingForRetirement:
            if (selectedTierResident ||
                runtimeInfo.refractionPyramidsResident) {
                return false;
            }
            ++deepLayeredLifecycleValidation_.retirements;
            setBenchmarkModelEnabled(true);
            deepLayeredLifecycleValidation_.phase =
                State::Phase::WaitingForReactivation;
            logEvent("retired_reveal_for_reactivation");
            return false;
        case State::Phase::WaitingForReactivation:
            if (!topologyResident) return false;
            ++deepLayeredLifecycleValidation_.reactivations;
            if (deepLayeredLifecycleValidation_.reactivations < 2u) {
                setBenchmarkModelEnabled(false);
                deepLayeredLifecycleValidation_.phase =
                    State::Phase::WaitingForRetirement;
                logEvent("reactivated_hide_for_retirement");
                return false;
            }
            deepLayeredLifecycleValidation_.completionMeasuredFrame =
                measuredFrameIndex;
            deepLayeredLifecycleValidation_.phase = State::Phase::Complete;
            logEvent("reactivated_complete");
            return true;
        case State::Phase::Complete:
            return false;
        }
        return false;
    }

    std::string Application::persistBakedReflectionProbe(
        SceneEntityUuid owner,
        const ReflectionProbeCaptureCompletion::Product& captured) {
        try {
            const AssetGuid sceneGuid = sceneDocumentService_.sceneAssetGuid();
            const std::filesystem::path scenePath =
                sceneDocumentService_.currentPath();
            if (sceneGuid.isNil() || scenePath.empty())
                return "Bake failed: save the scene once to establish stable asset identity.";
            if (sceneDocumentService_.dirty())
                return "Bake failed: save the scene first so capture provenance matches it.";
            if (captured.resolution == 0 || captured.mipLevels == 0 ||
                captured.radiance.empty() ||
                captured.prefilteredSpecular.empty())
                return "Bake failed: GPU capture readback is incomplete.";

            const LoadedEnvironmentAsset* sharedEnvironment = nullptr;
            if (const auto active = loadedEnvironments_.find(
                    activeEnvironmentAssetGuid_);
                active != loadedEnvironments_.end())
                sharedEnvironment = &active->second;
            else if (!loadedEnvironments_.empty())
                sharedEnvironment = &loadedEnvironments_.begin()->second;
            if (sharedEnvironment == nullptr ||
                sharedEnvironment->brdfLut.empty())
                return "Bake failed: a resident HDRI is required for the shared BRDF product.";

            const std::filesystem::path projectRoot(PROJECT_ROOT_DIR);
            const std::filesystem::path relativeSource =
                std::filesystem::path("generated") / "reflection-probes" /
                sceneGuid.toString() / (owner.toString() + ".irprobe");
            const std::filesystem::path sourcePath =
                projectRoot / "assets" / relativeSource;
            const std::filesystem::path metadataPath =
                assetMetadataSidecarPath(sourcePath);

            AssetMetadata metadata;
            std::error_code filesystemError;
            if (std::filesystem::exists(metadataPath, filesystemError) &&
                !filesystemError) {
                AssetMetadataReadResult existing =
                    readAssetMetadata(metadataPath);
                if (!existing.metadata || existing.hasErrors())
                    return "Bake failed: the existing generated asset metadata is invalid.";
                metadata = std::move(*existing.metadata);
                if (metadata.assetType != "iridium.environment" ||
                    metadata.importerId !=
                        "iridium.environment.probe_capture")
                    return "Bake failed: the generated path belongs to an incompatible asset.";
            } else {
                if (filesystemError)
                    return "Bake failed: could not inspect generated asset metadata.";
                metadata.assetGuid = createAssetGuidV7();
                metadata.assetType = "iridium.environment";
                metadata.importerId =
                    "iridium.environment.probe_capture";
                metadata.importerVersion = 1;
                metadata.settingsSchemaVersion = 1;
                metadata.settings = nlohmann::json::object();
            }
            metadata.tags = { "generated", "reflection-probe" };

            constexpr uint32_t IrradianceSize = 32;
            std::vector<std::byte> irradiance =
                makeCapturedCubeDiffuseIrradiance(captured.radiance,
                    captured.resolution, IrradianceSize);
            const CookedEnvironmentManifest manifest{
                .sourceTextureGuid = metadata.assetGuid,
                .sourcePrimaries = "acescg_ap1_d60",
                .sourceRadianceScale = 1.0f,
                .convolutionImplementation =
                    "iridium_gpu_scene_capture_ggx_v1",
                .sampleSequence =
                    "exact_cube_texel_sh9_v1+hammersley_base2_vdc_v1_" +
                    std::to_string(
                        config_.reflectionProbeSettings.prefilterSampleCount),
                .toolVersion = std::string("Iridium ") +
                    IRIDIUM_SOURCE_COMMIT + " " + IRIDIUM_BUILD_CONFIGURATION,
                .radiance = { captured.resolution, captured.resolution, 1, 6,
                    TextureFormat::RGBA16_SFloat },
                .irradiance = { IrradianceSize, IrradianceSize, 1, 6,
                    TextureFormat::RGBA16_SFloat },
                .prefilteredSpecular = { captured.resolution,
                    captured.resolution, captured.mipLevels, 6,
                    TextureFormat::RGBA16_SFloat },
                .brdfLut = sharedEnvironment->manifest.brdfLut,
            };
            CookProduct product = makeCookedEnvironmentProduct(manifest,
                { captured.radiance, irradiance,
                    captured.prefilteredSpecular,
                    sharedEnvironment->brdfLut });
            if (hasCookErrors(product.diagnostics))
                return cookFailureMessage("Bake failed", product.diagnostics);

            const std::string sceneHash = sha256File(scenePath);
            std::string sourceIdentity = sceneGuid.toString() + "\n" +
                owner.toString() + "\n" + sceneHash + "\n" +
                sha256(captured.radiance) + "\n" +
                sha256(captured.prefilteredSpecular) + "\n" +
                sha256(irradiance) + "\n" +
                sha256(sharedEnvironment->brdfLut);
            const std::string sourceHash = sha256(std::as_bytes(std::span(
                sourceIdentity.data(), sourceIdentity.size())));
            std::vector<AssetDependency> dependencies{
                {
                    .type = AssetDependencyType::Asset,
                    .assetGuid = sceneGuid,
                    .location = std::filesystem::relative(scenePath,
                        projectRoot, filesystemError).generic_string(),
                    .contentHash = sceneHash,
                },
                {
                    .type = AssetDependencyType::Asset,
                    .assetGuid = sharedEnvironment->assetGuid,
                    .location = "environment/" +
                        sharedEnvironment->assetGuid.toString(),
                },
            };
            if (filesystemError) {
                filesystemError.clear();
                dependencies[0].location = scenePath.generic_string();
            }
            for (const char* shader : {
                    "reflection_probe_capture.vert",
                    "reflection_probe_capture.frag",
                    "reflection_probe_prefilter.comp" }) {
                const std::filesystem::path shaderPath =
                    projectRoot / "assets" / "shaders" / shader;
                dependencies.push_back({
                    .type = AssetDependencyType::Tool,
                    .location = (std::filesystem::path("assets") /
                        "shaders" / shader).generic_string(),
                    .contentHash = sha256File(shaderPath),
                });
            }
            std::ranges::sort(dependencies);
            const CookTarget target{
                .platform = "windows-x64",
                .profile = "editor",
                .qualityPolicy = "reflection-probe-high",
                .artifactContainerVersion = kCookedArtifactContainerVersion,
                .materialSchemaVersion = 2,
            };
            static constexpr std::byte EmptySettings[]{
                std::byte{ '{' }, std::byte{ '}' },
            };
            const std::string cookKey = calculateCookKey({
                .assetGuid = metadata.assetGuid,
                .importerId = "iridium.environment.probe_capture",
                .importerImplementationVersion = 1,
                .settingsSchemaVersion = 1,
                .canonicalSettings = EmptySettings,
                .sourceContentHash = sourceHash,
                .dependencies = dependencies,
                .target = target,
                .cookerFeatureVersion =
                    "reflection-probe-scene-capture-v1",
            });
            const CookedArtifact artifact{
                .assetGuid = metadata.assetGuid,
                .artifactType = product.artifactType,
                .artifactSchemaVersion = product.artifactSchemaVersion,
                .target = target,
                .cookKey = cookKey,
                .dependencies = std::move(dependencies),
                .sections = std::move(product.sections),
            };
            const CookedArtifactBlob blob = serializeCookedArtifact(artifact);
            std::string writeError;
            if (!writeBinaryAtomic(sourcePath, blob.bytes, writeError))
                return "Bake failed: " + writeError;
            if (!writeAssetMetadataAtomic(metadataPath, metadata, writeError))
                return "Bake product was written, but metadata publication failed: " +
                    writeError;
            if (assetCatalogService_)
                (void)assetCatalogService_->requestRefresh();
            return "Baked environment " + metadata.assetGuid.toString() +
                " to " + relativeSource.generic_string() +
                ". Assign it from the Asset Browser when ready.";
        } catch (const std::exception& exception) {
            return std::string("Bake failed: ") + exception.what();
        }
    }

    void Application::prepareGpuScenePublication(Entity selectedEntity) {
        gpuSceneFrame_ = nullptr;
        gpuSceneDirectFallbackCount_ = 0;
        if (!gpuScenePublisher_) return;
        CpuScope publicationScope(cpuProfiler_, "cpu.gpu_scene.publish");
        size_t observationCount = 0;

        {
        CpuScope observationScope(cpuProfiler_, "cpu.gpu_scene.observe");
        auto* transformPool = registry.getPool<TransformComponent>();
        auto* meshPool = registry.getPool<MeshComponent>();
        auto* instanceBatchPool = registry.findPool<
            RenderInstanceBatchComponent>();
        if (transformPool && meshPool) {
            gpuSceneObservations_.reserve(meshPool->entities.size());
            gpuSceneObservationMetadata_.reserve(meshPool->entities.size());
            for (Entity entity : meshPool->entities) {
                const MeshComponent& mesh = meshPool->get(entity);
                if (!mesh.enabled || !mesh.model ||
                    !mesh.model->geometry.isValid() ||
                    !transformPool->has(entity)) continue;
                if (instanceBatchPool && instanceBatchPool->has(entity)) {
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                const auto owner = sceneWorld_.identities().persistentId(entity);
                if (!owner || owner->isNil()) {
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                const ModelAsset& model = *mesh.model;
                if (observationCount == gpuSceneObservations_.size()) {
                    gpuSceneObservations_.emplace_back();
                    gpuSceneObservationMetadata_.emplace_back();
                }
                GpuSceneObservedInstance& observation =
                    gpuSceneObservations_[observationCount];
                GpuSceneObservationMetadata& metadata =
                    gpuSceneObservationMetadata_[observationCount];
                const glm::mat4 previousWorld = observation.worldTransform;
                const uint32_t previousFlags = observation.flags;
                const uint32_t previousMaximumLod = observation.maximumLod;
                const uint32_t previousConsumerMask =
                    observation.consumerMask;
                uint64_t overrideSignature = 1469598103934665603ull;
                const auto mix = [&overrideSignature](uint64_t value) {
                    overrideSignature ^= value;
                    overrideSignature *= 1099511628211ull;
                };
                for (const MeshComponent::MaterialOverride& value :
                        mesh.materialOverrides) {
                    for (uint8_t byte : value.sourceMaterialGuid.bytes()) mix(byte);
                    for (uint8_t byte : value.materialGuid.bytes()) mix(byte);
                    if (const auto resolved =
                            assetManager->findCookedMaterialRuntime(
                                value.materialGuid)) {
                        mix(resolved->binding.material.id);
                        mix(resolved->binding.pipeline.id);
                        mix(static_cast<uint32_t>(
                            resolved->binding.renderQueue));
                    }
                }
                const bool rebuildPrimitives = metadata.owner != *owner ||
                    metadata.model != &model ||
                    metadata.geometry != model.geometry ||
                    metadata.cookKey != model.artifactCookKey ||
                    metadata.materialOverrideSignature != overrideSignature;
                observation.identity.owner = *owner;
                observation.identity.modelAssetGuid = model.assetGuid;
                observation.identity.publishedRevision = 1;
                observation.identity.artifactCookKey = model.artifactCookKey;
                observation.mobility = GpuSceneMobility::Movable;
                observation.flags = GpuSceneInstanceEnabled |
                    (entity == selectedEntity
                        ? GpuSceneInstanceSelected : 0u);
                observation.maximumLod = static_cast<uint32_t>(
                    std::clamp(mesh.maximumLodLevel, 0,
                        MeshComponent::MaximumLodLevel));
                observation.consumerMask = metadata.baseConsumerMask |
                    (entity == selectedEntity
                        ? GpuSceneConsumerSelection : 0u);
                observation.worldTransform =
                    transformPool->get(entity).worldMatrix;
                if (rebuildPrimitives) {
                    metadata.owner = *owner;
                    metadata.model = &model;
                    metadata.geometry = model.geometry;
                    metadata.cookKey = model.artifactCookKey;
                    metadata.materialOverrideSignature = overrideSignature;
                    metadata.localMinimum = glm::vec3(
                        (std::numeric_limits<float>::max)());
                    metadata.localMaximum = glm::vec3(
                        (std::numeric_limits<float>::lowest)());
                    metadata.baseConsumerMask = 0;
                    metadata.valid = true;
                    observation.primitives.clear();
                    for (const SubMesh& subMesh : model.subMeshes) {
                    if (subMesh.materialIndex < 0 ||
                        static_cast<size_t>(subMesh.materialIndex) >=
                            model.materials.size()) continue;
                    const MaterialBinding* binding = &model.materials[
                        static_cast<size_t>(subMesh.materialIndex)];
                    AssetGuid effectiveMaterialGuid = subMesh.materialGuid;
                    const auto materialOverride = std::ranges::find_if(
                        mesh.materialOverrides,
                        [&subMesh](const MeshComponent::MaterialOverride& value) {
                            return value.sourceMaterialGuid == subMesh.materialGuid;
                        });
                    std::optional<CookedMaterialRuntimeBinding> overrideBinding;
                    if (materialOverride != mesh.materialOverrides.end()) {
                        overrideBinding = assetManager->findCookedMaterialRuntime(
                            materialOverride->materialGuid);
                        if (overrideBinding) {
                            binding = &overrideBinding->binding;
                            effectiveMaterialGuid = materialOverride->materialGuid;
                        }
                    }
                    if (binding->renderQueue == RenderQueue::Transparent) continue;
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid() ||
                        subMesh.sourcePrimitiveGuid.isNil() ||
                        subMesh.primitiveGuid.isNil() ||
                        effectiveMaterialGuid.isNil() ||
                        subMesh.indexCount == 0) {
                        metadata.valid = false;
                        break;
                    }
                    uint32_t primitiveFlags = GpuScenePrimitiveOpaque;
                    if (subMesh.coverage == static_cast<uint8_t>(
                            ModelCoverage::Masked))
                        primitiveFlags |= GpuScenePrimitiveAlphaMask;
                    if ((subMesh.flags & ModelPrimitiveDoubleSided) != 0)
                        primitiveFlags |= GpuScenePrimitiveTwoSided;
                    metadata.baseConsumerMask |= binding->renderQueue ==
                        RenderQueue::ForwardOpaque
                        ? GpuSceneConsumerForwardOpaque
                        : GpuSceneConsumerMainOpaque;
                    metadata.baseConsumerMask |= GpuSceneConsumerShadow |
                        GpuSceneConsumerProbe;
                    observation.primitives.push_back({
                        .identity = { *owner, subMesh.sourcePrimitiveGuid,
                            subMesh.primitiveGuid, effectiveMaterialGuid },
                        .geometryIdentity = { model.assetGuid,
                            subMesh.sourcePrimitiveGuid,
                            subMesh.primitiveGuid },
                        .legacyGeometry = subMesh.geometry.isValid()
                            ? subMesh.geometry : model.geometry,
                        .material = binding->material,
                        .pipeline = binding->pipeline,
                        .firstIndex = subMesh.indexStart,
                        .indexCount = subMesh.indexCount,
                        .vertexOffset = subMesh.vertexOffset,
                        .indexType = subMesh.indexFormat,
                        .vertexLayout = subMesh.attributeMask,
                        .primitiveFlags = primitiveFlags,
                        .consumerMask = binding->renderQueue ==
                            RenderQueue::ForwardOpaque
                            ? GpuSceneConsumerForwardOpaque |
                                GpuSceneConsumerShadow |
                                GpuSceneConsumerProbe
                            : GpuSceneConsumerMainOpaque |
                                GpuSceneConsumerShadow |
                                GpuSceneConsumerProbe,
                        .localBoundsSphere = glm::vec4(
                            subMesh.boundsSphereCenter,
                            subMesh.boundsSphereRadius),
                        .localBoundsMin = glm::vec4(subMesh.boundsMin, 0.0f),
                        .localBoundsMax = glm::vec4(subMesh.boundsMax, 0.0f),
                        .geometryProductRevision = static_cast<uint32_t>(
                            observation.identity.publishedRevision),
                        .materialRevision = binding->material.id,
                    });
                    const auto lodChain = std::ranges::find(model.lodChains,
                        subMesh.primitiveGuid, &ModelLodChain::basePrimitiveGuid);
                    if (lodChain != model.lodChains.end()) {
                        auto& children = observation.primitives.back().lodChildren;
                        for (size_t levelIndex = 1; levelIndex < lodChain->levels.size(); ++levelIndex) {
                            const auto& level = lodChain->levels[levelIndex];
                            const SubMesh& child = level.subMesh;
                            children.push_back({
                                .identity = { model.assetGuid, child.sourcePrimitiveGuid, child.primitiveGuid },
                                .geometry = child.geometry,
                                .firstIndex = child.indexStart, .indexCount = child.indexCount,
                                .vertexOffset = child.vertexOffset,
                                .indexType = child.indexFormat, .vertexLayout = child.attributeMask,
                                .localBoundsSphere = glm::vec4(child.boundsSphereCenter, child.boundsSphereRadius),
                                .localBoundsMin = glm::vec4(child.boundsMin, 0.0f),
                                .localBoundsMax = glm::vec4(child.boundsMax, 0.0f),
                                .geometricError = level.geometricError,
                            });
                        }
                    }
                    metadata.localMinimum = glm::min(
                        metadata.localMinimum, subMesh.boundsMin);
                    metadata.localMaximum = glm::max(
                        metadata.localMaximum, subMesh.boundsMax);
                    }
                    metadata.valid = metadata.valid &&
                        !observation.primitives.empty();
                }
                observation.consumerMask = metadata.baseConsumerMask |
                    (entity == selectedEntity
                        ? GpuSceneConsumerSelection : 0u);
                if (!metadata.valid) {
                    observation.primitives.clear();
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                glm::vec3 worldMinimum((std::numeric_limits<float>::max)());
                glm::vec3 worldMaximum((std::numeric_limits<float>::lowest)());
                for (uint32_t corner = 0; corner < 8u; ++corner) {
                    const glm::vec3 local{
                        (corner & 1u) ? metadata.localMaximum.x
                            : metadata.localMinimum.x,
                        (corner & 2u) ? metadata.localMaximum.y
                            : metadata.localMinimum.y,
                        (corner & 4u) ? metadata.localMaximum.z
                            : metadata.localMinimum.z,
                    };
                    const glm::vec3 world = glm::vec3(
                        observation.worldTransform * glm::vec4(local, 1.0f));
                    worldMinimum = glm::min(worldMinimum, world);
                    worldMaximum = glm::max(worldMaximum, world);
                }
                const glm::vec3 center = (worldMinimum + worldMaximum) * 0.5f;
                observation.worldBoundsSphere = glm::vec4(center,
                    glm::length(worldMaximum - center));
                observation.worldBoundsMin = glm::vec4(worldMinimum, 0.0f);
                observation.worldBoundsMax = glm::vec4(worldMaximum, 0.0f);
                const bool observationChanged = rebuildPrimitives ||
                    std::memcmp(&previousWorld, &observation.worldTransform,
                        sizeof(previousWorld)) != 0 ||
                    previousFlags != observation.flags ||
                    previousMaximumLod != observation.maximumLod ||
                    previousConsumerMask != observation.consumerMask;
                if (observationChanged) {
                    if (metadata.observationRevision ==
                            (std::numeric_limits<uint64_t>::max)()) {
                        throw std::overflow_error(
                            "GPU-scene observation revision exhausted");
                    }
                    ++metadata.observationRevision;
                }
                observation.observationRevision =
                    metadata.observationRevision;
                ++observationCount;
            }
        }
        gpuSceneObservations_.resize(observationCount);
        gpuSceneObservationMetadata_.resize(observationCount);
        }

        const GpuSceneFrameSerials serials =
            renderBackend->getGpuSceneFrameSerials();
        {
            CpuScope synchronizeScope(
                cpuProfiler_, "cpu.gpu_scene.synchronize");
            gpuSceneFrame_ = &gpuScenePublisher_->synchronize(
                sceneWorld_.stateEpoch(), gpuSceneObservations_,
                serials.lastSubmitted, serials.completed);
        }
        {
            CpuScope capacityScope(cpuProfiler_, "cpu.gpu_scene.prepare");
            renderBackend->prepareGpuScene({
                static_cast<uint32_t>(gpuSceneFrame_->transforms.size()),
                static_cast<uint32_t>(gpuSceneFrame_->instances.size()),
                static_cast<uint32_t>(gpuSceneFrame_->primitives.size()),
                static_cast<uint32_t>(gpuSceneFrame_->geometries.size()),
            });
        }
        const GpuScenePublisherStats& stats = gpuScenePublisher_->stats();
        cpuProfiler_.recordCounter("gpu_scene.instance.active",
            stats.activeInstances);
        cpuProfiler_.recordCounter("gpu_scene.primitive.active",
            stats.activePrimitives);
        cpuProfiler_.recordCounter("gpu_scene.geometry.active",
            stats.activeGeometries);
        cpuProfiler_.recordCounter("gpu_scene.transform.changed",
            stats.changedTransforms);
        cpuProfiler_.recordCounter("gpu_scene.instance.changed",
            stats.changedInstances);
        cpuProfiler_.recordCounter("gpu_scene.primitive.changed",
            stats.changedPrimitives);
        cpuProfiler_.recordCounter("gpu_scene.geometry.changed",
            stats.changedGeometries);
        cpuProfiler_.recordCounter("gpu_scene.publication.unchanged_fast_path",
            stats.unchangedFastPath);
        cpuProfiler_.recordCounter("gpu_scene.direct_fallback",
            gpuSceneDirectFallbackCount_ + stats.capacityFallbackInstances);
    }

    void Application::drawFrame(std::optional<uint64_t> captureFrameIndex,
        uint64_t applicationFrameIndex,
        bool validateOrdinary2Capture,
        bool validateDeepLayeredCapture,
        bool validateDepthPyramidCapture) {
        const bool dualViews = !activeBenchmark_ && editor.assetDocuments().active();
        const auto cadenceNow = EditorViewCadence::Clock::now();
        uint32_t renderView = 0;
        if (dualViews) {
            const bool sceneFocused = editor.getViewportPanel().isFocused;
            renderView = editorViewScheduler_.choose(cadenceNow, sceneFocused ? 0u : 1u,
                editor.getViewportPanel().isVisible, editor.getAssetViewerPanel().isVisible,
                editor.getAssetViewerPanel().backgroundFramesPerSecond);
        } else editorViewScheduler_.reset();
        editor.renderingAssetView = renderView == 1;
        for (const ReflectionProbeCaptureCompletion& completion :
                renderBackend->finalizeReflectionProbeCaptures()) {
            reflectionProbeCaptureScheduler_.markPublished(
                completion.owner, completion.captureTicket);
            const std::optional<Entity> entity =
                sceneWorld_.identities().resolve(completion.owner);
            auto* probePool = registry.findPool<ReflectionProbeComponent>();
            if (entity && registry.isAlive(*entity) && probePool &&
                probePool->has(*entity)) {
                probePool->get(*entity).publicationDiagnostic =
                    completion.bakedProduct
                    ? persistBakedReflectionProbe(completion.owner,
                        *completion.bakedProduct)
                    : "Runtime capture published in environment slot " +
                        std::to_string(completion.environmentSlot) + ".";
            }
        }
        LightingFramePacket lightingFrame;
        {
            CpuScope lightScope(cpuProfiler_, "cpu.light.extract");
            if (editor.renderingAssetView) {
                auto& previewRegistry = previewLightingWorld_.registry();
                if (previewSun_ == NULL_ENTITY) {
                    previewSun_ = previewLightingWorld_.createEntity();
                    previewRegistry.addComponent<TransformComponent>(previewSun_);
                    previewRegistry.addComponent<LightComponent>(previewSun_);
                }
                const auto settings = editor.getAssetViewerPanel().activeLighting();
                auto& transform = previewRegistry.getComponent<TransformComponent>(previewSun_);
                transform.rotation = {settings.sunPitchDegrees, settings.sunYawDegrees, 0};
                auto& light = previewRegistry.getComponent<LightComponent>(previewSun_);
                light.colorLinearRec709 = settings.sunColor;
                light.illuminanceLux = settings.sunEnabled ? 1000.0f * std::exp2(settings.sunEv) : 0.0f;
                lightingFrame = lightExtractor_.extract(previewLightingWorld_);
            } else lightingFrame = lightExtractor_.extract(sceneWorld_);
        }
        {
            CpuScope lightScope(cpuProfiler_, "cpu.light.prepare");
            renderBackend->prepareLighting(lightingFrame.requiredCapacity);
        }
        ReflectionProbeFramePacket extractedProbes;
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.extract");
            extractedProbes = extractReflectionProbes(sceneWorld_,
                [this](AssetGuid environment) {
                    return loadedEnvironments_.contains(environment);
                });
            std::vector<SceneEntityUuid> runtimeCaptureOwners;
            runtimeCaptureOwners.reserve(extractedProbes.candidates.size());
            for (const ReflectionProbeCandidate& candidate :
                    extractedProbes.candidates)
                if (candidate.probe.environmentAssetGuid.isNil())
                    runtimeCaptureOwners.push_back(candidate.owner);
            renderBackend->synchronizeReflectionProbeCaptureOwners(
                runtimeCaptureOwners);
            for (ReflectionProbeCandidate& candidate :
                    extractedProbes.candidates) {
                if (!candidate.probe.environmentAssetGuid.isNil()) continue;
                candidate.runtimeEnvironmentSlot = renderBackend->
                    capturedReflectionProbeEnvironmentSlot(candidate.owner);
                if (candidate.runtimeEnvironmentSlot)
                    candidate.resident = true;
            }
        }
        ReflectionProbeGpuFramePacket publishedProbes;
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.publish");
            reflectionProbeEnvironments_.clear();
            reflectionProbeEnvironments_.reserve(
                kMaximumGpuReflectionProbeEnvironments);
            for (const auto& [guid, environment] : loadedEnvironments_) {
                (void)guid;
                if (reflectionProbeEnvironments_.size() >=
                    kMaximumGpuReflectionProbeEnvironments) break;
                reflectionProbeEnvironments_.push_back(environment.lighting);
            }
            publishedProbes = reflectionProbePublisher_.publish(
                extractedProbes.candidates,
                [this](AssetGuid environment) -> std::optional<uint32_t> {
                    const auto found = loadedEnvironments_.find(environment);
                    if (found == loadedEnvironments_.end()) return std::nullopt;
                    const size_t index = static_cast<size_t>(std::distance(
                        loadedEnvironments_.begin(), found));
                    if (index >= reflectionProbeEnvironments_.size())
                        return std::nullopt;
                    return static_cast<uint32_t>(index);
                });
        }
        renderBackend->prepareReflectionProbes(
            publishedProbes.requiredCapacity, reflectionProbeEnvironments_);
        cpuProfiler_.recordCounter("probe.extracted",
            publishedProbes.stats.extractedCandidateCount);
        cpuProfiler_.recordCounter("probe.active",
            publishedProbes.stats.activeProbeCount);
        cpuProfiler_.recordCounter("probe.nonresident",
            publishedProbes.stats.nonresidentProbeCount);
        cpuProfiler_.recordCounter("probe.environment_unresolved",
            publishedProbes.stats.unresolvedEnvironmentCount);
        cpuProfiler_.recordCounter("probe.capacity_omitted",
            publishedProbes.stats.capacityOmittedCount);
        cpuProfiler_.recordCounter("probe.publish.changed_bytes",
            publishedProbes.stats.changedRecordBytes);
        std::vector<AssetGuid> previewDocuments;
        for (const auto& document : editor.assetDocuments().documents()) previewDocuments.push_back(document.assetGuid);
        assetManager->processMaterialPreviews(previewDocuments);
        prepareGpuScenePublication(editor.getSelectedEntity());
        // Descriptor publication waits for old users and must precede acquisition.
        // Keep the authored scene environment identity separate from this binding.
        auto desiredEnvironment = environmentLighting_;
        auto& viewer = editor.getAssetViewerPanel();
        viewer.environmentDiagnostic.clear();
        if (editor.renderingAssetView) {
            const AssetGuid requested = viewer.activeLighting().environmentAsset;
            if (!requested.isNil()) {
                if (const auto loaded = loadedEnvironments_.find(requested); loaded != loadedEnvironments_.end()) {
                    desiredEnvironment = loaded->second.lighting;
                    if (assetRuntimeService_) assetRuntimeService_->touch(requested, applicationFrameIndex);
                } else {
                    viewer.environmentDiagnostic = "Preparing HDRI; showing the scene environment until ready.";
                    const auto state = assetRuntimeService_ ? assetRuntimeService_->snapshot(requested) : std::nullopt;
                    if (state && (state->state == RuntimeAssetState::Failed || state->state == RuntimeAssetState::ReadyWithError))
                        viewer.environmentDiagnostic = state->diagnostic;
                    else if (assetCatalog_ && assetEnvironmentPreparationService_ && !assetEnvironmentPreparationService_->pending(requested)) {
                        const auto records = assetCatalog_->recordsForGuid(requested);
                        const auto record = std::ranges::find_if(records, [](const AssetCatalogRecord& item) {
                            return !item.parentGuid && item.assetType == "iridium.environment" &&
                                item.assetRoot == "project" && item.status == AssetCatalogStatus::Ready;
                        });
                        if (record == records.end()) viewer.environmentDiagnostic = "The selected HDRI is no longer available in this project.";
                        else if (!state || state->state != RuntimeAssetState::Queued) {
                            try { (void)assetEnvironmentPreparationService_->request(*record); }
                            catch (const std::exception& error) { viewer.environmentDiagnostic = error.what(); }
                        }
                    }
                }
            }
        }
        if (desiredEnvironment.isValid() && desiredEnvironment != renderBackend->getEnvironmentLighting())
            renderBackend->setEnvironmentLighting(desiredEnvironment);
        renderBackend->prepareRetainedViews(dualViews, renderView);
        editor.retainedSceneTexture = dualViews ? renderBackend->getRetainedViewTextureID(0) : nullptr;
        editor.retainedAssetTexture = dualViews ? renderBackend->getRetainedViewTextureID(1) : nullptr;
        // If the window was resized, OR acquire requests a swapchain rebuild:
        if (framebufferResized || renderBackend->beginFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        if (gpuSceneFrame_) renderBackend->publishGpuScene(*gpuSceneFrame_);
        const GpuSceneUploadTelemetry gpuSceneUpload =
            renderBackend->getGpuSceneUploadTelemetry();
        cpuProfiler_.recordCounter("gpu_scene.upload.bytes",
            gpuSceneUpload.bytes);
        cpuProfiler_.recordCounter("gpu_scene.upload.ranges",
            gpuSceneUpload.ranges);
        updateTextureResidencyChurn(applicationFrameIndex);

        // --- 1. CLEAR THE QUEUES ---
        opaqueQueue.clear();
        forwardOpaqueQueue.clear();
        transparentQueue.clear();
        sortedSurfaceQueue.clear();
        selectionQueue.clear();
        shadowCasterQueue.clear();
        probeCasterQueue_.clear();
        forwardInstanceTransforms_.clear();

        const EditorAssetDocument* previewDocument = editor.renderingAssetView
            ? editor.assetDocuments().active() : nullptr;
        std::shared_ptr<ModelAsset> previewModel = previewDocument
            ? resolveEditorAssetPreview() : std::shared_ptr<ModelAsset>{};
        bool assetPreviewActive = previewDocument != nullptr;
        const std::span<const uint32_t> shadowGpuScenePrimitiveIndices =
            !assetPreviewActive && gpuSceneFrame_
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->shadowConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};
        const std::span<const uint32_t> probeGpuScenePrimitiveIndices =
            !assetPreviewActive && gpuSceneFrame_ &&
                !config_.forceDirectGBufferReference &&
                !config_.forceDirectProbeCaptureReference
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->probeConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};
        Entity selectedEntity = assetPreviewActive
            ? NULL_ENTITY : editor.getSelectedEntity();

        // --- 2. GET CAMERA DATA ---
        glm::vec3 renderCameraPosition = cameraPos;
        float renderCameraNearPlane = cameraNearPlane_;
        float renderCameraFarPlane = cameraFarPlane_;
        float renderVerticalFovDegrees = verticalFovDegrees_;
        glm::mat4 viewMatrix = glm::lookAt(
            cameraPos, cameraPos + cameraFront, cameraUp);
        const float aspect = renderExtent_.height != 0
            ? static_cast<float>(renderExtent_.width) /
                static_cast<float>(renderExtent_.height)
            : 16.0f / 9.0f;
        glm::mat4 projMatrix = glm::perspective(
            glm::radians(verticalFovDegrees_), aspect,
            cameraNearPlane_, cameraFarPlane_);
        projMatrix[1][1] *= -1.0f; // Vulkan inverted Y
        if (assetPreviewActive) {
            if (const EditorOrbitCamera* camera =
                    editor.getAssetViewerPanel().activeCamera()) {
                renderCameraPosition = camera->position();
                viewMatrix = camera->viewMatrix();
                projMatrix = camera->projectionMatrix(aspect);
                const EditorOrbitCameraState& state = camera->state();
                renderCameraNearPlane = state.nearPlane;
                renderCameraFarPlane = state.farPlane;
                renderVerticalFovDegrees = state.verticalFovDegrees;
            }
        }

        // Build ImGui only after beginFrame selected currentImageIndex. The UI
        // descriptors are per swapchain image, so using them before acquisition
        // can sample a different target that has not yet been transitioned.
        {
            CpuScope editorScope(cpuProfiler_, "cpu.editor.build");
            renderBackend->beginUI();
            if (!activeBenchmark_) {
                glm::mat4 sceneProjection = glm::perspective(glm::radians(verticalFovDegrees_), aspect, cameraNearPlane_, cameraFarPlane_);
                sceneProjection[1][1] *= -1.0f;
                editor.update(registry, assetManager.get(), glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp), sceneProjection,
                    renderBackend->getLitSceneTextureID(),
                    renderBackend->getGlassDepthTextureID(),
                    aspect);
                EditorOutputSettings outputSettings{};
                if (editor.consumeOutputSettings(outputSettings)) {
                    if (outputSettings.transport != config_.outputTransport) {
                        config_.outputTransport = outputSettings.transport;
                        pendingOutputTransport_ = outputSettings.transport;
                    }
                    config_.manualExposureEv = outputSettings.manualExposureEv;
                    config_.paperWhiteNits = outputSettings.paperWhiteNits;
                    config_.peakNits = outputSettings.peakNits;
                    renderBackend->setOutputSettings(outputSettings.manualExposureEv,
                        outputSettings.paperWhiteNits, outputSettings.peakNits);
                    appliedViewExposureEv_ = outputSettings.manualExposureEv;
                }
                ProjectShadowSettings shadowSettings{};
                if (editor.consumeShadowSettings(shadowSettings)) {
                    // Resolution is immutable for the active backend allocation;
                    // every remaining project policy applies on the next frame.
                    shadowSettings.directionalResolution =
                        config_.shadowSettings.directionalResolution;
                    shadowSettings.spotAtlasResolution =
                        config_.shadowSettings.spotAtlasResolution;
                    shadowSettings.pointPool256Capacity =
                        config_.shadowSettings.pointPool256Capacity;
                    shadowSettings.pointPool512Capacity =
                        config_.shadowSettings.pointPool512Capacity;
                    shadowSettings.pointPool1024Capacity =
                        config_.shadowSettings.pointPool1024Capacity;
                    config_.shadowSettings = shadowSettings;
                }
                ProjectReflectionProbeSettings probeSettings{};
                if (editor.consumeReflectionProbeSettings(probeSettings)) {
                    config_.reflectionProbeSettings = probeSettings;
                    reflectionProbeCaptureScheduler_.configure({
                        .maximumRenderedTexels = probeSettings.
                            maximumRenderedTexelsPerFrame,
                        .maximumFacesPerProbePerFrame = probeSettings.
                            maximumFacesPerProbePerFrame,
                        .maximumCapturesInFlight = probeSettings.
                            maximumCapturesInFlight,
                        .minimumRealtimeFramesBetweenCaptures = probeSettings.
                            minimumRealtimeFramesBetweenCaptures,
                    });
                    renderBackend->configureReflectionProbeCaptures(
                        probeSettings);
                }
                previewDocument = editor.renderingAssetView ? editor.assetDocuments().active() : nullptr;
                assetPreviewActive = previewDocument != nullptr;
                selectedEntity = assetPreviewActive
                    ? NULL_ENTITY : editor.getSelectedEntity();
                if (assetPreviewActive) {
                    previewModel = resolveEditorAssetPreview();
                    if (const EditorOrbitCamera* camera =
                            editor.getAssetViewerPanel().activeCamera()) {
                        renderCameraPosition = camera->position();
                        viewMatrix = camera->viewMatrix();
                        projMatrix = camera->projectionMatrix(aspect);
                        const EditorOrbitCameraState& state = camera->state();
                        renderCameraNearPlane = state.nearPlane;
                        renderCameraFarPlane = state.farPlane;
                        renderVerticalFovDegrees = state.verticalFovDegrees;
                    }
                }
                else {
                    previewModel.reset();
                }
            }
            else {
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->Pos);
                ImGui::SetNextWindowSize(viewport->Size);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBringToFrontOnFocus;
                ImGui::Begin("Benchmark Output", nullptr, flags);
                ImGui::Image(reinterpret_cast<ImTextureID>(
                    renderBackend->getLitSceneTextureID()), ImGui::GetContentRegionAvail());
                ImGui::End();
                ImGui::PopStyleVar();
                if (activeBenchmark_->id == "color_volume_transparency_v1") {
                    editor.drawColorValidationOverlay();
                }
            }
        }

        if (assetPreviewActive) {
            const auto extent = editor.getAssetViewerPanel().requestedRenderExtent;
            const auto fit = previewImageFit(aspect, extent.height ? static_cast<float>(extent.width) / extent.height : aspect);
            projMatrix[0][0] *= fit.projectionScale;
            projMatrix[1][1] *= fit.projectionScale;
        }
        const ViewTransportRecord viewTransport = makeViewTransportRecord(
            viewMatrix, projMatrix, renderCameraPosition,
            renderCameraNearPlane, renderCameraFarPlane,
            { renderExtent_.width, renderExtent_.height });
        const RenderDebugView debugView = editor.getDebugView();
        const auto previewLighting = editor.getAssetViewerPanel().activeLighting();
        renderBackend->setEnvironmentLightingSettings(assetPreviewActive
            ? previewLighting.environmentSettings() : sceneEnvironmentSettings_);
        const float viewExposure = assetPreviewActive ? previewLighting.exposureEv : config_.manualExposureEv;
        if (!appliedViewExposureEv_ || *appliedViewExposureEv_ != viewExposure) {
            renderBackend->setOutputSettings(viewExposure, config_.paperWhiteNits, config_.peakNits);
            appliedViewExposureEv_ = viewExposure;
        }
        renderBackend->setDebugView(debugView);
        renderBackend->updateCamera(viewTransport, {
            .identity = assetPreviewActive ? previewDocument->sessionSerial + 2u : 1u,
            .resetRevision = assetPreviewActive ? previewDocument->framingRevision :
                activeBenchmark_ && activeBenchmark_->sceneFactory.cameraCutEnabled &&
                applicationFrameIndex >= activeBenchmark_->sceneFactory.cameraCutFrame ? 1u : 0u,
        });
        ViewportGridOverlay gridOverlay{};
        if (!activeBenchmark_ && !assetPreviewActive && !captureFrameIndex) {
            gridOverlay = editor.viewportGridOverlay(viewMatrix, projMatrix);
        }
        renderBackend->setViewportGridOverlay(gridOverlay);

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        uint64_t requestedModelRecords = 0;
        uint64_t requestedInstances = 0;
        uint64_t requestedSubmeshes = 0;
        uint64_t requestedSourceTriangles = 0;
        uint64_t requestedSourceIndexBytes = 0;
        uint64_t requestedArenaIndexBytes = 0;
        uint64_t requestedArenaSavedIndexBytes = 0;
        uint64_t requestedUInt16Indices = 0;
        uint64_t requestedUInt32Indices = 0;
        uint64_t requestedLodFallbackChains = 0;
        uint64_t requestedLodWithheldRanges = 0;
        uint64_t requestedLodWithheldIndexBytes = 0;
        uint32_t maximumRequestedLodResidentBase = 0;
        GpuSceneVisibilityStats gpuSceneVisibilityStats{};
        uint64_t gpuSceneDeferredCandidateCount = 0;
        uint64_t gpuSceneDeferredCandidateTriangles = 0;
        uint64_t gpuSceneForwardVisibleCount = 0;
        uint64_t gpuSceneForwardVisibleTriangles = 0;
        if (!assetPreviewActive && gpuSceneFrame_) {
            classifyGpuSceneFrustum(*gpuSceneFrame_,
                makeGpuSceneFrustum(projMatrix * viewMatrix),
                GpuSceneConsumerMainOpaque |
                    GpuSceneConsumerForwardOpaque,
                gpuSceneVisibility_);
            gpuSceneVisibilityStats = gpuSceneVisibility_.stats;
        }
        uint64_t transparentCulled = 0;
        {
            CpuScope extractionScope(cpuProfiler_, "cpu.render.extract");
            const auto appendModel = [&](const ModelAsset& model,
                    const glm::mat4& worldTransform,
                    const MeshComponent* meshComponent,
                    const RenderInstanceBatchComponent* instanceBatch,
                    const CookedMaterialRuntimeBinding* forcedMaterial,
                    bool selected, SceneEntityUuid owner,
                    bool persistentOpaque) {
                if (!model.geometry.isValid()) return;
                if (instanceBatch &&
                    instanceBatch->localTransforms.size() >
                        kWeightedOitMaximumInstanceCount) {
                    throw std::length_error(
                        "Render instance batch exceeds WeightedOIT capacity");
                }
                const uint32_t instanceCount = instanceBatch
                    ? static_cast<uint32_t>(
                        instanceBatch->localTransforms.size())
                    : 1u;
                if (instanceCount == 0u) return;
                ++requestedModelRecords;
                requestedSourceIndexBytes += model.sourceIndexBytes;
                requestedArenaIndexBytes += model.arenaIndexBytes;
                requestedArenaSavedIndexBytes += model.arenaSavedIndexBytes;
                requestedUInt16Indices += model.arenaUInt16IndexCount;
                requestedUInt32Indices += model.arenaUInt32IndexCount;
                requestedLodFallbackChains += model.lodFallbackChainCount;
                requestedLodWithheldRanges +=
                    model.lodWithheldPrimitiveRangeCount;
                requestedLodWithheldIndexBytes +=
                    model.lodWithheldIndexBytes;
                maximumRequestedLodResidentBase = (std::max)(
                    maximumRequestedLodResidentBase,
                    model.lodResidentBaseLevel);
                if (instanceBatch && selected) {
                    throw std::logic_error(
                        "Render instance batch selection is not implemented");
                }
                const uint32_t firstInstanceTransform = static_cast<uint32_t>(
                    forwardInstanceTransforms_.size());
                if (instanceBatch) {
                    forwardInstanceTransforms_.reserve(
                        forwardInstanceTransforms_.size() + instanceCount);
                    const glm::mat4 identity(1.0f);
                    if (std::memcmp(&worldTransform, &identity,
                            sizeof(glm::mat4)) == 0) {
                        forwardInstanceTransforms_.insert(
                            forwardInstanceTransforms_.end(),
                            instanceBatch->localTransforms.begin(),
                            instanceBatch->localTransforms.end());
                    }
                    else {
                        const size_t first =
                            forwardInstanceTransforms_.size();
                        forwardInstanceTransforms_.resize(first +
                            instanceCount);
                        for (uint32_t index = 0u;
                            index < instanceCount; ++index) {
                            forwardInstanceTransforms_[first + index] =
                                worldTransform *
                                instanceBatch->localTransforms[index];
                        }
                    }
                }
                requestedInstances += instanceCount;
                const float distanceToCamera = glm::distance(
                    renderCameraPosition, glm::vec3(worldTransform[3]));
                for (size_t subMeshIndex = 0u;
                    subMeshIndex < model.subMeshes.size(); ++subMeshIndex) {
                    const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                    const bool previewPartSelected = assetPreviewActive && previewDocument &&
                        previewDocument->selectedPart &&
                        *previewDocument->selectedPart == (previewDocument->selectedPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (assetPreviewActive && previewDocument &&
                        previewDocument->isolateSelectedPart && !previewPartSelected) continue;
                    requestedSubmeshes += instanceCount;
                    requestedSourceTriangles +=
                        (static_cast<uint64_t>(subMesh.indexCount) / 3u) *
                        instanceCount;
                    const int materialIndex = subMesh.materialIndex;
                    if (materialIndex < 0 ||
                        static_cast<size_t>(materialIndex) >= model.materials.size()) {
                        continue;
                    }
                    const MaterialBinding* binding = forcedMaterial
                        ? &forcedMaterial->binding
                        : &model.materials[materialIndex];
                    std::optional<CookedMaterialRuntimeBinding>
                        overrideBinding;
                    AssetGuid effectiveMaterialGuid = subMesh.materialGuid;
                    CompiledTransparencyPolicy effectiveTransparency =
                        subMesh.transparency;
                    TransparencyExecutionMode effectiveExecutionMode =
                        model.transparencyExecutionMode;
                    if (forcedMaterial) {
                        effectiveMaterialGuid = forcedMaterial->materialGuid;
                        effectiveTransparency = forcedMaterial->transparency;
                        effectiveExecutionMode =
                            forcedMaterial->transparencyExecutionMode;
                    }
                    if (!forcedMaterial && meshComponent) {
                        const auto materialOverride = std::ranges::find_if(
                            meshComponent->materialOverrides,
                            [&subMesh](const MeshComponent::MaterialOverride& candidate) {
                                return candidate.sourceMaterialGuid ==
                                    subMesh.materialGuid;
                            });
                        if (materialOverride !=
                            meshComponent->materialOverrides.end()) {
                            overrideBinding =
                                assetManager->findCookedMaterialRuntime(
                                    materialOverride->materialGuid);
                            if (overrideBinding) {
                                effectiveMaterialGuid =
                                    materialOverride->materialGuid;
                                binding = &overrideBinding->binding;
                                effectiveTransparency =
                                    overrideBinding->transparency;
                                effectiveExecutionMode = overrideBinding
                                    ->transparencyExecutionMode;
                            }
                        }
                    }
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid()) {
                        continue;
                    }
                    if (persistentOpaque &&
                        binding->renderQueue != RenderQueue::Transparent) {
                        continue;
                    }
                    DrawPacket packet{};
                    packet.geometry = subMesh.geometry.isValid()
                        ? subMesh.geometry : model.geometry;
                    packet.material = binding->material;
                    packet.pipeline = binding->pipeline;
                    packet.opaqueSortKey = binding->opaqueSortKey;
                    packet.indexCount = subMesh.indexCount;
                    packet.firstIndex = subMesh.indexStart;
                    packet.worldTransform = worldTransform;
                    packet.distanceToCamera = distanceToCamera;
                    packet.isSelected = selected ? 1 : 0;
                    packet.owner = owner;
                    packet.sourcePrimitiveGuid =
                        subMesh.sourcePrimitiveGuid;
                    packet.primitiveGuid = subMesh.primitiveGuid;
                    packet.materialGuid = effectiveMaterialGuid;
                    packet.transparency = activeBenchmark_ ? effectiveTransparency :
                        withLayeredInterfaceBudget(effectiveTransparency,
                            static_cast<unsigned>(editor.layeredInterfaceOverride()));
                    packet.transparencyExecutionMode =
                        effectiveExecutionMode;
                    packet.coverage = subMesh.coverage;
                    packet.firstInstanceTransform = instanceBatch
                        ? firstInstanceTransform : UINT32_MAX;
                    packet.instanceCount = instanceCount;
                    glm::vec3 packetBoundsMin = subMesh.boundsMin;
                    glm::vec3 packetBoundsMax = subMesh.boundsMax;
                    if (instanceBatch) {
                        if (subMeshIndex >=
                                instanceBatch->subMeshBounds.size() ||
                            !instanceBatch->subMeshBounds[subMeshIndex].valid) {
                            throw std::logic_error(
                                "Render instance batch bounds do not match model");
                        }
                        packetBoundsMin = instanceBatch->subMeshBounds[
                            subMeshIndex].minimum;
                        packetBoundsMax = instanceBatch->subMeshBounds[
                            subMeshIndex].maximum;
                    }
                    const glm::vec3 packetBoundsCenter =
                        (packetBoundsMin + packetBoundsMax) * 0.5f;
                    const float packetBoundsRadius = glm::length(
                        packetBoundsMax - packetBoundsCenter);
                    const ShadowCasterSphere shadowBounds =
                        transformShadowCasterSphere(
                            instanceBatch ? packetBoundsCenter :
                                subMesh.boundsSphereCenter,
                            instanceBatch ? packetBoundsRadius :
                                subMesh.boundsSphereRadius,
                            worldTransform);
                    packet.boundsSphereCenterWorld = shadowBounds.center;
                    packet.boundsSphereRadiusWorld = shadowBounds.radius;
                    if (binding->renderQueue == RenderQueue::Transparent) {
                        if (instanceBatch && !isWeightedOitPacket(packet)) {
                            throw std::logic_error(
                                "Render instance batches currently require "
                                "classified WeightedOIT materials");
                        }
                        const bool visible = prepareTransparentWorkInterval(
                            packet, packetBoundsMin, packetBoundsMax,
                            viewMatrix, renderCameraNearPlane,
                            renderCameraFarPlane);
                        if (!visible && effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified) {
                            ++transparentCulled;
                            continue;
                        }
                        if (effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified &&
                            (packet.transparency.resolvedClass ==
                                    TransparencyClass::SortedSurface ||
                                isWeightedOitPacket(packet))) {
                            sortedSurfaceQueue.push_back(packet);
                        }
                        else {
                            transparentQueue.push_back(packet);
                        }
                    }
                    else if (binding->renderQueue == RenderQueue::ForwardOpaque) {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Forward-opaque instance batches are not implemented");
                        }
                        forwardOpaqueQueue.push_back(packet);
                    }
                    else {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Opaque instance batches are not implemented");
                        }
                        opaqueQueue.push_back(packet);
                    }
                    const auto& previewPanel = editor.getAssetViewerPanel();
                    const bool previewHovered = assetPreviewActive && previewDocument &&
                        !previewDocument->isolateSelectedPart && !previewPanel.hoveredPart.isNil() &&
                        previewPanel.hoveredPart == (previewPanel.hoveredPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (selected || previewHovered || (previewPartSelected && !previewDocument->isolateSelectedPart)) {
                        packet.selectionFeedback = static_cast<uint8_t>(
                            ((selected || previewPartSelected) ? 1u : 0u) |
                            (previewHovered ? 2u : 0u));
                        selectionQueue.push_back(packet);
                    }
                }
            };

            if (assetPreviewActive) {
                if (previewModel) {
                    appendModel(*previewModel, glm::mat4(1.0f), nullptr,
                        nullptr,
                        nullptr, false,
                        {}, false);
                }
            }
            else {
                auto* transformPool = registry.getPool<TransformComponent>();
                auto* meshPool = registry.getPool<MeshComponent>();
                auto* instanceBatchPool = registry.findPool<
                    RenderInstanceBatchComponent>();
                if (transformPool && meshPool) {
                    for (Entity entity : meshPool->entities) {
                        MeshComponent& meshComponent = meshPool->get(entity);
                        if (!meshComponent.enabled || !meshComponent.model ||
                            !transformPool->has(entity)) {
                            continue;
                        }
                        const SceneEntityUuid owner = sceneWorld_.identities()
                            .persistentId(entity).value_or(
                                SceneEntityUuid{});
                        bool persistentOpaque = false;
                        if (gpuSceneFrame_ && !owner.isNil()) {
                            const auto found = std::ranges::lower_bound(
                                gpuSceneFrame_->instanceIdentities, owner, {},
                                &GpuSceneInstanceIdentity::owner);
                            persistentOpaque = found !=
                                gpuSceneFrame_->instanceIdentities.end() &&
                                found->owner == owner;
                        }
                        appendModel(*meshComponent.model,
                            transformPool->get(entity).worldMatrix,
                            &meshComponent,
                            instanceBatchPool && instanceBatchPool->has(entity)
                                ? &instanceBatchPool->get(entity) : nullptr,
                            nullptr,
                            entity == selectedEntity, owner,
                            persistentOpaque);
                    }
                }
            }

            // M7.2 parity stage: ordinary scene opaque work is reconstructed
            // from the persistent publication. Transparent and explicit
            // fallback owners above continue to use the M6 packet path.
            if (!assetPreviewActive && gpuSceneFrame_) {
                for (uint32_t primitiveIndex = 0;
                        primitiveIndex < gpuSceneFrame_->primitives.size();
                        ++primitiveIndex) {
                    const GpuScenePrimitiveRecord& primitive =
                        gpuSceneFrame_->primitives[primitiveIndex];
                    const bool cpuVisible = primitiveIndex <
                            gpuSceneVisibility_.primitiveVisibility.size() &&
                        gpuSceneVisibility_.primitiveVisibility[primitiveIndex] != 0u;
                    const uint32_t instanceIndex = primitive.binding.x;
                    if (instanceIndex >= gpuSceneFrame_->instances.size() ||
                        instanceIndex >= gpuSceneFrame_->instanceIdentities.size() ||
                        primitive.binding.y >= gpuSceneFrame_->geometries.size() ||
                        primitiveIndex >= gpuSceneFrame_->primitiveIdentities.size()) {
                        throw std::logic_error(
                            "Published visible GPU-scene references are invalid");
                    }
                    const GpuSceneInstanceRecord& instance =
                        gpuSceneFrame_->instances[instanceIndex];
                    if (instance.references.x >= gpuSceneFrame_->transforms.size()) {
                        throw std::logic_error(
                            "Published visible GPU-scene transform is invalid");
                    }
                    const glm::mat4 worldTransform = unpackGpuSceneAffine(
                        gpuSceneFrame_->transforms[instance.references.x]);
                    const SceneEntityUuid owner = gpuSceneFrame_->
                        instanceIdentities[instanceIndex].owner;
                    const bool selected = (instance.state.z &
                        GpuSceneInstanceSelected) != 0;
                    const GpuSceneGeometryRecord& geometry =
                        gpuSceneFrame_->geometries[primitive.binding.y];
                        DrawPacket packet{};
                        packet.geometry = GeometryHandle{ geometry.storage.x };
                        packet.material = MaterialHandle{ primitive.binding.z };
                        packet.pipeline = PipelineHandle{ primitive.binding.w };
                        packet.opaqueSortKey =
                            (static_cast<uint64_t>(primitive.binding.w) << 32u) |
                            primitive.binding.z;
                        packet.indexCount = geometry.draw.y;
                        packet.firstIndex = geometry.draw.x;
                        packet.executionFlags =
                            DrawPacketGpuScenePrimitive;
                        if (cpuVisible) {
                            packet.executionFlags |=
                                DrawPacketCpuVisibilityOracle;
                        }
                        packet.firstInstanceTransform = primitiveIndex;
                        packet.worldTransform = worldTransform;
                        packet.distanceToCamera = glm::distance(
                            renderCameraPosition, glm::vec3(worldTransform[3]));
                        packet.isSelected = selected ? 1 : 0;
                        packet.owner = owner;
                        const GpuScenePrimitiveIdentity& identity =
                            gpuSceneFrame_->primitiveIdentities[primitiveIndex];
                        packet.sourcePrimitiveGuid = identity.sourcePrimitiveGuid;
                        packet.primitiveGuid = identity.primitiveGuid;
                        packet.materialGuid = identity.effectiveMaterialGuid;
                        packet.coverage = (primitive.state.y &
                            GpuScenePrimitiveAlphaMask) != 0
                            ? static_cast<uint8_t>(ModelCoverage::Masked)
                            : static_cast<uint8_t>(ModelCoverage::Opaque);
                        packet.boundsSphereCenterWorld = {
                            instance.worldBoundsSphere.x,
                            instance.worldBoundsSphere.y,
                            instance.worldBoundsSphere.z,
                        };
                        packet.boundsSphereRadiusWorld =
                            instance.worldBoundsSphere.w;
                        if ((primitive.state.w &
                                GpuSceneConsumerForwardOpaque) != 0) {
                            if (cpuVisible) {
                                forwardOpaqueQueue.push_back(packet);
                                ++gpuSceneForwardVisibleCount;
                                gpuSceneForwardVisibleTriangles +=
                                    packet.indexCount / 3u;
                            }
                        }
                        else if ((primitive.state.w &
                                GpuSceneConsumerMainOpaque) != 0) {
                            opaqueQueue.push_back(packet);
                            ++gpuSceneDeferredCandidateCount;
                            gpuSceneDeferredCandidateTriangles +=
                                packet.indexCount / 3u;
                        }
                    if (selected && cpuVisible)
                        selectionQueue.push_back(packet);
                }
            }
        }

        cpuProfiler_.recordCounter("draw.requested.opaque", opaqueQueue.size());
        cpuProfiler_.recordCounter("draw.requested.forward_opaque",
            forwardOpaqueQueue.size());
        const uint64_t requestedTransparent =
            transparentQueue.size() + sortedSurfaceQueue.size();
        cpuProfiler_.recordCounter("draw.requested.transparent",
            requestedTransparent);
        cpuProfiler_.recordCounter("draw.requested.selection", selectionQueue.size());
        cpuProfiler_.recordCounter("geometry.model_record.requested",
            requestedModelRecords);
        cpuProfiler_.recordCounter("instance.requested", requestedInstances);
        cpuProfiler_.recordCounter("submesh.requested", requestedSubmeshes);
        cpuProfiler_.recordCounter("geometry.triangle.source_requested",
            requestedSourceTriangles);
        cpuProfiler_.recordCounter("geometry.index.source_requested_bytes",
            requestedSourceIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.arena_requested_bytes",
            requestedArenaIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.arena_saved_requested_bytes",
            requestedArenaSavedIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.uint16_requested",
            requestedUInt16Indices);
        cpuProfiler_.recordCounter("geometry.index.uint32_requested",
            requestedUInt32Indices);
        cpuProfiler_.recordCounter("geometry.lod.resident_base_level.maximum_requested",
            maximumRequestedLodResidentBase);
        cpuProfiler_.recordCounter("geometry.lod.fallback_chains.requested",
            requestedLodFallbackChains);
        cpuProfiler_.recordCounter("geometry.lod.withheld_ranges.requested",
            requestedLodWithheldRanges);
        cpuProfiler_.recordCounter("geometry.lod.withheld_index_bytes.requested",
            requestedLodWithheldIndexBytes, ProfileCounterStatus::Exact,
            ProfileCounterUnit::Bytes);
        const uint64_t gpuSceneQueuedPrimitives =
            gpuSceneDeferredCandidateCount + gpuSceneForwardVisibleCount;
        const uint64_t totalQueuedPrimitives =
            opaqueQueue.size() + forwardOpaqueQueue.size();
        const uint64_t directOpaquePrimitives = totalQueuedPrimitives >=
                gpuSceneQueuedPrimitives
            ? totalQueuedPrimitives - gpuSceneQueuedPrimitives : 0;
        const uint64_t visibleOpaquePrimitives = directOpaquePrimitives +
            gpuSceneVisibilityStats.visiblePrimitives;
        const uint64_t requestedOpaquePrimitives = directOpaquePrimitives +
            gpuSceneVisibilityStats.requestedPrimitives;
        const auto submittedTriangles = [](const auto& queue) noexcept {
            uint64_t triangles = 0;
            for (const DrawPacket& packet : queue) {
                triangles += (static_cast<uint64_t>(packet.indexCount) / 3u) *
                    packet.instanceCount;
            }
            return triangles;
        };
        const uint64_t totalQueuedTriangles =
            submittedTriangles(opaqueQueue) +
            submittedTriangles(forwardOpaqueQueue);
        const uint64_t gpuSceneQueuedTriangles =
            gpuSceneDeferredCandidateTriangles +
            gpuSceneForwardVisibleTriangles;
        const uint64_t directOpaqueTriangles = totalQueuedTriangles >=
                gpuSceneQueuedTriangles
            ? totalQueuedTriangles - gpuSceneQueuedTriangles : 0;
        const uint64_t visibleOpaqueTriangles = directOpaqueTriangles +
            gpuSceneVisibilityStats.visibleTriangles;
        const uint64_t requestedOpaqueTriangles = directOpaqueTriangles +
            gpuSceneVisibilityStats.requestedTriangles;
        cpuProfiler_.recordCounter("opaque.primitive.requested",
            requestedOpaquePrimitives);
        cpuProfiler_.recordCounter("opaque.triangle.requested",
            requestedOpaqueTriangles);
        cpuProfiler_.recordCounter("opaque.primitive.visible",
            visibleOpaquePrimitives);
        cpuProfiler_.recordCounter("opaque.primitive.frustum_rejected",
            requestedOpaquePrimitives - visibleOpaquePrimitives);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.requested",
            gpuSceneVisibilityStats.requestedInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.visible",
            gpuSceneVisibilityStats.visibleInstances);
        cpuProfiler_.recordCounter(
            "gpu_scene.visibility.instance.frustum_rejected",
            gpuSceneVisibilityStats.frustumRejectedInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.fail_visible",
            gpuSceneVisibilityStats.failVisibleInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.primitive.fail_visible",
            gpuSceneVisibilityStats.failVisiblePrimitives);
        cpuProfiler_.recordCounter("opaque.primitive.occlusion_rejected", 0,
            ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("opaque.triangle.visible",
            visibleOpaqueTriangles);
        const GpuScenePublisherStats gpuSceneStats = gpuScenePublisher_
            ? gpuScenePublisher_->stats() : GpuScenePublisherStats{};
        cpuProfiler_.recordCounter("gpu_scene.instance_upload_bytes",
            gpuSceneStats.changedInstanceBytes);
        cpuProfiler_.recordCounter("gpu_scene.transform_upload_bytes",
            gpuSceneStats.changedTransformBytes);
        cpuProfiler_.recordCounter("transparent.primitive.requested",
            requestedTransparent);
        const uint64_t sortedSurfacePackets = std::ranges::count_if(
            sortedSurfaceQueue, [](const DrawPacket& packet) {
                return packet.transparency.resolvedClass ==
                    TransparencyClass::SortedSurface;
            });
        const uint64_t weightedOitPackets = std::ranges::count_if(
            sortedSurfaceQueue, [](const DrawPacket& packet) {
                return isWeightedOitPacket(packet);
            });
        cpuProfiler_.recordCounter("transparent.class.sorted_surface",
            sortedSurfacePackets);
        cpuProfiler_.recordCounter("transparent.class.weighted_oit",
            weightedOitPackets);
        cpuProfiler_.recordCounter("transparent.class.compatibility_fallback",
            transparentQueue.size());
        uint64_t thinGlassPackets = 0;
        uint64_t layeredGlassPackets = 0;
        uint64_t zeroThicknessPackets = 0;
        uint64_t metricThicknessPackets = 0;
        const auto countTransportPackets = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (packet.transparency.resolvedClass ==
                        TransparencyClass::ThinGlass) {
                    ++thinGlassPackets;
                }
                else if (packet.transparency.resolvedClass ==
                        TransparencyClass::LayeredGlass) {
                    ++layeredGlassPackets;
                }
                else {
                    continue;
                }
                if (packet.transparency.thinSheetThicknessMeters > 0.0f)
                    ++metricThicknessPackets;
                else
                    ++zeroThicknessPackets;
            }
        };
        countTransportPackets(transparentQueue);
        countTransportPackets(sortedSurfaceQueue);
        cpuProfiler_.recordCounter("transparent.class.thin_glass",
            thinGlassPackets);
        cpuProfiler_.recordCounter("transparent.class.layered_glass",
            layeredGlassPackets);
        cpuProfiler_.recordCounter("transparent.transport.zero_sheet_thickness",
            zeroThicknessPackets);
        cpuProfiler_.recordCounter("transparent.transport.metric_sheet_thickness",
            metricThicknessPackets);
        cpuProfiler_.recordCounter("transparent.work.culled", transparentCulled);
        cpuProfiler_.recordCounter("light.scene",
            lightingFrame.stats.sceneLightCount);
        cpuProfiler_.recordCounter("light.active",
            lightingFrame.stats.activeLightCount);
        cpuProfiler_.recordCounter("light.omitted",
            lightingFrame.stats.omittedLightCount);
        cpuProfiler_.recordCounter("light.capacity",
            lightingFrame.stats.capacity);
        cpuProfiler_.recordCounter("light.changed_record_bytes",
            lightingFrame.stats.changedRecordBytes);
        cpuProfiler_.recordCounter("light.changed_record_ranges",
            lightingFrame.stats.changedRangeCount);
        cpuProfiler_.recordCounter("changed.transforms", changedTransformsThisFrame_);
        cpuProfiler_.recordCounter("changed.materials", 0,
            ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("changed.lights",
            lightingFrame.stats.changedRecordCount);
        cpuProfiler_.recordCounter("changed.instances",
            gpuSceneStats.changedInstances);

        // --- 4. THE SORTING PHASE (CPU Cache Optimization) ---

        // Group opaque objects by the PSO/material identity carried by each binding.
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.opaque");
            std::sort(opaqueQueue.begin(), opaqueQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
                if (a.opaqueSortKey != b.opaqueSortKey) return a.opaqueSortKey < b.opaqueSortKey;
                if (a.geometry != b.geometry) return a.geometry < b.geometry;
                return a.firstIndex < b.firstIndex;
                });
        }
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.forward_opaque");
            std::sort(forwardOpaqueQueue.begin(), forwardOpaqueQueue.end(),
                [](const DrawPacket& a, const DrawPacket& b) {
                    if (a.opaqueSortKey != b.opaqueSortKey) {
                        return a.opaqueSortKey < b.opaqueSortKey;
                    }
                    if (a.geometry != b.geometry) return a.geometry < b.geometry;
                    return a.firstIndex < b.firstIndex;
                });
        }

        shadowCasterQueue.reserve(
            opaqueQueue.size() + forwardOpaqueQueue.size());
        const auto appendDirectShadowFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (!hasGpuScenePrimitive(packet))
                    shadowCasterQueue.push_back(packet);
            }
        };
        appendDirectShadowFallbacks(opaqueQueue);
        appendDirectShadowFallbacks(forwardOpaqueQueue);
        const auto appendDirectProbeFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (config_.forceDirectGBufferReference ||
                    config_.forceDirectProbeCaptureReference ||
                    !hasGpuScenePrimitive(packet))
                    probeCasterQueue_.push_back(packet);
            }
        };
        appendDirectProbeFallbacks(opaqueQueue);
        appendDirectProbeFallbacks(forwardOpaqueQueue);
        const ShadowCasterSubmission shadowCasters{
            .gpuScenePrimitiveIndices = shadowGpuScenePrimitiveIndices,
            .directPackets = shadowCasterQueue,
            .membershipRevision = !assetPreviewActive && gpuSceneFrame_
                ? gpuSceneFrame_->shadowConsumerMembershipRevision : 0u,
        };
        cpuProfiler_.recordCounter("shadow.casters.gpu_scene",
            shadowGpuScenePrimitiveIndices.size());
        cpuProfiler_.recordCounter("shadow.casters.direct_fallback",
            shadowCasterQueue.size());
        cpuProfiler_.recordCounter("shadow.casters.submission_bytes",
            shadowGpuScenePrimitiveIndices.size() * sizeof(uint32_t) +
                shadowCasterQueue.size() * sizeof(DrawPacket),
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        const ReflectionProbeCasterSubmission probeCasters{
            .gpuScenePrimitiveIndices = probeGpuScenePrimitiveIndices,
            .directPackets = probeCasterQueue_,
            .membershipRevision = !assetPreviewActive && gpuSceneFrame_ &&
                !config_.forceDirectGBufferReference &&
                !config_.forceDirectProbeCaptureReference
                ? gpuSceneFrame_->probeConsumerMembershipRevision : 0u,
        };
        cpuProfiler_.recordCounter("probe.capture.casters.gpu_scene",
            probeGpuScenePrimitiveIndices.size());
        cpuProfiler_.recordCounter("probe.capture.casters.direct_fallback",
            probeCasterQueue_.size());
        cpuProfiler_.recordCounter("probe.capture.casters.submission_bytes",
            probeGpuScenePrimitiveIndices.size() * sizeof(uint32_t) +
                probeCasterQueue_.size() * sizeof(DrawPacket),
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);

        // Sort transparent objects Back-to-Front to ensure perfect alpha blending and refraction
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.transparent");
            std::sort(transparentQueue.begin(), transparentQueue.end(),
                transparentCompatibilityLess);
            std::sort(sortedSurfaceQueue.begin(), sortedSurfaceQueue.end(),
                transparentWorkLess);
        }
        cpuProfiler_.recordCounter("transparent.work.invalid_bounds",
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return (packet.transparentWorkFlags &
                        TransparentWorkInvalidBoundsFallback) != 0;
                }));
        cpuProfiler_.recordCounter("transparent.work.near_clipped",
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return (packet.transparentWorkFlags &
                        TransparentWorkNearClipped) != 0;
                }));
        transparentIntervalEndpointScratch.resize(sortedSurfaceQueue.size());
        transparentIntervalNearScratch.resize(sortedSurfaceQueue.size());
        transparentIntervalFenwickScratch.resize(
            sortedSurfaceQueue.size() + 1u);
        cpuProfiler_.recordCounter("transparent.sort.ambiguous_intervals",
            sweepAmbiguousTransparentIntervals(sortedSurfaceQueue,
                transparentIntervalEndpointScratch,
                transparentIntervalNearScratch,
                transparentIntervalFenwickScratch));

        // --- 5. THE SUBMISSION PHASE (The Black Box) ---

        std::vector<DirectionalShadowFramePacket> directionalShadows;
        const std::vector<DirectionalShadowSelection> shadowSelections =
            selectDirectionalShadowLights(lightingFrame,
                config_.shadowSettings.maximumDirectionalLights);
        if (!shadowSelections.empty()) {
            const glm::mat4 inverseView = glm::inverse(viewMatrix);
            DirectionalShadowCamera shadowCamera{};
            shadowCamera.position = renderCameraPosition;
            shadowCamera.forward = glm::normalize(-glm::vec3(inverseView[2]));
            shadowCamera.up = glm::normalize(glm::vec3(inverseView[1]));
            shadowCamera.verticalFovRadians = glm::radians(
                renderVerticalFovDegrees);
            shadowCamera.aspectRatio = aspect;
            shadowCamera.nearPlane = renderCameraNearPlane;
            shadowCamera.farPlane = (std::min)(renderCameraFarPlane,
                (std::max)(config_.shadowSettings.
                    directionalMaximumDistanceMeters,
                    renderCameraNearPlane + 0.001f));
            DirectionalShadowConfig shadowConfig{
                .resolution = config_.shadowSettings.directionalResolution,
                .splitLambda = config_.shadowSettings.directionalSplitLambda,
                .guardBandFraction =
                    config_.shadowSettings.directionalGuardBandFraction,
                .depthPaddingMeters =
                    config_.shadowSettings.directionalDepthPaddingMeters,
            };
            directionalShadows.reserve(shadowSelections.size());
            uint32_t dirtyCascades = 0;
            uint32_t casterInvalidatedCascades = 0;
            uint32_t updatedCascades = 0;
            uint32_t cachedCascades = 0;
            for (uint32_t shadowIndex = 0;
                shadowIndex < shadowSelections.size(); ++shadowIndex) {
                const DirectionalShadowSelection& selection =
                    shadowSelections[shadowIndex];
                const DirectionalShadowCascadePlan plan =
                    buildDirectionalShadowCascades(shadowCamera,
                        selection.lightForward, shadowConfig);
                const uint64_t lightRevision = selection.lightSlot <
                    lightingFrame.recordRevisions.size()
                    ? lightingFrame.recordRevisions[selection.lightSlot] : 0;
                const auto casterRevisions = renderBackend->
                    getDirectionalShadowCasterRevisions(
                        shadowCasters, plan);
                const DirectionalShadowSchedule schedule =
                    directionalShadowCaches_[shadowIndex].schedule({
                        .selection = selection,
                        .plan = plan,
                        .lightRevision = lightRevision,
                        .casterRevisions = casterRevisions,
                        .pipelineRevision = 1,
                    },
                        config_.shadowSettings.maximumCascadeUpdatesPerLight);
                directionalShadows.push_back(DirectionalShadowFramePacket{
                    .selection = selection,
                    .plan = plan,
                    .shadowIndex = shadowIndex,
                    .updateMask = schedule.updateMask,
                    .sampleableMask = schedule.sampleableMask,
                    .resolution = shadowConfig.resolution,
                    .sourceAngularDiameterDegrees = config_.shadowSettings.
                        directionalSourceAngularDiameterDegrees,
                    .receiverDepthBiasTexels = config_.shadowSettings.
                        directionalReceiverDepthBiasTexels,
                    .receiverPlaneClampTexels = config_.shadowSettings.
                        directionalReceiverPlaneClampTexels,
                    .normalOffsetTexels = config_.shadowSettings.
                        directionalNormalOffsetTexels,
                    .filterProfile = effectiveShadowFilterProfile(
                        config_.shadowSettings, selection.quality),
                });
                dirtyCascades += schedule.invalidatedCount;
                casterInvalidatedCascades +=
                    schedule.casterInvalidatedCount;
                updatedCascades += std::popcount(schedule.updateMask);
                cachedCascades += schedule.cacheHitCount;
            }
            for (uint32_t shadowIndex = static_cast<uint32_t>(
                    shadowSelections.size());
                shadowIndex < directionalShadowCaches_.size(); ++shadowIndex)
                directionalShadowCaches_[shadowIndex].reset();
            activeDirectionalShadowSelection_ = shadowSelections.front();
            activeDirectionalShadowSampleableMask_ =
                directionalShadows.front().sampleableMask;
            activeDirectionalShadowOwnerCount_ = static_cast<uint32_t>(
                shadowSelections.size());
            cpuProfiler_.recordCounter("shadow.directional.requested",
                shadowSelections.size());
            cpuProfiler_.recordCounter("shadow.directional.omitted",
                shadowSelections.front().omittedShadowDirectionalLights);
            cpuProfiler_.recordCounter("shadow.directional.cascades.dirty",
                dirtyCascades);
            cpuProfiler_.recordCounter(
                "shadow.directional.cascades.caster_invalidated",
                casterInvalidatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.updated",
                updatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.cached",
                cachedCascades);
            const auto fixedMillionths = [](float value) {
                return static_cast<uint64_t>(std::llround(
                    static_cast<double>(value) * 1'000'000.0));
            };
            cpuProfiler_.recordCounter("shadow.directional.coverage_distance_m",
                fixedMillionths(shadowCamera.farPlane),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.receiver_depth_bias_texels",
                fixedMillionths(config_.shadowSettings.
                    directionalReceiverDepthBiasTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.receiver_plane_clamp_texels",
                fixedMillionths(config_.shadowSettings.
                    directionalReceiverPlaneClampTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.normal_offset_texels",
                fixedMillionths(config_.shadowSettings.
                    directionalNormalOffsetTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            constexpr std::array<const char*, 4> splitCounterNames{
                "shadow.directional.cascade0.split_far_m",
                "shadow.directional.cascade1.split_far_m",
                "shadow.directional.cascade2.split_far_m",
                "shadow.directional.cascade3.split_far_m" };
            constexpr std::array<const char*, 4> densityCounterNames{
                "shadow.directional.cascade0.world_units_per_texel_m",
                "shadow.directional.cascade1.world_units_per_texel_m",
                "shadow.directional.cascade2.world_units_per_texel_m",
                "shadow.directional.cascade3.world_units_per_texel_m" };
            const DirectionalShadowCascadePlan& diagnosticPlan =
                directionalShadows.front().plan;
            for (uint32_t cascade = 0;
                    cascade < kDirectionalShadowCascadeCount; ++cascade) {
                cpuProfiler_.recordCounter(splitCounterNames[cascade],
                    fixedMillionths(diagnosticPlan.cascades[cascade].splitFar),
                    ProfileCounterStatus::Exact,
                    ProfileCounterUnit::Millionths);
                cpuProfiler_.recordCounter(densityCounterNames[cascade],
                    fixedMillionths(diagnosticPlan.cascades[cascade].
                        worldUnitsPerTexel), ProfileCounterStatus::Exact,
                    ProfileCounterUnit::Millionths);
            }
        }
        else {
            for (DirectionalShadowCache& cache : directionalShadowCaches_)
                cache.reset();
            activeDirectionalShadowSelection_.reset();
            activeDirectionalShadowSampleableMask_ = 0;
            activeDirectionalShadowOwnerCount_ = 0;
            cpuProfiler_.recordCounter("shadow.directional.requested", 0);
        }
        renderBackend->submitDirectionalShadows(
            shadowCasters, directionalShadows);
        for (const DirectionalShadowFramePacket& shadow : directionalShadows)
            directionalShadowCaches_[shadow.shadowIndex].markRendered(
                shadow.updateMask);

        // Spot shadows share the same extracted light slots and caster revision
        // as clustered lighting. Stable atlas allocation is reconciled before
        // cache scheduling so compatible tiles remain sampleable across frames.
        const std::vector<LocalShadowRequest> localShadowRequests =
            buildLocalShadowRequests(lightingFrame, renderCameraPosition);
        const LocalShadowAllocationStats spotAllocation =
            spotShadowAtlas_.reconcile(localShadowRequests);
        spotShadowCache_.configure({
            .maximumRenderedTexels = config_.shadowSettings.
                maximumSpotRenderedTexelsPerFrame,
            .maximumCompatibleStaleFrames = config_.shadowSettings.
                maximumCompatibleSpotStaleFrames,
        });
        const uint64_t localCasterRevision =
            renderBackend->getShadowCasterRevision(shadowCasters);
        std::vector<LocalShadowCacheInput> spotCacheInputs;
        spotCacheInputs.reserve(spotShadowAtlas_.allocations().size());
        for (const SpotShadowTile& tile : spotShadowAtlas_.allocations()) {
            const auto request = std::ranges::find_if(localShadowRequests,
                [&](const LocalShadowRequest& candidate) {
                    return candidate.kind == LocalShadowKind::Spot &&
                        candidate.owner == tile.owner;
                });
            if (request == localShadowRequests.end() ||
                tile.lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[tile.lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            if (!(farPlane > nearPlane)) continue;
            const SpotShadowProjection projection = buildSpotShadowProjection(
                glm::vec3(light.positionRange),
                glm::vec3(light.directionOuterCos),
                light.directionOuterCos.w, nearPlane, farPlane);
            const std::array<uint32_t, 5> allocationIdentity{
                tile.x, tile.y, tile.size, tile.guardTexels,
                config_.shadowSettings.spotAtlasResolution };
            spotCacheInputs.push_back({
                .request = *request,
                .resolution = tile.size,
                .allocationRevision = shadowRevision(allocationIdentity),
                .lightRevision = lightingFrame.recordRevisions[tile.lightSlot],
                .casterRevision = localCasterRevision,
                .projectionRevision = shadowRevision(
                    projection.worldToShadowClip),
                .pipelineRevision = 1,
            });
        }
        const LocalShadowSchedule& spotSchedule =
            spotShadowCache_.schedule(spotCacheInputs);
        std::vector<SpotShadowFramePacket> spotShadows;
        spotShadows.reserve(spotSchedule.entries.size());
        for (const LocalShadowScheduleEntry& entry : spotSchedule.entries) {
            const auto tile = std::ranges::find_if(
                spotShadowAtlas_.allocations(),
                [&](const SpotShadowTile& candidate) {
                    return candidate.owner == entry.owner;
                });
            if (tile == spotShadowAtlas_.allocations().end() ||
                tile->lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[tile->lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            const SpotShadowProjection projection = buildSpotShadowProjection(
                glm::vec3(light.positionRange),
                glm::vec3(light.directionOuterCos),
                light.directionOuterCos.w, nearPlane, farPlane);
            const uint32_t shadowDataSlot = static_cast<uint32_t>(
                std::distance(spotShadowAtlas_.allocations().begin(), tile));
            if (shadowDataSlot >= kSpotShadowEntryCapacity) continue;
            spotShadows.push_back({
                .owner = entry.owner,
                .worldToShadowClip = projection.worldToShadowClip,
                .lightSlot = tile->lightSlot,
                .shadowDataSlot = shadowDataSlot,
                .atlasX = tile->x,
                .atlasY = tile->y,
                .tileSize = tile->size,
                .guardTexels = tile->guardTexels,
                .update = entry.update,
                .sampleable = entry.sampleable,
                .stale = entry.stale,
                .staleAgeFrames = entry.staleAgeFrames,
                .nearPlane = nearPlane,
                .farPlane = farPlane,
                .sourceRadiusMeters = light.shapeMetadata.x,
                .filterProfile = effectiveShadowFilterProfile(
                    config_.shadowSettings,
                    (std::bit_cast<uint32_t>(light.shapeMetadata.z) &
                        PackedGpuLightShadowQualityMask) >>
                        PackedGpuLightShadowQualityShift),
            });
        }
        renderBackend->submitSpotShadows(
            shadowCasters, spotShadows);
        spotShadowCache_.markScheduledRendered();
        cpuProfiler_.recordCounter("shadow.spot.requested",
            spotAllocation.requested);
        cpuProfiler_.recordCounter("shadow.spot.allocated",
            spotAllocation.allocated);
        cpuProfiler_.recordCounter("shadow.spot.omitted",
            spotAllocation.omitted);
        cpuProfiler_.recordCounter("shadow.spot.cache_hits",
            spotSchedule.stats.cacheHits);
        cpuProfiler_.recordCounter("shadow.spot.updates",
            spotSchedule.stats.updates);
        cpuProfiler_.recordCounter("shadow.spot.stale_sampled",
            spotSchedule.stats.staleSampled);
        cpuProfiler_.recordCounter("shadow.spot.unshadowed",
            spotSchedule.stats.unshadowed);
        cpuProfiler_.recordCounter("shadow.spot.rendered_texels",
            spotSchedule.stats.renderedTexels);

        // Point lights use stable tiered cube slots. Cache publication is
        // all-or-nothing across the frozen six-face orientation so lighting can
        // never sample a partially refreshed cube.
        const LocalShadowAllocationStats pointAllocation =
            pointShadowPools_.reconcile(localShadowRequests);
        pointShadowCache_.configure({
            .maximumRenderedTexels = config_.shadowSettings.
                maximumPointRenderedTexelsPerFrame,
            .maximumCompatibleStaleFrames = config_.shadowSettings.
                maximumCompatiblePointStaleFrames,
        });
        std::vector<LocalShadowCacheInput> pointCacheInputs;
        pointCacheInputs.reserve(pointShadowPools_.allocations().size());
        for (const PointShadowSlot& slot : pointShadowPools_.allocations()) {
            const auto request = std::ranges::find_if(localShadowRequests,
                [&](const LocalShadowRequest& candidate) {
                    return candidate.kind == LocalShadowKind::Point &&
                        candidate.owner == slot.owner;
                });
            if (request == localShadowRequests.end() ||
                slot.lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[slot.lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            if (!(farPlane > nearPlane)) continue;
            const auto faces = buildPointShadowFaces(
                glm::vec3(light.positionRange), nearPlane, farPlane);
            std::array<glm::mat4, 6> matrices{};
            for (uint32_t face = 0; face < matrices.size(); ++face)
                matrices[face] = faces[face].worldToShadowClip;
            const std::array<uint32_t, 2> allocationIdentity{
                slot.resolution, slot.cubeIndex };
            pointCacheInputs.push_back({
                .request = *request,
                .resolution = slot.resolution,
                .allocationRevision = shadowRevision(allocationIdentity),
                .lightRevision = lightingFrame.recordRevisions[slot.lightSlot],
                .casterRevision = localCasterRevision,
                .projectionRevision = shadowRevision(matrices),
                .pipelineRevision = 1,
            });
        }
        const LocalShadowSchedule& pointSchedule =
            pointShadowCache_.schedule(pointCacheInputs);
        std::vector<PointShadowFramePacket> pointShadows;
        pointShadows.reserve(pointSchedule.entries.size());
        for (const LocalShadowScheduleEntry& entry : pointSchedule.entries) {
            const auto slot = std::ranges::find_if(
                pointShadowPools_.allocations(),
                [&](const PointShadowSlot& candidate) {
                    return candidate.owner == entry.owner;
                });
            if (slot == pointShadowPools_.allocations().end() ||
                slot->lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[slot->lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            const auto faces = buildPointShadowFaces(
                glm::vec3(light.positionRange), nearPlane, farPlane);
            const uint32_t shadowDataSlot = static_cast<uint32_t>(
                std::distance(pointShadowPools_.allocations().begin(), slot));
            if (shadowDataSlot >= kPointShadowEntryCapacity) continue;
            PointShadowFramePacket packet{
                .owner = entry.owner,
                .lightPosition = glm::vec3(light.positionRange),
                .nearPlane = nearPlane,
                .farPlane = farPlane,
                .lightSlot = slot->lightSlot,
                .shadowDataSlot = shadowDataSlot,
                .resolution = slot->resolution,
                .cubeIndex = slot->cubeIndex,
                .update = entry.update,
                .sampleable = entry.sampleable,
                .stale = entry.stale,
                .staleAgeFrames = entry.staleAgeFrames,
                .sourceRadiusMeters = light.shapeMetadata.x,
                .filterProfile = effectiveShadowFilterProfile(
                    config_.shadowSettings,
                    (std::bit_cast<uint32_t>(light.shapeMetadata.z) &
                        PackedGpuLightShadowQualityMask) >>
                        PackedGpuLightShadowQualityShift),
            };
            for (uint32_t face = 0; face < packet.worldToShadowClip.size();
                ++face)
                packet.worldToShadowClip[face] =
                    faces[face].worldToShadowClip;
            pointShadows.push_back(packet);
        }
        renderBackend->submitPointShadows(
            shadowCasters, pointShadows);
        pointShadowCache_.markScheduledRendered();
        cpuProfiler_.recordCounter("shadow.point.requested",
            pointAllocation.requested);
        cpuProfiler_.recordCounter("shadow.point.allocated",
            pointAllocation.allocated);
        cpuProfiler_.recordCounter("shadow.point.omitted",
            pointAllocation.omitted);
        cpuProfiler_.recordCounter("shadow.point.cache_hits",
            pointSchedule.stats.cacheHits);
        cpuProfiler_.recordCounter("shadow.point.updates",
            pointSchedule.stats.updates);
        cpuProfiler_.recordCounter("shadow.point.stale_sampled",
            pointSchedule.stats.staleSampled);
        cpuProfiler_.recordCounter("shadow.point.unshadowed",
            pointSchedule.stats.unshadowed);
        cpuProfiler_.recordCounter("shadow.point.rendered_texels",
            pointSchedule.stats.renderedTexels);

        // Scene probes must never capture the isolated model or its preview sun.
        if (!assetPreviewActive) {
        std::vector<ReflectionProbeCaptureRequest> probeCaptureRequests;
        probeCaptureRequests.reserve(extractedProbes.candidates.size());
        uint64_t environmentRevision = 1469598103934665603ull;
        for (char character : activeEnvironmentCookKey_) {
            environmentRevision ^= static_cast<uint8_t>(character);
            environmentRevision *= 1099511628211ull;
        }
        if (environmentRevision == 0u) environmentRevision = 1u;
        const uint64_t lightingRevision =
            reflectionProbeLightingRevision(lightingFrame);
        for (const ReflectionProbeCandidate& candidate :
                extractedProbes.candidates) {
            if (!candidate.probe.environmentAssetGuid.isNil()) continue;
            probeCaptureRequests.push_back({
                .owner = candidate.owner,
                .updateMode = candidate.probe.updateMode,
                .position = glm::vec3(candidate.probeToWorld[3]),
                .resolution = static_cast<uint32_t>(
                    candidate.probe.captureResolution),
                .nearPlane = candidate.probe.captureNearMeters,
                .farPlane = candidate.probe.captureFarMeters,
                .priority = candidate.probe.priority,
                .captureSky = candidate.probe.captureSky,
                .settingsRevision = reflectionProbeSettingsRevision(candidate),
                .explicitRequestRevision =
                    candidate.probe.explicitCaptureRevision,
                .sceneRevision = localCasterRevision,
                .lightingRevision = lightingRevision,
                .environmentRevision = environmentRevision,
                .pipelineRevision = 1,
                .frameIndex = applicationFrameIndex,
            });
        }
        const ReflectionProbeCaptureSchedule& probeCaptureSchedule =
            reflectionProbeCaptureScheduler_.schedule(probeCaptureRequests);
        renderBackend->submitReflectionProbeCaptures(
            probeCasters, probeCaptureSchedule.entries,
            lightingFrame);
        reflectionProbeCaptureScheduler_.markScheduledFacesRendered();
        const ReflectionProbeCaptureTelemetry probeCaptureTelemetry =
            renderBackend->getReflectionProbeCaptureTelemetry();
        cpuProfiler_.recordCounter("probe.capture.faces_scheduled",
            probeCaptureSchedule.stats.facesScheduled);
        cpuProfiler_.recordCounter("probe.capture.budget_deferred",
            probeCaptureSchedule.stats.budgetDeferred);
        cpuProfiler_.recordCounter("probe.capture.capacity_deferred",
            probeCaptureSchedule.stats.capacityDeferred);
        cpuProfiler_.recordCounter("probe.capture.cadence_deferred",
            probeCaptureSchedule.stats.cadenceDeferred);
        cpuProfiler_.recordCounter("probe.capture.faces_rendered",
            probeCaptureTelemetry.facesRendered);
        cpuProfiler_.recordCounter("probe.capture.filtered",
            probeCaptureTelemetry.capturesFiltered);
        cpuProfiler_.recordCounter("probe.capture.published",
            probeCaptureTelemetry.capturesPublished);
        cpuProfiler_.recordCounter("probe.capture.staging_bytes",
            probeCaptureTelemetry.stagingLogicalBytes,
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        cpuProfiler_.recordCounter("probe.capture.published_bytes",
            probeCaptureTelemetry.publishedLogicalBytes,
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);

        }
        // Keep scene-probe resources resident, but exclude their local influence
        // from the isolated preview. Tag the active-list identity across views.
        publishedProbes.activeListRevision = publishedProbes.activeListRevision * 2u + (assetPreviewActive ? 1u : 0u);
        if (assetPreviewActive) {
            publishedProbes.activeSlots = {};
            publishedProbes.stats.activeProbeCount = 0;
        }
        // Pass 1: Opaque G-Buffer
        bool isWireframe = config_.forceWireframe ||
            (assetPreviewActive ? editor.getAssetViewerPanel().debugRenderMode == 1 : editor.currentRenderMode == 1);
        const std::span<const DrawPacket> activeSelectionQueue =
            debugView == RenderDebugView::Final
            ? std::span<const DrawPacket>(selectionQueue.data(), selectionQueue.size())
            : std::span<const DrawPacket>{};
        renderBackend->prepareDepthPyramidHistory(
            std::span<const DrawPacket>(opaqueQueue.data(), opaqueQueue.size()),
            std::span<const DrawPacket>(forwardOpaqueQueue.data(),
                forwardOpaqueQueue.size()));
        renderBackend->submitOpaqueQueue(
            std::span<const DrawPacket>(opaqueQueue.data(), opaqueQueue.size()),
            activeSelectionQueue,
            isWireframe);

        // Pass 2: Deferred Lighting 
        renderBackend->submitLightingPass(
            renderCameraPosition, viewMatrix, projMatrix,
            renderCameraNearPlane, renderCameraFarPlane, lightingFrame,
            publishedProbes);
        const LightingUploadTelemetry lightUpload =
            renderBackend->getLightingUploadTelemetry();
        cpuProfiler_.recordCounter("light.gpu_upload_bytes", lightUpload.bytes,
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        cpuProfiler_.recordCounter("light.gpu_upload_ranges", lightUpload.ranges);
        const ClusteredLightingTelemetry clusters =
            renderBackend->getClusteredLightingTelemetry();
        cpuProfiler_.recordCounter("cluster.buffer_bytes_per_frame",
            clusters.bufferBytesPerFrame,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable,
            ProfileCounterUnit::Bytes);
        cpuProfiler_.recordCounter("cluster.count", clusters.clusterCount,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.lights.active", clusters.activeLights,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.lights.directional",
            clusters.directionalLights,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.lights.local", clusters.localLights,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.references.requested",
            clusters.requestedReferences,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.references.published",
            clusters.publishedReferences,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.used", clusters.clustersUsed,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.occupancy.maximum",
            clusters.maximumOccupancy,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.fallback_lights",
            clusters.fallbackLights,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.dropped_lights",
            clusters.droppedLights,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("cluster.overflow_code", clusters.overflowCode,
            clusters.available ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable);

        // Pass 3: The AAA Translucency Pipeline (includes per-layer glass depth)
        if (validateOrdinary2Capture) {
            renderBackend->requestOrdinary2CaptureValidation(0u);
        }
        if (validateDeepLayeredCapture) {
            renderBackend->requestDeepLayeredCaptureValidation(
                0u, config_.deepLayeredCaptureQuality);
        }
        if (validateDepthPyramidCapture) {
            renderBackend->requestDepthPyramidCaptureValidation(0u);
        }
        renderBackend->submitForwardQueues(
            std::span<const DrawPacket>(
                forwardOpaqueQueue.data(), forwardOpaqueQueue.size()),
            std::span<const DrawPacket>(
                sortedSurfaceQueue.data(), sortedSurfaceQueue.size()),
            std::span<const DrawPacket>(
                transparentQueue.data(), transparentQueue.size()),
            std::span<const glm::mat4>(forwardInstanceTransforms_.data(),
                forwardInstanceTransforms_.size()));

        if (captureFrameIndex &&
            config_.capturePoint == FrameCapturePoint::SceneLinear) {
            renderBackend->captureCurrentFrame(*captureFrameIndex,
                config_.capturePoint);
            capturedApplicationFrameIndex_ = applicationFrameIndex;
        }

        // Pass 4: Final output mapping.
        renderBackend->submitOutputPass();

        if (captureFrameIndex &&
            (config_.capturePoint == FrameCapturePoint::FinalSdr ||
                config_.capturePoint == FrameCapturePoint::FinalOutput)) {
            renderBackend->captureCurrentFrame(*captureFrameIndex,
                config_.capturePoint);
            capturedApplicationFrameIndex_ = applicationFrameIndex;
        }

        // Pass 5: ImGui/Editor UI.
        renderBackend->submitUIPass();

        if (renderBackend->endFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        if (dualViews) editorViewScheduler_.rendered(renderView, EditorViewCadence::Clock::now());
        if (config_.validateOutputTransportSwitch &&
            outputTransportValidationStep_ < 3u &&
            !pendingOutputTransport_) {
            constexpr std::array sequence{
                Color::OutputTransport::ScRgb,
                Color::OutputTransport::Hdr10Pq,
                Color::OutputTransport::SdrSrgb,
            };
            pendingOutputTransport_ = sequence[outputTransportValidationStep_++];
            config_.outputTransport = *pendingOutputTransport_;
        }
        if (pendingOutputTransport_) {
            const Color::OutputTransport requested = *pendingOutputTransport_;
            pendingOutputTransport_.reset();
            const auto switchStart = std::chrono::steady_clock::now();
            renderBackend->setOutputTransport(window, requested);
            renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
            replaceOutputTransformLut(
                renderRuntimeInfo_.effectiveOutputTransportMode);
            publishOutputTransportStatus();
            const uint64_t switchNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - switchStart).count());
            std::cout << "IRIDIUM_OUTPUT_TRANSPORT_SWITCH {\"requested\":\""
                << renderRuntimeInfo_.requestedOutputTransport
                << "\",\"effective\":\""
                << renderRuntimeInfo_.effectiveOutputTransport
                << "\",\"diagnostic\":\""
                << renderRuntimeInfo_.outputTransportDiagnostic
                << "\",\"duration_ns\":" << switchNanoseconds
                << "}\n" << std::flush;
            return;
        }
        if (!activeBenchmark_ && config_.windowVisible) {
            const RenderExtent requested =
                editor.requestedRenderExtent();
            if (requested.width == 0 || requested.height == 0 ||
                (requested.width == renderExtent_.width &&
                    requested.height == renderExtent_.height)) {
                // The steady/minimized path performs no clock query, target
                // allocation, descriptor update, or graph rebuild.
                viewportExtentPolicy_.reset();
                return;
            }
            const uint64_t nowMilliseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (const auto replacement = viewportExtentPolicy_.observe(
                    requested, renderExtent_, nowMilliseconds)) {
                cpuProfiler_.recordCounter("viewport.resize.requests", 1);
                std::string diagnostic;
                const auto resizeStart = std::chrono::steady_clock::now();
                const bool resized = renderBackend->resizeSceneRenderExtent(
                    *replacement, diagnostic);
                const uint64_t resizeNanoseconds = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - resizeStart).count());
                cpuProfiler_.recordCounter(
                    "viewport.resize.cpu_ns", resizeNanoseconds);
                if (resized) {
                    renderExtent_ = renderBackend->getRenderExtent();
                    viewportExtentDiagnostic_.clear();
                    cpuProfiler_.recordCounter("viewport.resize.successes", 1);
                    cpuProfiler_.recordCounter("viewport.target.pixels",
                        static_cast<uint64_t>(renderExtent_.width) *
                            renderExtent_.height);
                    engineLog_.info("renderer.viewport",
                        "Scene target resized to " +
                            std::to_string(renderExtent_.width) + "x" +
                            std::to_string(renderExtent_.height));
                }
                else {
                    viewportExtentPolicy_.reportFailure(nowMilliseconds);
                    viewportExtentDiagnostic_ = diagnostic.empty()
                        ? "Scene target resize failed; retaining the previous target"
                        : std::move(diagnostic);
                    cpuProfiler_.recordCounter("viewport.resize.failures", 1);
                    engineLog_.warning(
                        "renderer.viewport", viewportExtentDiagnostic_);
                }
            }
        }
    }

    void Application::cleanup() {
        if (renderBackend) {
            for (TextureHandle texture :
                textureScaleProbeTextures_) {
                renderBackend->freeTexture(
                    texture);
            }
            textureScaleProbeTextures_.clear();
            for (MaterialHandle material :
                materialScaleProbeMaterials_) {
                renderBackend->freeMaterial(
                    material);
            }
            materialScaleProbeMaterials_.clear();
            if (materialScaleProbeTexture_
                    .isValid()) {
                renderBackend->freeTexture(
                    materialScaleProbeTexture_);
                materialScaleProbeTexture_ = {};
            }
        }
        if (renderBackend && residencyProbeTexture_.isValid()) {
            renderBackend->freeTexture(residencyProbeTexture_);
            residencyProbeTexture_ = {};
        }
        if (renderBackend && residencyReplacementTexture_.isValid()) {
            renderBackend->freeTexture(residencyReplacementTexture_);
            residencyReplacementTexture_ = {};
        }
        residencyProbePixels_.clear();
        editor.cleanup();
        assetThumbnailService_.reset();
        pendingThumbnailUploads_.clear();
        assetEnvironmentPreparationService_.reset();
        assetModelPreparationService_.reset();
        editorModelDdc_.reset();
        assetRuntimeService_.reset();
        assetCatalogService_.reset();
        assetCatalog_.reset();
        loadedEnvironments_.clear();
        assetManager.reset();

        environmentLighting_ = {};

        if (renderBackend && outputTransformLut.isValid()) {
            renderBackend->freeTexture(outputTransformLut);
            outputTransformLut = {};
        }

        if (renderBackend) {
            renderBackend->cleanup();
            renderBackend.reset();
        }

        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfwInitialized_) {
            glfwTerminate();
            glfwInitialized_ = false;
        }
    }

    // --- GLFW CALLBACK STUBS ---
    void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (app) app->framebufferResized = true;
    }

    void Application::mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        float xpos = static_cast<float>(xposIn);
        float ypos = static_cast<float>(yposIn);

        if (app->firstMouse) {
            app->lastX = xpos;
            app->lastY = ypos;
            app->firstMouse = false;
        }

        float xoffset = xpos - app->lastX;
        float yoffset = app->lastY - ypos;
        app->lastX = xpos;
        app->lastY = ypos;

        // Asset documents own their orbit controls through ImGui and never move
        // the active scene camera while being inspected.
        if (!app->editor.getAssetViewerPanel().isFocused &&
            (app->editor.getViewportPanel().isHovered || glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            xoffset *= app->mouseSensitivity;
            yoffset *= app->mouseSensitivity;

            // INVERSION FIX: Swap += to -= if your X or Y still feels backward!
            app->yaw += xoffset;
            app->pitch += yoffset;

            // Clamp pitch to prevent flipping upside down
            if (app->pitch > 89.0f)  app->pitch = 89.0f;
            if (app->pitch < -89.0f) app->pitch = -89.0f;

            glm::vec3 front;
            front.x = cos(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            front.y = sin(glm::radians(app->pitch));
            front.z = sin(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            app->cameraFront = glm::normalize(front);
        }
    }

    void Application::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;
        if (app->editor.getAssetViewerPanel().isFocused || !app->editor.getViewportPanel().isHovered) return;

        // Use scroll wheel to change camera fly speed
        app->cameraSpeed += static_cast<float>(yoffset) * 0.5f;
        if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
        if (app->cameraSpeed > 20.0f) app->cameraSpeed = 20.0f;
    }

    void Application::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        // Only activate camera look on Right Click
        if (button == GLFW_MOUSE_BUTTON_RIGHT &&
            ((!app->editor.getAssetViewerPanel().isFocused && app->editor.getViewportPanel().isHovered) || action == GLFW_RELEASE)) {
            if (action == GLFW_PRESS) {
                app->firstMouse = true; // Prevent violent camera snapping
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); // Hide cursor
            }
            else if (action == GLFW_RELEASE) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // Show cursor
            }
        }
    }
    void Application::processInput(GLFWwindow* window) {
        CpuScope inputScope(cpuProfiler_, "cpu.input");
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // Only move camera if Right Mouse Button is held down (standard editor behavior)
        if (editor.getAssetViewerPanel().isFocused && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (!editor.getAssetViewerPanel().isFocused && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            float velocity = cameraSpeed * deltaTime;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                cameraPos += cameraFront * velocity;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                cameraPos -= cameraFront * velocity;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                cameraPos -= glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                cameraPos += glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
        }
    }

    void Application::ProcessMeshSwaps() {
        CpuScope swapScope(cpuProfiler_, "cpu.scene.asset_swaps");
        if (!assetManager) return;

        if (assetModelPreparationService_ &&
            assetRuntimeService_) {
            for (PreparedCatalogModel& result :
                assetModelPreparationService_
                    ->takeResults()) {
                if (!result.succeeded ||
                    !result.artifact ||
                    !result.product) {
                    const std::string diagnostic =
                        result.diagnostic.empty()
                        ? "Catalog model preparation failed without a diagnostic."
                        : result.diagnostic;
                    assetRuntimeService_->reportFailure(
                        result.assetGuid,
                        diagnostic);
                    continue;
                }
                const AssetGuid assetGuid =
                    result.assetGuid;
                if (assetThumbnailService_) {
                    assetThumbnailService_->invalidate(
                        assetGuid);
                }
                const uint64_t cpuBytes =
                    result.cpuResidentBytes;
                const uint64_t gpuBytes =
                    result.gpuResidentBytes;
                if (gpuBytes >
                    EditorModelPublicationLimitBytes) {
                    const std::string diagnostic =
                        "Prepared model requires " +
                        std::to_string(
                            gpuBytes / (1024ull *
                                1024ull)) +
                        " MiB of GPU upload data, exceeding the editor's 1 GiB atomic model-publication safety cap.";
                    assetRuntimeService_->
                        reportFailure(
                            assetGuid,
                            diagnostic);
                    engineLog_.error(
                        "Asset Runtime",
                        diagnostic);
                    continue;
                }
                if (gpuBytes > EditorRuntimeUploadBudgetBytes) {
                    engineLog_.warning("Asset Runtime",
                        "Prepared model requires " + std::to_string(
                            gpuBytes / (1024ull * 1024ull)) +
                        " MiB of GPU upload data. It will publish atomically as the only asset upload this frame; the 128 MiB value is a scheduling budget, not a model-size limit.");
                }
                std::shared_ptr<CookedArtifact> artifact =
                    std::move(result.artifact);
                std::shared_ptr<CookedModelProductData> product =
                    std::move(result.product);
                (void)assetRuntimeService_->enqueuePrepared(
                    assetGuid,
                    PreparedRuntimeAsset{
                        .cookKey = artifact->cookKey,
                        .estimatedUploadBytes = gpuBytes,
                        .allowSingleOversizedUpload =
                            gpuBytes > EditorRuntimeUploadBudgetBytes,
                        .publish =
                            [this,
                                artifact = std::move(artifact),
                                product = std::move(product),
                                cpuBytes,
                                gpuBytes] {
                                try {
                                    (void)assetManager->
                                        replaceSelfContainedModelFromCookedProduct(
                                            *artifact, *product);
                                    return RuntimeAssetPublishOutcome{
                                        .succeeded = true,
                                        .cpuResidentBytes =
                                            cpuBytes,
                                        .gpuResidentBytes =
                                            gpuBytes,
                                    };
                                }
                                catch (const std::exception&
                                    exception) {
                                    return RuntimeAssetPublishOutcome{
                                        .diagnostic =
                                            exception.what(),
                                    };
                                }
                            },
                    });
            }
        }

        if (assetEnvironmentPreparationService_ &&
            assetRuntimeService_) {
            for (PreparedCatalogEnvironment& result :
                assetEnvironmentPreparationService_->takeResults()) {
                if (!result.succeeded || !result.artifact || !result.product) {
                    assetRuntimeService_->reportFailure(result.assetGuid,
                        result.diagnostic.empty()
                        ? "HDRI environment preparation failed without a diagnostic."
                        : result.diagnostic);
                    continue;
                }
                const AssetGuid assetGuid = result.assetGuid;
                if (assetThumbnailService_) {
                    assetThumbnailService_->invalidate(assetGuid);
                }
                const uint64_t gpuBytes = result.gpuResidentBytes;
                if (gpuBytes > EditorEnvironmentPublicationLimitBytes) {
                    assetRuntimeService_->reportFailure(assetGuid,
                        "Prepared HDRI environment exceeds the 640 MiB editor environment publication limit.");
                    continue;
                }
                std::shared_ptr<CookedArtifact> artifact =
                    std::move(result.artifact);
                (void)assetRuntimeService_->enqueuePrepared(assetGuid,
                    PreparedRuntimeAsset{
                        .cookKey = artifact->cookKey,
                        .estimatedUploadBytes = gpuBytes,
                        .allowSingleOversizedUpload = true,
                        .publish = [this, artifact = std::move(artifact),
                            gpuBytes] {
                            try {
                                LoadedEnvironmentAsset replacement =
                                    assetManager->loadEnvironmentFromCookedArtifact(
                                        *artifact);
                                const AssetGuid replacementGuid =
                                    replacement.assetGuid;
                                const bool replacesActiveEnvironment =
                                    replacementGuid == activeEnvironmentAssetGuid_;
                                EnvironmentLightingHandles previous{};
                                if (const auto loaded = loadedEnvironments_.find(
                                        replacementGuid);
                                    loaded != loadedEnvironments_.end()) {
                                    previous = loaded->second.lighting;
                                }
                                const bool replacesBoundEnvironment = previous.isValid() &&
                                    previous == renderBackend->getEnvironmentLighting();
                                if (replacesActiveEnvironment || replacesBoundEnvironment) try {
                                    renderBackend->setEnvironmentLighting(
                                        replacement.lighting);
                                }
                                catch (...) {
                                    assetManager->releaseEnvironment(
                                        replacement.lighting);
                                    throw;
                                }
                                if (replacesActiveEnvironment) {
                                    environmentLighting_ = replacement.lighting;
                                    activeEnvironmentSourceGuid_ =
                                        replacement.manifest.sourceTextureGuid;
                                    activeEnvironmentCookKey_ =
                                        replacement.cookKey;
                                    activeEnvironmentSourcePrimaries_ =
                                        replacement.manifest.sourcePrimaries;
                                    activeEnvironmentRadianceScale_ =
                                        replacement.manifest.sourceRadianceScale;
                                }
                                loadedEnvironments_.insert_or_assign(
                                    replacementGuid, std::move(replacement));
                                assetManager->releaseEnvironment(previous);
                                return RuntimeAssetPublishOutcome{
                                    .succeeded = true,
                                    .gpuResidentBytes = gpuBytes,
                                };
                            }
                            catch (const std::exception& exception) {
                                return RuntimeAssetPublishOutcome{
                                    .diagnostic = exception.what(),
                                };
                            }
                        },
                    });
            }
        }

        if (assetThumbnailService_) {
            for (PreparedAssetThumbnailBatch& batch :
                assetThumbnailService_
                    ->takeResults()) {
                if (!batch.diagnostic.empty()) {
                    std::cerr
                        << "Failed to prepare thumbnails for "
                        << batch.rootAssetGuid.toString()
                        << ": " << batch.diagnostic
                        << '\n';
                }
                for (AssetThumbnailPixels& thumbnail :
                    batch.thumbnails) {
                    if (thumbnail.valid() &&
                        assetThumbnailService_
                            ->isDemanded(
                                thumbnail.assetGuid)) {
                        pendingThumbnailUploads_
                            .enqueue(
                                std::move(thumbnail));
                    }
                }
            }

            constexpr uint64_t
                thumbnailUploadBudget =
                    512ull * 1024ull;
            const AssetThumbnailUploadDrain
                thumbnailDrain =
                    pendingThumbnailUploads_.drain(
                        thumbnailUploadBudget,
                        [this](AssetGuid guid) {
                            return assetThumbnailService_
                                ->isDemanded(guid);
                        },
                        [this](
                            const AssetThumbnailPixels&
                                thumbnail) {
                            try {
                                if (thumbnail.purpose ==
                                        AssetThumbnailPurpose::
                                            Detail) {
                                    assetManager->
                                        publishEditorDetailThumbnail(
                                            thumbnail.assetGuid,
                                            thumbnail.width,
                                            thumbnail.height,
                                            thumbnail.rgba8);
                                }
                                else {
                                    const std::optional<
                                        AssetGuid> evicted =
                                            assetManager->
                                            publishEditorThumbnail(
                                                thumbnail.assetGuid,
                                                thumbnail.width,
                                                thumbnail.height,
                                                thumbnail.rgba8);
                                    assetThumbnailService_
                                        ->markPublished(
                                            thumbnail.assetGuid);
                                    if (evicted) {
                                        assetThumbnailService_
                                            ->markEvicted(
                                                *evicted);
                                    }
                                }
                            }
                            catch (const std::exception&
                                exception) {
                                assetThumbnailService_
                                    ->reportFailure(
                                        thumbnail.assetGuid,
                                        exception.what());
                                std::cerr
                                    << "Failed to upload thumbnail "
                                    << thumbnail.assetGuid
                                        .toString()
                                    << ": "
                                    << exception.what()
                                    << '\n';
                            }
                        });
            thumbnailUploadsTotal_ +=
                thumbnailDrain.uploaded;
            thumbnailUploadBytesTotal_ +=
                thumbnailDrain.uploadedBytes;
            const AssetThumbnailServiceStats
                thumbnailStats =
                    assetThumbnailService_->stats();
            cpuProfiler_.recordCounter(
                "asset.thumbnail.upload_bytes",
                thumbnailDrain.uploadedBytes);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.uploaded",
                thumbnailDrain.uploaded);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.cancelled",
                thumbnailDrain.cancelled);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.deferred",
                thumbnailDrain
                    .deferredByBudget);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.pending_uploads",
                thumbnailDrain
                    .queuedAfterDrain);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.uploaded_total",
                thumbnailUploadsTotal_);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.upload_bytes_total",
                thumbnailUploadBytesTotal_);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.produced_total",
                thumbnailStats
                    .thumbnailsProduced);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.failed_total",
                thumbnailStats
                    .thumbnailsFailed);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.demanded",
                thumbnailStats
                    .demandedAssets);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.queued_roots",
                thumbnailStats
                    .queuedRoots);
        }

        auto* probePool = registry.getPool<ReflectionProbeComponent>();
        if (probePool) {
            for (Entity entity : probePool->entities) {
                ReflectionProbeComponent& probe = probePool->get(entity);
                const AssetGuid requested =
                    !probe.requestedEnvironmentAssetGuid.isNil()
                    ? probe.requestedEnvironmentAssetGuid
                    : probe.environmentAssetGuid;
                if (!probe.enabled || requested.isNil()) continue;
                if (loadedEnvironments_.contains(requested)) {
                    probe.environmentAssetGuid = requested;
                    probe.resolvedEnvironmentAssetGuid = requested;
                    probe.requestedEnvironmentAssetGuid = {};
                    probe.publicationDiagnostic.clear();
                    continue;
                }
                probe.resolvedEnvironmentAssetGuid = {};
                const auto snapshot = assetRuntimeService_
                    ? assetRuntimeService_->snapshot(requested)
                    : std::nullopt;
                if (snapshot &&
                    (snapshot->state == RuntimeAssetState::Failed ||
                     snapshot->state == RuntimeAssetState::ReadyWithError)) {
                    probe.publicationDiagnostic = snapshot->diagnostic;
                }
                const bool alreadyPending = snapshot &&
                    (snapshot->state == RuntimeAssetState::Queued ||
                     snapshot->state == RuntimeAssetState::Ready);
                if (alreadyPending ||
                    (assetCatalogService_ && assetCatalogService_->busy())) {
                    continue;
                }
                const std::vector<AssetCatalogRecord> records = assetCatalog_
                    ? assetCatalog_->recordsForGuid(requested)
                    : std::vector<AssetCatalogRecord>{};
                const auto record = std::ranges::find_if(records,
                    [](const AssetCatalogRecord& candidate) {
                        return !candidate.parentGuid &&
                            candidate.assetType == "iridium.environment" &&
                            candidate.assetRoot == "project" &&
                            candidate.status == AssetCatalogStatus::Ready;
                    });
                if (record == records.end()) {
                    probe.publicationDiagnostic =
                        "GUID is not currently present as a ready reflection-probe environment asset.";
                }
                else if (!assetEnvironmentPreparationService_) {
                    probe.publicationDiagnostic =
                        "Background reflection-probe environment preparation is unavailable.";
                }
                else if (!assetEnvironmentPreparationService_->pending(
                        requested)) {
                    (void)assetEnvironmentPreparationService_->request(*record);
                    probe.publicationDiagnostic.clear();
                }
            }
        }

        auto* skyPool = registry.getPool<SkyComponent>();
        if (skyPool && !skyPool->entities.empty()) {
            Entity activeSkyEntity = NULL_ENTITY;
            std::optional<SceneEntityUuid> activeSkyUuid;
            SkyComponent* activeSky = nullptr;
            for (Entity entity : skyPool->entities) {
                SkyComponent& candidate = skyPool->get(entity);
                if (!candidate.enabled || candidate.mode != SkyMode::Hdri) {
                    continue;
                }
                const auto candidateUuid =
                    sceneWorld_.identities().persistentId(entity);
                const bool stableTieBreak = candidateUuid
                    ? (!activeSkyUuid || *candidateUuid < *activeSkyUuid)
                    : (!activeSkyUuid &&
                        entity.index() < activeSkyEntity.index());
                if (!activeSky || candidate.priority > activeSky->priority ||
                    (candidate.priority == activeSky->priority &&
                     stableTieBreak)) {
                    activeSkyEntity = entity;
                    activeSkyUuid = candidateUuid;
                    activeSky = &candidate;
                }
            }
            if (activeSky) {
                sceneEnvironmentSettings_ = {
                    .lightingIntensity = activeSky->hdri.lightingIntensity,
                    .backgroundIntensity = activeSky->hdri.backgroundIntensity,
                    .rotationRadians = glm::radians(
                        activeSky->hdri.rotationDegrees),
                    .visibleToCamera = activeSky->hdri.visibleToCamera,
                    .affectsLighting = activeSky->hdri.affectsLighting,
                };
                renderBackend->setEnvironmentLightingSettings(sceneEnvironmentSettings_);
                const AssetGuid requested =
                    !activeSky->requestedEnvironmentAssetGuid.isNil()
                    ? activeSky->requestedEnvironmentAssetGuid
                    : activeSky->hdri.environmentAssetGuid;
                if (!requested.isNil() &&
                    requested == activeEnvironmentAssetGuid_) {
                    activeSky->hdri.environmentAssetGuid = requested;
                    activeSky->resolvedEnvironmentAssetGuid = requested;
                    activeSky->requestedEnvironmentAssetGuid = {};
                    activeSky->requestedAssetSourcePath.clear();
                    activeSky->assetResolutionDiagnostic.clear();
                }
                else if (!requested.isNil()) {
                    if (const auto loaded = loadedEnvironments_.find(requested);
                        loaded != loadedEnvironments_.end()) {
                        renderBackend->setEnvironmentLighting(
                            loaded->second.lighting);
                        environmentLighting_ = loaded->second.lighting;
                        activeEnvironmentAssetGuid_ = loaded->second.assetGuid;
                        activeEnvironmentSourceGuid_ =
                            loaded->second.manifest.sourceTextureGuid;
                        activeEnvironmentCookKey_ = loaded->second.cookKey;
                        activeEnvironmentSourcePrimaries_ =
                            loaded->second.manifest.sourcePrimaries;
                        activeEnvironmentRadianceScale_ =
                            loaded->second.manifest.sourceRadianceScale;
                        activeSky->hdri.environmentAssetGuid = requested;
                        activeSky->resolvedEnvironmentAssetGuid = requested;
                        activeSky->requestedEnvironmentAssetGuid = {};
                        activeSky->requestedAssetSourcePath.clear();
                        activeSky->assetResolutionDiagnostic.clear();
                    }
                    else {
                    const auto snapshot = assetRuntimeService_
                        ? assetRuntimeService_->snapshot(requested)
                        : std::nullopt;
                    if (snapshot &&
                        (snapshot->state == RuntimeAssetState::Failed ||
                         snapshot->state == RuntimeAssetState::ReadyWithError)) {
                        activeSky->assetResolutionDiagnostic =
                            snapshot->diagnostic;
                    }
                    const bool alreadyPending = snapshot &&
                        (snapshot->state == RuntimeAssetState::Queued ||
                         snapshot->state == RuntimeAssetState::Ready);
                    if (!alreadyPending &&
                        !(assetCatalogService_ && assetCatalogService_->busy())) {
                        const std::vector<AssetCatalogRecord> records =
                            assetCatalog_
                            ? assetCatalog_->recordsForGuid(requested)
                            : std::vector<AssetCatalogRecord>{};
                        const auto record = std::ranges::find_if(records,
                            [](const AssetCatalogRecord& candidate) {
                                return !candidate.parentGuid &&
                                    candidate.assetType ==
                                        "iridium.environment" &&
                                    candidate.assetRoot == "project" &&
                                    candidate.status ==
                                        AssetCatalogStatus::Ready;
                            });
                        if (record == records.end()) {
                            activeSky->assetResolutionDiagnostic =
                                "GUID is not currently present as a ready HDRI environment asset.";
                        }
                        else if (!assetEnvironmentPreparationService_) {
                            activeSky->assetResolutionDiagnostic =
                                "Background HDRI preparation is unavailable.";
                        }
                        else if (!assetEnvironmentPreparationService_->pending(
                                requested)) {
                            (void)assetEnvironmentPreparationService_->request(
                                *record);
                            activeSky->requestedAssetSourcePath =
                                record->sourcePath;
                            activeSky->assetResolutionDiagnostic.clear();
                        }
                    }
                    }
                }
            }
        }

        auto* meshPool = registry.getPool<MeshComponent>();
        if (!meshPool) return;

        for (Entity entity : meshPool->entities) {
            auto& meshComp = meshPool->get(entity);
            if (!meshComp.requestedAssetGuid.isNil()) {
                const AssetGuid requestedGuid =
                    meshComp.requestedAssetGuid;
                try {
                    std::shared_ptr<ModelAsset> resolved =
                        assetManager->findCookedModel(
                            requestedGuid);
                    if (!resolved && mainModel &&
                        mainModel->assetGuid ==
                            requestedGuid) {
                        resolved = mainModel;
                    }
                    if (resolved) {
                        meshComp.model =
                            std::move(resolved);
                        meshComp.assetGuid =
                            requestedGuid;
                        meshComp.requestedAssetGuid = {};
                        meshComp.requestedAssetSourcePath.clear();
                        meshComp.assetResolutionDiagnostic.clear();
                        continue;
                    }

                    std::string priorFailure;
                    if (assetRuntimeService_) {
                        const std::optional<
                            RuntimeAssetSnapshot> snapshot =
                                assetRuntimeService_->snapshot(
                                    requestedGuid);
                        if (snapshot &&
                            (snapshot->state ==
                                RuntimeAssetState::Queued ||
                             snapshot->state ==
                                RuntimeAssetState::Ready)) {
                            continue;
                        }
                        if (snapshot &&
                            (snapshot->state ==
                                RuntimeAssetState::Failed ||
                             snapshot->state ==
                                RuntimeAssetState::ReadyWithError)) {
                            priorFailure =
                                snapshot->diagnostic;
                        }
                    }

                    // A refresh replaces the rebuildable catalog atomically, but
                    // resolution can still arrive while its background discovery
                    // job is active. Keep the stable GUID pending instead of
                    // converting a transient refresh window into a permanent
                    // component failure. Failed stale runtime preparations also
                    // fall through so the current catalog path can be retried.
                    if (assetCatalogService_ &&
                        assetCatalogService_->busy()) {
                        continue;
                    }

                    const std::vector<AssetCatalogRecord> records =
                        assetCatalog_
                        ? assetCatalog_->recordsForGuid(
                            requestedGuid)
                        : std::vector<AssetCatalogRecord>{};
                    const auto record =
                        std::ranges::find_if(
                            records,
                            [](const AssetCatalogRecord&
                                candidate) {
                                return !candidate.parentGuid &&
                                    candidate.assetType ==
                                        "iridium.model" &&
                                    candidate.assetRoot ==
                                        "project" &&
                                    candidate.status ==
                                        AssetCatalogStatus::Ready;
                            });
                    if (record == records.end()) {
                        meshComp.assetResolutionDiagnostic =
                            "GUID is not currently present as a ready model asset. "
                            "The assignment will retry after catalog refresh.";
                        continue;
                    }
                    if (!assetModelPreparationService_) {
                        throw std::runtime_error(
                            "Background cooked model preparation is unavailable.");
                    }
                    if (!priorFailure.empty() &&
                        meshComp.requestedAssetSourcePath ==
                            record->sourcePath) {
                        meshComp.assetResolutionDiagnostic =
                            priorFailure;
                        continue;
                    }
                    if (!assetModelPreparationService_->pending(
                            requestedGuid)) {
                        (void)assetModelPreparationService_
                            ->request(*record);
                        meshComp.requestedAssetSourcePath =
                            record->sourcePath;
                        meshComp.assetResolutionDiagnostic.clear();
                    }
                }
                catch (const std::exception& error) {
                    const std::string diagnostic =
                        error.what();
                    if (meshComp.assetResolutionDiagnostic !=
                        diagnostic) {
                        std::cerr << "Failed to resolve model asset "
                            << requestedGuid.toString() << ": "
                            << diagnostic << '\n';
                    }
                    meshComp.assetResolutionDiagnostic =
                        diagnostic;
                }
                continue;
            }
            for (const MeshComponent::MaterialOverride&
                    materialOverride :
                meshComp.materialOverrides) {
                if (materialOverride.materialGuid.isNil() ||
                    assetManager->findCookedMaterial(
                        materialOverride.materialGuid)) {
                    continue;
                }
                const std::vector<AssetCatalogRecord>
                    materialRecords =
                        assetCatalog_
                        ? assetCatalog_->recordsForGuid(
                            materialOverride.materialGuid)
                        : std::vector<AssetCatalogRecord>{};
                const auto materialRecord =
                    std::ranges::find_if(
                        materialRecords,
                        [](const AssetCatalogRecord& record) {
                            return record.parentGuid &&
                                record.assetType ==
                                    "iridium.material" &&
                                record.status ==
                                    AssetCatalogStatus::Ready;
                        });
                if (materialRecord ==
                        materialRecords.end() ||
                    !materialRecord->parentGuid ||
                    !assetModelPreparationService_) {
                    continue;
                }
                const AssetGuid ownerGuid =
                    *materialRecord->parentGuid;
                if (std::ranges::find(
                        meshComp.requestedMaterialAssetRoots,
                        ownerGuid) !=
                    meshComp.requestedMaterialAssetRoots.end()) {
                    continue;
                }
                const std::vector<AssetCatalogRecord>
                    ownerRecords =
                        assetCatalog_->recordsForGuid(
                            ownerGuid);
                const auto owner =
                    std::ranges::find_if(
                        ownerRecords,
                        [](const AssetCatalogRecord& record) {
                            return !record.parentGuid &&
                                record.assetType ==
                                    "iridium.model" &&
                                record.status ==
                                    AssetCatalogStatus::Ready;
                        });
                if (owner != ownerRecords.end()) {
                    if (!assetModelPreparationService_
                            ->pending(ownerGuid)) {
                        (void)assetModelPreparationService_
                            ->request(*owner);
                    }
                    meshComp.requestedMaterialAssetRoots
                        .push_back(ownerGuid);
                }
            }
        }
    }

    std::shared_ptr<ModelAsset> Application::resolveEditorAssetPreview() {
        const EditorAssetDocument* document =
            editor.assetDocuments().active();
        if (!document || !assetManager) {
            framedPreviewDocumentGuid_ = {};
            framedPreviewCookKey_.clear();
            return {};
        }

        const AssetGuid presentationGuid = document->presentationAssetGuid;
        std::shared_ptr<ModelAsset> model =
            assetManager->findMaterialPreview(document->assetGuid);
        if (!model) model = assetManager->findCookedModel(presentationGuid);
        if (!model && mainModel && mainModel->assetGuid == presentationGuid) {
            model = mainModel;
        }
        if (model) {
            if (assetRuntimeService_) {
                assetRuntimeService_->touch(
                    presentationGuid, measuredFrameCount_ + 1);
            }
            if (framedPreviewDocumentGuid_ != document->assetGuid ||
                framedPreviewSession_ != document->sessionSerial ||
                framedPreviewCookKey_ != model->artifactCookKey ||
                framedPreviewRevision_ != document->framingRevision) {
                glm::vec3 minimum(
                    (std::numeric_limits<float>::max)());
                glm::vec3 maximum(
                    (std::numeric_limits<float>::lowest)());
                bool hasBounds = false;
                for (const SubMesh& subMesh : model->subMeshes) {
                    if (document->isolateSelectedPart && document->selectedPart &&
                        *document->selectedPart != (document->selectedPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid)) continue;
                    minimum = glm::min(minimum, subMesh.boundsMin);
                    maximum = glm::max(maximum, subMesh.boundsMax);
                    hasBounds = true;
                }
                if (!hasBounds) {
                    minimum = glm::vec3(-1.0f);
                    maximum = glm::vec3(1.0f);
                }
                const float aspect = renderExtent_.height != 0
                    ? static_cast<float>(renderExtent_.width) /
                        static_cast<float>(renderExtent_.height)
                    : 1.0f;
                editor.getAssetViewerPanel().frameActiveBounds(
                    minimum, maximum, aspect);
                framedPreviewDocumentGuid_ = document->assetGuid;
                framedPreviewCookKey_ = model->artifactCookKey;
                framedPreviewRevision_ = document->framingRevision;
                framedPreviewSession_ = document->sessionSerial;
            }
            return model;
        }

        if (!assetModelPreparationService_ || !assetCatalog_) return {};
        if (assetModelPreparationService_->pending(presentationGuid)) return {};
        if (assetRuntimeService_) {
            const auto snapshot = assetRuntimeService_->snapshot(presentationGuid);
            if (snapshot &&
                (snapshot->state == RuntimeAssetState::Queued ||
                 snapshot->state == RuntimeAssetState::Failed)) {
                return {};
            }
        }
        const std::vector<AssetCatalogRecord> records =
            assetCatalog_->recordsForGuid(presentationGuid);
        const auto record = std::ranges::find_if(records,
            [](const AssetCatalogRecord& candidate) {
                return !candidate.parentGuid &&
                    candidate.assetType == "iridium.model" &&
                    candidate.assetRoot == "project" &&
                    candidate.status == AssetCatalogStatus::Ready;
            });
        if (record != records.end()) {
            try {
                (void)assetModelPreparationService_->request(*record);
            }
            catch (const std::exception& exception) {
                if (assetRuntimeService_) {
                    assetRuntimeService_->reportFailure(
                        presentationGuid, exception.what());
                }
            }
        }
        return {};
    }

    void Application::configureCookedModelHotReload() {
        if (activeCookedModelArtifact_.empty() ||
            !mainModel ||
            mainModel->assetGuid.isNil() ||
            !assetRuntimeService_) {
            return;
        }
        const CookedArtifactBlob baselineBlob =
            readCookedArtifactBlobFile(
                activeCookedModelArtifact_);
        const CookedArtifactReadResult baselineArtifact =
            readCookedArtifact(
                baselineBlob.bytes,
                baselineBlob.artifactHash);
        if (!baselineArtifact.valid()) {
            throw std::runtime_error(
                "Cooked hot-reload baseline container is invalid.");
        }
        const CookedModelReadResult baselineModel =
            readCookedModelProduct(
                *baselineArtifact.artifact);
        if (!baselineModel.valid()) {
            throw std::runtime_error(
                "Cooked hot-reload baseline model is invalid.");
        }
        if (baselineArtifact.artifact->assetGuid !=
            mainModel->assetGuid) {
            throw std::runtime_error(
                "Cooked hot-reload baseline GUID does not match the loaded model.");
        }

        const auto residentBytes =
            [](const CookedModelProductData& product) {
                uint64_t gpuBytes =
                    product.vertices.size() *
                        sizeof(Vertex) +
                    product.indices.size() *
                        sizeof(uint32_t) +
                    product.materials.size() *
                        sizeof(PackedGpuMaterial);
                for (const CookedModelTextureView& view :
                    product.textureViews) {
                    gpuBytes += view.payload.size();
                }
                const uint64_t cpuBytes =
                    sizeof(ModelAsset) +
                    product.manifest.primitives.size() *
                        sizeof(SubMesh) +
                    product.materials.size() *
                        sizeof(MaterialBinding);
                return std::pair{
                    cpuBytes, gpuBytes,
                };
            };
        const auto [baselineCpuBytes,
            baselineGpuBytes] =
                residentBytes(
                    *baselineModel.data);
        if (baselineGpuBytes > EditorModelPublicationLimitBytes) {
            throw std::runtime_error(
                "Cooked model hot-reload baseline exceeds the 1 GiB atomic model-publication safety cap.");
        }
        const AssetGuid expectedGuid =
            mainModel->assetGuid;
        const std::filesystem::path artifactPath =
            activeCookedModelArtifact_;
        std::map<std::filesystem::path,
            std::string> watchedSources;
        watchedSources.emplace(
            artifactPath,
            baselineBlob.artifactHash);
        std::shared_ptr<ModelSourceReimportContext>
            sourceContext;
        const std::filesystem::path assetRoot =
            std::filesystem::path(
                PROJECT_ROOT_DIR) / "assets";
        const AssetDiscoveryResult discovery =
            discoverAssetRoots(std::array{
                AssetRoot{ "project", assetRoot },
            });
        const auto sourceRecord =
            std::ranges::find_if(
                discovery.records,
                [expectedGuid](
                    const AssetCatalogRecord& record) {
                    return record.guid ==
                            expectedGuid &&
                        !record.parentGuid &&
                        record.status ==
                            AssetCatalogStatus::Ready;
                });
        if (sourceRecord !=
            discovery.records.end()) {
            sourceContext =
                std::make_shared<
                    ModelSourceReimportContext>();
            sourceContext->assetRoot =
                assetRoot;
            sourceContext->sourceRelativePath =
                sourceRecord->sourcePath;
            sourceContext->metadataPath =
                assetRoot /
                    sourceRecord->metadataPath;
            sourceContext->target =
                baselineArtifact.artifact->target;
            sourceContext->cache =
                std::make_shared<
                    LocalDerivedDataCache>(
                    std::filesystem::path(
                        PROJECT_ROOT_DIR) /
                    "out" / "m3.5" /
                    "editor-live-ddc");
            sourceContext->importers
                .registerImporter(
                    std::make_shared<
                        TextFixtureImporter>());
            sourceContext->importers
                .registerImporter(
                    std::make_shared<
                        TextureImporter>());
            registerGltfModelImporters(sourceContext->importers);
            const std::filesystem::path
                sourcePath =
                    assetRoot /
                    sourceContext
                        ->sourceRelativePath;
            watchedSources[sourcePath] =
                sha256File(sourcePath);
            watchedSources[
                sourceContext->metadataPath] =
                    sha256File(
                        sourceContext
                            ->metadataPath);
            for (const AssetDependency& dependency :
                baselineArtifact.artifact
                    ->dependencies) {
                if (dependency.type ==
                        AssetDependencyType::SourceFile &&
                    !dependency.location.empty() &&
                    !dependency.contentHash.empty()) {
                    watchedSources[
                        assetRoot /
                            dependency.location] =
                                dependency
                                    .contentHash;
                }
            }
        }
        std::vector<TrackedSourceFile>
            trackedSources;
        trackedSources.reserve(
            watchedSources.size());
        for (auto& [path, hash] :
            watchedSources) {
            trackedSources.push_back({
                path, std::move(hash),
            });
        }
        assetRuntimeService_->track({
            .assetGuid = expectedGuid,
            .sources =
                std::move(trackedSources),
            .dependencies =
                baselineArtifact.artifact
                    ->dependencies,
            .prepare =
                [this, artifactPath,
                    expectedGuid, residentBytes,
                    sourceContext](
                    const AssetReimportCause&
                        cause,
                    std::stop_token stopToken) {
                    if (stopToken.stop_requested()) {
                        throw std::runtime_error(
                            "Cooked model reimport cancelled.");
                    }
                    const bool sourceChanged =
                        sourceContext &&
                        std::ranges::any_of(
                            cause.changedSources,
                            [&artifactPath](
                                const SourceContentChange&
                                    change) {
                                return change.sourcePath !=
                                    artifactPath;
                            });
                    CookedArtifactBlob blob;
                    if (sourceChanged) {
                        const AssetMetadataReadResult
                            metadata =
                                readAssetMetadata(
                                    sourceContext
                                        ->metadataPath);
                        if (!metadata.metadata ||
                            metadata.hasErrors() ||
                            metadata.metadata
                                ->assetGuid !=
                                    expectedGuid) {
                            throw std::runtime_error(
                                "Source reimport metadata is invalid or has the wrong GUID.");
                        }
                        auto prepared =
                            std::make_shared<
                                PreparedAssetCook>(
                            prepareAssetCook(
                                sourceContext
                                    ->importers,
                                sourceContext
                                    ->assetRoot,
                                sourceContext
                                    ->sourceRelativePath,
                                *metadata.metadata,
                                sourceContext
                                    ->target,
                                "m3.2-framework-v3",
                                stopToken));
                        if (!prepared->valid()) {
                            throw std::runtime_error(
                                cookFailureMessage(
                                    "Source reimport preparation failed",
                                    prepared
                                        ->diagnostics));
                        }
                        const auto cookProgressStart =
                            std::chrono::steady_clock::now();
                        prepared->context.progress =
                            [this,
                                sourcePath = sourceContext
                                    ->sourceRelativePath
                                    .generic_string(),
                                cookProgressStart](
                                const AssetCookContext::Progress&
                                    progress) {
                                const auto elapsedMilliseconds =
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() -
                                        cookProgressStart).count();
                                std::string message = "[" +
                                    progress.stage + "] ";
                                if (progress.total != 0) {
                                    message += std::to_string(
                                        progress.completed) + "/" +
                                        std::to_string(progress.total) + " ";
                                }
                                message += progress.detail + " (" +
                                    std::to_string(elapsedMilliseconds) +
                                    " ms): " + sourcePath;
                                engineLog_.info(
                                    "Asset Cook", std::move(message));
                            };
                        DdcRequestResult cooked =
                            requestPreparedCook(
                                *sourceContext
                                    ->cache,
                                prepared,
                                stopToken).get();
                        if ((cooked.status !=
                                DdcRequestStatus::Built &&
                             cooked.status !=
                                DdcRequestStatus::CacheHit) ||
                            !cooked.blob) {
                            throw std::runtime_error(
                                cookFailureMessage(
                                    "Source reimport cook failed",
                                    cooked
                                        .diagnostics));
                        }
                        (void)storePreparedCookReceipt(
                            *sourceContext->cache,
                            sourceContext
                                ->sourceRelativePath,
                            *prepared);
                        blob =
                            std::move(*cooked.blob);
                    } else {
                        blob =
                            readCookedArtifactBlobFile(
                                artifactPath);
                    }
                    CookedArtifactReadResult decoded =
                        readCookedArtifact(
                            blob.bytes,
                            blob.artifactHash);
                    if (!decoded.valid() ||
                        decoded.artifact->assetGuid !=
                            expectedGuid) {
                        throw std::runtime_error(
                            "Cooked model replacement container or GUID is invalid.");
                    }
                    CookedModelReadResult model =
                        readCookedModelProduct(
                            *decoded.artifact);
                    if (!model.valid()) {
                        throw std::runtime_error(
                            "Cooked model replacement product is invalid.");
                    }
                    if (stopToken.stop_requested()) {
                        throw std::runtime_error(
                            "Cooked model reimport cancelled.");
                    }
                    const auto [cpuBytes, gpuBytes] =
                        residentBytes(*model.data);
                    if (gpuBytes > EditorModelPublicationLimitBytes) {
                        throw std::runtime_error(
                            "Cooked model replacement exceeds the 1 GiB atomic model-publication safety cap.");
                    }
                    const std::string cookKey =
                        decoded.artifact->cookKey;
                    CookedArtifact artifact =
                        std::move(*decoded.artifact);
                    CookedModelProductData product =
                        std::move(*model.data);
                    return PreparedRuntimeAsset{
                        .cookKey = cookKey,
                        .estimatedUploadBytes =
                            gpuBytes,
                        .allowSingleOversizedUpload =
                            gpuBytes > EditorRuntimeUploadBudgetBytes,
                        .publish =
                            [this,
                                artifact =
                                    std::move(artifact),
                                product =
                                    std::move(product),
                                cpuBytes,
                                gpuBytes]() mutable {
                                try {
                                    mainModel =
                                        assetManager->
                                            replaceSelfContainedModelFromCookedProduct(
                                                artifact,
                                                product);
                                    return RuntimeAssetPublishOutcome{
                                        .succeeded = true,
                                        .cpuResidentBytes =
                                            cpuBytes,
                                        .gpuResidentBytes =
                                            gpuBytes,
                                    };
                                } catch (const std::exception&
                                    exception) {
                                    return RuntimeAssetPublishOutcome{
                                        .diagnostic =
                                            exception.what(),
                                    };
                                }
                            },
                    };
                },
            .pinned = true,
        });
        assetRuntimeService_->adoptPublished(
            expectedGuid,
            baselineArtifact.artifact->cookKey,
            baselineCpuBytes,
            baselineGpuBytes);
    }

    void Application::configureCookedEnvironmentHotReload() {
        if (activeCookedEnvironmentArtifact_.empty() ||
            activeEnvironmentAssetGuid_.isNil() ||
            !assetRuntimeService_) {
            return;
        }
        const CookedArtifactBlob baselineBlob =
            readCookedArtifactBlobFile(activeCookedEnvironmentArtifact_);
        const CookedArtifactReadResult baselineArtifact =
            readCookedArtifact(baselineBlob.bytes, baselineBlob.artifactHash);
        if (!baselineArtifact.valid() ||
            baselineArtifact.artifact->assetGuid !=
                activeEnvironmentAssetGuid_) {
            throw std::runtime_error(
                "Cooked environment hot-reload baseline container is invalid.");
        }
        const CookedEnvironmentReadResult baselineEnvironment =
            readCookedEnvironmentProduct(*baselineArtifact.artifact);
        if (!baselineEnvironment.valid()) {
            throw std::runtime_error(
                "Cooked environment hot-reload baseline product is invalid.");
        }
        const auto residentBytes = [](const CookedEnvironmentProductData& product) {
            return static_cast<uint64_t>(product.radiance.size()) +
                product.irradiance.size() +
                product.prefilteredSpecular.size() +
                product.brdfLut.size();
        };
        const uint64_t baselineGpuBytes =
            residentBytes(*baselineEnvironment.data);
        if (baselineGpuBytes > EditorEnvironmentPublicationLimitBytes)
            throw std::runtime_error(
                "Cooked environment hot-reload baseline exceeds the 640 MiB editor environment publication limit.");
        const AssetGuid expectedGuid = activeEnvironmentAssetGuid_;
        const std::filesystem::path artifactPath =
            activeCookedEnvironmentArtifact_;
        assetRuntimeService_->track({
            .assetGuid = expectedGuid,
            .sources = { TrackedSourceFile{
                artifactPath, baselineBlob.artifactHash } },
            .dependencies = baselineArtifact.artifact->dependencies,
            .prepare = [this, artifactPath, expectedGuid, residentBytes](
                const AssetReimportCause&, std::stop_token stopToken) {
                if (stopToken.stop_requested())
                    throw std::runtime_error(
                        "Cooked environment reimport cancelled.");
                CookedArtifactBlob blob =
                    readCookedArtifactBlobFile(artifactPath);
                CookedArtifactReadResult decoded =
                    readCookedArtifact(blob.bytes, blob.artifactHash);
                if (!decoded.valid() ||
                    decoded.artifact->assetGuid != expectedGuid)
                    throw std::runtime_error(
                        "Cooked environment replacement container or GUID is invalid.");
                CookedEnvironmentReadResult environment =
                    readCookedEnvironmentProduct(*decoded.artifact);
                if (!environment.valid())
                    throw std::runtime_error(
                        "Cooked environment replacement product is invalid.");
                if (stopToken.stop_requested())
                    throw std::runtime_error(
                        "Cooked environment reimport cancelled.");
                const uint64_t gpuBytes = residentBytes(*environment.data);
                if (gpuBytes > EditorEnvironmentPublicationLimitBytes)
                    throw std::runtime_error(
                        "Cooked environment replacement exceeds the 640 MiB editor environment publication limit.");
                const std::string cookKey = decoded.artifact->cookKey;
                CookedArtifact artifact = std::move(*decoded.artifact);
                return PreparedRuntimeAsset{
                    .cookKey = cookKey,
                    .estimatedUploadBytes = gpuBytes,
                    .allowSingleOversizedUpload = true,
                    .publish = [this, artifact = std::move(artifact),
                        gpuBytes]() mutable {
                        try {
                            LoadedEnvironmentAsset replacement =
                                assetManager->loadEnvironmentFromCookedArtifact(
                                    artifact);
                            try {
                                renderBackend->setEnvironmentLighting(
                                    replacement.lighting);
                            } catch (...) {
                                assetManager->releaseEnvironment(
                                    replacement.lighting);
                                throw;
                            }
                            const EnvironmentLightingHandles previous =
                                environmentLighting_;
                            environmentLighting_ = replacement.lighting;
                            activeEnvironmentAssetGuid_ = replacement.assetGuid;
                            activeEnvironmentSourceGuid_ =
                                replacement.manifest.sourceTextureGuid;
                            activeEnvironmentCookKey_ =
                                replacement.cookKey;
                            activeEnvironmentSourcePrimaries_ =
                                replacement.manifest.sourcePrimaries;
                            activeEnvironmentRadianceScale_ =
                                replacement.manifest.sourceRadianceScale;
                            loadedEnvironments_.insert_or_assign(
                                replacement.assetGuid,
                                std::move(replacement));
                            assetManager->releaseEnvironment(previous);
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                                .cpuResidentBytes = 0,
                                .gpuResidentBytes = gpuBytes,
                            };
                        } catch (const std::exception& exception) {
                            return RuntimeAssetPublishOutcome{
                                .diagnostic = exception.what(),
                            };
                        }
                    },
                };
            },
            .pinned = true,
        });
        assetRuntimeService_->adoptPublished(
            expectedGuid, baselineArtifact.artifact->cookKey,
            0, baselineGpuBytes);
    }

    void Application::recreateSwapchain() {
        if (renderBackend) {
            renderBackend->recreateSwapchain(window);
            renderExtent_ = renderBackend->getRenderExtent();
            renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
            if (renderRuntimeInfo_.effectiveOutputTransportMode !=
                    outputTransformLutTransport_) {
                replaceOutputTransformLut(
                    renderRuntimeInfo_.effectiveOutputTransportMode);
            }
            publishOutputTransportStatus();
        }
    }

    void Application::replaceOutputTransformLut(
        Color::OutputTransport effectiveTransport) {
        if (!renderBackend) return;
        if (effectiveTransport == Color::OutputTransport::Automatic) {
            throw std::logic_error(
                "The backend returned an unresolved automatic output transport.");
        }
        const bool hdrTransport = effectiveTransport !=
            Color::OutputTransport::SdrSrgb;
        const bool currentHdrTransport = outputTransformLutTransport_ !=
            Color::OutputTransport::SdrSrgb;
        // scRGB and HDR10 intentionally share the same P3-D65 1000-nit ACES
        // transform; only their post-LUT encoding/composition differs.
        if (outputTransformLut.isValid() &&
            currentHdrTransport == hdrTransport) {
            outputTransformLutTransport_ = effectiveTransport;
            return;
        }
        const Color::AcesOutputLut outputLut = Color::loadAcesOutputLut(
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "color" /
            (hdrTransport ? "aces2_p3d65_1000nit_rec2100_pq_128.irlt" :
                "aces2_rec709_100nit_srgb_128.irlt"));
        const TextureDesc outputLutDesc{
            .width = outputLut.width(),
            .height = outputLut.height(),
            .format = TextureFormat::RGBA32_SFloat,
            .usageClass = TextureUsageClass::Sampled2D,
            .sampler = {
                .minFilter = FilterMode::Nearest,
                .magFilter = FilterMode::Nearest,
                .addressU = SamplerAddressMode::ClampToEdge,
                .addressV = SamplerAddressMode::ClampToEdge,
                .addressW = SamplerAddressMode::ClampToEdge,
            },
        };
        const TextureHandle replacement = renderBackend->allocateTexture(
            outputLutDesc, std::as_bytes(std::span(outputLut.rgba32f)));
        renderBackend->setOutputTransformLut(replacement);
        if (outputTransformLut.isValid()) {
            renderBackend->freeTexture(outputTransformLut);
        }
        outputTransformLut = replacement;
        outputTransformLutTransport_ = effectiveTransport;
    }

    void Application::publishOutputTransportStatus() {
        editor.setOutputTransportStatus(
            renderRuntimeInfo_.requestedOutputTransportMode,
            renderRuntimeInfo_.effectiveOutputTransportMode,
            renderRuntimeInfo_.supportedOutputTransportModes,
            renderRuntimeInfo_.outputTransportDiagnostic);
    }

    void Application::updateBenchmarkState(uint64_t frameIndex) {
        if (!activeBenchmark_) return;
        const BenchmarkSceneFactory& factory = activeBenchmark_->sceneFactory;
        if (factory.animateInstances || factory.objectStepEnabled) {
            auto* transforms = registry.getPool<TransformComponent>();
            if (transforms != nullptr) {
                for (size_t index = 0; index < benchmarkInstances_.size(); ++index) {
                    BenchmarkInstanceState& instance = benchmarkInstances_[index];
                    if (!transforms->has(instance.entity)) continue;
                    TransformComponent& transform = transforms->get(instance.entity);
                    const glm::vec3 position = instance.basePosition +
                        evaluateBenchmarkInstanceOffset(factory, frameIndex,
                            index);
                    if (transform.position.x != position.x ||
                        transform.position.y != position.y ||
                        transform.position.z != position.z) {
                        transform.position = position;
                        transform.isDirty = true;
                    }
                }
            }
        }
        if (factory.objectVisibilityStepEnabled) {
            auto* meshes = registry.getPool<MeshComponent>();
            const size_t index = factory.objectVisibilityStepInstanceIndex;
            if (meshes != nullptr && index < benchmarkInstances_.size()) {
                const Entity entity = benchmarkInstances_[index].entity;
                if (meshes->has(entity)) {
                    MeshComponent& mesh = meshes->get(entity);
                    const bool enabled = frameIndex >=
                            factory.objectVisibilityStepFrame
                        ? factory.objectVisibilityAfterStep
                        : !factory.objectVisibilityAfterStep;
                    mesh.enabled = enabled;
                }
            }
        }

        const BenchmarkCameraPose camera = evaluateBenchmarkCamera(
            *activeBenchmark_, frameIndex);
        cameraPos = camera.position;
        cameraFront = glm::normalize(camera.target - camera.position);
    }
} // namespace Iridium
