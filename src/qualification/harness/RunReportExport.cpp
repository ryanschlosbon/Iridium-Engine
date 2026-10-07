// Run artifacts written after cleanup (Finalize): the capture artifact with its
// metadata sidecar (IRIDIUM_CAPTURE) and the --profile-cpu-output JSONL run report.
// Both keep the pre-R2 schema; metadata comes from the Application's run snapshot
// and the harness's retained backend facts.

#include "qualification/harness/QualificationHarness.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

#include "core/BuildInfo.h"
#include "platform/SystemProfile.h"
#include "profiling/CpuProfileExport.h"
#include "profiling/CpuProfiler.h"
#include "qualification/harness/HarnessDetail.h"
#include "renderer/rhi/Mesh.h"

namespace Iridium {

    void QualificationHarness::setCaptureNaming(CaptureArtifactMetadata& metadata,
        const ApplicationConfig& config, uint64_t measuredFrameIndex) const {
        metadata.measuredFrameIndex = measuredFrameIndex;
        metadata.debugView = config.forceWireframe
            ? "wireframe"
            : std::string(renderDebugViewName(config.debugView));
        if (benchmark_) {
            metadata.fixtureId = benchmark_->id;
            metadata.fixtureRevision = benchmark_->revision;
            metadata.cameraId = benchmark_->camera.id;
        }
    }

    void QualificationHarness::setCaptureFrame(CaptureArtifactMetadata& metadata,
        uint64_t captureId) const {
        const auto record = std::ranges::find(captureFrames_, captureId,
            &CaptureFrameRecord::captureId);
        metadata.measuredFrameIndex = captureId;
        metadata.applicationFrameIndex =
            record != captureFrames_.end() ? record->applicationFrameIndex : 0u;
        metadata.benchmarkStateFrameIndex =
            benchmarkStateFrameIndex(options_, metadata.applicationFrameIndex);
        metadata.temporalJitter = record != captureFrames_.end()
            ? record->jitter : std::nullopt;
    }

    std::vector<CaptureArtifactPaths>
    QualificationHarness::exportCaptureArtifacts(
        const AppShutdownContext& context,
        const SystemProfile& systemProfile) {
        std::vector<CaptureArtifactPaths> artifacts;
        if (!completedCapture_ && streamedCaptures_.empty()) return artifacts;
        const CaptureArtifactMetadata runMetadata =
            runCaptureMetadata(context, systemProfile);
        const auto report = [](const CaptureArtifactPaths& captureArtifact) {
            std::cout << "IRIDIUM_CAPTURE {\"image\":\""
                << captureArtifact.image.generic_string() << "\",\"metadata\":\""
                << captureArtifact.metadata.generic_string() << "\",\"sha256\":\""
                << captureArtifact.imageSha256 << "\"}\n";
        };
        if (completedCapture_) {
            CaptureArtifactMetadata captureMetadata = runMetadata;
            setCaptureFrame(captureMetadata, *options_.captureFrameIndex);
            artifacts.push_back(writeCaptureArtifact(
                options_.captureDirectory, *completedCapture_, captureMetadata));
            report(artifacts.back());
        }
        // Sequence sidecars, committed in capture order; a failure discards
        // every image not yet committed.
        try {
            while (!streamedCaptures_.empty()) {
                const PendingCaptureImage& image = streamedCaptures_.front();
                CaptureArtifactMetadata captureMetadata = runMetadata;
                setCaptureFrame(captureMetadata, image.captureId);
                artifacts.push_back(commitCaptureArtifact(image, captureMetadata));
                streamedCaptures_.erase(streamedCaptures_.begin());
                report(artifacts.back());
            }
        }
        catch (...) {
            discardStreamedCaptures();
            throw;
        }
        return artifacts;
    }

