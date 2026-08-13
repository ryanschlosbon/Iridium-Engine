#include "Application.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "scene/components/LightComponent.h"
#include "scene/components/NameComponent.h"
#include "scene/components/SkyComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/lighting/ShadowCasterCulling.h"
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
        // One atomic Cinematic 2048/2048 environment is about 512 MiB. Keep a
        // bounded margin for product metadata without silently allowing
        // arbitrary oversized model/texture publications.
        constexpr uint64_t EditorEnvironmentPublicationLimitBytes =
            640ull * 1024ull * 1024ull;
        constexpr std::string_view Aces2TransformId =
            "urn:ampas:aces:transformId:v2.0:Output.Academy.Rec709-D65_100nit_in_Rec709-D65_sRGB-Piecewise.a2.v1";
        constexpr std::string_view Aces2HdrTransformId =
            "urn:ampas:aces:transformId:v2.0:Output.Academy.P3-D65_1000nit_in_Rec2100-D65_ST2084.a2.v1";

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
            renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        }
        catch (...) {
            cleanup();
            throw;
        }
        cleanup();

        const SystemProfile systemProfile = querySystemProfile();

        if (completedCapture_) {
            CaptureArtifactMetadata captureMetadata{};
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
            captureMetadata.cacheState = config_.cacheState;
            captureMetadata.outputOperator = outputOperatorName(config_.outputOperator);
            captureMetadata.manualExposureEv = config_.manualExposureEv;
            captureMetadata.gamutMapping = gamutMappingName(config_.outputOperator);
            if (config_.outputTransport == Color::OutputTransport::SdrSrgb) {
                captureMetadata.displayProfile = "windows_sdr_rec709_srgb";
                captureMetadata.outputTransfer = "iec_61966_2_1_srgb";
                captureMetadata.paperWhiteNits = 100.0;
                captureMetadata.peakNits = 100.0;
            }
            else {
                captureMetadata.displayProfile = config_.outputTransport ==
                    Color::OutputTransport::ScRgb
                    ? "windows_scrgb_extended_srgb_linear"
                    : "windows_hdr10_rec2100_pq";
                captureMetadata.outputTransfer = config_.outputTransport ==
                    Color::OutputTransport::ScRgb ? "linear" : "st2084_pq";
                captureMetadata.paperWhiteNits = config_.paperWhiteNits;
                captureMetadata.peakNits = config_.peakNits;
            }
            captureMetadata.acesPackageVersion = "v2.0.0+2025.04.04";
            captureMetadata.acesTransformId = transformId(config_.outputOperator,
                config_.outputTransport);
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
                ? "neutral-black-fallback"
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
            metadata.gpuLightRecordsAvailable =
                renderCapabilities_.gpuLightRecords;
            metadata.maxGpuLightRecords =
                renderCapabilities_.maxGpuLightRecords;
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
                ? "neutral-black-fallback"
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
            .cpuProfiler = &cpuProfiler_,
            .enableGpuProfiling = config_.enableGpuProfiling,
            .enableTransparentPipelineStatistics =
                config_.enableTransparentPipelineStatistics,
            .validateReflectionProbeCaptureTargets =
                config_.validateReflectionProbes,
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
        startupProfile_.backendNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - backendStart).count());

        assetManager = std::make_unique<AssetManager>(renderBackend.get());
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

        const bool hdrTransport = config_.outputTransport !=
			Color::OutputTransport::SdrSrgb;
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
        outputTransformLut = renderBackend->allocateTexture(outputLutDesc,
            std::as_bytes(std::span(outputLut.rgba32f)));
        renderBackend->setOutputTransformLut(outputTransformLut);

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
            // Preserve the existing local diagnostic scene outside benchmark mode.
            const auto importStart = std::chrono::steady_clock::now();
            if (config_.cookedModelArtifact.empty()) {
                const AssetCatalogQueryPage models =
                    assetCatalog_->query({
                        .assetType =
                            std::string("iridium.model"),
                        .includeSubassets = false,
                        .limit = 10000,
                    });
                const auto defaultModel =
                    std::ranges::find_if(
                        models.records,
                        [](const AssetCatalogRecord& record) {
                            return record.sourcePath ==
                                "models/alfa_romeo/alfa_romeo.gltf";
                        });
                if (defaultModel ==
                    models.records.end()) {
                    throw std::runtime_error(
                        "Default editor model is not registered in the project asset catalog.");
                }
                startupModelGuid =
                    defaultModel->guid;
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
        Entity firstEntity = NULL_ENTITY;
        for (uint32_t z = 0; z < grid.z; ++z) {
            for (uint32_t y = 0; y < grid.y; ++y) {
                for (uint32_t x = 0; x < grid.x; ++x) {
                    const glm::vec3 position = activeBenchmark_
                        ? (glm::vec3(x, y, z) - gridCenter) * spacing
                        : glm::vec3(0.0f, -1.0f, 0.0f);
                    const Entity entity = createModelEditorEntity(
                        registry, startupModelGuid,
                        activeBenchmark_ ? "Benchmark Model" : "Alfa Romeo",
                        position);
                    if (firstEntity == NULL_ENTITY) firstEntity = entity;
                    auto& transform = registry.getComponent<TransformComponent>(entity);
                    transform.rotation = glm::vec3(0.0f);
                    transform.scale = glm::vec3(1.0f);
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
                }
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
        const uint32_t fixtureLightCount = directionalShadowFixture
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
                if (sampleCarLightingFixture) {
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
                else if (activeBenchmark_ &&
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
                if (directionalShadowFixture || spotShadowContactFixture ||
                    pointShadowContactFixture) {
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
            const Entity skyEntity = sceneWorld_.createEntity(
                SceneEntityUuid::fromUuidV7Fields(
                    1'775'000'400'000ull,
                    std::array<uint8_t, 10>{ 0x49, 0x52, 0x49, 0x44, 0x49,
                        0x55, 0x4d, 0x53, 0x4b, 0x59 }));
            registry.addComponent<NameComponent>(skyEntity, "Sky");
            registry.addComponent<RelationshipComponent>(skyEntity)
                .siblingOrder = static_cast<int32_t>(generatedLightCount + 1u);
            auto& sky = registry.addComponent<SkyComponent>(skyEntity);
            sky.mode = SkyMode::Hdri;
            const auto defaultEnvironment = AssetGuid::parse(
                "019c5d3a-1234-7abc-8def-1029384756aa");
            if (!activeEnvironmentAssetGuid_.isNil()) {
                sky.hdri.environmentAssetGuid = activeEnvironmentAssetGuid_;
                sky.resolvedEnvironmentAssetGuid =
                    activeEnvironmentAssetGuid_;
            }
            else if (defaultEnvironment) {
                sky.hdri.environmentAssetGuid = *defaultEnvironment;
                sky.requestedEnvironmentAssetGuid = *defaultEnvironment;
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

            const Entity captureProbeEntity = sceneWorld_.createEntity(
                SceneEntityUuid::fromUuidV7Fields(
                    1'775'000'410'001ull,
                    std::array<uint8_t, 10>{ 0x49, 0x52, 0x49, 0x44, 0x49,
                        0x55, 0x4d, 0x43, 0x41, 0x50 }));
            registry.addComponent<NameComponent>(captureProbeEntity,
                "Runtime Reflection Capture Validation");
            registry.addComponent<TransformComponent>(captureProbeEntity);
            registry.addComponent<RelationshipComponent>(captureProbeEntity)
                .siblingOrder = static_cast<int32_t>(
                    generatedLightCount + 3u);
            auto& captureProbe =
                registry.addComponent<ReflectionProbeComponent>(
                    captureProbeEntity);
            captureProbe.shape = ReflectionProbeShape::Sphere;
            captureProbe.sphereRadiusMeters = 1'000.0f;
            captureProbe.blendDistanceMeters = 0.0f;
            captureProbe.parallaxMode = ReflectionProbeParallaxMode::None;
            captureProbe.priority = 1;
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
                    std::string title = "Iridium Engine - FPS: " + std::to_string(frameCount) +
                        " (" + std::to_string(1000.0f / frameCount).substr(0, 4) + " ms/frame)";
                    glfwSetWindowTitle(window, title.c_str());

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
                    changedTransformsThisFrame_ = transformSystem.update(registry);
                }

                // 4. The frame acquisition must precede UI construction so the
                // viewport texture IDs correspond to the image acquired this frame.
                const std::optional<uint64_t> captureFrameIndex =
                    isMeasuredFrame && config_.captureFrameIndex == measuredFrameCount_
                    ? config_.captureFrameIndex
                    : std::nullopt;
                drawFrame(captureFrameIndex, applicationFrameCount);
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

    void Application::drawFrame(std::optional<uint64_t> captureFrameIndex,
        uint64_t applicationFrameIndex) {
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
            lightingFrame = lightExtractor_.extract(sceneWorld_);
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
        // If the window was resized, OR acquire requests a swapchain rebuild:
        if (framebufferResized || renderBackend->beginFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        updateTextureResidencyChurn(applicationFrameIndex);

        // --- 1. CLEAR THE QUEUES ---
        opaqueQueue.clear();
        forwardOpaqueQueue.clear();
        transparentQueue.clear();
        selectionQueue.clear();
        shadowCasterQueue.clear();

        const EditorAssetDocument* previewDocument = !activeBenchmark_
            ? editor.assetDocuments().active() : nullptr;
        std::shared_ptr<ModelAsset> previewModel = previewDocument
            ? resolveEditorAssetPreview() : std::shared_ptr<ModelAsset>{};
        bool assetPreviewActive = previewDocument != nullptr;
        Entity selectedEntity = assetPreviewActive
            ? NULL_ENTITY : editor.getSelectedEntity();

        // --- 2. GET CAMERA DATA ---
        glm::vec3 renderCameraPosition = cameraPos;
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
            }
        }

        // Build ImGui only after beginFrame selected currentImageIndex. The UI
        // descriptors are per swapchain image, so using them before acquisition
        // can sample a different target that has not yet been transitioned.
        {
            CpuScope editorScope(cpuProfiler_, "cpu.editor.build");
            renderBackend->beginUI();
            if (!activeBenchmark_) {
                editor.update(registry, assetManager.get(), viewMatrix, projMatrix,
                    renderBackend->getLitSceneTextureID(),
                    renderBackend->getGlassDepthTextureID(),
                    aspect);
                EditorOutputSettings outputSettings{};
                if (editor.consumeOutputSettings(outputSettings)) {
                    config_.manualExposureEv = outputSettings.manualExposureEv;
                    config_.paperWhiteNits = outputSettings.paperWhiteNits;
                    config_.peakNits = outputSettings.peakNits;
                    renderBackend->setOutputSettings(outputSettings.manualExposureEv,
                        outputSettings.paperWhiteNits, outputSettings.peakNits);
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
                previewDocument = editor.assetDocuments().active();
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

        renderBackend->updateCamera(viewMatrix, projMatrix);
        const RenderDebugView debugView = editor.getDebugView();
        renderBackend->setDebugView(debugView);

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        uint64_t requestedInstances = 0;
        uint64_t requestedSubmeshes = 0;
        {
            CpuScope extractionScope(cpuProfiler_, "cpu.render.extract");
            const auto appendModel = [&](const ModelAsset& model,
                    const glm::mat4& worldTransform,
                    const MeshComponent* meshComponent,
                    const MaterialBinding* forcedMaterial,
                    bool selected, SceneEntityUuid owner) {
                if (!model.geometry.isValid()) return;
                ++requestedInstances;
                const float distanceToCamera = glm::distance(
                    renderCameraPosition, glm::vec3(worldTransform[3]));
                for (const SubMesh& subMesh : model.subMeshes) {
                    ++requestedSubmeshes;
                    const int materialIndex = subMesh.materialIndex;
                    if (materialIndex < 0 ||
                        static_cast<size_t>(materialIndex) >= model.materials.size()) {
                        continue;
                    }
                    const MaterialBinding* binding = forcedMaterial
                        ? forcedMaterial : &model.materials[materialIndex];
                    std::optional<MaterialBinding> overrideBinding;
                    if (!forcedMaterial && meshComponent) {
                        const auto materialOverride = std::ranges::find_if(
                            meshComponent->materialOverrides,
                            [&subMesh](const MeshComponent::MaterialOverride& candidate) {
                                return candidate.sourceMaterialGuid ==
                                    subMesh.materialGuid;
                            });
                        if (materialOverride !=
                            meshComponent->materialOverrides.end()) {
                            overrideBinding = assetManager->findCookedMaterial(
                                materialOverride->materialGuid);
                            if (overrideBinding) binding = &*overrideBinding;
                        }
                    }
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid()) {
                        continue;
                    }
                    DrawPacket packet{};
                    packet.geometry = model.geometry;
                    packet.material = binding->material;
                    packet.pipeline = binding->pipeline;
                    packet.opaqueSortKey = binding->opaqueSortKey;
                    packet.indexCount = subMesh.indexCount;
                    packet.firstIndex = subMesh.indexStart;
                    packet.worldTransform = worldTransform;
                    packet.distanceToCamera = distanceToCamera;
                    packet.isSelected = selected ? 1 : 0;
                    packet.owner = owner;
                    const ShadowCasterSphere shadowBounds =
                        transformShadowCasterSphere(
                            subMesh.boundsSphereCenter,
                            subMesh.boundsSphereRadius, worldTransform);
                    packet.boundsSphereCenterWorld = shadowBounds.center;
                    packet.boundsSphereRadiusWorld = shadowBounds.radius;
                    if (binding->renderQueue == RenderQueue::Transparent) {
                        transparentQueue.push_back(packet);
                    }
                    else if (binding->renderQueue == RenderQueue::ForwardOpaque) {
                        forwardOpaqueQueue.push_back(packet);
                    }
                    else {
                        opaqueQueue.push_back(packet);
                    }
                    if (selected) selectionQueue.push_back(packet);
                }
            };

            if (assetPreviewActive) {
                if (previewModel) {
                    std::optional<MaterialBinding> previewMaterial;
                    if (previewDocument &&
                        previewDocument->kind == EditorAssetViewerKind::Material) {
                        previewMaterial = assetManager->findCookedMaterial(
                            previewDocument->assetGuid);
                    }
                    appendModel(*previewModel, glm::mat4(1.0f), nullptr,
                        previewMaterial ? &*previewMaterial : nullptr, false,
                        {});
                }
            }
            else {
                auto* transformPool = registry.getPool<TransformComponent>();
                auto* meshPool = registry.getPool<MeshComponent>();
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
                        appendModel(*meshComponent.model,
                            transformPool->get(entity).worldMatrix,
                            &meshComponent, nullptr,
                            entity == selectedEntity, owner);
                    }
                }
            }
        }

        cpuProfiler_.recordCounter("draw.requested.opaque", opaqueQueue.size());
        cpuProfiler_.recordCounter("draw.requested.forward_opaque",
            forwardOpaqueQueue.size());
        cpuProfiler_.recordCounter("draw.requested.transparent", transparentQueue.size());
        cpuProfiler_.recordCounter("draw.requested.selection", selectionQueue.size());
        cpuProfiler_.recordCounter("instance.requested", requestedInstances);
        cpuProfiler_.recordCounter("submesh.requested", requestedSubmeshes);
        cpuProfiler_.recordCounter("transparent.primitive.requested", transparentQueue.size());
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
        cpuProfiler_.recordCounter("changed.instances", 0,
            ProfileCounterStatus::Unavailable);

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
        shadowCasterQueue.insert(shadowCasterQueue.end(),
            opaqueQueue.begin(), opaqueQueue.end());
        shadowCasterQueue.insert(shadowCasterQueue.end(),
            forwardOpaqueQueue.begin(), forwardOpaqueQueue.end());

        // Sort transparent objects Back-to-Front to ensure perfect alpha blending and refraction
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.transparent");
            std::sort(transparentQueue.begin(), transparentQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
                if (a.distanceToCamera != b.distanceToCamera) return a.distanceToCamera > b.distanceToCamera;
                if (a.pipeline != b.pipeline) return a.pipeline < b.pipeline;
                if (a.material != b.material) return a.material < b.material;
                return a.geometry < b.geometry;
                });
        }

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
            shadowCamera.verticalFovRadians = glm::radians(verticalFovDegrees_);
            shadowCamera.aspectRatio = aspect;
            shadowCamera.nearPlane = cameraNearPlane_;
            shadowCamera.farPlane = cameraFarPlane_;
            DirectionalShadowConfig shadowConfig{
                .resolution = config_.shadowSettings.directionalResolution,
                .splitLambda = config_.shadowSettings.directionalSplitLambda,
                .guardBandFraction =
                    config_.shadowSettings.directionalGuardBandFraction,
                .depthPaddingMeters =
                    config_.shadowSettings.directionalDepthPaddingMeters,
            };
            const uint64_t casterRevision =
                renderBackend->getShadowCasterRevision(shadowCasterQueue);
            directionalShadows.reserve(shadowSelections.size());
            uint32_t dirtyCascades = 0;
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
                const DirectionalShadowSchedule schedule =
                    directionalShadowCaches_[shadowIndex].schedule({ selection,
                        plan, lightRevision, casterRevision, 1 },
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
                    .filterProfile = effectiveShadowFilterProfile(
                        config_.shadowSettings, selection.quality),
                });
                dirtyCascades += schedule.invalidatedCount;
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
            cpuProfiler_.recordCounter("shadow.directional.cascades.updated",
                updatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.cached",
                cachedCascades);
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
            std::span<const DrawPacket>(shadowCasterQueue.data(),
                shadowCasterQueue.size()),
            directionalShadows);
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
            renderBackend->getShadowCasterRevision(shadowCasterQueue);
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
            std::span<const DrawPacket>(shadowCasterQueue.data(),
                shadowCasterQueue.size()), spotShadows);
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
            std::span<const DrawPacket>(shadowCasterQueue.data(),
                shadowCasterQueue.size()), pointShadows);
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
            opaqueQueue, forwardOpaqueQueue, probeCaptureSchedule.entries,
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

        // Pass 1: Opaque G-Buffer
        bool isWireframe = config_.forceWireframe ||
            editor.currentRenderMode == 1;
        const std::span<const DrawPacket> activeSelectionQueue =
            debugView == RenderDebugView::Final
            ? std::span<const DrawPacket>(selectionQueue.data(), selectionQueue.size())
            : std::span<const DrawPacket>{};
        renderBackend->submitOpaqueQueue(
            std::span<const DrawPacket>(opaqueQueue.data(), opaqueQueue.size()),
            activeSelectionQueue,
            isWireframe);

        // Pass 2: Deferred Lighting 
        renderBackend->submitLightingPass(
            renderCameraPosition, viewMatrix, projMatrix,
            cameraNearPlane_, cameraFarPlane_, lightingFrame,
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
        renderBackend->submitForwardQueues(
            std::span<const DrawPacket>(
                forwardOpaqueQueue.data(), forwardOpaqueQueue.size()),
            std::span<const DrawPacket>(
                transparentQueue.data(), transparentQueue.size()));

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
        if (!app->editor.assetDocuments().active() &&
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
        if (app->editor.assetDocuments().active()) return;

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
            !app->editor.assetDocuments().active()) {
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
        if (!editor.assetDocuments().active() &&
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
                    EditorRuntimeUploadBudgetBytes) {
                    const std::string diagnostic =
                        "Prepared model requires " +
                        std::to_string(
                            gpuBytes / (1024ull *
                                1024ull)) +
                        " MiB of GPU upload data, exceeding the editor's 128 MiB single-publication budget.";
                    assetRuntimeService_->
                        reportFailure(
                            assetGuid,
                            diagnostic);
                    engineLog_.error(
                        "Asset Runtime",
                        diagnostic);
                    continue;
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
                                if (replacesActiveEnvironment) try {
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
                renderBackend->setEnvironmentLightingSettings({
                    .lightingIntensity = activeSky->hdri.lightingIntensity,
                    .backgroundIntensity = activeSky->hdri.backgroundIntensity,
                    .rotationRadians = glm::radians(
                        activeSky->hdri.rotationDegrees),
                    .visibleToCamera = activeSky->hdri.visibleToCamera,
                    .affectsLighting = activeSky->hdri.affectsLighting,
                });
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
            assetManager->findCookedModel(presentationGuid);
        if (!model && mainModel && mainModel->assetGuid == presentationGuid) {
            model = mainModel;
        }
        if (model) {
            if (assetRuntimeService_) {
                assetRuntimeService_->touch(
                    presentationGuid, measuredFrameCount_ + 1);
            }
            if (framedPreviewDocumentGuid_ != document->assetGuid ||
                framedPreviewCookKey_ != model->artifactCookKey) {
                glm::vec3 minimum(
                    (std::numeric_limits<float>::max)());
                glm::vec3 maximum(
                    (std::numeric_limits<float>::lowest)());
                bool hasBounds = false;
                for (const SubMesh& subMesh : model->subMeshes) {
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
            sourceContext->importers
                .registerImporter(
                    std::make_shared<
                        GltfModelImporter>());
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
        }
    }

    void Application::updateBenchmarkState(uint64_t frameIndex) {
        if (!activeBenchmark_) return;
        const BenchmarkSceneFactory& factory = activeBenchmark_->sceneFactory;
        if (factory.animateInstances) {
            auto* transforms = registry.getPool<TransformComponent>();
            if (transforms != nullptr) {
                for (size_t index = 0; index < benchmarkInstances_.size(); ++index) {
                    BenchmarkInstanceState& instance = benchmarkInstances_[index];
                    if (!transforms->has(instance.entity)) continue;
                    TransformComponent& transform = transforms->get(instance.entity);
                    transform.position = instance.basePosition;
                    transform.position.y += evaluateBenchmarkInstanceYOffset(
                        factory, frameIndex, index);
                    transform.isDirty = true;
                }
            }
        }

        const BenchmarkCameraPose camera = evaluateBenchmarkCamera(
            *activeBenchmark_, frameIndex);
        cameraPos = camera.position;
        cameraFront = glm::normalize(camera.target - camera.position);
    }
    void Application::selectEntityAtMouse(double mouseX, double mouseY) { /* ... */ }

} // namespace Iridium
