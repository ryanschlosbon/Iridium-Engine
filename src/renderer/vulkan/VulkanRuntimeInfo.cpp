#include "VulkanVertexBackend.h"
#include "profiling/CpuProfiler.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

// M7.10.0: the backend's runtime information, capabilities and per-frame
// telemetry reporting, with their string helpers. Moved unchanged out of
// VulkanVertexBackend.cpp; these are VulkanVertexBackend members.

namespace Iridium {

    namespace {
        uint64_t swapchainRequestedBytes(const VkSwapchain& swapchain) noexcept {
            uint64_t bytesPerTexel = 0;
            switch (swapchain.getImageFormat()) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                bytesPerTexel = 4;
                break;
            default:
                break;
            }
            const VkExtent2D extent = swapchain.getExtent();
            return static_cast<uint64_t>(extent.width) * extent.height *
                swapchain.getImageCount() * bytesPerTexel;
        }

        std::string versionString(uint32_t version) {
            return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
                std::to_string(VK_API_VERSION_MINOR(version)) + "." +
                std::to_string(VK_API_VERSION_PATCH(version));
        }

        std::string uuidString(const uint8_t* uuid, size_t size) {
            std::ostringstream output;
            output << std::hex << std::setfill('0');
            for (size_t index = 0; index < size; ++index) {
                output << std::setw(2) << static_cast<unsigned>(uuid[index]);
            }
            return output.str();
        }

        std::string driverVersionString(uint32_t vendorId, uint32_t version) {
            if (vendorId == 0x10de) {
                return std::to_string((version >> 22) & 0x3ff) + "." +
                    std::to_string((version >> 14) & 0xff) + "." +
                    std::to_string((version >> 6) & 0xff) + "." +
                    std::to_string(version & 0x3f);
            }
            return versionString(version);
        }

