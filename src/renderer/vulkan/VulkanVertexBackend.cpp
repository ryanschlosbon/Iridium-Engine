#include "VulkanVertexBackend.h"
#include "VulkanProductionRenderGraph.h"
#include "VulkanGBufferLayout.h"
#include "renderer/color/SceneColor.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "renderer/rhi/LightUploadPlanner.h"
#include "renderer/rhi/GpuSceneUploadPlanner.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/transparency/WeightedOit.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/ClusteredReflectionProbes.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"
#include "vendor/imguizmo/ImGuizmo.h"
#include "profiling/CpuProfiler.h"
#include "utils/File.h"
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
#include <tuple>
#include <utility>
#include <vector>

namespace Iridium {

    namespace {
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

        // CPU LOD selectors for the qualification oracle's expected commands.
        struct DensityLodContext {
            float worldUnitsPerTexel = 0.0f;
            float errorTexels = 0.0f;
        };
        IndirectLodMetric densityLodMetric(const DensityLodContext& context) {
            return { &context, [](const void* opaque,
                const VulkanIndirectScene& scene,
                const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) {
                const auto& lod = *static_cast<const DensityLodContext*>(opaque);
                return selectGpuSceneDensityLodGeometry(scene.geometries,
                    primitive.binding.y, scene.instances[primitive.binding.x],
                    lod.worldUnitsPerTexel, lod.errorTexels, maximumLod);
            } };
        }
        struct PerspectiveLodContext {
            glm::mat4 worldToClip{ 1.0f };
            glm::vec2 viewportPixels{ 0.0f };
            float errorPixels = 0.0f;
        };
        IndirectLodMetric perspectiveLodMetric(
            const PerspectiveLodContext& context) {
            return { &context, [](const void* opaque,
                const VulkanIndirectScene& scene,
                const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) {
                const auto& lod =
                    *static_cast<const PerspectiveLodContext*>(opaque);
                const GpuSceneInstanceRecord& instance =
                    scene.instances[primitive.binding.x];
                const glm::mat4 clipFromLocal = lod.worldToClip *
                    unpackGpuSceneAffine(scene.transforms[instance.references.x]);
                return selectGpuSceneLodGeometry(scene.geometries,
                    primitive.binding.y, clipFromLocal, lod.viewportPixels,
                    lod.errorPixels, maximumLod);
            } };
        }
        struct RadialLodContext {
            glm::vec3 position{ 0.0f };
            float resolution = 0.0f;
            float errorPixels = 0.0f;
        };
        IndirectLodMetric radialLodMetric(const RadialLodContext& context) {
            return { &context, [](const void* opaque,
                const VulkanIndirectScene& scene,
                const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) {
                const auto& lod = *static_cast<const RadialLodContext*>(opaque);
                return selectGpuSceneRadialLodGeometry(scene.geometries,
                    primitive.binding.y, scene.instances[primitive.binding.x],
                    lod.position, lod.resolution, lod.errorPixels, maximumLod);
            } };
        }

        constexpr uint64_t FixedPipelineIdentityMask = uint64_t{ 1 } << 63;



        enum class FixedPipelineIdentity : uint64_t {
            GBufferWireframe = FixedPipelineIdentityMask | 1,
            SelectionMask = FixedPipelineIdentityMask | 2,
            DeferredLighting = FixedPipelineIdentityMask | 3,
            SelectionOutline = FixedPipelineIdentityMask | 4,
            RetiredGlassDepth = FixedPipelineIdentityMask | 5, // reserved; never reuse
            ImGui = FixedPipelineIdentityMask | 6,
            OutputTransform = FixedPipelineIdentityMask | 7,
            LayeredInterfaceCapture = FixedPipelineIdentityMask | 8,
            LayeredLocalComposition = FixedPipelineIdentityMask | 9,
            LayeredSceneResolve = FixedPipelineIdentityMask | 10,
            LayeredResidualComposition = FixedPipelineIdentityMask | 11,
            LayeredTileTermination = FixedPipelineIdentityMask | 12,
            WeightedOitAccumulation = FixedPipelineIdentityMask | 13,
            WeightedOitResolve = FixedPipelineIdentityMask | 14,
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

        void appendFnv1a(uint64_t& hash, const void* data,
            size_t size) noexcept {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        }

        // The history depth was rasterized from a complete camera transform.
        // Hashing only the projection allowed ordinary camera translation or
        // rotation to consume depth from a different view pose.
        uint64_t viewProjectionRevision(const glm::mat4& view,
            const glm::mat4& projection) noexcept {
            uint64_t hash = 1469598103934665603ull;
            appendFnv1a(hash, &view, sizeof(view));
            appendFnv1a(hash, &projection, sizeof(projection));
            return hash == 0u ? 1u : hash;
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

    // ==============================================================================
    // 1. SYSTEM LIFECYCLE
    // ==============================================================================

    void VulkanVertexBackend::init(GLFWwindow* window, const RenderBackendConfig& config) {
        if (initialized_) {
            throw std::logic_error("VulkanVertexBackend was initialized more than once.");
        }

        // Extensions see the configuration before any graph or resource
        // exists; their hook declarations and oracle are fixed for the process.
        for (IVulkanBackendExtension* extension : extensions_)
            extension->configure(config);
        graphHooks_ = VulkanGraphHooks::none();
        for (IVulkanBackendExtension* extension : extensions_)
            graphHooks_ = graphHooks_ | extension->graphHooks();
        cpuProfiler_ = config.cpuProfiler;
        gBufferLayout_ = config.gBufferLayout;
        depthPyramidEnabled_ = config.experimentalDepthPyramid ||
            config.experimentalDepthOcclusionQuery ||
            config.experimentalDepthOcclusionRejection;
        depthOcclusionQueryEnabled_ = config.experimentalDepthOcclusionQuery ||
            config.experimentalDepthOcclusionRejection;
        depthOcclusionRejectionEnabled_ =
            config.experimentalDepthOcclusionRejection;
        weightedOitOrderSeed_ = config.weightedOitOrderSeed;
        forceDirectGBufferReference_ = config.forceDirectGBufferReference;
        forceDirectShadowReference_ = config.forceDirectShadowReference;
        experimentalShadowLodErrorTexels_ =
            config.experimentalShadowLodErrorTexels;
        shadowLodMaximumLevel_ = (std::min)(config.shadowLodMaximumLevel,
            MaximumGpuSceneLodLevels - 1u);
        experimentalGpuLodErrorPixels_ = config.experimentalGpuLodErrorPixels;
        gpuLodMaximumLevel_ = (std::min)(config.gpuLodMaximumLevel, MaximumGpuSceneLodLevels - 1u);
        gpuLodHysteresisFraction_ = config.gpuLodHysteresisFraction;
        experimentalProbeLodErrorPixels_ =
            config.experimentalProbeLodErrorPixels;
        probeLodMaximumLevel_ = (std::min)(config.probeLodMaximumLevel,
            MaximumGpuSceneLodLevels - 1u);
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
		requestedOutputTransport_ = config.outputTransport;
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
            config.enableTransparentPipelineStatistics, window,
            config.enableValidation && config.enableSynchronizationValidation);
        if (!vkContext->hasDescriptorIndexing()) {
            throw std::runtime_error(
                "Indexed material descriptors are required for the production path, "
                "but the Vulkan device lacks the complete descriptor-indexing feature set.");
        }
        resourceAllocator.init(vkContext->getPhysicalDevice(), vkContext->getDevice(),
            vkContext->hasMemoryBudget());
        // The VSM depth oracle qualifies live VSM demand, so it implies the
        // resources.
        if (config.experimentalVirtualShadowResources ||
            activeIndirectOracle(VulkanIndirectOracleView::VirtualShadowDepth)) {
            virtualShadowClipPageSize_ = config.virtualShadowResources.pageSizeTexels;
            virtualShadowResources_.init(vkContext->getDevice(), resourceAllocator,
                vkContext->getPhysicalDeviceProperties().limits,
                config.virtualShadowResources,
                VulkanFrameScheduler::FramesInFlight);
        }
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
        meshLayouts.init(vkContext->getDevice(),
            lightingPipeline->getDescriptorSetLayout(),
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout());
        reflectionProbeCapturePass_.init(vkContext->getDevice(),
            vkContext->getPhysicalDevice(), resourceAllocator,
            descriptorAllocator, indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            lightingPipeline->getDescriptorSetLayout(),
            meshLayouts.getGpuSceneSetLayout());
        reflectionProbeCaptureTargets_.init(vkContext->getDevice(),
            vkContext->getPhysicalDevice(), resourceAllocator,
            reflectionProbeCapturePass_.renderPass());
        forwardPass = std::make_unique<VkForwardRenderPass>(vkContext.get(),
            VulkanSceneColorFormat, VK_FORMAT_D32_SFLOAT);
        transparentPass = std::make_unique<VkForwardRenderPass>(vkContext.get(),
            VulkanSceneColorFormat, VK_FORMAT_D32_SFLOAT, true);
		outputPass.init(*vkContext, descriptorAllocator, outputTargetFormat_);
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            hdrEncodePass.init(*vkContext, descriptorAllocator,
                vkSwapchain->getImageFormat());
        }
        createGpuSceneCullPipeline();
        weightedOit_.init(vkContext->getDevice(), descriptorAllocator,
            meshLayouts.getForwardPipelineLayout());
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
        if (depthPyramidEnabled_) depthPyramid_.init(vkContext->getDevice(),
            descriptorAllocator, resourceAllocator,
            meshLayouts.getGlobalSetLayout(),
            meshLayouts.getGpuSceneSetLayout());
        directionalShadow_.init(vkContext->getDevice(), resourceAllocator,
            uploadContext, descriptorAllocator,
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            meshLayouts.getGpuSceneSetLayout(),
            directionalShadowResolution_);
        createDirectionalShadowIndirectPipeline();
        directionalCuller_.resize(512u, frameOpen_);
        createReflectionProbeIndirectPipeline();
        probeCuller_.resize(512u, frameOpen_);
        if (vkContext->getPhysicalDeviceProperties().limits.maxUniformBufferRange <
            sizeof(VulkanSpotShadowData)) {
            throw std::runtime_error(
                "Vulkan uniform-buffer range cannot hold the spot shadow table");
        }
        spotShadow_.init(vkContext->getDevice(), resourceAllocator,
            uploadContext, descriptorAllocator,
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            meshLayouts.getGpuSceneSetLayout(),
            spotShadowAtlasResolution_);
        createSpotShadowIndirectPipeline();
        spotCuller_.resize(512u, frameOpen_);
        if (vkContext->getPhysicalDeviceProperties().limits.maxUniformBufferRange <
            sizeof(VulkanPointShadowData)) {
            throw std::runtime_error(
                "Vulkan uniform-buffer range cannot hold the point shadow table");
        }
        pointShadow_.init(vkContext->getDevice(), resourceAllocator,
            uploadContext, descriptorAllocator,
            indexedTextureTable_.materialViewLayout(),
            indexedTextureTable_.samplerLayout(),
            meshLayouts.getGpuSceneSetLayout(), pointShadowCapacities_);
        createPointShadowIndirectPipeline();
        pointCuller_.resize(512u, frameOpen_);

        // 3. G-Buffer Pass
        gBufferPass = std::make_unique<VkRenderPassWrapper>(vkContext.get(),
            vkSwapchain.get(), gBufferLayout_);
        gBufferPipeline = std::make_unique<VkGraphicsPipeline>(vkContext.get(), vkSwapchain.get(), gBufferPass.get(),
            meshLayouts.getGBufferPipelineLayout(), gBufferLayout_);

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
        const uint64_t storageRange = vkContext->getPhysicalDeviceProperties()
            .limits.maxStorageBufferRange;
        gpuSceneMaximumCapacity_ = {
            static_cast<uint32_t>(storageRange /
                sizeof(GpuSceneAffineTransform)),
            static_cast<uint32_t>(storageRange /
                sizeof(GpuSceneInstanceRecord)),
            static_cast<uint32_t>(storageRange /
                sizeof(GpuScenePrimitiveRecord)),
            static_cast<uint32_t>(storageRange /
                sizeof(GpuSceneGeometryRecord)),
        };
        if (gpuSceneMaximumCapacity_.transforms == 0 ||
            gpuSceneMaximumCapacity_.instances == 0 ||
            gpuSceneMaximumCapacity_.primitives == 0 ||
            gpuSceneMaximumCapacity_.geometries == 0) {
            throw std::runtime_error(
                "Vulkan storage-buffer range cannot hold GPU-scene records");
        }
        createGpuSceneBuffers({ 2u, 1u, 1u, 1u });
        opaqueCuller_.resize(512u, frameOpen_);
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
            gpuSceneDescriptorSets_[i] = descriptorAllocator.allocate(
                meshLayouts.getGpuSceneSetLayout());
            opaqueCuller_.allocateSet(static_cast<uint32_t>(i));

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
        bindGpuSceneBuffers();
        opaqueCuller_.bindBuffers();
        transparencyPyramid_.rebuild(frameTargets);
        if (depthPyramidEnabled_) {
            depthPyramid_.rebuild(frameTargets);
            bindDepthPyramidHistory(true);
        }
        layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
        layeredLocalComposition_.rebuildDescriptors(frameTargets);
        layeredSceneResolve_.rebuildDescriptors(frameTargets);
        weightedOit_.rebuildDescriptors(frameTargets);

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
        io.ConfigWindowsMoveFromTitleBarOnly = true;
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
            const VkImageView editorDepthView = targets.depth.view;
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(),
                editorDepthView,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }

        initialized_ = true;
        cleaned_ = false;
        const VulkanBackendServices services = backendServices();
        for (IVulkanBackendExtension* extension : extensions_)
            extension->onBackendInitialized(services);
    }

    void VulkanVertexBackend::attachExtension(IRenderBackendExtension* extension) {
        if (extension == nullptr)
            throw std::invalid_argument("Render backend extension is null");
        if (initialized_)
            throw std::logic_error(
                "Render backend extensions attach before init()");
        if (extension->api() != RenderBackendApi::Vulkan)
            throw std::invalid_argument(
                "Render backend extension targets another graphics API");
        auto* vulkanExtension = dynamic_cast<IVulkanBackendExtension*>(extension);
        if (vulkanExtension == nullptr)
            throw std::invalid_argument(
                "Vulkan backend extension does not implement IVulkanBackendExtension");
        extensions_.push_back(vulkanExtension);
        // The first extension providing each service serves it.
        if (indirectOracle_ == nullptr)
            indirectOracle_ = vulkanExtension->indirectOracle();
        if (indirectStreamObserver_ == nullptr)
            indirectStreamObserver_ = vulkanExtension->indirectStreamObserver();
    }

    VulkanBackendServices VulkanVertexBackend::backendServices() noexcept {
        return {
            .device = vkContext->getDevice(),
            .allocator = &resourceAllocator,
            .scheduler = &scheduler,
            .graph = &renderGraph_,
            .frameTargets = &frameTargets,
            .probeCaptureTargets = &reflectionProbeCaptureTargets_,
            .depthPyramid = depthPyramidEnabled_ ? &depthPyramid_ : nullptr,
            .profiler = cpuProfiler_,
        };
    }

    bool VulkanVertexBackend::anyExtensionWants(
        const VulkanHookContext& context) const {
        return std::ranges::any_of(extensions_,
            [&](const IVulkanBackendExtension* extension) {
                return extension->wantsHook(context);
            });
    }

    void VulkanVertexBackend::runPassHook(const VulkanHookContext& context,
        bool declared, RenderGraph::PassId pass, const char* gpuRangeName) {
        if (!declared) return;
        if (!anyExtensionWants(context)) {
            renderGraph_.skipPass(pass);
            return;
        }
        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(gpuRangeName);
        renderGraph_.beginPass(currentCmd, pass);
        for (IVulkanBackendExtension* extension : extensions_)
            if (extension->wantsHook(context)) extension->onHook(context);
        scheduler.endGpuRange(gpuRange);
    }

    void VulkanVertexBackend::notifyHook(const VulkanHookContext& context) {
        for (IVulkanBackendExtension* extension : extensions_)
            if (extension->wantsHook(context)) extension->onHook(context);
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
        // Bind each frame's descriptor set only after its fence completes. View
        // switches must not idle both frames merely to choose a resident HDRI.
        environmentLighting_ = environment;
        for (VulkanTexturePayload* payload : {
                textureVault.get(environment.radiance),
                textureVault.get(environment.irradiance),
                textureVault.get(environment.prefilteredSpecular),
                textureVault.get(environment.brdfLut) })
            resourceAllocator.reclassify(payload->image,
                ProfileMemoryCategory::Environment);
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

    void VulkanVertexBackend::bindEnvironmentProducts(uint32_t frame) {
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
        }, frame);
        if (frame == UINT32_MAX) frameEnvironments_.fill(environmentLighting_);
        else frameEnvironments_[frame] = environmentLighting_;
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
        for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            collectVirtualShadowRequests(frame);
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
        for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            opaqueCuller_.collect(frame);
        for (VulkanIndirectViewCuller* culler : indirectViewCullers())
            for (uint32_t frame = 0;
                    frame < VulkanFrameScheduler::FramesInFlight; ++frame)
                culler->collect(frame);
        for (IVulkanBackendExtension* extension : extensions_)
            extension->onBeforeDeviceDestroy();

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
        destroyRetainedViews();
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
        depthPyramid_.clearDescriptors();
        layeredInterfaceCapture_.clearDescriptors();
        layeredLocalComposition_.clearDescriptors();
        layeredSceneResolve_.clearDescriptors();
        weightedOit_.clearDescriptors();
        frameTargets.cleanup();
        renderGraph_.cleanupAfterDeviceIdle();
        directionalCuller_.destroy(device);
        spotCuller_.destroy(device);
        pointCuller_.destroy(device);
        probeCuller_.destroy(device);