    CaptureArtifactMetadata QualificationHarness::runCaptureMetadata(
        const AppShutdownContext& context,
        const SystemProfile& systemProfile) const {
        const ApplicationConfig& config = context.config;
        const AppRunSnapshot& run = context.run;
        CaptureArtifactMetadata captureMetadata{};
        captureMetadata.requireSpatialSignal = options_.requireCaptureSignal;
        captureMetadata.buildConfiguration = BuildInfo::configuration();
        captureMetadata.sourceCommit = BuildInfo::sourceCommit();
        captureMetadata.sourceBranch = BuildInfo::sourceBranch();
        captureMetadata.sourceDirtyAtConfigure =
            BuildInfo::sourceDirtyAtConfigure();
        captureMetadata.validationEnabled = config.enableValidation;
        captureMetadata.cpuProfilingEnabled = context.profiler.isEnabled();
        captureMetadata.gpuProfilingRequested = config.enableGpuProfiling;
        captureMetadata.gpuProfilingAvailable = config.enableGpuProfiling &&
            capabilities_.gpuTimestampProfiling;
        captureMetadata.windowVisible = config.windowVisible;
        captureMetadata.windowDecorated = config.windowDecorated;
        captureMetadata.compiler = BuildInfo::compiler();
        captureMetadata.shaderCompiler = BuildInfo::shaderCompiler();
        captureMetadata.operatingSystem = systemProfile.operatingSystem;
        captureMetadata.cpuName = systemProfile.cpuName;
        captureMetadata.systemMemoryBytes = systemProfile.physicalMemoryBytes;
        captureMetadata.gpuName = runtimeInfo_.gpuName;
        captureMetadata.gpuUuid = runtimeInfo_.gpuUuid;
        captureMetadata.gpuDriver = runtimeInfo_.driverName + " " +
            runtimeInfo_.driverVersion;
        captureMetadata.vulkanDeviceApiVersion =
            runtimeInfo_.vulkanDeviceApiVersion;
        captureMetadata.vulkanLoaderApiVersion =
            runtimeInfo_.vulkanLoaderApiVersion;
        captureMetadata.vulkanSdkVersion = BuildInfo::vulkanSdk();
        captureMetadata.applicationEnabledLayers =
            runtimeInfo_.applicationEnabledLayers;
        captureMetadata.activeTools = runtimeInfo_.activeTools;
        captureMetadata.swapchainFormat = runtimeInfo_.swapchainFormat;
        captureMetadata.swapchainColorSpace =
            runtimeInfo_.swapchainColorSpace;
        captureMetadata.presentMode = runtimeInfo_.presentMode;
        captureMetadata.outputMode = runtimeInfo_.outputMode;
        captureMetadata.reconstructionMode =
            runtimeInfo_.reconstructionMode;
        captureMetadata.qualitySettings =
            "m5_directional_shadow_" + std::to_string(
                config.shadowSettings.directionalResolution) +
            "_d32_4c_" + std::to_string(
                config.shadowSettings.maximumDirectionalLights) +
            "l_5x5_tent";
        captureMetadata.qualitySettings += options_.forceDirectGBufferReference
            ? "_gbuffer_direct_unculled_reference" : "_gbuffer_automatic";
        captureMetadata.qualitySettings +=
            (options_.forceDirectGBufferReference ||
                options_.forceDirectShadowReference)
            ? "_shadow_direct_reference" : "_shadow_automatic";
        captureMetadata.qualitySettings +=
            (options_.forceDirectGBufferReference ||
                options_.forceDirectProbeCaptureReference)
            ? "_probe_capture_direct_reference"
            : "_probe_capture_automatic";
        captureMetadata.qualitySettings += "_shadow_indirect_oracle_" +
            std::to_string(options_.shadowIndirectQualificationOracle);
        captureMetadata.qualitySettings += "_shadow_lod_texels_" +
            std::to_string(config.experimentalShadowLodErrorTexels) +
            "_max_" + std::to_string(config.shadowLodMaximumLevel);
        captureMetadata.qualitySettings += "_experimental_lod_px_" +
            std::to_string(config.experimentalGpuLodErrorPixels) + "_max_" +
            std::to_string(config.gpuLodMaximumLevel) + "_hysteresis_" +
            std::to_string(config.gpuLodHysteresisFraction) + "_oracle_" +
            std::to_string(options_.gpuLodQualificationOracle) + "_resident_floor_" +
            std::to_string(options_.gpuLodMinimumResidentLevel);
        captureMetadata.qualitySettings += "_probe_lod_px_" +
            std::to_string(config.experimentalProbeLodErrorPixels) +
            "_max_" + std::to_string(config.probeLodMaximumLevel) +
            "_oracle_" + std::to_string(options_.probeLodQualificationOracle);
        captureMetadata.qualitySettings += "_depth_pyramid_" +
            std::to_string(config.experimentalDepthPyramid) +
            "_occlusion_query_" +
            std::to_string(config.experimentalDepthOcclusionQuery) +
            "_occlusion_rejection_" +
            std::to_string(config.experimentalDepthOcclusionRejection);
        captureMetadata.qualitySettings += "_occlusion_oracle_" +
            std::to_string(options_.depthOcclusionQualificationOracle);
        captureMetadata.qualitySettings += "_fixture_lights_" +
            std::to_string(benchmark_
                ? benchmark_->lights.size() : 0u);
        captureMetadata.cacheState = options_.cacheState;
        captureMetadata.outputOperator = outputOperatorName(config.outputOperator);
        captureMetadata.manualExposureEv = config.manualExposureEv;
        captureMetadata.gamutMapping = gamutMappingName(config.outputOperator);
        const Color::OutputTransport effectiveOutputTransport =
            runtimeInfo_.effectiveOutputTransportMode;
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
            captureMetadata.paperWhiteNits = config.paperWhiteNits;
            captureMetadata.peakNits = config.peakNits;
        }
        captureMetadata.acesPackageVersion = "v2.0.0+2025.04.04";
        captureMetadata.acesTransformId = transformId(config.outputOperator,
            effectiveOutputTransport);
        captureMetadata.warmupFrameCount = config.warmupFrameCount;
        setCaptureNaming(captureMetadata, config, 0u);
        captureMetadata.debugViewSemantics = config.forceWireframe
            ? "editor opaque geometry in wireframe with normal forward composition"
            : std::string(renderDebugViewDescription(config.debugView));
        if (benchmark_) {
            captureMetadata.manifestPath = benchmarkManifestPath_;
            captureMetadata.manifestSha256 = benchmarkManifestSha256_;
            for (const BenchmarkContentFile& file :
                benchmark_->contentFiles) {
                captureMetadata.contentHashes.emplace_back(
                    file.relativePath.generic_string(), file.sha256);
            }
        }
        captureMetadata.modelLoadMode =
            config.cookedModelArtifact.empty()
            ? "source-import"
            : "self-contained-cooked-artifact";
        if (run.mainModel) {
            captureMetadata.modelLocation =
                run.mainModel->filePath;
            captureMetadata.modelAssetGuid =
                run.mainModel->assetGuid.isNil()
                ? "" : run.mainModel->assetGuid.toString();
            captureMetadata.modelArtifactCookKey =
                run.mainModel->artifactCookKey;
        }
        captureMetadata.environmentLoadMode =
            run.environment.cookedArtifact.empty()
            // Metadata is emitted after cleanup retires GPU handles; use
            // retained publication identity, not the cleared lighting handle.
            ? (benchmark_ && !run.environment.assetGuid.isNil()
                ? "benchmark-procedural-constant" : "neutral-black-fallback")
            : "self-contained-cooked-artifact";
        captureMetadata.environmentLocation =
            run.environment.cookedArtifact.generic_string();
        captureMetadata.environmentAssetGuid =
            run.environment.assetGuid.isNil()
            ? "" : run.environment.assetGuid.toString();
        captureMetadata.environmentArtifactCookKey =
            run.environment.cookKey;
        captureMetadata.environmentSourceTextureGuid =
            run.environment.sourceGuid.isNil()
            ? "" : run.environment.sourceGuid.toString();
        captureMetadata.environmentSourcePrimaries =
            run.environment.sourcePrimaries;
        captureMetadata.environmentRadianceScale =
            run.environment.radianceScale;
        captureMetadata.directionalShadowActive =
            run.directionalShadowSelection.has_value() &&
            run.directionalShadowSampleableMask != 0;
        captureMetadata.directionalShadowOwnerCount =
            run.directionalShadowOwnerCount;
        if (run.directionalShadowSelection) {
            captureMetadata.directionalShadowOwner =
                run.directionalShadowSelection->owner.toString();
            captureMetadata.directionalShadowLightSlot =
                run.directionalShadowSelection->lightSlot;
            captureMetadata.omittedShadowDirectionalLights =
                run.directionalShadowSelection->
                    omittedShadowDirectionalLights;
        }
        captureMetadata.directionalShadowResolution =
            config.shadowSettings.directionalResolution;
        captureMetadata.directionalShadowCascadeCount =
            kDirectionalShadowCascadeCount;
        captureMetadata.directionalShadowSampleableMask =
            run.directionalShadowSampleableMask;
        captureMetadata.directionalShadowFormat = "D32_SFLOAT";
        const uint32_t selectedShadowQuality =
            run.directionalShadowSelection
            ? run.directionalShadowSelection->quality
            : static_cast<uint32_t>(ShadowQualityProfile::Ultra);
        const ShadowFilterProfile captureShadowFilter =
            effectiveShadowFilterProfile(config.shadowSettings,
                selectedShadowQuality);
        captureMetadata.directionalShadowFilter =
            captureShadowFilter.contactHardening
            ? "bounded_spatial_pcss" : "fixed_5x5_pcf";
        captureMetadata.directionalShadowSourceAngularDiameterDegrees =
            config.shadowSettings.directionalSourceAngularDiameterDegrees;
        captureMetadata.directionalShadowMaximumPenumbraTexels =
            captureShadowFilter.maximumPenumbraTexels;
        captureMetadata.directionalShadowReceiverDepthBiasTexels =
            config.shadowSettings.directionalReceiverDepthBiasTexels;
        captureMetadata.directionalShadowReceiverPlaneClampTexels =
            config.shadowSettings.directionalReceiverPlaneClampTexels;
        captureMetadata.directionalShadowNormalOffsetTexels =
            config.shadowSettings.directionalNormalOffsetTexels;
        captureMetadata.directionalShadowBlockerSearchSamples =
            captureShadowFilter.blockerSearchSamples;
        captureMetadata.directionalShadowFilterSamples =
            captureShadowFilter.filterSamples;
        captureMetadata.unavailableFields = {};
        return captureMetadata;
    }

    void QualificationHarness::exportCpuProfile(
        const AppShutdownContext& context,
        const SystemProfile& systemProfile,
        const std::vector<CaptureArtifactPaths>& captureArtifacts) const {
        const ApplicationConfig& config = context.config;
        if (options_.cpuProfileOutput.empty()) return;
        const AppRunSnapshot& run = context.run;
        CpuProfileRunMetadata metadata{};
        metadata.runId = "cpu-" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        metadata.buildConfiguration = BuildInfo::configuration();
        metadata.sourceCommit = BuildInfo::sourceCommit();
        metadata.sourceBranch = BuildInfo::sourceBranch();
        metadata.compiler = BuildInfo::compiler();
        metadata.shaderCompiler = BuildInfo::shaderCompiler();
        metadata.vulkanSdkVersion = BuildInfo::vulkanSdk();
        metadata.operatingSystem = systemProfile.operatingSystem;
        metadata.cpuName = systemProfile.cpuName;
        metadata.systemMemoryBytes = systemProfile.physicalMemoryBytes;
        metadata.gpuName = runtimeInfo_.gpuName;
        metadata.gpuUuid = runtimeInfo_.gpuUuid;
        metadata.gpuVendorId = runtimeInfo_.gpuVendorId;
        metadata.gpuDeviceId = runtimeInfo_.gpuDeviceId;
        metadata.gpuDriverName = runtimeInfo_.driverName;
        metadata.gpuDriverVersion = runtimeInfo_.driverVersion;
        metadata.gpuDriverInfo = runtimeInfo_.driverInfo;
        metadata.vulkanDeviceApiVersion =
            runtimeInfo_.vulkanDeviceApiVersion;
        metadata.vulkanLoaderApiVersion =
            runtimeInfo_.vulkanLoaderApiVersion;
        metadata.applicationEnabledLayers =
            runtimeInfo_.applicationEnabledLayers;
        metadata.activeVulkanTools = runtimeInfo_.activeTools;
        metadata.sourceDirtyAtConfigure = BuildInfo::sourceDirtyAtConfigure();
        metadata.validationEnabled = config.enableValidation;
        metadata.windowVisible = config.windowVisible;
        metadata.windowDecorated = config.windowDecorated;
        metadata.requestedWindowWidth = config.windowWidth;
        metadata.requestedWindowHeight = config.windowHeight;
        metadata.renderWidth = run.renderExtent.width;
        metadata.renderHeight = run.renderExtent.height;
        metadata.warmupFrameCount = config.warmupFrameCount;
        metadata.frameLimit = config.frameLimit;
        metadata.measuredFrameCount = run.measuredFrameCount;
        metadata.measurementWallNanoseconds = run.measurementWallNanoseconds;
        metadata.cpuProfilingEnabled = context.profiler.isEnabled();
        metadata.gpuProfilingRequested = config.enableGpuProfiling;
        metadata.gpuProfilingAvailable = config.enableGpuProfiling &&
            capabilities_.gpuTimestampProfiling;
        metadata.gpuTimestampPeriodNanoseconds =
            capabilities_.gpuTimestampPeriodNanoseconds;
        metadata.gpuTimestampValidBits =
            capabilities_.gpuTimestampValidBits;
        metadata.engineAllocationTrackingAvailable =
            capabilities_.engineAllocationTracking;
        metadata.driverMemoryBudgetAvailable =
            capabilities_.driverMemoryBudget;
        metadata.cppAllocationTrackingAvailable = true;
        metadata.transparentPipelineStatisticsRequested =
            config.enableTransparentPipelineStatistics;
        metadata.transparentPipelineStatisticsAvailable =
            config.enableTransparentPipelineStatistics &&
            capabilities_.transparentPipelineStatistics;
        metadata.swapchainFormat = runtimeInfo_.swapchainFormat;
        metadata.swapchainColorSpace = runtimeInfo_.swapchainColorSpace;
        metadata.presentMode = runtimeInfo_.presentMode;
        metadata.swapchainImageCount = runtimeInfo_.swapchainImageCount;
			metadata.supportedOutputTransports =
				runtimeInfo_.supportedOutputTransports;
			metadata.requestedOutputTransport =
				runtimeInfo_.requestedOutputTransport;
			metadata.effectiveOutputTransport =
				runtimeInfo_.effectiveOutputTransport;
			metadata.outputTransportDiagnostic =
				runtimeInfo_.outputTransportDiagnostic;
			metadata.swapchainColorspaceExtensionEnabled =
				runtimeInfo_.swapchainColorspaceExtensionEnabled;
			metadata.hdrMetadataExtensionEnabled =
				runtimeInfo_.hdrMetadataExtensionEnabled;
        const bool effectiveSdr = runtimeInfo_.effectiveOutputTransport ==
            "sdr_srgb";
        const bool effectiveScRgb =
            runtimeInfo_.effectiveOutputTransport == "scrgb_linear";
        metadata.hdrMetadataApplied =
            runtimeInfo_.hdrMetadataExtensionEnabled &&
            runtimeInfo_.effectiveOutputTransport == "hdr10_pq";
        metadata.displayProfile = effectiveSdr
            ? "windows_sdr_rec709_srgb"
            : (effectiveScRgb ? "windows_scrgb_extended_srgb_linear"
                : "windows_hdr10_rec2100_pq");
        metadata.outputTransfer = effectiveSdr ? "iec_61966_2_1_srgb"
            : (effectiveScRgb ? "linear" : "st2084_pq");
        metadata.paperWhiteNits = effectiveSdr ? 100.0 :
            config.paperWhiteNits;
        metadata.peakNits = effectiveSdr ? 100.0 : config.peakNits;
        metadata.baseWidth = runtimeInfo_.baseWidth;
        metadata.baseHeight = runtimeInfo_.baseHeight;
        metadata.reconstructionMode = runtimeInfo_.reconstructionMode;
        metadata.outputMode = runtimeInfo_.outputMode;
        metadata.qualitySettings = "m5_cluster_" +
            std::to_string(config.clusterTileSize) + "x" +
            std::to_string(config.clusterTileSize) + "x" +
            std::to_string(config.clusterDepthSlices) +
            "_scene_linear_fixed_quality";
        metadata.qualitySettings += options_.forceDirectGBufferReference
            ? "_gbuffer_direct_unculled_reference" : "_gbuffer_automatic";
        metadata.qualitySettings +=
            (options_.forceDirectGBufferReference ||
                options_.forceDirectShadowReference)
            ? "_shadow_direct_reference" : "_shadow_automatic";
        metadata.qualitySettings +=
            (options_.forceDirectGBufferReference ||
                options_.forceDirectProbeCaptureReference)
            ? "_probe_capture_direct_reference"
            : "_probe_capture_automatic";
        metadata.qualitySettings += "_shadow_indirect_oracle_" +
            std::to_string(options_.shadowIndirectQualificationOracle);
        metadata.qualitySettings += "_shadow_lod_texels_" +
            std::to_string(config.experimentalShadowLodErrorTexels) +
            "_max_" + std::to_string(config.shadowLodMaximumLevel);
        metadata.qualitySettings += "_experimental_lod_px_" +
            std::to_string(config.experimentalGpuLodErrorPixels) + "_max_" +
            std::to_string(config.gpuLodMaximumLevel) + "_hysteresis_" +
            std::to_string(config.gpuLodHysteresisFraction) + "_oracle_" +
            std::to_string(options_.gpuLodQualificationOracle) + "_resident_floor_" +
            std::to_string(options_.gpuLodMinimumResidentLevel);
        metadata.qualitySettings += "_probe_lod_px_" +
            std::to_string(config.experimentalProbeLodErrorPixels) +
            "_max_" + std::to_string(config.probeLodMaximumLevel) +
            "_oracle_" + std::to_string(options_.probeLodQualificationOracle);
        metadata.qualitySettings += "_depth_pyramid_" +
            std::to_string(config.experimentalDepthPyramid) +
            "_occlusion_query_" +
            std::to_string(config.experimentalDepthOcclusionQuery) +
            "_occlusion_rejection_" +
            std::to_string(config.experimentalDepthOcclusionRejection);
        metadata.qualitySettings += "_occlusion_oracle_" +
            std::to_string(options_.depthOcclusionQualificationOracle);
        metadata.qualitySettings += "_fixture_lights_" +
            std::to_string(benchmark_
                ? benchmark_->lights.size() : 0u);
        metadata.renderMode = "graph_deferred_plus_forward_scene_linear_canonical_" +
            std::string(gBufferLayoutName(config.gBufferLayout)) +
            "_" + runtimeInfo_.textureBindingMode +
            (config.forceWireframe ? "_opaque_wireframe" : "");
        metadata.cacheState = options_.cacheState;
        metadata.outputOperator = std::string(outputOperatorName(
            config.outputOperator)) + "_final_output";
        metadata.exposureState = "manual_ev_" +
            std::to_string(config.manualExposureEv) +
            "_applied_in_final_output; cooked_ap1_environment_scale_in_manifest";
        metadata.colorDomain = "scene_linear_acescg_ap1_pre_output";
        metadata.renderGraphEnabled = runtimeInfo_.renderGraphEnabled;
        metadata.renderGraphTopologyHash =
            runtimeInfo_.renderGraphTopologyHash;
        metadata.renderGraphPassCount = runtimeInfo_.renderGraphPassCount;
        metadata.renderGraphLogicalResourceCount =
            runtimeInfo_.renderGraphLogicalResourceCount;
        metadata.renderGraphPhysicalSlotCount =
            runtimeInfo_.renderGraphPhysicalSlotCount;
        metadata.renderGraphBarrierCount =
            runtimeInfo_.renderGraphBarrierCount;
        metadata.renderGraphFrameCount = runtimeInfo_.renderGraphFrameCount;
        metadata.renderGraphRequestedBytes =
            runtimeInfo_.renderGraphRequestedBytes;
        metadata.renderGraphCommittedBytes =
            runtimeInfo_.renderGraphCommittedBytes;
        metadata.renderGraphRebuildCount =
            runtimeInfo_.renderGraphRebuildCount;
        metadata.renderGraphCacheMissCount =
            runtimeInfo_.renderGraphCacheMissCount;
        metadata.renderGraphTransientAliasing =
            runtimeInfo_.renderGraphTransientAliasing;
        metadata.renderGraphAliasHeapCount = runtimeInfo_.renderGraphAliasHeapCount;
        metadata.renderGraphAliasedResourceCount =
            runtimeInfo_.renderGraphAliasedResourceCount;
        metadata.renderGraphAliasedRequestedBytes =
            runtimeInfo_.renderGraphAliasedRequestedBytes;
        metadata.renderGraphAliasHeapCommittedBytes =
            runtimeInfo_.renderGraphAliasHeapCommittedBytes;
        metadata.ordinary2AtlasResident =
            runtimeInfo_.ordinary2AtlasResident;
        metadata.ordinary2AtlasWidth =
            runtimeInfo_.ordinary2AtlasWidth;
        metadata.ordinary2AtlasHeight =
            runtimeInfo_.ordinary2AtlasHeight;
        metadata.gpuLightRecordsAvailable =
            capabilities_.gpuLightRecords;
        metadata.maxGpuLightRecords =
            capabilities_.maxGpuLightRecords;
        metadata.multiDrawIndirectAvailable =
            capabilities_.multiDrawIndirect;
        metadata.drawIndirectFirstInstanceAvailable =
            capabilities_.drawIndirectFirstInstance;
        metadata.drawIndirectCountAvailable =
            capabilities_.drawIndirectCount;
        metadata.maxDrawIndirectCount =
            capabilities_.maxDrawIndirectCount;
        metadata.gpuLightCapacity = runtimeInfo_.gpuLightCapacity;
        metadata.gpuLightActiveCount =
            runtimeInfo_.gpuLightActiveCount;
        metadata.gpuLightUploadBytes =
            runtimeInfo_.gpuLightUploadBytes;
        metadata.gpuLightUploadRanges =
            runtimeInfo_.gpuLightUploadRanges;
        metadata.startupTotalNanoseconds = run.startup.totalNanoseconds;
        metadata.windowInitNanoseconds = run.startup.windowNanoseconds;
        metadata.backendInitNanoseconds = run.startup.backendNanoseconds;
        metadata.editorInitNanoseconds = run.startup.editorNanoseconds;
        metadata.manifestVerificationNanoseconds =
            run.startup.manifestVerificationNanoseconds;
        // Source import is no longer a production startup path. Preserve the
        // legacy field as zero for profile-schema compatibility and report the
        // cooked artifact load separately.
        metadata.sourceImportNanoseconds = 0;
        metadata.modelLoadNanoseconds = run.startup.modelLoadNanoseconds;
        metadata.environmentCreationNanoseconds =
            run.startup.environmentCreationNanoseconds;
        metadata.sceneConstructionNanoseconds =
            run.startup.sceneConstructionNanoseconds;
        metadata.frameTopologyPrewarmNanoseconds =
            run.startup.frameTopologyPrewarmNanoseconds;
        metadata.frameTopologyPrewarmRequested =
            runtimeInfo_.frameTopologyPrewarmRequested;
        metadata.frameTopologyPrewarmChanged =
            runtimeInfo_.frameTopologyPrewarmChanged;
        metadata.pipelineCacheState = runtimeInfo_.pipelineCacheState;
        metadata.pipelineCacheLoadedBytes = runtimeInfo_.pipelineCacheLoadedBytes;
        metadata.uploadQueueMode = std::string(runtimeInfo_.uploadQueueMode);
        metadata.uploadQueueKind = std::string(runtimeInfo_.uploadQueueKind);
        metadata.uploadQueueFamily = runtimeInfo_.uploadQueueFamily;
        metadata.uploadStagingRingBytes = runtimeInfo_.uploadStagingRingBytes;
        metadata.uploadStagingRingWaits = runtimeInfo_.uploads.stagingRingWaits;
        metadata.uploadDedicatedStagingUploads =
            runtimeInfo_.uploads.dedicatedStagingUploads;
        metadata.uploadAsyncSubmits = runtimeInfo_.uploads.asyncSubmits;
        metadata.refractionPyramidsResident =
            runtimeInfo_.refractionPyramidsResident;
        metadata.modelLoadMode =
            config.cookedModelArtifact.empty()
            ? "source-import"
            : "self-contained-cooked-artifact";
        if (run.mainModel) {
            metadata.modelLocation = run.mainModel->filePath;
            metadata.modelAssetGuid =
                run.mainModel->assetGuid.isNil()
                ? "" : run.mainModel->assetGuid.toString();
            metadata.modelArtifactCookKey =
                run.mainModel->artifactCookKey;
        }
        metadata.environmentLoadMode =
            run.environment.cookedArtifact.empty()
            ? (benchmark_ && !run.environment.assetGuid.isNil()
                ? "benchmark-procedural-constant" : "neutral-black-fallback")
            : "self-contained-cooked-artifact";
        metadata.environmentLocation =
            run.environment.cookedArtifact.generic_string();
        metadata.environmentAssetGuid =
            run.environment.assetGuid.isNil()
            ? "" : run.environment.assetGuid.toString();
        metadata.environmentArtifactCookKey =
            run.environment.cookKey;
        metadata.environmentSourceTextureGuid =
            run.environment.sourceGuid.isNil()
            ? "" : run.environment.sourceGuid.toString();
        metadata.environmentSourcePrimaries =
            run.environment.sourcePrimaries;
        metadata.environmentRadianceScale =
            run.environment.radianceScale;
        metadata.uploadSubmittedBytes =
            runtimeInfo_.uploads.submittedBytes;
        metadata.uploadSubmittedBatches =
            runtimeInfo_.uploads.submittedBatches;
        metadata.uploadSubmitAndWaitNanoseconds =
            runtimeInfo_.uploads.submitAndWaitNanoseconds;
        metadata.renderDebugView = config.forceWireframe
            ? "wireframe"
            : std::string(renderDebugViewName(run.debugView));
        metadata.renderDebugViewSemantics = config.forceWireframe
            ? "editor opaque geometry in wireframe with normal forward composition"
            : std::string(renderDebugViewDescription(run.debugView));
        if (benchmark_) {
            metadata.benchmarkFixtureId = benchmark_->id;
            metadata.benchmarkFixtureRevision = benchmark_->revision;
            metadata.benchmarkCameraId = benchmark_->camera.id;
            metadata.benchmarkManifestPath = benchmarkManifestPath_;
            metadata.benchmarkManifestSha256 = benchmarkManifestSha256_;
            for (const BenchmarkContentFile& file : benchmark_->contentFiles) {
                metadata.benchmarkContentHashes.emplace_back(
                    file.relativePath.generic_string(), file.sha256);
            }
        }
        metadata.unavailableFields = {
            "gpu_clocks_power_behavior"
        };
        for (const CaptureArtifactPaths& captureArtifact : captureArtifacts) {
            metadata.captureOutputs.emplace_back(
                captureArtifact.image.generic_string(),
                captureArtifact.imageSha256);
        }
        if (captureArtifacts.empty()) {
            metadata.unavailableFields.push_back("capture_outputs");
        }
        if (!benchmark_) {
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
        writeCpuProfileJsonLines(options_.cpuProfileOutput, context.profiler, metadata);
        appendScriptedChangeRecords(context);
    }

} // namespace Iridium
