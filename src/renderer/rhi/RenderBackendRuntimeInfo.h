#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Iridium {

    struct BackendUploadTelemetry {
        uint64_t submittedBytes = 0;
        uint64_t submittedBatches = 0;
        uint64_t submitAndWaitNanoseconds = 0;
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
        bool frameTopologyPrewarmRequested = false;
        bool frameTopologyPrewarmChanged = false;
        uint64_t frameTopologyPrewarmNanoseconds = 0;
        uint32_t gpuLightCapacity = 0;
        uint32_t gpuLightActiveCount = 0;
        uint64_t gpuLightUploadBytes = 0;
        uint32_t gpuLightUploadRanges = 0;
        BackendUploadTelemetry uploads;
    };

} // namespace Iridium