        if (indirectCullerSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device,
                indirectCullerSetLayout_, nullptr);
            indirectCullerSetLayout_ = VK_NULL_HANDLE;
        }
        directionalShadow_.cleanup();
        spotShadow_.cleanup();
        pointShadow_.cleanup();
        virtualShadowResources_.cleanup();
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
            if (payload.arenaAllocation) {
                if (!payload.ownsArenaBuffers) return;
                resourceAllocator.destroy(payload.vertexBuffer);
                resourceAllocator.destroy(payload.arenaUInt16IndexBuffer);
                resourceAllocator.destroy(payload.arenaUInt32IndexBuffer);
            }
            else {
                resourceAllocator.destroy(payload.vertexBuffer);
                resourceAllocator.destroy(payload.indexBuffer);
            }
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
        for (VulkanBufferResource& buffer : weightedOitInstanceBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : lightRecordBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : gpuSceneTransformBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : gpuSceneInstanceBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : gpuScenePrimitiveBuffers_)
            resourceAllocator.destroy(buffer);
        for (VulkanBufferResource& buffer : gpuSceneGeometryBuffers_)
            resourceAllocator.destroy(buffer);
        opaqueCuller_.destroy(device);
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
        depthPyramid_.cleanup();
        layeredSceneResolve_.cleanup();
        layeredLocalComposition_.cleanup();
        layeredInterfaceCapture_.cleanup();
        weightedOit_.cleanup();

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
        hero4AtlasResidency_.restore(false);
        cinematic8AtlasResidency_.restore(false);
        weightedOitResidency_.restore(false);
        ordinary2AtlasExtent_ = {};
        hero4AtlasExtent_ = {};
        cinematic8AtlasExtent_ = {};
        frameTopologyPrewarm_ = {};
        cpuProfiler_ = nullptr;
        collectFrameCounters_ = false;
        uniqueMaterialIds_.clear();
        uniquePipelineIds_.clear();
        manualExposureEv_ = 0.0f;
        outputOperator_ = OutputTransformOperator::Aces2;
        requestedOutputTransport_ = Color::OutputTransport::SdrSrgb;
        outputTransport_ = Color::OutputTransport::SdrSrgb;
        outputTargetFormat_ = VulkanSdrOutputFormat;
        canonicalMaterialCapacity_ = 0;
        weightedOitInstanceCapacity_ = 0;
        canonicalMaterialMaximumCapacity_ = 0;
        gpuSceneCapacity_ = {};
        gpuSceneMaximumCapacity_ = {};
        gpuScenePublishedCounts_ = {};
        gpuSceneCpuMirrors_ = {};
        gpuSceneUploadTelemetry_ = {};
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
            frameCounters_.drawWeightedOitAccumulation +
            frameCounters_.drawWeightedOitResolve +
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
        cpuProfiler_->recordCounter("opaque.indirect.command_count",
            frameCounters_.opaqueIndirectCommands);
        cpuProfiler_->recordCounter("opaque.indirect.bin_count",
            frameCounters_.opaqueIndirectBins);
        cpuProfiler_->recordCounter("opaque.indirect.direct_fallback_packets",
            frameCounters_.opaqueIndirectFallbackPackets);
        cpuProfiler_->recordCounter("opaque.indirect.fallback_reason",
            frameCounters_.opaqueIndirectFallbackReason);
        cpuProfiler_->recordCounter("depth.occlusion.history_eligible",
            frameCounters_.depthHistoryEligible);
        cpuProfiler_->recordCounter("depth.occlusion.history_rejection",
            frameCounters_.depthHistoryRejection);
        cpuProfiler_->recordCounter("draw.recorded.selection", frameCounters_.drawSelection);
        cpuProfiler_->recordCounter("draw.recorded.shadow.directional",
            frameCounters_.drawShadowDirectional);
        cpuProfiler_->recordCounter("draw.recorded.shadow.directional.alpha_mask",
            frameCounters_.drawShadowDirectionalAlphaMask);
        cpuProfiler_->recordCounter("shadow.directional.casters.tested",
            frameCounters_.shadowDirectionalCastersTested);
        cpuProfiler_->recordCounter("shadow.directional.casters.culled",
            frameCounters_.shadowDirectionalCastersCulled);
        cpuProfiler_->recordCounter("shadow.directional.indirect.commands",
            frameCounters_.shadowDirectionalIndirectCommands);
        cpuProfiler_->recordCounter("shadow.directional.indirect.bins",
            frameCounters_.shadowDirectionalIndirectBins);
        cpuProfiler_->recordCounter(
            "shadow.directional.indirect.direct_fallback",
            frameCounters_.shadowDirectionalDirectFallback);
        cpuProfiler_->recordCounter(
            "shadow.directional.indirect.fallback_reason",
            frameCounters_.shadowDirectionalIndirectFallbackReason);
        cpuProfiler_->recordCounter(
            "shadow.directional.indirect.membership_cache_hit",
            frameCounters_.shadowDirectionalMembershipCacheHit);
        cpuProfiler_->recordCounter("draw.recorded.shadow.spot",
            frameCounters_.drawShadowSpot);
        cpuProfiler_->recordCounter("draw.recorded.shadow.spot.alpha_mask",
            frameCounters_.drawShadowSpotAlphaMask);
        cpuProfiler_->recordCounter("shadow.spot.casters.tested",
            frameCounters_.shadowSpotCastersTested);
        cpuProfiler_->recordCounter("shadow.spot.casters.culled",
            frameCounters_.shadowSpotCastersCulled);
        cpuProfiler_->recordCounter("shadow.spot.indirect.commands",
            frameCounters_.shadowSpotIndirectCommands);
        cpuProfiler_->recordCounter("shadow.spot.indirect.bins",
            frameCounters_.shadowSpotIndirectBins);
        cpuProfiler_->recordCounter("shadow.spot.indirect.direct_fallback",
            frameCounters_.shadowSpotDirectFallback);
        cpuProfiler_->recordCounter("shadow.spot.indirect.fallback_reason",
            frameCounters_.shadowSpotIndirectFallbackReason);
        cpuProfiler_->recordCounter("shadow.spot.indirect.membership_cache_hit",
            frameCounters_.shadowSpotMembershipCacheHit);
        cpuProfiler_->recordCounter("draw.recorded.shadow.point",
            frameCounters_.drawShadowPoint);
        cpuProfiler_->recordCounter("draw.recorded.shadow.point.alpha_mask",
            frameCounters_.drawShadowPointAlphaMask);
        cpuProfiler_->recordCounter("shadow.point.casters.tested",
            frameCounters_.shadowPointCastersTested);
        cpuProfiler_->recordCounter("shadow.point.casters.culled",
            frameCounters_.shadowPointCastersCulled);
        cpuProfiler_->recordCounter("shadow.point.indirect.commands",
            frameCounters_.shadowPointIndirectCommands);
        cpuProfiler_->recordCounter("shadow.point.indirect.bins",
            frameCounters_.shadowPointIndirectBins);
        cpuProfiler_->recordCounter("shadow.point.indirect.direct_fallback",
            frameCounters_.shadowPointDirectFallback);
        cpuProfiler_->recordCounter("shadow.point.indirect.fallback_reason",
            frameCounters_.shadowPointIndirectFallbackReason);
        cpuProfiler_->recordCounter("shadow.point.indirect.membership_cache_hit",
            frameCounters_.shadowPointMembershipCacheHit);
        cpuProfiler_->recordCounter("draw.recorded.lighting", frameCounters_.drawLighting);
        cpuProfiler_->recordCounter("draw.recorded.output", frameCounters_.drawOutput);
        cpuProfiler_->recordCounter("draw.recorded.transparent.depth",
            frameCounters_.drawTransparentDepth);
        cpuProfiler_->recordCounter("draw.recorded.transparent.forward",
            frameCounters_.drawTransparentForward);
        cpuProfiler_->recordCounter(
            "draw.recorded.transparent.oit.accumulation",
            frameCounters_.drawWeightedOitAccumulation);
        cpuProfiler_->recordCounter("draw.recorded.transparent.oit.resolve",
            frameCounters_.drawWeightedOitResolve);
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
        cpuProfiler_->recordCounter("transparent.oit.packets",
            frameCounters_.weightedOitPackets);
        cpuProfiler_->recordCounter("transparent.oit.sorted_fallback_packets",
            frameCounters_.weightedOitSortedFallbackPackets);
        cpuProfiler_->recordCounter(
            "transparent.oit.instance_capacity_fallback_packets",
            frameCounters_.weightedOitInstanceCapacityFallbackPackets);
        cpuProfiler_->recordCounter("transparent.oit.instances",
            frameCounters_.weightedOitInstances);
        cpuProfiler_->recordCounter("transparent.oit.instance_upload_bytes",
            frameCounters_.weightedOitInstanceUploadBytes);
        cpuProfiler_->recordCounter("transparent.oit.resident",
            static_cast<uint64_t>(weightedOitResidency_.enabled()));
        cpuProfiler_->recordCounter("transparent.oit.order_seed",
            weightedOitOrderSeed_);
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

    VulkanProductionGraphFeatures
        VulkanVertexBackend::productionGraphFeatures() const noexcept {
        return {
            .depthPyramid = depthPyramidEnabled_,
            .virtualShadowWorkingSetBytes = virtualShadowResources_.initialized()
                ? virtualShadowResources_.info().workingSetLayout.totalBytes : 0,
            // The CPU profiler's enabled state is fixed for the process.
            .clusterTelemetryReadback =
                cpuProfiler_ != nullptr && cpuProfiler_->isEnabled(),
            .hooks = graphHooks_,
            .pointShadowPoolCapacities = pointShadowCapacities_,
        };
    }

    VulkanImageResource VulkanVertexBackend::swapchainGraphImage(
        uint32_t imageIndex) const {
        VulkanImageResource image{};
        image.image = vkSwapchain->getImages().at(imageIndex);
        image.view = vkSwapchain->getImageViews().at(imageIndex);
        image.extent = vkSwapchain->getExtent();
        image.format = vkSwapchain->getImageFormat();
        image.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        return image;
    }

    void VulkanVertexBackend::bindGraphImportedImages() {
        // R3b.6. Render passes own these layouts until dynamic rendering
        // (R4a); the executor asserts them. The swapchain is rebound per frame
        // after acquire (every slot starts on image 0 so validateFrame holds
        // before a slot's first acquire); its render passes go UNDEFINED ->
        // PRESENT. Shadow maps persist across slots and their render passes go
        // READ_ONLY -> READ_ONLY.
        using RenderGraph::Access;
        for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            renderGraph_.bindExternalImage(frame, graphIds_.swapchain,
                swapchainGraphImage(0), Access::Undefined,
                ExternalSyncPolicy::renderPassManaged(Access::Undefined,
                    Access::Present));
        const ExternalSyncPolicy shadowPolicy =
            ExternalSyncPolicy::renderPassManaged(Access::SampledRead,
                Access::SampledRead);
        renderGraph_.bindExternalImage(VulkanGlobalBinding,
            graphIds_.shadowDirectionalMap, directionalShadow_.image(),
            Access::SampledRead, shadowPolicy);
        renderGraph_.bindExternalImage(VulkanGlobalBinding,
            graphIds_.shadowSpotMap, spotShadow_.image(), Access::SampledRead,
            shadowPolicy);
        for (uint32_t tier = 0; tier < graphIds_.shadowPointMaps.size(); ++tier)
            renderGraph_.bindExternalImage(VulkanGlobalBinding,
                graphIds_.shadowPointMaps[tier], pointShadow_.image(tier),
                Access::SampledRead, shadowPolicy);
    }

    void VulkanVertexBackend::bindDepthPyramidHistory(bool reset) {
        // R3b.9: one executor-owned global import, bound to the retained
        // view's history image. Switching views keeps each image's tracked
        // state; a rebuild (fresh images) starts both from Undefined.
        if (!depthPyramidEnabled_ || !graphIds_.depthPyramidHistory.isValid()) return;
        if (reset) {
            depthHistoryAccess_.fill(RenderGraph::Access::Undefined);
            depthHistoryBoundView_ = UINT32_MAX;
        }
        if (depthHistoryBoundView_ == retainedRenderView_) return;
        if (depthHistoryBoundView_ < depthHistoryAccess_.size())
            depthHistoryAccess_[depthHistoryBoundView_] = renderGraph_.externalImageAccess(
                VulkanGlobalBinding, graphIds_.depthPyramidHistory);
        renderGraph_.bindExternalImage(VulkanGlobalBinding,
            graphIds_.depthPyramidHistory,
            depthPyramid_.historyImage(retainedRenderView_),
            depthHistoryAccess_[retainedRenderView_],
            ExternalSyncPolicy::executorOwned());
        depthHistoryBoundView_ = retainedRenderView_;
    }

    void VulkanVertexBackend::bindGraphImportedBuffers() {
        // R3b.7: per-slot indirect command/count buffers of the four drawing
        // cullers and the reflection-probe cluster buffers. Their owners
        // replace them on capacity growth, so every slot is unbound before
        // any is rebound (a recycled handle must not meet a destroyed one).
        if (renderGraph_.compiledGraph() == nullptr) return;
        if (frameOpen_)
            throw std::logic_error(
                "Graph imported buffers rebind only at a frame boundary");
        scheduler.waitForAllFrames();
        for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            renderGraph_.onFrameFenceCompleted(frame);
        struct Binding {
            RenderGraph::GraphResourceId id;
            const std::array<VulkanBufferResource,
                VulkanFrameScheduler::FramesInFlight>* buffers;
        };
        const std::array<Binding, 10> bindings{ {
            { graphIds_.directionalIndirect.commands, &directionalCuller_.buffers().commands },
            { graphIds_.directionalIndirect.counts, &directionalCuller_.buffers().counts },
            { graphIds_.spotIndirect.commands, &spotCuller_.buffers().commands },
            { graphIds_.spotIndirect.counts, &spotCuller_.buffers().counts },
            { graphIds_.pointIndirect.commands, &pointCuller_.buffers().commands },
            { graphIds_.pointIndirect.counts, &pointCuller_.buffers().counts },
            { graphIds_.opaqueIndirect.commands, &opaqueCuller_.buffers().commands },
            { graphIds_.opaqueIndirect.counts, &opaqueCuller_.buffers().counts },
            { graphIds_.probeClusterHeaders, &reflectionProbeClusterHeaderBuffers_ },
            { graphIds_.probeClusterIndices, &reflectionProbeClusterIndexBuffers_ },
        } };
        for (const Binding& binding : bindings) {
            if (!binding.id.isValid()) continue;
            for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
                renderGraph_.unbindExternalBuffer(frame, binding.id);
        }
        for (const Binding& binding : bindings) {
            if (!binding.id.isValid()) continue;
            for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                const VulkanBufferResource& buffer = (*binding.buffers)[frame];
                if (buffer.buffer == VK_NULL_HANDLE) continue;
                renderGraph_.bindExternalBuffer(frame, binding.id, buffer.buffer,
                    buffer.size);
            }
        }
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
                hero4AtlasExtent_, cinematic8AtlasExtent_,
                weightedOitResidency_.enabled() }, productionGraphFeatures()));
        // R3b.4: ids are resolved once per plan; the barrier API follows the
        // device feature explicitly; callback passes record GPU ranges through
        // the scheduler.
        graphIds_ = resolveVulkanProductionGraphIds(renderGraph_);
        renderGraph_.setBarrierApi(vkContext->hasSynchronization2()
            ? VulkanBarrierApi::Synchronization2
            : VulkanBarrierApi::Synchronization1);
        renderGraph_.setGpuRangeSink(VulkanGpuRangeSink::forScheduler(scheduler));
        bindGraphImportedImages();
        bindGraphImportedBuffers();
        // The depth-pyramid history is bound after its images are rebuilt.
        depthHistoryBoundView_ = UINT32_MAX;
        if (virtualShadowResources_.initialized()) {
            for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                const auto& buffer = virtualShadowResources_.workingSet(frame);
                renderGraph_.bindExternalBuffer(frame, graphIds_.virtualShadowWorkingSet,
                    buffer.buffer, buffer.size);
            }
            virtualShadowDepthBindings_.fill(VK_NULL_HANDLE);
        }
    }

    void VulkanVertexBackend::setWeightedOitInstanceCapacity(uint32_t capacity) {
        if (capacity == weightedOitInstanceCapacity_) return;
        if (capacity > kWeightedOitMaximumInstanceCount) {
            throw std::out_of_range(
                "WeightedOIT instance capacity exceeds the production bound");
        }

        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> replacement{};
        if (capacity != 0u) {
            try {
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(
                    weightedOitInstanceStreamBytes(capacity));
                for (VulkanBufferResource& buffer : replacement) {
                    buffer = resourceAllocator.createBuffer(bytes,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::Uniform);
                }
            }
            catch (...) {
                for (VulkanBufferResource& buffer : replacement)
                    resourceAllocator.destroy(buffer);
                throw;
            }
        }

        for (VulkanBufferResource& buffer : weightedOitInstanceBuffers_)
            resourceAllocator.destroy(buffer);
        weightedOitInstanceBuffers_ = replacement;
        weightedOitInstanceCapacity_ = capacity;
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
            !weightedOitResidency_.changePending() &&
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
        const bool previousWeightedOitEnabled =
            weightedOitResidency_.enabled();
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
            depthPyramid_.clearDescriptors();
            layeredInterfaceCapture_.clearDescriptors();
            layeredLocalComposition_.clearDescriptors();
            layeredSceneResolve_.clearDescriptors();
            weightedOit_.clearDescriptors();
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
            if (depthPyramidEnabled_) {
                depthPyramid_.rebuild(frameTargets);
                bindDepthPyramidHistory(true);
            }
            layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
            layeredLocalComposition_.rebuildDescriptors(frameTargets);
            layeredSceneResolve_.rebuildDescriptors(frameTargets);
            weightedOit_.rebuildDescriptors(frameTargets);
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
                const VkImageView editorDepthView = targets.depth.view;
                uiDepthTextures[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), editorDepthView,
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
            weightedOitResidency_.publishRequested();
            setWeightedOitInstanceCapacity(weightedOitResidency_.enabled()
                ? kWeightedOitMaximumInstanceCount : 0u);
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
                weightedOitResidency_.restore(previousWeightedOitEnabled);
                setWeightedOitInstanceCapacity(previousWeightedOitEnabled
                    ? kWeightedOitMaximumInstanceCount : 0u);
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
        setOutputTransport(window, requestedOutputTransport_);
    }

    void VulkanVertexBackend::setOutputTransport(GLFWwindow* window,
        Color::OutputTransport requestedTransport) {
        if (!initialized_ || !vkContext || !vkSwapchain) {
            throw std::logic_error(
                "Output transport can only change on an initialized backend.");
        }
        if (frameOpen_) {
            throw std::logic_error(
                "Output transport can only change between frames.");
        }
        // 1. Handle Minimization (Pause the engine until it's un-minimized)
        opaqueCuller_.lodHistory().resetView();
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        const uint32_t oldImageCount = vkSwapchain->getImageCount();
        auto candidate = std::make_unique<VkSwapchain>(vkContext.get(), window,
			requestedTransport, vkSwapchain->getSwapchain());
        const Color::OutputTransport candidateTransport =
            candidate->getOutputTransportSelection().effective;
        const VkFormat candidateOutputFormat = candidateTransport ==
            Color::OutputTransport::SdrSrgb
            ? VulkanSdrOutputFormat : VK_FORMAT_R16G16B16A16_SFLOAT;

        // Validate topology before retiring the active renderer resources.
        (void)buildVulkanProductionRenderGraph(sceneExtent_,
            candidate->getExtent(), candidate->getImageFormat(),
            candidateOutputFormat, candidateTransport ==
                Color::OutputTransport::Hdr10Pq, gBufferLayout_,
            clusterConfig_, directionalShadowResolution_,
            spotShadowAtlasResolution_,
            transparencyPyramidResidency_.enabled(),
            VulkanLayeredGraphConfig{ ordinary2AtlasExtent_,
                hero4AtlasExtent_, cinematic8AtlasExtent_,
                weightedOitResidency_.enabled() }, productionGraphFeatures());

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
        depthPyramid_.clearDescriptors();
        layeredInterfaceCapture_.clearDescriptors();
        layeredLocalComposition_.clearDescriptors();
        layeredSceneResolve_.clearDescriptors();
        weightedOit_.clearDescriptors();
        frameTargets.cleanup();
		renderGraph_.cleanupAfterDeviceIdle();
		hdrEncodePass.cleanup();
		outputPass.cleanup();
		uiPass.reset();

        vkSwapchain = std::move(candidate);
		requestedOutputTransport_ = requestedTransport;
		outputTransport_ = candidateTransport;
		outputTargetFormat_ = candidateOutputFormat;
        vkSwapchain->setHdrMetadata(peakNits_);
		outputPass.init(*vkContext, descriptorAllocator, outputTargetFormat_);
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            hdrEncodePass.init(*vkContext, descriptorAllocator,
                vkSwapchain->getImageFormat());
        }
        const bool hdr10Composition = outputTransport_ ==
            Color::OutputTransport::Hdr10Pq;
        uiPass = std::make_unique<VkUIRenderPass>(vkContext.get(),
            hdr10Composition ? VK_FORMAT_R16G16B16A16_SFLOAT
                : vkSwapchain->getImageFormat(), !hdr10Composition);
        if (imguiInitialized_) {
            ImGui_ImplVulkan_PipelineInfo pipelineInfo{};
            pipelineInfo.RenderPass = uiPass->getRenderPass();
            pipelineInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            ImGui_ImplVulkan_CreateMainPipeline(&pipelineInfo);
            ImGui_ImplVulkan_SetDisplayColorConfiguration(
                outputTransport_ == Color::OutputTransport::ScRgb
                    ? paperWhiteNits_ / 80.0f : 1.0f,
                outputTransport_ == Color::OutputTransport::Hdr10Pq ? 1u : 0u);
        }
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
        if (depthPyramidEnabled_) {
            depthPyramid_.rebuild(frameTargets);
            bindDepthPyramidHistory(true);
        }
        layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
        layeredLocalComposition_.rebuildDescriptors(frameTargets);
        layeredSceneResolve_.rebuildDescriptors(frameTargets);
        weightedOit_.rebuildDescriptors(frameTargets);
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
            const VkImageView editorDepthView = targets.depth.view;
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(),
                editorDepthView,
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
            if (extent.width == 0 || extent.height == 0) opaqueCuller_.lodHistory().resetView();
            diagnostic = "Requested scene extent is outside Vulkan image limits";
            return false;
        }
        const VkExtent2D requested{ extent.width, extent.height };
        if (requested.width == sceneExtent_.width &&
            requested.height == sceneExtent_.height) {
            return true;
        }

        opaqueCuller_.lodHistory().resetView();

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
                    requestedCinematic8AtlasExtent,
                    weightedOitResidency_.enabled() }, productionGraphFeatures());
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
            depthPyramid_.clearDescriptors();
            layeredInterfaceCapture_.clearDescriptors();
            layeredLocalComposition_.clearDescriptors();
            layeredSceneResolve_.clearDescriptors();
            weightedOit_.clearDescriptors();
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
            if (depthPyramidEnabled_) {
                depthPyramid_.rebuild(frameTargets);
                bindDepthPyramidHistory(true);
            }
            layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
            layeredLocalComposition_.rebuildDescriptors(frameTargets);
            layeredSceneResolve_.rebuildDescriptors(frameTargets);
            weightedOit_.rebuildDescriptors(frameTargets);
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
                const VkImageView editorDepthView = targets.depth.view;
                uiDepthTextures[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), editorDepthView,
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
            .multiDrawIndirect = vkContext->hasMultiDrawIndirect(),
            .drawIndirectFirstInstance =
                vkContext->hasDrawIndirectFirstInstance(),
            .drawIndirectCount = vkContext->hasDrawIndirectCount(),
            .maxIndexedTextureViews = vkContext->getMaxIndexedTextureViews(),
            .maxIndexedSamplers = vkContext->getMaxIndexedSamplers(),
            .maxUpdateAfterBindDescriptors =
                vkContext->getMaxUpdateAfterBindDescriptors(),
            .maxGpuLightRecords = lightRecordMaximumCapacity_,
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
        info.gpuSceneTransformCapacity = gpuSceneCapacity_.transforms;
        info.gpuSceneInstanceCapacity = gpuSceneCapacity_.instances;
        info.gpuScenePrimitiveCapacity = gpuSceneCapacity_.primitives;
        info.gpuSceneGeometryCapacity = gpuSceneCapacity_.geometries;
        info.gpuSceneUploadBytes = gpuSceneUploadTelemetry_.bytes;
        info.gpuSceneUploadRanges = gpuSceneUploadTelemetry_.ranges;
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
        info.weightedOitResident = weightedOitResidency_.enabled();
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
                requirements.cinematic8LayeredInterfaces ||
                requirements.weightedOit,
        };
        const bool previousPyramids =
            transparencyPyramidResidency_.enabled();
        const VkExtent2D previousOrdinary2AtlasExtent =
            ordinary2AtlasExtent_;
        const VkExtent2D previousHero4AtlasExtent = hero4AtlasExtent_;
        const VkExtent2D previousCinematic8AtlasExtent =
            cinematic8AtlasExtent_;
        const bool previousWeightedOit = weightedOitResidency_.enabled();
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
        if (!requirePyramids && !requirements.weightedOit) {
            frameTopologyPrewarm_ = result;
            return result;
        }

        if (requirePyramids) {
            transparencyPyramidResidency_.observe(true);
            ordinary2AtlasResidency_.observe(
                requirements.ordinary2LayeredInterfaces);
            hero4AtlasResidency_.observe(
                requirements.hero4LayeredInterfaces);
            cinematic8AtlasResidency_.observe(
                requirements.cinematic8LayeredInterfaces);
        }
        weightedOitResidency_.observe(requirements.weightedOit);
        if (!transparencyPyramidResidency_.changePending() &&
            !ordinary2AtlasResidency_.changePending() &&
            !hero4AtlasResidency_.changePending() &&
            !cinematic8AtlasResidency_.changePending() &&
            !weightedOitResidency_.changePending() &&
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
                cinematic8AtlasExtent_.height ||
            previousWeightedOit != weightedOitResidency_.enabled();
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
            if (payload->arenaAllocation) {
                throw std::logic_error(
                    "Geometry-arena primitive handles must be retired together");
            }
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

    GeometryArenaAllocation VulkanVertexBackend::allocateGeometryArena(
        uint32_t vertexStride,
        std::span<const std::byte> vertexBytes,
        const GeometryArenaData& arena) {
        if (arena.abiVersion != GeometryArenaAbiVersion || vertexStride == 0 ||
            arena.primitives.empty() ||
            arena.vertexCount != vertexBytes.size_bytes() / vertexStride ||
            arena.vertexCount * vertexStride != vertexBytes.size_bytes()) {
            throw std::invalid_argument(
                "Geometry arena has an unsupported ABI or vertex layout");
        }
        for (const GeometryArenaPrimitiveRange& range : arena.primitives) {
            const uint64_t streamSize = range.indexStream ==
                    GeometryArenaIndexStream::UInt16
                ? arena.uint16Indices.size() : arena.uint32Indices.size();
            if (range.indexCount == 0 || range.firstIndex > streamSize ||
                range.indexCount > streamSize - range.firstIndex ||
                range.vertexOffset < 0 ||
                static_cast<uint64_t>(range.vertexOffset) >= arena.vertexCount) {
                throw std::invalid_argument(
                    "Geometry arena primitive range is out of bounds");
            }
        }

        VulkanBufferResource vertexBuffer;
        VulkanBufferResource uint16Buffer;
        VulkanBufferResource uint32Buffer;
        GeometryArenaAllocation allocation;
        allocation.primitiveGeometry.reserve(arena.primitives.size());
        try {
            vertexBuffer = resourceAllocator.createBuffer(vertexBytes.size_bytes(),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::GeometryVertex);
            if (!arena.uint16Indices.empty()) {
                uint16Buffer = resourceAllocator.createBuffer(
                    arena.uint16Indices.size() * sizeof(uint16_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                    ProfileMemoryCategory::GeometryIndex);
            }
            if (!arena.uint32Indices.empty()) {
                uint32Buffer = resourceAllocator.createBuffer(
                    arena.uint32Indices.size() * sizeof(uint32_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                    ProfileMemoryCategory::GeometryIndex);
            }

            for (size_t index = 0; index < arena.primitives.size(); ++index) {
                const GeometryArenaPrimitiveRange& range =
                    arena.primitives[index];
                VulkanGeometryPayload payload{
                    .vertexBuffer = vertexBuffer,
                    .indexBuffer = range.indexStream ==
                            GeometryArenaIndexStream::UInt16
                        ? uint16Buffer : uint32Buffer,
                    .arenaUInt16IndexBuffer = uint16Buffer,
                    .arenaUInt32IndexBuffer = uint32Buffer,
                    .vertexOffset = static_cast<VkDeviceSize>(
                        range.vertexOffset) * vertexStride,
                    .indexCount = range.indexCount,
                    .indexFormat = range.indexStream ==
                            GeometryArenaIndexStream::UInt16
                        ? IndexFormat::UInt16 : IndexFormat::UInt32,
                    .arenaAllocation = true,
                    .ownsArenaBuffers = index == 0,
                };
                allocation.primitiveGeometry.push_back(
                    geometryVault.allocate(std::move(payload)));
            }

            uploadContext.enqueueBufferUpload(vertexBuffer, vertexBytes,
                ResourceState::VertexBuffer);
            if (!arena.uint16Indices.empty()) {
                uploadContext.enqueueBufferUpload(uint16Buffer,
                    std::as_bytes(std::span(arena.uint16Indices)),
                    ResourceState::IndexBuffer);
            }
            if (!arena.uint32Indices.empty()) {
                uploadContext.enqueueBufferUpload(uint32Buffer,
                    std::as_bytes(std::span(arena.uint32Indices)),
                    ResourceState::IndexBuffer);
            }
        }
        catch (...) {
            for (GeometryHandle handle : allocation.primitiveGeometry)
                geometryVault.free(handle);
            resourceAllocator.destroy(uint32Buffer);
            resourceAllocator.destroy(uint16Buffer);
            resourceAllocator.destroy(vertexBuffer);
            throw;
        }
        return allocation;
    }

    void VulkanVertexBackend::freeGeometryArena(
        std::span<const GeometryHandle> primitiveGeometry) {
        if (primitiveGeometry.empty()) return;
        VulkanGeometryPayload* first = geometryVault.get(
            primitiveGeometry.front());
        if (!first || !first->arenaAllocation) {
            throw std::invalid_argument(
                "Geometry arena retirement requires live arena handles");
        }
        for (GeometryHandle handle : primitiveGeometry) {
            VulkanGeometryPayload* payload = geometryVault.get(handle);
            if (!payload || !payload->arenaAllocation ||
                payload->vertexBuffer.buffer != first->vertexBuffer.buffer ||
                payload->arenaUInt16IndexBuffer.buffer !=
                    first->arenaUInt16IndexBuffer.buffer ||
                payload->arenaUInt32IndexBuffer.buffer !=
                    first->arenaUInt32IndexBuffer.buffer) {
                throw std::invalid_argument(
                    "Geometry arena retirement cannot mix allocations");
            }
        }
        VulkanBufferResource vertex = first->vertexBuffer;
        VulkanBufferResource uint16 = first->arenaUInt16IndexBuffer;
        VulkanBufferResource uint32 = first->arenaUInt32IndexBuffer;
        scheduler.defer([this, vertex, uint16, uint32]() mutable {
            resourceAllocator.destroy(vertex);
            resourceAllocator.destroy(uint16);
            resourceAllocator.destroy(uint32);
        });
        for (GeometryHandle handle : primitiveGeometry)
            geometryVault.free(handle);
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
                layeredInterfaceCapture_.renderPass(),
                layeredLocalComposition_.renderPass(),
                weightedOit_.accumulationRenderPass(),
                weightedOit_.resolveRenderPass(), outputPass.renderPass(),
                uiPass->getRenderPass() },
            VulkanFrameScheduler::FramesInFlight,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            transparencyPyramidResidency_.enabled(),
            VulkanLayeredGraphConfig{ ordinary2AtlasExtent_,
                hero4AtlasExtent_, cinematic8AtlasExtent_,
                weightedOitResidency_.enabled() },
            renderGraph_, graphIds_);
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
        depthHistoryPrepared_ = false;
        currentDepthHistoryDecision_ = {};
        finalCaptureHookRecorded_ = false;
        probeCaptureHandled_ = false;
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
        const uint32_t frameSlot = scheduler.currentFrameIndex();
        // beginFrame has waited this slot's fence before returning, including
        // the out-of-date acquire path. Its capture readbacks are now CPU-safe.
        for (IVulkanBackendExtension* extension : extensions_)
            extension->onFrameSlotRetired(completedFrameIndex);
        collectVirtualShadowRequests(completedFrameIndex);
        if (depthPyramidEnabled_) {
            depthPyramid_.onFrameFenceCompleted(completedFrameIndex,
                scheduler.completedSerial());
        }
        collectClusterDiagnostics(completedFrameIndex);
        opaqueCuller_.collect(completedFrameIndex);
        collectIndirectViewValidations(completedFrameIndex);
        {
            CpuScope graphScope(cpuProfiler_, "cpu.render_graph.lookup");
            renderGraph_.onFrameFenceCompleted(completedFrameIndex);
            // The acquired swapchain image for this slot (R3b.6).
            if (frame.status != FrameStatus::RecreateSwapchain) {
                renderGraph_.bindExternalImage(frameSlot, graphIds_.swapchain,
                    swapchainGraphImage(frame.imageIndex),
                    RenderGraph::Access::Undefined,
                    ExternalSyncPolicy::renderPassManaged(
                        RenderGraph::Access::Undefined,
                        RenderGraph::Access::Present));
            }
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
        if (frameEnvironments_[scheduler.currentFrameIndex()] != environmentLighting_)
            bindEnvironmentProducts(scheduler.currentFrameIndex());
        bindDepthPyramidHistory(false);
        renderGraph_.beginFrameExecution(scheduler.currentFrameIndex());
        renderGraph_.setFrameRecordContext({
            .commandBuffer = currentCmd,
            .frameIndex = scheduler.currentFrameIndex(),
            .imageIndex = currentImageIndex,
            .collectCounters = collectFrameCounters_,
            .sceneExtent = sceneExtent_,
        });
        frameOpen_ = true;
        return frame.status;
    }

    bool VulkanVertexBackend::resolveGpuSceneCaster(
        uint32_t primitiveIndex, uint32_t consumerMask,
        ResolvedShadowCaster& caster) const noexcept {
        return resolveIndirectCaster(indirectScene(scheduler.currentFrameIndex()),
            primitiveIndex, consumerMask, caster);
    }

    VulkanIndirectScene VulkanVertexBackend::indirectScene(
        uint32_t frame) const noexcept {
        const GpuSceneCpuMirror& scene = gpuSceneCpuMirrors_[frame];
        return {
            .transforms = scene.transforms,
            .instances = scene.instances,
            .primitives = scene.primitives,
            .geometries = scene.geometries,
            .identities = scene.primitiveIdentities,
            .published = gpuScenePublishedCounts_,
        };
    }

    VulkanIndirectAssetResolver VulkanVertexBackend::indirectAssets() const noexcept {
        return {
            .owner = this,
            .geometry = [](const void* owner, GeometryHandle handle,
                VulkanIndirectGeometry& geometry) {
                const VulkanGeometryPayload* payload =
                    static_cast<const VulkanVertexBackend*>(owner)->
                        geometryVault.get(handle);
                if (payload == nullptr) return false;
                geometry = { payload->vertexBuffer.buffer,
                    payload->indexBuffer.buffer, payload->indexFormat,
                    payload->vertexOffset };
                return true;
            },
            .material = [](const void* owner, MaterialHandle handle,
                VulkanIndirectMaterial& material) {
                const VulkanMaterialPayload* payload =
                    static_cast<const VulkanVertexBackend*>(owner)->
                        materialVault.get(handle);
                if (payload == nullptr) return false;
                material = { payload->packed.alphaMode,
                    payload->packed.doubleSided };
                return true;
            },
            .gbufferIndirectPipeline = [](const void* owner,
                PipelineHandle handle) {
                const VulkanPipelineRecord* record =
                    static_cast<const VulkanVertexBackend*>(owner)->
                        pipelineLibrary.get(handle);
                return record != nullptr &&
                    record->gpuSceneIndirectPipeline != VK_NULL_HANDLE &&
                    record->pipelineLayout != VK_NULL_HANDLE &&
                    record->renderPass == RenderPassClass::GBuffer;
            },
        };
    }

    template<typename Visitor>
    void VulkanVertexBackend::visitShadowCasters(
        const ShadowCasterSubmission& submission, Visitor&& visitor) const {
        ResolvedShadowCaster caster{};
        for (uint32_t primitiveIndex : submission.gpuScenePrimitiveIndices) {
            if (resolveGpuSceneCaster(primitiveIndex,
                    GpuSceneConsumerShadow, caster)) {
                caster.gpuScenePrimitiveIndex = primitiveIndex;
                visitor(caster);
            }
        }
        for (const DrawPacket& packet : submission.directPackets) {
            caster = {
                .geometry = packet.geometry,
                .material = packet.material,
                .pipeline = packet.pipeline,
                .worldTransform = packet.worldTransform,
                .boundsSphereCenterWorld = packet.boundsSphereCenterWorld,
                .boundsSphereRadiusWorld = packet.boundsSphereRadiusWorld,
                .indexCount = packet.indexCount,
                .firstIndex = packet.firstIndex,
                .gpuScenePrimitiveIndex = InvalidGpuSceneIndex,
                .owner = packet.owner,
            };
            visitor(caster);
        }
    }

    template<typename Visitor>
    void VulkanVertexBackend::visitReflectionProbeCasters(
        const ReflectionProbeCasterSubmission& submission,
        Visitor&& visitor) const {
        ResolvedShadowCaster caster{};
        for (uint32_t primitiveIndex : submission.gpuScenePrimitiveIndices) {
            if (resolveGpuSceneCaster(primitiveIndex,
                    GpuSceneConsumerProbe, caster)) {
                caster.gpuScenePrimitiveIndex = primitiveIndex;
                visitor(caster);
            }
        }
        for (const DrawPacket& packet : submission.directPackets) {
            caster = {
                .geometry = packet.geometry,
                .material = packet.material,
                .pipeline = packet.pipeline,
                .worldTransform = packet.worldTransform,
                .boundsSphereCenterWorld = packet.boundsSphereCenterWorld,
                .boundsSphereRadiusWorld = packet.boundsSphereRadiusWorld,
                .indexCount = packet.indexCount,
                .firstIndex = packet.firstIndex,
                .gpuScenePrimitiveIndex = InvalidGpuSceneIndex,
                .owner = packet.owner,
            };
            visitor(caster);
        }
    }

    uint64_t VulkanVertexBackend::getShadowCasterRevision(
        const ShadowCasterSubmission& shadowCasters) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        const auto append = [&hash](const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        };
        visitShadowCasters(shadowCasters,
            [&](const ResolvedShadowCaster& caster) {
            append(&caster.worldTransform, sizeof(caster.worldTransform));
            append(&caster.geometry.id, sizeof(caster.geometry.id));
            append(&caster.material.id, sizeof(caster.material.id));
            append(&caster.pipeline.id, sizeof(caster.pipeline.id));
            append(&caster.indexCount, sizeof(caster.indexCount));
            append(&caster.firstIndex, sizeof(caster.firstIndex));
            if (const VulkanMaterialPayload* material =
                    materialVault.get(caster.material)) {
                append(&material->packedRevision,
                    sizeof(material->packedRevision));
                append(&material->packed.alphaMode,
                    sizeof(material->packed.alphaMode));
                append(&material->packed.doubleSided,
                    sizeof(material->packed.doubleSided));
            }
        });
        return hash;
    }

    std::array<uint64_t, kDirectionalShadowCascadeCount>
        VulkanVertexBackend::getDirectionalShadowCasterRevisions(
            const ShadowCasterSubmission& shadowCasters,
            const DirectionalShadowCascadePlan& plan) const noexcept {
        std::array<uint64_t, kDirectionalShadowCascadeCount> hashes{};
        hashes.fill(1469598103934665603ull);
        const auto append = [](uint64_t& hash, const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        };
        visitShadowCasters(shadowCasters,
            [&](const ResolvedShadowCaster& caster) {
            const uint32_t cascadeMask = directionalShadowCasterCascadeMask(
                plan, caster.boundsSphereCenterWorld,
                caster.boundsSphereRadiusWorld, 0xfu);
            for (uint32_t cascade = 0;
                cascade < kDirectionalShadowCascadeCount; ++cascade) {
                if ((cascadeMask & (1u << cascade)) == 0u) continue;
                uint64_t& hash = hashes[cascade];
                append(hash, &caster.worldTransform,
                    sizeof(caster.worldTransform));
                append(hash, &caster.geometry.id, sizeof(caster.geometry.id));
                append(hash, &caster.material.id, sizeof(caster.material.id));
                append(hash, &caster.pipeline.id, sizeof(caster.pipeline.id));
                append(hash, &caster.indexCount, sizeof(caster.indexCount));
                append(hash, &caster.firstIndex, sizeof(caster.firstIndex));
                if (const VulkanMaterialPayload* material =
                        materialVault.get(caster.material)) {
                    append(hash, &material->packedRevision,
                        sizeof(material->packedRevision));
                    append(hash, &material->packed.alphaMode,
                        sizeof(material->packed.alphaMode));
                    append(hash, &material->packed.doubleSided,
                        sizeof(material->packed.doubleSided));
                }
            }
        });
        return hashes;
    }

    void VulkanVertexBackend::prepareDepthPyramidHistory(
        std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> opaqueForwardQueue) {
        if (!frameOpen_)
            throw std::logic_error(
                "Depth-pyramid history preparation requires an open frame");

        currentDepthContentRevision_ = getShadowCasterRevision({
            .directPackets = opaqueQueue,
        });
        const uint64_t forwardRevision =
            getShadowCasterRevision({
                .directPackets = opaqueForwardQueue,
            });
        appendFnv1a(currentDepthContentRevision_, &forwardRevision,
            sizeof(forwardRevision));
        if (currentDepthContentRevision_ == 0u)
            currentDepthContentRevision_ = 1u;
        depthHistoryPrepared_ = true;

        if (!depthPyramidEnabled_) {
            currentDepthHistoryDecision_ = evaluateDepthPyramidHistory({});
            frameCounters_.depthHistoryRejection = static_cast<uint32_t>(
                currentDepthHistoryDecision_.rejection);
            return;
        }

        const auto& view = gpuSceneCpuViews_[scheduler.currentFrameIndex()];
        bool projectionValid = true;
        for (uint32_t column = 0; column < 4u; ++column) {
            for (uint32_t row = 0; row < 4u; ++row) {
                projectionValid = projectionValid &&
                    std::isfinite(view.view[column][row]) &&
                    std::isfinite(view.projection[column][row]);
            }
        }
        const DepthPyramidHistoryOwner currentOwner{
            .viewIdentity = currentViewHistory_.identity,
            .sceneEpoch = retainedRenderView_ == 0u
                ? publishedGpuSceneEpoch_
                : currentViewHistory_.identity,
            .depthContentRevision = currentDepthContentRevision_,
            .projectionRevision = currentProjectionRevision_,
            .resetRevision = currentViewHistory_.resetRevision,
        };
        const auto& history = depthPyramid_.queuedHistory(
            retainedRenderView_);
        const VkExtent2D extent = frameTargets.extent();
        currentDepthHistoryDecision_ = evaluateDepthPyramidHistory({
            .currentExtent = {extent.width, extent.height},
            .historyExtent = history.extent,
            .currentOwner = currentOwner,
            .historyOwner = history.owner,
            .currentFrameSerial = scheduler.lastSubmittedSerial() + 1u,
            .historyFrameSerial = history.submissionSerial,
            .currentConvention = DeviceDepthConvention::ForwardZeroToOne,
            .historyConvention = history.convention,
            .enabled = true,
            .historyAvailable = history.available,
            .projectionValid = projectionValid,
        });
        frameCounters_.depthHistoryEligible =
            currentDepthHistoryDecision_.eligible ? 1u : 0u;
        frameCounters_.depthHistoryRejection = static_cast<uint32_t>(
            currentDepthHistoryDecision_.rejection);
    }

    bool VulkanVertexBackend::prepareDirectionalShadowIndirectSubmission(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const DirectionalShadowFramePacket> shadows) {
        const uint32_t frame = scheduler.currentFrameIndex();
        const bool planned = directionalCuller_.plan({
            .primitiveIndices = shadowCasters.gpuScenePrimitiveIndices,
            .membershipRevision = shadowCasters.membershipRevision,
            .lodErrorThreshold = experimentalShadowLodErrorTexels_,
            .lodMaximumLevel = shadowLodMaximumLevel_,
            .forceDirectGBufferReference = forceDirectGBufferReference_,
            .forceDirectShadowReference = forceDirectShadowReference_,
            .scene = indirectScene(frame),
            .assets = indirectAssets(),
        }, directionalShadowWork(shadows), frame);
        frameCounters_.shadowDirectionalIndirectFallbackReason =
            static_cast<uint32_t>(directionalCuller_.fallbackReason());
        if (directionalCuller_.membershipCacheHit())
            frameCounters_.shadowDirectionalMembershipCacheHit = 1u;
        if (!planned) {
            renderGraph_.skipPass(graphIds_.directionalIndirect.compact);
            return false;
        }
        renderGraph_.beginPass(currentCmd, graphIds_.directionalIndirect.compact);
        frameCounters_.dispatchRecorded += directionalCuller_.recordCompaction(
            currentCmd, frame, {
                .set0 = directionalShadow_.renderDescriptor(frame),
                .gpuScene = gpuSceneDescriptorSets_[frame],
            });
        return true;
    }

    void VulkanVertexBackend::collectVirtualShadowRequests(uint32_t slot) {
        if (!virtualShadowReadbackPending_[slot]) return;
        const auto& buffer = virtualShadowResources_.requestReadback(slot);
        PackedDirectionalVirtualShadowGpuCompactionTelemetry telemetry{};
        std::memcpy(&telemetry, buffer.mapped, sizeof(telemetry));
        const auto& info = virtualShadowResources_.info();
        const auto& packet = virtualShadowFrameClipPlans_[slot];
        if (telemetry.abiMismatchMarks || telemetry.invalidLevelMarks ||
            telemetry.outputRequestCount > info.requestCapacity ||
            telemetry.outputRequestCount > telemetry.uniquePagesBeforeCapacity ||
            telemetry.requestCapacityOverflow != telemetry.uniquePagesBeforeCapacity - telemetry.outputRequestCount ||
            (!packet && telemetry.outputRequestCount))
            throw std::runtime_error("Live virtual-shadow request telemetry violates the published slot contract");
        const auto* bytes = static_cast<const std::byte*>(buffer.mapped) + info.workingSetLayout.telemetry.size;
        IVulkanIndirectOracle* oracle = packet
            ? activeIndirectOracle(VulkanIndirectOracleView::VirtualShadowDepth)
            : nullptr;
        if (oracle != nullptr && !oracle->beginVirtualShadowVerify({
                .slot = slot,
                .levels = packet->levels(),
                .pageSizeTexels = virtualShadowClipPageSize_,
                .requestCapacity = info.requestCapacity,
                .uniquePagesBeforeCapacity = telemetry.uniquePagesBeforeCapacity,
                .outputRequestCount = telemetry.outputRequestCount,
                .requestCapacityOverflow = telemetry.requestCapacityOverflow,
                .requestCapacityDroppedSamples =
                    telemetry.requestCapacityDroppedSamples }))
            oracle = nullptr;
        for (uint32_t i = 0; i < telemetry.outputRequestCount; ++i) {
            PackedDirectionalVirtualShadowGpuRequest request{};
            std::memcpy(&request, bytes + i * sizeof(request), sizeof(request));
            if (!packet || request.selectedLevelIndex >= packet->levelCount)
                throw std::runtime_error("Live virtual-shadow request selects an unpublished clip");
            const auto ownedRequest = unpackDirectionalVirtualShadowGpuRequest(request,
                packet->levels(), virtualShadowClipPageSize_);
            if (oracle != nullptr) oracle->verifyVirtualShadowRequest(i, ownedRequest);
        }
        if (cpuProfiler_) {
            cpuProfiler_->recordCounter("shadow.virtual.requests.unique", telemetry.uniquePagesBeforeCapacity);
            cpuProfiler_->recordCounter("shadow.virtual.requests.output", telemetry.outputRequestCount);
            cpuProfiler_->recordCounter("shadow.virtual.requests.overflow", telemetry.requestCapacityOverflow);
            cpuProfiler_->recordCounter("shadow.virtual.requests.dropped_samples", telemetry.requestCapacityDroppedSamples);
            cpuProfiler_->recordCounter("shadow.virtual.requests.validated_slot", slot);
            cpuProfiler_->recordCounter("shadow.virtual.requests.readback_bytes", info.requestedReadbackBytes);
            cpuProfiler_->recordCounter("shadow.virtual.oracle.compared_requests",
                oracle != nullptr ? oracle->comparedVirtualShadowRequests() : 0);
            cpuProfiler_->recordCounter("shadow.virtual.oracle.validated", oracle != nullptr);
        }
        virtualShadowReadbackPending_[slot] = false;
    }

    void VulkanVertexBackend::submitDirectionalShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const DirectionalShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error(
                "Directional shadows require an open frame");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        directionalShadow_.updateFrame(frameIndex, shadows);
        // Publish live CPU clip packets even on conventional cache-hit frames.
        // Compute dispatch remains separately graph-gated.
        if (virtualShadowResources_.initialized()) {
            CpuScope virtualClipScope(cpuProfiler_, "cpu.render.virtual-shadow.clip-publication");
            virtualShadowClipPlan_.reset();
            bool gridRejected = false;
            if (!shadows.empty()) {
                virtualShadowCasterBoundsScratch_.clear();
                visitShadowCasters(shadowCasters, [&](const ResolvedShadowCaster& caster) {
                    virtualShadowCasterBoundsScratch_.push_back({
                        caster.boundsSphereCenterWorld, caster.boundsSphereRadiusWorld});
                });
                DirectionalVirtualShadowClipConfig clipConfig{};
                clipConfig.lightOwner = shadows.front().selection.owner;
                clipConfig.lightForward = shadows.front().selection.lightForward;
                clipConfig.focusWorld = glm::vec3(gpuSceneCpuViews_[frameIndex].cameraPosition);
                clipConfig.pageSizeTexels = virtualShadowClipPageSize_;
                clipConfig.virtualResolutionTexels = virtualShadowClipPageSize_ * 128u;
                // No static/dynamic classification is claimed: invalidate both
                // layers conservatively with the complete caster-content key.
                clipConfig.staticCasterRevision = getShadowCasterRevision(shadowCasters);
                clipConfig.dynamicCasterRevision = clipConfig.staticCasterRevision;
                virtualShadowClipPlan_ = virtualShadowClipPublisher_.publish(
                    clipConfig, virtualShadowCasterBoundsScratch_);
                if (virtualShadowClipPlan_) {
                    DirectionalVirtualShadowMarkConfig markConfig{};
                    markConfig.pageSizeTexels = virtualShadowClipPageSize_;
                    markConfig.maximumUniquePageRequests = virtualShadowResources_.info().requestCapacity;
                    try {
                        (void)buildVirtualShadowFullViewPageGrid(markConfig, virtualShadowClipPlan_->levels(),
                            virtualShadowResources_.info().workingSetLayout.scratchCapacity);
                    } catch (const std::invalid_argument&) {
                        // Never partially scan a view or publish an unaddressable
                        // clip stack. Conventional shadows remain the fallback.
                        virtualShadowClipPlan_.reset();
                        gridRejected = true;
                    }
                }
            } else {
                virtualShadowClipPublisher_.reset();
            }
            if (cpuProfiler_) {
                cpuProfiler_->recordCounter("shadow.virtual.clip.available", virtualShadowClipPlan_.has_value());
                cpuProfiler_->recordCounter("shadow.virtual.clip.grid_rejected", gridRejected);
                cpuProfiler_->recordCounter("shadow.virtual.clip.levels",
                    virtualShadowClipPlan_ ? virtualShadowClipPlan_->levelCount : 0);
                cpuProfiler_->recordCounter("shadow.virtual.clip.projection_revision",
                    virtualShadowClipPlan_ ? virtualShadowClipPlan_->clips[0].projectionRevision : 0);
                cpuProfiler_->recordCounter("shadow.virtual.clip.caster_bounds",
                    shadows.empty() ? 0 : virtualShadowCasterBoundsScratch_.size());
            }
            virtualShadowFrameClipPlans_[frameIndex] = virtualShadowClipPlan_;
            std::array<PackedDirectionalVirtualShadowClipLevel, 16> packed{};
            if (const auto& packet = virtualShadowFrameClipPlans_[frameIndex])
                for (uint32_t i = 0; i < packet->levelCount; ++i)
                    packed[i] = packDirectionalVirtualShadowClipLevel(packet->clips[i]);
            renderGraph_.beginPass(currentCmd, graphIds_.virtualShadowClipUpload);
            const auto& range = virtualShadowResources_.info().workingSetLayout.clipLevels;
            if (range.size != sizeof(packed))
                throw std::logic_error("Virtual-shadow clip upload does not match the working-set ABI");
            vkCmdUpdateBuffer(currentCmd, virtualShadowResources_.workingSet(frameIndex).buffer,
                range.offset, sizeof(packed), packed.data());
            if (cpuProfiler_) cpuProfiler_->recordCounter("shadow.virtual.clip.upload_bytes", sizeof(packed));
        }
        const bool hasUpdates = std::any_of(shadows.begin(), shadows.end(),
            [](const DirectionalShadowFramePacket& shadow) {
                return shadow.updateMask != 0u;
            });
        if (!hasUpdates) {
            renderGraph_.skipPass(graphIds_.directionalIndirect.compact);
            renderGraph_.skipPass(graphIds_.shadowDirectional);
            return;
        }
        for (const DirectionalShadowFramePacket& shadow : shadows)
            if (shadow.resolution != directionalShadow_.resolution())
                throw std::invalid_argument(
                    "Directional shadow packet resolution does not match storage");

        CpuScope recordScope(cpuProfiler_, "cpu.render.record.shadow.directional");
        shadowCasterScratch_.clear();
        shadowCasterScratch_.reserve(shadowCasters.size());
        visitShadowCasters(shadowCasters,
            [&](const ResolvedShadowCaster& caster) {
                shadowCasterScratch_.push_back(caster);
            });
        // R3b.7: compaction is its own pass ("shadow.directional.compact").
        const bool indirectValid =
            prepareDirectionalShadowIndirectSubmission(shadowCasters, shadows);
        renderGraph_.beginPass(currentCmd, graphIds_.shadowDirectional);
        IVulkanIndirectOracle* const shadowOracle =
            activeIndirectOracle(VulkanIndirectOracleView::DirectionalShadow);
        frameCounters_.shadowDirectionalDirectFallback =
            static_cast<uint64_t>(std::count_if(
                shadowCasterScratch_.begin(), shadowCasterScratch_.end(),
                [indirectValid](const ResolvedShadowCaster& caster) {
                    return !indirectValid || caster.gpuScenePrimitiveIndex ==
                        InvalidGpuSceneIndex;
                }));
        const VkPipelineLayout layout = directionalShadow_.pipelineLayout();
        const VkDescriptorSet shadowSet =
            directionalShadow_.renderDescriptor(frameIndex);
        VulkanGpuRangeToken gpuRange =
            scheduler.beginGpuRange("gpu.shadow.directional");

        for (const DirectionalShadowFramePacket& shadow : shadows) {
          directionalShadowCasterMaskScratch_.resize(
              shadowCasterScratch_.size());
          for (size_t casterIndex = 0;
              casterIndex < shadowCasterScratch_.size(); ++casterIndex) {
            const ResolvedShadowCaster& caster =
                shadowCasterScratch_[casterIndex];
            const bool requiresCpuVisibility =
                shadowOracle != nullptr || !indirectValid ||
                caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex;
            if (!requiresCpuVisibility) {
                directionalShadowCasterMaskScratch_[casterIndex] = 0u;
                continue;
            }
            const uint32_t visibleMask = directionalShadowCasterCascadeMask(
                shadow.plan, caster.boundsSphereCenterWorld,
                caster.boundsSphereRadiusWorld, shadow.updateMask);
            directionalShadowCasterMaskScratch_[casterIndex] =
                static_cast<uint8_t>(visibleMask);
            if (collectFrameCounters_) {
                frameCounters_.shadowDirectionalCastersTested +=
                    std::popcount(shadow.updateMask);
                frameCounters_.shadowDirectionalCastersCulled +=
                    std::popcount(shadow.updateMask & ~visibleMask);
            }
          }
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
            if (indirectValid) {
                vkCmdBindDescriptorSets(currentCmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    3u, 1u, &gpuSceneDescriptorSets_[frameIndex],
                    0u, nullptr);
                if (directionalCuller_.anyAlphaMaskedBin()) {
                    bindMaterialDescriptors(layout);
                    materialDescriptorsBound = true;
                }
                const uint32_t layer = shadow.shadowIndex *
                    kDirectionalShadowCascadeCount + cascade;
                const uint32_t workIndex = directionalCuller_.workIndex(layer);
                frameCounters_.shadowDirectionalIndirectBins +=
                    directionalCuller_.recordDraws(currentCmd, frameIndex,
                        workIndex, {
                            .layout = layout,
                            .owner = &directionalShadow_,
                            .pipeline = [](const void* owner, bool alphaMasked,
                                bool doubleSided) {
                                return static_cast<const VulkanDirectionalShadowMap*>(
                                    owner)->pipeline(alphaMasked, doubleSided, true);
                            },
                            .pushSlotWord = true,
                            .slotWord = layer,
                        });
                if (shadowOracle != nullptr) {
                    const DensityLodContext lodContext{
                        shadow.plan.cascades[cascade].worldUnitsPerTexel,
                        experimentalShadowLodErrorTexels_ };
                    directionalCuller_.emitExpectations(*shadowOracle,
                        frameIndex, workIndex, shadowCasterScratch_,
                        directionalShadowCasterMaskScratch_,
                        static_cast<uint8_t>(1u << cascade),
                        densityLodMetric(lodContext));
                    recordIndirectOracleDraws(directionalShadowCasterMaskScratch_,
                        static_cast<uint8_t>(1u << cascade),
                        frameCounters_.drawShadowDirectional,
                        frameCounters_.shadowDirectionalIndirectCommands,
                        frameCounters_.drawShadowDirectionalAlphaMask);
                }
                activePipeline = VK_NULL_HANDLE;
                activeGeometry = {};
            }
            for (size_t casterIndex = 0;
                casterIndex < shadowCasterScratch_.size(); ++casterIndex) {
                if ((directionalShadowCasterMaskScratch_[casterIndex] &
                        (1u << cascade)) == 0u)
                    continue;
                const ResolvedShadowCaster& caster =
                    shadowCasterScratch_[casterIndex];
                if (indirectValid && caster.gpuScenePrimitiveIndex !=
                        InvalidGpuSceneIndex)
                    continue;
                VulkanGeometryPayload* geometry = geometryVault.get(caster.geometry);
                VulkanMaterialPayload* material = materialVault.get(caster.material);
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
                if (caster.geometry != activeGeometry) {
                    const VkDeviceSize offset = geometry->vertexOffset;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    activeGeometry = caster.geometry;
                }
                CanonicalMeshPushConstants push{};
                push.renderMatrix = caster.worldTransform;
                push.materialIndex = caster.material.getIndex();
                push.padding[0] = shadow.shadowIndex *
                    kDirectionalShadowCascadeCount + cascade;
                vkCmdPushConstants(currentCmd, layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, caster.indexCount, 1,
                    caster.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawShadowDirectional,
                    caster.indexCount / 3u);
                if (collectFrameCounters_ && alphaMasked)
                    ++frameCounters_.drawShadowDirectionalAlphaMask;
            }
            directionalShadow_.endCascade(currentCmd);
          }
        }
        scheduler.endGpuRange(gpuRange);
    }

    bool VulkanVertexBackend::prepareSpotShadowIndirectSubmission(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const SpotShadowFramePacket> shadows) {
        const uint32_t frame = scheduler.currentFrameIndex();
        const bool planned = spotCuller_.plan({
            .primitiveIndices = shadowCasters.gpuScenePrimitiveIndices,
            .membershipRevision = shadowCasters.membershipRevision,
            .lodErrorThreshold = experimentalShadowLodErrorTexels_,
            .lodMaximumLevel = shadowLodMaximumLevel_,
            .forceDirectGBufferReference = forceDirectGBufferReference_,
            .forceDirectShadowReference = forceDirectShadowReference_,
            .scene = indirectScene(frame),
            .assets = indirectAssets(),
        }, spotShadowWork(shadows), frame);
        frameCounters_.shadowSpotIndirectFallbackReason =
            static_cast<uint32_t>(spotCuller_.fallbackReason());
        if (spotCuller_.membershipCacheHit())
            frameCounters_.shadowSpotMembershipCacheHit = 1u;
        if (!planned) {
            renderGraph_.skipPass(graphIds_.spotIndirect.compact);
            return false;
        }
        renderGraph_.beginPass(currentCmd, graphIds_.spotIndirect.compact);
        frameCounters_.dispatchRecorded += spotCuller_.recordCompaction(
            currentCmd, frame, {
                .set0 = spotShadow_.renderDescriptor(frame),
                .gpuScene = gpuSceneDescriptorSets_[frame],
            });
        return true;
    }

    void VulkanVertexBackend::submitSpotShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const SpotShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error("Spot shadows require an open frame");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        spotShadow_.updateFrame(frameIndex, shadows);

        spotShadowMappingScratch_.assign(lightRecordCapacity_,
            kInvalidShadowDataSlot);
        for (const SpotShadowFramePacket& shadow : shadows) {
            if (shadow.lightSlot >= lightRecordCapacity_ ||
                shadow.shadowDataSlot >= kSpotShadowEntryCapacity)
                throw std::out_of_range(
                    "Spot shadow light or data slot is invalid");
            if (shadow.sampleable)
                spotShadowMappingScratch_[shadow.lightSlot] =
                    shadow.shadowDataSlot;
        }
        if (spotShadowMappingScratch_ != spotShadowDataSlots_) {
            spotShadowDataSlots_ = spotShadowMappingScratch_;
            ++spotShadowMappingRevision_;
            if (spotShadowMappingRevision_ == 0u)
                ++spotShadowMappingRevision_;
        }

        const bool hasUpdates = std::ranges::any_of(shadows,
            [](const SpotShadowFramePacket& shadow) { return shadow.update; });
        if (!hasUpdates) {
            renderGraph_.skipPass(graphIds_.spotIndirect.compact);
            renderGraph_.skipPass(graphIds_.shadowSpot);
            return;
        }
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.shadow.spot");
        shadowCasterScratch_.clear();
        shadowCasterScratch_.reserve(shadowCasters.size());
        visitShadowCasters(shadowCasters,
            [&](const ResolvedShadowCaster& caster) {
                shadowCasterScratch_.push_back(caster);
            });
        const bool indirectValid =
            prepareSpotShadowIndirectSubmission(shadowCasters, shadows);
        renderGraph_.beginPass(currentCmd, graphIds_.shadowSpot);
        IVulkanIndirectOracle* const shadowOracle =
            activeIndirectOracle(VulkanIndirectOracleView::SpotShadow);
        frameCounters_.shadowSpotDirectFallback =
            static_cast<uint64_t>(std::count_if(
                shadowCasterScratch_.begin(), shadowCasterScratch_.end(),
                [indirectValid](const ResolvedShadowCaster& caster) {
                    return !indirectValid || caster.gpuScenePrimitiveIndex ==
                        InvalidGpuSceneIndex;
                }));
        const VkPipelineLayout layout = spotShadow_.pipelineLayout();
        const VkDescriptorSet shadowSet =
            spotShadow_.renderDescriptor(frameIndex);
        VulkanGpuRangeToken gpuRange =
            scheduler.beginGpuRange("gpu.shadow.spot");
        for (const SpotShadowFramePacket& shadow : shadows) {
            if (!shadow.update) continue;
            directionalShadowCasterMaskScratch_.resize(
                shadowCasterScratch_.size());
            for (size_t casterIndex = 0;
                    casterIndex < shadowCasterScratch_.size(); ++casterIndex) {
                const ResolvedShadowCaster& caster =
                    shadowCasterScratch_[casterIndex];
                const bool requiresCpuVisibility =
                    shadowOracle != nullptr || !indirectValid ||
                    caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex;
                if (!requiresCpuVisibility) {
                    directionalShadowCasterMaskScratch_[casterIndex] = 0u;
                    continue;
                }
                const bool visible = shadowCasterSphereIntersectsClipVolume(
                    shadow.worldToShadowClip,
                    caster.boundsSphereCenterWorld,
                    caster.boundsSphereRadiusWorld);
                directionalShadowCasterMaskScratch_[casterIndex] =
                    visible ? 1u : 0u;
                if (collectFrameCounters_) {
                    ++frameCounters_.shadowSpotCastersTested;
                    frameCounters_.shadowSpotCastersCulled += visible ? 0u : 1u;
                }
            }
            spotShadow_.beginTile(currentCmd, shadow);
            vkCmdBindDescriptorSets(currentCmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                0, 1, &shadowSet, 0, nullptr);
            VkPipeline activePipeline = VK_NULL_HANDLE;
            GeometryHandle activeGeometry{};
            bool materialDescriptorsBound = false;
            if (indirectValid) {
                vkCmdBindDescriptorSets(currentCmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    3u, 1u, &gpuSceneDescriptorSets_[frameIndex],
                    0u, nullptr);
                if (spotCuller_.anyAlphaMaskedBin()) {
                    bindMaterialDescriptors(layout);
                    materialDescriptorsBound = true;
                }
                const uint32_t workIndex =
                    spotCuller_.workIndex(shadow.shadowDataSlot);
                frameCounters_.shadowSpotIndirectBins +=
                    spotCuller_.recordDraws(currentCmd, frameIndex, workIndex, {
                        .layout = layout,
                        .owner = &spotShadow_,
                        .pipeline = [](const void* owner, bool alphaMasked,
                            bool doubleSided) {
                            return static_cast<const VulkanSpotShadowAtlas*>(
                                owner)->pipeline(alphaMasked, doubleSided, true);
                        },
                        .pushSlotWord = true,
                        .slotWord = shadow.shadowDataSlot,
                    });
                if (shadowOracle != nullptr) {
                    const PerspectiveLodContext lodContext{
                        shadow.worldToShadowClip,
                        glm::vec2(static_cast<float>(shadow.tileSize)),
                        experimentalShadowLodErrorTexels_ };
                    spotCuller_.emitExpectations(*shadowOracle, frameIndex,
                        workIndex, shadowCasterScratch_,
                        directionalShadowCasterMaskScratch_, 1u,
                        perspectiveLodMetric(lodContext));
                    recordIndirectOracleDraws(directionalShadowCasterMaskScratch_,
                        1u, frameCounters_.drawShadowSpot,
                        frameCounters_.shadowSpotIndirectCommands,
                        frameCounters_.drawShadowSpotAlphaMask);
                }
                activePipeline = VK_NULL_HANDLE;
                activeGeometry = {};
            }
            for (size_t casterIndex = 0;
                    casterIndex < shadowCasterScratch_.size(); ++casterIndex) {
                if (directionalShadowCasterMaskScratch_[casterIndex] == 0u)
                    continue;
                const ResolvedShadowCaster& caster =
                    shadowCasterScratch_[casterIndex];
                if (indirectValid && caster.gpuScenePrimitiveIndex !=
                        InvalidGpuSceneIndex)
                    continue;
                VulkanGeometryPayload* geometry = geometryVault.get(caster.geometry);
                VulkanMaterialPayload* material = materialVault.get(caster.material);
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
                if (caster.geometry != activeGeometry) {
                    const VkDeviceSize offset = geometry->vertexOffset;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    activeGeometry = caster.geometry;
                }
                CanonicalMeshPushConstants push{};
                push.renderMatrix = caster.worldTransform;
                push.materialIndex = caster.material.getIndex();
                push.padding[0] = shadow.shadowDataSlot;
                vkCmdPushConstants(currentCmd, layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, caster.indexCount, 1,
                    caster.firstIndex, 0, 0);
                recordDraw(frameCounters_.drawShadowSpot,
                    caster.indexCount / 3u);
                if (collectFrameCounters_ && alphaMasked)
                    ++frameCounters_.drawShadowSpotAlphaMask;
            }
            spotShadow_.endTile(currentCmd);
        }
        scheduler.endGpuRange(gpuRange);
    }

    bool VulkanVertexBackend::preparePointShadowIndirectSubmission(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const PointShadowFramePacket> shadows) {
        const uint32_t frame = scheduler.currentFrameIndex();
        const bool planned = pointCuller_.plan({
            .primitiveIndices = shadowCasters.gpuScenePrimitiveIndices,
            .membershipRevision = shadowCasters.membershipRevision,
            .lodErrorThreshold = experimentalShadowLodErrorTexels_,
            .lodMaximumLevel = shadowLodMaximumLevel_,
            .forceDirectGBufferReference = forceDirectGBufferReference_,
            .forceDirectShadowReference = forceDirectShadowReference_,
            .scene = indirectScene(frame),
            .assets = indirectAssets(),
        }, pointShadowWork(shadows), frame);
        frameCounters_.shadowPointIndirectFallbackReason =
            static_cast<uint32_t>(pointCuller_.fallbackReason());
        if (pointCuller_.membershipCacheHit())
            frameCounters_.shadowPointMembershipCacheHit = 1u;
        if (!planned) {
            renderGraph_.skipPass(graphIds_.pointIndirect.compact);
            return false;
        }
        renderGraph_.beginPass(currentCmd, graphIds_.pointIndirect.compact);
        frameCounters_.dispatchRecorded += pointCuller_.recordCompaction(
            currentCmd, frame, {
                .set0 = pointShadow_.renderDescriptor(frame),
                .gpuScene = gpuSceneDescriptorSets_[frame],
            });
        return true;
    }

    void VulkanVertexBackend::submitPointShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const PointShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error("Point shadows require an open frame");
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        pointShadow_.updateFrame(frameIndex, shadows);

        pointShadowMappingScratch_.assign(lightRecordCapacity_,
            kInvalidShadowDataSlot);
        for (const PointShadowFramePacket& shadow : shadows) {
            if (shadow.lightSlot >= lightRecordCapacity_ ||
                shadow.shadowDataSlot >= kPointShadowEntryCapacity)
                throw std::out_of_range(
                    "Point shadow light or data slot is invalid");
            if (shadow.sampleable)
                pointShadowMappingScratch_[shadow.lightSlot] =
                    shadow.shadowDataSlot;
        }
        if (pointShadowMappingScratch_ != pointShadowDataSlots_) {
            pointShadowDataSlots_ = pointShadowMappingScratch_;
            ++pointShadowMappingRevision_;
            if (pointShadowMappingRevision_ == 0u)
                ++pointShadowMappingRevision_;
        }

        const bool hasUpdates = std::ranges::any_of(shadows,
            [](const PointShadowFramePacket& shadow) { return shadow.update; });
        if (!hasUpdates) {
            renderGraph_.skipPass(graphIds_.pointIndirect.compact);
            renderGraph_.skipPass(graphIds_.shadowPoint);
            return;
        }
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.shadow.point");
        shadowCasterScratch_.clear();
        shadowCasterScratch_.reserve(shadowCasters.size());
        visitShadowCasters(shadowCasters,
            [&](const ResolvedShadowCaster& caster) {
                shadowCasterScratch_.push_back(caster);
            });
        const bool indirectValid =
            preparePointShadowIndirectSubmission(shadowCasters, shadows);
        renderGraph_.beginPass(currentCmd, graphIds_.shadowPoint);
        IVulkanIndirectOracle* const shadowOracle =
            activeIndirectOracle(VulkanIndirectOracleView::PointShadow);
        frameCounters_.shadowPointDirectFallback =
            static_cast<uint64_t>(std::count_if(
                shadowCasterScratch_.begin(), shadowCasterScratch_.end(),
                [indirectValid](const ResolvedShadowCaster& caster) {
                    return !indirectValid || caster.gpuScenePrimitiveIndex ==
                        InvalidGpuSceneIndex;
                }));
        const VkPipelineLayout layout = pointShadow_.pipelineLayout();
        const VkDescriptorSet shadowSet =
            pointShadow_.renderDescriptor(frameIndex);
        VulkanGpuRangeToken gpuRange =
            scheduler.beginGpuRange("gpu.shadow.point");
        for (const PointShadowFramePacket& shadow : shadows) {
            if (!shadow.update) continue;
            for (uint32_t face = 0; face < 6u; ++face) {
                directionalShadowCasterMaskScratch_.resize(
                    shadowCasterScratch_.size());
                for (size_t casterIndex = 0;
                        casterIndex < shadowCasterScratch_.size();
                        ++casterIndex) {
                    const ResolvedShadowCaster& caster =
                        shadowCasterScratch_[casterIndex];
                    const bool requiresCpuVisibility =
                        shadowOracle != nullptr || !indirectValid ||
                        caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex;
                    if (!requiresCpuVisibility) {
                        directionalShadowCasterMaskScratch_[casterIndex] = 0u;
                        continue;
                    }
                    const bool visible =
                        shadowCasterSphereIntersectsClipVolume(
                            shadow.worldToShadowClip[face],
                            caster.boundsSphereCenterWorld,
                            caster.boundsSphereRadiusWorld);
                    directionalShadowCasterMaskScratch_[casterIndex] =
                        visible ? 1u : 0u;
                    if (collectFrameCounters_) {
                        ++frameCounters_.shadowPointCastersTested;
                        frameCounters_.shadowPointCastersCulled +=
                            visible ? 0u : 1u;
                    }
                }
                pointShadow_.beginFace(currentCmd, shadow, face);
                vkCmdBindDescriptorSets(currentCmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    0, 1, &shadowSet, 0, nullptr);
                VkPipeline activePipeline = VK_NULL_HANDLE;
                GeometryHandle activeGeometry{};
                bool materialDescriptorsBound = false;
                if (indirectValid) {
                    vkCmdBindDescriptorSets(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                        3u, 1u, &gpuSceneDescriptorSets_[frameIndex],
                        0u, nullptr);
                    if (pointCuller_.anyAlphaMaskedBin()) {
                        bindMaterialDescriptors(layout);
                        materialDescriptorsBound = true;
                    }
                    const uint32_t faceSlot =
                        shadow.shadowDataSlot * 6u + face;
                    const uint32_t workIndex = pointCuller_.workIndex(faceSlot);
                    frameCounters_.shadowPointIndirectBins +=
                        pointCuller_.recordDraws(currentCmd, frameIndex,
                            workIndex, {
                                .layout = layout,
                                .owner = &pointShadow_,
                                .pipeline = [](const void* owner,
                                    bool alphaMasked, bool doubleSided) {
                                    return static_cast<
                                        const VulkanPointShadowPools*>(owner)->
                                        pipeline(alphaMasked, doubleSided, true);
                                },
                                .pushSlotWord = true,
                                .slotWord = faceSlot,
                            });
                    if (shadowOracle != nullptr) {
                        const RadialLodContext lodContext{ shadow.lightPosition,
                            static_cast<float>(shadow.resolution),
                            experimentalShadowLodErrorTexels_ };
                        pointCuller_.emitExpectations(*shadowOracle, frameIndex,
                            workIndex, shadowCasterScratch_,
                            directionalShadowCasterMaskScratch_, 1u,
                            radialLodMetric(lodContext));
                        recordIndirectOracleDraws(
                            directionalShadowCasterMaskScratch_, 1u,
                            frameCounters_.drawShadowPoint,
                            frameCounters_.shadowPointIndirectCommands,
                            frameCounters_.drawShadowPointAlphaMask);
                    }
                    activePipeline = VK_NULL_HANDLE;
                    activeGeometry = {};
                }
                for (size_t casterIndex = 0;
                        casterIndex < shadowCasterScratch_.size();
                        ++casterIndex) {
                    if (directionalShadowCasterMaskScratch_[casterIndex] == 0u)
                        continue;
                    const ResolvedShadowCaster& caster =
                        shadowCasterScratch_[casterIndex];
                    if (indirectValid && caster.gpuScenePrimitiveIndex !=
                            InvalidGpuSceneIndex)
                        continue;
                    VulkanGeometryPayload* geometry =
                        geometryVault.get(caster.geometry);
                    VulkanMaterialPayload* material =
                        materialVault.get(caster.material);
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
                    if (caster.geometry != activeGeometry) {
                        const VkDeviceSize offset = geometry->vertexOffset;
                        vkCmdBindVertexBuffers(currentCmd, 0, 1,
                            &geometry->vertexBuffer.buffer, &offset);
                        vkCmdBindIndexBuffer(currentCmd,
                            geometry->indexBuffer.buffer, 0,
                            toVkIndexType(geometry->indexFormat));
                        activeGeometry = caster.geometry;
                    }
                    CanonicalMeshPushConstants push{};
                    push.renderMatrix = caster.worldTransform;
                    push.materialIndex = caster.material.getIndex();
                    push.padding[0] = shadow.shadowDataSlot * 6u + face;
                    vkCmdPushConstants(currentCmd, layout,
                        VK_SHADER_STAGE_VERTEX_BIT |
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(push), &push);
                    vkCmdDrawIndexed(currentCmd, caster.indexCount, 1,
                        caster.firstIndex, 0, 0);
                    recordDraw(frameCounters_.drawShadowPoint,
                        caster.indexCount / 3u);
                    if (collectFrameCounters_ && alphaMasked)
                        ++frameCounters_.drawShadowPointAlphaMask;
                }
                pointShadow_.endFace(currentCmd);
            }
        }
        scheduler.endGpuRange(gpuRange);
    }

    bool VulkanVertexBackend::prepareReflectionProbeIndirectSubmission(
        const ReflectionProbeCasterSubmission& probeCasters,
        std::span<const ReflectionProbeCaptureScheduleEntry> captures) {
        const uint32_t frame = scheduler.currentFrameIndex();
        if (!probeCuller_.plan({
                .primitiveIndices = probeCasters.gpuScenePrimitiveIndices,
                .membershipRevision = probeCasters.membershipRevision,
                .lodErrorThreshold = experimentalProbeLodErrorPixels_,
                .lodMaximumLevel = probeLodMaximumLevel_,
                .forceDirectGBufferReference = forceDirectGBufferReference_,
                .forceDirectShadowReference = forceDirectShadowReference_,
                .scene = indirectScene(frame),
                .assets = indirectAssets(),
            }, reflectionProbeWork(captures), frame))
            return false;
        // Per-face dispatches follow in recordReflectionProbeIndirectDispatch.
        frameCounters_.dispatchRecorded +=
            probeCuller_.recordCompaction(currentCmd, frame, {});
        return true;
    }

    void VulkanVertexBackend::recordReflectionProbeIndirectDispatch(
        uint32_t frameIndex, uint32_t faceRecord,
        uint32_t excludedInstanceIndex) {
        frameCounters_.dispatchRecorded += probeCuller_.recordWorkItem(
            currentCmd, frameIndex, {
                .set0 = reflectionProbeCapturePass_.faceComputeDescriptor(
                    frameIndex),
                .set0DynamicOffset =
                    reflectionProbeCapturePass_.faceComputeDynamicOffset(
                        faceRecord),
                .gpuScene = gpuSceneDescriptorSets_[frameIndex],
            }, { excludedInstanceIndex, faceRecord, 0u });
    }

    void VulkanVertexBackend::submitReflectionProbeCaptures(
        const ReflectionProbeCasterSubmission& probeCasters,
        std::span<const ReflectionProbeCaptureScheduleEntry> captures,
        const LightingFramePacket& lights) {
        if (!frameOpen_)
            throw std::logic_error(
                "Reflection-probe capture requires an open frame");
        const bool hasWork = std::ranges::any_of(captures,
            [](const ReflectionProbeCaptureScheduleEntry& capture) {
                return capture.scheduledFaceMask != 0u;
            });
        // R3b.8: "probe.capture" (reads the shadow maps; staging and the
        // per-face compaction keep their barriers inside the pass).
        probeCaptureHandled_ = true;
        if (!hasWork) {
            renderGraph_.skipPass(graphIds_.probeCapture);
            return;
        }
        CpuScope recordScope(cpuProfiler_,
            "cpu.render.record.probe_capture");
        renderGraph_.beginPass(currentCmd, graphIds_.probeCapture);
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        uploadLightsForFrame(frameIndex, lights);
        const VkDescriptorSet sceneSet = sceneDescriptors.get(frameIndex);
        const VkPipelineLayout layout =
            reflectionProbeCapturePass_.graphicsLayout();
        const VkPipelineLayout gpuSceneLayout =
            reflectionProbeCapturePass_.gpuSceneGraphicsLayout();
        shadowCasterScratch_.clear();
        shadowCasterScratch_.reserve(probeCasters.size());
        visitReflectionProbeCasters(probeCasters,
            [this](const ResolvedShadowCaster& caster) {
                shadowCasterScratch_.push_back(caster);
            });
        const bool indirectValid =
            prepareReflectionProbeIndirectSubmission(probeCasters, captures);
        IVulkanIndirectOracle* const shadowOracle =
            activeIndirectOracle(VulkanIndirectOracleView::ReflectionProbe);
        const bool probeQualificationOracle =
            shadowOracle != nullptr;
        const uint64_t resolvedGpuSceneCasters =
            std::ranges::count_if(shadowCasterScratch_,
                [](const ResolvedShadowCaster& caster) {
                    return caster.gpuScenePrimitiveIndex !=
                        InvalidGpuSceneIndex;
                });
        const uint64_t resolvedDirectCasters =
            shadowCasterScratch_.size() - resolvedGpuSceneCasters;
        uint64_t casterFaceTests = 0;
        uint64_t casterFacesCulled = 0;
        uint64_t casterFaceDraws = 0;
        uint64_t gpuSceneFaceDraws = 0;
        uint64_t directFaceDraws = 0;
        uint64_t ownerFaceExclusions = 0;
        uint32_t faceRecord = 0;
        VulkanGpuRangeToken captureRange =
            scheduler.beginGpuRange("gpu.probe.capture");
        for (const ReflectionProbeCaptureScheduleEntry& capture : captures) {
            if (capture.scheduledFaceMask == 0u) continue;
            uint32_t excludedInstanceIndex = InvalidGpuSceneIndex;
            uint32_t gpuOwnerPrimitiveCount = 0u;
            if (indirectValid) {
                const GpuSceneCpuMirror& scene =
                    gpuSceneCpuMirrors_[frameIndex];
                for (uint32_t primitiveIndex :
                        probeCasters.gpuScenePrimitiveIndices) {
                    if (primitiveIndex >= scene.primitives.size() ||
                        primitiveIndex >= scene.primitiveIdentities.size() ||
                        scene.primitiveIdentities[primitiveIndex].owner !=
                            capture.owner)
                        continue;
                    const uint32_t instanceIndex =
                        scene.primitives[primitiveIndex].binding.x;
                    if (excludedInstanceIndex != InvalidGpuSceneIndex &&
                        excludedInstanceIndex != instanceIndex)
                        throw std::logic_error(
                            "Reflection-probe owner spans multiple GPU-scene instances");
                    excludedInstanceIndex = instanceIndex;
                    ++gpuOwnerPrimitiveCount;
                }
                if (!probeQualificationOracle)
                    ownerFaceExclusions +=
                        static_cast<uint64_t>(gpuOwnerPrimitiveCount) *
                        std::popcount(static_cast<uint32_t>(
                            capture.scheduledFaceMask));
            }
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
                if (indirectValid)
                    recordReflectionProbeIndirectDispatch(frameIndex,
                        faceRecord, excludedInstanceIndex);
                directionalShadowCasterMaskScratch_.assign(
                    shadowCasterScratch_.size(), 0u);
                const size_t cpuVisibilityBegin = indirectValid &&
                        !probeQualificationOracle
                    ? static_cast<size_t>(resolvedGpuSceneCasters) : 0u;
                for (size_t casterIndex = cpuVisibilityBegin;
                        casterIndex < shadowCasterScratch_.size();
                        ++casterIndex) {
                    const ResolvedShadowCaster& caster =
                        shadowCasterScratch_[casterIndex];
                    if (caster.owner == capture.owner) {
                        ++ownerFaceExclusions;
                        continue;
                    }
                    ++casterFaceTests;
                    const bool visible =
                        shadowCasterSphereIntersectsClipVolume(
                            capture.faces[face].worldToClip,
                            caster.boundsSphereCenterWorld,
                            caster.boundsSphereRadiusWorld);
                    directionalShadowCasterMaskScratch_[casterIndex] =
                        visible ? 1u : 0u;
                    casterFacesCulled += visible ? 0u : 1u;
                }
                reflectionProbeCapturePass_.beginFace(currentCmd, target,
                    face, frameIndex, faceRecord, sceneSet);
                reflectionProbeCapturePass_.bindFaceDescriptors(currentCmd,
                    frameIndex, faceRecord, sceneSet);
                // Both capture layouts have an identical set 0-3 prefix. Bind
                // the shared tables through the longer layout once so compact
                // GPU-scene draws and explicit fallback packets can interleave.
                bindMaterialDescriptors(gpuSceneLayout);
                vkCmdBindDescriptorSets(currentCmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, gpuSceneLayout,
                    4u, 1u, &gpuSceneDescriptorSets_[frameIndex],
                    0u, nullptr);
                VkPipeline activePipeline = VK_NULL_HANDLE;
                GeometryHandle activeGeometry{};
                if (indirectValid) {
                    (void)probeCuller_.recordDraws(currentCmd, frameIndex,
                        faceRecord, {
                            .owner = &reflectionProbeCapturePass_,
                            .pipeline = [](const void* owner, bool alphaMasked,
                                bool doubleSided) {
                                return static_cast<
                                    const VulkanReflectionProbeCapturePass*>(
                                    owner)->pipeline(alphaMasked, doubleSided,
                                        true);
                            },
                        });
                    if (probeQualificationOracle) {
                        const RadialLodContext lodContext{ capture.position,
                            static_cast<float>(capture.resolution),
                            experimentalProbeLodErrorPixels_ };
                        probeCuller_.emitExpectations(*shadowOracle, frameIndex,
                            faceRecord,
                            std::span(shadowCasterScratch_).first(
                                static_cast<size_t>(resolvedGpuSceneCasters)),
                            std::span(directionalShadowCasterMaskScratch_).first(
                                static_cast<size_t>(resolvedGpuSceneCasters)),
                            1u, radialLodMetric(lodContext));
                        for (size_t casterIndex = 0;
                                casterIndex < resolvedGpuSceneCasters;
                                ++casterIndex) {
                            if (directionalShadowCasterMaskScratch_[
                                    casterIndex] == 0u)
                                continue;
                            ++gpuSceneFaceDraws;
                            ++casterFaceDraws;
                        }
                    }
                    activePipeline = VK_NULL_HANDLE;
                    activeGeometry = {};
                }
                const size_t directDrawBegin = indirectValid
                    ? static_cast<size_t>(resolvedGpuSceneCasters) : 0u;
                for (size_t casterIndex = directDrawBegin;
                        casterIndex < shadowCasterScratch_.size();
                        ++casterIndex) {
                    if (directionalShadowCasterMaskScratch_[casterIndex] == 0u)
                        continue;
                    const ResolvedShadowCaster& caster =
                        shadowCasterScratch_[casterIndex];
                    VulkanGeometryPayload* geometry =
                        geometryVault.get(caster.geometry);
                    VulkanMaterialPayload* material =
                        materialVault.get(caster.material);
                    if (geometry == nullptr || material == nullptr) continue;
                    const bool gpuScene = caster.gpuScenePrimitiveIndex !=
                        InvalidGpuSceneIndex;
                    const VkPipeline pipeline =
                        reflectionProbeCapturePass_.pipeline(
                            material->packed.alphaMode == 1u,
                            material->packed.doubleSided != 0u,
                            gpuScene);
                    if (pipeline != activePipeline) {
                        vkCmdBindPipeline(currentCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                        activePipeline = pipeline;
                    }
                    if (caster.geometry != activeGeometry) {
                        const VkDeviceSize offset = geometry->vertexOffset;
                        vkCmdBindVertexBuffers(currentCmd, 0, 1,
                            &geometry->vertexBuffer.buffer, &offset);
                        vkCmdBindIndexBuffer(currentCmd,
                            geometry->indexBuffer.buffer, 0,
                            toVkIndexType(geometry->indexFormat));
                        activeGeometry = caster.geometry;
                    }
                    if (gpuScene) {
                        vkCmdDrawIndexed(currentCmd, caster.indexCount, 1,
                            caster.firstIndex, 0,
                            caster.gpuScenePrimitiveIndex);
                        ++gpuSceneFaceDraws;
                    }
                    else {
                        CanonicalMeshPushConstants push{};
                        push.renderMatrix = caster.worldTransform;
                        push.materialIndex = caster.material.getIndex();
                        vkCmdPushConstants(currentCmd, layout,
                            VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(push), &push);
                        vkCmdDrawIndexed(currentCmd, caster.indexCount, 1,
                            caster.firstIndex, 0, 0);
                        ++directFaceDraws;
                    }
                    ++casterFaceDraws;
                }
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
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter(
                "probe.capture.casters.resolved_gpu_scene",
                resolvedGpuSceneCasters);
            cpuProfiler_->recordCounter(
                "probe.capture.casters.resolved_direct",
                resolvedDirectCasters);
            cpuProfiler_->recordCounter(
                "probe.capture.casters.invalid_gpu_scene",
                probeCasters.gpuScenePrimitiveIndices.size() -
                    resolvedGpuSceneCasters);
            const ProfileCounterStatus cpuVisibilityStatus = indirectValid &&
                    !probeQualificationOracle
                ? ProfileCounterStatus::Unavailable
                : ProfileCounterStatus::Exact;
            cpuProfiler_->recordCounter("probe.capture.caster_face_tests",
                casterFaceTests, cpuVisibilityStatus);
            cpuProfiler_->recordCounter("probe.capture.caster_faces_culled",
                casterFacesCulled, cpuVisibilityStatus);
            cpuProfiler_->recordCounter("probe.capture.caster_face_draws",
                casterFaceDraws, cpuVisibilityStatus);
            cpuProfiler_->recordCounter(
                "probe.capture.gpu_scene_face_draws", gpuSceneFaceDraws,
                cpuVisibilityStatus);
            cpuProfiler_->recordCounter(
                "probe.capture.direct_face_draws", directFaceDraws);
            cpuProfiler_->recordCounter(
                "probe.capture.owner_face_exclusions",
                ownerFaceExclusions);
            cpuProfiler_->recordCounter(
                "probe.capture.indirect.enabled", indirectValid ? 1u : 0u);
            cpuProfiler_->recordCounter(
                "probe.capture.indirect.bins",
                indirectValid ? probeCuller_.bins().size() : 0u);
            cpuProfiler_->recordCounter(
                "probe.capture.indirect.fallback_reason",
                static_cast<uint32_t>(probeCuller_.fallbackReason()));
        }
    }

    bool VulkanVertexBackend::prepareOpaqueIndirectSubmission(
        std::span<const DrawPacket> opaqueQueue) {
        const uint32_t frame = scheduler.currentFrameIndex();
        if (!opaqueCuller_.plan({
                .queue = opaqueQueue,
                .scene = indirectScene(frame),
                .sceneBuffersMapped =
                    gpuScenePrimitiveBuffers_[frame].mapped != nullptr &&
                    gpuSceneGeometryBuffers_[frame].mapped != nullptr &&
                    gpuSceneInstanceBuffers_[frame].mapped != nullptr &&
                    gpuSceneTransformBuffers_[frame].mapped != nullptr,
                .assets = indirectAssets(),
                .queryOcclusion = depthOcclusionQueryEnabled_ &&
                    currentDepthHistoryDecision_.eligible,
                .view = &gpuSceneCpuViews_[frame],
            }, frame)) {
            renderGraph_.skipPass(graphIds_.opaqueIndirect.compact);
            return false;
        }
        renderGraph_.beginPass(currentCmd, graphIds_.opaqueIndirect.compact);
        frameCounters_.dispatchRecorded += opaqueCuller_.recordCompaction(
            currentCmd, frame, {
                .globalSet = globalDescriptorSets[frame],
                .gpuSceneSet = gpuSceneDescriptorSets_[frame],
                .retainedView = retainedRenderView_,
            });
        return true;
    }

    void VulkanVertexBackend::submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> selectionQueue, bool isWireframe) {
        if (depthPyramidEnabled_ && !depthHistoryPrepared_)
            throw std::logic_error(
                "Depth-pyramid history must be prepared before opaque submission");
        selectionOutlineActive_ = !selectionQueue.empty();
        // Frames that never submit probe captures (asset preview) skip the
        // declared pass before the opaque compaction.
        if (!probeCaptureHandled_) {
            renderGraph_.skipPass(graphIds_.probeCapture);
            probeCaptureHandled_ = true;
        }
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.gbuffer");
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        // R3b.7: "gpu-scene.opaque.compact" is begun or skipped here.
        if (isWireframe) renderGraph_.skipPass(graphIds_.opaqueIndirect.compact);
        const bool indirectValid = !isWireframe &&
            prepareOpaqueIndirectSubmission(opaqueQueue);
        renderGraph_.beginPass(currentCmd, graphIds_.gbuffer);
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
                    VkDeviceSize offset = geometry->vertexOffset;
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
            if (indirectValid) {
                const uint32_t frame = scheduler.currentFrameIndex();
                const VulkanIndirectStreamTap drawStream{
                    activeIndirectStreamObserver(),
                    VulkanIndirectStreamView::Opaque, frame };
                uint64_t oracleVisibleCommands = 0;
                for (uint32_t binIndex = 0;
                        binIndex < opaqueCuller_.bins().size(); ++binIndex) {
                    const VulkanOpaqueIndirectCuller::Bin& bin =
                        opaqueCuller_.bins()[binIndex];
                    const DrawPacket& packet = opaqueQueue[bin.packetBegin];
                    auto* geometry = geometryVault.get(bin.geometry);
                    const VulkanPipelineRecord* record =
                        pipelineLibrary.get(bin.pipeline);

                    if (bin.pipeline != lastBoundPipeline) {
                        vkCmdBindPipeline(currentCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            record->gpuSceneIndirectPipeline);
                        recordPipelineBind(bin.pipeline.id);
                        activeLayout = record->pipelineLayout;
                        vkCmdBindDescriptorSets(currentCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                            0u, 1u, &globalDescriptorSets[frame], 0u, nullptr);
                        vkCmdBindDescriptorSets(currentCmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                            4u, 1u, &gpuSceneDescriptorSets_[frame],
                            0u, nullptr);
                        lastBoundPipeline = bin.pipeline;
                        lastBoundMaterial = MaterialHandle{};
                    }
                    if (bin.material != lastBoundMaterial) {
                        bindMaterialDescriptors(activeLayout);
                        recordMaterialBind(bin.material);
                        lastBoundMaterial = bin.material;
                    }
                    // Commands carry exact signed base vertices; children in the
                    // same physical arena need no per-primitive buffer rebind.
                    const VkDeviceSize vertexOffset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                        &geometry->vertexBuffer.buffer, &vertexOffset);
                    vkCmdBindIndexBuffer(currentCmd,
                        geometry->indexBuffer.buffer, 0u,
                        toVkIndexType(geometry->indexFormat));

                    CanonicalMeshPushConstants push{};
                    push.materialIndex = bin.material.getIndex();
                    push.padding[0] = static_cast<uint32_t>(debugView_);
                    vkCmdPushConstants(currentCmd, activeLayout,
                        VK_SHADER_STAGE_VERTEX_BIT |
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                        0u, sizeof(push), &push);
                    vkCmdDrawIndexedIndirectCount(currentCmd,
                        opaqueCuller_.buffers().commands[frame].buffer,
                        static_cast<VkDeviceSize>(bin.commandBegin) *
                            sizeof(GpuSceneIndexedIndirectCommand),
                        opaqueCuller_.buffers().counts[frame].buffer,
                        static_cast<VkDeviceSize>(binIndex) * sizeof(uint32_t),
                        bin.commandCount,
                        sizeof(GpuSceneIndexedIndirectCommand));
                    drawStream.indirectDraw(record->gpuSceneIndirectPipeline,
                        geometry->vertexBuffer.buffer,
                        geometry->indexBuffer.buffer,
                        toVkIndexType(geometry->indexFormat),
                        push.padding[0],
                        opaqueCuller_.buffers().commands[frame].buffer,
                        static_cast<VkDeviceSize>(bin.commandBegin) *
                            sizeof(GpuSceneIndexedIndirectCommand),
                        opaqueCuller_.buffers().counts[frame].buffer,
                        static_cast<VkDeviceSize>(binIndex) * sizeof(uint32_t),
                        bin.commandCount);
                    for (uint32_t command = 0;
                            command < bin.commandCount; ++command) {
                        const DrawPacket& drawn = opaqueQueue[
                            bin.packetBegin + command];
                        if (cpuVisibilityOracleVisible(drawn)) {
                            recordDraw(frameCounters_.drawOpaque,
                                drawn.indexCount / 3u);
                            ++oracleVisibleCommands;
                        }
                    }
                }
                frameCounters_.opaqueIndirectCommands =
                    oracleVisibleCommands;
                frameCounters_.opaqueIndirectBins = opaqueCuller_.bins().size();
            }
            else for (const auto& packet : opaqueQueue) {
                if (!forceDirectGBufferReference_ && hasGpuScenePrimitive(packet) &&
                    !cpuVisibilityOracleVisible(packet)) {
                    continue;
                }
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
VkDeviceSize offset = geometry->vertexOffset;
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
            if (!indirectValid) {
                frameCounters_.opaqueIndirectFallbackPackets = 0u;
                for (const DrawPacket& packet : opaqueQueue) {
                    if (forceDirectGBufferReference_ || !hasGpuScenePrimitive(packet) ||
                        cpuVisibilityOracleVisible(packet)) {
                        ++frameCounters_.opaqueIndirectFallbackPackets;
                    }
                }
                frameCounters_.opaqueIndirectFallbackReason =
                    static_cast<uint32_t>(opaqueCuller_.indirectPlan().fallbackReason ==
                            GpuSceneIndirectFallbackReason::None
                        ? GpuSceneIndirectFallbackReason::InvalidPacket
                        : opaqueCuller_.indirectPlan().fallbackReason);
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
                push.padding[0] = packet.selectionFeedback != 0
                    ? packet.selectionFeedback : 1u;

                if (packet.material != lastBoundMaterial) {
                    bindMaterialDescriptors(meshLayout);
                    recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = geometry->vertexOffset;
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

    void VulkanVertexBackend::createGpuSceneBuffers(
        const GpuSceneCapacityRequirements& capacity) {
        if (frameOpen_) {
            throw std::logic_error(
                "GPU-scene buffers may grow only at a frame boundary");
        }
        if (capacity.transforms == 0 || capacity.instances == 0 ||
            capacity.primitives == 0 || capacity.geometries == 0 ||
            capacity.transforms > gpuSceneMaximumCapacity_.transforms ||
            capacity.instances > gpuSceneMaximumCapacity_.instances ||
            capacity.primitives > gpuSceneMaximumCapacity_.primitives ||
            capacity.geometries > gpuSceneMaximumCapacity_.geometries) {
            throw std::invalid_argument(
                "GPU-scene capacity is outside the device storage limit");
        }
        using Buffers = std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight>;
        Buffers transforms{}, instances{}, primitives{}, geometries{};
        const auto create = [this](VulkanBufferResource& destination,
            uint64_t count, uint64_t stride) {
            destination = resourceAllocator.createBuffer(count * stride,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::GpuScene);
            std::memset(destination.mapped, 0,
                static_cast<size_t>(count * stride));
        };
        try {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                create(transforms[frame], capacity.transforms,
                    sizeof(GpuSceneAffineTransform));
                create(instances[frame], capacity.instances,
                    sizeof(GpuSceneInstanceRecord));
                create(primitives[frame], capacity.primitives,
                    sizeof(GpuScenePrimitiveRecord));
                create(geometries[frame], capacity.geometries,
                    sizeof(GpuSceneGeometryRecord));
            }
        }
        catch (...) {
            for (Buffers* buffers : { &transforms, &instances,
                    &primitives, &geometries })
                for (VulkanBufferResource& buffer : *buffers)
                    resourceAllocator.destroy(buffer);
            throw;
        }
        if (gpuSceneCapacity_.instances != 0) scheduler.waitForAllFrames();
        for (Buffers* buffers : { &gpuSceneTransformBuffers_,
                &gpuSceneInstanceBuffers_, &gpuScenePrimitiveBuffers_,
                &gpuSceneGeometryBuffers_ })
            for (VulkanBufferResource& buffer : *buffers)
                resourceAllocator.destroy(buffer);
        gpuSceneTransformBuffers_ = transforms;
        gpuSceneInstanceBuffers_ = instances;
        gpuScenePrimitiveBuffers_ = primitives;
        gpuSceneGeometryBuffers_ = geometries;
        gpuSceneCapacity_ = capacity;
        if (gpuSceneDescriptorSets_[0] != VK_NULL_HANDLE)
            bindGpuSceneBuffers();
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            uploadedGpuSceneTransformRevisions_[frame].assign(
                capacity.transforms, 0);
            uploadedGpuSceneInstanceRevisions_[frame].assign(
                capacity.instances, 0);
            uploadedGpuScenePrimitiveRevisions_[frame].assign(
                capacity.primitives, 0);
            uploadedGpuSceneGeometryRevisions_[frame].assign(
                capacity.geometries, 0);
            auto& mirror = gpuSceneCpuMirrors_[frame];
            mirror.transforms.resize(capacity.transforms);
            mirror.instances.resize(capacity.instances);
            mirror.primitives.resize(capacity.primitives);
            mirror.geometries.resize(capacity.geometries);
            mirror.primitiveIdentities.resize(capacity.primitives);
        }
        gpuSceneUploadRanges_.reserve((std::max)({ capacity.transforms,
            capacity.instances, capacity.primitives, capacity.geometries }));
    }

    void VulkanVertexBackend::bindGpuSceneBuffers() {
        for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            const std::array<VkDescriptorBufferInfo, 4> infos{{
                { gpuSceneTransformBuffers_[frame].buffer, 0,
                    gpuSceneTransformBuffers_[frame].size },
                { gpuSceneInstanceBuffers_[frame].buffer, 0,
                    gpuSceneInstanceBuffers_[frame].size },
                { gpuScenePrimitiveBuffers_[frame].buffer, 0,
                    gpuScenePrimitiveBuffers_[frame].size },
                { gpuSceneGeometryBuffers_[frame].buffer, 0,
                    gpuSceneGeometryBuffers_[frame].size },
            }};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding] = {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[binding].dstSet = gpuSceneDescriptorSets_[frame];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorType =
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[binding].descriptorCount = 1u;
                writes[binding].pBufferInfo = &infos[binding];
            }
            vkUpdateDescriptorSets(vkContext->getDevice(),
                static_cast<uint32_t>(writes.size()), writes.data(),
                0u, nullptr);
        }
    }

    VulkanCullerServices VulkanVertexBackend::cullerServices() {
        cullerDevice_ = { vkContext->getDevice(), &resourceAllocator,
            &descriptorAllocator, &scheduler };
        return {
            .commands = VulkanCullerCommands::vulkan(&scheduler),
            .resources = cullerDevice_.resources(),
            .capabilities = {
                .multiDrawIndirect = vkContext->hasMultiDrawIndirect(),
                .drawIndirectFirstInstance =
                    vkContext->hasDrawIndirectFirstInstance(),
                .drawIndirectCount = vkContext->hasDrawIndirectCount(),
                .maxDrawIndirectCount = vkContext->getMaxDrawIndirectCount(),
            },
            .profiler = cpuProfiler_,
            .oracle = indirectOracle_,
            .streamObserver = activeIndirectStreamObserver(),
            .sceneOwner = this,
            .scene = [](const void* owner, uint32_t slot) {
                return static_cast<const VulkanVertexBackend*>(owner)->
                    indirectScene(slot);
            },
            .maximumPrimitiveCapacity = MaximumOpaqueIndirectCommandCapacity,
        };
    }

    void VulkanVertexBackend::createDirectionalShadowIndirectPipeline() {
        const VkDevice device = vkContext->getDevice();
        indirectCullerSetLayout_ =
            createIndirectSetLayout(device, "directional-shadow");
        directionalCuller_.init(cullerServices(),
            IndirectViewKind::DirectionalShadow,
            createIndirectViewPipeline(device, IndirectViewKind::DirectionalShadow,
                directionalShadow_.renderSetLayout(),
                meshLayouts.getGpuSceneSetLayout(), indirectCullerSetLayout_),
            indirectCullerSetLayout_,
            activeIndirectOracle(VulkanIndirectOracleView::DirectionalShadow));
    }

    void VulkanVertexBackend::createSpotShadowIndirectPipeline() {
        if (indirectCullerSetLayout_ == VK_NULL_HANDLE)
            throw std::logic_error(
                "spot-shadow indirect resources require the shared shadow layout");
        spotCuller_.init(cullerServices(), IndirectViewKind::SpotShadow,
            createIndirectViewPipeline(vkContext->getDevice(),
                IndirectViewKind::SpotShadow, spotShadow_.renderSetLayout(),
                meshLayouts.getGpuSceneSetLayout(), indirectCullerSetLayout_),
            indirectCullerSetLayout_,
            activeIndirectOracle(VulkanIndirectOracleView::SpotShadow));
    }

    void VulkanVertexBackend::createPointShadowIndirectPipeline() {
        if (indirectCullerSetLayout_ == VK_NULL_HANDLE)
            throw std::logic_error(
                "point-shadow indirect resources require the shared shadow layout");
        pointCuller_.init(cullerServices(), IndirectViewKind::PointShadow,
            createIndirectViewPipeline(vkContext->getDevice(),
                IndirectViewKind::PointShadow, pointShadow_.renderSetLayout(),
                meshLayouts.getGpuSceneSetLayout(), indirectCullerSetLayout_),
            indirectCullerSetLayout_,
            activeIndirectOracle(VulkanIndirectOracleView::PointShadow));
    }

    void VulkanVertexBackend::createReflectionProbeIndirectPipeline() {
        if (indirectCullerSetLayout_ == VK_NULL_HANDLE ||
            reflectionProbeCapturePass_.captureSetLayout() == VK_NULL_HANDLE)
            throw std::logic_error(
                "reflection-probe indirect resources require capture and command layouts");
        probeCuller_.init(cullerServices(), IndirectViewKind::ReflectionProbe,
            createIndirectViewPipeline(vkContext->getDevice(),
                IndirectViewKind::ReflectionProbe,
                reflectionProbeCapturePass_.captureSetLayout(),
                meshLayouts.getGpuSceneSetLayout(), indirectCullerSetLayout_),
            indirectCullerSetLayout_,
            activeIndirectOracle(VulkanIndirectOracleView::ReflectionProbe));
    }

    void VulkanVertexBackend::collectIndirectViewValidations(uint32_t frameIndex) {
        for (VulkanIndirectViewCuller* culler : indirectViewCullers())
            culler->collect(frameIndex);
    }

    void VulkanVertexBackend::recordIndirectOracleDraws(
        std::span<const uint8_t> visibility, uint8_t visibilityBit,
        uint64_t& drawCounter, uint64_t& commandCounter,
        uint64_t& alphaMaskCounter) {
        for (size_t casterIndex = 0; casterIndex < shadowCasterScratch_.size();
                ++casterIndex) {
            const ResolvedShadowCaster& caster = shadowCasterScratch_[casterIndex];
            if (caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex ||
                (visibility[casterIndex] & visibilityBit) == 0u)
                continue;
            const VulkanMaterialPayload* material =
                materialVault.get(caster.material);
            recordDraw(drawCounter, caster.indexCount / 3u);
            ++commandCounter;
            if (collectFrameCounters_ && material != nullptr &&
                material->packed.alphaMode == 1u)
                ++alphaMaskCounter;
        }
    }

    void VulkanVertexBackend::createGpuSceneCullPipeline() {
        opaqueCuller_.init(cullerServices(), {
                .depthOcclusionQuery = depthOcclusionQueryEnabled_,
                .depthOcclusionRejection = depthOcclusionRejectionEnabled_,
                .lodErrorPixels = experimentalGpuLodErrorPixels_,
                .lodMaximumLevel = gpuLodMaximumLevel_,
                .lodHysteresisFraction = gpuLodHysteresisFraction_,
                .forceDirectGBufferReference = forceDirectGBufferReference_,
                .lodOracle = activeIndirectOracle(VulkanIndirectOracleView::OpaqueLod),
                .occlusionOracle =
                    activeIndirectOracle(VulkanIndirectOracleView::DepthOcclusion),
                .depthPyramid = &depthPyramid_,
            },
            createOpaqueCullPipelines(vkContext->getDevice(),
                depthOcclusionRejectionEnabled_, meshLayouts.getGlobalSetLayout(),
                meshLayouts.getGpuSceneSetLayout()));
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
        clusteredLighting_.rebuildDescriptors(renderGraph_, graphIds_.cluster,
            records, active,
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
                info(renderGraph_.buffer(frame, graphIds_.cluster.global)),
                info(renderGraph_.buffer(frame, graphIds_.cluster.headers)),
                info(renderGraph_.buffer(frame, graphIds_.cluster.indices)),
                info(renderGraph_.buffer(frame, graphIds_.cluster.fallback)),
                info(renderGraph_.buffer(frame, graphIds_.cluster.diagnostics)),
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
        bindGraphImportedBuffers();
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

    void VulkanVertexBackend::prepareGpuScene(
        const GpuSceneCapacityRequirements& requirements) {
        if (!initialized_ || cleaned_) {
            throw std::logic_error("Vulkan backend is not initialized");
        }
        if (frameOpen_) {
            throw std::logic_error(
                "GPU-scene capacity must be prepared before beginFrame");
        }
        const auto grow = [](uint32_t current, uint32_t required,
            uint32_t maximum) {
            required = (std::max)(required, 1u);
            return required <= current ? current : nextMaterialTableCapacity(
                current, required, maximum);
        };
        GpuSceneCapacityRequirements next{
            grow(gpuSceneCapacity_.transforms, requirements.transforms,
                gpuSceneMaximumCapacity_.transforms),
            grow(gpuSceneCapacity_.instances, requirements.instances,
                gpuSceneMaximumCapacity_.instances),
            grow(gpuSceneCapacity_.primitives, requirements.primitives,
                gpuSceneMaximumCapacity_.primitives),
            grow(gpuSceneCapacity_.geometries, requirements.geometries,
                gpuSceneMaximumCapacity_.geometries),
        };
        if (next.transforms != gpuSceneCapacity_.transforms ||
            next.instances != gpuSceneCapacity_.instances ||
            next.primitives != gpuSceneCapacity_.primitives ||
            next.geometries != gpuSceneCapacity_.geometries)
            createGpuSceneBuffers(next);
        const uint32_t desiredIndirectCapacity = (std::min)(
            (std::max)(requirements.primitives, 1u),
            MaximumOpaqueIndirectCommandCapacity);
        if (desiredIndirectCapacity > opaqueCuller_.commandCapacity()) {
            const uint32_t grownCapacity = nextMaterialTableCapacity(
                opaqueCuller_.commandCapacity(), desiredIndirectCapacity,
                MaximumOpaqueIndirectCommandCapacity);
            opaqueCuller_.resize(grownCapacity, frameOpen_);
            if (grownCapacity > directionalCuller_.primitiveCapacity())
                directionalCuller_.resize(grownCapacity, frameOpen_);
            if (grownCapacity > spotCuller_.primitiveCapacity())
                spotCuller_.resize(grownCapacity, frameOpen_);
            if (grownCapacity > pointCuller_.primitiveCapacity())
                pointCuller_.resize(grownCapacity, frameOpen_);
            if (grownCapacity > probeCuller_.primitiveCapacity())
                probeCuller_.resize(grownCapacity, frameOpen_);
            bindGraphImportedBuffers();
        }
    }

    void VulkanVertexBackend::publishGpuScene(
        const GpuScenePackedTables& scene) {
        if (!frameOpen_) {
            throw std::logic_error(
                "GPU-scene publication requires an acquired frame context");
        }
        if (scene.abiVersion != GpuSceneAbiVersion ||
            scene.transforms.size() > gpuSceneCapacity_.transforms ||
            scene.instances.size() > gpuSceneCapacity_.instances ||
            scene.primitives.size() > gpuSceneCapacity_.primitives ||
            scene.geometries.size() > gpuSceneCapacity_.geometries ||
            scene.transformRevisions.size() != scene.transforms.size() ||
            scene.instanceRevisions.size() != scene.instances.size() ||
            scene.primitiveRevisions.size() != scene.primitives.size() ||
            scene.geometryRevisions.size() != scene.geometries.size() ||
            scene.primitiveIdentities.size() != scene.primitives.size()) {
            throw std::out_of_range(
                "GPU-scene publication is outside prepared capacity or ABI");
        }
        CpuScope uploadScope(cpuProfiler_, "cpu.gpu_scene.upload");
        publishedGpuSceneEpoch_ = scene.sceneEpoch == 0u
            ? 1u : scene.sceneEpoch;
        if (experimentalGpuLodErrorPixels_ > 0.0f) opaqueCuller_.lodHistory().publish(scene);
        gpuScenePublishedCounts_ = {
            static_cast<uint32_t>(scene.transforms.size()),
            static_cast<uint32_t>(scene.instances.size()),
            static_cast<uint32_t>(scene.primitives.size()),
            static_cast<uint32_t>(scene.geometries.size()),
        };
        const uint32_t frame = scheduler.currentFrameIndex();
        gpuSceneUploadTelemetry_ = {};
        const auto upload = [this](auto records,
            std::span<const uint64_t> revisions,
            std::vector<uint64_t>& uploaded,
            VulkanBufferResource& destination, auto& cpuMirror, uint32_t& tableRanges) {
            buildGpuSceneUploadRanges(revisions, uploaded,
                gpuSceneUploadRanges_);
            tableRanges = static_cast<uint32_t>(gpuSceneUploadRanges_.size());
            gpuSceneUploadTelemetry_.ranges += tableRanges;
            using Record = typename decltype(records)::value_type;
            for (const GpuSceneRecordRange range : gpuSceneUploadRanges_) {
                const auto source = records.subspan(
                    range.firstRecord, range.recordCount);
                resourceAllocator.write(destination,
                    static_cast<VkDeviceSize>(range.firstRecord) *
                        sizeof(Record), std::as_bytes(source));
                std::copy(source.begin(), source.end(), cpuMirror.begin() + range.firstRecord);
                std::copy_n(revisions.begin() + range.firstRecord,
                    range.recordCount,
                    uploaded.begin() + range.firstRecord);
                gpuSceneUploadTelemetry_.bytes +=
                    static_cast<uint64_t>(range.recordCount) * sizeof(Record);
            }
        };
        upload(std::span(scene.transforms), scene.transformRevisions,
            uploadedGpuSceneTransformRevisions_[frame],
            gpuSceneTransformBuffers_[frame],
            gpuSceneCpuMirrors_[frame].transforms,
            gpuSceneUploadTelemetry_.transformRanges);
        upload(std::span(scene.instances), scene.instanceRevisions,
            uploadedGpuSceneInstanceRevisions_[frame],
            gpuSceneInstanceBuffers_[frame],
            gpuSceneCpuMirrors_[frame].instances,
            gpuSceneUploadTelemetry_.instanceRanges);
        upload(std::span(scene.primitives), scene.primitiveRevisions,
            uploadedGpuScenePrimitiveRevisions_[frame],
            gpuScenePrimitiveBuffers_[frame],
            gpuSceneCpuMirrors_[frame].primitives,
            gpuSceneUploadTelemetry_.primitiveRanges);
        for (const GpuSceneRecordRange range : gpuSceneUploadRanges_)
            std::copy_n(scene.primitiveIdentities.begin() +
                    range.firstRecord,
                range.recordCount,
                gpuSceneCpuMirrors_[frame].primitiveIdentities.begin() +
                    range.firstRecord);
        upload(std::span(scene.geometries), scene.geometryRevisions,
            uploadedGpuSceneGeometryRevisions_[frame],
            gpuSceneGeometryBuffers_[frame],
            gpuSceneCpuMirrors_[frame].geometries,
            gpuSceneUploadTelemetry_.geometryRanges);
        if (cpuProfiler_ != nullptr) {
            uint64_t mirrorBytes = 0;
            for (const auto& mirror : gpuSceneCpuMirrors_)
                mirrorBytes += mirror.transforms.capacity() * sizeof(GpuSceneAffineTransform) +
                    mirror.instances.capacity() * sizeof(GpuSceneInstanceRecord) +
                    mirror.primitives.capacity() * sizeof(GpuScenePrimitiveRecord) +
                    mirror.geometries.capacity() * sizeof(GpuSceneGeometryRecord) +
                    mirror.primitiveIdentities.capacity() *
                        sizeof(GpuScenePrimitiveIdentity);
            cpuProfiler_->recordCounter("gpu_scene.cpu_mirror.capacity_bytes", mirrorBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_->recordCounter("gpu_scene.lod.history_buffer_bytes",
                opaqueCuller_.lodHistoryBufferBytes(),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        }
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

    void VulkanVertexBackend::updateCamera(const ViewTransportRecord& view, ViewHistoryContext history) {
        currentViewHistory_ = history;
        currentProjectionRevision_ = viewProjectionRevision(
            view.view, view.projection);
        if (experimentalGpuLodErrorPixels_ > 0.0f) opaqueCuller_.lodHistory().updateView(view, history);
        gpuSceneCpuViews_[scheduler.currentFrameIndex()] = view;
        UniformBufferObject ubo{};
        ubo.model = glm::mat4(1.0f); // Handled individually via push constants
        ubo.view = view.view;
        ubo.proj = view.projection;
        ubo.inverseView = view.inverseView;
        ubo.inverseProjection = view.inverseProjection;
        ubo.cameraPosition = view.cameraPosition;
        ubo.depthRange = view.depthRange;
        ubo.renderInfo = view.renderInfo;
        ubo.renderInfo.w &= ~(ViewTransportRefractionPyramidsAvailable |
            ViewTransportDebugViewMask);
        if (transparencyPyramidResidency_.enabled())
            ubo.renderInfo.w |= ViewTransportRefractionPyramidsAvailable;
        ubo.renderInfo.w |= (static_cast<uint32_t>(debugView_) <<
            ViewTransportDebugViewShift) & ViewTransportDebugViewMask;
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
            // R3b.7: "lighting.probe-cluster" (the dispatch's consumers
            // receive the compute -> fragment barrier from the executor).
            if (probeDimensions.clusterCount() == 0u) {
                renderGraph_.skipPass(graphIds_.probeCluster);
            }
            else {
                renderGraph_.beginPass(currentCmd, graphIds_.probeCluster);
                frameCounters_.dispatchRecorded += reflectionProbePipeline_.record(
                    currentCmd, frameIndex,
                    static_cast<uint32_t>(probeDimensions.clusterCount()));
            }
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
                currentCmd, renderGraph_, graphIds_.cluster, frameIndex,
                static_cast<uint32_t>(dimensions.clusterCount()),
                lights.stats.activeLightCount);
            if (productionGraphFeatures().clusterTelemetryReadback) {
                renderGraph_.beginPass(currentCmd, graphIds_.clusterReadback);
                const VulkanBufferResource& diagnostics = renderGraph_.buffer(
                    frameIndex, graphIds_.cluster.diagnostics);
                const VkBufferCopy copy{ 0, 0, 64 };
                vkCmdCopyBuffer(currentCmd, diagnostics.buffer,
                    clusterDiagnosticReadbackBuffers_[frameIndex].buffer,
                    1, &copy);
                clusterDiagnosticReadbackPending_[frameIndex] = true;
            }
            submittedClusterCounts_[frameIndex] =
                static_cast<uint32_t>(dimensions.clusterCount());
            scheduler.endGpuRange(clusterGpuRange);
        }
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        renderGraph_.beginPass(currentCmd, graphIds_.lighting);

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
        const RenderGraph::PassId pass = exitCapture
            ? graphIds_.ordinary2ExitCapture : graphIds_.ordinary2EntryCapture;
        if (draws.empty()) {
            renderGraph_.skipPass(pass);
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
        renderGraph_.beginPass(currentCmd, pass);
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
                const VkDeviceSize offset = geometry->vertexOffset;
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
        const std::span<const RenderGraph::PassId> passes = quality ==
                TransparencyQuality::Hero4
            ? std::span<const RenderGraph::PassId>(
                graphIds_.hero4.interfaceCapture).first(4u)
            : quality == TransparencyQuality::Cinematic8
                ? std::span<const RenderGraph::PassId>(
                    graphIds_.cinematic8.interfaceCapture).first(8u)
                : std::span<const RenderGraph::PassId>{};
        const std::span<const char* const> gpuRanges = quality ==
                TransparencyQuality::Hero4
            ? std::span<const char* const>(Hero4CaptureGpuRanges)
            : quality == TransparencyQuality::Cinematic8
                ? std::span<const char* const>(Cinematic8CaptureGpuRanges)
                : std::span<const char* const>{};
        if (interfaceIndex >= passes.size() ||
            interfaceIndex >= gpuRanges.size()) {
            throw std::out_of_range(
                "Deep layered interface index is invalid");
        }
        const RenderGraph::PassId pass = passes[interfaceIndex];
        const bool hasTierDraws = std::ranges::any_of(draws,
            [quality](const LayeredCaptureDraw& draw) {
                return draw.quality == quality;
            });
        if (!hasTierDraws) {
            renderGraph_.skipPass(pass);
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
        renderGraph_.beginPass(currentCmd, pass);
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
const VkDeviceSize offset = geometry->vertexOffset;
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
        const RenderGraph::PassId pass = quality == TransparencyQuality::Hero4
            ? graphIds_.hero4.terminateTiles[interfaceIndex]
            : graphIds_.cinematic8.terminateTiles[interfaceIndex];
        const bool hasTierDraws = std::ranges::any_of(draws,
            [quality](const LayeredCaptureDraw& draw) {
                return draw.quality == quality;
            });
        if (!hasTierDraws) {
            renderGraph_.skipPass(pass);
            return;
        }
        const char* gpuRangeName = quality == TransparencyQuality::Hero4
            ? Hero4TerminationGpuRanges[interfaceIndex]
            : Cinematic8TerminationGpuRanges[interfaceIndex];
        VulkanGpuRangeToken gpuRange = scheduler.beginGpuRange(
            gpuRangeName);
        renderGraph_.beginPass(currentCmd, pass);
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
        const RenderGraph::PassId pass = quality == TransparencyQuality::Hero4
            ? graphIds_.hero4.localCompose : graphIds_.cinematic8.localCompose;
        const bool hasTierDraws = std::ranges::any_of(draws,
            [quality](const LayeredCaptureDraw& draw) {
                return draw.quality == quality;
            });
        if (!hasTierDraws) {
            renderGraph_.skipPass(pass);
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
        renderGraph_.beginPass(currentCmd, pass);
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
                const VkDeviceSize offset = geometry->vertexOffset;
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
            push.padding[1] = mirrored | (interfaceCount << 16u) |
                (static_cast<uint32_t>(debugView_) << 24u);
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
                    const VkDeviceSize offset = geometry->vertexOffset;
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
                    (interfaceCount << 16u) |
                    (static_cast<uint32_t>(debugView_) << 24u);
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
        if (draws.empty()) {
            renderGraph_.skipPass(graphIds_.ordinary2LocalCompose);
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
        renderGraph_.beginPass(currentCmd, graphIds_.ordinary2LocalCompose);
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
                const VkDeviceSize offset = geometry->vertexOffset;
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
        if (draws.empty()) {
            renderGraph_.skipPass(graphIds_.ordinary2ComposeHook);
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
        renderGraph_.beginPass(currentCmd, graphIds_.ordinary2ComposeHook);
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
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer,
                    0u, toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = draw.workTableIndex;
            push.padding[0] =
                (static_cast<uint32_t>(debugView_) << 8u) | (2u << 16u);
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
        if (draws.empty() || deepResolvedPacketCount_ == 0u) {
            renderGraph_.skipPass(graphIds_.deepComposeHook);
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
        renderGraph_.beginPass(currentCmd, graphIds_.deepComposeHook);
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
                const VkDeviceSize offset = geometry->vertexOffset;
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
            push.padding[0] = 1u |
                (static_cast<uint32_t>(debugView_) << 8u) |
                (layeredQualityTierContract(draw.quality).
                    maximumInterfaceCount << 16u);
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
        notifyHook({ .point = VulkanHookPoint::DeepLayeredResolveCounts,
            .cmd = currentCmd, .slot = frameIndex,
            .payload = VulkanDeepResolveCountsPayload{ sceneResolveDrawCounts } });
    }

    void VulkanVertexBackend::recordDeepLayeredValidationHook(
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality) {
        if (!graphHooks_.layeredValidation) return;
        const uint32_t interfaceCount = layeredQualityTierContract(
            quality).maximumInterfaceCount;
        if (quality != TransparencyQuality::Hero4 &&
            quality != TransparencyQuality::Cinematic8) {
            throw std::invalid_argument(
                "Deep validation requires Hero4 or Cinematic8");
        }
        const uint32_t drawCount = static_cast<uint32_t>(
            std::ranges::count_if(draws,
                [quality](const LayeredCaptureDraw& draw) {
                    return draw.quality == quality;
                }));
        runPassHook({ .point = VulkanHookPoint::DeepLayeredValidation,
                .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                .payload = VulkanDeepLayeredHookPayload{ quality, interfaceCount,
                    drawCount, static_cast<uint32_t>(
                        deepLayeredAtlasPlan_.workIdentities().size()) } },
            true, quality == TransparencyQuality::Hero4
                ? graphIds_.hero4.validationReadbackHook
                : graphIds_.cinematic8.validationReadbackHook,
            quality == TransparencyQuality::Hero4
                ? "gpu.transparency.layered.hero4.validation-readback"
                : "gpu.transparency.layered.cinematic8.validation-readback");
    }

    void VulkanVertexBackend::submitForwardQueues(
        std::span<const DrawPacket> opaqueForwardQueue,
        std::span<const DrawPacket> sortedSurfaceQueue,
        std::span<const DrawPacket> compatibilityTransparentQueue,
        std::span<const glm::mat4> instanceTransforms) {
        if (!opaqueForwardQueue.empty()) {
            frameCounters_.opaqueIndirectFallbackPackets +=
                opaqueForwardQueue.size();
            if (frameCounters_.opaqueIndirectFallbackReason == 0u) {
                frameCounters_.opaqueIndirectFallbackReason =
                    static_cast<uint32_t>(
                        GpuSceneIndirectFallbackReason::UnsupportedPass);
            }
        }
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
        const uint64_t weightedOitPacketCount = static_cast<uint64_t>(
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return isWeightedOitPacket(packet);
                }));
        uint64_t weightedOitInstanceCount = 0u;
        for (const DrawPacket& packet : sortedSurfaceQueue) {
            if (!isWeightedOitPacket(packet)) continue;
            if (packet.instanceCount == 0u) {
                throw std::logic_error(
                    "WeightedOIT packet has zero instances");
            }
            if (packet.firstInstanceTransform != UINT32_MAX) {
                const uint64_t rangeEnd = static_cast<uint64_t>(
                    packet.firstInstanceTransform) + packet.instanceCount;
                if (rangeEnd > instanceTransforms.size()) {
                    throw std::logic_error(
                        "WeightedOIT packet instance range is invalid");
                }
            }
            else if (packet.instanceCount != 1u) {
                throw std::logic_error(
                    "WeightedOIT multi-instance packet has no transform range");
            }
            weightedOitInstanceCount += packet.instanceCount;
        }
        weightedOitResidency_.observe(weightedOitPacketCount != 0u);
        const bool weightedOitExecutionEnabled =
            weightedOitResidency_.enabled() &&
            weightedOitInstanceCount <= weightedOitInstanceCapacity_;
        if (collectFrameCounters_) {
            frameCounters_.weightedOitPackets = weightedOitPacketCount;
            frameCounters_.weightedOitSortedFallbackPackets =
                weightedOitExecutionEnabled ? 0u : weightedOitPacketCount;
            frameCounters_.weightedOitInstanceCapacityFallbackPackets =
                weightedOitResidency_.enabled() &&
                    !weightedOitExecutionEnabled
                ? weightedOitPacketCount : 0u;
        }
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
            RenderGraph::PassId pass, std::string_view gpuRangeName,
            VkRenderPass renderPass, VkFramebuffer framebuffer,
            RenderPassClass expectedPassClass,
            bool skipResolvedLayered, bool skipWeightedOit) {
            bool hasPackets = false;
            for (size_t packetIndex = 0u;
                packetIndex < queue.size(); ++packetIndex) {
                const DrawPacket& packet = queue[packetIndex];
                if (skipWeightedOit && isWeightedOitPacket(packet))
                    continue;
                if (skipResolvedLayered && isLayeredPacketResolved(
                        static_cast<uint32_t>(packetIndex)))
                    continue;
                hasPackets = true;
                break;
            }
            if (!hasPackets) {
                renderGraph_.skipPass(pass);
                return;
            }

            VulkanGpuRangeToken forwardGpuRange =
                scheduler.beginGpuRange(gpuRangeName.data());
            renderGraph_.beginPass(currentCmd, pass);
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
                if (skipWeightedOit && isWeightedOitPacket(packet))
                    continue;
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
                    const VkDeviceSize offset = geometry->vertexOffset;
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
        recordForwardPass(opaqueForwardQueue, graphIds_.forwardOpaque,
            "gpu.forward.opaque", forwardPass->getRenderPass(),
            targets.forwardFramebuffer, RenderPassClass::Forward, false,
            false);

        if (virtualShadowResources_.initialized()) {
            const uint32_t slot = scheduler.currentFrameIndex();
            const auto& packet = virtualShadowFrameClipPlans_[slot];
            const auto& view = gpuSceneCpuViews_[slot];
            auto gpuRange = scheduler.beginGpuRange("gpu.shadow.virtual.depth-demand");
            renderGraph_.beginPass(currentCmd, graphIds_.virtualShadowDepthMark);
            if (packet) {
                if (virtualShadowDepthBindings_[slot] != targets.depth.view) {
                    virtualShadowResources_.fullViewPass().bindDepth(slot, targets.depth.view,
                        virtualShadowResources_.depthSampler(), sceneExtent_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                    virtualShadowDepthBindings_[slot] = targets.depth.view;
                }
                DirectionalVirtualShadowMarkConfig markConfig{};
                markConfig.pageSizeTexels = virtualShadowClipPageSize_;
                markConfig.maximumUniquePageRequests = virtualShadowResources_.info().requestCapacity;
                const auto cells = virtualShadowResources_.fullViewPass().record(currentCmd, slot,
                    markConfig, packet->levels(), view.inverseView * view.inverseProjection);
                virtualShadowResources_.markingPass().recordCompaction(currentCmd, slot, cells, packet->levelCount);
            } else {
                // No clip is read for zero marks; still overwrite output/counts.
                virtualShadowResources_.markingPass().recordCompaction(currentCmd, slot, 0, 1);
            }
            scheduler.endGpuRange(gpuRange);
            renderGraph_.beginPass(currentCmd, graphIds_.virtualShadowRequestReadback);
            if constexpr (kQualificationBuild) {
                // Explicit qualification copies the exact depth consumed above.
                if (packet && graphHooks_.virtualShadowDepthSnapshot)
                    notifyHook({ .point = VulkanHookPoint::VirtualShadowDepthSnapshot,
                        .cmd = currentCmd, .slot = slot,
                        .payload = VulkanVirtualShadowDepthPayload{ &targets.depth,
                            sceneExtent_, view.inverseView * view.inverseProjection } });
            }
            const auto& layout = virtualShadowResources_.info().workingSetLayout;
            const auto& readback = virtualShadowResources_.requestReadback(slot);
            const std::array<VkBufferCopy, 2> copies{{
                {layout.telemetry.offset, 0, layout.telemetry.size},
                {layout.outputRequests.offset, layout.telemetry.size, layout.outputRequests.size}}};
            vkCmdCopyBuffer(currentCmd, virtualShadowResources_.workingSet(slot).buffer,
                readback.buffer, 2, copies.data());
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = readback.buffer; barrier.size = readback.size;
            vkCmdPipelineBarrier(currentCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 0, nullptr, 1, &barrier, 0, nullptr);
            virtualShadowReadbackPending_[slot] = true;
        }

        const bool requiresRefractionPyramids =
            !compatibilityTransparentQueue.empty();
        transparencyPyramidResidency_.observe(requiresRefractionPyramids);
        if (transparencyPyramidResidency_.requiresFallback(
                requiresRefractionPyramids) && collectFrameCounters_) {
            ++frameCounters_.transparencyPyramidFallbackFrames;
        }
        if (transparencyPyramidResidency_.enabled() &&
            !requiresRefractionPyramids) {
            renderGraph_.skipPass(graphIds_.refractionPyramids);
        }
        else if (transparencyPyramidResidency_.enabled()) {
            VulkanGpuRangeToken pyramidGpuRange = scheduler.beginGpuRange(
                "gpu.transparency.refraction-pyramids");
            renderGraph_.beginPass(currentCmd, graphIds_.refractionPyramids);
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
        if (depthPyramidEnabled_) {
            auto range = scheduler.beginGpuRange("gpu.depth.occlusion-pyramid");
            renderGraph_.beginPass(currentCmd, graphIds_.depthPyramidBuild);
            const DepthPyramidHistoryOwner owner{
                .viewIdentity = currentViewHistory_.identity,
                .sceneEpoch = retainedRenderView_ == 0u
                    ? publishedGpuSceneEpoch_
                    : currentViewHistory_.identity,
                .depthContentRevision = currentDepthContentRevision_,
                .projectionRevision = currentProjectionRevision_,
                .resetRevision = currentViewHistory_.resetRevision,
            };
            const uint32_t dispatches = depthPyramid_.record(currentCmd,
                scheduler.currentFrameIndex(), retainedRenderView_, owner,
                scheduler.lastSubmittedSerial() + 1u);
            if (collectFrameCounters_)
                frameCounters_.dispatchRecorded += dispatches;
            scheduler.endGpuRange(range);
            runPassHook({ .point = VulkanHookPoint::DepthPyramidValidation,
                    .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                    .payload = VulkanDepthPyramidHookPayload{ retainedRenderView_ } },
                graphHooks_.depthPyramidValidation,
                graphIds_.depthPyramidValidationHook,
                "gpu.depth.occlusion-pyramid.validation-readback");
        }
        recordForwardPass(sortedSurfaceQueue, graphIds_.sortedForward,
            "gpu.transparency.sorted.forward", transparentPass->getRenderPass(),
            targets.transparentFramebuffer, RenderPassClass::Transparent,
            false, weightedOitExecutionEnabled);
        if (collectFrameCounters_) {
            frameCounters_.transparentSortedPackets = sortedSurfaceQueue.size() -
                (weightedOitExecutionEnabled
                    ? weightedOitPacketCount : 0u);
        }
        if (ordinary2CaptureTopologyActive) {
            recordOrdinary2Captures(compatibilityTransparentQueue,
                captureDraws);
            recordOrdinary2LocalComposition(compatibilityTransparentQueue,
                captureDraws);
            runPassHook({ .point = VulkanHookPoint::Ordinary2Validation,
                    .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                    .payload = VulkanOrdinary2HookPayload{ ordinary2AtlasExtent_,
                        static_cast<uint32_t>(captureDraws.size()),
                        static_cast<uint32_t>(
                            ordinary2AtlasPlan_.workIdentities().size()) } },
                graphHooks_.layeredValidation,
                graphIds_.ordinary2ValidationHook,
                "gpu.transparency.layered.validation-readback");
            recordOrdinary2SceneResolve(compatibilityTransparentQueue,
                captureDraws);
        }

        if (hero4CaptureTopologyActive) {
            recordDeepLayeredCaptures(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Hero4);
            recordDeepLayeredLocalComposition(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Hero4);
            recordDeepLayeredValidationHook(deepCaptureDraws,
                TransparencyQuality::Hero4);
        }
        if (cinematic8CaptureTopologyActive) {
            recordDeepLayeredCaptures(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Cinematic8);
            recordDeepLayeredLocalComposition(compatibilityTransparentQueue,
                deepCaptureDraws, TransparencyQuality::Cinematic8);
            recordDeepLayeredValidationHook(deepCaptureDraws,
                TransparencyQuality::Cinematic8);
        }
        if (hero4CaptureTopologyActive || cinematic8CaptureTopologyActive) {
            recordDeepLayeredSceneResolve(compatibilityTransparentQueue,
                deepCaptureDraws);
        }

        if (collectFrameCounters_) {
            frameCounters_.transparentBackgroundPackets = 0u;
            frameCounters_.transparentForegroundPackets = 0u;
            frameCounters_.transparentNonemptyBuckets = 0u;
        }
        recordForwardPass(compatibilityTransparentQueue,
            graphIds_.compatibilityForward,
            "gpu.transparency.compatibility.forward",
            forwardPass->getRenderPass(), targets.forwardFramebuffer,
            RenderPassClass::Forward, true, false);

        if (weightedOitResidency_.enabled() &&
            (!weightedOitExecutionEnabled || weightedOitPacketCount == 0u)) {
            renderGraph_.skipPass(graphIds_.oitAccumulate);
            renderGraph_.skipPass(graphIds_.oitResolve);
        }
        else if (weightedOitExecutionEnabled) {
            const uint32_t oitFrameIndex = scheduler.currentFrameIndex();
            const VulkanFrameContextTargets& oitTargets =
                frameTargets.get(oitFrameIndex);
            const VkExtent2D oitExtent = frameTargets.extent();
            const VkViewport viewport{ 0.0f, 0.0f,
                static_cast<float>(oitExtent.width),
                static_cast<float>(oitExtent.height), 0.0f, 1.0f };
            const VkRect2D scissor{ { 0, 0 }, oitExtent };
            VulkanBufferResource& instanceBuffer =
                weightedOitInstanceBuffers_[oitFrameIndex];
            if (instanceBuffer.mapped == nullptr ||
                instanceBuffer.size < weightedOitInstanceStreamBytes(
                    weightedOitInstanceCapacity_)) {
                throw std::logic_error(
                    "WeightedOIT instance stream is not resident");
            }
            const uint32_t oitQueueSize = static_cast<uint32_t>(
                sortedSurfaceQueue.size());
            uint32_t preparedInstanceCount = 0u;
            for (uint32_t ordinal = 0u; ordinal < oitQueueSize; ++ordinal) {
                const uint32_t packetIndex = weightedOitPermutationIndex(
                    ordinal, oitQueueSize, weightedOitOrderSeed_);
                const DrawPacket& packet = sortedSurfaceQueue[packetIndex];
                if (!isWeightedOitPacket(packet) ||
                    geometryVault.get(packet.geometry) == nullptr ||
                    materialVault.get(packet.material) == nullptr) {
                    continue;
                }
                if (packet.firstInstanceTransform == UINT32_MAX) {
                    std::memcpy(static_cast<std::byte*>(instanceBuffer.mapped) +
                            weightedOitInstanceStreamBytes(
                                preparedInstanceCount),
                        &packet.worldTransform, sizeof(packet.worldTransform));
                }
                else {
                    std::memcpy(static_cast<std::byte*>(instanceBuffer.mapped) +
                            weightedOitInstanceStreamBytes(
                                preparedInstanceCount),
                        instanceTransforms.data() +
                            packet.firstInstanceTransform,
                        weightedOitInstanceStreamBytes(packet.instanceCount));
                }
                preparedInstanceCount += packet.instanceCount;
            }
            if (collectFrameCounters_) {
                frameCounters_.weightedOitInstances = preparedInstanceCount;
                frameCounters_.weightedOitInstanceUploadBytes =
                    weightedOitInstanceStreamBytes(preparedInstanceCount);
            }

            renderGraph_.beginPass(currentCmd, graphIds_.oitAccumulate);
            VulkanGpuRangeToken accumulationRange = scheduler.beginGpuRange(
                "gpu.transparency.oit.accumulate");
            std::array<VkClearValue, 2> clears{};
            clears[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
            clears[1].color = { { 1.0f, 0.0f, 0.0f, 0.0f } };
            VkRenderPassBeginInfo accumulationInfo{
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            accumulationInfo.renderPass =
                weightedOit_.accumulationRenderPass();
            accumulationInfo.framebuffer =
                oitTargets.weightedOitAccumulationFramebuffer;
            accumulationInfo.renderArea.extent = oitExtent;
            accumulationInfo.clearValueCount = static_cast<uint32_t>(
                clears.size());
            accumulationInfo.pClearValues = clears.data();
            vkCmdBeginRenderPass(currentCmd, &accumulationInfo,
                VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);

            const VkPipelineLayout accumulationLayout =
                weightedOit_.accumulationPipelineLayout();
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                weightedOit_.accumulationPipeline());
            recordPipelineBind(pipelineIdentity(
                FixedPipelineIdentity::WeightedOitAccumulation));
            const VkDescriptorSet globalSet =
                globalDescriptorSets[oitFrameIndex];
            const VkDescriptorSet sceneSet = sceneDescriptors.get(
                oitFrameIndex);
            vkCmdBindDescriptorSets(currentCmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS, accumulationLayout,
                0u, 1u, &globalSet, 0u, nullptr);
            vkCmdBindDescriptorSets(currentCmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS, accumulationLayout,
                3u, 1u, &sceneSet, 0u, nullptr);

            const VkDeviceSize instanceOffset = 0u;
            vkCmdBindVertexBuffers(currentCmd, 1u, 1u,
                &instanceBuffer.buffer, &instanceOffset);
            MaterialHandle lastMaterial{};
            GeometryHandle lastGeometry{};
            const DrawPacket* batchPacket = nullptr;
            VulkanGeometryPayload* batchGeometry = nullptr;
            VulkanMaterialPayload* batchMaterial = nullptr;
            uint32_t batchFirstInstance = 0u;
            uint32_t batchInstanceCount = 0u;
            const auto sameBatchState = [](const DrawPacket& lhs,
                    const DrawPacket& rhs) {
                constexpr uint32_t MirroredMask = TransparentWorkMirrored;
                return lhs.geometry == rhs.geometry &&
                    lhs.material == rhs.material &&
                    lhs.indexCount == rhs.indexCount &&
                    lhs.firstIndex == rhs.firstIndex &&
                    (lhs.transparentWorkFlags & MirroredMask) ==
                        (rhs.transparentWorkFlags & MirroredMask);
            };
            const auto flushBatch = [&] {
                if (batchInstanceCount == 0u || batchPacket == nullptr ||
                    batchGeometry == nullptr || batchMaterial == nullptr) {
                    return;
                }
                if (batchPacket->material != lastMaterial) {
                    bindMaterialDescriptors(accumulationLayout);
                    recordMaterialBind(batchPacket->material);
                    lastMaterial = batchPacket->material;
                }
                if (batchPacket->geometry != lastGeometry) {
                    const VkDeviceSize offset = batchGeometry->vertexOffset;
                    vkCmdBindVertexBuffers(currentCmd, 0u, 1u,
                        &batchGeometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd,
                        batchGeometry->indexBuffer.buffer, 0u,
                        toVkIndexType(batchGeometry->indexFormat));
                    lastGeometry = batchPacket->geometry;
                }
                const bool mirrored = (batchPacket->transparentWorkFlags &
                    TransparentWorkMirrored) != 0u;
                CanonicalMeshPushConstants push{};
                push.renderMatrix = glm::mat4(1.0f);
                push.materialIndex = batchPacket->material.getIndex();
                push.padding[0] = static_cast<uint32_t>(debugView_);
                push.padding[1] = mirrored ? 1u : 0u;
                vkCmdPushConstants(currentCmd, accumulationLayout,
                    VK_SHADER_STAGE_VERTEX_BIT |
                        VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
                vkCmdDrawIndexed(currentCmd, batchPacket->indexCount,
                    batchInstanceCount, batchPacket->firstIndex, 0,
                    batchFirstInstance);
                recordDraw(frameCounters_.drawWeightedOitAccumulation,
                    static_cast<uint64_t>(batchPacket->indexCount / 3u) *
                        batchInstanceCount);
                if (collectFrameCounters_) {
                    const MaterialClosureClass closure =
                        static_cast<MaterialClosureClass>(
                            batchMaterial->packed.closureClass);
                    if (closure == MaterialClosureClass::StandardForward)
                        ++frameCounters_.drawStandardForward;
                    else if (closure == MaterialClosureClass::ComplexForward) {
                        ++frameCounters_.drawComplexForward;
                        for (uint32_t lobe = 0u;
                            lobe < batchMaterial->packed.complexLobeCount; ++lobe) {
                            const uint32_t type =
                                batchMaterial->packed.complexLobes[lobe].type;
                            if (type < frameCounters_.complexLobeDraws.size())
                                ++frameCounters_.complexLobeDraws[type];
                        }
                    }
                    else if (closure == MaterialClosureClass::Unlit)
                        ++frameCounters_.drawUnlitForward;
                }
                batchPacket = nullptr;
                batchGeometry = nullptr;
                batchMaterial = nullptr;
                batchInstanceCount = 0u;
            };
            uint32_t instanceCursor = 0u;
            for (uint32_t ordinal = 0u; ordinal < oitQueueSize; ++ordinal) {
                const uint32_t packetIndex = weightedOitPermutationIndex(
                    ordinal, oitQueueSize, weightedOitOrderSeed_);
                const DrawPacket& packet = sortedSurfaceQueue[packetIndex];
                if (!isWeightedOitPacket(packet)) continue;
                VulkanGeometryPayload* geometry = geometryVault.get(
                    packet.geometry);
                VulkanMaterialPayload* material = materialVault.get(
                    packet.material);
                if (geometry == nullptr || material == nullptr) continue;
                if (batchPacket != nullptr &&
                    !sameBatchState(*batchPacket, packet)) {
                    flushBatch();
                }
                if (batchPacket == nullptr) {
                    batchPacket = &packet;
                    batchGeometry = geometry;
                    batchMaterial = material;
                    batchFirstInstance = instanceCursor;
                }
                batchInstanceCount += packet.instanceCount;
                instanceCursor += packet.instanceCount;
            }
            flushBatch();
            if (instanceCursor != preparedInstanceCount) {
                throw std::logic_error(
                    "WeightedOIT instance preparation changed during recording");
            }
            vkCmdEndRenderPass(currentCmd);
            scheduler.endGpuRange(accumulationRange);

            renderGraph_.beginPass(currentCmd, graphIds_.oitResolve);
            VulkanGpuRangeToken resolveRange = scheduler.beginGpuRange(
                "gpu.transparency.oit.resolve");
            VkRenderPassBeginInfo resolveInfo{
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            resolveInfo.renderPass = weightedOit_.resolveRenderPass();
            resolveInfo.framebuffer =
                oitTargets.weightedOitResolveFramebuffer;
            resolveInfo.renderArea.extent = oitExtent;
            vkCmdBeginRenderPass(currentCmd, &resolveInfo,
                VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(currentCmd, 0u, 1u, &viewport);
            vkCmdSetScissor(currentCmd, 0u, 1u, &scissor);
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                weightedOit_.resolvePipeline());
            recordPipelineBind(pipelineIdentity(
                FixedPipelineIdentity::WeightedOitResolve));
            const VkPipelineLayout resolveLayout =
                weightedOit_.resolvePipelineLayout();
            const VkDescriptorSet resolveSet =
                weightedOit_.resolveDescriptorSet(oitFrameIndex);
            vkCmdBindDescriptorSets(currentCmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS, resolveLayout,
                0u, 1u, &resolveSet, 0u, nullptr);
            const uint32_t resolveDebugView =
                static_cast<uint32_t>(debugView_);
            vkCmdPushConstants(currentCmd, resolveLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0u,
                sizeof(resolveDebugView), &resolveDebugView);
            vkCmdDraw(currentCmd, 3u, 1u, 0u, 0u);
            recordDraw(frameCounters_.drawWeightedOitResolve, 1u);
            vkCmdEndRenderPass(currentCmd);
            scheduler.endGpuRange(resolveRange);
        }

        if (pipelineStatisticsActive) {
            scheduler.endTransparentPipelineStatistics();
        }
        runCaptureHook(VulkanHookPoint::SceneColorComplete,
            FrameCapturePoint::SceneLinear);
    }

    // ------------------------------------------------------------------
    // Capture hooks (M7R R2.7). The qualification extension owns the
    // readbacks and analysis; the backend owns the graph bracket around a
    // capture copy.
    // ------------------------------------------------------------------

    VulkanCaptureHookPayload VulkanVertexBackend::captureSource(
        FrameCapturePoint point) {
        VulkanFrameContextTargets& targets = frameTargets.get(
            scheduler.currentFrameIndex());
        const bool sceneLinear = point == FrameCapturePoint::SceneLinear;
        return { point, sceneLinear ? &targets.litScene : &targets.output,
            frameTargets.extent(),
            sceneLinear ? frameTargets.format() : outputTargetFormat_ };
    }

    template<typename Record>
    void VulkanVertexBackend::recordCaptureCopy(FrameCapturePoint point,
        Record&& record) {
        const VulkanCaptureHookPayload source = captureSource(point);
        if (point == FrameCapturePoint::SceneLinear) {
            // R3b.5: the declared hook pass moves scene.color to
            // TransferSource; output-transform's begin returns it.
            if (!graphHooks_.sceneColorCapture)
                throw std::logic_error(
                    "Scene-linear capture requires the scene-color-capture-hook pass");
            renderGraph_.beginPass(currentCmd, graphIds_.sceneColorCaptureHook);
        }
        else if (!finalCaptureHookRecorded_) {
            renderGraph_.beginPass(currentCmd, graphIds_.finalCaptureHook);
            finalCaptureHookRecorded_ = true;
        }
        record(source);
    }

    void VulkanVertexBackend::runCaptureHook(VulkanHookPoint point,
        FrameCapturePoint capturePoint) {
        VulkanHookContext context{ .point = point, .cmd = currentCmd,
            .slot = scheduler.currentFrameIndex() };
        if (!anyExtensionWants(context)) {
            if (capturePoint == FrameCapturePoint::SceneLinear &&
                graphHooks_.sceneColorCapture)
                renderGraph_.skipPass(graphIds_.sceneColorCaptureHook);
            return;
        }
        recordCaptureCopy(capturePoint, [&](const VulkanCaptureHookPayload& source) {
            context.payload = source;
            notifyHook(context);
        });
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
                renderGraph_.skipPass(graphIds_.bloomHook);
                renderGraph_.beginPass(currentCmd, graphIds_.outputTransform);
            }
            {
                VulkanGpuScope outputGpuScope(scheduler, "gpu.output.transform");
                outputPass.record(currentCmd, scheduler.currentFrameIndex(),
                    targets.outputFramebuffer, frameTargets.extent(), manualExposureEv_,
                    static_cast<uint32_t>(outputOperator_),
                    static_cast<uint32_t>(outputTransport_), paperWhiteNits_, peakNits_,
                    selectionOutlineActive_, viewportGridOverlay_);
                if (collectFrameCounters_) {
                    recordPipelineBind(pipelineIdentity(
                        FixedPipelineIdentity::OutputTransform));
                    recordDraw(frameCounters_.drawOutput, 1);
                }
            }
        }
        runCaptureHook(VulkanHookPoint::FinalCaptureHook,
            outputTransport_ == Color::OutputTransport::SdrSrgb
                ? FrameCapturePoint::FinalSdr : FrameCapturePoint::FinalOutput);
    }

    void VulkanVertexBackend::submitUIPass() {
        if (retainedViewsEnabled_) publishRetainedView();
        if (!finalCaptureHookRecorded_) {
            renderGraph_.skipPass(graphIds_.finalCaptureHook);
        }
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.ui");
        renderGraph_.beginPass(currentCmd, graphIds_.ui);
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
            renderGraph_.beginPass(currentCmd, graphIds_.hdr10EncodePresent);
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

    void VulkanVertexBackend::destroyRetainedViews() {
        for (size_t i = 0; i < retainedViewImages_.size(); ++i) {
            if (retainedViewDescriptors_[i]) ImGui_ImplVulkan_RemoveTexture(retainedViewDescriptors_[i]);
            retainedViewDescriptors_[i] = VK_NULL_HANDLE;
            resourceAllocator.destroy(retainedViewImages_[i]);
        }
        if (retainedViewSampler_) vkDestroySampler(vkContext->getDevice(), retainedViewSampler_, nullptr);
        retainedViewSampler_ = VK_NULL_HANDLE;
    }

    void VulkanVertexBackend::prepareRetainedViews(bool enabled, uint32_t renderView) {
        if (renderView >= 2) throw std::out_of_range("Retained view index");
        retainedRenderView_ = renderView;
        retainedViewsEnabled_ = enabled;
        const auto& output = frameTargets.get(0).output;
        const auto& existing = retainedViewImages_[0];
        if (enabled && existing.isValid() && existing.extent.width == output.extent.width &&
            existing.extent.height == output.extent.height && existing.format == output.format) return;
        if (!enabled && !existing.isValid()) return;
        scheduler.waitForAllFrames();
        destroyRetainedViews();
        if (!enabled) return;
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(vkContext->getDevice(), &sampler, nullptr, &retainedViewSampler_) != VK_SUCCESS)
            throw std::runtime_error("Retained view sampler allocation failed");
        try {
            for (size_t i = 0; i < retainedViewImages_.size(); ++i) {
                retainedViewImages_[i] = resourceAllocator.createImage2D(output.extent, output.format,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ProfileMemoryCategory::Texture);
                retainedViewDescriptors_[i] = ImGui_ImplVulkan_AddTexture(retainedViewSampler_,
                    retainedViewImages_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        } catch (...) { destroyRetainedViews(); throw; }
    }

    void* VulkanVertexBackend::getRetainedViewTextureID(uint32_t view) {
        return view < 2 ? reinterpret_cast<void*>(retainedViewDescriptors_[view]) : nullptr;
    }

    void VulkanVertexBackend::publishRetainedView() {
        VulkanCommandList commands(currentCmd);
        for (auto& image : retainedViewImages_) if (image.state == ResourceState::Undefined) {
            commands.transition(image, ResourceState::CopyDestination);
            const VkClearColorValue black{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(currentCmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            commands.transition(image, ResourceState::ShaderResource);
        }
        if (!finalCaptureHookRecorded_) {
            renderGraph_.beginPass(currentCmd, graphIds_.finalCaptureHook);
            finalCaptureHookRecorded_ = true;
        }
        auto& target = retainedViewImages_[retainedRenderView_];
        commands.transition(target, ResourceState::CopyDestination);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {target.extent.width, target.extent.height, 1};
        commands.copyImage(frameTargets.get(scheduler.currentFrameIndex()).output, target, copy);
        commands.transition(target, ResourceState::ShaderResource);
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
