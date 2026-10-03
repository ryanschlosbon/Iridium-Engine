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

    // ==============================================================================
    // 1. SYSTEM LIFECYCLE
    // ==============================================================================

    void VulkanVertexBackend::init(GLFWwindow* window, const RenderBackendConfig& config) {
        if (initialized_) {
            throw std::logic_error("VulkanVertexBackend was initialized more than once.");
        }

        extensionHooks_.configure(config);
        cpuProfiler_ = config.cpuProfiler;
        gBufferLayout_ = config.gBufferLayout;
        opaque_.configure({
            .gBufferLayout = config.gBufferLayout,
            .depthPyramid = config.experimentalDepthPyramid ||
                config.experimentalDepthOcclusionQuery ||
                config.experimentalDepthOcclusionRejection,
            .depthOcclusionQuery = config.experimentalDepthOcclusionQuery ||
                config.experimentalDepthOcclusionRejection,
            .depthOcclusionRejection = config.experimentalDepthOcclusionRejection,
            .lodErrorPixels = config.experimentalGpuLodErrorPixels,
            .lodMaximumLevel = (std::min)(config.gpuLodMaximumLevel,
                MaximumGpuSceneLodLevels - 1u),
            .lodHysteresisFraction = config.gpuLodHysteresisFraction,
            .forceDirectGBufferReference = config.forceDirectGBufferReference,
        });
        oit_.configure(config.weightedOitOrderSeed);
        forceDirectGBufferReference_ = config.forceDirectGBufferReference;
        forceDirectShadowReference_ = config.forceDirectShadowReference;
        experimentalShadowLodErrorTexels_ =
            config.experimentalShadowLodErrorTexels;
        shadowLodMaximumLevel_ = (std::min)(config.shadowLodMaximumLevel,
            MaximumGpuSceneLodLevels - 1u);
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
        output_.configure(static_cast<float>(config.manualExposureEv),
            config.outputOperator);
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
        probes_.configureCaptures(config.reflectionProbeSettings);
        if (std::ranges::any_of(pointShadowCapacities_,
                [](uint32_t value) { return value == 0u; }) ||
            pointShadowCapacities_[0] > kPointShadowPool256Capacity ||
            pointShadowCapacities_[1] > kPointShadowPool512Capacity ||
            pointShadowCapacities_[2] > kPointShadowPool1024Capacity)
            throw std::invalid_argument(
                "Point shadow pool capacity exceeds the GPU table contract");
        telemetry_.init(cpuProfiler_);
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
            shadows_.initVirtualShadows(vkContext->getDevice(), resourceAllocator,
                vkContext->getPhysicalDeviceProperties().limits,
                config.virtualShadowResources);
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

        resources_.init({
            .device = vkContext->getDevice(),
            .allocator = &resourceAllocator,
            .uploads = &uploadContext,
            .scheduler = &scheduler,
            .pipelines = &pipelineLibrary,
            .profiler = cpuProfiler_,
            .frameOpen = &frameOpen_,
        });
        resources_.setEditorDescriptorRelease(this,
            [](void* owner, VkDescriptorSet descriptor) {
                if (static_cast<VulkanVertexBackend*>(owner)->imguiInitialized_)
                    ImGui_ImplVulkan_RemoveTexture(descriptor);
            });
        featureContext_.emplace(VulkanFeatureContext{
            .vk = *vkContext,
            .device = vkContext->getDevice(),
            .allocator = resourceAllocator,
            .uploads = uploadContext,
            .descriptors = descriptorAllocator,
            .scheduler = scheduler,
            .graph = renderGraph_,
            .frameTargets = frameTargets,
            .meshLayouts = meshLayouts,
            .pipelines = pipelineLibrary,
            .resources = resources_,
            .gpuScene = gpuScene_,
            .profiler = cpuProfiler_,
            .telemetry = telemetry_,
            .extensions = extensionHooks_,
            .frameOpen = frameOpen_,
        });
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
            resources_.textureTable().init(vkContext->getDevice(),
                initialCapacity, maximumCapacity);
        } else {
            throw std::runtime_error(
                "Vulkan update-after-bind descriptor limits are below "
                "Iridium's minimum indexed material table.");
        }

        // 2. Lighting and forward pass contracts needed by the shared mesh layouts.
        // R3c.8: the deferred-lighting owner (render pass, pipeline, lighting set).
        lighting_.configure(gBufferLayout_, { &clusterLighting_, &shadows_,
            &localShadows_, &probes_ });
        lighting_.create(*featureContext_);
        clusterLighting_.configure(clusterConfig_, (std::min)(
            static_cast<uint32_t>(
                vkContext->getPhysicalDeviceProperties()
                    .limits.maxStorageBufferRange /
                sizeof(PackedGpuLight)),
            kMaximumGpuLightCapacity));
        clusterLighting_.create(*featureContext_);
        meshLayouts.init(vkContext->getDevice(),
            lighting_.setLayout(),
            resources_.textureTable().materialViewLayout(),
            resources_.textureTable().samplerLayout());
        // R3c.6: the probe owner's capture pass and targets.
        probes_.configure(clusterConfig_, probeViewSettings(), casterScratch_,
            lighting_.setLayout(), clusterLighting_, { this,
                [](void* owner) {
                    auto& self = *static_cast<VulkanVertexBackend*>(owner);
                    if (self.lighting_.sceneSetReady())
                        self.lighting_.bindReflectionProbeBuffers();
                    self.bindGraphImportedBuffers();
                },
                [](void* owner) {
                    static_cast<VulkanVertexBackend*>(owner)->
                        lighting_.bindReflectionProbeEnvironments();
                } });
        probes_.create(*featureContext_);
        forwardPass = std::make_unique<VkForwardRenderPass>(vkContext.get(),
            VulkanSceneColorFormat, VK_FORMAT_D32_SFLOAT);
        transparentPass = std::make_unique<VkForwardRenderPass>(vkContext.get(),
            VulkanSceneColorFormat, VK_FORMAT_D32_SFLOAT, true);
        output_.create(*featureContext_);
        output_.createPipelines(outputTargetFormat_,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            vkSwapchain->getImageFormat());
        oit_.create(*featureContext_);
        hooks_.create(*featureContext_);
        hooks_.setFinalCaptureConsumer({ this,
            [](void* owner, VkCommandBuffer commandBuffer) {
                static_cast<VulkanVertexBackend*>(owner)->initializeRetainedViews(commandBuffer);
            },
            [](void* owner, VkCommandBuffer commandBuffer) {
                static_cast<VulkanVertexBackend*>(owner)->copyRetainedView(commandBuffer);
            } });
        layeredInterfaceCapture_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout(),
            resources_.textureTable().materialViewLayout(),
            resources_.textureTable().samplerLayout());
        layeredLocalComposition_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout(),
            resources_.textureTable().materialViewLayout(),
            resources_.textureTable().samplerLayout(),
            lighting_.setLayout());
        layeredSceneResolve_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout(),
            transparentPass->getRenderPass());
        transparencyPyramid_.init(vkContext->getDevice(),
            descriptorAllocator, meshLayouts.getGlobalSetLayout());
        // R3c.5: the shadow owners create their maps and cullers; the shared
        // 3-binding indirect set layout outlives every view culler.
        indirectCullerSetLayout_ = createIndirectSetLayout(
            vkContext->getDevice(), "directional-shadow");
        shadows_.configure(directionalShadowResolution_, shadowViewSettings(),
            casterScratch_, { cullerServices(), indirectCullerSetLayout_ });
        shadows_.create(*featureContext_);
        probes_.createCuller({ cullerServices(), indirectCullerSetLayout_ });
        localShadows_.configure(spotShadowAtlasResolution_, pointShadowCapacities_,
            shadowViewSettings(), casterScratch_,
            { cullerServices(), indirectCullerSetLayout_ });
        localShadows_.create(*featureContext_);

        // 3. G-Buffer Pass (R3c.7: the opaque owner, with its culler and the
        // depth pyramid).
        opaque_.setCullerServices(cullerServices());
        opaque_.create(*featureContext_);

        pipelineLibrary.init(vkContext->getDevice(),
            { opaque_.gBufferRenderPass(), meshLayouts.getGBufferPipelineLayout(),
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
            output_.rebuildHdr10Targets(vkSwapchain->getImageViews(),
                vkSwapchain->getExtent());
        }
        // Target descriptors declare shader-read layouts, so submit their initial
        // Undefined -> ShaderResource transitions before any descriptor or ImGui
        // registration can reference those images.
        lighting_.createNeutralEnvironment();
        uploadContext.flush();

        // 8. Global Camera Buffers
        view_.createBuffers(resourceAllocator);
        resources_.setMaterialTableMaximumCapacity((std::min)(
            static_cast<uint32_t>(
                vkContext->getPhysicalDeviceProperties()
                    .limits.maxStorageBufferRange /
                sizeof(PackedGpuMaterial)),
            MaterialHandle::MaxIndex + 1u));
        resources_.createCanonicalMaterialBuffers(
            (std::min)(DesiredCapacity,
                resources_.materialTableMaximumCapacity()));
        if (clusterLighting_.lightRecordMaximumCapacity() == 0) {
            throw std::runtime_error(
                "Vulkan storage-buffer range cannot hold one GPU light record");
        }
        clusterLighting_.createLightRecordBuffers((std::min)(kInitialGpuLightCapacity,
            clusterLighting_.lightRecordMaximumCapacity()));
        const uint64_t storageRange = vkContext->getPhysicalDeviceProperties()
            .limits.maxStorageBufferRange;
        gpuScene_.init(vkContext->getDevice(), resourceAllocator, scheduler,
            cpuProfiler_, frameOpen_, storageRange);
        gpuScene_.createBuffers({ 2u, 1u, 1u, 1u });
        opaque_.culler().resize(512u, frameOpen_);
        probes_.createInitialBuffers(sceneExtent_);

        // --------------------------------

        // 2. Global Descriptor Sets (Camera Data)
        for (uint32_t i = 0; i < VulkanFrameScheduler::FramesInFlight; i++) {
            view_.allocateSet(i, descriptorAllocator, meshLayouts.getGlobalSetLayout());
            gpuScene_.setDescriptorSet(i,
                descriptorAllocator.allocate(meshLayouts.getGpuSceneSetLayout()));
            opaque_.culler().allocateSet(i);
            view_.writeSet(i, vkContext->getDevice());
        }
        gpuScene_.bindBuffers();
        opaque_.culler().bindBuffers();
        transparencyPyramid_.rebuild(frameTargets);
        opaque_.rebuildDescriptors();
        layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
        layeredLocalComposition_.rebuildDescriptors(frameTargets);
        layeredSceneResolve_.rebuildDescriptors(frameTargets);
        oit_.rebuildDescriptors();

        // 3. Lighting descriptors (one set per frame context).
        const uint32_t imgCount = vkSwapchain->getImageCount();
        lighting_.rebuildSceneSet();
        output_.rebuildDescriptors();

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
        registerEditorTargetTextures();

        initialized_ = true;
        cleaned_ = false;
        extensionHooks_.onBackendInitialized(backendServices());
    }

    void VulkanVertexBackend::attachExtension(IRenderBackendExtension* extension) {
        extensionHooks_.attach(extension, initialized_);
    }

    VulkanBackendServices VulkanVertexBackend::backendServices() noexcept {
        return {
            .device = vkContext->getDevice(),
            .allocator = &resourceAllocator,
            .scheduler = &scheduler,
            .graph = &renderGraph_,
            .frameTargets = &frameTargets,
            .probeCaptureTargets = &probes_.captureTargets(),
            .depthPyramid = opaque_.depthPyramidEnabled() ? &opaque_.depthPyramid() : nullptr,
            .profiler = cpuProfiler_,
        };
    }

    void VulkanVertexBackend::setEnvironmentLighting(
        const EnvironmentLightingHandles& environment) {
        lighting_.setEnvironment(environment);
    }

    void VulkanVertexBackend::setEnvironmentLightingSettings(
        const EnvironmentLightingSettings& settings) {
        lighting_.setEnvironmentSettings(settings);
    }

    void VulkanVertexBackend::setOutputTransformLut(TextureHandle lutHandle) {
        VulkanTexturePayload* payload = resources_.textures().get(lutHandle);
        if (payload == nullptr || payload->retired ||
            payload->format != TextureFormat::RGBA32_SFloat ||
            payload->width != 16384 || payload->height != 128) {
            throw std::invalid_argument(
                "ACES 2 output LUT must be the pinned 128^3 RGBA32F asset.");
        }
        scheduler.waitForAllFrames();
        for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            shadows_.collectVirtualShadowRequests(frame);
        output_.setLut(lutHandle);
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
        output_.setManualExposure(manualExposureEv);
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
            opaque_.culler().collect(frame);
        for (VulkanIndirectViewCuller* culler : indirectViewCullers())
            for (uint32_t frame = 0;
                    frame < VulkanFrameScheduler::FramesInFlight; ++frame)
                culler->collect(frame);
        extensionHooks_.onBeforeDeviceDestroy();

        pipelineLibrary.cleanup();

        releaseEditorTargetTextures();
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

        lighting_.releaseSceneSet();
        transparencyPyramid_.clearDescriptors();
        opaque_.clearDescriptors();
        layeredInterfaceCapture_.clearDescriptors();
        layeredLocalComposition_.clearDescriptors();
        layeredSceneResolve_.clearDescriptors();
        oit_.clearDescriptors();
        frameTargets.cleanup();
        renderGraph_.cleanupAfterDeviceIdle();
        shadows_.destroy();
        localShadows_.destroy();
        probes_.destroy();

        if (indirectCullerSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device,
                indirectCullerSetLayout_, nullptr);
            indirectCullerSetLayout_ = VK_NULL_HANDLE;
        }

        resources_.destroyResources();

        view_.destroy(resourceAllocator);
        gpuScene_.destroy();
        opaque_.destroy();


        forwardPass.reset();
        transparentPass.reset();

        lighting_.destroy();

        uiPass.reset();
        output_.destroy();

        transparencyPyramid_.cleanup();
        layeredSceneResolve_.cleanup();
        layeredLocalComposition_.cleanup();
        layeredInterfaceCapture_.cleanup();
        oit_.destroy();
        hooks_.destroy();

        meshLayouts.cleanup();
        resources_.textureTable().cleanup();
        clusterLighting_.destroy();
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
        telemetry_.cleanup();
        requestedOutputTransport_ = Color::OutputTransport::SdrSrgb;
        outputTransport_ = Color::OutputTransport::SdrSrgb;
        outputTargetFormat_ = VulkanSdrOutputFormat;
        resources_.reset();
        gpuScene_.reset();
        featureContext_.reset();
    }

    void VulkanVertexBackend::emitFrameCounters() {
        if (!telemetry_.collecting() || cpuProfiler_ == nullptr) {
            return;
        }
        const VulkanIndexedTextureTable& table = resources_.textureTable();
        telemetry_.emit({
            .weightedOitResident = weightedOitResidency_.enabled(),
            .weightedOitOrderSeed = oit_.orderSeed(),
            .refractionPyramidsResident = transparencyPyramidResidency_.enabled(),
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

    void VulkanVertexBackend::bindMaterialDescriptors(
        VkPipelineLayout layout) {
        resources_.bindMaterialDescriptors(currentCmd,
            scheduler.currentFrameIndex(), layout);
    }

    VulkanProductionGraphFeatures
        VulkanVertexBackend::productionGraphFeatures() const noexcept {
        return {
            .depthPyramid = opaque_.depthPyramidEnabled(),
            .virtualShadowWorkingSetBytes = shadows_.virtualShadows().initialized()
                ? shadows_.virtualShadows().info().workingSetLayout.totalBytes : 0,
            // The CPU profiler's enabled state is fixed for the process.
            .clusterTelemetryReadback =
                cpuProfiler_ != nullptr && cpuProfiler_->isEnabled(),
            .hooks = extensionHooks_.graphHooks(),
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
            graphIds_.shadowDirectionalMap, shadows_.map().image(),
            Access::SampledRead, shadowPolicy);
        renderGraph_.bindExternalImage(VulkanGlobalBinding,
            graphIds_.shadowSpotMap, localShadows_.spot().image(), Access::SampledRead,
            shadowPolicy);
        for (uint32_t tier = 0; tier < graphIds_.shadowPointMaps.size(); ++tier)
            renderGraph_.bindExternalImage(VulkanGlobalBinding,
                graphIds_.shadowPointMaps[tier], localShadows_.point().image(tier),
                Access::SampledRead, shadowPolicy);
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
            { graphIds_.directionalIndirect.commands, &shadows_.culler().buffers().commands },
            { graphIds_.directionalIndirect.counts, &shadows_.culler().buffers().counts },
            { graphIds_.spotIndirect.commands, &localShadows_.spotCuller().buffers().commands },
            { graphIds_.spotIndirect.counts, &localShadows_.spotCuller().buffers().counts },
            { graphIds_.pointIndirect.commands, &localShadows_.pointCuller().buffers().commands },
            { graphIds_.pointIndirect.counts, &localShadows_.pointCuller().buffers().counts },
            { graphIds_.opaqueIndirect.commands, &opaque_.culler().buffers().commands },
            { graphIds_.opaqueIndirect.counts, &opaque_.culler().buffers().counts },
            { graphIds_.probeClusterHeaders, &probes_.clusterHeaderBuffers() },
            { graphIds_.probeClusterIndices, &probes_.clusterIndexBuffers() },
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
        // R3c: feature owners re-query graph resources and register their
        // callbacks on the new plan (a rebuild cleared every registration).
        // The shadow owner binds the VSM working set here (R3c.5).
        if (featureContext_) {
            for (IVulkanFeature* feature : features()) {
                feature->onGraphRebuilt(graphIds_);
                feature->registerPasses(renderGraph_);
            }
        }
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
        const auto releaseTargets = [&] { releaseFrameTargets(); };
        const auto createTargets = [&] {
            createFrameTargets();
            registerEditorTargetTextures();
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
            oit_.setInstanceCapacity(weightedOitResidency_.enabled()
                ? kWeightedOitMaximumInstanceCount : 0u);
            ordinary2AtlasExtent_ = nextOrdinary2AtlasExtent;
            hero4AtlasExtent_ = nextHero4AtlasExtent;
            cinematic8AtlasExtent_ = nextCinematic8AtlasExtent;
            createTargets();
            if (telemetry_.collecting())
                ++telemetry_.counters().transparencyPyramidTopologyRebuilds;
        }
        catch (const std::exception& exception) {
            if (telemetry_.collecting())
                ++telemetry_.counters().transparencyPyramidTopologyRebuildFailures;
            try {
                CpuScope restoreScope(cpuProfiler_,
                    "cpu.renderer.transparency_topology_restore");
                releaseTargets();
                transparencyPyramidResidency_.restore(previousEnabled);
                ordinary2AtlasResidency_.restore(previousOrdinary2Enabled);
                hero4AtlasResidency_.restore(previousHero4Enabled);
                cinematic8AtlasResidency_.restore(previousCinematic8Enabled);
                weightedOitResidency_.restore(previousWeightedOitEnabled);
                oit_.setInstanceCapacity(previousWeightedOitEnabled
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
        opaque_.culler().lodHistory().resetView();
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

        // Owners with descriptor sets over transient graph resources retire
        // them before the graph is rebuilt, and recreate them afterward.
        releaseFrameTargets();
        output_.destroyPipelines();
        uiPass.reset();

        vkSwapchain = std::move(candidate);
		requestedOutputTransport_ = requestedTransport;
		outputTransport_ = candidateTransport;
		outputTargetFormat_ = candidateOutputFormat;
        vkSwapchain->setHdrMetadata(peakNits_);
        output_.createPipelines(outputTargetFormat_,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            vkSwapchain->getImageFormat());
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
        // The replacement target images are referenced by descriptor sets and
        // ImGui immediately below; createFrameTargets establishes their
        // declared layouts (upload flush) first.
        createFrameTargets();
        if (newImageCount != oldImageCount) {
            ImGui_ImplVulkan_SetMinImageCount(newImageCount);
        }
        registerEditorTargetTextures();
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
            if (extent.width == 0 || extent.height == 0) opaque_.culler().lodHistory().resetView();
            diagnostic = "Requested scene extent is outside Vulkan image limits";
            return false;
        }
        const VkExtent2D requested{ extent.width, extent.height };
        if (requested.width == sceneExtent_.width &&
            requested.height == sceneExtent_.height) {
            return true;
        }

        opaque_.culler().lodHistory().resetView();

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
        const auto releaseTargets = [&] { releaseFrameTargets(); };
        const auto createTargets = [&] {
            createFrameTargets();
            registerEditorTargetTextures();
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
        const LightingUploadTelemetry lightUploads = clusterLighting_.uploadTelemetry();
        info.gpuLightCapacity = lightUploads.capacity;
        info.gpuLightActiveCount = lightUploads.activeLights;
        info.gpuLightUploadBytes = lightUploads.bytes;
        info.gpuLightUploadRanges = lightUploads.ranges;
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
        return resources_.allocateGeometry(desc, vertexBytes, indexBytes);
    }

    void VulkanVertexBackend::freeGeometry(GeometryHandle handle) {
        resources_.freeGeometry(handle);
    }

    GeometryArenaAllocation VulkanVertexBackend::allocateGeometryArena(
        uint32_t vertexStride,
        std::span<const std::byte> vertexBytes,
        const GeometryArenaData& arena) {
        return resources_.allocateGeometryArena(vertexStride, vertexBytes, arena);
    }

    void VulkanVertexBackend::freeGeometryArena(
        std::span<const GeometryHandle> primitiveGeometry) {
        resources_.freeGeometryArena(primitiveGeometry);
    }

    TextureHandle VulkanVertexBackend::allocateTexture(const TextureDesc& desc,
        std::span<const std::byte> pixelBytes) {
        return resources_.allocateTexture(desc, pixelBytes);
    }

    void VulkanVertexBackend::freeTexture(TextureHandle handle) {
        resources_.freeTexture(handle);
    }

    MaterialBinding VulkanVertexBackend::allocateCanonicalMaterial(
        const CanonicalMaterialAsset& asset) {
        return resources_.allocateCanonicalMaterial(asset);
    }

    void VulkanVertexBackend::updateCanonicalMaterial(MaterialHandle handle,
        const PackedGpuMaterial& material) {
        resources_.updateCanonicalMaterial(handle, material);
    }

    void VulkanVertexBackend::freeMaterial(MaterialHandle handle) {
        resources_.freeMaterial(handle);
    }

    // --- PRIVATE HELPERS ---

    void VulkanVertexBackend::releaseEditorTargetTextures() {
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
    }

    void VulkanVertexBackend::registerEditorTargetTextures() {
        uiSceneTextures.resize(frameTargets.size());
        uiDepthTextures.resize(frameTargets.size());
        for (size_t index = 0; index < frameTargets.size(); ++index) {
            const VulkanFrameContextTargets& targets = frameTargets.get(index);
            uiSceneTextures[index] = ImGui_ImplVulkan_AddTexture(
                frameTargets.sampler(), targets.output.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            const VkImageView editorDepthView = targets.depth.view;
            uiDepthTextures[index] = ImGui_ImplVulkan_AddTexture(
                frameTargets.sampler(), editorDepthView,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }
    }

    void VulkanVertexBackend::releaseFrameTargets() {
        // Only between frames, after every slot retired: shared descriptor
        // sets and scene targets must be unreferenced.
        releaseEditorTargetTextures();
        lighting_.releaseSceneSet();
        for (IVulkanFeature* feature : features()) feature->onGraphReleased();
        transparencyPyramid_.clearDescriptors();
        opaque_.clearDescriptors();
        layeredInterfaceCapture_.clearDescriptors();
        layeredLocalComposition_.clearDescriptors();
        layeredSceneResolve_.clearDescriptors();
        oit_.clearDescriptors();
        frameTargets.cleanup();
        renderGraph_.cleanupAfterDeviceIdle();
    }

    void VulkanVertexBackend::createFrameTargets() {
        rebuildRenderGraphAfterDeviceIdle();
        initFrameTargets();
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq) {
            output_.rebuildHdr10Targets(vkSwapchain->getImageViews(),
                vkSwapchain->getExtent());
        }
        // Establish the targets' declared layouts before descriptors (and
        // the editor) reference them.
        uploadContext.flush();
        lighting_.rebuildSceneSet();
        transparencyPyramid_.rebuild(frameTargets);
        opaque_.rebuildDescriptors();
        layeredInterfaceCapture_.rebuildDescriptors(frameTargets);
        layeredLocalComposition_.rebuildDescriptors(frameTargets);
        layeredSceneResolve_.rebuildDescriptors(frameTargets);
        oit_.rebuildDescriptors();
        output_.rebuildDescriptors();
    }

    void VulkanVertexBackend::initFrameTargets() {
        frameTargets.init(vkContext->getDevice(), *vkSwapchain, sceneExtent_,
            { opaque_.gBufferRenderPass(), lighting_.renderPass(),
                forwardPass->getRenderPass(), transparentPass->getRenderPass(),
                layeredInterfaceCapture_.renderPass(),
                layeredLocalComposition_.renderPass(),
                oit_.accumulationRenderPass(),
                oit_.resolveRenderPass(), output_.outputRenderPass(),
                uiPass->getRenderPass() },
            VulkanFrameScheduler::FramesInFlight,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            transparencyPyramidResidency_.enabled(),
            VulkanLayeredGraphConfig{ ordinary2AtlasExtent_,
                hero4AtlasExtent_, cinematic8AtlasExtent_,
                weightedOitResidency_.enabled() },
            renderGraph_, graphIds_);
    }

    // ==============================================================================
    // 3. THE FRAME PIPELINE (Data-Driven Execution)
    // ==============================================================================

    FrameStatus VulkanVertexBackend::beginFrame() {
        frameOpen_ = false;
        opaque_.beginFrame();
        probes_.beginFrame();
        ordinary2ViewProjectionValid_ = false;
        telemetry_.beginFrame();
        CpuScope beginFrameScope(cpuProfiler_, "cpu.renderer.begin_frame");
        uploadContext.flush();
        applyTransparencyPyramidTopologyChange();
        const uint32_t completedFrameIndex = scheduler.currentFrameIndex();
        const VulkanFrameBegin frame = scheduler.beginFrame(vkSwapchain->getSwapchain());
        const uint32_t frameSlot = scheduler.currentFrameIndex();
        // beginFrame has waited this slot's fence before returning, including
        // the out-of-date acquire path. Its capture readbacks are now CPU-safe.
        extensionHooks_.onFrameSlotRetired(completedFrameIndex);
        shadows_.collectVirtualShadowRequests(completedFrameIndex);
        opaque_.onFrameFenceCompleted(completedFrameIndex, scheduler.completedSerial());
        for (IVulkanFeature* feature : features())
            feature->onFrameSlotRetired(completedFrameIndex);
        opaque_.culler().collect(completedFrameIndex);
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

        if (resources_.textureTable().active()) {
            resources_.textureTable().ensureFrameCapacity(
                scheduler.currentFrameIndex(),
                resources_.textureTable().requiredCapacity());
            resources_.textureTable().synchronizeFrame(
                scheduler.currentFrameIndex());
        }
        resources_.uploadCanonicalMaterialsForFrame(scheduler.currentFrameIndex());
        currentImageIndex = frame.imageIndex;
        currentCmd = frame.commandBuffer;
        lighting_.bindFrameEnvironment(scheduler.currentFrameIndex());
        opaque_.bindHistory(false);
        renderGraph_.beginFrameExecution(scheduler.currentFrameIndex());
        renderGraph_.setFrameRecordContext({
            .commandBuffer = currentCmd,
            .frameIndex = scheduler.currentFrameIndex(),
            .imageIndex = currentImageIndex,
            .collectCounters = telemetry_.collecting(),
            .sceneExtent = sceneExtent_,
        });
        frameOpen_ = true;
        return frame.status;
    }

    VulkanIndirectScene VulkanVertexBackend::indirectScene(
        uint32_t frame) const noexcept {
        return gpuScene_.indirectScene(frame);
    }

    VulkanIndirectAssetResolver VulkanVertexBackend::indirectAssets() const noexcept {
        return vulkanIndirectAssets(*featureContext_);
    }

    VulkanIndirectViewSettings VulkanVertexBackend::shadowViewSettings() const noexcept {
        return {
            .lodErrorThreshold = experimentalShadowLodErrorTexels_,
            .lodMaximumLevel = shadowLodMaximumLevel_,
            .forceDirectGBufferReference = forceDirectGBufferReference_,
            .forceDirectShadowReference = forceDirectShadowReference_,
        };
    }

    uint64_t VulkanVertexBackend::getShadowCasterRevision(
        const ShadowCasterSubmission& shadowCasters) const noexcept {
        return shadowCasterRevision(indirectScene(scheduler.currentFrameIndex()),
            resources_, shadowCasters);
    }

    std::array<uint64_t, kDirectionalShadowCascadeCount>
        VulkanVertexBackend::getDirectionalShadowCasterRevisions(
            const ShadowCasterSubmission& shadowCasters,
            const DirectionalShadowCascadePlan& plan) const noexcept {
        return directionalShadowCasterRevisions(
            indirectScene(scheduler.currentFrameIndex()), resources_,
            shadowCasters, plan);
    }

    void VulkanVertexBackend::prepareDepthPyramidHistory(
        std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> opaqueForwardQueue) {
        if (!frameOpen_)
            throw std::logic_error(
                "Depth-pyramid history preparation requires an open frame");
        opaque_.prepareDepthHistory(opaqueQueue, opaqueForwardQueue);
    }

    void VulkanVertexBackend::submitDirectionalShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const DirectionalShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error(
                "Directional shadows require an open frame");
        // R3c.5 drain point: clip upload, compaction and cascades.
        shadows_.submit(shadowCasters, shadows);
    }

    void VulkanVertexBackend::submitSpotShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const SpotShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error("Spot shadows require an open frame");
        localShadows_.submitSpot(shadowCasters, shadows, clusterLighting_);
    }

    void VulkanVertexBackend::submitPointShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const PointShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error("Point shadows require an open frame");
        localShadows_.submitPoint(shadowCasters, shadows, clusterLighting_);
    }

    void VulkanVertexBackend::submitReflectionProbeCaptures(
        const ReflectionProbeCasterSubmission& probeCasters,
        std::span<const ReflectionProbeCaptureScheduleEntry> captures,
        const LightingFramePacket& lights) {
        if (!frameOpen_)
            throw std::logic_error(
                "Reflection-probe capture requires an open frame");
        // R3c.6 drain point: "probe.capture" (reads the shadow maps; staging
        // and the per-face compaction keep their barriers inside the pass).
        probes_.submitCaptures(probeCasters, captures, lights,
            lighting_.sceneSet(scheduler.currentFrameIndex()));
    }

    void VulkanVertexBackend::submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> selectionQueue, bool isWireframe) {
        if (opaque_.depthPyramidEnabled() && !opaque_.historyPrepared())
            throw std::logic_error(
                "Depth-pyramid history must be prepared before opaque submission");
        selectionOutlineActive_ = !selectionQueue.empty();
        // Frames that never submit probe captures (asset preview) skip the
        // declared pass before the opaque compaction.
        probes_.skipCaptureIfUnhandled();
        // R3c.7 drain point: "gpu-scene.opaque.compact" and "gbuffer".
        opaque_.submit({
            .opaqueQueue = opaqueQueue,
            .selectionQueue = selectionQueue,
            .wireframe = isWireframe,
            .globalSet = view_.globalSet(scheduler.currentFrameIndex()),
            .debugView = debugView_,
        });
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
            .oracle = extensionHooks_.indirectOracle(),
            .streamObserver = activeIndirectStreamObserver(),
            .sceneOwner = this,
            .scene = [](const void* owner, uint32_t slot) {
                return static_cast<const VulkanVertexBackend*>(owner)->
                    indirectScene(slot);
            },
            .maximumPrimitiveCapacity = MaximumOpaqueIndirectCommandCapacity,
        };
    }

    VulkanIndirectViewSettings VulkanVertexBackend::probeViewSettings() const noexcept {
        return {
            .lodErrorThreshold = experimentalProbeLodErrorPixels_,
            .lodMaximumLevel = probeLodMaximumLevel_,
            .forceDirectGBufferReference = forceDirectGBufferReference_,
            .forceDirectShadowReference = forceDirectShadowReference_,
        };
    }

    void VulkanVertexBackend::collectIndirectViewValidations(uint32_t frameIndex) {
        for (VulkanIndirectViewCuller* culler : indirectViewCullers())
            culler->collect(frameIndex);
    }

    std::optional<uint32_t>
    VulkanVertexBackend::capturedReflectionProbeEnvironmentSlot(
        SceneEntityUuid owner) const noexcept {
        return probes_.capturedEnvironmentSlot(owner);
    }

    void VulkanVertexBackend::synchronizeReflectionProbeCaptureOwners(
        std::span<const SceneEntityUuid> owners) {
        probes_.synchronizeCaptureOwners(owners);
    }

    void VulkanVertexBackend::configureReflectionProbeCaptures(
        const ProjectReflectionProbeSettings& settings) {
        probes_.configureCaptures(settings);
    }

    std::vector<ReflectionProbeCaptureCompletion>
    VulkanVertexBackend::finalizeReflectionProbeCaptures() {
        return probes_.finalizeCaptures();
    }

    void VulkanVertexBackend::prepareReflectionProbes(
        uint32_t requiredCapacity,
        std::span<const EnvironmentLightingHandles> environments) {
        if (!initialized_ || cleaned_)
            throw std::logic_error("Vulkan backend is not initialized");
        if (frameOpen_)
            throw std::logic_error(
                "Reflection probes must be prepared before beginFrame");
        probes_.prepare(requiredCapacity, environments, sceneExtent_);
    }

    void VulkanVertexBackend::prepareLighting(uint32_t requiredCapacity) {
        if (!initialized_ || cleaned_) {
            throw std::logic_error("Vulkan backend is not initialized");
        }
        if (frameOpen_) {
            throw std::logic_error(
                "Lighting capacity must be prepared before beginFrame");
        }
        if (clusterLighting_.prepare(requiredCapacity) && lighting_.sceneSetReady())
            lighting_.bindLightBuffers();
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
        gpuScene_.prepare(requirements);
        const uint32_t desiredIndirectCapacity = (std::min)(
            (std::max)(requirements.primitives, 1u),
            MaximumOpaqueIndirectCommandCapacity);
        if (desiredIndirectCapacity > opaque_.culler().commandCapacity()) {
            const uint32_t grownCapacity = nextMaterialTableCapacity(
                opaque_.culler().commandCapacity(), desiredIndirectCapacity,
                MaximumOpaqueIndirectCommandCapacity);
            opaque_.culler().resize(grownCapacity, frameOpen_);
            shadows_.growIndirectCapacity(grownCapacity);
            localShadows_.growIndirectCapacity(grownCapacity);
            probes_.growIndirectCapacity(grownCapacity);
            bindGraphImportedBuffers();
        }
    }

    void VulkanVertexBackend::publishGpuScene(
        const GpuScenePackedTables& scene) {
        if (!frameOpen_) {
            throw std::logic_error(
                "GPU-scene publication requires an acquired frame context");
        }
        gpuScene_.validatePublication(scene);
        CpuScope uploadScope(cpuProfiler_, "cpu.gpu_scene.upload");
        opaque_.publishScene(scene);
        gpuScene_.publish(scene, scheduler.currentFrameIndex());
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter("gpu_scene.lod.history_buffer_bytes",
                opaque_.culler().lodHistoryBufferBytes(),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        }
    }

    void VulkanVertexBackend::updateCamera(const ViewTransportRecord& view, ViewHistoryContext history) {
        opaque_.updateView(view, history);
        gpuScene_.views()[scheduler.currentFrameIndex()] = view;
        view_.update(scheduler.currentFrameIndex(), view,
            transparencyPyramidResidency_.enabled(), debugView_);
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
        clusterLighting_.uploadFrame(frameIndex, view, proj, nearPlane, farPlane,
            lights, sceneExtent_, lighting_.environmentSettings());
        probes_.uploadFrame(frameIndex, view, proj, nearPlane, farPlane,
            reflectionProbes, sceneExtent_);
        const ClusterGridDimensions probeDimensions = clusterGridDimensions(
            clusterConfig_, { sceneExtent_.width, sceneExtent_.height,
                nearPlane, farPlane, view, proj });
        // R3c.1 drain points: "lighting.probe-cluster" and the cluster build
        // run here, where they were recorded imperatively.
        clusterLighting_.recordProbeCluster(
            static_cast<uint32_t>(probeDimensions.clusterCount()));
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            clusterConfig_,
            { sceneExtent_.width, sceneExtent_.height, nearPlane, farPlane,
                view, proj });
        clusterLighting_.recordClusters(frameIndex,
            static_cast<uint32_t>(dimensions.clusterCount()),
            lights.stats.activeLightCount);
        // R3c.8 drain point: "lighting".
        lighting_.record({
            .cameraPosition = cameraPos,
            .view = view,
            .projection = proj,
            .debugView = debugView_,
        });
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
        telemetry_.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredInterfaceCapture));
        const VkDescriptorSet globalSet = view_.globalSet(
            scheduler.currentFrameIndex());
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
                resources_.geometries().get(packet.geometry);
            if (geometry == nullptr || resources_.materials().get(packet.material) == nullptr)
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
                telemetry_.recordDraw(telemetry_.counters().ordinary2CaptureExitDraws,
                    packet.indexCount / 3u);
            }
            else {
                telemetry_.recordDraw(telemetry_.counters().ordinary2CaptureEntryDraws,
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
        telemetry_.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredInterfaceCapture));
        const VkDescriptorSet globalSet = view_.globalSet(frameIndex);
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
                resources_.geometries().get(packet.geometry);
            if (geometry == nullptr ||
                resources_.materials().get(packet.material) == nullptr) {
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
            telemetry_.recordDraw(telemetry_.counters().deepLayeredInterfaceDraws,
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
        telemetry_.recordPipelineBind(pipelineIdentity(
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
        const VkDescriptorSet globalSet = view_.globalSet(frameIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        bindMaterialDescriptors(layout);
        const VkDescriptorSet sceneSet = lighting_.sceneSet(frameIndex);
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
        telemetry_.recordPipelineBind(pipelineIdentity(
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
                resources_.geometries().get(packet.geometry);
            if (geometry == nullptr ||
                resources_.materials().get(packet.material) == nullptr) {
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
            telemetry_.recordDraw(telemetry_.counters().deepLayeredResidualProbeDraws,
                packet.indexCount / 3u);
        }
        if (residualQueryActive)
            scheduler.endLayeredResidualQuery(residualQuerySlot);

        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layeredLocalComposition_.deepPipeline());
        telemetry_.recordPipelineBind(pipelineIdentity(
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
                    resources_.geometries().get(packet.geometry);
                if (geometry == nullptr ||
                    resources_.materials().get(packet.material) == nullptr) {
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
                telemetry_.recordDraw(telemetry_.counters().deepLayeredLocalCompositionDraws,
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
        telemetry_.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredLocalComposition));
        const VkDescriptorSet globalSet = view_.globalSet(frameIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        bindMaterialDescriptors(layout);
        const VkDescriptorSet sceneSet = lighting_.sceneSet(frameIndex);
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
                resources_.geometries().get(packet.geometry);
            if (geometry == nullptr ||
                resources_.materials().get(packet.material) == nullptr)
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
            telemetry_.recordDraw(telemetry_.counters().ordinary2LocalCompositionDraws,
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
        telemetry_.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredSceneResolve));
        const VkDescriptorSet globalSet = view_.globalSet(frameIndex);
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
                resources_.geometries().get(packet.geometry);
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
            telemetry_.recordDraw(telemetry_.counters().ordinary2SceneResolveDraws,
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
        telemetry_.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredSceneResolve));
        const VkDescriptorSet globalSet = view_.globalSet(frameIndex);
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
                resources_.geometries().get(packet.geometry);
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
            telemetry_.recordDraw(telemetry_.counters().deepLayeredSceneResolveDraws,
                packet.indexCount / 3u);
            ++sceneResolveDrawCounts[layeredQualityTierIndex(draw.quality)];
        }
        vkCmdEndRenderPass(currentCmd);
        scheduler.endGpuRange(gpuRange);
        extensionHooks_.notify({ .point = VulkanHookPoint::DeepLayeredResolveCounts,
            .cmd = currentCmd, .slot = frameIndex,
            .payload = VulkanDeepResolveCountsPayload{ sceneResolveDrawCounts } });
    }

    void VulkanVertexBackend::recordDeepLayeredValidationHook(
        std::span<const LayeredCaptureDraw> draws,
        TransparencyQuality quality) {
        if (!extensionHooks_.graphHooks().layeredValidation) return;
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
        hooks_.runPassHook(quality == TransparencyQuality::Hero4
                ? VulkanHookPasses::PassHook::Hero4Validation
                : VulkanHookPasses::PassHook::Cinematic8Validation,
            { .point = VulkanHookPoint::DeepLayeredValidation,
                .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                .payload = VulkanDeepLayeredHookPayload{ quality, interfaceCount,
                    drawCount, static_cast<uint32_t>(
                        deepLayeredAtlasPlan_.workIdentities().size()) } });
    }

    void VulkanVertexBackend::submitForwardQueues(
        std::span<const DrawPacket> opaqueForwardQueue,
        std::span<const DrawPacket> sortedSurfaceQueue,
        std::span<const DrawPacket> compatibilityTransparentQueue,
        std::span<const glm::mat4> instanceTransforms) {
        if (!opaqueForwardQueue.empty()) {
            telemetry_.counters().opaqueIndirectFallbackPackets +=
                opaqueForwardQueue.size();
            if (telemetry_.counters().opaqueIndirectFallbackReason == 0u) {
                telemetry_.counters().opaqueIndirectFallbackReason =
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
            weightedOitInstanceCount <= oit_.instanceCapacity();
        if (telemetry_.collecting()) {
            telemetry_.counters().weightedOitPackets = weightedOitPacketCount;
            telemetry_.counters().weightedOitSortedFallbackPackets =
                weightedOitExecutionEnabled ? 0u : weightedOitPacketCount;
            telemetry_.counters().weightedOitInstanceCapacityFallbackPackets =
                weightedOitResidency_.enabled() &&
                    !weightedOitExecutionEnabled
                ? weightedOitPacketCount : 0u;
        }
        bool ordinary2PreparedThisFrame = false;
        // Active Ordinary2 topology prepares the fixed-capacity draw plan every
        // frame. When inactive, profiler frames retain the earlier demand probe
        // without changing topology or recording commands.
        if ((ordinary2CaptureTopologyActive || telemetry_.collecting()) &&
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
            if (telemetry_.collecting()) {
                const Ordinary2RequestCollectionStats& collection =
                    ordinary2RequestCollector_.stats();
                const Ordinary2AtlasStats& atlas = ordinary2AtlasPlan_.stats();
                const Ordinary2CaptureDrawStats& capture =
                    ordinary2CaptureDrawPlan_.stats();
                ++telemetry_.counters().ordinary2ProbeFrames;
                telemetry_.counters().ordinary2CandidatePackets =
                    collection.candidatePacketCount;
                telemetry_.counters().ordinary2ProjectedPackets =
                    collection.projectedPacketCount;
                telemetry_.counters().ordinary2ProjectionCulledPackets =
                    collection.culledPacketCount;
                telemetry_.counters().ordinary2InvalidBoundsFallbackPackets =
                    collection.invalidBoundsFallbackCount;
                telemetry_.counters().ordinary2NearPlaneFallbackPackets =
                    collection.nearPlaneFallbackCount;
                telemetry_.counters().ordinary2UnsafeProjectionFallbackPackets =
                    collection.unsafeProjectionFallbackCount;
                telemetry_.counters().ordinary2RequestCapacityFallbackPackets =
                    collection.requestCapacityFallbackCount;
                telemetry_.counters().ordinary2AtlasAcceptedPackets =
                    atlas.acceptedPacketCount;
                telemetry_.counters().ordinary2AtlasAcceptedIslands =
                    atlas.acceptedIslandCount;
                telemetry_.counters().ordinary2AtlasRejectedPackets =
                    atlas.requestCount - atlas.acceptedPacketCount;
                telemetry_.counters().ordinary2AtlasAllocatedTexels =
                    atlas.allocatedTexelCount;
                telemetry_.counters().ordinary2CapturePreparedDraws =
                    capture.preparedDrawCount;
                telemetry_.counters().ordinary2CapturePreparationFallbackPackets =
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
            if (telemetry_.collecting()) {
                const LayeredRequestCollectionStats& collection =
                    deepLayeredRequestCollector_.stats();
                const LayeredAtlasStats& atlas =
                    deepLayeredAtlasPlan_.stats();
                const LayeredCaptureDrawStats& capture =
                    deepLayeredCaptureDrawPlan_.stats();
                telemetry_.counters().deepLayeredCandidatePackets =
                    collection.candidatePacketCount;
                telemetry_.counters().deepLayeredProjectedPackets =
                    collection.projectedPacketCount;
                telemetry_.counters().deepLayeredAtlasAcceptedPackets =
                    atlas.acceptedPacketCount;
                telemetry_.counters().deepLayeredAtlasAcceptedIslands =
                    atlas.acceptedIslandCount;
                telemetry_.counters().deepLayeredAtlasRejectedPackets =
                    atlas.requestCount - atlas.acceptedPacketCount;
                telemetry_.counters().deepLayeredCapturePreparedDraws =
                    capture.preparedDrawCount;
                telemetry_.counters().deepLayeredCapturePreparationFallbackPackets =
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
            const VkDescriptorSet sceneSet = lighting_.sceneSet(
                scheduler.currentFrameIndex());
            const VkDescriptorSet globalSet = view_.globalSet(
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
                auto* geometry = resources_.geometries().get(packet.geometry);
                auto* material = resources_.materials().get(packet.material);
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
                    telemetry_.recordPipelineBind(effectivePipeline.id);
                    activeLayout = record->pipelineLayout;
                    vkCmdBindDescriptorSets(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        0, 1, &globalSet, 0, nullptr);
                    vkCmdBindDescriptorSets(currentCmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        3u,
                        1, &sceneSet, 0, nullptr);
                    lastBoundPipeline = effectivePipeline;
                    lastBoundMaterial = MaterialHandle{};
                }
                if (packet.material != lastBoundMaterial) {
                    bindMaterialDescriptors(activeLayout);
                    telemetry_.recordMaterialBind(packet.material);
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
                telemetry_.recordDraw(telemetry_.counters().drawTransparentForward,
                    packet.indexCount / 3);
                if (telemetry_.collecting()) {
                    const MaterialClosureClass closure =
                        static_cast<MaterialClosureClass>(
                            material->packed.closureClass);
                    if (closure == MaterialClosureClass::StandardForward) {
                        ++telemetry_.counters().drawStandardForward;
                    }
                    else if (closure == MaterialClosureClass::ComplexForward) {
                        ++telemetry_.counters().drawComplexForward;
                        for (uint32_t lobe = 0;
                            lobe < material->packed.complexLobeCount; ++lobe) {
                            const uint32_t type =
                                material->packed.complexLobes[lobe].type;
                            if (type < telemetry_.counters().complexLobeDraws.size()) {
                                ++telemetry_.counters().complexLobeDraws[type];
                            }
                        }
                    }
                    else if (closure == MaterialClosureClass::Unlit) {
                        ++telemetry_.counters().drawUnlitForward;
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

        // R3c.5 drain point: VSM depth-demand marking and request readback.
        shadows_.recordVirtualShadowDemand();

        const bool requiresRefractionPyramids =
            !compatibilityTransparentQueue.empty();
        transparencyPyramidResidency_.observe(requiresRefractionPyramids);
        if (transparencyPyramidResidency_.requiresFallback(
                requiresRefractionPyramids) && telemetry_.collecting()) {
            ++telemetry_.counters().transparencyPyramidFallbackFrames;
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
                view_.globalSet(scheduler.currentFrameIndex()),
                frameTargets);
            if (telemetry_.collecting())
                telemetry_.counters().dispatchRecorded += dispatches;
            if (telemetry_.collecting()) {
                ++telemetry_.counters().transparencyPyramidBuilds;
                telemetry_.counters().transparencyPyramidMipDispatches += dispatches;
            }
            scheduler.endGpuRange(pyramidGpuRange);
        }
        if (opaque_.depthPyramidEnabled()) {
            // R3c.7 drain point: "depth.occlusion-pyramid.build".
            opaque_.recordDepthPyramid();
            // R3c.4 drain point (a no-op unless the hook is declared).
            hooks_.runPassHook(VulkanHookPasses::PassHook::DepthPyramidValidation,
                { .point = VulkanHookPoint::DepthPyramidValidation,
                    .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                    .payload = VulkanDepthPyramidHookPayload{ retainedRenderView_ } });
        }
        recordForwardPass(sortedSurfaceQueue, graphIds_.sortedForward,
            "gpu.transparency.sorted.forward", transparentPass->getRenderPass(),
            targets.transparentFramebuffer, RenderPassClass::Transparent,
            false, weightedOitExecutionEnabled);
        if (telemetry_.collecting()) {
            telemetry_.counters().transparentSortedPackets = sortedSurfaceQueue.size() -
                (weightedOitExecutionEnabled
                    ? weightedOitPacketCount : 0u);
        }
        if (ordinary2CaptureTopologyActive) {
            recordOrdinary2Captures(compatibilityTransparentQueue,
                captureDraws);
            recordOrdinary2LocalComposition(compatibilityTransparentQueue,
                captureDraws);
            hooks_.runPassHook(VulkanHookPasses::PassHook::Ordinary2Validation,
                { .point = VulkanHookPoint::Ordinary2Validation,
                    .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                    .payload = VulkanOrdinary2HookPayload{ ordinary2AtlasExtent_,
                        static_cast<uint32_t>(captureDraws.size()),
                        static_cast<uint32_t>(
                            ordinary2AtlasPlan_.workIdentities().size()) } });
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

        if (telemetry_.collecting()) {
            telemetry_.counters().transparentBackgroundPackets = 0u;
            telemetry_.counters().transparentForegroundPackets = 0u;
            telemetry_.counters().transparentNonemptyBuckets = 0u;
        }
        recordForwardPass(compatibilityTransparentQueue,
            graphIds_.compatibilityForward,
            "gpu.transparency.compatibility.forward",
            forwardPass->getRenderPass(), targets.forwardFramebuffer,
            RenderPassClass::Forward, true, false);

        // R3c.3 drain point: "transparent.oit.{accumulate,resolve}", inside
        // the transparent pipeline-statistics bracket as before.
        {
            const uint32_t oitFrameIndex = scheduler.currentFrameIndex();
            oit_.record({
                .sortedSurfaceQueue = sortedSurfaceQueue,
                .instanceTransforms = instanceTransforms,
                .execute = weightedOitExecutionEnabled &&
                    weightedOitPacketCount != 0u,
                .globalSet = view_.globalSet(oitFrameIndex),
                .sceneSet = lighting_.sceneSet(oitFrameIndex),
                .debugView = debugView_,
            });
        }

        if (pipelineStatisticsActive) {
            scheduler.endTransparentPipelineStatistics();
        }
        // R3c.4 drain point: scene-linear captures.
        hooks_.runSceneColorCapture(currentCmd,
            captureSource(FrameCapturePoint::SceneLinear));
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

    void VulkanVertexBackend::submitOutputPass() {
        // R3c.2 drain point: bloom-hook (skipped) and output-transform.
        output_.recordOutputTransform({
            .transport = outputTransport_,
            .paperWhiteNits = paperWhiteNits_,
            .peakNits = peakNits_,
            .selectionOutline = selectionOutlineActive_,
        });
        // R3c.4 drain point: final-output captures and the retained views.
        hooks_.runFinalCapture(currentCmd,
            captureSource(outputTransport_ == Color::OutputTransport::SdrSrgb
                ? FrameCapturePoint::FinalSdr : FrameCapturePoint::FinalOutput),
            retainedViewsEnabled_);
    }

    void VulkanVertexBackend::submitUIPass() {
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
            if (telemetry_.collecting() && framebufferWidth > 0 && framebufferHeight > 0) {
                telemetry_.recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::ImGui));
                const ImVec2 clipOffset = draw_data->DisplayPos;
                const ImVec2 clipScale = draw_data->FramebufferScale;
                for (const ImDrawList* drawList : draw_data->CmdLists) {
                    for (const ImDrawCmd& command : drawList->CmdBuffer) {
                        if (command.UserCallback != nullptr) {
                            if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                                telemetry_.recordPipelineBind(
                                    pipelineIdentity(FixedPipelineIdentity::ImGui));
                            }
                            else {
                                ++telemetry_.counters().uiUntrackedCallbacks;
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

                        telemetry_.recordDraw(telemetry_.counters().drawUi, command.ElemCount / 3);
                    }
                }
            }
            ImGui_ImplVulkan_RenderDrawData(draw_data, currentCmd);
        }

        vkCmdEndRenderPass(currentCmd);
        }
        // R3c.2 drain point: "hdr10-encode-present" (declared for HDR10
        // composition only; otherwise a no-op).
        output_.recordHdr10Encode(vkSwapchain->getExtent(), paperWhiteNits_,
            peakNits_);
        renderGraph_.finishFrameExecution();
    }

    FrameStatus VulkanVertexBackend::endFrame() {
        const FrameStatus status = scheduler.endFrame(
            vkSwapchain->getSwapchain(), currentImageIndex);
        frameOpen_ = false;
        if (telemetry_.collecting() && cpuProfiler_ != nullptr) {
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
        opaque_.setRetainedView(renderView);
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

    void VulkanVertexBackend::initializeRetainedViews(VkCommandBuffer commandBuffer) {
        VulkanCommandList commands(commandBuffer);
        for (auto& image : retainedViewImages_) if (image.state == ResourceState::Undefined) {
            commands.transition(image, ResourceState::CopyDestination);
            const VkClearColorValue black{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            commands.transition(image, ResourceState::ShaderResource);
        }
    }

    void VulkanVertexBackend::copyRetainedView(VkCommandBuffer commandBuffer) {
        VulkanCommandList commands(commandBuffer);
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
        VulkanTexturePayload* payload = resources_.textures().get(texture);
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
