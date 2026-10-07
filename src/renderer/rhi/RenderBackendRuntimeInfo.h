#pragma once

#include "renderer/color/OutputTransformConfig.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    struct BackendUploadTelemetry {
        uint64_t submittedBytes = 0;
        uint64_t submittedBatches = 0;
        uint64_t submitAndWaitNanoseconds = 0;
        // M7R R4d: CPU waits for a full staging ring, uploads that used
        // dedicated staging, and batches submitted without a CPU wait.
        uint64_t stagingRingWaits = 0;
        uint64_t dedicatedStagingUploads = 0;
        uint64_t asyncSubmits = 0;
    };

    struct RenderBackendRuntimeInfo {
        std::string backendApi;
        std::string gpuName;
        std::string gpuUuid;
        uint32_t gpuVendorId = 0;
        uint32_t gpuDeviceId = 0;
        std::string driverName;
        std::string driverInfo;
        std::string driverVersion;
        std::string vulkanDeviceApiVersion;
        std::string vulkanLoaderApiVersion;
        std::vector<std::string> applicationEnabledLayers;
        std::vector<std::string> activeTools;
        std::string swapchainFormat;
        std::string swapchainColorSpace;
        std::string presentMode;
        uint32_t swapchainImageCount = 0;
		std::vector<std::string> supportedOutputTransports;
		std::string requestedOutputTransport;
		std::string effectiveOutputTransport;
		std::string outputTransportDiagnostic;
		std::array<bool, 3> supportedOutputTransportModes{};
		Color::OutputTransport requestedOutputTransportMode =
			Color::OutputTransport::SdrSrgb;
		Color::OutputTransport effectiveOutputTransportMode =
			Color::OutputTransport::SdrSrgb;
		bool swapchainColorspaceExtensionEnabled = false;
		bool hdrMetadataExtensionEnabled = false;
        std::string outputMode;
        uint32_t baseWidth = 0;
        uint32_t baseHeight = 0;
        std::string reconstructionMode;
        std::string textureBindingMode;
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
        // M7R R4b.4 transient aliasing: heaps per frame slot; byte totals over
        // every slot (committed bytes above include the heaps).
        bool renderGraphTransientAliasing = false;
        uint32_t renderGraphAliasHeapCount = 0;
        uint32_t renderGraphAliasedResourceCount = 0;
        uint64_t renderGraphAliasedRequestedBytes = 0;
        uint64_t renderGraphAliasHeapCommittedBytes = 0;
        bool refractionPyramidsResident = false;
        bool ordinary2AtlasResident = false;
        uint32_t ordinary2AtlasWidth = 0;
        uint32_t ordinary2AtlasHeight = 0;
        bool hero4AtlasResident = false;
        uint32_t hero4AtlasWidth = 0;
        uint32_t hero4AtlasHeight = 0;
        bool cinematic8AtlasResident = false;
        uint32_t cinematic8AtlasWidth = 0;
        uint32_t cinematic8AtlasHeight = 0;
        bool weightedOitResident = false;
        bool frameTopologyPrewarmRequested = false;
        bool frameTopologyPrewarmChanged = false;
        uint64_t frameTopologyPrewarmNanoseconds = 0;
        // Persisted pipeline cache at backend init (M7R R4c.4): off, cold,
        // warm or discarded, and the payload bytes loaded.
        std::string pipelineCacheState = "off";
        uint64_t pipelineCacheLoadedBytes = 0;
        // M7R R4d (--upload-queue): the mode in effect, the queue family
        // fresh uploads use and its kind (dedicated-transfer, async-compute or
        // graphics), and the staging ring size (0 in legacy-blocking). Static
        // names: qualification validators read this struct every frame.
        std::string_view uploadQueueMode = "legacy-blocking";
        std::string_view uploadQueueKind = "graphics";
        uint32_t uploadQueueFamily = 0;
        uint64_t uploadStagingRingBytes = 0;
        uint32_t gpuLightCapacity = 0;
        uint32_t gpuLightActiveCount = 0;
        uint64_t gpuLightUploadBytes = 0;
        uint32_t gpuLightUploadRanges = 0;
        uint32_t gpuSceneTransformCapacity = 0;
        uint32_t gpuSceneInstanceCapacity = 0;
        uint32_t gpuScenePrimitiveCapacity = 0;
        uint32_t gpuSceneGeometryCapacity = 0;
        uint64_t gpuSceneUploadBytes = 0;
        uint32_t gpuSceneUploadRanges = 0;
        BackendUploadTelemetry uploads;
    };

} // namespace Iridium
