#pragma once

#include "CpuProfiler.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace Iridium {

    struct CpuProfileRunMetadata {
        std::string runId;
        std::string buildConfiguration;
        std::string sourceCommit;
        std::string sourceBranch;
        std::string compiler;
        std::string shaderCompiler;
        std::string operatingSystem;
        std::string cpuName;
        uint64_t systemMemoryBytes = 0;
        std::string gpuName;
        std::string gpuUuid;
        uint32_t gpuVendorId = 0;
        uint32_t gpuDeviceId = 0;
        std::string gpuDriverName;
        std::string gpuDriverVersion;
        std::string gpuDriverInfo;
        std::string vulkanDeviceApiVersion;
        std::string vulkanLoaderApiVersion;
        std::string vulkanSdkVersion;
        std::vector<std::string> applicationEnabledLayers;
        std::vector<std::string> activeVulkanTools;
        bool sourceDirtyAtConfigure = false;
        bool validationEnabled = false;
        bool windowVisible = true;
        bool windowDecorated = true;
        uint32_t requestedWindowWidth = 0;
        uint32_t requestedWindowHeight = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint64_t warmupFrameCount = 0;
        uint64_t frameLimit = 0;
        uint64_t measuredFrameCount = 0;
        uint64_t measurementWallNanoseconds = 0;
        bool cpuProfilingEnabled = false;
        bool gpuProfilingRequested = false;
        bool gpuProfilingAvailable = false;
        double gpuTimestampPeriodNanoseconds = 0.0;
        uint32_t gpuTimestampValidBits = 0;
        bool engineAllocationTrackingAvailable = false;
        bool driverMemoryBudgetAvailable = false;
        bool cppAllocationTrackingAvailable = false;
        bool transparentPipelineStatisticsRequested = false;
        bool transparentPipelineStatisticsAvailable = false;
        std::string swapchainFormat;
        std::string swapchainColorSpace;
        std::string presentMode;
        uint32_t swapchainImageCount = 0;
		std::vector<std::string> supportedOutputTransports;
		std::string requestedOutputTransport;
		std::string effectiveOutputTransport;
		std::string outputTransportDiagnostic;
		bool swapchainColorspaceExtensionEnabled = false;
		bool hdrMetadataExtensionEnabled = false;
        bool hdrMetadataApplied = false;
        std::string displayProfile;
        std::string outputTransfer;
        double paperWhiteNits = 100.0;
        double peakNits = 100.0;
        double scRgbNitsPerUnit = 80.0;
        uint32_t baseWidth = 0;
        uint32_t baseHeight = 0;
        std::string reconstructionMode;
        std::string outputMode;
        std::string qualitySettings;
        std::string renderMode;
        std::string cacheState;
        std::string outputOperator;
        std::string exposureState;
        bool renderGraphEnabled = false;
        uint64_t renderGraphTopologyHash = 0;
        uint32_t renderGraphPassCount = 0;
        uint32_t renderGraphLogicalResourceCount = 0;
        uint32_t renderGraphPhysicalSlotCount = 0;
        uint32_t renderGraphBarrierCount = 0;
        uint32_t renderGraphFrameCount = 0;
        uint64_t renderGraphRequestedBytes = 0;
        uint64_t renderGraphCommittedBytes = 0;
        uint64_t renderGraphRebuildCount = 0;
        uint64_t renderGraphCacheMissCount = 0;
        bool gpuLightRecordsAvailable = false;
        uint32_t maxGpuLightRecords = 0;
        uint32_t gpuLightCapacity = 0;
        uint32_t gpuLightActiveCount = 0;
        uint64_t gpuLightUploadBytes = 0;
        uint32_t gpuLightUploadRanges = 0;
        uint64_t startupTotalNanoseconds = 0;
        uint64_t windowInitNanoseconds = 0;
        uint64_t backendInitNanoseconds = 0;
        uint64_t editorInitNanoseconds = 0;
        uint64_t manifestVerificationNanoseconds = 0;
        // Retained in schema 1 so historical profile readers remain compatible.
        // Production startup must leave this zero after the M3 cooked-only cutover.
        uint64_t sourceImportNanoseconds = 0;
        uint64_t modelLoadNanoseconds = 0;
        uint64_t environmentCreationNanoseconds = 0;
        uint64_t sceneConstructionNanoseconds = 0;
        std::string modelLoadMode;
        std::string modelLocation;
        std::string modelAssetGuid;
        std::string modelArtifactCookKey;
        std::string environmentLoadMode;
        std::string environmentLocation;
        std::string environmentAssetGuid;
        std::string environmentArtifactCookKey;
        std::string environmentSourceTextureGuid;
        std::string environmentSourcePrimaries;
        double environmentRadianceScale = 0.0;
        uint64_t uploadSubmittedBytes = 0;
        uint64_t uploadSubmittedBatches = 0;
        uint64_t uploadSubmitAndWaitNanoseconds = 0;
        std::string colorDomain = "scene_linear_acescg_ap1_pre_output";
        std::string benchmarkFixtureId;
        uint32_t benchmarkFixtureRevision = 0;
        std::string benchmarkCameraId;
        std::string benchmarkManifestPath;
        std::string benchmarkManifestSha256;
        std::string renderDebugView = "final";
        std::string renderDebugViewSemantics;
        std::vector<std::pair<std::string, std::string>> benchmarkContentHashes;
        std::vector<std::pair<std::string, std::string>> captureOutputs;
        std::vector<std::string> unavailableFields;
    };

    void writeCpuProfileJsonLines(std::ostream& output,
        const CpuProfiler& profiler, const CpuProfileRunMetadata& metadata);

    void writeCpuProfileJsonLines(const std::filesystem::path& outputPath,
        const CpuProfiler& profiler, const CpuProfileRunMetadata& metadata);

} // namespace Iridium