        const char* formatName(VkFormat format) noexcept {
            switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
            case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
            case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
            case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return "VK_FORMAT_R16G16B16A16_SFLOAT";
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
				return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
			case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
				return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
            default: return "VK_FORMAT_OTHER";
            }
        }

        const char* colorSpaceName(VkColorSpaceKHR colorSpace) noexcept {
            switch (colorSpace) {
            case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
                return "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR";
			case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
				return "VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT";
			case VK_COLOR_SPACE_HDR10_ST2084_EXT:
				return "VK_COLOR_SPACE_HDR10_ST2084_EXT";
            default: return "VK_COLOR_SPACE_OTHER";
            }
        }

        const char* presentModeName(VkPresentModeKHR mode) noexcept {
            switch (mode) {
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return "VK_PRESENT_MODE_IMMEDIATE_KHR";
            case VK_PRESENT_MODE_MAILBOX_KHR: return "VK_PRESENT_MODE_MAILBOX_KHR";
            case VK_PRESENT_MODE_FIFO_KHR: return "VK_PRESENT_MODE_FIFO_KHR";
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "VK_PRESENT_MODE_FIFO_RELAXED_KHR";
            default: return "VK_PRESENT_MODE_OTHER";
            }
        }

		const char* outputTransportName(Color::OutputTransport transport) noexcept {
			switch (transport) {
			case Color::OutputTransport::SdrSrgb: return "sdr_srgb";
			case Color::OutputTransport::ScRgb: return "scrgb_linear";
			case Color::OutputTransport::Hdr10Pq: return "hdr10_pq";
			case Color::OutputTransport::Automatic: return "auto";
			}
			return "unknown";
		}
    }

    RenderBackendCapabilities VulkanVertexBackend::getCapabilities() const {
        if (!vkContext) {
            return {};
        }
        const double period = vkContext->getTimestampPeriodNanoseconds();
        const uint32_t validBits = vkContext->getTimestampValidBits();
        return {
            .gpuTimestampProfiling = period > 0.0 && validBits > 0 && validBits <= 64,
            .gpuTimestampPeriodNanoseconds = period,
            .gpuTimestampValidBits = validBits,
            .engineAllocationTracking = true,
            .driverMemoryBudget = vkContext->hasMemoryBudget(),
            .transparentPipelineStatistics = vkContext->hasPipelineStatistics(),
            .indexedTextureViews = vkContext->hasDescriptorIndexing(),
            .separateTextureSamplers = vkContext->hasDescriptorIndexing(),
            .descriptorUpdateAfterBind = vkContext->hasDescriptorIndexing(),
            .gpuLightRecords = clusterLighting_.lightRecordCapacity() != 0,
            .multiDrawIndirect = vkContext->hasMultiDrawIndirect(),
            .drawIndirectFirstInstance =
                vkContext->hasDrawIndirectFirstInstance(),
            .drawIndirectCount = vkContext->hasDrawIndirectCount(),
            .maxIndexedTextureViews = vkContext->getMaxIndexedTextureViews(),
            .maxIndexedSamplers = vkContext->getMaxIndexedSamplers(),
            .maxUpdateAfterBindDescriptors =
                vkContext->getMaxUpdateAfterBindDescriptors(),
            .maxGpuLightRecords = clusterLighting_.lightRecordMaximumCapacity(),
            .maxDrawIndirectCount = vkContext->getMaxDrawIndirectCount(),
        };
    }

    RenderBackendRuntimeInfo VulkanVertexBackend::getRuntimeInfo() const {
        RenderBackendRuntimeInfo info{};
        if (!vkContext || !vkSwapchain) {
            return info;
        }
        const VkPhysicalDeviceProperties& properties =
            vkContext->getPhysicalDeviceProperties();
        const VkPhysicalDeviceIDProperties& idProperties =
            vkContext->getPhysicalDeviceIdProperties();
        const VkPhysicalDeviceDriverProperties& driverProperties =
            vkContext->getPhysicalDeviceDriverProperties();
        const VkExtent2D extent = sceneExtent_;

        info.backendApi = "Vulkan";
        info.gpuName = properties.deviceName;
        info.gpuUuid = uuidString(idProperties.deviceUUID, VK_UUID_SIZE);
        info.gpuVendorId = properties.vendorID;
        info.gpuDeviceId = properties.deviceID;
        info.driverName = driverProperties.driverName;
        info.driverInfo = driverProperties.driverInfo;
        info.driverVersion = driverVersionString(properties.vendorID,
            properties.driverVersion);
        info.vulkanDeviceApiVersion = versionString(properties.apiVersion);
        info.vulkanLoaderApiVersion = versionString(vkContext->getLoaderApiVersion());
        if (vkContext->enableValidationLayers) {
            info.applicationEnabledLayers.emplace_back(
                "VK_LAYER_KHRONOS_validation");
        }
        info.activeTools = vkContext->getActiveTools();
        info.swapchainFormat = formatName(vkSwapchain->getImageFormat());
        info.swapchainColorSpace = colorSpaceName(vkSwapchain->getColorSpace());
        info.presentMode = presentModeName(vkSwapchain->getPresentMode());
        info.swapchainImageCount = vkSwapchain->getImageCount();
        info.gpuSceneTransformCapacity = gpuScene_.capacity().transforms;
        info.gpuSceneInstanceCapacity = gpuScene_.capacity().instances;
        info.gpuScenePrimitiveCapacity = gpuScene_.capacity().primitives;
        info.gpuSceneGeometryCapacity = gpuScene_.capacity().geometries;
        info.gpuSceneUploadBytes = gpuScene_.uploadTelemetry().bytes;
        info.gpuSceneUploadRanges = gpuScene_.uploadTelemetry().ranges;
		for (const Color::OutputTransport transport :
			vkSwapchain->getSupportedOutputTransports()) {
			info.supportedOutputTransports.emplace_back(outputTransportName(transport));
		}
		const VulkanOutputTransportSelection& transport =
			vkSwapchain->getOutputTransportSelection();
		info.requestedOutputTransportMode = transport.requested;
		info.effectiveOutputTransportMode = transport.effective;
		info.requestedOutputTransport = outputTransportName(transport.requested);
		info.effectiveOutputTransport = outputTransportName(transport.effective);
		info.outputTransportDiagnostic = transport.diagnostic;
		for (const Color::OutputTransport supported :
				vkSwapchain->getSupportedOutputTransports()) {
			const size_t index = static_cast<size_t>(supported);
			if (index < info.supportedOutputTransportModes.size()) {
				info.supportedOutputTransportModes[index] = true;
			}
		}
		info.swapchainColorspaceExtensionEnabled =
			vkContext->hasSwapchainColorspace();
		info.hdrMetadataExtensionEnabled = vkContext->hasHdrMetadata();
        if (outputTransport_ == Color::OutputTransport::ScRgb) {
            info.outputMode =
                "scene_linear_acescg_to_aces2_p3d65_1000nit_scrgb_linear";
        }
        else if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            info.outputMode =
                "scene_linear_acescg_to_aces2_p3d65_1000nit_rec2100_pq_hdr10";
        }
        else switch (output_.outputOperator()) {
        case OutputTransformOperator::Aces2:
            info.outputMode = "scene_linear_acescg_to_aces2_rec709_srgb_sdr";
            break;
        case OutputTransformOperator::AcesFittedLegacy:
            info.outputMode = "scene_linear_acescg_to_aces_fitted_legacy_srgb_sdr";
            break;
        case OutputTransformOperator::IdentityClampDiagnostic:
            info.outputMode = "scene_linear_acescg_to_identity_clamp_srgb_sdr";
            break;
        }
        info.baseWidth = extent.width;
        info.baseHeight = extent.height;
        info.reconstructionMode = "none_native";
        info.textureBindingMode =
            "indexed_views_separate_samplers";
        const VulkanGraphStats graphStats = renderGraph_.stats();
        info.renderGraphEnabled = true;
        info.renderGraphTopologyHash = graphStats.topologyHash;
        info.renderGraphPassCount = graphStats.passCount;
        info.renderGraphLogicalResourceCount = graphStats.logicalResourceCount;
        info.renderGraphPhysicalSlotCount = graphStats.physicalSlotCount;
        info.renderGraphBarrierCount = graphStats.barrierCount;
        info.renderGraphFrameCount = graphStats.frameCount;
        info.renderGraphRequestedBytes = graphStats.requestedBytes;
        info.renderGraphCommittedBytes = graphStats.committedBytes;
        info.renderGraphRebuildCount = graphStats.rebuildCount;
        info.renderGraphCacheMissCount = graphStats.cacheMissCount;
        info.renderGraphTransientAliasing = graphStats.transientAliasing;
        info.renderGraphAliasHeapCount = graphStats.aliasHeapCount;
        info.renderGraphAliasedResourceCount = graphStats.aliasedResourceCount;
        info.renderGraphAliasedRequestedBytes = graphStats.aliasedRequestedBytes;
        info.renderGraphAliasHeapCommittedBytes = graphStats.aliasHeapCommittedBytes;
        info.refractionPyramidsResident =
            forward_.pyramidResidency().enabled();
        info.ordinary2AtlasResident = layered_.ordinary2Residency().enabled() &&
            layered_.ordinary2Extent().width != 0u &&
            layered_.ordinary2Extent().height != 0u;
        info.ordinary2AtlasWidth = layered_.ordinary2Extent().width;
        info.ordinary2AtlasHeight = layered_.ordinary2Extent().height;
        info.hero4AtlasResident = layered_.hero4Residency().enabled() &&
            layered_.hero4Extent().width != 0u && layered_.hero4Extent().height != 0u;
        info.hero4AtlasWidth = layered_.hero4Extent().width;
        info.hero4AtlasHeight = layered_.hero4Extent().height;
        info.cinematic8AtlasResident = layered_.cinematic8Residency().enabled() &&
            layered_.cinematic8Extent().width != 0u &&
            layered_.cinematic8Extent().height != 0u;
        info.cinematic8AtlasWidth = layered_.cinematic8Extent().width;
        info.cinematic8AtlasHeight = layered_.cinematic8Extent().height;
        info.weightedOitResident = oit_.residency().enabled();
        info.frameTopologyPrewarmRequested =
            frameTopologyPrewarm_.requested;
        info.frameTopologyPrewarmChanged = frameTopologyPrewarm_.changed;
        info.frameTopologyPrewarmNanoseconds =
            frameTopologyPrewarm_.durationNanoseconds;
        info.pipelineCacheState =
            std::string(pipelineCacheStateName(pipelineCache_.stats().state));
        info.pipelineCacheLoadedBytes = pipelineCache_.stats().loadedBytes;
        const LightingUploadTelemetry lightUploads = clusterLighting_.uploadTelemetry();
        info.gpuLightCapacity = lightUploads.capacity;
        info.gpuLightActiveCount = lightUploads.activeLights;
        info.gpuLightUploadBytes = lightUploads.bytes;
        info.gpuLightUploadRanges = lightUploads.ranges;
        info.uploads = uploadContext.telemetry();
        switch (uploadContext.mode()) {
        case UploadQueueMode::Auto: info.uploadQueueMode = "auto"; break;
        case UploadQueueMode::Graphics: info.uploadQueueMode = "graphics"; break;
        case UploadQueueMode::LegacyBlocking:
            info.uploadQueueMode = "legacy-blocking";
            break;
        }
        info.uploadQueueKind = uploadContext.usesTransferQueue()
            ? vulkanTransferQueueKindName(vkContext->getTransferQueueKind())
            : std::string_view("graphics");
        info.uploadQueueFamily = uploadContext.usesTransferQueue()
            ? uploadContext.transferQueueFamily() : vkContext->getGraphicsQueueFamily();
        info.uploadStagingRingBytes = uploadContext.stagingRingBytes();
        return info;
    }

    RenderFrameTelemetry VulkanVertexBackend::frameTelemetry() const noexcept {
        return {
            .gpuSceneUpload = gpuScene_.uploadTelemetry(),
            .probeCaptures = probes_.telemetry(),
            .lightUploads = clusterLighting_.uploadTelemetry(),
            .clusters = clusterLighting_.clusterTelemetry(),
        };
    }

    void VulkanVertexBackend::emitFrameCounters() {
        if (!telemetry_.collecting() || cpuProfiler_ == nullptr) {
            return;
        }
        const VulkanIndexedTextureTable& table = resources_.textureTable();
        telemetry_.emit({
            .weightedOitResident = oit_.residency().enabled(),
            .weightedOitOrderSeed = oit_.orderSeed(),
            .refractionPyramidsResident = forward_.pyramidResidency().enabled(),
            .texturesResident = resources_.textures().activeCount() -
                resources_.retiredTextureCount(),
            .texturesRetired = resources_.retiredTextureCount(),
            .samplersLive = resources_.liveSamplerCount(),
            .samplersCached = resources_.cachedSamplerCount(),
            .materialsResident = resources_.materials().activeCount(),
            .materialTableCapacity = resources_.materialTableCapacity(),
            .materialTableMaximumCapacity =
                resources_.materialTableMaximumCapacity(),
            .textureViewCapacity = table.frameCapacity(scheduler.currentFrameIndex()),
            .textureSamplerCapacity = table.frameCapacity(scheduler.currentFrameIndex()),
            .textureRequiredCapacity = table.requiredCapacity(),
            .textureMaximumCapacity = table.maximumCapacity(),
        });
    }

    FrameMemoryProfile VulkanVertexBackend::memorySnapshot() {
        FrameMemoryProfile result = resourceAllocator.memorySnapshot();
        const size_t categoryIndex = static_cast<size_t>(
            ProfileMemoryCategory::ExternalSwapchain);
        ProfileMemoryCategorySnapshot& external = result.categories[categoryIndex];
        const uint64_t requestedBytes = vkSwapchain
            ? swapchainRequestedBytes(*vkSwapchain)
            : 0;
        const uint64_t imageCount = vkSwapchain ? vkSwapchain->getImageCount() : 0;
        externalSwapchainRequestedPeakBytes_ = std::max(
            externalSwapchainRequestedPeakBytes_, requestedBytes);
        externalSwapchainPeakImageCount_ = std::max(
            externalSwapchainPeakImageCount_, imageCount);
        external.requestedLiveBytes = requestedBytes;
        external.requestedPeakBytes = externalSwapchainRequestedPeakBytes_;
        external.liveAllocationCount = imageCount;
        external.peakAllocationCount = externalSwapchainPeakImageCount_;
        external.requestedBytesAvailable = requestedBytes != 0;
        external.committedBytesAvailable = false;
        external.engineOwned = false;
        return result;
    }

} // namespace Iridium
