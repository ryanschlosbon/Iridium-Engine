#include "VulkanVertexBackend.h"
#include "VulkanProductionRenderGraph.h"
#include "VulkanGBufferLayout.h"
#include "renderer/color/SceneColor.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "renderer/rhi/LightUploadPlanner.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/lighting/ClusteredReflectionProbes.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"
#include "vendor/imguizmo/ImGuizmo.h"
#include "profiling/CpuProfiler.h"
#include <algorithm>
#include <stdexcept>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Iridium {

    namespace {
        constexpr std::array<std::string_view, 6> Hero4PassNames{
            "transparent.layered.hero4.interface.0.capture",
            "transparent.layered.hero4.interface.1.capture",
            "transparent.layered.hero4.interface.2.capture",
            "transparent.layered.hero4.interface.3.capture",
            "transparent.layered.hero4.local-compose",
            "transparent.layered.hero4.validation-readback-hook",
        };
        constexpr std::array<std::string_view, 10> Cinematic8PassNames{
            "transparent.layered.cinematic8.interface.0.capture",
            "transparent.layered.cinematic8.interface.1.capture",
            "transparent.layered.cinematic8.interface.2.capture",
            "transparent.layered.cinematic8.interface.3.capture",
            "transparent.layered.cinematic8.interface.4.capture",
            "transparent.layered.cinematic8.interface.5.capture",
            "transparent.layered.cinematic8.interface.6.capture",
            "transparent.layered.cinematic8.interface.7.capture",
            "transparent.layered.cinematic8.local-compose",
            "transparent.layered.cinematic8.validation-readback-hook",
        };
        constexpr std::array<const char*, 4> Hero4CaptureGpuRanges{
            "gpu.transparency.layered.hero4.interface.0.capture",
            "gpu.transparency.layered.hero4.interface.1.capture",
            "gpu.transparency.layered.hero4.interface.2.capture",
            "gpu.transparency.layered.hero4.interface.3.capture",
        };
        constexpr std::array<const char*, 8> Cinematic8CaptureGpuRanges{
            "gpu.transparency.layered.cinematic8.interface.0.capture",
            "gpu.transparency.layered.cinematic8.interface.1.capture",
            "gpu.transparency.layered.cinematic8.interface.2.capture",
            "gpu.transparency.layered.cinematic8.interface.3.capture",
            "gpu.transparency.layered.cinematic8.interface.4.capture",
            "gpu.transparency.layered.cinematic8.interface.5.capture",
            "gpu.transparency.layered.cinematic8.interface.6.capture",
            "gpu.transparency.layered.cinematic8.interface.7.capture",
        };
        constexpr std::array<const char*, 4> Hero4TerminationGpuRanges{
            "gpu.transparency.layered.hero4.interface.0.terminate-tiles",
            "gpu.transparency.layered.hero4.interface.1.terminate-tiles",
            "gpu.transparency.layered.hero4.interface.2.terminate-tiles",
            "gpu.transparency.layered.hero4.interface.3.terminate-tiles",
        };
        constexpr std::array<const char*, 8> Cinematic8TerminationGpuRanges{
            "gpu.transparency.layered.cinematic8.interface.0.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.1.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.2.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.3.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.4.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.5.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.6.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.7.terminate-tiles",
        };

        VkIndexType toVkIndexType(IndexFormat format) {
            switch (format) {
            case IndexFormat::UInt16:
                return VK_INDEX_TYPE_UINT16;
            case IndexFormat::UInt32:
                return VK_INDEX_TYPE_UINT32;
            }
            throw std::invalid_argument("Unsupported geometry index format.");
        }

        constexpr uint64_t FixedPipelineIdentityMask = uint64_t{ 1 } << 63;

        enum class FixedPipelineIdentity : uint64_t {
            GBufferWireframe = FixedPipelineIdentityMask | 1,
            SelectionMask = FixedPipelineIdentityMask | 2,
            DeferredLighting = FixedPipelineIdentityMask | 3,
            SelectionOutline = FixedPipelineIdentityMask | 4,
            GlassDepth = FixedPipelineIdentityMask | 5,
            ImGui = FixedPipelineIdentityMask | 6,
            OutputTransform = FixedPipelineIdentityMask | 7,
            LayeredInterfaceCapture = FixedPipelineIdentityMask | 8,
            LayeredLocalComposition = FixedPipelineIdentityMask | 9,
            LayeredSceneResolve = FixedPipelineIdentityMask | 10,
            LayeredResidualComposition = FixedPipelineIdentityMask | 11,
            LayeredTileTermination = FixedPipelineIdentityMask | 12,
        };

        constexpr uint64_t pipelineIdentity(FixedPipelineIdentity identity) noexcept {
            return static_cast<uint64_t>(identity);
        }

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

        uint32_t captureSourceBytesPerPixel(VkFormat format) noexcept {
            switch (format) {
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return 8;
            default:
                return 0;
            }
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
			}
			return "unknown";
		}
    }

    // ==============================================================================
    // 1. SYSTEM LIFECYCLE
    // ==============================================================================

    void VulkanVertexBackend::init(GLFWwindow* window, const RenderBackendConfig& config) {
        if (initialized_) {
            throw std::logic_error("VulkanVertexBackend was initialized more than once.");
        }

        cpuProfiler_ = config.cpuProfiler;
        gBufferLayout_ = config.gBufferLayout;
        if ((config.clusterTileSize != 16 && config.clusterTileSize != 32) ||
            (config.clusterDepthSlices != 24 &&
                config.clusterDepthSlices != 32)) {
            throw std::invalid_argument(
                "Cluster bake-off supports 16/32 pixel tiles and 24/32 slices");
        }
        clusterConfig_.tileWidth = config.clusterTileSize;
        clusterConfig_.tileHeight = config.clusterTileSize;
        clusterConfig_.depthSlices = config.clusterDepthSlices;
        manualExposureEv_ = static_cast<float>(config.manualExposureEv);
        outputOperator_ = config.outputOperator;
		outputTransport_ = config.outputTransport;
        paperWhiteNits_ = static_cast<float>(config.paperWhiteNits);
        peakNits_ = static_cast<float>(config.peakNits);
        directionalShadowResolution_ = config.directionalShadowResolution;
        spotShadowAtlasResolution_ = config.spotShadowAtlasResolution;
        pointShadowCapacities_ = {
            config.pointShadowPool256Capacity,
            config.pointShadowPool512Capacity,
            config.pointShadowPool1024Capacity };
        configureReflectionProbeCaptures(config.reflectionProbeSettings);
        if (std::ranges::any_of(pointShadowCapacities_,
                [](uint32_t value) { return value == 0u; }) ||
            pointShadowCapacities_[0] > kPointShadowPool256Capacity ||
            pointShadowCapacities_[1] > kPointShadowPool512Capacity ||
            pointShadowCapacities_[2] > kPointShadowPool1024Capacity)
            throw std::invalid_argument(
                "Point shadow pool capacity exceeds the GPU table contract");
        if (cpuProfiler_ != nullptr && cpuProfiler_->isEnabled()) {
            uniqueMaterialIds_.reserve(MaxUniqueResourcesPerFrame);
            uniquePipelineIds_.reserve(MaxUniqueResourcesPerFrame);
        }
        vkContext = std::make_unique<VkContext>(config.enableValidation,
            config.enableGpuProfiling,
            config.enableTransparentPipelineStatistics, window);
        if (!vkContext->hasDescriptorIndexing()) {
            throw std::runtime_error(
                "Indexed material descriptors are required for the production path, "
                "but the Vulkan device lacks the complete descriptor-indexing feature set.");
        }
        resourceAllocator.init(vkContext->getPhysicalDevice(), vkContext->getDevice(),
            vkContext->hasMemoryBudget());
        uploadContext.init(vkContext->getDevice(), vkContext->getGraphicsQueue(),
            vkContext->getGraphicsQueueFamily(), resourceAllocator, cpuProfiler_);
		vkSwapchain = std::make_unique<VkSwapchain>(vkContext.get(), window,
			outputTransport_);
		sceneExtent_ = vkSwapchain->getExtent();
		outputTransport_ = vkSwapchain->getOutputTransportSelection().effective;
		vkSwapchain->setHdrMetadata(peakNits_);
		outputTargetFormat_ = outputTransport_ == Color::OutputTransport::SdrSrgb
			? VulkanSdrOutputFormat : VK_FORMAT_R16G16B16A16_SFLOAT;
        scheduler.init(vkContext->getDevice(), vkContext->getGraphicsQueue(),
            vkContext->getPresentQueue(), vkContext->getGraphicsQueueFamily(),
            vkSwapchain->getImageCount(), cpuProfiler_, config.enableGpuProfiling,
            vkContext->getTimestampPeriodNanoseconds(),
            vkContext->getTimestampValidBits(), vkContext->hasDebugUtils(),
            config.enableTransparentPipelineStatistics &&
                vkContext->hasPipelineStatistics(),
            static_cast<uint64_t>(sceneExtent_.width) *
                sceneExtent_.height);

        descriptorAllocator.init(vkContext->getDevice());
        // Keep initial driver allocation modest; the per-frame tables grow
        // geometrically at fence-safe frame boundaries.
        constexpr uint32_t DesiredCapacity = 64;
        const uint32_t poolLimit =
            vkContext->getMaxUpdateAfterBindDescriptors();
        constexpr uint32_t PackedSamplerMaximumCapacity = 0xffffu;
        const uint32_t maximumCapacity = (std::min)({
            vkContext->getMaxIndexedTextureViews(),
            vkContext->getMaxIndexedSamplers(),
            // Leave room for one fence-safe replacement pool to coexist
            // briefly with both active per-frame pools during growth.
            poolLimit / ((VulkanIndexedTextureTable::FrameSetCount + 1u) * 2u),
            PackedSamplerMaximumCapacity,
        });
        const uint32_t initialCapacity =
            (std::min)(DesiredCapacity, maximumCapacity);
        if (initialCapacity >= 2) {
            indexedTextureTable_.init(vkContext->getDevice(),
                initialCapacity, maximumCapacity);
        } else {
            throw std::runtime_error(
                "Vulkan update-after-bind descriptor limits are below "
                "Iridium's minimum indexed material table.");
        }

        // 2. Lighting and forward pass contracts needed by the shared mesh layouts.
        createLightingRenderPass();
        lightingPipeline = std::make_unique<VkLightingPipeline>(vkContext.get(),
            lightingRenderPass, gBufferLayout_);
        clusteredLighting_.init(vkContext->getDevice(), descriptorAllocator);
        reflectionProbePipeline_.init(vkContext->getDevice(),
            descriptorAllocator);
        reflectionProbeCapturePass_.init(vkContext->getDevice(),
            vkContext->getPhysicalDevice(), resourceAllocator,
            descriptorAllocator, indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            lightingPipeline->getDescriptorSetLayout());
        reflectionProbeCaptureTargets_.init(vkContext->getDevice(),
            vkContext->getPhysicalDevice(), resourceAllocator,
            reflectionProbeCapturePass_.renderPass());
        if (config.validateReflectionProbeCaptureTargets) {
            const SceneEntityUuid validationOwner = *SceneEntityUuid::parse(
                "019fb73d-5a80-7000-8000-000000000999");
            const auto& validationTarget =
                reflectionProbeCaptureTargets_.acquire(
                    validationOwner, 1, 128);
            if (!validationTarget.rawRadiance.isValid() ||
                !validationTarget.depth.isValid() ||
                !validationTarget.prefilteredRadiance.isValid())
                throw std::runtime_error(
                    "Reflection-probe validation capture allocation failed");
            reflectionProbeCaptureTargets_.promote(validationOwner, 1);
            if (reflectionProbeCaptureTargets_.capturesInFlight() != 0 ||
                reflectionProbeCaptureTargets_.stagingLogicalBytes() != 0 ||
                reflectionProbeCaptureTargets_.publishedCount() != 1 ||
                reflectionProbeCaptureTargets_.published(validationOwner) == nullptr)
                throw std::runtime_error(
                    "Reflection-probe validation capture promotion failed");
            [[maybe_unused]] const auto& validationRefreshTarget =
                reflectionProbeCaptureTargets_.acquire(
                    validationOwner, 2, 128);
            reflectionProbeCaptureTargets_.abandon(validationOwner, 2);
            if (reflectionProbeCaptureTargets_.capturesInFlight() != 0 ||
                reflectionProbeCaptureTargets_.stagingLogicalBytes() != 0 ||
                reflectionProbeCaptureTargets_.publishedCount() != 1 ||
                reflectionProbeCaptureTargets_.published(validationOwner) == nullptr)
                throw std::runtime_error(
                    "Reflection-probe validation refresh retirement failed");
            reflectionProbeCaptureTargets_.remove(validationOwner);
            if (reflectionProbeCaptureTargets_.publishedCount() != 0 ||
                reflectionProbeCaptureTargets_.publishedLogicalBytes() != 0)
                throw std::runtime_error(
                    "Reflection-probe validation owner retirement failed");
        }
        forwardPass = std::make_unique<VkForwardRenderPass>(vkContext.get(),
            VulkanSceneColorFormat, VK_FORMAT_D32_SFLOAT);
        transparentPass = std::make_unique<VkForwardRenderPass>(vkContext.get(),
            VulkanSceneColorFormat, VK_FORMAT_D32_SFLOAT, true);
		outputPass.init(*vkContext, descriptorAllocator, outputTargetFormat_);
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            hdrEncodePass.init(*vkContext, descriptorAllocator,
                vkSwapchain->getImageFormat());
        }
        meshLayouts.init(vkContext->getDevice(),
            lightingPipeline->getDescriptorSetLayout(),
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout());
        layeredInterfaceCapture_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout(),
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout());
        layeredLocalComposition_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout(),
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            lightingPipeline->getDescriptorSetLayout());
        layeredSceneResolve_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout(),
            transparentPass->getRenderPass());
        transparencyPyramid_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout());
        directionalShadow_.init(vkContext->getDevice(), resourceAllocator,
            uploadContext, descriptorAllocator,
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            directionalShadowResolution_);
        if (vkContext->getPhysicalDeviceProperties().limits.maxUniformBufferRange <
            sizeof(VulkanSpotShadowData)) {
            throw std::runtime_error(
                "Vulkan uniform-buffer range cannot hold the spot shadow table");
        }
        spotShadow_.init(vkContext->getDevice(), resourceAllocator,
            uploadContext, descriptorAllocator,
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            spotShadowAtlasResolution_);
        if (vkContext->getPhysicalDeviceProperties().limits.maxUniformBufferRange <
            sizeof(VulkanPointShadowData)) {
            throw std::runtime_error(
                "Vulkan uniform-buffer range cannot hold the point shadow table");
        }
        pointShadow_.init(vkContext->getDevice(), resourceAllocator,
            uploadContext, descriptorAllocator,
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(), pointShadowCapacities_);

        // 3. G-Buffer Pass
        gBufferPass = std::make_unique<VkRenderPassWrapper>(vkContext.get(),
            vkSwapchain.get(), gBufferLayout_);
        gBufferPipeline = std::make_unique<VkGraphicsPipeline>(vkContext.get(), vkSwapchain.get(), gBufferPass.get(),
            meshLayouts.getGBufferPipelineLayout(), gBufferLayout_);

        // 4. Glass Depth Pass
        glassDepthPass = std::make_unique<GlassDepthRenderPass>();
        glassDepthPass->init(vkContext->getDevice(), VK_FORMAT_D32_SFLOAT);

        glassDepthPipeline = std::make_unique<GlassDepthPipeline>();
        glassDepthPipeline->init(vkContext->getDevice(), glassDepthPass->getRenderPass(),
            meshLayouts.getGBufferPipelineLayout(),
            std::string(PROJECT_ROOT_DIR) + "assets/shaders/glass_depth_vert.spv");

        pipelineLibrary.init(vkContext->getDevice(),
            { gBufferPass->getRenderPass(), meshLayouts.getGBufferPipelineLayout(),
                vulkanGBufferFormats(gBufferLayout_).colorAttachmentCount },
            { forwardPass->getRenderPass(), meshLayouts.getForwardPipelineLayout(), 1 },
            { transparentPass->getRenderPass(),
                meshLayouts.getForwardPipelineLayout(), 1 },
            gBufferLayout_);

        // 5. UI Pass
        const bool hdr10Composition = outputTransport_ ==
            Color::OutputTransport::Hdr10Pq;
        uiPass = std::make_unique<VkUIRenderPass>(vkContext.get(),
            hdr10Composition ? VK_FORMAT_R16G16B16A16_SFLOAT
                : vkSwapchain->getImageFormat(), !hdr10Composition);

        // 7. Render Targets
        rebuildRenderGraphAfterDeviceIdle();
        initFrameTargets();
        if (hdr10Composition) {
            hdrEncodePass.rebuild(frameTargets, vkSwapchain->getImageViews(),
                vkSwapchain->getExtent());
        }
        // Target descriptors declare shader-read layouts, so submit their initial
        // Undefined -> ShaderResource transitions before any descriptor or ImGui
        // registration can reference those images.
        createNeutralEnvironmentProducts();
        uploadContext.flush();

        // 8. Global Camera Buffers
        createUniformBuffers();
        canonicalMaterialMaximumCapacity_ = (std::min)(
            static_cast<uint32_t>(
                vkContext->getPhysicalDeviceProperties()
                    .limits.maxStorageBufferRange /
                sizeof(PackedGpuMaterial)),
            MaterialHandle::MaxIndex + 1u);
        createCanonicalMaterialBuffers(
            (std::min)(DesiredCapacity,
                canonicalMaterialMaximumCapacity_));
        lightRecordMaximumCapacity_ = (std::min)(
            static_cast<uint32_t>(
                vkContext->getPhysicalDeviceProperties()
                    .limits.maxStorageBufferRange /
                sizeof(PackedGpuLight)),
            kMaximumGpuLightCapacity);
        if (lightRecordMaximumCapacity_ == 0) {
            throw std::runtime_error(
                "Vulkan storage-buffer range cannot hold one GPU light record");
        }
        createLightRecordBuffers((std::min)(kInitialGpuLightCapacity,
            lightRecordMaximumCapacity_));
        reflectionProbeRecordMaximumCapacity_ = (std::min)(
            static_cast<uint32_t>(
                vkContext->getPhysicalDeviceProperties()
                    .limits.maxStorageBufferRange /
                sizeof(PackedGpuReflectionProbe)),
            kMaximumGpuReflectionProbeCapacity);
        if (reflectionProbeRecordMaximumCapacity_ == 0)
            throw std::runtime_error(
                "Vulkan storage-buffer range cannot hold one reflection probe");
        const ClusterGridDimensions initialProbeGrid = clusterGridDimensions(
            clusterConfig_, { sceneExtent_.width, sceneExtent_.height,
                0.1f, 100.0f, glm::mat4(1.0f), glm::mat4(1.0f) });
        const uint32_t initialProbeClusters = static_cast<uint32_t>(
            initialProbeGrid.clusterCount());
        const uint32_t initialProbeReferences = static_cast<uint32_t>(
            (std::min)(initialProbeGrid.clusterCount() *
                kMaximumReflectionProbesPerCluster,
                static_cast<uint64_t>(kMaximumClusterProbeReferences)));
        createReflectionProbeBuffers((std::min)(
            kInitialGpuReflectionProbeCapacity,
            reflectionProbeRecordMaximumCapacity_),
            initialProbeClusters, initialProbeReferences);

        // --------------------------------

        // 2. Global Descriptor Sets (Camera Data)
        globalDescriptorSets.resize(VulkanFrameScheduler::FramesInFlight);
        for (size_t i = 0; i < VulkanFrameScheduler::FramesInFlight; i++) {
            globalDescriptorSets[i] = descriptorAllocator.allocate(meshLayouts.getGlobalSetLayout());

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkWriteDescriptorSet descriptorWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            descriptorWrite.dstSet = globalDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(vkContext->getDevice(), 1, &descriptorWrite, 0, nullptr);
        }
        transparencyPyramid_.rebuild(frameTargets);
        layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
        layeredLocalComposition_.rebuildDescriptors(frameTargets);
        layeredSceneResolve_.rebuildDescriptors(frameTargets);

        // 3. Lighting descriptors (one set per frame context).
        const uint32_t imgCount = vkSwapchain->getImageCount();
        sceneDescriptors.init(vkContext->getDevice(), descriptorAllocator,
            lightingPipeline->getDescriptorSetLayout());
        bindLightRecordBuffers();
        bindSceneClusterBuffers();
        bindEnvironmentProducts();
        bindDirectionalShadowDescriptors();
        bindSpotShadowDescriptors();
        bindPointShadowDescriptors();
        bindReflectionProbeBuffers();
        bindReflectionProbeEnvironments();
        sceneDescriptors.rebuild(frameTargets);
        if (VulkanTexturePayload* lut = textureVault.get(outputTransformLut_);
            lut != nullptr && !lut->retired) {
            outputPass.rebuildDescriptors(frameTargets, lut->image.view, lut->sampler);
        }
        else {
            outputPass.rebuildDescriptors(frameTargets);
        }

        // 4. ImGui Initialization & UI Textures
        // Create a small pool specifically for ImGui's internal fonts and textures
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096} };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = pool_sizes;
        vkCreateDescriptorPool(vkContext->getDevice(), &pool_info, nullptr, &imguiPool);

        // Init ImGui contexts
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui_ImplGlfw_InitForVulkan(window, true);

        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = vkContext->getInstance();
        init_info.PhysicalDevice = vkContext->getPhysicalDevice();
        init_info.Device = vkContext->getDevice();
        init_info.QueueFamily = vkContext->getGraphicsQueueFamily();
        init_info.Queue = vkContext->getGraphicsQueue();
        init_info.PipelineCache = VK_NULL_HANDLE;
        init_info.DescriptorPool = imguiPool;
        init_info.MinImageCount = imgCount;
        init_info.ImageCount = imgCount;
        init_info.PipelineInfoMain.RenderPass = uiPass->getRenderPass();
        const std::vector<char> imguiFragmentBytes = readFile(
            std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/imgui_color_managed_frag.spv");
        if (imguiFragmentBytes.empty() ||
            imguiFragmentBytes.size() % sizeof(uint32_t) != 0) {
            throw std::runtime_error("Color-managed ImGui shader is invalid.");
        }
        imguiFragmentShaderCode_.resize(
            imguiFragmentBytes.size() / sizeof(uint32_t));
        std::memcpy(imguiFragmentShaderCode_.data(), imguiFragmentBytes.data(),
            imguiFragmentBytes.size());
        init_info.CustomShaderFragCreateInfo = {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        init_info.CustomShaderFragCreateInfo.codeSize =
            imguiFragmentShaderCode_.size() * sizeof(uint32_t);
        init_info.CustomShaderFragCreateInfo.pCode =
            imguiFragmentShaderCode_.data();
        init_info.DisplayColorScale = outputTransport_ ==
            Color::OutputTransport::ScRgb ? paperWhiteNits_ / 80.0f : 1.0f;
        init_info.OutputColorSpace = outputTransport_ ==
            Color::OutputTransport::Hdr10Pq ? 1u : 0u;
        ImGui_ImplVulkan_Init(&init_info);
        imguiInitialized_ = true;

        // Create the initial ImGui textures for the viewport!
        const size_t frameTargetCount = frameTargets.size();
        uiSceneTextures.resize(frameTargetCount);
        uiDepthTextures.resize(frameTargetCount);
        for (size_t i = 0; i < frameTargetCount; i++) {
            const VulkanFrameContextTargets& targets = frameTargets.get(i);
            uiSceneTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(),
                targets.output.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(),
                targets.glassDepth.view,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }

        initialized_ = true;
        cleaned_ = false;
    }
    
    void VulkanVertexBackend::setEnvironmentLighting(
        const EnvironmentLightingHandles& environment) {
        if (!environment.isValid())
            throw std::invalid_argument(
                "Environment lighting requires four valid texture handles.");
        const VulkanTexturePayload* radiance = textureVault.get(environment.radiance);
        const VulkanTexturePayload* irradiance = textureVault.get(environment.irradiance);
        const VulkanTexturePayload* prefiltered =
            textureVault.get(environment.prefilteredSpecular);
        const VulkanTexturePayload* brdf = textureVault.get(environment.brdfLut);
        if (radiance == nullptr || irradiance == nullptr || prefiltered == nullptr ||
            brdf == nullptr || radiance->retired || irradiance->retired ||
            prefiltered->retired || brdf->retired ||
            radiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            irradiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            prefiltered->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            brdf->image.viewType != VK_IMAGE_VIEW_TYPE_2D ||
            radiance->format != TextureFormat::RGBA16_SFloat ||
            irradiance->format != TextureFormat::RGBA16_SFloat ||
            prefiltered->format != TextureFormat::RGBA16_SFloat ||
            brdf->format != TextureFormat::RG16_SFloat) {
            throw std::invalid_argument(
                "Environment lighting textures do not match the cube/LUT contract.");
        }
        // Environment publication is rare and updates every per-frame scene set.
        // Wait until none of those sets are referenced by pending command buffers;
        // the descriptors were intentionally not created update-after-bind.
        scheduler.waitForAllFrames();
        environmentLighting_ = environment;
        for (VulkanTexturePayload* payload : {
                textureVault.get(environment.radiance),
                textureVault.get(environment.irradiance),
                textureVault.get(environment.prefilteredSpecular),
                textureVault.get(environment.brdfLut) })
            resourceAllocator.reclassify(payload->image,
                ProfileMemoryCategory::Environment);
        bindEnvironmentProducts();
    }

    void VulkanVertexBackend::setEnvironmentLightingSettings(
        const EnvironmentLightingSettings& settings) {
        if (!std::isfinite(settings.lightingIntensity) ||
            settings.lightingIntensity < 0.0f ||
            !std::isfinite(settings.backgroundIntensity) ||
            settings.backgroundIntensity < 0.0f ||
            !std::isfinite(settings.rotationRadians)) {
            throw std::invalid_argument(
                "Environment lighting settings must be finite and nonnegative.");
        }
        environmentLightingSettings_ = settings;
    }

    void VulkanVertexBackend::createNeutralEnvironmentProducts() {
        if (neutralEnvironmentCube_.isValid() ||
            neutralEnvironmentBrdfLut_.isValid()) {
            throw std::logic_error(
                "Neutral environment products were initialized twice.");
        }

        TextureDesc cubeDesc{};
        cubeDesc.width = 1;
        cubeDesc.height = 1;
        cubeDesc.format = TextureFormat::RGBA16_SFloat;
        cubeDesc.usageClass = TextureUsageClass::Environment;
        cubeDesc.arrayLayers = 6;
        cubeDesc.topology = TextureTopology::Cube;
        cubeDesc.sampler.addressU = SamplerAddressMode::ClampToEdge;
        cubeDesc.sampler.addressV = SamplerAddressMode::ClampToEdge;
        cubeDesc.sampler.addressW = SamplerAddressMode::ClampToEdge;

        // Six layer-major RGBA16F black texels. The same semantic neutral cube
        // is safe for irradiance, prefiltered radiance, and sky radiance.
        const std::array<std::byte, 6u * 4u * sizeof(uint16_t)> blackCube{};
        neutralEnvironmentCube_ = allocateTexture(cubeDesc, blackCube);

        TextureDesc brdfDesc{};
        brdfDesc.width = 1;
        brdfDesc.height = 1;
        brdfDesc.format = TextureFormat::RG16_SFloat;
        brdfDesc.usageClass = TextureUsageClass::Environment;
        brdfDesc.sampler.addressU = SamplerAddressMode::ClampToEdge;
        brdfDesc.sampler.addressV = SamplerAddressMode::ClampToEdge;
        brdfDesc.sampler.addressW = SamplerAddressMode::ClampToEdge;
        // Half-float (1, 0) is the identity split-sum fallback: F0 * 1 + F90 * 0.
        const std::array<uint16_t, 2> brdfIdentity{ 0x3c00u, 0u };
        neutralEnvironmentBrdfLut_ = allocateTexture(
            brdfDesc, std::as_bytes(std::span{ brdfIdentity }));
    }

    void VulkanVertexBackend::bindEnvironmentProducts() {
        const EnvironmentLightingHandles handles = environmentLighting_.isValid()
            ? environmentLighting_
            : EnvironmentLightingHandles{
                .radiance = neutralEnvironmentCube_,
                .irradiance = neutralEnvironmentCube_,
                .prefilteredSpecular = neutralEnvironmentCube_,
                .brdfLut = neutralEnvironmentBrdfLut_,
            };
        const VulkanTexturePayload* radiance = textureVault.get(handles.radiance);
        const VulkanTexturePayload* irradiance = textureVault.get(handles.irradiance);
        const VulkanTexturePayload* prefiltered =
            textureVault.get(handles.prefilteredSpecular);
        const VulkanTexturePayload* brdf = textureVault.get(handles.brdfLut);
        if (radiance == nullptr || irradiance == nullptr || prefiltered == nullptr ||
            brdf == nullptr || radiance->retired || irradiance->retired ||
            prefiltered->retired || brdf->retired ||
            radiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            irradiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            prefiltered->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            brdf->image.viewType != VK_IMAGE_VIEW_TYPE_2D) {
            throw std::logic_error(
                "Neutral environment products are unavailable or incompatible.");
        }
        const VkDescriptorImageInfo radianceInfo{ radiance->sampler,
            radiance->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo irradianceInfo{ irradiance->sampler,
            irradiance->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prefilteredInfo{ prefiltered->sampler,
            prefiltered->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo brdfInfo{ brdf->sampler, brdf->image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        sceneDescriptors.setEnvironmentImages({
            .irradiance = irradianceInfo,
            .prefilteredRadiance = prefilteredInfo,
            .brdfLut = brdfInfo,
            .skyRadiance = radianceInfo,
        });
    }

    void VulkanVertexBackend::bindDirectionalShadowDescriptors() {
        std::vector<VkDescriptorBufferInfo> frameData;
        frameData.reserve(VulkanFrameScheduler::FramesInFlight);
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            frameData.push_back(directionalShadow_.sampleBuffer(frame));
        sceneDescriptors.setDirectionalShadow({
            directionalShadow_.sampleImage(), std::move(frameData) });
    }

    void VulkanVertexBackend::bindSpotShadowDescriptors() {
        std::vector<VkDescriptorBufferInfo> frameData;
        frameData.reserve(VulkanFrameScheduler::FramesInFlight);
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            frameData.push_back(spotShadow_.sampleBuffer(frame));
        sceneDescriptors.setSpotShadow({
            spotShadow_.sampleImage(), std::move(frameData) });
    }

    void VulkanVertexBackend::bindPointShadowDescriptors() {
        std::vector<VkDescriptorBufferInfo> frameData;
        frameData.reserve(VulkanFrameScheduler::FramesInFlight);
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            frameData.push_back(pointShadow_.sampleBuffer(frame));
        sceneDescriptors.setPointShadow({ pointShadow_.sampleImages(),
            std::move(frameData) });
    }

    void VulkanVertexBackend::setOutputTransformLut(TextureHandle lutHandle) {
        VulkanTexturePayload* payload = textureVault.get(lutHandle);
        if (payload == nullptr || payload->retired ||
            payload->format != TextureFormat::RGBA32_SFloat ||
            payload->width != 16384 || payload->height != 128) {
            throw std::invalid_argument(
                "ACES 2 output LUT must be the pinned 128^3 RGBA32F asset.");
        }
        scheduler.waitForAllFrames();
        outputTransformLut_ = lutHandle;
        outputPass.rebuildDescriptors(frameTargets, payload->image.view,
            payload->sampler);
    }

    void VulkanVertexBackend::setOutputSettings(float manualExposureEv,
        float paperWhiteNits, float peakNits) {
        if (!std::isfinite(manualExposureEv) || manualExposureEv < -16.0f ||
            manualExposureEv > 16.0f || !std::isfinite(paperWhiteNits) ||
            paperWhiteNits < 80.0f || paperWhiteNits > 1000.0f ||
            !std::isfinite(peakNits) || peakNits < paperWhiteNits ||
            peakNits > 10000.0f) {
            throw std::invalid_argument("Live output settings are outside supported bounds.");
        }
        manualExposureEv_ = manualExposureEv;
        paperWhiteNits_ = paperWhiteNits;
        peakNits_ = peakNits;
        if (vkSwapchain) vkSwapchain->setHdrMetadata(peakNits_);
        if (imguiInitialized_) {
            ImGui_ImplVulkan_SetDisplayColorConfiguration(
                outputTransport_ == Color::OutputTransport::ScRgb
                    ? paperWhiteNits_ / 80.0f : 1.0f,
                outputTransport_ == Color::OutputTransport::Hdr10Pq ? 1u : 0u);
        }
    }

    void VulkanVertexBackend::cleanup() {
        if (!initialized_ || cleaned_) {
            return;
        }
        cleaned_ = true;

        const VkDevice device = vkContext->getDevice();
        vkDeviceWaitIdle(device);
        uploadContext.flush();
        scheduler.waitForAllFrames();
        destroyPendingFrameCaptures();
        completedFrameCaptures_.clear();
        destroyPendingOrdinary2CaptureValidations();
        completedOrdinary2CaptureValidations_.clear();
        ordinary2CaptureValidationRequest_.reset();
        destroyPendingDeepLayeredCaptureValidations();
        completedDeepLayeredCaptureValidations_.clear();
        deepLayeredCaptureValidationRequest_.reset();

        pipelineLibrary.cleanup();

        for (VkDescriptorSet texture : uiSceneTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        for (VkDescriptorSet texture : uiDepthTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        uiSceneTextures.clear();
        uiDepthTextures.clear();
        if (imguiInitialized_) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            imguiInitialized_ = false;
        }
        imguiFragmentShaderCode_.clear();
        if (imguiPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, imguiPool, nullptr);
            imguiPool = VK_NULL_HANDLE;
        }

        sceneDescriptors.cleanup();
        clusteredLighting_.clearDescriptors();
        reflectionProbePipeline_.clearDescriptors();
        outputPass.clearDescriptors();
        hdrEncodePass.clearTargets();
        transparencyPyramid_.clearDescriptors();
        layeredInterfaceCapture_.clearDescriptors();
        layeredLocalComposition_.clearDescriptors();
        layeredSceneResolve_.clearDescriptors();
        frameTargets.cleanup();
        renderGraph_.cleanupAfterDeviceIdle();
        directionalShadow_.cleanup();
        spotShadow_.cleanup();
        pointShadow_.cleanup();
        for (PendingReflectionProbeCapture& pending :
                pendingReflectionProbeCaptures_) {
            reflectionProbeCapturePass_.releaseDescriptors(
                pending.filterDescriptors);
            resourceAllocator.destroy(pending.bakedReadback.buffer);
        }
        pendingReflectionProbeCaptures_.clear();
        reflectionProbeCaptureTargets_.cleanup();
        reflectionProbeCapturePass_.cleanup();

        geometryVault.forEach([this](VulkanGeometryPayload& payload) {
            resourceAllocator.destroy(payload.vertexBuffer);
            resourceAllocator.destroy(payload.indexBuffer);
            });

        textureVault.forEach([this](VulkanTexturePayload& payload) {
            if (!payload.retired) resourceAllocator.destroy(payload.image);
            });
        cleanupSamplerCache();

        for (size_t i = 0; i < uniformBuffers.size(); i++) {
            resourceAllocator.destroy(uniformBuffers[i]);
        }
        for (VulkanBufferResource& buffer : canonicalMaterialBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : lightRecordBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : activeLightSlotBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : fallbackCandidateBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : clusterParameterBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : clusterDiagnosticReadbackBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : reflectionProbeRecordBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : reflectionProbeActiveSlotBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : reflectionProbeParameterBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer :
                reflectionProbeClusterHeaderBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer :
                reflectionProbeClusterIndexBuffers_)
            resourceAllocator.destroy(buffer);

        if (glassDepthPipeline) {
            glassDepthPipeline->cleanup();
            glassDepthPipeline.reset();
        }
        if (glassDepthPass) {
            glassDepthPass->cleanup();
            glassDepthPass.reset();
        }

        forwardPass.reset();
        transparentPass.reset();

        lightingPipeline.reset();
        if (lightingRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, lightingRenderPass, nullptr);
            lightingRenderPass = VK_NULL_HANDLE;
        }

        uiPass.reset();
        hdrEncodePass.cleanup();
        outputPass.cleanup();
        gBufferPipeline.reset();
        gBufferPass.reset();

        transparencyPyramid_.cleanup();
        layeredSceneResolve_.cleanup();
        layeredLocalComposition_.cleanup();
        layeredInterfaceCapture_.cleanup();
        meshLayouts.cleanup();
        indexedTextureTable_.cleanup();
        clusteredLighting_.cleanup();
        reflectionProbePipeline_.cleanup();
        descriptorAllocator.cleanup();

        scheduler.cleanup();
        uploadContext.cleanup();
        resourceAllocator.cleanup();

        vkSwapchain.reset();
        vkContext.reset();
        initialized_ = false;
        frameOpen_ = false;
        transparencyPyramidResidency_.restore(false);
        ordinary2AtlasResidency_.restore(false);
        ordinary2AtlasExtent_ = {};
        frameTopologyPrewarm_ = {};
        cpuProfiler_ = nullptr;
        collectFrameCounters_ = false;
        uniqueMaterialIds_.clear();
        uniquePipelineIds_.clear();
        manualExposureEv_ = 0.0f;
        outputOperator_ = OutputTransformOperator::Aces2;
        canonicalMaterialCapacity_ = 0;
        canonicalMaterialMaximumCapacity_ = 0;
        retiredTextureCount_ = 0;
        environmentLighting_ = {};
        reflectionProbeEnvironments_.clear();
        capturedReflectionProbeSlots_.clear();
        reflectionProbeCaptureTelemetry_ = {};
        reflectionProbeRecordCapacity_ = 0;
        reflectionProbeRecordMaximumCapacity_ = 0;
        reflectionProbeClusterCapacity_ = 0;
        reflectionProbeReferenceCapacity_ = 0;
        neutralEnvironmentCube_ = {};
        neutralEnvironmentBrdfLut_ = {};
        outputTransformLut_ = {};
        finalCaptureHookRecorded_ = false;
    }

    void VulkanVertexBackend::resetFrameCounters() {
        frameCounters_ = {};
        uniqueMaterialIds_.clear();
        uniquePipelineIds_.clear();
    }

    void VulkanVertexBackend::recordMaterialBind(MaterialHandle material) {
        if (!collectFrameCounters_) {
            return;
        }
        ++frameCounters_.materialBinds;
        const uint32_t identity = material.id;
        if (std::find(uniqueMaterialIds_.begin(), uniqueMaterialIds_.end(), identity) !=
            uniqueMaterialIds_.end()) {
            return;
        }
        if (uniqueMaterialIds_.size() >= MaxUniqueResourcesPerFrame) {
            ++frameCounters_.materialUniqueOverflow;
            return;
        }
        uniqueMaterialIds_.push_back(identity);
    }

    void VulkanVertexBackend::recordPipelineBind(uint64_t pipelineIdentityValue) {
        if (!collectFrameCounters_) {
            return;
        }
        ++frameCounters_.pipelineBinds;
        if (std::find(uniquePipelineIds_.begin(), uniquePipelineIds_.end(),
            pipelineIdentityValue) != uniquePipelineIds_.end()) {
            return;
        }
        if (uniquePipelineIds_.size() >= MaxUniqueResourcesPerFrame) {
            ++frameCounters_.pipelineUniqueOverflow;
            return;
        }
        uniquePipelineIds_.push_back(pipelineIdentityValue);
    }

    void VulkanVertexBackend::recordDraw(uint64_t& drawCounter,
        uint64_t submittedTriangles) {
        if (!collectFrameCounters_) {
            return;
        }
        ++drawCounter;
        frameCounters_.trianglesSubmitted += submittedTriangles;
    }

    void VulkanVertexBackend::emitFrameCounters() {
        if (!collectFrameCounters_ || cpuProfiler_ == nullptr) {
            return;
        }

        const uint64_t transparentDraws = frameCounters_.drawTransparentDepth +
            frameCounters_.drawTransparentForward +
            frameCounters_.ordinary2CaptureEntryDraws +
            frameCounters_.ordinary2CaptureExitDraws +
            frameCounters_.ordinary2LocalCompositionDraws +
            frameCounters_.ordinary2SceneResolveDraws +
            frameCounters_.deepLayeredInterfaceDraws +
            frameCounters_.deepLayeredResidualProbeDraws +
            frameCounters_.deepLayeredLocalCompositionDraws +
            frameCounters_.deepLayeredSceneResolveDraws;
        const uint64_t totalDraws = frameCounters_.drawOpaque +
            frameCounters_.drawSelection +
            frameCounters_.drawShadowDirectional +
            frameCounters_.drawShadowSpot +
            frameCounters_.drawShadowPoint +
            frameCounters_.drawLighting +
            frameCounters_.drawOutput +
            transparentDraws + frameCounters_.drawUi;
        const ProfileCounterStatus uiAwareStatus = frameCounters_.uiUntrackedCallbacks == 0
            ? ProfileCounterStatus::Exact
            : ProfileCounterStatus::Estimated;
        const ProfileCounterStatus materialUniqueStatus =
            frameCounters_.materialUniqueOverflow == 0
            ? ProfileCounterStatus::Exact
            : ProfileCounterStatus::Estimated;
        const ProfileCounterStatus pipelineUniqueStatus =
            frameCounters_.pipelineUniqueOverflow == 0
            ? ProfileCounterStatus::Exact
            : ProfileCounterStatus::Estimated;

        cpuProfiler_->recordCounter("draw.recorded.opaque", frameCounters_.drawOpaque);
        cpuProfiler_->recordCounter("draw.recorded.selection", frameCounters_.drawSelection);
        cpuProfiler_->recordCounter("draw.recorded.shadow.directional",
            frameCounters_.drawShadowDirectional);
        cpuProfiler_->recordCounter("draw.recorded.shadow.directional.alpha_mask",
            frameCounters_.drawShadowDirectionalAlphaMask);
        cpuProfiler_->recordCounter("draw.recorded.shadow.spot",
            frameCounters_.drawShadowSpot);
        cpuProfiler_->recordCounter("draw.recorded.shadow.spot.alpha_mask",
            frameCounters_.drawShadowSpotAlphaMask);
        cpuProfiler_->recordCounter("shadow.spot.casters.tested",
            frameCounters_.shadowSpotCastersTested);
        cpuProfiler_->recordCounter("shadow.spot.casters.culled",
            frameCounters_.shadowSpotCastersCulled);
        cpuProfiler_->recordCounter("draw.recorded.shadow.point",
            frameCounters_.drawShadowPoint);
        cpuProfiler_->recordCounter("draw.recorded.shadow.point.alpha_mask",
            frameCounters_.drawShadowPointAlphaMask);
        cpuProfiler_->recordCounter("shadow.point.casters.tested",
            frameCounters_.shadowPointCastersTested);
        cpuProfiler_->recordCounter("shadow.point.casters.culled",
            frameCounters_.shadowPointCastersCulled);
        cpuProfiler_->recordCounter("draw.recorded.lighting", frameCounters_.drawLighting);
        cpuProfiler_->recordCounter("draw.recorded.output", frameCounters_.drawOutput);
        cpuProfiler_->recordCounter("draw.recorded.transparent.depth",
            frameCounters_.drawTransparentDepth);
        cpuProfiler_->recordCounter("draw.recorded.transparent.forward",
            frameCounters_.drawTransparentForward);
        cpuProfiler_->recordCounter("draw.recorded.forward.standard",
            frameCounters_.drawStandardForward);
        cpuProfiler_->recordCounter("draw.recorded.forward.complex",
            frameCounters_.drawComplexForward);
        cpuProfiler_->recordCounter("draw.recorded.forward.unlit",
            frameCounters_.drawUnlitForward);
        constexpr std::array<const char*, 8> LobeCounterNames{
            "draw.recorded.lobe.clearcoat", "draw.recorded.lobe.sheen",
            "draw.recorded.lobe.anisotropy", "draw.recorded.lobe.iridescence",
            "draw.recorded.lobe.thin_transmission",
            "draw.recorded.lobe.volume_transmission",
            "draw.recorded.lobe.dispersion",
            "draw.recorded.lobe.diffuse_transmission",
        };
        for (size_t index = 0; index < LobeCounterNames.size(); ++index)
            cpuProfiler_->recordCounter(LobeCounterNames[index],
                frameCounters_.complexLobeDraws[index]);
        cpuProfiler_->recordCounter("draw.recorded.transparent", transparentDraws);
        cpuProfiler_->recordCounter("draw.recorded.ui", frameCounters_.drawUi,
            uiAwareStatus);
        cpuProfiler_->recordCounter("draw.recorded.total", totalDraws, uiAwareStatus);
        cpuProfiler_->recordCounter("dispatch.recorded",
            frameCounters_.dispatchRecorded);
        cpuProfiler_->recordCounter("triangle.submitted", frameCounters_.trianglesSubmitted,
            uiAwareStatus);
        cpuProfiler_->recordCounter("material.binds", frameCounters_.materialBinds);
        cpuProfiler_->recordCounter("material.unique", uniqueMaterialIds_.size(),
            materialUniqueStatus);
        cpuProfiler_->recordCounter("material.unique_overflow",
            frameCounters_.materialUniqueOverflow);
        cpuProfiler_->recordCounter("pipeline.binds", frameCounters_.pipelineBinds);
        cpuProfiler_->recordCounter("pipeline.unique", uniquePipelineIds_.size(),
            pipelineUniqueStatus);
        cpuProfiler_->recordCounter("pipeline.unique_overflow",
            frameCounters_.pipelineUniqueOverflow);
        cpuProfiler_->recordCounter("transparent.bucket.background_packets",
            frameCounters_.transparentBackgroundPackets);
        cpuProfiler_->recordCounter("transparent.bucket.foreground_packets",
            frameCounters_.transparentForegroundPackets);
        cpuProfiler_->recordCounter("transparent.bucket.nonempty",
            frameCounters_.transparentNonemptyBuckets);
        cpuProfiler_->recordCounter("transparent.sorted.packets",
            frameCounters_.transparentSortedPackets);
        cpuProfiler_->recordCounter("transparent.pyramid.builds",
            frameCounters_.transparencyPyramidBuilds);
        cpuProfiler_->recordCounter("transparent.pyramid.mip_dispatches",
            frameCounters_.transparencyPyramidMipDispatches);
        cpuProfiler_->recordCounter("transparent.pyramid.resident",
            static_cast<uint64_t>(transparencyPyramidResidency_.enabled()));
        cpuProfiler_->recordCounter("transparent.pyramid.topology_rebuilds",
            frameCounters_.transparencyPyramidTopologyRebuilds);
        cpuProfiler_->recordCounter(
            "transparent.pyramid.topology_rebuild_failures",
            frameCounters_.transparencyPyramidTopologyRebuildFailures);
        cpuProfiler_->recordCounter("transparent.pyramid.fallback_frames",
            frameCounters_.transparencyPyramidFallbackFrames);
        cpuProfiler_->recordCounter("transparent.ordinary2.probe_frames",
            frameCounters_.ordinary2ProbeFrames);
        cpuProfiler_->recordCounter("transparent.ordinary2.candidate_packets",
            frameCounters_.ordinary2CandidatePackets);
        cpuProfiler_->recordCounter("transparent.ordinary2.projected_packets",
            frameCounters_.ordinary2ProjectedPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.projection_culled_packets",
            frameCounters_.ordinary2ProjectionCulledPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.fallback.invalid_bounds_packets",
            frameCounters_.ordinary2InvalidBoundsFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.fallback.near_plane_packets",
            frameCounters_.ordinary2NearPlaneFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.fallback.unsafe_projection_packets",
            frameCounters_.ordinary2UnsafeProjectionFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.fallback.request_capacity_packets",
            frameCounters_.ordinary2RequestCapacityFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.atlas.accepted_packets",
            frameCounters_.ordinary2AtlasAcceptedPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.atlas.accepted_islands",
            frameCounters_.ordinary2AtlasAcceptedIslands);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.atlas.rejected_packets",
            frameCounters_.ordinary2AtlasRejectedPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.atlas.allocated_texels",
            frameCounters_.ordinary2AtlasAllocatedTexels);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.capture.prepared_draws",
            frameCounters_.ordinary2CapturePreparedDraws);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.capture.preparation_fallback_packets",
            frameCounters_.ordinary2CapturePreparationFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.capture.entry_draws",
            frameCounters_.ordinary2CaptureEntryDraws);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.capture.exit_draws",
            frameCounters_.ordinary2CaptureExitDraws);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.local_composition_draws",
            frameCounters_.ordinary2LocalCompositionDraws);
        cpuProfiler_->recordCounter(
            "transparent.ordinary2.scene_resolve_draws",
            frameCounters_.ordinary2SceneResolveDraws);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.candidate_packets",
            frameCounters_.deepLayeredCandidatePackets);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.projected_packets",
            frameCounters_.deepLayeredProjectedPackets);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.atlas.accepted_packets",
            frameCounters_.deepLayeredAtlasAcceptedPackets);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.atlas.accepted_islands",
            frameCounters_.deepLayeredAtlasAcceptedIslands);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.atlas.rejected_packets",
            frameCounters_.deepLayeredAtlasRejectedPackets);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.capture.prepared_draws",
            frameCounters_.deepLayeredCapturePreparedDraws);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.capture.preparation_fallback_packets",
            frameCounters_.deepLayeredCapturePreparationFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.capture.interface_draws",
            frameCounters_.deepLayeredInterfaceDraws);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.residual.probe_draws",
            frameCounters_.deepLayeredResidualProbeDraws);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.local_composition_draws",
            frameCounters_.deepLayeredLocalCompositionDraws);
        cpuProfiler_->recordCounter(
            "transparent.layered.deep.scene_resolve_draws",
            frameCounters_.deepLayeredSceneResolveDraws);
        cpuProfiler_->recordCounter("ui.untracked_callbacks",
            frameCounters_.uiUntrackedCallbacks);
        cpuProfiler_->recordCounter("texture.resident",
            textureVault.activeCount() - retiredTextureCount_);
        cpuProfiler_->recordCounter("texture.retired", retiredTextureCount_);
        cpuProfiler_->recordCounter("texture.sampler.live", liveSamplerCount());
        cpuProfiler_->recordCounter("texture.sampler.cached", samplerCache_.size());
        cpuProfiler_->recordCounter("material.resident", materialVault.activeCount());
        cpuProfiler_->recordCounter("material.descriptor.sets",
            VulkanIndexedTextureTable::FrameSetCount *
                VulkanIndexedTextureTable::SetsPerFrame);
        cpuProfiler_->recordCounter("material.descriptor.indexed",
            1);
        cpuProfiler_->recordCounter("material.table.capacity",
            canonicalMaterialCapacity_);
        cpuProfiler_->recordCounter("material.table.maximum_capacity",
            canonicalMaterialMaximumCapacity_);
        cpuProfiler_->recordCounter("texture.descriptor.view_capacity",
            indexedTextureTable_.frameCapacity(scheduler.currentFrameIndex()));
        cpuProfiler_->recordCounter("texture.descriptor.sampler_capacity",
            indexedTextureTable_.frameCapacity(scheduler.currentFrameIndex()));
        cpuProfiler_->recordCounter("texture.descriptor.required_capacity",
            indexedTextureTable_.requiredCapacity());
        cpuProfiler_->recordCounter("texture.descriptor.maximum_capacity",
            indexedTextureTable_.maximumCapacity());
    }

    void VulkanVertexBackend::bindMaterialDescriptors(
        VkPipelineLayout layout) {
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        const auto sets = indexedTextureTable_.descriptorSets(frameIndex);
        if (sets[0] == VK_NULL_HANDLE || sets[1] == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "Indexed material descriptor sets are unavailable");
        }
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 1, static_cast<uint32_t>(sets.size()),
            sets.data(), 0, nullptr);
    }

    uint32_t VulkanVertexBackend::acquireSampler(const SamplerDesc& desc) {
        for (uint32_t index = 0; index < samplerCache_.size(); ++index) {
            CachedSampler& cached = samplerCache_[index];
            if (cached.desc == desc) {
                ++cached.referenceCount;
                return index;
            }
        }

        const auto toVkFilter = [](FilterMode mode) {
            return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        };
        const auto toVkAddressMode = [](SamplerAddressMode mode) {
            switch (mode) {
            case SamplerAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SamplerAddressMode::MirroredRepeat:
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case SamplerAddressMode::ClampToEdge:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        const auto toVkCompareOp = [](SamplerCompareOp operation) {
            switch (operation) {
            case SamplerCompareOp::Never: return VK_COMPARE_OP_NEVER;
            case SamplerCompareOp::Less: return VK_COMPARE_OP_LESS;
            case SamplerCompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case SamplerCompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case SamplerCompareOp::GreaterOrEqual:
                return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case SamplerCompareOp::Always: return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_NEVER;
        };

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = toVkFilter(desc.magFilter);
        samplerInfo.minFilter = toVkFilter(desc.minFilter);
        samplerInfo.mipmapMode = desc.mipmapFilter == MipmapFilterMode::Nearest
            ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = toVkAddressMode(desc.addressU);
        samplerInfo.addressModeV = toVkAddressMode(desc.addressV);
        samplerInfo.addressModeW = toVkAddressMode(desc.addressW);
        samplerInfo.minLod = static_cast<float>(desc.minLod);
        samplerInfo.maxLod = static_cast<float>(desc.maxLod);
        samplerInfo.anisotropyEnable = desc.maxAnisotropy > 1
            ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy =
            static_cast<float>(std::max<uint8_t>(1, desc.maxAnisotropy));
        samplerInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
        samplerInfo.compareOp = toVkCompareOp(desc.compareOp);

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr,
                &sampler) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create texture sampler.");
        }
        samplerCache_.push_back(CachedSampler{
            .desc = desc,
            .sampler = sampler,
            .referenceCount = 1,
        });
        return static_cast<uint32_t>(samplerCache_.size() - 1);
    }

    void VulkanVertexBackend::releaseSampler(uint32_t cacheIndex) noexcept {
        if (cacheIndex >= samplerCache_.size()) {
            return;
        }
        CachedSampler& cached = samplerCache_[cacheIndex];
        if (cached.referenceCount > 0) {
            --cached.referenceCount;
        }
    }

    void VulkanVertexBackend::cleanupSamplerCache() noexcept {
        if (!vkContext) {
            samplerCache_.clear();
            return;
        }
        for (CachedSampler& cached : samplerCache_) {
            if (cached.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(vkContext->getDevice(), cached.sampler, nullptr);
                cached.sampler = VK_NULL_HANDLE;
            }
        }
        samplerCache_.clear();
    }

    uint64_t VulkanVertexBackend::liveSamplerCount() const noexcept {
        return static_cast<uint64_t>(std::count_if(
            samplerCache_.begin(), samplerCache_.end(),
            [](const CachedSampler& cached) {
                return cached.referenceCount > 0;
            }));
    }

    void VulkanVertexBackend::rebuildRenderGraphAfterDeviceIdle() {
        renderGraph_.cleanupAfterDeviceIdle();
        renderGraph_.init(resourceAllocator,
            VulkanFrameScheduler::FramesInFlight,
            ProfileMemoryCategory::RenderGraphTransient);
        renderGraph_.rebuild(buildVulkanProductionRenderGraph(
            sceneExtent_, vkSwapchain->getExtent(),
            vkSwapchain->getImageFormat(),
            outputTargetFormat_, outputTransport_ ==
                Color::OutputTransport::Hdr10Pq, gBufferLayout_,
            clusterConfig_, directionalShadowResolution_,
            spotShadowAtlasResolution_,
            transparencyPyramidResidency_.enabled(),
            VulkanLayeredGraphConfig{ ordinary2AtlasExtent_,
                hero4AtlasExtent_, cinematic8AtlasExtent_ }));
    }

    void VulkanVertexBackend::applyTransparencyPyramidTopologyChange(
        std::optional<VkExtent2D> requestedOrdinary2AtlasExtent,
        std::optional<VkExtent2D> requestedHero4AtlasExtent,
        std::optional<VkExtent2D> requestedCinematic8AtlasExtent) {
        const VkExtent2D previousOrdinary2AtlasExtent =
            ordinary2AtlasExtent_;
        const VkExtent2D previousHero4AtlasExtent = hero4AtlasExtent_;
        const VkExtent2D previousCinematic8AtlasExtent =
            cinematic8AtlasExtent_;
        const auto requestedExtent = [&](std::optional<VkExtent2D> explicitExtent,
                const TransparencyPyramidResidency& residency,
                TransparencyQuality quality) {
            if (explicitExtent) return *explicitExtent;
            if (!residency.requestedEnabled()) return VkExtent2D{};
            const Ordinary2AtlasExtent capacity = layeredAtlasCapacityExtent(
                sceneExtent_.width, sceneExtent_.height, quality);
            return VkExtent2D{ capacity.width, capacity.height };
        };
        const VkExtent2D nextOrdinary2AtlasExtent = requestedExtent(
            requestedOrdinary2AtlasExtent, ordinary2AtlasResidency_,
            TransparencyQuality::Ordinary2);
        const VkExtent2D nextHero4AtlasExtent = requestedExtent(
            requestedHero4AtlasExtent, hero4AtlasResidency_,
            TransparencyQuality::Hero4);
        const VkExtent2D nextCinematic8AtlasExtent = requestedExtent(
            requestedCinematic8AtlasExtent, cinematic8AtlasResidency_,
            TransparencyQuality::Cinematic8);
        const auto extentChanged = [](VkExtent2D lhs, VkExtent2D rhs) {
            return lhs.width != rhs.width || lhs.height != rhs.height;
        };
        const bool ordinary2AtlasChange = extentChanged(
            previousOrdinary2AtlasExtent, nextOrdinary2AtlasExtent);
        const bool hero4AtlasChange = extentChanged(
            previousHero4AtlasExtent, nextHero4AtlasExtent);
        const bool cinematic8AtlasChange = extentChanged(
            previousCinematic8AtlasExtent, nextCinematic8AtlasExtent);
        if (!transparencyPyramidResidency_.changePending() &&
            !ordinary2AtlasResidency_.changePending() &&
            !hero4AtlasResidency_.changePending() &&
            !cinematic8AtlasResidency_.changePending() &&
            !ordinary2AtlasChange && !hero4AtlasChange &&
            !cinematic8AtlasChange)
            return;

        CpuScope topologyChangeScope(cpuProfiler_,
            "cpu.renderer.transparency_topology_change");

        const bool previousEnabled =
            transparencyPyramidResidency_.enabled();
        const bool previousOrdinary2Enabled =
            ordinary2AtlasResidency_.enabled();
        const bool previousHero4Enabled = hero4AtlasResidency_.enabled();
        const bool previousCinematic8Enabled =
            cinematic8AtlasResidency_.enabled();
        const auto releaseTargets = [&] {
            for (VkDescriptorSet texture : uiSceneTextures) {
                if (texture != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTexture(texture);
            }
            for (VkDescriptorSet texture : uiDepthTextures) {
                if (texture != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTexture(texture);
            }
            uiSceneTextures.clear();
            uiDepthTextures.clear();
            sceneDescriptors.cleanup();
            clusteredLighting_.clearDescriptors();
            outputPass.clearDescriptors();
            hdrEncodePass.clearTargets();
            transparencyPyramid_.clearDescriptors();
            layeredInterfaceCapture_.clearDescriptors();
            layeredLocalComposition_.clearDescriptors();
            layeredSceneResolve_.clearDescriptors();
            frameTargets.cleanup();
            renderGraph_.cleanupAfterDeviceIdle();
        };
        const auto createTargets = [&] {
            rebuildRenderGraphAfterDeviceIdle();
            initFrameTargets();
            if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
                hdrEncodePass.rebuild(frameTargets,
                    vkSwapchain->getImageViews(), vkSwapchain->getExtent());
            }
            uploadContext.flush();
            sceneDescriptors.init(vkContext->getDevice(), descriptorAllocator,
                lightingPipeline->getDescriptorSetLayout());
            bindLightRecordBuffers();
            bindSceneClusterBuffers();
            bindEnvironmentProducts();
            bindDirectionalShadowDescriptors();
            bindSpotShadowDescriptors();
            bindPointShadowDescriptors();
            bindReflectionProbeBuffers();
            bindReflectionProbeEnvironments();
            sceneDescriptors.rebuild(frameTargets);
            transparencyPyramid_.rebuild(frameTargets);
            layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
            layeredLocalComposition_.rebuildDescriptors(frameTargets);
            layeredSceneResolve_.rebuildDescriptors(frameTargets);
            bindClusterBuffers();
            if (VulkanTexturePayload* lut = textureVault.get(
                    outputTransformLut_); lut != nullptr && !lut->retired) {
                outputPass.rebuildDescriptors(frameTargets,
                    lut->image.view, lut->sampler);
            }
            else {
                outputPass.rebuildDescriptors(frameTargets);
            }
            uiSceneTextures.resize(frameTargets.size());
            uiDepthTextures.resize(frameTargets.size());
            for (size_t index = 0; index < frameTargets.size(); ++index) {
                const VulkanFrameContextTargets& targets =
                    frameTargets.get(index);
                uiSceneTextures[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), targets.output.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                uiDepthTextures[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), targets.glassDepth.view,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }
        };

        // This executes only between frames. All shared descriptor sets and
        // scene targets must be unreferenced before the topology is retired.
        {
            CpuScope waitScope(cpuProfiler_,
                "cpu.renderer.transparency_topology_wait");
            scheduler.waitForAllFrames();
        }
        try {
            CpuScope rebuildScope(cpuProfiler_,
                "cpu.renderer.transparency_topology_rebuild");
            releaseTargets();
            transparencyPyramidResidency_.publishRequested();
            ordinary2AtlasResidency_.publishRequested();
            hero4AtlasResidency_.publishRequested();
            cinematic8AtlasResidency_.publishRequested();
            ordinary2AtlasExtent_ = nextOrdinary2AtlasExtent;
            hero4AtlasExtent_ = nextHero4AtlasExtent;
            cinematic8AtlasExtent_ = nextCinematic8AtlasExtent;
            createTargets();
            if (collectFrameCounters_)
                ++frameCounters_.transparencyPyramidTopologyRebuilds;
        }
        catch (const std::exception& exception) {
            if (collectFrameCounters_)
                ++frameCounters_.transparencyPyramidTopologyRebuildFailures;
            try {
                CpuScope restoreScope(cpuProfiler_,
                    "cpu.renderer.transparency_topology_restore");
                releaseTargets();
                transparencyPyramidResidency_.restore(previousEnabled);
                ordinary2AtlasResidency_.restore(previousOrdinary2Enabled);
                hero4AtlasResidency_.restore(previousHero4Enabled);
                cinematic8AtlasResidency_.restore(previousCinematic8Enabled);
                ordinary2AtlasExtent_ = previousOrdinary2AtlasExtent;
                hero4AtlasExtent_ = previousHero4AtlasExtent;
                cinematic8AtlasExtent_ = previousCinematic8AtlasExtent;
                createTargets();
            }
            catch (const std::exception& restoreException) {
                throw std::runtime_error(std::string(
                    "Transparency topology rebuild failed: ") +
                    exception.what() + "; restoring the previous topology "
                    "failed: " + restoreException.what());
            }
        }
    }

    void VulkanVertexBackend::recreateSwapchain(GLFWwindow* window) {
        // 1. Handle Minimization (Pause the engine until it's un-minimized)
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        const VkFormat oldImageFormat = vkSwapchain->getImageFormat();
        const uint32_t oldImageCount = vkSwapchain->getImageCount();
        auto candidate = std::make_unique<VkSwapchain>(vkContext.get(), window,
			outputTransport_, vkSwapchain->getSwapchain());
        if (candidate->getImageFormat() != oldImageFormat) {
            throw std::runtime_error("Swapchain format changed; a full renderer rebuild is required.");
        }

        // Resize is the one accepted global stall, after candidate validation.
        vkDeviceWaitIdle(vkContext->getDevice());

        for (VkDescriptorSet texture : uiSceneTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        for (VkDescriptorSet texture : uiDepthTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        uiSceneTextures.clear();
        uiDepthTextures.clear();
        sceneDescriptors.cleanup();
        // The clustered pass owns descriptor sets that reference transient
        // render-graph buffers.  Swapchain recreation rebuilds that graph, so
        // retire the bindings before the buffers and recreate them afterward.
        clusteredLighting_.clearDescriptors();
        outputPass.clearDescriptors();
        hdrEncodePass.clearTargets();
        transparencyPyramid_.clearDescriptors();
        layeredInterfaceCapture_.clearDescriptors();
        layeredLocalComposition_.clearDescriptors();
        layeredSceneResolve_.clearDescriptors();
        frameTargets.cleanup();

        vkSwapchain = std::move(candidate);
        vkSwapchain->setHdrMetadata(peakNits_);
        const uint32_t newImageCount = vkSwapchain->getImageCount();
        scheduler.resetSwapchainImages(newImageCount);
        scheduler.setTransparentTargetPixelCount(
            static_cast<uint64_t>(sceneExtent_.width) *
            sceneExtent_.height);
        rebuildRenderGraphAfterDeviceIdle();
        initFrameTargets();
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            hdrEncodePass.rebuild(frameTargets, vkSwapchain->getImageViews(),
                vkSwapchain->getExtent());
        }
        // The replacement target images are referenced by descriptor sets and
        // ImGui immediately below; establish their declared layouts first.
        uploadContext.flush();
        sceneDescriptors.init(vkContext->getDevice(), descriptorAllocator,
            lightingPipeline->getDescriptorSetLayout());
        bindLightRecordBuffers();
        bindSceneClusterBuffers();
        bindEnvironmentProducts();
        bindDirectionalShadowDescriptors();
        bindSpotShadowDescriptors();
        bindPointShadowDescriptors();
        bindReflectionProbeBuffers();
        bindReflectionProbeEnvironments();
        sceneDescriptors.rebuild(frameTargets);
        transparencyPyramid_.rebuild(frameTargets);
        layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
        layeredLocalComposition_.rebuildDescriptors(frameTargets);
        layeredSceneResolve_.rebuildDescriptors(frameTargets);
        bindClusterBuffers();
        if (VulkanTexturePayload* lut = textureVault.get(outputTransformLut_);
            lut != nullptr && !lut->retired) {
            outputPass.rebuildDescriptors(frameTargets, lut->image.view, lut->sampler);
        }
        else {
            outputPass.rebuildDescriptors(frameTargets);
        }
        if (newImageCount != oldImageCount) {
            ImGui_ImplVulkan_SetMinImageCount(newImageCount);
        }

        uiSceneTextures.resize(frameTargets.size());
        uiDepthTextures.resize(frameTargets.size());
        for (size_t i = 0; i < frameTargets.size(); i++) {
            const VulkanFrameContextTargets& targets = frameTargets.get(i);
            uiSceneTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(),
                targets.output.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(),
                targets.glassDepth.view,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }

    }

    RenderExtent VulkanVertexBackend::getRenderExtent() const {
        if (sceneExtent_.width == 0 || sceneExtent_.height == 0) {
            return {};
        }
        return { sceneExtent_.width, sceneExtent_.height };
    }

    bool VulkanVertexBackend::resizeSceneRenderExtent(
        RenderExtent extent, std::string& diagnostic) {
        diagnostic.clear();
        if (!initialized_ || !vkContext || !vkSwapchain) {
            diagnostic = "The Vulkan backend is not initialized";
            return false;
        }
        if (frameOpen_) {
            diagnostic = "Scene targets can only be resized between frames";
            return false;
        }
        const uint32_t maximum = vkContext->getPhysicalDeviceProperties()
            .limits.maxImageDimension2D;
        if (extent.width < 64 || extent.height < 64 ||
            extent.width > maximum || extent.height > maximum) {
            diagnostic = "Requested scene extent is outside Vulkan image limits";
            return false;
        }
        const VkExtent2D requested{ extent.width, extent.height };
        if (requested.width == sceneExtent_.width &&
            requested.height == sceneExtent_.height) {
            return true;
        }

        // Compile first so invalid graph contracts cannot disturb the active
        // target. Resource allocation is retried with the previous extent if
        // the replacement fails after the fence-safe cutover begins.
        const VkExtent2D previousOrdinary2AtlasExtent =
            ordinary2AtlasExtent_;
        const VkExtent2D previousHero4AtlasExtent = hero4AtlasExtent_;
        const VkExtent2D previousCinematic8AtlasExtent =
            cinematic8AtlasExtent_;
        VkExtent2D requestedOrdinary2AtlasExtent{};
        VkExtent2D requestedHero4AtlasExtent{};
        VkExtent2D requestedCinematic8AtlasExtent{};
        const auto resizeTier = [&](bool resident, TransparencyQuality quality,
                VkExtent2D& atlasExtent) {
            if (!resident) return;
            const Ordinary2AtlasExtent capacity = layeredAtlasCapacityExtent(
                requested.width, requested.height, quality);
            atlasExtent = { capacity.width, capacity.height };
        };
        resizeTier(ordinary2AtlasResidency_.enabled(),
            TransparencyQuality::Ordinary2,
            requestedOrdinary2AtlasExtent);
        resizeTier(hero4AtlasResidency_.enabled(),
            TransparencyQuality::Hero4, requestedHero4AtlasExtent);
        resizeTier(cinematic8AtlasResidency_.enabled(),
            TransparencyQuality::Cinematic8,
            requestedCinematic8AtlasExtent);
        try {
            (void)buildVulkanProductionRenderGraph(requested,
                vkSwapchain->getExtent(), vkSwapchain->getImageFormat(),
                outputTargetFormat_, outputTransport_ ==
                    Color::OutputTransport::Hdr10Pq, gBufferLayout_,
                clusterConfig_, directionalShadowResolution_,
                spotShadowAtlasResolution_,
                transparencyPyramidResidency_.enabled(),
                VulkanLayeredGraphConfig{ requestedOrdinary2AtlasExtent,
                    requestedHero4AtlasExtent,
                    requestedCinematic8AtlasExtent });
        }
        catch (const std::exception& exception) {
            diagnostic = exception.what();
            return false;
        }

        const VkExtent2D previous = sceneExtent_;
        const auto releaseTargets = [&] {
            for (VkDescriptorSet texture : uiSceneTextures) {
                if (texture != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(texture);
                }
            }
            for (VkDescriptorSet texture : uiDepthTextures) {
                if (texture != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(texture);
                }
            }
            uiSceneTextures.clear();
            uiDepthTextures.clear();
            sceneDescriptors.cleanup();
            clusteredLighting_.clearDescriptors();
            outputPass.clearDescriptors();
            hdrEncodePass.clearTargets();
            transparencyPyramid_.clearDescriptors();
            layeredInterfaceCapture_.clearDescriptors();
            layeredLocalComposition_.clearDescriptors();
            layeredSceneResolve_.clearDescriptors();
            frameTargets.cleanup();
            renderGraph_.cleanupAfterDeviceIdle();
        };
        const auto createTargets = [&] {
            rebuildRenderGraphAfterDeviceIdle();
            initFrameTargets();
            if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
                hdrEncodePass.rebuild(frameTargets,
                    vkSwapchain->getImageViews(), vkSwapchain->getExtent());
            }
            uploadContext.flush();
            sceneDescriptors.init(vkContext->getDevice(), descriptorAllocator,
                lightingPipeline->getDescriptorSetLayout());
            bindLightRecordBuffers();
            bindSceneClusterBuffers();
            bindEnvironmentProducts();
            bindDirectionalShadowDescriptors();
            bindSpotShadowDescriptors();
            bindPointShadowDescriptors();
            bindReflectionProbeBuffers();
            bindReflectionProbeEnvironments();
            sceneDescriptors.rebuild(frameTargets);
            transparencyPyramid_.rebuild(frameTargets);
            layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
            layeredLocalComposition_.rebuildDescriptors(frameTargets);
            layeredSceneResolve_.rebuildDescriptors(frameTargets);
            bindClusterBuffers();
            if (VulkanTexturePayload* lut = textureVault.get(outputTransformLut_);
                lut != nullptr && !lut->retired) {
                outputPass.rebuildDescriptors(frameTargets,
                    lut->image.view, lut->sampler);
            }
            else {
                outputPass.rebuildDescriptors(frameTargets);
            }
            uiSceneTextures.resize(frameTargets.size());
            uiDepthTextures.resize(frameTargets.size());
            for (size_t index = 0; index < frameTargets.size(); ++index) {
                const VulkanFrameContextTargets& targets =
                    frameTargets.get(index);
                uiSceneTextures[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), targets.output.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                uiDepthTextures[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), targets.glassDepth.view,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }
            scheduler.setTransparentTargetPixelCount(
                static_cast<uint64_t>(sceneExtent_.width) *
                sceneExtent_.height);
        };

        scheduler.waitForAllFrames();
        releaseTargets();
        sceneExtent_ = requested;
        ordinary2AtlasExtent_ = requestedOrdinary2AtlasExtent;
        hero4AtlasExtent_ = requestedHero4AtlasExtent;
        cinematic8AtlasExtent_ = requestedCinematic8AtlasExtent;
        try {
            createTargets();
            return true;
        }
        catch (const std::exception& exception) {
            diagnostic = std::string("Scene target resize failed: ") +
                exception.what();
            releaseTargets();
            sceneExtent_ = previous;
            ordinary2AtlasExtent_ = previousOrdinary2AtlasExtent;
            hero4AtlasExtent_ = previousHero4AtlasExtent;
            cinematic8AtlasExtent_ = previousCinematic8AtlasExtent;
            try {
                createTargets();
            }
            catch (const std::exception& restoreException) {
                throw std::runtime_error(
                    diagnostic + "; restoring the previous scene target failed: " +
                    restoreException.what());
            }
            return false;
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
            .gpuLightRecords = lightRecordCapacity_ != 0,
            .maxIndexedTextureViews = vkContext->getMaxIndexedTextureViews(),
            .maxIndexedSamplers = vkContext->getMaxIndexedSamplers(),
            .maxUpdateAfterBindDescriptors =
                vkContext->getMaxUpdateAfterBindDescriptors(),
            .maxGpuLightRecords = lightRecordMaximumCapacity_,
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
		for (const Color::OutputTransport transport :
			vkSwapchain->getSupportedOutputTransports()) {
			info.supportedOutputTransports.emplace_back(outputTransportName(transport));
		}
		const VulkanOutputTransportSelection& transport =
			vkSwapchain->getOutputTransportSelection();
		info.requestedOutputTransport = outputTransportName(transport.requested);
		info.effectiveOutputTransport = outputTransportName(transport.effective);
		info.outputTransportDiagnostic = transport.diagnostic;
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
        else switch (outputOperator_) {
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
        info.refractionPyramidsResident =
            transparencyPyramidResidency_.enabled();
        info.ordinary2AtlasResident = ordinary2AtlasResidency_.enabled() &&
            ordinary2AtlasExtent_.width != 0u &&
            ordinary2AtlasExtent_.height != 0u;
        info.ordinary2AtlasWidth = ordinary2AtlasExtent_.width;
        info.ordinary2AtlasHeight = ordinary2AtlasExtent_.height;
        info.hero4AtlasResident = hero4AtlasResidency_.enabled() &&
            hero4AtlasExtent_.width != 0u && hero4AtlasExtent_.height != 0u;
        info.hero4AtlasWidth = hero4AtlasExtent_.width;
        info.hero4AtlasHeight = hero4AtlasExtent_.height;
        info.cinematic8AtlasResident = cinematic8AtlasResidency_.enabled() &&
            cinematic8AtlasExtent_.width != 0u &&
            cinematic8AtlasExtent_.height != 0u;
        info.cinematic8AtlasWidth = cinematic8AtlasExtent_.width;
        info.cinematic8AtlasHeight = cinematic8AtlasExtent_.height;
        info.frameTopologyPrewarmRequested =
            frameTopologyPrewarm_.requested;
        info.frameTopologyPrewarmChanged = frameTopologyPrewarm_.changed;
        info.frameTopologyPrewarmNanoseconds =
            frameTopologyPrewarm_.durationNanoseconds;
        info.gpuLightCapacity = lightRecordCapacity_;
        info.gpuLightActiveCount = activeLightCount_;
        info.gpuLightUploadBytes = lightUploadBytes_;
        info.gpuLightUploadRanges = lightUploadRangeCount_;
        info.uploads = uploadContext.telemetry();
        return info;
    }

    FrameTopologyPreparation VulkanVertexBackend::prepareFrameTopology(
        const FrameTopologyRequirements& requirements) {
        if (frameOpen_) {
            throw std::logic_error(
                "Frame topology preparation is only valid between frames");
        }

        FrameTopologyPreparation result{
            .requested = requirements.refractionPyramids ||
                requirements.ordinary2LayeredInterfaces ||
                requirements.hero4LayeredInterfaces ||
                requirements.cinematic8LayeredInterfaces,
        };
        const bool previousPyramids =
            transparencyPyramidResidency_.enabled();
        const VkExtent2D previousOrdinary2AtlasExtent =
            ordinary2AtlasExtent_;
        const VkExtent2D previousHero4AtlasExtent = hero4AtlasExtent_;
        const VkExtent2D previousCinematic8AtlasExtent =
            cinematic8AtlasExtent_;
        const bool requirePyramids = requirements.refractionPyramids ||
            requirements.ordinary2LayeredInterfaces ||
            requirements.hero4LayeredInterfaces ||
            requirements.cinematic8LayeredInterfaces;
        VkExtent2D requestedOrdinary2AtlasExtent = ordinary2AtlasExtent_;
        VkExtent2D requestedHero4AtlasExtent = hero4AtlasExtent_;
        VkExtent2D requestedCinematic8AtlasExtent = cinematic8AtlasExtent_;
        const auto requireTier = [&](bool required,
                TransparencyQuality quality, VkExtent2D& requestedExtent,
                const char* name) {
            if (!required) return;
            const Ordinary2AtlasExtent capacity = layeredAtlasCapacityExtent(
                sceneExtent_.width, sceneExtent_.height, quality);
            if (capacity.empty()) {
                throw std::runtime_error(std::string(name) +
                    " startup topology requires a tile-sized scene extent");
            }
            requestedExtent = { capacity.width, capacity.height };
        };
        requireTier(requirements.ordinary2LayeredInterfaces,
            TransparencyQuality::Ordinary2, requestedOrdinary2AtlasExtent,
            "Ordinary2");
        requireTier(requirements.hero4LayeredInterfaces,
            TransparencyQuality::Hero4, requestedHero4AtlasExtent, "Hero4");
        requireTier(requirements.cinematic8LayeredInterfaces,
            TransparencyQuality::Cinematic8,
            requestedCinematic8AtlasExtent, "Cinematic8");
        const auto extentChanged = [](VkExtent2D lhs, VkExtent2D rhs) {
            return lhs.width != rhs.width || lhs.height != rhs.height;
        };
        const bool ordinary2Change = extentChanged(
            requestedOrdinary2AtlasExtent, ordinary2AtlasExtent_);
        const bool hero4Change = extentChanged(
            requestedHero4AtlasExtent, hero4AtlasExtent_);
        const bool cinematic8Change = extentChanged(
            requestedCinematic8AtlasExtent, cinematic8AtlasExtent_);
        if (!requirePyramids) {
            frameTopologyPrewarm_ = result;
            return result;
        }

        transparencyPyramidResidency_.observe(true);
        ordinary2AtlasResidency_.observe(
            requirements.ordinary2LayeredInterfaces);
        hero4AtlasResidency_.observe(requirements.hero4LayeredInterfaces);
        cinematic8AtlasResidency_.observe(
            requirements.cinematic8LayeredInterfaces);
        if (!transparencyPyramidResidency_.changePending() &&
            !ordinary2AtlasResidency_.changePending() &&
            !hero4AtlasResidency_.changePending() &&
            !cinematic8AtlasResidency_.changePending() &&
            !ordinary2Change && !hero4Change && !cinematic8Change) {
            frameTopologyPrewarm_ = result;
            return result;
        }

        const auto start = std::chrono::steady_clock::now();
        applyTransparencyPyramidTopologyChange(
            requestedOrdinary2AtlasExtent, requestedHero4AtlasExtent,
            requestedCinematic8AtlasExtent);
        result.durationNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        result.changed =
            previousPyramids != transparencyPyramidResidency_.enabled() ||
            previousOrdinary2AtlasExtent.width != ordinary2AtlasExtent_.width ||
            previousOrdinary2AtlasExtent.height != ordinary2AtlasExtent_.height ||
            previousHero4AtlasExtent.width != hero4AtlasExtent_.width ||
            previousHero4AtlasExtent.height != hero4AtlasExtent_.height ||
            previousCinematic8AtlasExtent.width !=
                cinematic8AtlasExtent_.width ||
            previousCinematic8AtlasExtent.height !=
                cinematic8AtlasExtent_.height;
        frameTopologyPrewarm_ = result;
        return result;
    }

    // ==============================================================================
    // 2. RESOURCE MANAGEMENT (Thread-Safe & Anti-Fragmentation)
    // ==============================================================================

    GeometryHandle VulkanVertexBackend::allocateGeometry(const GeometryDesc& desc,
        std::span<const std::byte> vertexBytes, std::span<const std::byte> indexBytes) {
        const uint32_t indexSize = indexElementSize(desc.indexFormat);
        if (desc.vertexStride == 0 || indexSize == 0) {
            throw std::invalid_argument("Geometry format must define nonzero element sizes.");
        }
        if (indexBytes.size_bytes() % indexSize != 0) {
            throw std::invalid_argument("Geometry index data is not aligned to its index format.");
        }

        VulkanGeometryPayload payload{};
        payload.indexCount = static_cast<uint32_t>(indexBytes.size_bytes() / indexSize);
        payload.indexFormat = desc.indexFormat;

        payload.vertexBuffer = resourceAllocator.createBuffer(vertexBytes.size_bytes(),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
            ProfileMemoryCategory::GeometryVertex);
        try {
            payload.indexBuffer = resourceAllocator.createBuffer(indexBytes.size_bytes(),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::GeometryIndex);

            uploadContext.enqueueBufferUpload(payload.vertexBuffer, vertexBytes,
                ResourceState::VertexBuffer);
            uploadContext.enqueueBufferUpload(payload.indexBuffer, indexBytes,
                ResourceState::IndexBuffer);
        } catch (...) {
            resourceAllocator.destroy(payload.indexBuffer);
            resourceAllocator.destroy(payload.vertexBuffer);
            throw;
        }

        return geometryVault.allocate(payload);
    }

    void VulkanVertexBackend::freeGeometry(GeometryHandle handle) {
        auto* payload = geometryVault.get(handle);
        if (payload) {
            // Capture the Vulkan pointers by value so the lambda remembers them
            // Defer the destruction! The GPU won't crash, and the CPU won't stall.
            scheduler.defer([this,
                vertex = payload->vertexBuffer, index = payload->indexBuffer]() mutable {
                resourceAllocator.destroy(vertex);
                resourceAllocator.destroy(index);
                });

            geometryVault.free(handle);
        }
    }

    TextureHandle VulkanVertexBackend::allocateTexture(const TextureDesc& desc,
        std::span<const std::byte> pixelBytes) {
        if (!validTextureTopology(desc)) {
            throw std::invalid_argument(
                "Texture dimensions, layers, mips, or topology are invalid");
        }
        if (pixelBytes.empty()) {
            throw std::invalid_argument("Texture pixel data must be nonempty");
        }

        const size_t expectedBytes = static_cast<size_t>(textureDataSize(desc));
        if (bytesPerBlock(desc.format) == 0 || pixelBytes.size() != expectedBytes) {
            throw std::invalid_argument("Texture pixel data size does not match the descriptor");
        }

        VulkanTexturePayload payload{};
        payload.format = desc.format;
        payload.width = desc.width;
        payload.height = desc.height;

        VkFormat format = VK_FORMAT_UNDEFINED;
        switch (desc.format) {
        case TextureFormat::RGBA8_UNorm:
            format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case TextureFormat::RGBA8_sRGB:
            format = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case TextureFormat::RGBA16_SFloat:
            format = VK_FORMAT_R16G16B16A16_SFLOAT;
            break;
        case TextureFormat::RGBA32_SFloat:
            format = VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        case TextureFormat::RG16_SFloat:
            format = VK_FORMAT_R16G16_SFLOAT;
            break;
        case TextureFormat::BC4_UNorm:
            format = VK_FORMAT_BC4_UNORM_BLOCK;
            break;
        case TextureFormat::BC5_UNorm:
            format = VK_FORMAT_BC5_UNORM_BLOCK;
            break;
        case TextureFormat::BC6H_UFloat:
            format = VK_FORMAT_BC6H_UFLOAT_BLOCK;
            break;
        case TextureFormat::BC7_UNorm:
            format = VK_FORMAT_BC7_UNORM_BLOCK;
            break;
        case TextureFormat::BC7_sRGB:
            format = VK_FORMAT_BC7_SRGB_BLOCK;
            break;
        }

        const bool cube = desc.topology == TextureTopology::Cube;
        const VkImageViewType viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE :
            (desc.topology == TextureTopology::Texture2DArray
                ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        payload.image = resourceAllocator.createImage2D({ desc.width, desc.height }, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, desc.usageClass == TextureUsageClass::Environment
                ? ProfileMemoryCategory::Environment
                : ProfileMemoryCategory::Texture, desc.mipLevels,
            desc.arrayLayers,
            cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, viewType);

        try {
            payload.samplerCacheIndex = acquireSampler(desc.sampler);
            payload.sampler =
                samplerCache_[payload.samplerCacheIndex].sampler;
        } catch (...) {
            resourceAllocator.destroy(payload.image);
            throw;
        }

        try {
            CpuScope uploadScheduleScope(
                cpuProfiler_, "cpu.texture.upload_schedule");
            uploadContext.enqueueImageUpload(
                payload.image, pixelBytes, ResourceState::ShaderResource);
        } catch (...) {
            releaseSampler(payload.samplerCacheIndex);
            resourceAllocator.destroy(payload.image);
            throw;
        }

        const TextureHandle handle = textureVault.allocate(payload);
        if (indexedTextureTable_.active()) {
            if (handle.getIndex() >= indexedTextureTable_.maximumCapacity()) {
                uploadContext.flush();
                textureVault.free(handle);
                releaseSampler(payload.samplerCacheIndex);
                resourceAllocator.destroy(payload.image);
                throw std::runtime_error(
                    "Indexed material texture table capacity was exhausted");
            }
            const uint32_t frameIndex = scheduler.currentFrameIndex();
            if (handle.getIndex() >=
                    indexedTextureTable_.frameCapacity(frameIndex) &&
                frameOpen_) {
                uploadContext.flush();
                textureVault.free(handle);
                releaseSampler(payload.samplerCacheIndex);
                resourceAllocator.destroy(payload.image);
                throw std::runtime_error(
                    "Indexed material texture-table growth must occur at a "
                    "frame boundary");
            }
            indexedTextureTable_.ensureFrameCapacity(
                frameIndex, handle.getIndex() + 1);
            indexedTextureTable_.write(handle.getIndex(), payload.image.view,
                handle.getIndex(), payload.sampler);
            // Pre-frame publication can batch descriptor synchronization into
            // beginFrame. Mid-frame publication must make the current
            // fence-owned set visible before draw submission.
            if (frameOpen_) {
                indexedTextureTable_.synchronizeFrame(
                    frameIndex);
            }
        }
        return handle;
    }



    void VulkanVertexBackend::freeTexture(TextureHandle handle) {
        auto* payload = textureVault.get(handle);
        if (payload && !payload->retired) {
            indexedTextureTable_.writeFallback(
                handle.getIndex(), handle.getIndex());
            if (indexedTextureTable_.active() &&
                frameOpen_) {
                indexedTextureTable_.synchronizeFrame(
                    scheduler.currentFrameIndex());
            }
            const uint32_t samplerCacheIndex = payload->samplerCacheIndex;
            VkDescriptorSet imguiDescriptor = payload->imguiDescriptor;
            VulkanImageResource image = payload->image;
            payload->retired = true;
            ++retiredTextureCount_;

            scheduler.defer([this, handle, imguiDescriptor, image]() mutable {
                if (imguiDescriptor != VK_NULL_HANDLE && imguiInitialized_)
                    ImGui_ImplVulkan_RemoveTexture(imguiDescriptor);
                resourceAllocator.destroy(image);
                textureVault.free(handle);
                if (retiredTextureCount_ != 0) --retiredTextureCount_;
                });

            releaseSampler(samplerCacheIndex);
        }
    }

    MaterialBinding VulkanVertexBackend::allocateCanonicalMaterial(
        const CanonicalMaterialAsset& asset) {
        const bool deferred = asset.packed.closureClass ==
                static_cast<uint32_t>(MaterialClosureClass::StandardDeferred) &&
            asset.pipelineState.shaderProgram == ShaderProgram::CanonicalPbrGBuffer &&
            asset.pipelineState.renderPass == RenderPassClass::GBuffer;
        const bool complexProgram =
            asset.pipelineState.shaderProgram ==
                ShaderProgram::CanonicalComplexOpaqueForward ||
            asset.pipelineState.shaderProgram ==
                ShaderProgram::CanonicalComplexForward;
        const bool forward = asset.packed.closureClass !=
                static_cast<uint32_t>(MaterialClosureClass::StandardDeferred) &&
            asset.packed.closureClass !=
                static_cast<uint32_t>(MaterialClosureClass::Invalid) &&
            complexProgram &&
            asset.pipelineState.renderPass == RenderPassClass::Forward;
        const bool transparent = asset.packed.closureClass !=
                static_cast<uint32_t>(MaterialClosureClass::Invalid) &&
            complexProgram &&
            asset.pipelineState.renderPass == RenderPassClass::Transparent;
        if (asset.packed.schemaVersion != PackedGpuMaterial::SchemaVersion ||
            (!deferred && !forward && !transparent))
            throw std::invalid_argument("canonical material asset has an incompatible contract");

        std::array<VulkanTexturePayload*, PackedGpuMaterial::MaxTextureUses> textures{};
        for (size_t index = 0; index < textures.size(); ++index) {
            textures[index] = textureVault.get(asset.textures[index]);
            if (!textures[index] || textures[index]->retired)
                throw std::invalid_argument("canonical material has an invalid texture handle");
        }

        VulkanMaterialPayload materialPayload{};
        materialPayload.pipeline = pipelineLibrary.getOrCreatePipeline(asset.pipelineState);
        PipelineStateDesc mirroredPipelineState = asset.pipelineState;
        if (mirroredPipelineState.renderPass ==
                RenderPassClass::Transparent &&
            mirroredPipelineState.cullMode != CullMode::None) {
            mirroredPipelineState.frontFace =
                mirroredPipelineState.frontFace == FrontFace::Clockwise
                ? FrontFace::CounterClockwise : FrontFace::Clockwise;
            materialPayload.mirroredPipeline =
                pipelineLibrary.getOrCreatePipeline(mirroredPipelineState);
        }
        else {
            materialPayload.mirroredPipeline = materialPayload.pipeline;
        }
        materialPayload.renderQueue = forward || transparent
            ? (asset.pipelineState.blendMode == BlendMode::Opaque
                ? RenderQueue::ForwardOpaque : RenderQueue::Transparent)
            : RenderQueue::Opaque;
        materialPayload.packed = asset.packed;
        materialPayload.packedRevision = 1;

        const MaterialHandle material = materialVault.allocate(materialPayload);
        try {
            ensureCanonicalMaterialCapacity(
                material.getIndex() + 1u);
            for (uint32_t frame = 0; frame <
                VulkanFrameScheduler::FramesInFlight; ++frame) {
                const auto sets = indexedTextureTable_.descriptorSets(frame);
                if (sets[0] == VK_NULL_HANDLE ||
                    sets[1] == VK_NULL_HANDLE) {
                    throw std::runtime_error(
                        "Indexed material descriptor sets are unavailable");
                }
            }
        }
        catch (...) {
            materialVault.free(material);
            throw;
        }
        VulkanMaterialPayload* stored =
            materialVault.get(material);
        return { material, stored->pipeline, stored->renderQueue,
            makeOpaqueSortKey(stored->pipeline, material) };
    }

    void VulkanVertexBackend::updateCanonicalMaterial(MaterialHandle handle,
        const PackedGpuMaterial& material) {
        VulkanMaterialPayload* payload = materialVault.get(handle);
        if (!payload)
            throw std::invalid_argument("canonical material update handle is invalid");
        if (material.schemaVersion != PackedGpuMaterial::SchemaVersion ||
            material.closureClass != payload->packed.closureClass)
            throw std::invalid_argument("canonical material update changes its schema or closure");
        payload->packed = material;
        ++payload->packedRevision;
        if (payload->packedRevision == 0) payload->packedRevision = 1;
    }

    void VulkanVertexBackend::freeMaterial(MaterialHandle handle) {
        auto* payload = materialVault.get(handle);
        if (!payload) {
            return;
        }

        materialVault.free(handle);
    }

    // --- PRIVATE HELPERS ---

    void VulkanVertexBackend::createLightingRenderPass() {
        // This pass writes the evaluated lighting to the frame-context lit-scene target.
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VulkanSceneColorFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(vkContext->getDevice(), &renderPassInfo, nullptr, &lightingRenderPass) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create lighting render pass!");
        }
    }

    void VulkanVertexBackend::initFrameTargets() {
        frameTargets.init(vkContext->getDevice(), *vkSwapchain, sceneExtent_,
            { gBufferPass->getRenderPass(), lightingRenderPass,
                forwardPass->getRenderPass(), transparentPass->getRenderPass(),
                glassDepthPass->getRenderPass(),
                layeredInterfaceCapture_.renderPass(),
                layeredLocalComposition_.renderPass(), outputPass.renderPass(),
                uiPass->getRenderPass() },
            VulkanFrameScheduler::FramesInFlight,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            transparencyPyramidResidency_.enabled(),
            VulkanLayeredGraphConfig{ ordinary2AtlasExtent_,
                hero4AtlasExtent_, cinematic8AtlasExtent_ },
            renderGraph_);
    }

    void VulkanVertexBackend::createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);
        size_t frameCount = VulkanFrameScheduler::FramesInFlight;

        uniformBuffers.resize(frameCount);

        for (size_t i = 0; i < frameCount; i++) {
            uniformBuffers[i] = resourceAllocator.createBuffer(bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::Uniform);
        }
    }

    // ==============================================================================
    // 3. THE FRAME PIPELINE (Data-Driven Execution)
    // ==============================================================================

    FrameStatus VulkanVertexBackend::beginFrame() {
        frameOpen_ = false;
        finalCaptureHookRecorded_ = false;
        ordinary2ViewProjectionValid_ = false;
        collectFrameCounters_ = cpuProfiler_ != nullptr && cpuProfiler_->isFrameOpen();
        if (collectFrameCounters_ && uniqueMaterialIds_.capacity() == 0) {
            uniqueMaterialIds_.reserve(MaxUniqueResourcesPerFrame);
            uniquePipelineIds_.reserve(MaxUniqueResourcesPerFrame);
        }
        resetFrameCounters();
        CpuScope beginFrameScope(cpuProfiler_, "cpu.renderer.begin_frame");
        uploadContext.flush();
        applyTransparencyPyramidTopologyChange();
        const uint32_t completedFrameIndex = scheduler.currentFrameIndex();
        const VulkanFrameBegin frame = scheduler.beginFrame(vkSwapchain->getSwapchain());
        // beginFrame has waited this slot's fence before returning, including
        // the out-of-date acquire path. Its capture readbacks are now CPU-safe.
        collectFrameCapturesForSlot(completedFrameIndex);
        collectOrdinary2CaptureValidationsForSlot(completedFrameIndex);
        collectDeepLayeredCaptureValidationsForSlot(completedFrameIndex);
        collectClusterDiagnostics(completedFrameIndex);
        {
            CpuScope graphScope(cpuProfiler_, "cpu.render_graph.lookup");
            renderGraph_.onFrameFenceCompleted(completedFrameIndex);
            if (!renderGraph_.validateFrame(completedFrameIndex)) {
                throw std::runtime_error(
                    "Graph-owned frame targets failed executor validation");
            }
        }
        if (frame.status == FrameStatus::RecreateSwapchain) {
            return frame.status;
        }

        if (indexedTextureTable_.active()) {
            indexedTextureTable_.ensureFrameCapacity(
                scheduler.currentFrameIndex(),
                indexedTextureTable_.requiredCapacity());
            indexedTextureTable_.synchronizeFrame(
                scheduler.currentFrameIndex());
        }
        uploadCanonicalMaterialsForFrame(scheduler.currentFrameIndex());
        currentImageIndex = frame.imageIndex;
        currentCmd = frame.commandBuffer;
        renderGraph_.beginFrameExecution(scheduler.currentFrameIndex());
        frameOpen_ = true;
        return frame.status;
    }

    uint64_t VulkanVertexBackend::getShadowCasterRevision(
        std::span<const DrawPacket> shadowCasters) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        const auto append = [&hash](const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        };
        for (const DrawPacket& packet : shadowCasters) {
            append(&packet.worldTransform, sizeof(packet.worldTransform));
            append(&packet.geometry.id, sizeof(packet.geometry.id));
            append(&packet.material.id, sizeof(packet.material.id));
            append(&packet.pipeline.id, sizeof(packet.pipeline.id));
            append(&packet.indexCount, sizeof(packet.indexCount));
            append(&packet.firstIndex, sizeof(packet.firstIndex));
            if (const VulkanMaterialPayload* material =
                    materialVault.get(packet.material)) {
                append(&material->packedRevision,
                    sizeof(material->packedRevision));
                append(&material->packed.alphaMode,
                    sizeof(material->packed.alphaMode));
                append(&material->packed.doubleSided,
                    sizeof(material->packed.doubleSided));
            }
        }
        return hash;
    }

    void VulkanVertexBackend::submitDirectionalShadows(
        std::span<const DrawPacket> shadowCasters,
        std::span<const DirectionalShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error(
                "Directional shadows require an open frame");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        directionalShadow_.updateFrame(frameIndex, shadows);
        const bool hasUpdates = std::any_of(shadows.begin(), shadows.end(),
            [](const DirectionalShadowFramePacket& shadow) {
                return shadow.updateMask != 0u;
            });
        if (!hasUpdates) {
            renderGraph_.skipPass("shadow.directional");
            return;
        }
        renderGraph_.beginPass(currentCmd, "shadow.directional");
        for (const DirectionalShadowFramePacket& shadow : shadows)
            if (shadow.resolution != directionalShadow_.resolution())
                throw std::invalid_argument(
                    "Directional shadow packet resolution does not match storage");

        CpuScope recordScope(cpuProfiler_, "cpu.render.record.shadow.directional");
        const VkPipelineLayout layout = directionalShadow_.pipelineLayout();
        const VkDescriptorSet shadowSet =
            directionalShadow_.renderDescriptor(frameIndex);
        VulkanGpuRangeToken gpuRange =
            scheduler.beginGpuRange("gpu.shadow.directional");

        for (const DirectionalShadowFramePacket& shadow : shadows) {
          for (uint32_t cascade = 0;
              cascade < kDirectionalShadowCascadeCount; ++cascade) {
            if ((shadow.updateMask & (1u << cascade)) == 0u) continue;
            directionalShadow_.beginCascade(currentCmd,
                shadow.shadowIndex, cascade);
            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                layout, 0, 1, &shadowSet, 0, nullptr);

            VkPipeline activePipeline = VK_NULL_HANDLE;
            GeometryHandle activeGeometry{};
            bool materialDescriptorsBound = false;
            for (const DrawPacket& packet : shadowCasters) {
                VulkanGeometryPayload* geometry = geometryVault.get(packet.geometry);
                VulkanMaterialPayload* material = materialVault.get(packet.material);
                if (geometry == nullptr || material == nullptr) continue;
                const bool alphaMasked = material->packed.alphaMode == 1u;
                const bool doubleSided = material->packed.doubleSided != 0u;
                const VkPipeline pipeline = directionalShadow_.pipeline(
                    alphaMasked, doubleSided);
                if (pipeline != activePipeline) {
                    vkCmdBindPipeline(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    activePipeline = pipeline;
                }
                if (alphaMasked && !materialDescriptorsBound) {
                    bindMaterialDescriptors(layout);
                    materialDescriptorsBound = true;
                }
                if (packet.geometry != activeGeometry) {
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    activeGeometry = packet.geometry;
                }
                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = shadow.shadowIndex *
                    kDirectionalShadowCascadeCount + cascade;
                vkCmdPushConstants(currentCmd, layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1,
                    packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawShadowDirectional,
                    packet.indexCount / 3u);
                if (collectFrameCounters_ && alphaMasked)
                    ++frameCounters_.drawShadowDirectionalAlphaMask;
            }
            directionalShadow_.endCascade(currentCmd);
          }
        }
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::submitSpotShadows(
        std::span<const DrawPacket> shadowCasters,
        std::span<const SpotShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error("Spot shadows require an open frame");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        spotShadow_.updateFrame(frameIndex, shadows);

        std::vector<uint32_t> nextMapping(lightRecordCapacity_,
            kInvalidShadowDataSlot);
        for (const SpotShadowFramePacket& shadow : shadows) {
            if (shadow.lightSlot >= lightRecordCapacity_ ||
                shadow.shadowDataSlot >= kSpotShadowEntryCapacity)
                throw std::out_of_range(
                    "Spot shadow light or data slot is invalid");
            if (shadow.sampleable)
                nextMapping[shadow.lightSlot] = shadow.shadowDataSlot;
        }
        if (nextMapping != spotShadowDataSlots_) {
            spotShadowDataSlots_ = std::move(nextMapping);
            ++spotShadowMappingRevision_;
            if (spotShadowMappingRevision_ == 0u)
                ++spotShadowMappingRevision_;
        }

        const bool hasUpdates = std::ranges::any_of(shadows,
            [](const SpotShadowFramePacket& shadow) { return shadow.update; });
        if (!hasUpdates) {
            renderGraph_.skipPass("shadow.spot");
            return;
        }
        renderGraph_.beginPass(currentCmd, "shadow.spot");
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.shadow.spot");
        const VkPipelineLayout layout = spotShadow_.pipelineLayout();
        const VkDescriptorSet shadowSet =
            spotShadow_.renderDescriptor(frameIndex);
        VulkanGpuRangeToken gpuRange =
            scheduler.beginGpuRange("gpu.shadow.spot");
        for (const SpotShadowFramePacket& shadow : shadows) {
            if (!shadow.update) continue;
            spotShadow_.beginTile(currentCmd, shadow);
            vkCmdBindDescriptorSets(currentCmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                0, 1, &shadowSet, 0, nullptr);
            VkPipeline activePipeline = VK_NULL_HANDLE;
            GeometryHandle activeGeometry{};
            bool materialDescriptorsBound = false;
            for (const DrawPacket& packet : shadowCasters) {
                if (collectFrameCounters_)
                    ++frameCounters_.shadowSpotCastersTested;
                if (!shadowCasterSphereIntersectsClipVolume(
                        shadow.worldToShadowClip,
                        packet.boundsSphereCenterWorld,
                        packet.boundsSphereRadiusWorld)) {
                    if (collectFrameCounters_)
                        ++frameCounters_.shadowSpotCastersCulled;
                    continue;
                }
                VulkanGeometryPayload* geometry = geometryVault.get(packet.geometry);
                VulkanMaterialPayload* material = materialVault.get(packet.material);
                if (geometry == nullptr || material == nullptr) continue;
                const bool alphaMasked = material->packed.alphaMode == 1u;
                const bool doubleSided = material->packed.doubleSided != 0u;
                const VkPipeline pipeline = spotShadow_.pipeline(
                    alphaMasked, doubleSided);
                if (pipeline != activePipeline) {
                    vkCmdBindPipeline(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    activePipeline = pipeline;
                }
                if (alphaMasked && !materialDescriptorsBound) {
                    bindMaterialDescriptors(layout);
                    materialDescriptorsBound = true;
                }
                if (packet.geometry != activeGeometry) {
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    activeGeometry = packet.geometry;
                }
                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = shadow.shadowDataSlot;
                vkCmdPushConstants(currentCmd, layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1,
                    packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawShadowSpot,
                    packet.indexCount / 3u);
                if (collectFrameCounters_ && alphaMasked)
                    ++frameCounters_.drawShadowSpotAlphaMask;
            }
            spotShadow_.endTile(currentCmd);
        }
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::submitPointShadows(
        std::span<const DrawPacket> shadowCasters,
        std::span<const PointShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error("Point shadows require an open frame");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        pointShadow_.updateFrame(frameIndex, shadows);

        std::vector<uint32_t> nextMapping(lightRecordCapacity_,
            kInvalidShadowDataSlot);
        for (const PointShadowFramePacket& shadow : shadows) {
            if (shadow.lightSlot >= lightRecordCapacity_ ||
                shadow.shadowDataSlot >= kPointShadowEntryCapacity)
                throw std::out_of_range(
                    "Point shadow light or data slot is invalid");
            if (shadow.sampleable)
                nextMapping[shadow.lightSlot] = shadow.shadowDataSlot;
        }
        if (nextMapping != pointShadowDataSlots_) {
            pointShadowDataSlots_ = std::move(nextMapping);
            ++pointShadowMappingRevision_;
            if (pointShadowMappingRevision_ == 0u)
                ++pointShadowMappingRevision_;
        }

        const bool hasUpdates = std::ranges::any_of(shadows,
            [](const PointShadowFramePacket& shadow) { return shadow.update; });
        if (!hasUpdates) {
            renderGraph_.skipPass("shadow.point");
            return;
        }
        renderGraph_.beginPass(currentCmd, "shadow.point");
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.shadow.point");
        const VkPipelineLayout layout = pointShadow_.pipelineLayout();
        const VkDescriptorSet shadowSet =
            pointShadow_.renderDescriptor(frameIndex);
        VulkanGpuRangeToken gpuRange =
            scheduler.beginGpuRange("gpu.shadow.point");
        for (const PointShadowFramePacket& shadow : shadows) {
            if (!shadow.update) continue;
            for (uint32_t face = 0; face < 6u; ++face) {
                pointShadow_.beginFace(currentCmd, shadow, face);
                vkCmdBindDescriptorSets(currentCmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    0, 1, &shadowSet, 0, nullptr);
                VkPipeline activePipeline = VK_NULL_HANDLE;
                GeometryHandle activeGeometry{};
                bool materialDescriptorsBound = false;
                for (const DrawPacket& packet : shadowCasters) {
                    if (collectFrameCounters_)
                        ++frameCounters_.shadowPointCastersTested;
                    if (!shadowCasterSphereIntersectsClipVolume(
                            shadow.worldToShadowClip[face],
                            packet.boundsSphereCenterWorld,
                            packet.boundsSphereRadiusWorld)) {
                        if (collectFrameCounters_)
                            ++frameCounters_.shadowPointCastersCulled;
                        continue;
                    }
                    VulkanGeometryPayload* geometry =
                        geometryVault.get(packet.geometry);
                    VulkanMaterialPayload* material =
                        materialVault.get(packet.material);
                    if (geometry == nullptr || material == nullptr) continue;
                    const bool alphaMasked = material->packed.alphaMode == 1u;
                    const bool doubleSided = material->packed.doubleSided != 0u;
                    const VkPipeline pipeline = pointShadow_.pipeline(
                        alphaMasked, doubleSided);
                    if (pipeline != activePipeline) {
                        vkCmdBindPipeline(currentCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                        activePipeline = pipeline;
                    }
                    if (alphaMasked && !materialDescriptorsBound) {
                        bindMaterialDescriptors(layout);
                        materialDescriptorsBound = true;
                    }
                    if (packet.geometry != activeGeometry) {
                        const VkDeviceSize offset = 0;
                        vkCmdBindVertexBuffers(currentCmd, 0, 1,
                            &geometry->vertexBuffer.buffer, &offset);
                        vkCmdBindIndexBuffer(currentCmd,
                            geometry->indexBuffer.buffer, 0,
                            toVkIndexType(geometry->indexFormat));
                        activeGeometry = packet.geometry;
                    }
                    CanonicalMeshPushConstants push{};
                    push.renderMatrix = packet.worldTransform;
                    push.materialIndex = packet.material.getIndex();
                    push.padding[0] = shadow.shadowDataSlot * 6u + face;
                    vkCmdPushConstants(currentCmd, layout,
                        VK_SHADER_STAGE_VERTEX_BIT |
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(push), &push);
                    vkCmdDrawIndexed(currentCmd, packet.indexCount, 1,
                        packet.firstIndex, 0, 0);
                    recordDraw(frameCounters_.drawShadowPoint,
                        packet.indexCount / 3u);
                    if (collectFrameCounters_ && alphaMasked)
                        ++frameCounters_.drawShadowPointAlphaMask;
                }
                pointShadow_.endFace(currentCmd);
            }
        }
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::submitReflectionProbeCaptures(
        std::span<const DrawPacket> opaqueCasters,
        std::span<const DrawPacket> complexOpaqueCasters,
        std::span<const ReflectionProbeCaptureScheduleEntry> captures,
        const LightingFramePacket& lights) {
        if (!frameOpen_)
            throw std::logic_error(
                "Reflection-probe capture requires an open frame");
        const bool hasWork = std::ranges::any_of(captures,
            [](const ReflectionProbeCaptureScheduleEntry& capture) {
                return capture.scheduledFaceMask != 0u;
            });
        if (!hasWork) return;
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        uploadLightsForFrame(frameIndex, lights);
        const VkDescriptorSet sceneSet = sceneDescriptors.get(frameIndex);
        const VkPipelineLayout layout =
            reflectionProbeCapturePass_.graphicsLayout();
        uint32_t faceRecord = 0;
        VulkanGpuRangeToken captureRange =
            scheduler.beginGpuRange("gpu.probe.capture");
        for (const ReflectionProbeCaptureScheduleEntry& capture : captures) {
            if (capture.scheduledFaceMask == 0u) continue;
            const VulkanReflectionProbeCaptureStaging& target =
                reflectionProbeCaptureTargets_.acquire(capture.owner,
                    capture.captureTicket, capture.resolution);
            for (uint32_t face = 0;
                face < kReflectionProbeCaptureFaceCount; ++face) {
                const uint8_t bit = static_cast<uint8_t>(1u << face);
                if ((capture.scheduledFaceMask & bit) == 0u) continue;
                if (faceRecord >=
                    VulkanReflectionProbeCapturePass::MaximumFaceRecords)
                    throw std::overflow_error(
                        "Reflection-probe capture face records are exhausted");
                reflectionProbeCapturePass_.writeFace(frameIndex, faceRecord,
                    capture.faces[face], capture.position,
                    capture.nearPlane, lights.stats.activeLightCount,
                    capture.captureSky, capture.resolution);
                reflectionProbeCapturePass_.beginFace(currentCmd, target,
                    face, frameIndex, faceRecord, sceneSet);
                reflectionProbeCapturePass_.bindFaceDescriptors(currentCmd,
                    frameIndex, faceRecord, sceneSet);
                bindMaterialDescriptors(layout);
                VkPipeline activePipeline = VK_NULL_HANDLE;
                GeometryHandle activeGeometry{};
                const auto renderQueue = [&](std::span<const DrawPacket> queue) {
                    for (const DrawPacket& packet : queue) {
                        if (packet.owner == capture.owner) continue;
                        if (!shadowCasterSphereIntersectsClipVolume(
                                capture.faces[face].worldToClip,
                                packet.boundsSphereCenterWorld,
                                packet.boundsSphereRadiusWorld))
                            continue;
                        VulkanGeometryPayload* geometry =
                            geometryVault.get(packet.geometry);
                        VulkanMaterialPayload* material =
                            materialVault.get(packet.material);
                        if (geometry == nullptr || material == nullptr) continue;
                        const VkPipeline pipeline =
                            reflectionProbeCapturePass_.pipeline(
                                material->packed.alphaMode == 1u,
                                material->packed.doubleSided != 0u);
                        if (pipeline != activePipeline) {
                            vkCmdBindPipeline(currentCmd,
                                VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                            activePipeline = pipeline;
                        }
                        if (packet.geometry != activeGeometry) {
                            const VkDeviceSize offset = 0;
                            vkCmdBindVertexBuffers(currentCmd, 0, 1,
                                &geometry->vertexBuffer.buffer, &offset);
                            vkCmdBindIndexBuffer(currentCmd,
                                geometry->indexBuffer.buffer, 0,
                                toVkIndexType(geometry->indexFormat));
                            activeGeometry = packet.geometry;
                        }
                        CanonicalMeshPushConstants push{};
                        push.renderMatrix = packet.worldTransform;
                        push.materialIndex = packet.material.getIndex();
                        vkCmdPushConstants(currentCmd, layout,
                            VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(push), &push);
                        vkCmdDrawIndexed(currentCmd, packet.indexCount, 1,
                            packet.firstIndex, 0, 0);
                    }
                };
                renderQueue(opaqueCasters);
                renderQueue(complexOpaqueCasters);
                reflectionProbeCapturePass_.endFace(currentCmd);
                ++faceRecord;
                ++reflectionProbeCaptureTelemetry_.facesRendered;
                reflectionProbeCaptureTelemetry_.renderedTexels +=
                    static_cast<uint64_t>(capture.resolution) *
                    capture.resolution;
            }
            const uint8_t completedMask = static_cast<uint8_t>(
                capture.capturedFaceMask | capture.scheduledFaceMask);
            if (completedMask == kReflectionProbeCaptureCompleteMask) {
                const auto duplicate = std::ranges::find_if(
                    pendingReflectionProbeCaptures_,
                    [&](const PendingReflectionProbeCapture& pending) {
                        return pending.owner == capture.owner;
                    });
                if (duplicate != pendingReflectionProbeCaptures_.end())
                    throw std::logic_error(
                        "Reflection-probe capture publication is duplicated");
                PendingReflectionProbeCapture pending{
                    .owner = capture.owner,
                    .captureTicket = capture.captureTicket,
                    .filterDescriptors =
                        reflectionProbeCapturePass_.recordPrefilter(
                            currentCmd, target,
                            reflectionProbePrefilterSampleCount_),
                    .resolution = target.resolution,
                    .mipLevels = target.mipLevels,
                };
                if (capture.updateMode == ReflectionProbeUpdateMode::Baked)
                    pending.bakedReadback =
                        reflectionProbeCapturePass_.recordReadback(
                            currentCmd, target);
                pendingReflectionProbeCaptures_.push_back(
                    std::move(pending));
                ++reflectionProbeCaptureTelemetry_.capturesFiltered;
            }
        }
        scheduler.endGpuRange(captureRange);
        reflectionProbeCaptureTelemetry_.capturesInFlight =
            reflectionProbeCaptureTargets_.capturesInFlight();
        reflectionProbeCaptureTelemetry_.stagingLogicalBytes =
            reflectionProbeCaptureTargets_.stagingLogicalBytes();
        reflectionProbeCaptureTelemetry_.publishedLogicalBytes =
            reflectionProbeCaptureTargets_.publishedLogicalBytes();
    }

    void VulkanVertexBackend::submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> selectionQueue, bool isWireframe) {
        selectionOutlineActive_ = !selectionQueue.empty();
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.gbuffer");
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        renderGraph_.beginPass(currentCmd, "gbuffer");
        VkRenderPassBeginInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpInfo.renderPass = gBufferPass->getRenderPass();
        rpInfo.framebuffer = frameTargets.get(
            scheduler.currentFrameIndex()).gBufferFramebuffer;
        rpInfo.renderArea.extent = frameTargets.extent();

        std::array<VkClearValue, 6> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Normal
        clearValues[1].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Diffuse / albedo
        clearValues[2].color = { {0.0f, 0.0f, 0.0f, 0.0f} }; // Emissive
        clearValues[3].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // F0 / roughness
        clearValues[4].color.uint32[0] = 0u;                  // Material / flags
        clearValues[5].depthStencil = { 1.0f, 0 };
        rpInfo.clearValueCount = 6;
        rpInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(currentCmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Dynamic Viewport/Scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;            // Start at the bottom
        viewport.width = (float)frameTargets.extent().width;
        viewport.height = (float)frameTargets.extent().height;       // Draw upwards!
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        vkCmdSetViewport(currentCmd, 0, 1, &viewport);
        VkRect2D scissor{ {0, 0}, rpInfo.renderArea.extent };
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);

        const VkPipelineLayout meshLayout = meshLayouts.getGBufferPipelineLayout();

        // ==============================================================================
        // PHASE 1: DRAW OPAQUE SCENE
        // ==============================================================================

        VulkanGpuRangeToken opaqueGpuRange =
            scheduler.beginGpuRange("gpu.gbuffer.opaque");

        PipelineHandle lastBoundPipeline{};
        MaterialHandle lastBoundMaterial{};
        GeometryHandle lastBoundGeometry{};

        if (isWireframe) {
            // Editor wireframe is a deliberate fixed override, not a material PSO.
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getWireframePipeline());
            recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::GBufferWireframe));
            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);

            for (const auto& packet : opaqueQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                if (!geometry || !material) continue;

                if (packet.material != lastBoundMaterial) {
                    bindMaterialDescriptors(meshLayout);
                    recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = static_cast<uint32_t>(debugView_);
                vkCmdPushConstants(currentCmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawOpaque, packet.indexCount / 3);
            }
        }
        else {
            VkPipelineLayout activeLayout = VK_NULL_HANDLE;

            for (const auto& packet : opaqueQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                const VulkanPipelineRecord* record = pipelineLibrary.get(packet.pipeline);
                if (!geometry || !material) continue;

                // Invalid/stale handles and non-G-buffer records are not drawable here.
                if (!record || record->pipeline == VK_NULL_HANDLE ||
                    record->pipelineLayout == VK_NULL_HANDLE ||
                    record->renderPass != RenderPassClass::GBuffer) {
                    continue;
                }

                if (packet.pipeline != lastBoundPipeline) {
                    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
                    recordPipelineBind(packet.pipeline.id);
                    activeLayout = record->pipelineLayout;
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    lastBoundPipeline = packet.pipeline;
                    lastBoundMaterial = MaterialHandle{};
                }
                if (packet.material != lastBoundMaterial) {
                    bindMaterialDescriptors(activeLayout);
                    recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = static_cast<uint32_t>(debugView_);
                vkCmdPushConstants(currentCmd, activeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawOpaque, packet.indexCount / 3);
            }
        }
        scheduler.endGpuRange(opaqueGpuRange);

        // ==============================================================================
        // PHASE 2: DRAW SELECTION MASKS (Depth Testing Disabled = X-Ray)
        // ==============================================================================

        if (!selectionQueue.empty()) {
            VulkanGpuRangeToken selectionGpuRange =
                scheduler.beginGpuRange("gpu.gbuffer.selection");
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getOutlinePipeline());
            recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::SelectionMask));
            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);

            lastBoundMaterial = MaterialHandle{};
            lastBoundGeometry = GeometryHandle{};

            for (const auto& packet : selectionQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);

                if (!geometry || !material) continue;

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();

                if (packet.material != lastBoundMaterial) {
                    bindMaterialDescriptors(meshLayout);
                    recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                vkCmdPushConstants(currentCmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);

                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawSelection, packet.indexCount / 3);
            }
            scheduler.endGpuRange(selectionGpuRange);
        }

        vkCmdEndRenderPass(currentCmd);

    }

    void VulkanVertexBackend::createCanonicalMaterialBuffers(
        uint32_t capacity) {
        if (capacity == 0 ||
            capacity > canonicalMaterialMaximumCapacity_) {
            throw std::invalid_argument(
                "canonical material buffer capacity is outside the device limit");
        }
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(capacity) *
            sizeof(PackedGpuMaterial);
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight>
            replacement{};
        try {
            for (VulkanBufferResource& buffer :
                replacement) {
                buffer = resourceAllocator.createBuffer(bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true,
                    ProfileMemoryCategory::MaterialGpu);
            }
        }
        catch (...) {
            for (VulkanBufferResource& buffer :
                replacement) {
                resourceAllocator.destroy(buffer);
            }
            throw;
        }

        if (canonicalMaterialCapacity_ != 0) {
            if (frameOpen_) {
                for (VulkanBufferResource& buffer :
                    replacement) {
                    resourceAllocator.destroy(buffer);
                }
                throw std::logic_error(
                    "canonical material buffers may grow only at a frame boundary");
            }
            scheduler.waitForAllFrames();
        }
        for (VulkanBufferResource& buffer :
            canonicalMaterialBuffers_) {
            resourceAllocator.destroy(buffer);
        }
        canonicalMaterialBuffers_ = replacement;
        canonicalMaterialCapacity_ = capacity;
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight;
            ++frame) {
            indexedTextureTable_.bindMaterialBuffer(
                frame,
                canonicalMaterialBuffers_[frame].buffer,
                canonicalMaterialBuffers_[frame].size);
        }
        materialVault.forEach(
            [](VulkanMaterialPayload& material) {
                material.uploadedPackedRevisions.fill(0);
            });
    }

    void VulkanVertexBackend::ensureCanonicalMaterialCapacity(
        uint32_t requiredCapacity) {
        if (requiredCapacity <=
            canonicalMaterialCapacity_) {
            return;
        }
        if (requiredCapacity >
            canonicalMaterialMaximumCapacity_) {
            throw std::overflow_error(
                "canonical material table exhausted the device storage-buffer limit");
        }
        createCanonicalMaterialBuffers(
            nextMaterialTableCapacity(
                canonicalMaterialCapacity_,
                requiredCapacity,
                canonicalMaterialMaximumCapacity_));
    }

    void VulkanVertexBackend::uploadCanonicalMaterialsForFrame(uint32_t frameIndex) {
        if (frameIndex >= canonicalMaterialBuffers_.size())
            throw std::out_of_range("canonical material frame index is invalid");
        VulkanBufferResource& buffer = canonicalMaterialBuffers_[frameIndex];
        materialVault.forEachIndexed([&](MaterialHandle handle,
            VulkanMaterialPayload& material) {
            if (handle.getIndex() >=
                canonicalMaterialCapacity_)
                throw std::overflow_error("canonical material table capacity exceeded");
            uint64_t& uploadedRevision =
                material.uploadedPackedRevisions[frameIndex];
            uint64_t nextUploadedRevision = uploadedRevision;
            if (!consumeMaterialUploadRevision(material.packedRevision,
                nextUploadedRevision)) return;
            resourceAllocator.write(buffer,
                static_cast<VkDeviceSize>(handle.getIndex()) * sizeof(PackedGpuMaterial),
                std::as_bytes(std::span(&material.packed, size_t{ 1 })));
            uploadedRevision = nextUploadedRevision;
        });
    }

    void VulkanVertexBackend::createLightRecordBuffers(uint32_t capacity) {
        if (capacity == 0 || capacity > lightRecordMaximumCapacity_) {
            throw std::invalid_argument(
                "GPU light record capacity is outside the device limit");
        }
        if (frameOpen_) {
            throw std::logic_error(
                "GPU light record buffers may grow only at a frame boundary");
        }
        const VkDeviceSize recordBytes = static_cast<VkDeviceSize>(capacity) *
            sizeof(PackedGpuLight);
        const VkDeviceSize activeBytes = static_cast<VkDeviceSize>(capacity) *
            sizeof(uint32_t);
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> recordReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> activeReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> fallbackReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> parameterReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> readbackReplacement{};
        const bool createParameters = !clusterParameterBuffers_[0].isValid();
        try {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                recordReplacement[frame] = resourceAllocator.createBuffer(recordBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::LightGpu);
                activeReplacement[frame] = resourceAllocator.createBuffer(activeBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::LightGpu);
                if (createParameters) {
                    fallbackReplacement[frame] = resourceAllocator.createBuffer(
                        kMaximumClusterFallbackLights * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::LightGpu);
                    parameterReplacement[frame] = resourceAllocator.createBuffer(
                        sizeof(PackedGpuClusterParameters),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::LightGpu);
                    readbackReplacement[frame] = resourceAllocator.createBuffer(
                        64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::LightGpu);
                }
                std::memset(recordReplacement[frame].mapped, 0,
                    static_cast<size_t>(recordBytes));
                std::memset(activeReplacement[frame].mapped, 0,
                    static_cast<size_t>(activeBytes));
                if (createParameters) {
                    std::memset(fallbackReplacement[frame].mapped, 0xff,
                        kMaximumClusterFallbackLights * sizeof(uint32_t));
                    std::memset(parameterReplacement[frame].mapped, 0,
                        sizeof(PackedGpuClusterParameters));
                    std::memset(readbackReplacement[frame].mapped, 0, 64);
                }
            }
        }
        catch (...) {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                resourceAllocator.destroy(recordReplacement[frame]);
                resourceAllocator.destroy(activeReplacement[frame]);
                resourceAllocator.destroy(fallbackReplacement[frame]);
                resourceAllocator.destroy(parameterReplacement[frame]);
                resourceAllocator.destroy(readbackReplacement[frame]);
            }
            throw;
        }
        if (lightRecordCapacity_ != 0) scheduler.waitForAllFrames();
        clusteredLighting_.clearDescriptors();
        for (VulkanBufferResource& buffer : lightRecordBuffers_) {
            resourceAllocator.destroy(buffer);
        }
        for (VulkanBufferResource& buffer : activeLightSlotBuffers_)
            resourceAllocator.destroy(buffer);
        lightRecordBuffers_ = recordReplacement;
        activeLightSlotBuffers_ = activeReplacement;
        if (createParameters) {
            fallbackCandidateBuffers_ = fallbackReplacement;
            clusterParameterBuffers_ = parameterReplacement;
            clusterDiagnosticReadbackBuffers_ = readbackReplacement;
        }
        lightRecordCapacity_ = capacity;
        for (std::vector<uint64_t>& revisions : uploadedLightRevisions_) {
            revisions.assign(capacity, uint64_t{ 0 });
        }
        uploadedActiveListRevisions_.fill(0);
        uploadedSpotShadowMappingRevisions_.fill(0);
        uploadedPointShadowMappingRevisions_.fill(0);
        clusterDiagnosticReadbackPending_.fill(false);
        lightUploadRanges_.reserve(capacity);
        fallbackSelectionScratch_.reserve(capacity);
        if (sceneDescriptors.size() != 0) {
            bindLightRecordBuffers();
            bindSceneClusterBuffers();
        }
        bindClusterBuffers();
    }

    void VulkanVertexBackend::bindLightRecordBuffers() {
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> descriptors{};
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            descriptors[frame].buffer = lightRecordBuffers_[frame].buffer;
            descriptors[frame].range = lightRecordBuffers_[frame].size;
        }
        sceneDescriptors.setLightBuffers(descriptors);
    }

    void VulkanVertexBackend::bindClusterBuffers() {
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> records{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> active{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> fallbackCandidates{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> parameters{};
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            records[frame] = { lightRecordBuffers_[frame].buffer, 0,
                lightRecordBuffers_[frame].size };
            active[frame] = { activeLightSlotBuffers_[frame].buffer, 0,
                activeLightSlotBuffers_[frame].size };
            fallbackCandidates[frame] = {
                fallbackCandidateBuffers_[frame].buffer, 0,
                fallbackCandidateBuffers_[frame].size };
            parameters[frame] = { clusterParameterBuffers_[frame].buffer, 0,
                sizeof(PackedGpuClusterParameters) };
        }
        clusteredLighting_.rebuildDescriptors(renderGraph_, records, active,
            fallbackCandidates, parameters);
    }

    void VulkanVertexBackend::bindSceneClusterBuffers() {
        std::array<VulkanClusterSceneBufferDescriptors,
            VulkanFrameScheduler::FramesInFlight> descriptors{};
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            const auto info = [](const VulkanBufferResource& buffer) {
                return VkDescriptorBufferInfo{ buffer.buffer, 0, buffer.size };
            };
            descriptors[frame] = {
                info(renderGraph_.bufferResource(frame,
                    kClusterGlobalResourceName)),
                info(renderGraph_.bufferResource(frame,
                    kClusterHeaderResourceName)),
                info(renderGraph_.bufferResource(frame,
                    kClusterIndexResourceName)),
                info(renderGraph_.bufferResource(frame,
                    kClusterFallbackResourceName)),
                info(renderGraph_.bufferResource(frame,
                    kClusterDiagnosticResourceName)),
                { clusterParameterBuffers_[frame].buffer, 0,
                    sizeof(PackedGpuClusterParameters) },
            };
        }
        sceneDescriptors.setClusterBuffers(descriptors);
    }

    void VulkanVertexBackend::createReflectionProbeBuffers(
        uint32_t recordCapacity, uint32_t clusterCapacity,
        uint32_t referenceCapacity) {
        if (recordCapacity == 0 ||
            recordCapacity > reflectionProbeRecordMaximumCapacity_ ||
            clusterCapacity == 0 || referenceCapacity == 0 ||
            referenceCapacity > kMaximumClusterProbeReferences)
            throw std::invalid_argument(
                "Reflection-probe GPU capacity is invalid");
        if (frameOpen_)
            throw std::logic_error(
                "Reflection-probe buffers may grow only at a frame boundary");
        const VkDeviceSize recordBytes = static_cast<VkDeviceSize>(
            recordCapacity) * sizeof(PackedGpuReflectionProbe);
        const VkDeviceSize activeBytes = static_cast<VkDeviceSize>(
            recordCapacity) * sizeof(uint32_t);
        const VkDeviceSize headerBytes = static_cast<VkDeviceSize>(
            clusterCapacity) * sizeof(ClusterLightHeader);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(
            referenceCapacity) * sizeof(uint32_t);
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> records{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> active{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> parameters{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> headers{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> indices{};
        try {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                records[frame] = resourceAllocator.createBuffer(recordBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::Environment);
                active[frame] = resourceAllocator.createBuffer(activeBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::Environment);
                parameters[frame] = resourceAllocator.createBuffer(
                    sizeof(PackedGpuReflectionProbeClusterParameters),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::Environment);
                headers[frame] = resourceAllocator.createBuffer(headerBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    false, ProfileMemoryCategory::Environment);
                indices[frame] = resourceAllocator.createBuffer(indexBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    false, ProfileMemoryCategory::Environment);
                std::memset(records[frame].mapped, 0,
                    static_cast<size_t>(recordBytes));
                std::memset(active[frame].mapped, 0,
                    static_cast<size_t>(activeBytes));
                std::memset(parameters[frame].mapped, 0,
                    sizeof(PackedGpuReflectionProbeClusterParameters));
            }
        }
        catch (...) {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                resourceAllocator.destroy(records[frame]);
                resourceAllocator.destroy(active[frame]);
                resourceAllocator.destroy(parameters[frame]);
                resourceAllocator.destroy(headers[frame]);
                resourceAllocator.destroy(indices[frame]);
            }
            throw;
        }
        if (reflectionProbeRecordCapacity_ != 0) scheduler.waitForAllFrames();
        reflectionProbePipeline_.clearDescriptors();
        for (VulkanBufferResource& buffer : reflectionProbeRecordBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : reflectionProbeActiveSlotBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : reflectionProbeParameterBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer :
                reflectionProbeClusterHeaderBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer :
                reflectionProbeClusterIndexBuffers_)
            resourceAllocator.destroy(buffer);
        reflectionProbeRecordBuffers_ = records;
        reflectionProbeActiveSlotBuffers_ = active;
        reflectionProbeParameterBuffers_ = parameters;
        reflectionProbeClusterHeaderBuffers_ = headers;
        reflectionProbeClusterIndexBuffers_ = indices;
        reflectionProbeRecordCapacity_ = recordCapacity;
        reflectionProbeClusterCapacity_ = clusterCapacity;
        reflectionProbeReferenceCapacity_ = referenceCapacity;
        for (auto& revisions : uploadedReflectionProbeRevisions_)
            revisions.assign(recordCapacity, uint64_t{ 0 });
        uploadedReflectionProbeActiveListRevisions_.fill(0);
        reflectionProbeUploadRanges_.reserve(recordCapacity);
        if (sceneDescriptors.size() != 0) bindReflectionProbeBuffers();
    }

    void VulkanVertexBackend::bindReflectionProbeBuffers() {
        std::array<VulkanReflectionProbeBufferDescriptors,
            VulkanFrameScheduler::FramesInFlight> scene{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> records{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> active{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> parameters{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> headers{};
        std::array<VkDescriptorBufferInfo,
            VulkanFrameScheduler::FramesInFlight> indices{};
        const auto info = [](const VulkanBufferResource& buffer) {
            return VkDescriptorBufferInfo{ buffer.buffer, 0, buffer.size };
        };
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            records[frame] = info(reflectionProbeRecordBuffers_[frame]);
            active[frame] = info(reflectionProbeActiveSlotBuffers_[frame]);
            parameters[frame] = info(reflectionProbeParameterBuffers_[frame]);
            headers[frame] = info(
                reflectionProbeClusterHeaderBuffers_[frame]);
            indices[frame] = info(
                reflectionProbeClusterIndexBuffers_[frame]);
            scene[frame] = { records[frame], headers[frame], indices[frame] };
        }
        sceneDescriptors.setReflectionProbeBuffers(scene);
        reflectionProbePipeline_.rebuildDescriptors(records, active,
            parameters, headers, indices);
    }

    void VulkanVertexBackend::bindReflectionProbeEnvironments() {
        const VulkanTexturePayload* neutral = textureVault.get(
            neutralEnvironmentCube_);
        if (neutral == nullptr || neutral->retired ||
            neutral->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE)
            throw std::logic_error(
                "Neutral reflection-probe environment is unavailable");
        const VkDescriptorImageInfo fallback{ neutral->sampler,
            neutral->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        std::array<VkDescriptorImageInfo,
            kMaximumGpuReflectionProbeEnvironments> images{};
        images.fill(fallback);
        for (size_t index = 0;
            index < reflectionProbeEnvironments_.size(); ++index) {
            const EnvironmentLightingHandles& environment =
                reflectionProbeEnvironments_[index];
            const VulkanTexturePayload* prefiltered = textureVault.get(
                environment.prefilteredSpecular);
            if (prefiltered == nullptr || prefiltered->retired ||
                prefiltered->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
                prefiltered->format != TextureFormat::RGBA16_SFloat)
                throw std::invalid_argument(
                    "Local reflection-probe environment is incompatible");
            images[index] = { prefiltered->sampler,
                prefiltered->image.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        for (const auto& [owner, slot] : capturedReflectionProbeSlots_) {
            if (slot >= images.size())
                throw std::logic_error(
                    "Captured reflection-probe table slot is invalid");
            const VulkanImageResource* published =
                reflectionProbeCaptureTargets_.published(owner);
            if (published == nullptr || !published->isValid())
                throw std::logic_error(
                    "Captured reflection-probe product is unavailable");
            images[slot] = { reflectionProbeCapturePass_.sampler(),
                published->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        sceneDescriptors.setReflectionProbeImages(images);
    }

    std::optional<uint32_t>
    VulkanVertexBackend::capturedReflectionProbeEnvironmentSlot(
        SceneEntityUuid owner) const noexcept {
        const auto found = capturedReflectionProbeSlots_.find(owner);
        return found == capturedReflectionProbeSlots_.end()
            ? std::optional<uint32_t>{}
            : std::optional<uint32_t>{ found->second };
    }

    void VulkanVertexBackend::synchronizeReflectionProbeCaptureOwners(
        std::span<const SceneEntityUuid> owners) {
        if (frameOpen_)
            throw std::logic_error(
                "Reflection-probe owners must synchronize before beginFrame");
        const auto retained = [&](SceneEntityUuid owner) {
            return std::ranges::find(owners, owner) != owners.end();
        };
        const bool hasRemoved = std::ranges::any_of(
            capturedReflectionProbeSlots_,
            [&](const auto& entry) { return !retained(entry.first); });
        if (!hasRemoved) return;
        scheduler.waitForAllFrames();
        for (auto current = capturedReflectionProbeSlots_.begin();
            current != capturedReflectionProbeSlots_.end();) {
            if (retained(current->first)) { ++current; continue; }
            reflectionProbeCaptureTargets_.remove(current->first);
            current = capturedReflectionProbeSlots_.erase(current);
        }
        reflectionProbeCaptureTelemetry_.publishedLogicalBytes =
            reflectionProbeCaptureTargets_.publishedLogicalBytes();
        bindReflectionProbeEnvironments();
    }

    void VulkanVertexBackend::configureReflectionProbeCaptures(
        const ProjectReflectionProbeSettings& settings) {
        if (settings.prefilterSampleCount < 64u ||
            settings.prefilterSampleCount > 1024u)
            throw std::invalid_argument(
                "Reflection-probe prefilter sample count is invalid");
        reflectionProbePrefilterSampleCount_ = settings.prefilterSampleCount;
    }

    std::vector<ReflectionProbeCaptureCompletion>
    VulkanVertexBackend::finalizeReflectionProbeCaptures() {
        if (frameOpen_)
            throw std::logic_error(
                "Reflection-probe captures must finalize before beginFrame");
        std::vector<ReflectionProbeCaptureCompletion> completed;
        if (pendingReflectionProbeCaptures_.empty()) return completed;
        scheduler.waitForAllFrames();
        completed.reserve(pendingReflectionProbeCaptures_.size());
        for (PendingReflectionProbeCapture& pending :
                pendingReflectionProbeCaptures_) {
            reflectionProbeCapturePass_.releaseDescriptors(
                pending.filterDescriptors);
            reflectionProbeCaptureTargets_.promote(
                pending.owner, pending.captureTicket);
            auto found = capturedReflectionProbeSlots_.find(pending.owner);
            if (found == capturedReflectionProbeSlots_.end()) {
                std::array<bool, kMaximumGpuReflectionProbeEnvironments> used{};
                for (const auto& [owner, slot] : capturedReflectionProbeSlots_) {
                    (void)owner;
                    if (slot < used.size()) used[slot] = true;
                }
                uint32_t slot = kInvalidEnvironmentTableSlot;
                for (uint32_t candidate =
                        kMaximumGpuReflectionProbeEnvironments;
                    candidate-- > 0u;) {
                    if (!used[candidate]) { slot = candidate; break; }
                }
                if (slot == kInvalidEnvironmentTableSlot)
                    throw std::overflow_error(
                        "Captured reflection-probe table is exhausted");
                found = capturedReflectionProbeSlots_.emplace(
                    pending.owner, slot).first;
            }
            ReflectionProbeCaptureCompletion completion{
                .owner = pending.owner,
                .captureTicket = pending.captureTicket,
                .environmentSlot = found->second,
            };
            if (pending.bakedReadback.buffer.isValid()) {
                if (pending.bakedReadback.buffer.mapped == nullptr)
                    throw std::logic_error(
                        "Reflection-probe baked readback is not mapped");
                ReflectionProbeCaptureCompletion::Product product{
                    .resolution = pending.resolution,
                    .mipLevels = pending.mipLevels,
                };
                const auto* bytes = static_cast<const std::byte*>(
                    pending.bakedReadback.buffer.mapped);
                product.radiance.assign(bytes,
                    bytes + pending.bakedReadback.radianceBytes);
                product.prefilteredSpecular.assign(
                    bytes + pending.bakedReadback.radianceBytes,
                    bytes + pending.bakedReadback.radianceBytes +
                        pending.bakedReadback.prefilteredBytes);
                completion.bakedProduct = std::move(product);
                resourceAllocator.destroy(pending.bakedReadback.buffer);
            }
            completed.push_back(std::move(completion));
        }
        pendingReflectionProbeCaptures_.clear();
        reflectionProbeCaptureTelemetry_.capturesPublished +=
            static_cast<uint32_t>(completed.size());
        reflectionProbeCaptureTelemetry_.capturesInFlight =
            reflectionProbeCaptureTargets_.capturesInFlight();
        reflectionProbeCaptureTelemetry_.stagingLogicalBytes =
            reflectionProbeCaptureTargets_.stagingLogicalBytes();
        reflectionProbeCaptureTelemetry_.publishedLogicalBytes =
            reflectionProbeCaptureTargets_.publishedLogicalBytes();
        bindReflectionProbeEnvironments();
        return completed;
    }

    void VulkanVertexBackend::prepareReflectionProbes(
        uint32_t requiredCapacity,
        std::span<const EnvironmentLightingHandles> environments) {
        if (!initialized_ || cleaned_)
            throw std::logic_error("Vulkan backend is not initialized");
        if (frameOpen_)
            throw std::logic_error(
                "Reflection probes must be prepared before beginFrame");
        if (requiredCapacity > reflectionProbeRecordMaximumCapacity_)
            throw std::overflow_error(
                "GPU reflection-probe records exhausted the device limit");
        if (environments.size() > kMaximumGpuReflectionProbeEnvironments)
            throw std::overflow_error(
                "Reflection-probe environment table exhausted its capacity");
        for (const auto& [owner, slot] : capturedReflectionProbeSlots_) {
            (void)owner;
            if (slot < environments.size())
                throw std::overflow_error(
                    "Asset and captured reflection-probe table slots overlap");
        }
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            clusterConfig_, { sceneExtent_.width, sceneExtent_.height,
                0.1f, 100.0f, glm::mat4(1.0f), glm::mat4(1.0f) });
        if (dimensions.clusterCount() >
            (std::numeric_limits<uint32_t>::max)())
            throw std::overflow_error(
                "Reflection-probe cluster grid exceeds 32-bit addressing");
        const uint32_t clusterCapacity = static_cast<uint32_t>(
            dimensions.clusterCount());
        const uint32_t referenceCapacity = static_cast<uint32_t>((std::min)(
            dimensions.clusterCount() * kMaximumReflectionProbesPerCluster,
            static_cast<uint64_t>(kMaximumClusterProbeReferences)));
        uint32_t recordCapacity = reflectionProbeRecordCapacity_;
        if (requiredCapacity > recordCapacity)
            recordCapacity = nextMaterialTableCapacity(recordCapacity,
                requiredCapacity, reflectionProbeRecordMaximumCapacity_);
        if (recordCapacity != reflectionProbeRecordCapacity_ ||
            clusterCapacity != reflectionProbeClusterCapacity_ ||
            referenceCapacity != reflectionProbeReferenceCapacity_)
            createReflectionProbeBuffers(recordCapacity, clusterCapacity,
                referenceCapacity);

        const bool environmentsChanged =
            environments.size() != reflectionProbeEnvironments_.size() ||
            !std::equal(environments.begin(), environments.end(),
                reflectionProbeEnvironments_.begin(),
                reflectionProbeEnvironments_.end());
        if (environmentsChanged) {
            for (const EnvironmentLightingHandles& environment : environments)
                if (!environment.isValid())
                    throw std::invalid_argument(
                        "Reflection-probe table contains an invalid environment");
            scheduler.waitForAllFrames();
            reflectionProbeEnvironments_.assign(
                environments.begin(), environments.end());
            bindReflectionProbeEnvironments();
        }
    }

    void VulkanVertexBackend::uploadReflectionProbesForFrame(
        uint32_t frameIndex,
        const ReflectionProbeGpuFramePacket& probes) {
        CpuScope uploadScope(cpuProfiler_, "cpu.probe.upload");
        if (frameIndex >= reflectionProbeRecordBuffers_.size() ||
            probes.records.size() > reflectionProbeRecordCapacity_ ||
            probes.recordRevisions.size() < probes.records.size() ||
            probes.activeSlots.size() > reflectionProbeRecordCapacity_)
            throw std::out_of_range(
                "Reflection-probe packet is outside prepared capacity");
        std::vector<uint64_t>& uploaded =
            uploadedReflectionProbeRevisions_[frameIndex];
        reflectionProbeUploadRanges_.clear();
        uint32_t index = 0;
        while (index < probes.records.size()) {
            if (probes.recordRevisions[index] == uploaded[index]) {
                ++index;
                continue;
            }
            const uint32_t first = index++;
            while (index < probes.records.size() &&
                probes.recordRevisions[index] != uploaded[index]) ++index;
            reflectionProbeUploadRanges_.push_back(
                { first, index - first });
        }
        uint64_t uploadedBytes = 0;
        for (const ReflectionProbeRecordRange range :
                reflectionProbeUploadRanges_) {
            const auto records = probes.records.subspan(
                range.firstRecord, range.recordCount);
            resourceAllocator.write(reflectionProbeRecordBuffers_[frameIndex],
                static_cast<VkDeviceSize>(range.firstRecord) *
                    sizeof(PackedGpuReflectionProbe),
                std::as_bytes(records));
            for (uint32_t slot = range.firstRecord;
                slot < range.firstRecord + range.recordCount; ++slot)
                uploaded[slot] = probes.recordRevisions[slot];
            uploadedBytes += static_cast<uint64_t>(range.recordCount) *
                sizeof(PackedGpuReflectionProbe);
        }
        if (uploadedReflectionProbeActiveListRevisions_[frameIndex] !=
            probes.activeListRevision) {
            if (!probes.activeSlots.empty())
                resourceAllocator.write(
                    reflectionProbeActiveSlotBuffers_[frameIndex], 0,
                    std::as_bytes(probes.activeSlots));
            uploadedReflectionProbeActiveListRevisions_[frameIndex] =
                probes.activeListRevision;
            uploadedBytes += probes.activeSlots.size() * sizeof(uint32_t);
        }
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter("probe.gpu_upload_bytes",
                uploadedBytes, ProfileCounterStatus::Exact,
                ProfileCounterUnit::Bytes);
            cpuProfiler_->recordCounter("probe.gpu_upload_ranges",
                reflectionProbeUploadRanges_.size());
        }
    }

    void VulkanVertexBackend::updateReflectionProbeParameters(
        uint32_t frameIndex, const glm::mat4& view,
        const glm::mat4& projection, float nearPlane, float farPlane,
        uint32_t activeProbeCount) {
        if (frameIndex >= reflectionProbeParameterBuffers_.size() ||
            !(nearPlane > 0.0f) || !(farPlane > nearPlane))
            throw std::invalid_argument(
                "Invalid reflection-probe cluster frame");
        const ClusterFrameParameters frame{ sceneExtent_.width,
            sceneExtent_.height, nearPlane, farPlane, view, projection };
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            clusterConfig_, frame);
        PackedGpuReflectionProbeClusterParameters parameters{};
        parameters.view = view;
        parameters.projection = projection;
        parameters.inverseView = glm::inverse(view);
        parameters.grid = { sceneExtent_.width, sceneExtent_.height,
            dimensions.tilesX, dimensions.tilesY };
        parameters.depth = { nearPlane, farPlane,
            static_cast<float>(clusterConfig_.depthSlices) /
                std::log(farPlane / nearPlane), 0.0f };
        parameters.limits = { clusterConfig_.depthSlices,
            kMaximumReflectionProbesPerCluster,
            reflectionProbeReferenceCapacity_, activeProbeCount };
        parameters.tiles = { clusterConfig_.tileWidth,
            clusterConfig_.tileHeight, 0u, 0u };
        resourceAllocator.write(reflectionProbeParameterBuffers_[frameIndex],
            0, std::as_bytes(std::span(&parameters, size_t{ 1 })));
    }

    void VulkanVertexBackend::prepareLighting(uint32_t requiredCapacity) {
        if (!initialized_ || cleaned_) {
            throw std::logic_error("Vulkan backend is not initialized");
        }
        if (frameOpen_) {
            throw std::logic_error(
                "Lighting capacity must be prepared before beginFrame");
        }
        if (requiredCapacity <= lightRecordCapacity_) return;
        if (requiredCapacity > lightRecordMaximumCapacity_) {
            throw std::overflow_error(
                "GPU light records exhausted the device storage-buffer limit");
        }
        createLightRecordBuffers(nextMaterialTableCapacity(
            lightRecordCapacity_, requiredCapacity,
            lightRecordMaximumCapacity_));
    }

    void VulkanVertexBackend::uploadLightsForFrame(uint32_t frameIndex,
        const LightingFramePacket& lights) {
        CpuScope uploadScope(cpuProfiler_, "cpu.light.upload");
        if (frameIndex >= lightRecordBuffers_.size() ||
            lights.records.size() > lightRecordCapacity_ ||
            lights.recordRevisions.size() < lights.records.size() ||
            lights.selectionMetadata.size() < lights.records.size() ||
            lights.activeSlots.size() > lightRecordCapacity_) {
            throw std::out_of_range("GPU light packet is outside prepared capacity");
        }
        std::vector<uint64_t>& uploaded = uploadedLightRevisions_[frameIndex];
        const bool shadowMappingChanged =
            uploadedSpotShadowMappingRevisions_[frameIndex] !=
                spotShadowMappingRevision_ ||
            uploadedPointShadowMappingRevisions_[frameIndex] !=
                pointShadowMappingRevision_;
        if (shadowMappingChanged) {
            lightUploadRanges_.clear();
            if (!lights.records.empty())
                lightUploadRanges_.push_back({ 0,
                    static_cast<uint32_t>(lights.records.size()) });
        }
        else {
            buildLightUploadRanges(lights.recordRevisions.first(
                lights.records.size()), uploaded, lightUploadRanges_);
        }
        lightUploadBytes_ = 0;
        for (const LightRecordRange range : lightUploadRanges_) {
            const std::span<const PackedGpuLight> records =
                lights.records.subspan(range.firstRecord, range.recordCount);
            patchedLightRecordsScratch_.assign(records.begin(), records.end());
            for (uint32_t index = 0; index < range.recordCount; ++index) {
                const uint32_t slot = range.firstRecord + index;
                const uint32_t type = std::bit_cast<uint32_t>(
                    patchedLightRecordsScratch_[index].shapeMetadata.z) & 3u;
                uint32_t shadowDataSlot = kInvalidShadowDataSlot;
                if (type == static_cast<uint32_t>(
                        PackedGpuLightType::Spot) &&
                    slot < spotShadowDataSlots_.size())
                    shadowDataSlot = spotShadowDataSlots_[slot];
                else if (type == static_cast<uint32_t>(
                        PackedGpuLightType::Point) &&
                    slot < pointShadowDataSlots_.size())
                    shadowDataSlot = pointShadowDataSlots_[slot];
                patchedLightRecordsScratch_[index].shapeMetadata.w =
                    std::bit_cast<float>(shadowDataSlot);
            }
            resourceAllocator.write(lightRecordBuffers_[frameIndex],
                static_cast<VkDeviceSize>(range.firstRecord) *
                    sizeof(PackedGpuLight),
                std::as_bytes(std::span(patchedLightRecordsScratch_)));
            for (uint32_t slot = range.firstRecord;
                slot < range.firstRecord + range.recordCount; ++slot) {
                uploaded[slot] = lights.recordRevisions[slot];
            }
            lightUploadBytes_ += static_cast<uint64_t>(range.recordCount) *
                sizeof(PackedGpuLight);
        }
        uploadedSpotShadowMappingRevisions_[frameIndex] =
            spotShadowMappingRevision_;
        uploadedPointShadowMappingRevisions_[frameIndex] =
            pointShadowMappingRevision_;
        lightUploadRangeCount_ = static_cast<uint32_t>(lightUploadRanges_.size());

        if (uploadedActiveListRevisions_[frameIndex] !=
            lights.activeListRevision) {
            if (!lights.activeSlots.empty()) {
                resourceAllocator.write(activeLightSlotBuffers_[frameIndex], 0,
                    std::as_bytes(lights.activeSlots));
            }
            uploadedActiveListRevisions_[frameIndex] =
                lights.activeListRevision;
            lightUploadBytes_ += static_cast<uint64_t>(lights.activeSlots.size()) *
                sizeof(uint32_t);
            ++lightUploadRangeCount_;
        }
        activeLightCount_ = lights.stats.activeLightCount;
    }

    void VulkanVertexBackend::updateClusterParameters(uint32_t frameIndex,
        const glm::mat4& view, const glm::mat4& projection,
        float nearPlane, float farPlane, uint32_t activeLightCount) {
        if (frameIndex >= clusterParameterBuffers_.size() ||
            !(nearPlane > 0.0f) || !(farPlane > nearPlane)) {
            throw std::invalid_argument("Invalid clustered-lighting frame parameters");
        }
        const ClusterFrameParameters frame{
            sceneExtent_.width, sceneExtent_.height, nearPlane, farPlane,
            view, projection };
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            clusterConfig_, frame);
        const uint64_t clusterCount = dimensions.clusterCount();
        if (clusterCount > (std::numeric_limits<uint32_t>::max)()) {
            throw std::overflow_error("Cluster grid exceeds the GPU index domain");
        }
        PackedGpuClusterParameters parameters{};
        parameters.view = view;
        parameters.projection = projection;
        parameters.grid = { sceneExtent_.width, sceneExtent_.height,
            dimensions.tilesX, dimensions.tilesY };
        parameters.depth = { nearPlane, farPlane,
            static_cast<float>(clusterConfig_.depthSlices) /
                std::log(farPlane / nearPlane), 0.0f };
        parameters.limits = { clusterConfig_.depthSlices,
            clusterConfig_.maximumLightsPerCluster,
            clusterConfig_.maximumLightReferences,
            clusterConfig_.maximumDirectionalLights };
        parameters.input = { activeLightCount,
            clusterConfig_.maximumFallbackLights,
            clusterConfig_.tileWidth, clusterConfig_.tileHeight };
        const uint32_t environmentFlags =
            (environmentLightingSettings_.visibleToCamera ? 1u : 0u) |
            (environmentLightingSettings_.affectsLighting ? 2u : 0u);
        parameters.environment = {
            environmentLightingSettings_.lightingIntensity,
            environmentLightingSettings_.backgroundIntensity,
            environmentLightingSettings_.rotationRadians,
            static_cast<float>(environmentFlags),
        };
        resourceAllocator.write(clusterParameterBuffers_[frameIndex], 0,
            std::as_bytes(std::span(&parameters, size_t{ 1 })));
        lightUploadBytes_ += sizeof(parameters);
        ++lightUploadRangeCount_;
    }

    void VulkanVertexBackend::updateClusterFallbackCandidates(
        uint32_t frameIndex, const glm::mat4& view,
        const LightingFramePacket& lights) {
        CpuScope fallbackScope(cpuProfiler_, "cpu.light.cluster_fallback");
        if (frameIndex >= fallbackCandidateBuffers_.size() ||
            lights.selectionMetadata.size() < lights.records.size()) {
            throw std::out_of_range(
                "Cluster fallback inputs are outside the prepared frame");
        }
        selectClusterFallbackLights(lights, view,
            kMaximumClusterFallbackLights, fallbackSelectionScratch_);
        const size_t selectedCount = fallbackSelectionScratch_.size();
        std::array<uint32_t, kMaximumClusterFallbackLights> selected{};
        selected.fill(UINT32_MAX);
        std::copy_n(fallbackSelectionScratch_.begin(), selectedCount,
            selected.begin());
        resourceAllocator.write(fallbackCandidateBuffers_[frameIndex], 0,
            std::as_bytes(std::span(selected)));
        lightUploadBytes_ += sizeof(selected);
        ++lightUploadRangeCount_;
    }

    void VulkanVertexBackend::collectClusterDiagnostics(
        uint32_t frameIndex) noexcept {
        if (frameIndex >= clusterDiagnosticReadbackBuffers_.size() ||
            !clusterDiagnosticReadbackPending_[frameIndex] ||
            clusterDiagnosticReadbackBuffers_[frameIndex].mapped == nullptr) {
            return;
        }
        std::array<uint32_t, 16> values{};
        std::memcpy(values.data(),
            clusterDiagnosticReadbackBuffers_[frameIndex].mapped,
            sizeof(values));
        const uint64_t clusterCount = submittedClusterCounts_[frameIndex];
        const uint64_t bufferBytesPerFrame =
            static_cast<uint64_t>(clusterConfig_.maximumDirectionalLights) * 4u +
            clusterCount * sizeof(ClusterLightHeader) +
            static_cast<uint64_t>(clusterConfig_.maximumLightReferences) * 4u +
            static_cast<uint64_t>(clusterConfig_.maximumFallbackLights) * 4u +
            64u + clusterCount * 4u + clusterCount * 4u +
            clusterScanScratchElementCount(clusterCount) * 4u + 32u;
        clusterTelemetry_ = {
            .bufferBytesPerFrame = bufferBytesPerFrame,
            .clusterCount = submittedClusterCounts_[frameIndex],
            .activeLights = values[0],
            .directionalLights = values[1],
            .localLights = values[2],
            .clustersUsed = values[6],
            .maximumOccupancy = values[7],
            .requestedReferences = values[4],
            .publishedReferences = values[5],
            .fallbackLights = values[8],
            .droppedLights = values[9],
            .overflowCode = values[3],
            .available = true,
        };
        clusterDiagnosticReadbackPending_[frameIndex] = false;
    }

    void VulkanVertexBackend::updateCamera(const ViewTransportRecord& view) {
        UniformBufferObject ubo{};
        ubo.model = glm::mat4(1.0f); // Handled individually via push constants
        ubo.view = view.view;
        ubo.proj = view.projection;
        ubo.inverseView = view.inverseView;
        ubo.inverseProjection = view.inverseProjection;
        ubo.cameraPosition = view.cameraPosition;
        ubo.depthRange = view.depthRange;
        ubo.renderInfo = view.renderInfo;
        ubo.renderInfo.w &= ~ViewTransportRefractionPyramidsAvailable;
        if (transparencyPyramidResidency_.enabled())
            ubo.renderInfo.w |= ViewTransportRefractionPyramidsAvailable;
        ubo.worldUnits = view.worldUnits;

        // Push the matrices to the GPU!
        std::memcpy(uniformBuffers[scheduler.currentFrameIndex()].mapped, &ubo, sizeof(ubo));
    }

    void VulkanVertexBackend::submitLightingPass(const glm::vec3& cameraPos,
        const glm::mat4& view, const glm::mat4& proj,
        float nearPlane, float farPlane,
        const LightingFramePacket& lights,
        const ReflectionProbeGpuFramePacket& reflectionProbes) {
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.lighting");
        ordinary2ViewProjection_ = proj * view;
        ordinary2ViewProjectionValid_ = true;
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        uploadLightsForFrame(frameIndex, lights);
        updateClusterFallbackCandidates(frameIndex, view, lights);
        updateClusterParameters(frameIndex, view, proj, nearPlane, farPlane,
            lights.stats.activeLightCount);
        uploadReflectionProbesForFrame(frameIndex, reflectionProbes);
        updateReflectionProbeParameters(frameIndex, view, proj,
            nearPlane, farPlane, reflectionProbes.stats.activeProbeCount);
        const ClusterGridDimensions probeDimensions = clusterGridDimensions(
            clusterConfig_, { sceneExtent_.width, sceneExtent_.height,
                nearPlane, farPlane, view, proj });
        {
            CpuScope probeClusterScope(cpuProfiler_,
                "cpu.render.record.probe_cluster");
            VulkanGpuRangeToken probeGpuRange =
                scheduler.beginGpuRange("gpu.lighting.probe_cluster");
            frameCounters_.dispatchRecorded += reflectionProbePipeline_.record(
                currentCmd, frameIndex,
                static_cast<uint32_t>(probeDimensions.clusterCount()));
            scheduler.endGpuRange(probeGpuRange);
        }
        {
            CpuScope clusterRecordScope(cpuProfiler_, "cpu.render.record.cluster");
            const ClusterGridDimensions dimensions = clusterGridDimensions(
                clusterConfig_,
                { sceneExtent_.width, sceneExtent_.height, nearPlane, farPlane,
                    view, proj });
            VulkanGpuRangeToken clusterGpuRange =
                scheduler.beginGpuRange("gpu.lighting.cluster");
            frameCounters_.dispatchRecorded += clusteredLighting_.record(
                currentCmd, renderGraph_, frameIndex,
                static_cast<uint32_t>(dimensions.clusterCount()),
                lights.stats.activeLightCount);
            renderGraph_.beginPass(currentCmd, "lighting.cluster.readback");
            const VulkanBufferResource& diagnostics =
                renderGraph_.bufferResource(frameIndex,
                    kClusterDiagnosticResourceName);
            const VkBufferCopy copy{ 0, 0, 64 };
            vkCmdCopyBuffer(currentCmd, diagnostics.buffer,
                clusterDiagnosticReadbackBuffers_[frameIndex].buffer,
                1, &copy);
            clusterDiagnosticReadbackPending_[frameIndex] = true;
            submittedClusterCounts_[frameIndex] =
                static_cast<uint32_t>(dimensions.clusterCount());
            scheduler.endGpuRange(clusterGpuRange);
        }
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        renderGraph_.beginPass(currentCmd, "lighting");

        VkRenderPassBeginInfo lightingPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        lightingPassInfo.renderPass = lightingRenderPass;
        lightingPassInfo.framebuffer = frameTargets.get(
            scheduler.currentFrameIndex()).lightingFramebuffer;
        lightingPassInfo.renderArea.extent = frameTargets.extent();

        VkClearValue lightingClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        lightingPassInfo.clearValueCount = 1;
        lightingPassInfo.pClearValues = &lightingClearColor;

        vkCmdBeginRenderPass(currentCmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        VulkanGpuRangeToken deferredGpuRange =
            scheduler.beginGpuRange("gpu.lighting.deferred");
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipeline());
        recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::DeferredLighting));

        // Bind the G-Buffer Textures internally managed by the backend
        const VkDescriptorSet sceneSet = sceneDescriptors.get(
            scheduler.currentFrameIndex());
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipelineLayout(),
            0, 1, &sceneSet, 0, nullptr);

        LightingPushConstants push{};
        push.viewPos = glm::vec4(cameraPos, 1.0f);
        push.invView = glm::inverse(view);
        push.invProj = glm::inverse(proj);
        push.debugView = glm::ivec4(static_cast<int32_t>(debugView_), 0, 0, 0);

        vkCmdPushConstants(currentCmd, lightingPipeline->getPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(LightingPushConstants), &push);

        // Draw the full screen triangle without vertex buffers
        vkCmdDraw(currentCmd, 3, 1, 0, 0);
        recordDraw(frameCounters_.drawLighting, 1);
        scheduler.endGpuRange(deferredGpuRange);

        vkCmdEndRenderPass(currentCmd);
    }

    void VulkanVertexBackend::recordOrdinary2InterfaceCapture(
        std::span<const DrawPacket> packets,
        std::span<const Ordinary2CaptureDraw> draws,
        bool exitCapture) {
        const char* passName = exitCapture
            ? "transparent.layered.exit.capture"
            : "transparent.layered.entry.capture";
        if (draws.empty()) {
            renderGraph_.skipPass(passName);
            return;
        }
        if (ordinary2AtlasExtent_.width == 0u ||
            ordinary2AtlasExtent_.height == 0u ||
            layeredInterfaceCapture_.pipeline() == VK_NULL_HANDLE ||
            layeredInterfaceCapture_.descriptorFrameCount() <=
                scheduler.currentFrameIndex()) {
            throw std::logic_error(
                "Ordinary2 capture recording requires resident atlas targets");
        }

        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        const VkFramebuffer framebuffer = exitCapture
            ? targets.layeredExitFramebuffer
            : targets.layeredEntryFramebuffer;
        if (framebuffer == VK_NULL_HANDLE)
            throw std::logic_error(
                "Ordinary2 capture framebuffer is unavailable");

        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(exitCapture
            ? "gpu.transparency.layered.exit.capture"
            : "gpu.transparency.layered.entry.capture");
        renderGraph_.beginPass(currentCmd, passName);
        std::array<VkClearValue, 2> clears{};
        clears[0].color.uint32[0] = 0u;
        clears[1].depthStencil = { 1.0f, 0u };
        VkRenderPassBeginInfo passInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passInfo.renderPass = layeredInterfaceCapture_.renderPass();
        passInfo.framebuffer = framebuffer;
        passInfo.renderArea.extent = ordinary2AtlasExtent_;
        passInfo.clearValueCount = static_cast<uint32_t>(clears.size());
        passInfo.pClearValues = clears.data();
        vkCmdBeginRenderPass(currentCmd, &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);

        const VkPipelineLayout layout =
            layeredInterfaceCapture_.pipelineLayout();
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredInterfaceCapture_.pipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredInterfaceCapture));
        const VkDescriptorSet globalSet = globalDescriptorSets[
            scheduler.currentFrameIndex()];
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        bindMaterialDescriptors(layout);
        const VkDescriptorSet captureSet =
            layeredInterfaceCapture_.descriptorSet(
                scheduler.currentFrameIndex(), exitCapture);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &captureSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        for (const Ordinary2CaptureDraw& draw : draws) {
            if (draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                geometryVault.get(packet.geometry);
            if (geometry == nullptr || materialVault.get(packet.material) == nullptr)
                continue;
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = 0u;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer,
                    0u, toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            LayeredInterfaceCapturePushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.workTableIndex = draw.workTableIndex;
            push.flags |= kLayeredCaptureRequirePairedOrientation;
            if ((packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0u)
                push.flags |= kLayeredCaptureMirrored;
            if (exitCapture)
                push.flags |= kLayeredCaptureHasPrevious;
            push.packedViewportOffset = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(currentCmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            if (exitCapture) {
                recordDraw(frameCounters_.ordinary2CaptureExitDraws,
                    packet.indexCount / 3u);
            }
            else {
                recordDraw(frameCounters_.ordinary2CaptureEntryDraws,
                    packet.indexCount / 3u);
            }
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::recordOrdinary2Captures(
        std::span<const DrawPacket> packets,
        std::span<const Ordinary2CaptureDraw> draws) {
        recordOrdinary2InterfaceCapture(packets, draws, false);
        recordOrdinary2InterfaceCapture(packets, draws, true);
    }

    void VulkanVertexBackend::recordDeepLayeredInterfaceCapture(
        std::span<const DrawPacket> packets,
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality, uint32_t interfaceIndex) {
        const std::span<const std::string_view> passNames = quality ==
                TransparencyQuality::Hero4
            ? std::span<const std::string_view>(Hero4PassNames).first(4u)
            : quality == TransparencyQuality::Cinematic8
                ? std::span<const std::string_view>(
                    Cinematic8PassNames).first(8u)
                : std::span<const std::string_view>{};
        const std::span<const char* const> gpuRanges = quality ==
                TransparencyQuality::Hero4
            ? std::span<const char* const>(Hero4CaptureGpuRanges)
            : quality == TransparencyQuality::Cinematic8
                ? std::span<const char* const>(Cinematic8CaptureGpuRanges)
                : std::span<const char* const>{};
        if (interfaceIndex >= passNames.size() ||
            interfaceIndex >= gpuRanges.size()) {
            throw std::out_of_range(
                "Deep layered interface index is invalid");
        }
        const std::string_view passName = passNames[interfaceIndex];
        const bool hasTierDraws = std::ranges::any_of(draws,
            [quality](const LayeredCaptureDraw& draw) {
                return draw.quality == quality;
            });
        if (!hasTierDraws) {
            renderGraph_.skipPass(passName);
            return;
        }

        const uint32_t frameIndex = scheduler.currentFrameIndex();
        VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        VulkanFrameContextTargets::DeepLayeredTier* tier = quality ==
                TransparencyQuality::Hero4
            ? &targets.hero4 : &targets.cinematic8;
        if (!tier->active() || interfaceIndex >= tier->interfaceCount ||
            layeredInterfaceCapture_.pipeline() == VK_NULL_HANDLE ||
            layeredInterfaceCapture_.descriptorInterfaceCount(
                frameIndex, quality) != tier->interfaceCount) {
            throw std::logic_error(
                "Deep layered capture requires a complete resident tier");
        }
        const VkFramebuffer framebuffer =
            tier->interfaceFramebuffers[interfaceIndex];
        if (framebuffer == VK_NULL_HANDLE)
            throw std::logic_error(
                "Deep layered capture framebuffer is unavailable");

        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            gpuRanges[interfaceIndex]);
        renderGraph_.beginPass(currentCmd, passName);
        std::array<VkClearValue, 2> clears{};
        clears[0].color.uint32[0] = 0u;
        clears[1].depthStencil = { 1.0f, 0u };
        VkRenderPassBeginInfo passInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passInfo.renderPass = layeredInterfaceCapture_.renderPass();
        passInfo.framebuffer = framebuffer;
        passInfo.renderArea.extent = tier->atlasExtent;
        passInfo.clearValueCount = static_cast<uint32_t>(clears.size());
        passInfo.pClearValues = clears.data();
        vkCmdBeginRenderPass(currentCmd, &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);

        const VkPipelineLayout layout =
            layeredInterfaceCapture_.pipelineLayout();
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredInterfaceCapture_.pipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredInterfaceCapture));
        const VkDescriptorSet globalSet = globalDescriptorSets[frameIndex];
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        bindMaterialDescriptors(layout);
        const VkDescriptorSet captureSet =
            layeredInterfaceCapture_.descriptorSet(frameIndex, quality,
                interfaceIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &captureSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        for (const LayeredCaptureDraw& draw : draws) {
            if (draw.quality != quality || draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                geometryVault.get(packet.geometry);
            if (geometry == nullptr ||
                materialVault.get(packet.material) == nullptr) {
                continue;
            }
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = 0u;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            LayeredInterfaceCapturePushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.workTableIndex = draw.workTableIndex;
            if ((packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0u) {
                push.flags |= kLayeredCaptureMirrored;
            }
            if (interfaceIndex != 0u)
                push.flags |= kLayeredCaptureHasPrevious;
            if (interfaceIndex >= 2u)
                push.flags |= kLayeredCaptureHasTerminationMask;
            push.packedViewportOffset = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(currentCmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            recordDraw(frameCounters_.deepLayeredInterfaceDraws,
                packet.indexCount / 3u);
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::recordDeepLayeredCaptures(
        std::span<const DrawPacket> packets,
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality) {
        const uint32_t interfaceCount = layeredQualityTierContract(
            quality).maximumInterfaceCount;
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < interfaceCount; ++interfaceIndex) {
            recordDeepLayeredInterfaceCapture(packets, draws, quality,
                interfaceIndex);
            if (deepLayeredTerminationInterface(interfaceIndex,
                    interfaceCount)) {
                recordDeepLayeredTileTermination(draws, quality,
                    interfaceIndex);
            }
        }
    }

    void VulkanVertexBackend::recordDeepLayeredTileTermination(
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality, uint32_t interfaceIndex) {
        const char* tierName = quality == TransparencyQuality::Hero4
            ? "hero4" : quality == TransparencyQuality::Cinematic8
                ? "cinematic8" : nullptr;
        if (tierName == nullptr)
            throw std::invalid_argument(
                "Tile termination requires Hero4 or Cinematic8");
        const uint32_t interfaceCount = layeredQualityTierContract(
            quality).maximumInterfaceCount;
        if (interfaceIndex >= interfaceCount)
            throw std::out_of_range(
                "Layered tile-termination interface is invalid");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        VulkanFrameContextTargets::DeepLayeredTier& tier = quality ==
                TransparencyQuality::Hero4
            ? targets.hero4 : targets.cinematic8;
        if (!tier.active() ||
            layeredInterfaceCapture_.tileTerminationPipeline() ==
                VK_NULL_HANDLE) {
            throw std::logic_error(
                "Layered tile termination requires a resident tier");
        }
        const std::string passName = std::string(
            "transparent.layered.") + tierName + ".interface." +
            std::to_string(interfaceIndex) + ".terminate-tiles";
        const bool hasTierDraws = std::ranges::any_of(draws,
            [quality](const LayeredCaptureDraw& draw) {
                return draw.quality == quality;
            });
        if (!hasTierDraws) {
            renderGraph_.skipPass(passName);
            return;
        }
        const char* gpuRangeName = quality == TransparencyQuality::Hero4
            ? Hero4TerminationGpuRanges[interfaceIndex]
            : Cinematic8TerminationGpuRanges[interfaceIndex];
        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            gpuRangeName);
        renderGraph_.beginPass(currentCmd, passName);
        layeredInterfaceCapture_.recordTileTermination(currentCmd,
            frameIndex, quality, interfaceIndex, tier.atlasExtent);
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredTileTermination));
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::recordDeepLayeredLocalComposition(
        std::span<const DrawPacket> packets,
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality) {
        const LayeredQualityTierContract tierContract =
            layeredQualityTierContract(quality);
        if (quality != TransparencyQuality::Hero4 &&
            quality != TransparencyQuality::Cinematic8) {
            throw std::invalid_argument(
                "Deep local composition requires Hero4 or Cinematic8");
        }
        const uint32_t interfaceCount =
            tierContract.maximumInterfaceCount;
        const std::span<const std::string_view> passNames = quality ==
                TransparencyQuality::Hero4
            ? std::span<const std::string_view>(Hero4PassNames)
            : std::span<const std::string_view>(Cinematic8PassNames);
        const std::string_view passName = passNames[interfaceCount];
        const bool hasTierDraws = std::ranges::any_of(draws,
            [quality](const LayeredCaptureDraw& draw) {
                return draw.quality == quality;
            });
        if (!hasTierDraws) {
            renderGraph_.skipPass(passName);
            return;
        }

        const uint32_t frameIndex = scheduler.currentFrameIndex();
        VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        VulkanFrameContextTargets::DeepLayeredTier& tier = quality ==
                TransparencyQuality::Hero4
            ? targets.hero4 : targets.cinematic8;
        if (!tier.active() || tier.interfaceCount != interfaceCount ||
            tier.localCompositionFramebuffer == VK_NULL_HANDLE ||
            layeredLocalComposition_.deepPipeline() == VK_NULL_HANDLE ||
            layeredLocalComposition_.deepResidualPipeline() ==
                VK_NULL_HANDLE ||
            layeredLocalComposition_.deepDescriptorInterfaceCount(
                frameIndex, quality) != interfaceCount) {
            throw std::logic_error(
                "Deep local composition requires a complete resident tier");
        }

        const char* gpuRangeName = quality == TransparencyQuality::Hero4
            ? "gpu.transparency.layered.hero4.local-compose"
            : "gpu.transparency.layered.cinematic8.local-compose";
        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(gpuRangeName);
        renderGraph_.beginPass(currentCmd, passName);
        VkClearValue clear{};
        clear.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        VkRenderPassBeginInfo passInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passInfo.renderPass = layeredLocalComposition_.renderPass();
        passInfo.framebuffer = tier.localCompositionFramebuffer;
        passInfo.renderArea.extent = tier.atlasExtent;
        passInfo.clearValueCount = 1u;
        passInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(currentCmd, &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);

        const VkPipelineLayout layout =
            layeredLocalComposition_.deepPipelineLayout();
        const VkDescriptorSet globalSet = globalDescriptorSets[frameIndex];
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        bindMaterialDescriptors(layout);
        const VkDescriptorSet sceneSet = sceneDescriptors.get(frameIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &sceneSet, 0u, nullptr);
        const VkDescriptorSet interfaceSet =
            layeredLocalComposition_.deepDescriptorSet(frameIndex, quality);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 4u, 1u, &interfaceSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        // The bounded tail is evaluated first into the cleared local atlas.
        // Only semantic entries strictly behind the final stored interface
        // survive the residual shader. This compresses arbitrarily many
        // uncaptured interfaces into deterministic non-refractive operators
        // without adding another interface image or per-frame allocation.
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredLocalComposition_.deepResidualPipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredResidualComposition));
        const uint32_t residualQuerySlot = quality ==
                TransparencyQuality::Hero4
            ? 0u : 1u;
        const bool residualQueryActive =
            scheduler.beginLayeredResidualQuery(residualQuerySlot);
        for (const LayeredCaptureDraw& draw : draws) {
            if (draw.quality != quality || draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                geometryVault.get(packet.geometry);
            if (geometry == nullptr ||
                materialVault.get(packet.material) == nullptr) {
                continue;
            }
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = 0u;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.padding[0] = draw.workTableIndex;
            const uint32_t mirrored = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[1] = mirrored | (interfaceCount << 16u);
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(currentCmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            recordDraw(frameCounters_.deepLayeredResidualProbeDraws,
                packet.indexCount / 3u);
        }
        if (residualQueryActive)
            scheduler.endLayeredResidualQuery(residualQuerySlot);

        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredLocalComposition_.deepPipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredLocalComposition));
        // Captured slots are front-to-back. Rerasterizing them in reverse
        // makes premultiplied over blending deterministic per pixel without a
        // global object sort. The fragment shader accepts entry slots only and
        // validates the exact captured depth before evaluating shared transport.
        for (uint32_t interfaceIndex = interfaceCount;
            interfaceIndex-- > 0u;) {
            for (const LayeredCaptureDraw& draw : draws) {
                if (draw.quality != quality ||
                    draw.packetIndex >= packets.size()) {
                    continue;
                }
                const DrawPacket& packet = packets[draw.packetIndex];
                const VulkanGeometryPayload* geometry =
                    geometryVault.get(packet.geometry);
                if (geometry == nullptr ||
                    materialVault.get(packet.material) == nullptr) {
                    continue;
                }
                const VkViewport viewport{
                    -static_cast<float>(draw.viewportOffsetX),
                    -static_cast<float>(draw.viewportOffsetY),
                    static_cast<float>(frameTargets.extent().width),
                    static_cast<float>(frameTargets.extent().height),
                    0.0f, 1.0f };
                const VkRect2D scissor{
                    { static_cast<int32_t>(draw.atlasX),
                        static_cast<int32_t>(draw.atlasY) },
                    { draw.width, draw.height } };
                vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
                vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
                if (packet.geometry != lastGeometry) {
                    const VkDeviceSize offset = 0u;
                    vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0u,
                        toVkIndexType(geometry->indexFormat));
                    lastGeometry = packet.geometry;
                }
                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = draw.workTableIndex;
                const uint32_t mirrored = (packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0u ? 1u : 0u;
                push.padding[1] = mirrored | (interfaceIndex << 8u) |
                    (interfaceCount << 16u);
                push.padding[2] = packLayeredViewportOffset(
                    draw.viewportOffsetX, draw.viewportOffsetY);
                vkCmdPushConstants(currentCmd, layout,
                    VK_SHADER_STAGE_VERTEX_BIT |
                        VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                    packet.firstIndex, 0, 0u);
                recordDraw(frameCounters_.deepLayeredLocalCompositionDraws,
                    packet.indexCount / 3u);
            }
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::recordOrdinary2LocalComposition(
        std::span<const DrawPacket> packets,
        std::span<const Ordinary2CaptureDraw> draws) {
        constexpr std::string_view PassName =
            "transparent.layered.local-compose";
        if (draws.empty()) {
            renderGraph_.skipPass(PassName);
            return;
        }
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        if (ordinary2AtlasExtent_.width == 0u ||
            ordinary2AtlasExtent_.height == 0u ||
            layeredLocalComposition_.pipeline() == VK_NULL_HANDLE ||
            layeredLocalComposition_.descriptorFrameCount() <= frameIndex) {
            throw std::logic_error(
                "Ordinary2 local composition requires resident atlas targets");
        }
        VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        if (targets.layeredLocalCompositionFramebuffer == VK_NULL_HANDLE)
            throw std::logic_error(
                "Ordinary2 local-composition framebuffer is unavailable");

        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            "gpu.transparency.layered.local-compose");
        renderGraph_.beginPass(currentCmd, PassName);
        VkClearValue clear{};
        clear.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        VkRenderPassBeginInfo passInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passInfo.renderPass = layeredLocalComposition_.renderPass();
        passInfo.framebuffer =
            targets.layeredLocalCompositionFramebuffer;
        passInfo.renderArea.extent = ordinary2AtlasExtent_;
        passInfo.clearValueCount = 1u;
        passInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(currentCmd, &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);

        const VkPipelineLayout layout =
            layeredLocalComposition_.pipelineLayout();
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredLocalComposition_.pipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredLocalComposition));
        const VkDescriptorSet globalSet = globalDescriptorSets[frameIndex];
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        bindMaterialDescriptors(layout);
        const VkDescriptorSet sceneSet = sceneDescriptors.get(frameIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &sceneSet, 0u, nullptr);
        const VkDescriptorSet interfaceSet =
            layeredLocalComposition_.descriptorSet(frameIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 4u, 1u, &interfaceSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        // Reverse the stable capture order. Ordinary2 stores one shell, while
        // this order is the bounded back-to-front contract extended by M6.6.
        for (auto iterator = draws.rbegin(); iterator != draws.rend();
            ++iterator) {
            const Ordinary2CaptureDraw& draw = *iterator;
            if (draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                geometryVault.get(packet.geometry);
            if (geometry == nullptr ||
                materialVault.get(packet.material) == nullptr)
                continue;
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = 0u;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.padding[0] = draw.workTableIndex;
            push.padding[1] = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(currentCmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            recordDraw(frameCounters_.ordinary2LocalCompositionDraws,
                packet.indexCount / 3u);
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::prepareOrdinary2ResolvedPacketIndices(
        std::span<const Ordinary2CaptureDraw> draws) {
        ordinary2ResolvedPacketCount_ = static_cast<uint32_t>((std::min)(
            draws.size(), ordinary2ResolvedDraws_.size()));
        std::copy_n(draws.begin(), ordinary2ResolvedPacketCount_,
            ordinary2ResolvedDraws_.begin());
        std::sort(ordinary2ResolvedDraws_.begin(),
            ordinary2ResolvedDraws_.begin() + ordinary2ResolvedPacketCount_,
            [](const Ordinary2CaptureDraw& lhs,
                const Ordinary2CaptureDraw& rhs) {
                return lhs.packetIndex < rhs.packetIndex;
            });
    }

    bool VulkanVertexBackend::isOrdinary2PacketResolved(
        uint32_t packetIndex) const noexcept {
        const auto begin = ordinary2ResolvedDraws_.begin();
        const auto end = begin + ordinary2ResolvedPacketCount_;
        const auto found = std::lower_bound(begin, end, packetIndex,
            [](const Ordinary2CaptureDraw& draw, uint32_t index) {
                return draw.packetIndex < index;
            });
        return found != end && found->packetIndex == packetIndex;
    }

    void VulkanVertexBackend::prepareDeepResolvedPacketIndices(
        std::span<const LayeredCaptureDraw> draws) {
        deepResolvedPacketCount_ = 0u;
        for (const LayeredCaptureDraw& draw : draws) {
            if ((draw.quality != TransparencyQuality::Hero4 &&
                    draw.quality != TransparencyQuality::Cinematic8) ||
                deepResolvedPacketCount_ >= deepResolvedDraws_.size()) {
                continue;
            }
            deepResolvedDraws_[deepResolvedPacketCount_++] = draw;
        }
        std::sort(deepResolvedDraws_.begin(),
            deepResolvedDraws_.begin() + deepResolvedPacketCount_,
            [](const LayeredCaptureDraw& lhs,
                const LayeredCaptureDraw& rhs) {
                return lhs.packetIndex < rhs.packetIndex;
            });
    }

    bool VulkanVertexBackend::isDeepPacketResolved(
        uint32_t packetIndex) const noexcept {
        const auto begin = deepResolvedDraws_.begin();
        const auto end = begin + deepResolvedPacketCount_;
        const auto found = std::lower_bound(begin, end, packetIndex,
            [](const LayeredCaptureDraw& draw, uint32_t index) {
                return draw.packetIndex < index;
            });
        return found != end && found->packetIndex == packetIndex;
    }

    bool VulkanVertexBackend::isLayeredPacketResolved(
        uint32_t packetIndex) const noexcept {
        return isOrdinary2PacketResolved(packetIndex) ||
            isDeepPacketResolved(packetIndex);
    }

    void VulkanVertexBackend::recordOrdinary2SceneResolve(
        std::span<const DrawPacket> packets,
        std::span<const Ordinary2CaptureDraw> draws) {
        constexpr std::string_view PassName =
            "transparent.layered.compose-hook";
        if (draws.empty()) {
            renderGraph_.skipPass(PassName);
            return;
        }
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        if (layeredSceneResolve_.pipeline() == VK_NULL_HANDLE ||
            layeredSceneResolve_.descriptorFrameCount() <= frameIndex) {
            throw std::logic_error(
                "Ordinary2 scene resolve requires resident atlas descriptors");
        }
        VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        if (targets.transparentFramebuffer == VK_NULL_HANDLE)
            throw std::logic_error(
                "Ordinary2 scene resolve requires the scene framebuffer");

        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            "gpu.transparency.layered.scene-resolve");
        renderGraph_.beginPass(currentCmd, PassName);
        VkRenderPassBeginInfo passInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passInfo.renderPass = transparentPass->getRenderPass();
        passInfo.framebuffer = targets.transparentFramebuffer;
        passInfo.renderArea.extent = frameTargets.extent();
        vkCmdBeginRenderPass(currentCmd, &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);

        const VkPipelineLayout layout =
            layeredSceneResolve_.pipelineLayout();
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredSceneResolve_.pipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredSceneResolve));
        const VkDescriptorSet globalSet = globalDescriptorSets[frameIndex];
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        const VkDescriptorSet localSet =
            layeredSceneResolve_.descriptorSet(frameIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 1u, 1u, &localSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        const VkExtent2D sceneExtent = frameTargets.extent();
        // Packet-index order is the frontend's stable back-to-front order.
        for (uint32_t resolvedIndex = 0u;
            resolvedIndex < ordinary2ResolvedPacketCount_; ++resolvedIndex) {
            const Ordinary2CaptureDraw& draw =
                ordinary2ResolvedDraws_[resolvedIndex];
            if (draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                geometryVault.get(packet.geometry);
            if (geometry == nullptr)
                continue;
            const int64_t screenX = static_cast<int64_t>(draw.atlasX) +
                draw.viewportOffsetX;
            const int64_t screenY = static_cast<int64_t>(draw.atlasY) +
                draw.viewportOffsetY;
            if (screenX < 0 || screenY < 0 ||
                screenX >= sceneExtent.width || screenY >= sceneExtent.height)
                continue;
            const uint32_t scissorWidth = (std::min)(draw.width,
                sceneExtent.width - static_cast<uint32_t>(screenX));
            const uint32_t scissorHeight = (std::min)(draw.height,
                sceneExtent.height - static_cast<uint32_t>(screenY));
            if (scissorWidth == 0u || scissorHeight == 0u)
                continue;
            const VkViewport viewport{ 0.0f, 0.0f,
                static_cast<float>(sceneExtent.width),
                static_cast<float>(sceneExtent.height), 0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(screenX),
                    static_cast<int32_t>(screenY) },
                { scissorWidth, scissorHeight } };
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = 0u;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer,
                    0u, toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = draw.workTableIndex;
            push.padding[0] = 1u;
            push.padding[1] = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(currentCmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            recordDraw(frameCounters_.ordinary2SceneResolveDraws,
                packet.indexCount / 3u);
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::recordDeepLayeredSceneResolve(
        std::span<const DrawPacket> packets,
        std::span<const LayeredCaptureDraw> draws) {
        const bool hero4Active = hero4AtlasExtent_.width != 0u &&
            hero4AtlasExtent_.height != 0u;
        const bool cinematic8Active = cinematic8AtlasExtent_.width != 0u &&
            cinematic8AtlasExtent_.height != 0u;
        const std::string_view passName = hero4Active && cinematic8Active
            ? "transparent.layered.deep.compose-hook"
            : hero4Active
                ? "transparent.layered.hero4.compose-hook"
                : "transparent.layered.cinematic8.compose-hook";
        if (draws.empty() || deepResolvedPacketCount_ == 0u) {
            renderGraph_.skipPass(passName);
            return;
        }
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        if (layeredSceneResolve_.pipeline() == VK_NULL_HANDLE) {
            throw std::logic_error(
                "Deep scene resolve requires a resident pipeline");
        }
        VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        if (targets.transparentFramebuffer == VK_NULL_HANDLE) {
            throw std::logic_error(
                "Deep scene resolve requires resident scene targets");
        }

        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            hero4Active && cinematic8Active
                ? "gpu.transparency.layered.deep.scene-resolve"
                : hero4Active
                    ? "gpu.transparency.layered.hero4.scene-resolve"
                    : "gpu.transparency.layered.cinematic8.scene-resolve");
        renderGraph_.beginPass(currentCmd, passName);
        VkRenderPassBeginInfo passInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passInfo.renderPass = transparentPass->getRenderPass();
        passInfo.framebuffer = targets.transparentFramebuffer;
        passInfo.renderArea.extent = frameTargets.extent();
        vkCmdBeginRenderPass(currentCmd, &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);

        const VkPipelineLayout layout =
            layeredSceneResolve_.pipelineLayout();
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredSceneResolve_.pipeline());
        recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredSceneResolve));
        const VkDescriptorSet globalSet = globalDescriptorSets[frameIndex];
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        GeometryHandle lastGeometry{};
        TransparencyQuality lastQuality = TransparencyQuality::Ordinary2;
        const VkExtent2D sceneExtent = frameTargets.extent();
        std::array<uint32_t, kLayeredQualityTierCount>
            sceneResolveDrawCounts{};
        for (uint32_t resolvedIndex = 0u;
            resolvedIndex < deepResolvedPacketCount_; ++resolvedIndex) {
            const LayeredCaptureDraw& draw =
                deepResolvedDraws_[resolvedIndex];
            const bool tierActive = draw.quality ==
                    TransparencyQuality::Hero4
                ? targets.hero4.active()
                : draw.quality == TransparencyQuality::Cinematic8 &&
                    targets.cinematic8.active();
            if (!tierActive ||
                layeredSceneResolve_.descriptorFrameCount(draw.quality) <=
                    frameIndex) {
                throw std::logic_error(
                    "Deep scene resolve requires resident tier descriptors");
            }
            if (draw.packetIndex >= packets.size()) continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                geometryVault.get(packet.geometry);
            if (geometry == nullptr) continue;
            const int64_t screenX = static_cast<int64_t>(draw.atlasX) +
                draw.viewportOffsetX;
            const int64_t screenY = static_cast<int64_t>(draw.atlasY) +
                draw.viewportOffsetY;
            if (screenX < 0 || screenY < 0 ||
                screenX >= sceneExtent.width || screenY >= sceneExtent.height) {
                continue;
            }
            const uint32_t scissorWidth = (std::min)(draw.width,
                sceneExtent.width - static_cast<uint32_t>(screenX));
            const uint32_t scissorHeight = (std::min)(draw.height,
                sceneExtent.height - static_cast<uint32_t>(screenY));
            if (scissorWidth == 0u || scissorHeight == 0u) continue;
            const VkViewport viewport{ 0.0f, 0.0f,
                static_cast<float>(sceneExtent.width),
                static_cast<float>(sceneExtent.height), 0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(screenX),
                    static_cast<int32_t>(screenY) },
                { scissorWidth, scissorHeight } };
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            if (draw.quality != lastQuality) {
                const VkDescriptorSet localSet =
                    layeredSceneResolve_.descriptorSet(frameIndex,
                        draw.quality);
                vkCmdBindDescriptorSets(currentCmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1u, 1u,
                    &localSet, 0u, nullptr);
                lastQuality = draw.quality;
            }
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = 0u;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = draw.workTableIndex;
            push.padding[1] = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(currentCmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            recordDraw(frameCounters_.deepLayeredSceneResolveDraws,
                packet.indexCount / 3u);
            ++sceneResolveDrawCounts[layeredQualityTierIndex(draw.quality)];
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
        for (PendingDeepLayeredCaptureValidation& pending :
            pendingDeepLayeredCaptureValidations_) {
            if (pending.frameIndex == frameIndex) {
                pending.sceneResolveDrawCount = sceneResolveDrawCounts[
                    layeredQualityTierIndex(pending.quality)];
            }
        }
    }

    void VulkanVertexBackend::recordOrdinary2CaptureValidationReadback(
        std::span<const Ordinary2CaptureDraw> draws) {
        if (!ordinary2CaptureValidationRequest_ || draws.empty()) {
            renderGraph_.skipPass(
                "transparent.layered.validation-readback-hook");
            return;
        }
        if (ordinary2AtlasExtent_.width == 0u ||
            ordinary2AtlasExtent_.height == 0u) {
            throw std::logic_error(
                "Ordinary2 validation readback requires a resident atlas");
        }

        const uint64_t pixelCount =
            static_cast<uint64_t>(ordinary2AtlasExtent_.width) *
            ordinary2AtlasExtent_.height;
        constexpr uint64_t BytesPerImagePixel = sizeof(uint32_t);
        constexpr uint64_t InterfaceImageCount = 4u;
        if (pixelCount > std::numeric_limits<VkDeviceSize>::max() /
                (BytesPerImagePixel * InterfaceImageCount + 8u)) {
            throw std::overflow_error(
                "Ordinary2 validation readback exceeds VkDeviceSize");
        }
        const VkDeviceSize imageBytes = pixelCount * BytesPerImagePixel;
        const VkDeviceSize localColorBytes = pixelCount * 8u;
        const VkDeviceSize totalBytes =
            imageBytes * InterfaceImageCount + localColorBytes;

        PendingOrdinary2CaptureValidation pending{};
        pending.validationId = *ordinary2CaptureValidationRequest_;
        pending.frameIndex = scheduler.currentFrameIndex();
        pending.extent = ordinary2AtlasExtent_;
        pending.expectedDrawCount = static_cast<uint32_t>(draws.size());
        pending.workItemCount = static_cast<uint32_t>(
            ordinary2AtlasPlan_.workIdentities().size());
        pending.readback = resourceAllocator.createBuffer(totalBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, ProfileMemoryCategory::CaptureReadback);
        try {
            pendingOrdinary2CaptureValidations_.push_back(std::move(pending));
        }
        catch (...) {
            resourceAllocator.destroy(pending.readback);
            throw;
        }
        ordinary2CaptureValidationRequest_.reset();

        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            "gpu.transparency.layered.validation-readback");
        renderGraph_.beginPass(currentCmd,
            "transparent.layered.validation-readback-hook");
        PendingOrdinary2CaptureValidation& recorded =
            pendingOrdinary2CaptureValidations_.back();
        VulkanCommandList commandList(currentCmd);
        commandList.transition(recorded.readback,
            ResourceState::CopyDestination);
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        const auto copyImage = [&](const VulkanImageResource& image,
                VkImageAspectFlags aspect, VkDeviceSize offset) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = aspect;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = { ordinary2AtlasExtent_.width,
                ordinary2AtlasExtent_.height, 1u };
            commandList.copyImageToBuffer(image, recorded.readback, copy);
        };
        copyImage(targets.layeredEntryIdentity, VK_IMAGE_ASPECT_COLOR_BIT,
            0u);
        copyImage(targets.layeredEntryDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
            imageBytes);
        copyImage(targets.layeredExitIdentity, VK_IMAGE_ASPECT_COLOR_BIT,
            imageBytes * 2u);
        copyImage(targets.layeredExitDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
            imageBytes * 3u);
        copyImage(targets.layeredLocalColor, VK_IMAGE_ASPECT_COLOR_BIT,
            imageBytes * InterfaceImageCount);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::recordDeepLayeredCaptureValidationReadback(
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality) {
        const uint32_t interfaceCount = layeredQualityTierContract(
            quality).maximumInterfaceCount;
        const std::span<const std::string_view> passNames = quality ==
                TransparencyQuality::Hero4
            ? std::span<const std::string_view>(Hero4PassNames)
            : quality == TransparencyQuality::Cinematic8
                ? std::span<const std::string_view>(
                    Cinematic8PassNames)
                : std::span<const std::string_view>{};
        if (passNames.empty()) {
            throw std::invalid_argument(
                "Deep validation requires Hero4 or Cinematic8");
        }
        const std::string_view passName = passNames[interfaceCount + 1u];
        const bool requested = deepLayeredCaptureValidationRequest_ &&
            deepLayeredCaptureValidationRequest_->quality == quality;
        const uint32_t expectedDrawCount = static_cast<uint32_t>(
            std::ranges::count_if(draws,
                [quality](const LayeredCaptureDraw& draw) {
                    return draw.quality == quality;
                }));
        if (!requested || expectedDrawCount == 0u) {
            renderGraph_.skipPass(passName);
            return;
        }

        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        VulkanFrameContextTargets::DeepLayeredTier& tier = quality ==
                TransparencyQuality::Hero4
            ? targets.hero4 : targets.cinematic8;
        if (!tier.active() || tier.interfaceCount != interfaceCount ||
            tier.atlasExtent.width == 0u || tier.atlasExtent.height == 0u) {
            throw std::logic_error(
                "Deep validation readback requires a complete resident tier");
        }
        const uint64_t pixelCount =
            static_cast<uint64_t>(tier.atlasExtent.width) *
            tier.atlasExtent.height;
        constexpr uint64_t BytesPerImagePixel = sizeof(uint32_t);
        const uint64_t interfaceImageCount = interfaceCount * 2ull;
        const uint64_t bytesPerPixel =
            BytesPerImagePixel * interfaceImageCount + 8ull;
        if (pixelCount > std::numeric_limits<VkDeviceSize>::max() /
                bytesPerPixel) {
            throw std::overflow_error(
                "Deep validation readback exceeds VkDeviceSize");
        }
        const VkDeviceSize imageBytes = pixelCount * BytesPerImagePixel;
        const VkExtent2D tileExtent{
            (tier.atlasExtent.width +
                kDeepLayeredEarlyTerminationTileSize - 1u) /
                kDeepLayeredEarlyTerminationTileSize,
            (tier.atlasExtent.height +
                kDeepLayeredEarlyTerminationTileSize - 1u) /
                kDeepLayeredEarlyTerminationTileSize };
        const VkDeviceSize tileImageBytes = static_cast<VkDeviceSize>(
            tileExtent.width) * tileExtent.height * sizeof(uint32_t);
        const VkDeviceSize interfaceAndLocalBytes = pixelCount * bytesPerPixel;
        if (tileImageBytes > ((std::numeric_limits<VkDeviceSize>::max)() -
                interfaceAndLocalBytes) / interfaceCount) {
            throw std::overflow_error(
                "Deep validation tile readback exceeds VkDeviceSize");
        }
        const VkDeviceSize totalBytes = interfaceAndLocalBytes +
            tileImageBytes * interfaceCount;

        PendingDeepLayeredCaptureValidation pending{};
        pending.validationId =
            deepLayeredCaptureValidationRequest_->validationId;
        pending.frameIndex = scheduler.currentFrameIndex();
        pending.extent = tier.atlasExtent;
        pending.quality = quality;
        pending.interfaceCount = interfaceCount;
        pending.expectedDrawCount = expectedDrawCount;
        pending.workItemCount = static_cast<uint32_t>(
            deepLayeredAtlasPlan_.workIdentities().size());
        pending.readback = resourceAllocator.createBuffer(totalBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, ProfileMemoryCategory::CaptureReadback);
        try {
            pendingDeepLayeredCaptureValidations_.push_back(
                std::move(pending));
        }
        catch (...) {
            resourceAllocator.destroy(pending.readback);
            throw;
        }
        deepLayeredCaptureValidationRequest_.reset();

        const char* gpuRangeName = quality == TransparencyQuality::Hero4
            ? "gpu.transparency.layered.hero4.validation-readback"
            : "gpu.transparency.layered.cinematic8.validation-readback";
        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(gpuRangeName);
        renderGraph_.beginPass(currentCmd, passName);
        PendingDeepLayeredCaptureValidation& recorded =
            pendingDeepLayeredCaptureValidations_.back();
        VulkanCommandList commandList(currentCmd);
        commandList.transition(recorded.readback,
            ResourceState::CopyDestination);
        const auto copyImage = [&](const VulkanImageResource& image,
                VkImageAspectFlags aspect, VkDeviceSize offset) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = aspect;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = { tier.atlasExtent.width,
                tier.atlasExtent.height, 1u };
            commandList.copyImageToBuffer(image, recorded.readback, copy);
        };
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < interfaceCount; ++interfaceIndex) {
            copyImage(tier.interfaceIdentity[interfaceIndex],
                VK_IMAGE_ASPECT_COLOR_BIT,
                imageBytes * (interfaceIndex * 2u));
            copyImage(tier.interfaceDepth[interfaceIndex],
                VK_IMAGE_ASPECT_DEPTH_BIT,
                imageBytes * (interfaceIndex * 2u + 1u));
        }
        copyImage(tier.localColor, VK_IMAGE_ASPECT_COLOR_BIT,
            imageBytes * interfaceImageCount);
        const VkDeviceSize tileBaseOffset = imageBytes *
            interfaceImageCount + pixelCount * 8ull;
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < interfaceCount; ++interfaceIndex) {
            if (!deepLayeredTerminationInterface(interfaceIndex,
                    interfaceCount)) {
                continue;
            }
            VkBufferImageCopy copy{};
            copy.bufferOffset = tileBaseOffset +
                tileImageBytes * interfaceIndex;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = { tileExtent.width, tileExtent.height, 1u };
            commandList.copyImageToBuffer(
                tier.tileTermination[interfaceIndex], recorded.readback,
                copy);
        }
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::submitForwardQueues(
        std::span<const DrawPacket> opaqueForwardQueue,
        std::span<const DrawPacket> sortedSurfaceQueue,
        std::span<const DrawPacket> compatibilityTransparentQueue) {
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.forward");
        const bool pipelineStatisticsActive =
            scheduler.beginTransparentPipelineStatistics();

        const bool ordinary2CaptureTopologyActive =
            ordinary2AtlasExtent_.width != 0u &&
            ordinary2AtlasExtent_.height != 0u;
        const bool requiresOrdinary2Atlas = std::ranges::any_of(
            compatibilityTransparentQueue, isOrdinary2LayeredGlassPacket);
        ordinary2AtlasResidency_.observe(requiresOrdinary2Atlas);
        const bool requiresHero4Atlas = std::ranges::any_of(
            compatibilityTransparentQueue, [](const DrawPacket& packet) {
                return isLayeredGlassPacket(packet,
                    TransparencyQuality::Hero4);
            });
        const bool requiresCinematic8Atlas = std::ranges::any_of(
            compatibilityTransparentQueue, [](const DrawPacket& packet) {
                return isLayeredGlassPacket(packet,
                    TransparencyQuality::Cinematic8);
            });
        hero4AtlasResidency_.observe(requiresHero4Atlas);
        cinematic8AtlasResidency_.observe(requiresCinematic8Atlas);
        bool ordinary2PreparedThisFrame = false;
        // Active Ordinary2 topology prepares the fixed-capacity draw plan every
        // frame. When inactive, profiler frames retain the earlier demand probe
        // without changing topology or recording commands.
        if ((ordinary2CaptureTopologyActive || collectFrameCounters_) &&
            ordinary2ViewProjectionValid_) {
            CpuScope preparationScope(cpuProfiler_,
                ordinary2CaptureTopologyActive
                    ? "cpu.render.prepare.ordinary2"
                    : "cpu.render.prepare.ordinary2_probe");
            const VkExtent2D extent = frameTargets.extent();
            ordinary2RequestCollector_.collect(compatibilityTransparentQueue,
                ordinary2ViewProjection_, extent.width, extent.height);
            (void)ordinary2AtlasPlan_.prepare(
                ordinary2RequestCollector_.requests(),
                extent.width, extent.height);
            (void)ordinary2CaptureDrawPlan_.prepare(
                ordinary2AtlasPlan_.decisions(),
                compatibilityTransparentQueue,
                ordinary2AtlasPlan_.atlasExtent());
            ordinary2PreparedThisFrame = true;
            if (collectFrameCounters_) {
                const Ordinary2RequestCollectionStats& collection =
                    ordinary2RequestCollector_.stats();
                const Ordinary2AtlasStats& atlas = ordinary2AtlasPlan_.stats();
                const Ordinary2CaptureDrawStats& capture =
                    ordinary2CaptureDrawPlan_.stats();
                ++frameCounters_.ordinary2ProbeFrames;
                frameCounters_.ordinary2CandidatePackets =
                    collection.candidatePacketCount;
                frameCounters_.ordinary2ProjectedPackets =
                    collection.projectedPacketCount;
                frameCounters_.ordinary2ProjectionCulledPackets =
                    collection.culledPacketCount;
                frameCounters_.ordinary2InvalidBoundsFallbackPackets =
                    collection.invalidBoundsFallbackCount;
                frameCounters_.ordinary2NearPlaneFallbackPackets =
                    collection.nearPlaneFallbackCount;
                frameCounters_.ordinary2UnsafeProjectionFallbackPackets =
                    collection.unsafeProjectionFallbackCount;
                frameCounters_.ordinary2RequestCapacityFallbackPackets =
                    collection.requestCapacityFallbackCount;
                frameCounters_.ordinary2AtlasAcceptedPackets =
                    atlas.acceptedPacketCount;
                frameCounters_.ordinary2AtlasAcceptedIslands =
                    atlas.acceptedIslandCount;
                frameCounters_.ordinary2AtlasRejectedPackets =
                    atlas.requestCount - atlas.acceptedPacketCount;
                frameCounters_.ordinary2AtlasAllocatedTexels =
                    atlas.allocatedTexelCount;
                frameCounters_.ordinary2CapturePreparedDraws =
                    capture.preparedDrawCount;
                frameCounters_.ordinary2CapturePreparationFallbackPackets =
                    capture.invalidPacketIndexCount +
                    capture.incompatiblePacketCount +
                    capture.invalidPlacementCount;
            }
        }

        std::span<const Ordinary2CaptureDraw> captureDraws{};
        if (ordinary2CaptureTopologyActive && ordinary2PreparedThisFrame)
            captureDraws = ordinary2CaptureDrawPlan_.draws();
        prepareOrdinary2ResolvedPacketIndices(captureDraws);

        const bool hero4CaptureTopologyActive =
            hero4AtlasExtent_.width != 0u && hero4AtlasExtent_.height != 0u;
        const bool cinematic8CaptureTopologyActive =
            cinematic8AtlasExtent_.width != 0u &&
            cinematic8AtlasExtent_.height != 0u;
        std::span<const LayeredCaptureDraw> deepCaptureDraws{};
        if ((hero4CaptureTopologyActive || cinematic8CaptureTopologyActive) &&
            ordinary2ViewProjectionValid_) {
            CpuScope preparationScope(cpuProfiler_,
                "cpu.render.prepare.layered_deep");
            uint32_t activeTierMask = 0u;
            if (hero4CaptureTopologyActive)
                activeTierMask |= 1u << layeredQualityTierIndex(
                    TransparencyQuality::Hero4);
            if (cinematic8CaptureTopologyActive)
                activeTierMask |= 1u << layeredQualityTierIndex(
                    TransparencyQuality::Cinematic8);
            const VkExtent2D extent = frameTargets.extent();
            deepLayeredRequestCollector_.collect(
                compatibilityTransparentQueue, ordinary2ViewProjection_,
                extent.width, extent.height, activeTierMask);
            (void)deepLayeredAtlasPlan_.prepare(
                deepLayeredRequestCollector_.requests(),
                extent.width, extent.height);
            const std::array<Ordinary2AtlasExtent,
                kLayeredQualityTierCount> residentExtents{
                Ordinary2AtlasExtent{},
                { hero4AtlasExtent_.width, hero4AtlasExtent_.height },
                { cinematic8AtlasExtent_.width,
                    cinematic8AtlasExtent_.height },
            };
            (void)deepLayeredCaptureDrawPlan_.prepare(
                deepLayeredAtlasPlan_.decisions(),
                compatibilityTransparentQueue, residentExtents);
            deepCaptureDraws = deepLayeredCaptureDrawPlan_.draws();
            if (collectFrameCounters_) {
                const LayeredRequestCollectionStats& collection =
                    deepLayeredRequestCollector_.stats();
                const LayeredAtlasStats& atlas =
                    deepLayeredAtlasPlan_.stats();
                const LayeredCaptureDrawStats& capture =
                    deepLayeredCaptureDrawPlan_.stats();
                frameCounters_.deepLayeredCandidatePackets =
                    collection.candidatePacketCount;
                frameCounters_.deepLayeredProjectedPackets =
                    collection.projectedPacketCount;
                frameCounters_.deepLayeredAtlasAcceptedPackets =
                    atlas.acceptedPacketCount;
                frameCounters_.deepLayeredAtlasAcceptedIslands =
                    atlas.acceptedIslandCount;
                frameCounters_.deepLayeredAtlasRejectedPackets =
                    atlas.requestCount - atlas.acceptedPacketCount;
                frameCounters_.deepLayeredCapturePreparedDraws =
                    capture.preparedDrawCount;
                frameCounters_.deepLayeredCapturePreparationFallbackPackets =
                    capture.invalidPacketIndexCount +
                    capture.incompatiblePacketCount +
                    capture.invalidPlacementCount;
            }
        }
        prepareDeepResolvedPacketIndices(deepCaptureDraws);

        const auto recordForwardPass = [&](std::span<const DrawPacket> queue,
            std::string_view passName, std::string_view gpuRangeName,
            VkRenderPass renderPass, VkFramebuffer framebuffer,
            RenderPassClass expectedPassClass,
            bool skipResolvedLayered) {
            if (queue.empty()) {
                renderGraph_.skipPass(passName);
                return;
            }

            VulkanGpuRangeToken forwardGpuRange =
                scheduler.beginGpuRange(gpuRangeName.data());
            renderGraph_.beginPass(currentCmd, passName);
            VkRenderPassBeginInfo passInfo{
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            passInfo.renderPass = renderPass;
            passInfo.framebuffer = framebuffer;
            passInfo.renderArea.extent = frameTargets.extent();
            vkCmdBeginRenderPass(currentCmd, &passInfo,
                VK_SUBPASS_CONTENTS_INLINE);

            const VkViewport viewport{ 0.0f, 0.0f,
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{ { 0, 0 }, frameTargets.extent() };
            vkCmdSetViewport(currentCmd, 0, 1, &viewport);
            vkCmdSetScissor(currentCmd, 0, 1, &scissor);

            PipelineHandle lastBoundPipeline{};
            MaterialHandle lastBoundMaterial{};
            GeometryHandle lastBoundGeometry{};
            VkPipelineLayout activeLayout = VK_NULL_HANDLE;
            const VkDescriptorSet sceneSet = sceneDescriptors.get(
                scheduler.currentFrameIndex());

            for (const DrawPacket& packet : queue) {
                if (skipResolvedLayered) {
                    const uint32_t packetIndex = static_cast<uint32_t>(
                        &packet - compatibilityTransparentQueue.data());
                    if (isLayeredPacketResolved(packetIndex))
                        continue;
                }
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                const bool mirrored = (packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0;
                const PipelineHandle effectivePipeline = mirrored
                    ? material ? material->mirroredPipeline : PipelineHandle{}
                    : packet.pipeline;
                const VulkanPipelineRecord* record =
                    pipelineLibrary.get(effectivePipeline);
                if (!geometry || !material || !record ||
                    record->pipeline == VK_NULL_HANDLE ||
                    record->pipelineLayout == VK_NULL_HANDLE ||
                    record->renderPass != expectedPassClass) {
                    continue;
                }

                if (effectivePipeline != lastBoundPipeline) {
                    vkCmdBindPipeline(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
                    recordPipelineBind(effectivePipeline.id);
                    activeLayout = record->pipelineLayout;
                    vkCmdBindDescriptorSets(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        0, 1, &globalDescriptorSets[
                            scheduler.currentFrameIndex()], 0, nullptr);
                    vkCmdBindDescriptorSets(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        3u,
                        1, &sceneSet, 0, nullptr);
                    lastBoundPipeline = effectivePipeline;
                    lastBoundMaterial = MaterialHandle{};
                }
                if (packet.material != lastBoundMaterial) {
                    bindMaterialDescriptors(activeLayout);
                    recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = static_cast<uint32_t>(debugView_);
                push.padding[1] = mirrored ? 1u : 0u;
                vkCmdPushConstants(currentCmd, activeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT |
                        VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1,
                    packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawTransparentForward,
                    packet.indexCount / 3);
                if (collectFrameCounters_) {
                    const MaterialClosureClass closure =
                        static_cast<MaterialClosureClass>(
                            material->packed.closureClass);
                    if (closure == MaterialClosureClass::StandardForward) {
                        ++frameCounters_.drawStandardForward;
                    }
                    else if (closure == MaterialClosureClass::ComplexForward) {
                        ++frameCounters_.drawComplexForward;
                        for (uint32_t lobe = 0;
                            lobe < material->packed.complexLobeCount; ++lobe) {
                            const uint32_t type =
                                material->packed.complexLobes[lobe].type;
                            if (type < frameCounters_.complexLobeDraws.size()) {
                                ++frameCounters_.complexLobeDraws[type];
                            }
                        }
                    }
                    else if (closure == MaterialClosureClass::Unlit) {
                        ++frameCounters_.drawUnlitForward;
                    }
                }
            }
            vkCmdEndRenderPass(currentCmd);
            scheduler.endGpuRange(forwardGpuRange);
        };

        const VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        recordForwardPass(opaqueForwardQueue, "forward-opaque",
            "gpu.forward.opaque", forwardPass->getRenderPass(),
            targets.forwardFramebuffer, RenderPassClass::Forward, false);

        const bool requiresRefractionPyramids =
            !compatibilityTransparentQueue.empty();
        transparencyPyramidResidency_.observe(requiresRefractionPyramids);
        if (transparencyPyramidResidency_.requiresFallback(
                requiresRefractionPyramids) && collectFrameCounters_) {
            ++frameCounters_.transparencyPyramidFallbackFrames;
        }
        if (transparencyPyramidResidency_.enabled() &&
            !requiresRefractionPyramids) {
            renderGraph_.skipPass("transparent.refraction-pyramids");
        }
        else if (transparencyPyramidResidency_.enabled()) {
            VulkanGpuRangeToken pyramidGpuRange = scheduler.beginGpuRange(
                "gpu.transparency.refraction-pyramids");
            renderGraph_.beginPass(currentCmd,
                "transparent.refraction-pyramids");
            const uint32_t dispatches = transparencyPyramid_.record(
                currentCmd, scheduler.currentFrameIndex(),
                globalDescriptorSets[scheduler.currentFrameIndex()],
                frameTargets);
            if (collectFrameCounters_)
                frameCounters_.dispatchRecorded += dispatches;
            if (collectFrameCounters_) {
                ++frameCounters_.transparencyPyramidBuilds;
                frameCounters_.transparencyPyramidMipDispatches += dispatches;
            }
            scheduler.endGpuRange(pyramidGpuRange);
        }
        recordForwardPass(sortedSurfaceQueue, "transparent.sorted.forward",
            "gpu.transparency.sorted.forward", transparentPass->getRenderPass(),
            targets.transparentFramebuffer, RenderPassClass::Transparent,
            false);
        if (collectFrameCounters_) {
            frameCounters_.transparentSortedPackets = sortedSurfaceQueue.size();
        }
        if (ordinary2CaptureTopologyActive) {
            recordOrdinary2Captures(compatibilityTransparentQueue,
                captureDraws);
            recordOrdinary2LocalComposition(compatibilityTransparentQueue,
                captureDraws);
            recordOrdinary2CaptureValidationReadback(captureDraws);
            recordOrdinary2SceneResolve(compatibilityTransparentQueue,
                captureDraws);
        }

        if (hero4CaptureTopologyActive) {
            recordDeepLayeredCaptures(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Hero4);
            recordDeepLayeredLocalComposition(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Hero4);
            recordDeepLayeredCaptureValidationReadback(deepCaptureDraws,
                TransparencyQuality::Hero4);
        }
        if (cinematic8CaptureTopologyActive) {
            recordDeepLayeredCaptures(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Cinematic8);
            recordDeepLayeredLocalComposition(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Cinematic8);
            recordDeepLayeredCaptureValidationReadback(deepCaptureDraws,
                TransparencyQuality::Cinematic8);
        }
        if (hero4CaptureTopologyActive || cinematic8CaptureTopologyActive) {
            recordDeepLayeredSceneResolve(compatibilityTransparentQueue,
                deepCaptureDraws);
        }

        // Bucketize the genuinely transparent queue into the retained bounded
        // M2 background/foreground layers.
        // The queue is already sorted Back-to-Front by the frontend.
        std::optional<size_t> foregroundPacketIndex;
        uint64_t fallbackPacketCount = 0u;
        std::array<uint32_t, kLayeredQualityTierCount>
            deepCompatibilityForwardDrawCounts{};
        for (size_t packetIndex = 0u;
            packetIndex < compatibilityTransparentQueue.size(); ++packetIndex) {
            if (!isLayeredPacketResolved(
                    static_cast<uint32_t>(packetIndex))) {
                foregroundPacketIndex = packetIndex;
                ++fallbackPacketCount;
                const DrawPacket& packet =
                    compatibilityTransparentQueue[packetIndex];
                if (isLayeredGlassPacket(packet,
                        TransparencyQuality::Hero4) ||
                    isLayeredGlassPacket(packet,
                        TransparencyQuality::Cinematic8)) {
                    ++deepCompatibilityForwardDrawCounts[
                        layeredQualityTierIndex(
                            packet.transparency.quality)];
                }
            }
        }
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        for (PendingDeepLayeredCaptureValidation& pending :
            pendingDeepLayeredCaptureValidations_) {
            if (pending.frameIndex == frameIndex) {
                pending.compatibilityForwardDrawCount =
                    deepCompatibilityForwardDrawCounts[
                        layeredQualityTierIndex(pending.quality)];
            }
        }
        const std::span<const DrawPacket> foregroundBucket =
            foregroundPacketIndex
            ? compatibilityTransparentQueue.subspan(*foregroundPacketIndex, 1u)
            : std::span<const DrawPacket>{};
        const std::span<const DrawPacket> backgroundBucket =
            foregroundPacketIndex && *foregroundPacketIndex != 0u
            ? compatibilityTransparentQueue.first(*foregroundPacketIndex)
            : std::span<const DrawPacket>{};
        const uint64_t backgroundFallbackPacketCount =
            fallbackPacketCount - static_cast<uint64_t>(
                foregroundPacketIndex.has_value());
        if (collectFrameCounters_) {
            frameCounters_.transparentBackgroundPackets =
                backgroundFallbackPacketCount;
            frameCounters_.transparentForegroundPackets =
                static_cast<uint64_t>(foregroundPacketIndex.has_value());
            frameCounters_.transparentNonemptyBuckets =
                static_cast<uint64_t>(backgroundFallbackPacketCount != 0u) +
                static_cast<uint64_t>(foregroundPacketIndex.has_value());
        }

        // 2. THE REUSABLE RENDER LAMBDA
        auto executeGlassLayer = [&](std::span<const DrawPacket> glassBucket,
            uint64_t renderedPacketCount, bool foreground) {
            const std::string_view depthPassName = foreground
                ? "transparent.foreground.depth"
                : "transparent.background.depth";
            const std::string_view forwardPassName = foreground
                ? "transparent.foreground.forward"
                : "transparent.background.forward";
            if (renderedPacketCount == 0u) {
                renderGraph_.skipPass(depthPassName);
                renderGraph_.skipPass(forwardPassName);
                return;
            }

            VulkanFrameContextTargets& targets = frameTargets.get(
                scheduler.currentFrameIndex());
            // --- A. GLASS DEPTH PASS ---
            VulkanGpuRangeToken depthGpuRange = scheduler.beginGpuRange(foreground
                ? "gpu.transparency.foreground.depth"
                : "gpu.transparency.background.depth");
            renderGraph_.beginPass(currentCmd, depthPassName);
            VkRenderPassBeginInfo glassDepthPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            glassDepthPassInfo.renderPass = glassDepthPass->getRenderPass();
            glassDepthPassInfo.framebuffer = frameTargets.get(
                scheduler.currentFrameIndex()).glassDepthFramebuffer;
            glassDepthPassInfo.renderArea.extent = frameTargets.extent();

            VkClearValue depthClearValue{};
            depthClearValue.depthStencil = { 1.0f, 0 };
            glassDepthPassInfo.clearValueCount = 1;
            glassDepthPassInfo.pClearValues = &depthClearValue;

            vkCmdBeginRenderPass(currentCmd, &glassDepthPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkPipelineLayout gLayout = meshLayouts.getGBufferPipelineLayout();
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassDepthPipeline->getPipeline());
            recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::GlassDepth));

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;            // Start at the bottom
            viewport.width = (float)frameTargets.extent().width;
            viewport.height = (float)frameTargets.extent().height;       // Draw upwards!
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            vkCmdSetViewport(currentCmd, 0, 1, &viewport);
            VkRect2D scissor{ {0, 0}, frameTargets.extent() };
            vkCmdSetScissor(currentCmd, 0, 1, &scissor);

            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);

            for (const auto& packet : glassBucket) {
                const uint32_t packetIndex = static_cast<uint32_t>(
                    &packet - compatibilityTransparentQueue.data());
                if (isLayeredPacketResolved(packetIndex))
                    continue;
                auto* geometry = geometryVault.get(packet.geometry);
                if (!geometry) continue;

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                    toVkIndexType(geometry->indexFormat));

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                vkCmdPushConstants(currentCmd, gLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawTransparentDepth, packet.indexCount / 3);
            }
            vkCmdEndRenderPass(currentCmd);
            scheduler.endGpuRange(depthGpuRange);

            recordForwardPass(glassBucket, forwardPassName,
                foreground ? "gpu.transparency.foreground.forward"
                    : "gpu.transparency.background.forward",
                forwardPass->getRenderPass(),
                targets.forwardFramebuffer, RenderPassClass::Forward, true);
            };

        // 3. EXECUTE THE PASSES
        executeGlassLayer(backgroundBucket, backgroundFallbackPacketCount,
            false);
        executeGlassLayer(foregroundBucket,
            static_cast<uint64_t>(foregroundPacketIndex.has_value()), true);

        if (pipelineStatisticsActive) {
            scheduler.endTransparentPipelineStatistics();
        }
    }

    std::optional<FrameCapturePixelFormat> VulkanVertexBackend::capturePixelFormat(
        VkFormat format) noexcept {
        switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
            return FrameCapturePixelFormat::Rgba8Srgb;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return FrameCapturePixelFormat::Bgra8Srgb;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return FrameCapturePixelFormat::Rgba32Float;
        default:
            return std::nullopt;
        }
    }

    void VulkanVertexBackend::captureCurrentFrame(uint64_t captureId,
        FrameCapturePoint point) {
        if (!frameOpen_ || currentCmd == VK_NULL_HANDLE) {
            throw std::logic_error("Frame capture requires an active frame.");
        }
        const VkExtent2D extent = frameTargets.extent();
        const VkFormat format = point == FrameCapturePoint::SceneLinear
            ? frameTargets.format() : outputTargetFormat_;
        const uint32_t sourceBytesPerPixel = captureSourceBytesPerPixel(format);
        const uint32_t outputBytesPerPixel =
            format == VK_FORMAT_R16G16B16A16_SFLOAT ? 16u : 4u;
        if (!capturePixelFormat(format) || sourceBytesPerPixel == 0) {
            throw std::runtime_error(
                "Frame capture requires a supported sRGB or FP16 scene target.");
        }
        if (extent.width == 0 || extent.height == 0) {
            throw std::runtime_error("Frame capture requires a non-empty render extent.");
        }
        const uint64_t pixelCount = static_cast<uint64_t>(extent.width) *
            static_cast<uint64_t>(extent.height);
        if (pixelCount > std::numeric_limits<uint64_t>::max() /
            sourceBytesPerPixel) {
            throw std::overflow_error("Frame capture byte count exceeds uint64_t.");
        }
        const uint64_t byteCount = pixelCount * sourceBytesPerPixel;
        if (byteCount > std::numeric_limits<size_t>::max() ||
            static_cast<uint64_t>(extent.width) * sourceBytesPerPixel >
                std::numeric_limits<uint32_t>::max() ||
            pixelCount > std::numeric_limits<size_t>::max() /
                outputBytesPerPixel ||
            static_cast<uint64_t>(extent.width) * outputBytesPerPixel >
                std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("Frame capture dimensions exceed the readback contract.");
        }
        const auto duplicatePending = std::find_if(pendingFrameCaptures_.begin(),
            pendingFrameCaptures_.end(), [captureId](const PendingFrameCapture& capture) {
                return capture.captureId == captureId;
            });
        const auto duplicateCompleted = std::find_if(completedFrameCaptures_.begin(),
            completedFrameCaptures_.end(), [captureId](const FrameCapture& capture) {
                return capture.captureId == captureId;
            });
        if (duplicatePending != pendingFrameCaptures_.end() ||
            duplicateCompleted != completedFrameCaptures_.end()) {
            throw std::invalid_argument("Frame capture IDs must be unique.");
        }

        PendingFrameCapture pending{};
        pending.captureId = captureId;
        pending.frameIndex = scheduler.currentFrameIndex();
        pending.extent = extent;
        pending.format = format;
        pending.point = point;
        pending.readback = resourceAllocator.createBuffer(byteCount,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, ProfileMemoryCategory::CaptureReadback);

        try {
            pendingFrameCaptures_.push_back(std::move(pending));
        }
        catch (...) {
            resourceAllocator.destroy(pending.readback);
            throw;
        }

        PendingFrameCapture& recorded = pendingFrameCaptures_.back();
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        VulkanImageResource& source = point == FrameCapturePoint::SceneLinear
            ? targets.litScene : targets.output;
        VulkanCommandList commandList(currentCmd);
        if (point == FrameCapturePoint::SceneLinear) {
            renderGraph_.transitionImage(currentCmd, "scene.color",
                RenderGraph::Access::TransferSource);
        }
        else {
            renderGraph_.beginPass(currentCmd, "final-capture-hook");
            finalCaptureHookRecorded_ = true;
        }
        commandList.transition(recorded.readback, ResourceState::CopyDestination);

        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { extent.width, extent.height, 1 };
        commandList.copyImageToBuffer(source, recorded.readback, copy);
        if (point == FrameCapturePoint::SceneLinear) {
            renderGraph_.transitionImage(currentCmd, "scene.color",
                RenderGraph::Access::SampledRead);
        }
    }

    void VulkanVertexBackend::collectFrameCapturesForSlot(uint32_t frameIndex) {
        size_t index = 0;
        while (index < pendingFrameCaptures_.size()) {
            if (pendingFrameCaptures_[index].frameIndex != frameIndex) {
                ++index;
                continue;
            }

            PendingFrameCapture& pending = pendingFrameCaptures_[index];
            const auto pixelFormat = capturePixelFormat(pending.format);
            if (!pixelFormat || pending.readback.mapped == nullptr) {
                resourceAllocator.destroy(pending.readback);
                throw std::runtime_error("A completed frame capture has invalid readback state.");
            }
            const size_t pixelCount = static_cast<size_t>(pending.extent.width) *
                static_cast<size_t>(pending.extent.height);
            const bool sceneLinear =
                pending.point == FrameCapturePoint::SceneLinear;
            const bool floatCapture = pending.format ==
                VK_FORMAT_R16G16B16A16_SFLOAT;
            const size_t outputBytesPerPixel = floatCapture ? 16 : 4;
            const size_t byteCount = pixelCount * outputBytesPerPixel;
            FrameCapture completed{};
            completed.captureId = pending.captureId;
            completed.width = pending.extent.width;
            completed.height = pending.extent.height;
            completed.rowPitchBytes = pending.extent.width *
                static_cast<uint32_t>(outputBytesPerPixel);
            completed.pixelFormat = *pixelFormat;
            completed.colorDomain = sceneLinear
                ? FrameCaptureColorDomain::SceneLinearAcesCg
                : (floatCapture ? FrameCaptureColorDomain::DisplayLinearHdr
                    : FrameCaptureColorDomain::DisplayEncodedSdr);
            completed.pixels.resize(byteCount);
            if (floatCapture) {
                const auto* source = static_cast<const std::byte*>(
                    pending.readback.mapped);
                for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                    uint16_t channels[4]{};
                    std::memcpy(channels, source + pixel * 8, sizeof(channels));
                    float rgba[4] = { Color::halfToFloat(channels[0]),
                        Color::halfToFloat(channels[1]),
                        Color::halfToFloat(channels[2]),
                        Color::halfToFloat(channels[3]) };
                    std::memcpy(completed.pixels.data() + pixel * 16,
                        rgba, sizeof(rgba));
                }
            }
            else {
                std::memcpy(completed.pixels.data(), pending.readback.mapped,
                    byteCount);
            }
            completedFrameCaptures_.push_back(std::move(completed));
            resourceAllocator.destroy(pending.readback);
            if (index + 1 != pendingFrameCaptures_.size()) {
                pendingFrameCaptures_[index] =
                    std::move(pendingFrameCaptures_.back());
            }
            pendingFrameCaptures_.pop_back();
        }
    }

    std::vector<FrameCapture> VulkanVertexBackend::collectFrameCaptures(
        bool waitForPending) {
        if (frameOpen_) {
            throw std::logic_error(
                "Frame captures cannot be collected while a frame is open.");
        }
        if (waitForPending && !pendingFrameCaptures_.empty()) {
            scheduler.waitForAllFrames();
            for (uint32_t frameIndex = 0;
                frameIndex < VulkanFrameScheduler::FramesInFlight; ++frameIndex) {
                collectFrameCapturesForSlot(frameIndex);
            }
        }
        std::vector<FrameCapture> result = std::move(completedFrameCaptures_);
        completedFrameCaptures_.clear();
        return result;
    }

    void VulkanVertexBackend::requestOrdinary2CaptureValidation(
        uint64_t validationId) {
        if (!frameOpen_ || currentCmd == VK_NULL_HANDLE) {
            throw std::logic_error(
                "Ordinary2 capture validation must be requested during a frame");
        }
        const bool duplicatePending = std::ranges::any_of(
            pendingOrdinary2CaptureValidations_,
            [validationId](const PendingOrdinary2CaptureValidation& pending) {
                return pending.validationId == validationId;
            });
        const bool duplicateCompleted = std::ranges::any_of(
            completedOrdinary2CaptureValidations_,
            [validationId](const Ordinary2CaptureValidationResult& completed) {
                return completed.validationId == validationId;
            });
        if (ordinary2CaptureValidationRequest_ || duplicatePending ||
            duplicateCompleted) {
            throw std::invalid_argument(
                "Ordinary2 capture validation permits one unique request at a time");
        }
        ordinary2CaptureValidationRequest_ = validationId;
    }

    void VulkanVertexBackend::collectOrdinary2CaptureValidationsForSlot(
        uint32_t frameIndex) {
        constexpr uint32_t OrientationBit = 0x80000000u;
        constexpr uint32_t WorkMask = kLayeredInterfaceWorkMask;
        size_t pendingIndex = 0u;
        while (pendingIndex < pendingOrdinary2CaptureValidations_.size()) {
            PendingOrdinary2CaptureValidation& pending =
                pendingOrdinary2CaptureValidations_[pendingIndex];
            if (pending.frameIndex != frameIndex) {
                ++pendingIndex;
                continue;
            }
            if (pending.readback.mapped == nullptr) {
                resourceAllocator.destroy(pending.readback);
                throw std::runtime_error(
                    "Completed Ordinary2 readback is not host mapped");
            }

            Ordinary2CaptureValidationResult result{};
            result.validationId = pending.validationId;
            result.atlasWidth = pending.extent.width;
            result.atlasHeight = pending.extent.height;
            result.expectedDrawCount = pending.expectedDrawCount;
            result.workItemCount = pending.workItemCount;
            result.inspectedPixelCount =
                static_cast<uint64_t>(pending.extent.width) *
                pending.extent.height;
            const size_t pixelCount = static_cast<size_t>(
                result.inspectedPixelCount);
            const size_t imageBytes = pixelCount * sizeof(uint32_t);
            const auto* bytes = static_cast<const std::byte*>(
                pending.readback.mapped);
            const auto readUint = [&](size_t base, size_t pixel) {
                uint32_t value = 0u;
                std::memcpy(&value,
                    bytes + base + pixel * sizeof(uint32_t), sizeof(value));
                return value;
            };
            const auto readFloat = [&](size_t base, size_t pixel) {
                float value = 0.0f;
                std::memcpy(&value,
                    bytes + base + pixel * sizeof(float), sizeof(value));
                return value;
            };
            float minimumDelta = (std::numeric_limits<float>::max)();
            float maximumDelta = 0.0f;
            float minimumLocalAlpha = (std::numeric_limits<float>::max)();
            float maximumLocalAlpha = 0.0f;
            for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                const uint32_t entryIdentity = readUint(0u, pixel);
                const float entryDepth = readFloat(imageBytes, pixel);
                const uint32_t exitIdentity = readUint(
                    imageBytes * 2u, pixel);
                const float exitDepth = readFloat(
                    imageBytes * 3u, pixel);
                const bool hasEntry = entryIdentity != 0u;
                const bool hasExit = exitIdentity != 0u;
                const uint32_t entryWork = entryIdentity & WorkMask;
                const uint32_t exitWork = exitIdentity & WorkMask;
                const auto validDepth = [](float depth) {
                    return std::isfinite(depth) && depth >= 0.0f &&
                        depth <= 1.0f;
                };

                if (hasEntry) {
                    ++result.entryPixelCount;
                    if ((entryIdentity & OrientationBit) != 0u)
                        ++result.invalidOrientationPixelCount;
                    if (entryWork == 0u ||
                        entryWork > pending.workItemCount)
                        ++result.invalidWorkIndexPixelCount;
                    if (!validDepth(entryDepth))
                        ++result.invalidDepthPixelCount;
                }
                if (hasExit) {
                    ++result.exitPixelCount;
                    if ((exitIdentity & OrientationBit) == 0u)
                        ++result.invalidOrientationPixelCount;
                    if (exitWork == 0u || exitWork > pending.workItemCount)
                        ++result.invalidWorkIndexPixelCount;
                    if (!validDepth(exitDepth))
                        ++result.invalidDepthPixelCount;
                    if (!hasEntry) {
                        ++result.unpairedExitPixelCount;
                    }
                    else {
                        ++result.pairedPixelCount;
                        if (entryWork != exitWork)
                            ++result.workMismatchPixelCount;
                        if (validDepth(entryDepth) && validDepth(exitDepth)) {
                            if (!(exitDepth > entryDepth)) {
                                ++result.nonIncreasingDepthPixelCount;
                            }
                            else {
                                const float delta = exitDepth - entryDepth;
                                minimumDelta = (std::min)(minimumDelta, delta);
                                maximumDelta = (std::max)(maximumDelta, delta);
                            }
                        }
                    }
                }
                if (hasEntry && !hasExit)
                    ++result.entryOnlyPixelCount;

                std::array<uint16_t, 4> localHalf{};
                std::memcpy(localHalf.data(),
                    bytes + imageBytes * 4u + pixel * 8u, 8u);
                const std::array<float, 4> local{
                    Color::halfToFloat(localHalf[0]),
                    Color::halfToFloat(localHalf[1]),
                    Color::halfToFloat(localHalf[2]),
                    Color::halfToFloat(localHalf[3]) };
                const bool occupied = local[0] != 0.0f ||
                    local[1] != 0.0f || local[2] != 0.0f ||
                    local[3] != 0.0f;
                if (occupied) {
                    ++result.localColorPixelCount;
                    const bool valid = std::ranges::all_of(local,
                        [](float value) { return std::isfinite(value); }) &&
                        local[0] >= 0.0f && local[1] >= 0.0f &&
                        local[2] >= 0.0f && local[3] > 0.0f &&
                        local[3] <= 1.0f;
                    if (!valid) {
                        ++result.localColorInvalidPixelCount;
                    }
                    else {
                        minimumLocalAlpha = (std::min)(minimumLocalAlpha,
                            local[3]);
                        maximumLocalAlpha = (std::max)(maximumLocalAlpha,
                            local[3]);
                    }
                }
            }
            if (minimumDelta != (std::numeric_limits<float>::max)()) {
                result.minimumPairedDepthDelta = minimumDelta;
                result.maximumPairedDepthDelta = maximumDelta;
            }
            if (minimumLocalAlpha != (std::numeric_limits<float>::max)()) {
                result.minimumLocalAlpha = minimumLocalAlpha;
                result.maximumLocalAlpha = maximumLocalAlpha;
            }
            completedOrdinary2CaptureValidations_.push_back(result);
            resourceAllocator.destroy(pending.readback);
            if (pendingIndex + 1u !=
                    pendingOrdinary2CaptureValidations_.size()) {
                pendingOrdinary2CaptureValidations_[pendingIndex] = std::move(
                    pendingOrdinary2CaptureValidations_.back());
            }
            pendingOrdinary2CaptureValidations_.pop_back();
        }
    }

    std::vector<Ordinary2CaptureValidationResult>
    VulkanVertexBackend::collectOrdinary2CaptureValidations(
        bool waitForPending) {
        if (frameOpen_) {
            throw std::logic_error(
                "Ordinary2 validation cannot be collected during a frame");
        }
        if (waitForPending &&
            !pendingOrdinary2CaptureValidations_.empty()) {
            scheduler.waitForAllFrames();
            for (uint32_t frameIndex = 0;
                frameIndex < VulkanFrameScheduler::FramesInFlight;
                ++frameIndex) {
                collectOrdinary2CaptureValidationsForSlot(frameIndex);
            }
        }
        std::vector<Ordinary2CaptureValidationResult> result =
            std::move(completedOrdinary2CaptureValidations_);
        completedOrdinary2CaptureValidations_.clear();
        return result;
    }

    void VulkanVertexBackend::requestDeepLayeredCaptureValidation(
        uint64_t validationId, TransparencyQuality quality) {
        if (!frameOpen_ || currentCmd == VK_NULL_HANDLE) {
            throw std::logic_error(
                "Deep layered validation must be requested during a frame");
        }
        if (quality != TransparencyQuality::Hero4 &&
            quality != TransparencyQuality::Cinematic8) {
            throw std::invalid_argument(
                "Deep layered validation requires Hero4 or Cinematic8");
        }
        const bool duplicatePending = std::ranges::any_of(
            pendingDeepLayeredCaptureValidations_,
            [validationId](
                const PendingDeepLayeredCaptureValidation& pending) {
                return pending.validationId == validationId;
            });
        const bool duplicateCompleted = std::ranges::any_of(
            completedDeepLayeredCaptureValidations_,
            [validationId](
                const DeepLayeredCaptureValidationResult& completed) {
                return completed.validationId == validationId;
            });
        if (deepLayeredCaptureValidationRequest_ || duplicatePending ||
            duplicateCompleted) {
            throw std::invalid_argument(
                "Deep layered validation permits one unique request at a time");
        }
        deepLayeredCaptureValidationRequest_ =
            DeepLayeredCaptureValidationRequest{ validationId, quality };
    }

    void VulkanVertexBackend::collectDeepLayeredCaptureValidationsForSlot(
        uint32_t frameIndex) {
        constexpr uint32_t OrientationBit = 0x80000000u;
        constexpr uint32_t WorkMask = kDeepLayeredWorkMask;
        size_t pendingIndex = 0u;
        while (pendingIndex < pendingDeepLayeredCaptureValidations_.size()) {
            PendingDeepLayeredCaptureValidation& pending =
                pendingDeepLayeredCaptureValidations_[pendingIndex];
            if (pending.frameIndex != frameIndex) {
                ++pendingIndex;
                continue;
            }
            if (pending.readback.mapped == nullptr) {
                resourceAllocator.destroy(pending.readback);
                throw std::runtime_error(
                    "Completed deep layered readback is not host mapped");
            }

            DeepLayeredCaptureValidationResult result{};
            result.validationId = pending.validationId;
            result.quality = pending.quality;
            result.atlasWidth = pending.extent.width;
            result.atlasHeight = pending.extent.height;
            result.interfaceCount = pending.interfaceCount;
            result.expectedDrawCount = pending.expectedDrawCount;
            result.sceneResolveDrawCount = pending.sceneResolveDrawCount;
            result.compatibilityForwardDrawCount =
                pending.compatibilityForwardDrawCount;
            result.workItemCount = pending.workItemCount;
            result.inspectedPixelCount =
                static_cast<uint64_t>(pending.extent.width) *
                pending.extent.height;
            const size_t pixelCount = static_cast<size_t>(
                result.inspectedPixelCount);
            const size_t imageBytes = pixelCount * sizeof(uint32_t);
            const size_t localColorOffset = imageBytes *
                pending.interfaceCount * 2u;
            const uint32_t tileWidth = (pending.extent.width +
                kDeepLayeredEarlyTerminationTileSize - 1u) /
                kDeepLayeredEarlyTerminationTileSize;
            const uint32_t tileHeight = (pending.extent.height +
                kDeepLayeredEarlyTerminationTileSize - 1u) /
                kDeepLayeredEarlyTerminationTileSize;
            const size_t tileImageBytes = static_cast<size_t>(tileWidth) *
                tileHeight * sizeof(uint32_t);
            const size_t tileBaseOffset = localColorOffset + pixelCount * 8u;
            const auto* bytes = static_cast<const std::byte*>(
                pending.readback.mapped);
            const auto readUint = [&](size_t base, size_t pixel) {
                uint32_t value = 0u;
                std::memcpy(&value,
                    bytes + base + pixel * sizeof(uint32_t), sizeof(value));
                return value;
            };
            const auto readFloat = [&](size_t base, size_t pixel) {
                float value = 0.0f;
                std::memcpy(&value,
                    bytes + base + pixel * sizeof(float), sizeof(value));
                return value;
            };
            const auto validDepth = [](float depth) {
                return std::isfinite(depth) && depth >= 0.0f &&
                    depth <= 1.0f;
            };
            float minimumDelta = (std::numeric_limits<float>::max)();
            float maximumDelta = 0.0f;
            float minimumLocalAlpha = (std::numeric_limits<float>::max)();
            float maximumLocalAlpha = 0.0f;

            for (size_t pixel = 0u; pixel < pixelCount; ++pixel) {
                std::array<uint32_t, kMaximumLayeredInterfaceCount>
                    openWorks{};
                uint32_t openCount = 0u;
                uint32_t observedCount = 0u;
                uint32_t maximumOpenCount = 0u;
                uint32_t pairCount = 0u;
                uint32_t lastIdentity = 0u;
                bool crossingPair = false;
                bool seenEmpty = false;
                bool pixelInvalid = false;
                bool hasPreviousDepth = false;
                float previousDepth = 0.0f;
                for (uint32_t interfaceIndex = 0u;
                    interfaceIndex < pending.interfaceCount;
                    ++interfaceIndex) {
                    const size_t identityOffset = imageBytes *
                        (interfaceIndex * 2u);
                    const size_t depthOffset = identityOffset + imageBytes;
                    const uint32_t identity = readUint(identityOffset, pixel);
                    if (identity == 0u) {
                        seenEmpty = true;
                        continue;
                    }
                    ++result.interfacePixelCounts[interfaceIndex];
                    ++observedCount;
                    lastIdentity = identity;
                    if (seenEmpty) {
                        ++result.interfaceGapPixelCount;
                        pixelInvalid = true;
                    }
                    const uint32_t work = identity & WorkMask;
                    if (work == 0u || work > pending.workItemCount) {
                        ++result.invalidWorkIndexPixelCount;
                        pixelInvalid = true;
                    }
                    const float depth = readFloat(depthOffset, pixel);
                    if (!validDepth(depth)) {
                        ++result.invalidDepthPixelCount;
                        pixelInvalid = true;
                    }
                    else if (hasPreviousDepth) {
                        if (!(depth > previousDepth)) {
                            ++result.nonIncreasingDepthPixelCount;
                            pixelInvalid = true;
                        }
                        else {
                            const float delta = depth - previousDepth;
                            minimumDelta = (std::min)(minimumDelta, delta);
                            maximumDelta = (std::max)(maximumDelta, delta);
                        }
                    }
                    if (validDepth(depth)) {
                        previousDepth = depth;
                        hasPreviousDepth = true;
                    }

                    const bool exit = (identity & OrientationBit) != 0u;
                    if (!exit) {
                        const bool duplicate = std::find(
                            openWorks.begin(), openWorks.begin() + openCount,
                            work) != openWorks.begin() + openCount;
                        if (duplicate ||
                            openCount >= openWorks.size()) {
                            ++result.duplicateEntryPixelCount;
                            pixelInvalid = true;
                        }
                        else {
                            openWorks[openCount++] = work;
                            maximumOpenCount = (std::max)(maximumOpenCount,
                                openCount);
                        }
                    }
                    else {
                        uint32_t match = openCount;
                        while (match > 0u &&
                            openWorks[match - 1u] != work) {
                            --match;
                        }
                        if (match == 0u) {
                            ++result.unmatchedExitPixelCount;
                            pixelInvalid = true;
                        }
                        else {
                            const uint32_t matchIndex = match - 1u;
                            // Closing something other than the most recently
                            // opened work proves a valid crossing sequence:
                            // Entry(A), Entry(B), Exit(A), Exit(B).
                            crossingPair |= matchIndex + 1u != openCount;
                            for (uint32_t move = matchIndex + 1u;
                                move < openCount; ++move) {
                                openWorks[move - 1u] = openWorks[move];
                            }
                            --openCount;
                            ++pairCount;
                        }
                    }
                }
                result.maximumObservedInterfaceCount = (std::max)(
                    result.maximumObservedInterfaceCount, observedCount);
                if (openCount != 0u) {
                    if (observedCount == pending.interfaceCount) {
                        // A topology-validated closed workload that fills the
                        // tier while volumes remain open is a saturated exact
                        // prefix, not malformed capture. The unmatched entry
                        // and uncaptured entry surfaces are evaluated by the
                        // bounded residual material path.
                        ++result.saturatedResidualPixelCount;
                    }
                    else {
                        ++result.unclosedEntryPixelCount;
                        pixelInvalid = true;
                    }
                }
                const bool paired = !pixelInvalid && pairCount != 0u;
                if (paired) {
                    ++result.pairedPixelCount;
                    if (observedCount >= 4u && maximumOpenCount >= 2u)
                        ++result.nestedFourInterfacePixelCount;
                    if (crossingPair)
                        ++result.crossingPairPixelCount;
                    if (observedCount < pending.interfaceCount &&
                        deepLayeredOpenCount(lastIdentity) == 0u &&
                        deepLayeredTransmissionQuantized(lastIdentity) <=
                            kDeepLayeredTerminationThresholdQuantized) {
                        ++result.earlyTerminatedPixelCount;
                    }
                }

                std::array<uint16_t, 4> localHalf{};
                std::memcpy(localHalf.data(),
                    bytes + localColorOffset + pixel * 8u, 8u);
                const std::array<float, 4> local{
                    Color::halfToFloat(localHalf[0]),
                    Color::halfToFloat(localHalf[1]),
                    Color::halfToFloat(localHalf[2]),
                    Color::halfToFloat(localHalf[3]) };
                const bool occupied = local[0] != 0.0f ||
                    local[1] != 0.0f || local[2] != 0.0f ||
                    local[3] != 0.0f;
                if (occupied) {
                    ++result.localColorPixelCount;
                    const bool valid = std::ranges::all_of(local,
                        [](float value) { return std::isfinite(value); }) &&
                        local[0] >= 0.0f && local[1] >= 0.0f &&
                        local[2] >= 0.0f && local[3] > 0.0f &&
                        local[3] <= 1.0f;
                    if (!valid || !paired) {
                        ++result.localColorInvalidPixelCount;
                    }
                    else {
                        minimumLocalAlpha = (std::min)(minimumLocalAlpha,
                            local[3]);
                        maximumLocalAlpha = (std::max)(maximumLocalAlpha,
                            local[3]);
                    }
                }
            }

            for (uint32_t interfaceIndex = 0u;
                interfaceIndex < pending.interfaceCount; ++interfaceIndex) {
                if (!deepLayeredTerminationInterface(interfaceIndex,
                        pending.interfaceCount)) {
                    continue;
                }
                const size_t identityOffset = imageBytes *
                    (interfaceIndex * 2u);
                const size_t maskOffset = tileBaseOffset +
                    tileImageBytes * interfaceIndex;
                for (uint32_t tileY = 0u; tileY < tileHeight; ++tileY) {
                    for (uint32_t tileX = 0u; tileX < tileWidth; ++tileX) {
                        const size_t tileIndex = static_cast<size_t>(tileY) *
                            tileWidth + tileX;
                        if (readUint(maskOffset, tileIndex) == 0u)
                            continue;
                        bool occupied = false;
                        const uint32_t beginX = tileX *
                            kDeepLayeredEarlyTerminationTileSize;
                        const uint32_t beginY = tileY *
                            kDeepLayeredEarlyTerminationTileSize;
                        const uint32_t endX = (std::min)(beginX +
                            kDeepLayeredEarlyTerminationTileSize,
                            pending.extent.width);
                        const uint32_t endY = (std::min)(beginY +
                            kDeepLayeredEarlyTerminationTileSize,
                            pending.extent.height);
                        for (uint32_t y = beginY; y < endY && !occupied;
                            ++y) {
                            for (uint32_t x = beginX; x < endX; ++x) {
                                const size_t pixel = static_cast<size_t>(y) *
                                    pending.extent.width + x;
                                if ((readUint(identityOffset, pixel) &
                                        kDeepLayeredWorkMask) != 0u) {
                                    occupied = true;
                                    break;
                                }
                            }
                        }
                        if (occupied) {
                            ++result.terminatedOccupiedTileCounts[
                                interfaceIndex];
                            ++result.terminatedOccupiedTileCount;
                        }
                    }
                }
            }
            if (minimumDelta != (std::numeric_limits<float>::max)()) {
                result.minimumDepthDelta = minimumDelta;
                result.maximumDepthDelta = maximumDelta;
            }
            if (minimumLocalAlpha != (std::numeric_limits<float>::max)()) {
                result.minimumLocalAlpha = minimumLocalAlpha;
                result.maximumLocalAlpha = maximumLocalAlpha;
            }
            completedDeepLayeredCaptureValidations_.push_back(result);
            resourceAllocator.destroy(pending.readback);
            if (pendingIndex + 1u !=
                    pendingDeepLayeredCaptureValidations_.size()) {
                pendingDeepLayeredCaptureValidations_[pendingIndex] =
                    std::move(pendingDeepLayeredCaptureValidations_.back());
            }
            pendingDeepLayeredCaptureValidations_.pop_back();
        }
    }

    std::vector<DeepLayeredCaptureValidationResult>
    VulkanVertexBackend::collectDeepLayeredCaptureValidations(
        bool waitForPending) {
        if (frameOpen_) {
            throw std::logic_error(
                "Deep layered validation cannot be collected during a frame");
        }
        if (waitForPending &&
            !pendingDeepLayeredCaptureValidations_.empty()) {
            scheduler.waitForAllFrames();
            for (uint32_t frameIndex = 0u;
                frameIndex < VulkanFrameScheduler::FramesInFlight;
                ++frameIndex) {
                collectDeepLayeredCaptureValidationsForSlot(frameIndex);
            }
        }
        std::vector<DeepLayeredCaptureValidationResult> result =
            std::move(completedDeepLayeredCaptureValidations_);
        completedDeepLayeredCaptureValidations_.clear();
        return result;
    }

    void VulkanVertexBackend::destroyPendingFrameCaptures() noexcept {
        for (PendingFrameCapture& pending : pendingFrameCaptures_) {
            resourceAllocator.destroy(pending.readback);
        }
        pendingFrameCaptures_.clear();
    }

    void VulkanVertexBackend::destroyPendingOrdinary2CaptureValidations()
        noexcept {
        for (PendingOrdinary2CaptureValidation& pending :
                pendingOrdinary2CaptureValidations_) {
            resourceAllocator.destroy(pending.readback);
        }
        pendingOrdinary2CaptureValidations_.clear();
    }

    void VulkanVertexBackend::destroyPendingDeepLayeredCaptureValidations()
        noexcept {
        for (PendingDeepLayeredCaptureValidation& pending :
                pendingDeepLayeredCaptureValidations_) {
            resourceAllocator.destroy(pending.readback);
        }
        pendingDeepLayeredCaptureValidations_.clear();
    }

    void VulkanVertexBackend::submitOutputPass() {
        {
            CpuScope outputRecordScope(cpuProfiler_,
                "cpu.render.record.output_transform");
            VulkanFrameContextTargets& targets = frameTargets.get(
                scheduler.currentFrameIndex());
            {
                VulkanGpuScope transitionGpuScope(scheduler,
                    "gpu.output.graph_transition");
                // M1 exposes scene-linear color to a future bloom implementation
                // without paying for a disabled effect or changing resource versions.
                renderGraph_.skipPass("bloom-hook");
                renderGraph_.beginPass(currentCmd, "output-transform");
            }
            {
                VulkanGpuScope outputGpuScope(scheduler, "gpu.output.transform");
                outputPass.record(currentCmd, scheduler.currentFrameIndex(),
                    targets.outputFramebuffer, frameTargets.extent(), manualExposureEv_,
                    static_cast<uint32_t>(outputOperator_),
                    static_cast<uint32_t>(outputTransport_), paperWhiteNits_, peakNits_,
                    selectionOutlineActive_);
                if (collectFrameCounters_) {
                    recordPipelineBind(pipelineIdentity(
                        FixedPipelineIdentity::OutputTransform));
                    recordDraw(frameCounters_.drawOutput, 1);
                }
            }
        }
    }

    void VulkanVertexBackend::submitUIPass() {
        if (!finalCaptureHookRecorded_) {
            renderGraph_.skipPass("final-capture-hook");
        }
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.ui");
        renderGraph_.beginPass(currentCmd,
            outputTransport_ == Color::OutputTransport::Hdr10Pq
                ? "ui-compose" : "ui-present");
        {
        VulkanGpuScope gpuScope(scheduler, "gpu.ui");
        ImGui::Render();
        VkRenderPassBeginInfo uiPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        uiPassInfo.renderPass = uiPass->getRenderPass();
        uiPassInfo.framebuffer = outputTransport_ == Color::OutputTransport::Hdr10Pq
            ? frameTargets.get(scheduler.currentFrameIndex()).uiCompositionFramebuffer
            : frameTargets.uiFramebuffer(currentImageIndex);
        uiPassInfo.renderArea.extent = vkSwapchain->getExtent();

        VkClearValue uiClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        uiPassInfo.clearValueCount = 1;
        uiPassInfo.pClearValues = &uiClearColor;

        vkCmdBeginRenderPass(currentCmd, &uiPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Because we abstracted the UI pass, the backend just asks ImGui to record 
        // its internal vertex buffers into the current command buffer.
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data) {
            const int framebufferWidth = static_cast<int>(
                draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
            const int framebufferHeight = static_cast<int>(
                draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
            if (collectFrameCounters_ && framebufferWidth > 0 && framebufferHeight > 0) {
                recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::ImGui));
                const ImVec2 clipOffset = draw_data->DisplayPos;
                const ImVec2 clipScale = draw_data->FramebufferScale;
                for (const ImDrawList* drawList : draw_data->CmdLists) {
                    for (const ImDrawCmd& command : drawList->CmdBuffer) {
                        if (command.UserCallback != nullptr) {
                            if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                                recordPipelineBind(
                                    pipelineIdentity(FixedPipelineIdentity::ImGui));
                            }
                            else {
                                ++frameCounters_.uiUntrackedCallbacks;
                            }
                            continue;
                        }

                        ImVec2 clipMinimum{
                            (command.ClipRect.x - clipOffset.x) * clipScale.x,
                            (command.ClipRect.y - clipOffset.y) * clipScale.y };
                        ImVec2 clipMaximum{
                            (command.ClipRect.z - clipOffset.x) * clipScale.x,
                            (command.ClipRect.w - clipOffset.y) * clipScale.y };
                        clipMinimum.x = std::max(clipMinimum.x, 0.0f);
                        clipMinimum.y = std::max(clipMinimum.y, 0.0f);
                        clipMaximum.x = std::min(clipMaximum.x,
                            static_cast<float>(framebufferWidth));
                        clipMaximum.y = std::min(clipMaximum.y,
                            static_cast<float>(framebufferHeight));
                        if (clipMaximum.x <= clipMinimum.x ||
                            clipMaximum.y <= clipMinimum.y) {
                            continue;
                        }

                        recordDraw(frameCounters_.drawUi, command.ElemCount / 3);
                    }
                }
            }
            ImGui_ImplVulkan_RenderDrawData(draw_data, currentCmd);
        }

        vkCmdEndRenderPass(currentCmd);
        }
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            renderGraph_.beginPass(currentCmd,
                "hdr10-encode-present");
            VulkanGpuScope encodeScope(scheduler, "gpu.output.hdr10_encode");
            hdrEncodePass.record(currentCmd, scheduler.currentFrameIndex(),
                currentImageIndex, vkSwapchain->getExtent(), paperWhiteNits_,
                peakNits_);
            if (collectFrameCounters_) {
                recordPipelineBind(pipelineIdentity(
                    FixedPipelineIdentity::OutputTransform));
                recordDraw(frameCounters_.drawOutput, 1);
            }
        }
        renderGraph_.finishFrameExecution();
    }

    FrameStatus VulkanVertexBackend::endFrame() {
        const FrameStatus status = scheduler.endFrame(
            vkSwapchain->getSwapchain(), currentImageIndex);
        frameOpen_ = false;
        if (collectFrameCounters_ && cpuProfiler_ != nullptr) {
            cpuProfiler_->recordMemorySnapshot(memorySnapshot());
        }
        emitFrameCounters();
        return status;
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

    // ==============================================================================
    // 4. EDITOR & UI ABSTRACTIONS
    // ==============================================================================

    void VulkanVertexBackend::beginUI() {
        // We initialize the specific backend frames here so the high-level 
        // EditorSystem doesn't need to know we are using Vulkan or GLFW.
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    void* VulkanVertexBackend::getLitSceneTextureID() {
        // uiSceneTextures is the std::vector<VkDescriptorSet> we registered with ImGui during init().
        // We cast it to void* so it can securely cross the API boundary into your ViewportPanel.
        return (void*)uiSceneTextures[scheduler.currentFrameIndex()];
    }

    void* VulkanVertexBackend::getGlassDepthTextureID() {
        return (void*)uiDepthTextures[scheduler.currentFrameIndex()];
    }

    void* VulkanVertexBackend::getEditorTextureID(TextureHandle texture) {
        VulkanTexturePayload* payload = textureVault.get(texture);
        if (payload == nullptr || payload->retired || !imguiInitialized_) {
            return nullptr;
        }
        if (payload->imguiDescriptor == VK_NULL_HANDLE) {
            payload->imguiDescriptor = ImGui_ImplVulkan_AddTexture(payload->sampler,
                payload->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return reinterpret_cast<void*>(payload->imguiDescriptor);
    }

} 

// namespace Iridium
