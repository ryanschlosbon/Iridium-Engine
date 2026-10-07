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
#include "profiling/CpuProfiler.h"
#include <algorithm>
#include <stdexcept>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Iridium {

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
        renderGraphAliasing_ = config.renderGraphAliasing;
        antiAliasing_ = config.antiAliasing;
        taaTuning_ = config.taaTuning;
        exposure_.configure(config.exposureMode, config.autoExposure);   // M9.5
        bloom_.configure(config.bloom);   // M9.4
        uploadQueueMode_ = config.uploadQueue;
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
        // R4c.4: before any pipeline is created.
        pipelineCache_.init(vkContext->getDevice(),
            VulkanPipelineCacheIdentity::of(vkContext->getPhysicalDevice()),
            config.pipelineCacheDirectory);
        if (!vkContext->hasDescriptorIndexing()) {
            throw std::runtime_error(
                "Indexed material descriptors are required for the production path, "
                "but the Vulkan device lacks the complete descriptor-indexing feature set.");
        }
        resourceAllocator.init(vkContext->getInstance(), vkContext->getPhysicalDevice(),
            vkContext->getDevice(),
            vkContext->hasMemoryBudget());
        // The VSM depth oracle qualifies live VSM demand, so it implies the
        // resources.
        if (config.experimentalVirtualShadowResources ||
            activeIndirectOracle(VulkanIndirectOracleView::VirtualShadowDepth)) {
            shadows_.initVirtualShadows(vkContext->getDevice(),
                pipelineCache_.handle(), resourceAllocator,
                vkContext->getPhysicalDeviceProperties().limits,
                config.virtualShadowResources);
        }
        std::cout << "Upload queue: family " << vkContext->getTransferQueueFamily()
            << " (" << vulkanTransferQueueKindName(vkContext->getTransferQueueKind())
            << "), timeline semaphores "
            << (vkContext->hasTimelineSemaphore() ? "on" : "off") << '\n';
        uploadContext.init(vkContext->getDevice(), VulkanUploadContext::Queues{
                .graphics = vkContext->getGraphicsQueue(),
                .graphicsFamily = vkContext->getGraphicsQueueFamily(),
                .transfer = vkContext->getTransferQueue(),
                .transferFamily = vkContext->getTransferQueueFamily(),
            }, resourceAllocator, cpuProfiler_, VulkanUploadContext::Options{
                .mode = uploadQueueMode_,
                .timelineSemaphore = vkContext->hasTimelineSemaphore(),
                .synchronization2 = vkContext->hasSynchronization2(),
            });
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
                sceneExtent_.height,
            vkContext->hasSynchronization2(), vkContext->hasTimelineSemaphore());
        scheduler.attachAllocator(resourceAllocator);
        // M7R R4d.3: resources written by an upload outlive the frame that
        // waits on it.
        scheduler.setRetireFloor(&VulkanUploadContext::retireFloorThunk, &uploadContext);

        resources_.init({
            .device = vkContext->getDevice(),
            .allocator = &resourceAllocator,
            .uploads = &uploadContext,
            .scheduler = &scheduler,
            .pipelines = &pipelineLibrary,
            .profiler = cpuProfiler_,
            .frameOpen = &frameOpen_,
        });
        featureContext_.emplace(VulkanFeatureContext{
            .vk = *vkContext,
            .device = vkContext->getDevice(),
            .pipelineCache = pipelineCache_.handle(),
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
                [](void* owner, uint32_t swappedSlots) {
                    // R4c.2: the slots that swapped at once rebind now; the
                    // others rebind at their retirement (swapRetiredSlot).
                    auto& self = *static_cast<VulkanVertexBackend*>(owner);
                    if (self.lighting_.sceneSetReady()) {
                        if (swappedSlots == VulkanReflectionProbeFeature::AllSlots)
                            self.lighting_.bindReflectionProbeBuffers();
                        else
                            for (uint32_t slot = 0;
                                    slot < VulkanFrameScheduler::FramesInFlight; ++slot)
                                if ((swappedSlots & (1u << slot)) != 0u)
                                    self.lighting_.bindReflectionProbeBuffers(slot);
                    }
                    self.rebindIdleSlotImports();
                },
                [](void* owner) {
                    // R4c.3: idle slots rebind the environment table now, the
                    // others at their retirement (swapRetiredSlot).
                    auto& self = *static_cast<VulkanVertexBackend*>(owner);
                    bool anyInFlight = false;
                    for (uint32_t slot = 0;
                            slot < VulkanFrameScheduler::FramesInFlight; ++slot) {
                        self.probeEnvironmentRebindPending_[slot] =
                            self.scheduler.slotInFlight(slot);
                        anyInFlight = anyInFlight ||
                            self.probeEnvironmentRebindPending_[slot];
                    }
                    if (!anyInFlight) {
                        self.lighting_.bindReflectionProbeEnvironments();
                        return;
                    }
                    for (uint32_t slot = 0;
                            slot < VulkanFrameScheduler::FramesInFlight; ++slot)
                        if (!self.probeEnvironmentRebindPending_[slot])
                            self.lighting_.bindReflectionProbeEnvironments(slot);
                } });
        probes_.create(*featureContext_);
        // R3c.9: the forward owner (render passes, refraction pyramids).
        forward_.configure(layered_);
        forward_.create(*featureContext_);
        exposure_.create(*featureContext_);   // M9.5 (before its consumers)
        taa_.create(*featureContext_);   // M9.2
        bloom_.create(*featureContext_);   // M9.4
        bloom_.setExposureSource(&exposure_);   // exposed-unit threshold
        output_.create(*featureContext_);
        output_.setExposureFallback(exposure_.fallbackState());
        output_.createPipelines(outputTargetFormat_,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            vkSwapchain->getImageFormat());
        oit_.create(*featureContext_);
        hooks_.create(*featureContext_);
        // R3c.10: the editor bridge's retained views consume the final
        // capture hook; without a bridge there are no retained views.
        if (IVulkanEditorUi* editor = editorUi()) {
            hooks_.setFinalCaptureConsumer({ editor,
                [](void* owner, VkCommandBuffer commandBuffer) {
                    static_cast<IVulkanEditorUi*>(owner)->
                        prepareRetainedViewImages(commandBuffer);
                },
                [](void* owner, VkCommandBuffer commandBuffer) {
                    static_cast<IVulkanEditorUi*>(owner)->copyRetainedView(commandBuffer);
                } });
        }
        ui_.create(*featureContext_);
        ui_.setEditorUi(editorUi());
        // R3c.9: the layered-glass owner (capture, composition, resolve).
        layered_.configure(lighting_.setLayout());
        layered_.configureTopology({ &forward_.pyramidResidency(), &oit_, this,
            [](void* owner) {
                static_cast<VulkanVertexBackend*>(owner)->releaseFrameTargets();
            },
            [](void* owner) {
                auto& self = *static_cast<VulkanVertexBackend*>(owner);
                self.createFrameTargets();
                self.registerEditorTargetTextures();
            } });
        layered_.create(*featureContext_);
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

        // M9.1: velocity-writing direct draws push 144 B (Vulkan guarantees
        // 128; every supported desktop GPU exposes 256).
        if (vkContext->getPhysicalDeviceProperties().limits.maxPushConstantsSize <
                sizeof(CanonicalMotionPushConstants))
            throw std::runtime_error(
                "Iridium requires at least 144 bytes of push constants");
        // R4a: material pipelines use dynamic rendering (formats + layout).
        pipelineLibrary.init(vkContext->getDevice(), pipelineCache_.handle(),
            { [&] {
                std::array<VkFormat, VulkanPipelineMaxColorTargets> formats{};
                const auto pass = vulkanGBufferPassColorAttachmentFormats(gBufferLayout_);
                std::copy(pass.begin(), pass.end(), formats.begin());
                return formats;
              }(), VulkanGBufferPassColorAttachmentCount,
                VK_FORMAT_D32_SFLOAT, meshLayouts.getGBufferPipelineLayout() },
            { { VulkanSceneColorFormat }, 1, VK_FORMAT_D32_SFLOAT,
                meshLayouts.getForwardPipelineLayout() },
            // M9.1: forward-opaque writes scene colour and velocity.
            { { VulkanSceneColorFormat, VulkanVelocityFormat }, 2, VK_FORMAT_D32_SFLOAT,
                meshLayouts.getForwardPipelineLayout() },
            { { VulkanSceneColorFormat }, 1, VK_FORMAT_D32_SFLOAT,
                meshLayouts.getForwardPipelineLayout() },
            gBufferLayout_);

        // 5. UI Pass
        const bool hdr10Composition = outputTransport_ ==
            Color::OutputTransport::Hdr10Pq;
        ui_.setColorFormat(hdr10Composition ? VK_FORMAT_R16G16B16A16_SFLOAT
            : vkSwapchain->getImageFormat());

        // 7. Render Targets
        rebuildRenderGraphAfterDeviceIdle();
        initFrameTargets();
        if (hdr10Composition) output_.rebuildHdr10Targets();
        // Target descriptors declare shader-read layouts, so submit their initial
        // Undefined -> ShaderResource transitions before any descriptor or
        // editor registration can reference those images.
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
        forward_.rebuildDescriptors();
        opaque_.rebuildDescriptors();
        layered_.rebuildDescriptors();
        oit_.rebuildDescriptors();

        // 3. Lighting descriptors (one set per frame context).
        lighting_.rebuildSceneSet();
        output_.rebuildDescriptors();

        // 4. The editor bridge (R3c.10): its UI device state, then the
        // viewport textures over the frame targets.
        if (IVulkanEditorUi* editor = editorUi()) {
            editor->onUiDeviceReady({
                .instance = vkContext->getInstance(),
                .physicalDevice = vkContext->getPhysicalDevice(),
                .device = vkContext->getDevice(),
                .queueFamily = vkContext->getGraphicsQueueFamily(),
                .queue = vkContext->getGraphicsQueue(),
                .pipelineCache = pipelineCache_.handle(),
                .allocator = &resourceAllocator,
                .scheduler = &scheduler,
                .frameTargets = &frameTargets,
                .resources = &resources_,
                .telemetry = &telemetry_,
                .backend = this,
                .selectRetainedView = [](void* backend, uint32_t view) {
                    auto& self = *static_cast<VulkanVertexBackend*>(backend);
                    self.retainedRenderView_ = view;
                    self.opaque_.setRetainedView(view);
                },
            }, editorUiPresentation());
        }
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

    void VulkanVertexBackend::applyOutputSettings(
        const RenderFrameOutputSettings& settings) {
        // Unchanged settings re-apply nothing (swapchain metadata, editor
        // display colour), as the editor host used to skip the call.
        if (settings.manualExposureEv == output_.manualExposure() &&
            settings.paperWhiteNits == paperWhiteNits_ &&
            settings.peakNits == peakNits_) return;
        const float manualExposureEv = settings.manualExposureEv;
        const float paperWhiteNits = settings.paperWhiteNits;
        const float peakNits = settings.peakNits;
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
        if (IVulkanEditorUi* editor = editorUi())
            editor->onDisplayColorChanged(outputTransport_, paperWhiteNits_);
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

        // The editor bridge releases its textures and retained views and
        // shuts ImGui down while every backend resource still exists.
        if (IVulkanEditorUi* editor = editorUi()) editor->onUiShutdown();

        lighting_.releaseSceneSet();
        forward_.clearDescriptors();
        opaque_.clearDescriptors();
        layered_.clearDescriptors();
        oit_.clearDescriptors();
        frameTargets.cleanup();
        renderGraph_.discardRetainedHistory();
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


        lighting_.destroy();

        ui_.destroy();
        output_.destroy();
        taa_.destroy();
        bloom_.destroy();
        exposure_.destroy();

        forward_.destroy();
        layered_.destroy();
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
        // R4c.4: every pipeline is destroyed and the device is idle.
        pipelineCache_.save();
        pipelineCache_.destroy();
        vkContext.reset();
        initialized_ = false;
        frameOpen_ = false;
        forward_.pyramidResidency().restore(false);
        layered_.ordinary2Residency().restore(false);
        layered_.hero4Residency().restore(false);
        layered_.cinematic8Residency().restore(false);
        oit_.residency().restore(false);
        layered_.ordinary2Extent() = {};
        layered_.hero4Extent() = {};
        layered_.cinematic8Extent() = {};
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
            .transientAliasing = renderGraphAliasing_,
            .temporalAntiAliasing = antiAliasing_ == AntiAliasingMode::Taa,
            .autoExposure = exposure_.mode() == ExposureMode::Auto,
            .bloomLevels = bloom_.graphLevels(),
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
        // R3b.6, R4a. The swapchain is rebound per frame after acquire (every
        // slot starts on image 0 so validateFrame holds before a slot's first
        // acquire). It is executor-owned and discarded on first use: the
        // writing pass transitions it from UNDEFINED with a Present
        // (BOTTOM_OF_PIPE) source scope, which in sync2 is ALL_COMMANDS and
        // so chains with the acquire semaphore's COLOR_ATTACHMENT_OUTPUT
        // wait; the frame-end export moves it to PRESENT_SRC.
        using RenderGraph::Access;
        for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            renderGraph_.bindExternalImage(frame, graphIds_.swapchain,
                swapchainGraphImage(0), Access::Present,
                ExternalSyncPolicy::discardOnFirstUse());
        // R4a: the shadow maps (dynamic rendering) are executor-owned
        // globals; their state persists across slots. Each writing pass moves
        // a whole map to DepthAttachmentWrite with its contents kept, and the
        // next reader moves it back. Every frame that writes one also runs
        // lighting, which samples it, so a frame (and therefore a rebuild)
        // always leaves it SampledRead.
        renderGraph_.bindExternalImage(VulkanGlobalBinding,
            graphIds_.shadowDirectionalMap, shadows_.map().image(),
            Access::SampledRead, ExternalSyncPolicy::executorOwned());
        renderGraph_.bindExternalImage(VulkanGlobalBinding,
            graphIds_.shadowSpotMap, localShadows_.spot().image(), Access::SampledRead,
            ExternalSyncPolicy::executorOwned());
        for (uint32_t tier = 0; tier < graphIds_.shadowPointMaps.size(); ++tier)
            renderGraph_.bindExternalImage(VulkanGlobalBinding,
                graphIds_.shadowPointMaps[tier], localShadows_.point().image(tier),
                Access::SampledRead, ExternalSyncPolicy::executorOwned());
    }

    namespace {
        struct ImportedBufferBinding {
            RenderGraph::GraphResourceId id;
            const std::array<VulkanBufferResource,
                VulkanFrameScheduler::FramesInFlight>* buffers;
        };
    }

    void VulkanVertexBackend::bindGraphImportedBuffers() {
        // R3b.7: per-slot indirect command/count buffers of the four drawing
        // cullers and the reflection-probe cluster buffers. After a graph
        // rebuild (device idle; M7R R4c.2 removed the drain that capacity
        // growth used to reach here): every slot is unbound before any is
        // rebound (a recycled handle must not meet a destroyed one).
        if (renderGraph_.compiledGraph() == nullptr) return;
        if (frameOpen_)
            throw std::logic_error(
                "Graph imported buffers rebind only at a frame boundary");
        for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            renderGraph_.onFrameFenceCompleted(frame);
        using Binding = ImportedBufferBinding;
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
        importRebindPending_ = {};
    }

    void VulkanVertexBackend::rebindGraphImportedBuffers(uint32_t slot) {
        // R4c.2: one retired slot. Its replacement buffers were created while
        // the old ones were alive, so they cannot alias a handle still bound
        // in the other slot.
        if (renderGraph_.compiledGraph() == nullptr) return;
        if (frameOpen_)
            throw std::logic_error(
                "Graph imported buffers rebind only at a frame boundary");
        using Binding = ImportedBufferBinding;
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
        for (const Binding& binding : bindings)
            if (binding.id.isValid()) renderGraph_.unbindExternalBuffer(slot, binding.id);
        for (const Binding& binding : bindings) {
            if (!binding.id.isValid()) continue;
            const VulkanBufferResource& buffer = (*binding.buffers)[slot];
            if (buffer.buffer == VK_NULL_HANDLE) continue;
            renderGraph_.bindExternalBuffer(slot, binding.id, buffer.buffer,
                buffer.size);
        }
        importRebindPending_[slot] = false;
    }

    void VulkanVertexBackend::rebindIdleSlotImports() {
        if (renderGraph_.compiledGraph() == nullptr) return;
        // M7R R4b.6 fix: every idle slot swapped at once, so a growth that
        // destroyed one idle slot's buffers may hand a recycled handle to
        // another owner's replacement. Unbind every idle slot before binding
        // any (as bindGraphImportedBuffers does), or the first slot's bind
        // meets the second's stale handle (seen without validation layers,
        // whose handle wrapping never recycles).
        std::array<bool, VulkanFrameScheduler::FramesInFlight> idle{};
        for (uint32_t slot = 0; slot < VulkanFrameScheduler::FramesInFlight; ++slot) {
            if (scheduler.slotInFlight(slot)) {
                importRebindPending_[slot] = true;
                continue;
            }
            idle[slot] = true;
            // The slot's fence has been waited (possibly by a bounded stall
            // outside beginFrame): retire it in the executor before binding.
            renderGraph_.onFrameFenceCompleted(slot);
            unbindGraphImportedBuffers(slot);
        }
        for (uint32_t slot = 0; slot < VulkanFrameScheduler::FramesInFlight; ++slot)
            if (idle[slot]) rebindGraphImportedBuffers(slot);
    }

    void VulkanVertexBackend::unbindGraphImportedBuffers(uint32_t slot) {
        const RenderGraph::GraphResourceId ids[] = {
            graphIds_.directionalIndirect.commands, graphIds_.directionalIndirect.counts,
            graphIds_.spotIndirect.commands, graphIds_.spotIndirect.counts,
            graphIds_.pointIndirect.commands, graphIds_.pointIndirect.counts,
            graphIds_.opaqueIndirect.commands, graphIds_.opaqueIndirect.counts,
            graphIds_.probeClusterHeaders, graphIds_.probeClusterIndices };
        for (const RenderGraph::GraphResourceId id : ids)
            if (id.isValid()) renderGraph_.unbindExternalBuffer(slot, id);
    }

    void VulkanVertexBackend::swapRetiredSlot(uint32_t slot) {
        bool pending = importRebindPending_[slot] ||
            probeEnvironmentRebindPending_[slot] ||
            gpuScene_.slotSwapPending(slot) || resources_.slotSwapPending(slot) ||
            opaque_.culler().slotSwapPending(slot) ||
            clusterLighting_.slotSwapPending(slot) || probes_.slotSwapPending(slot);
        for (VulkanIndirectViewCuller* culler : indirectViewCullers())
            pending = pending || culler->slotSwapPending(slot);
        if (!pending) return;
        CpuScope swapScope(cpuProfiler_, "cpu.renderer.slot_swap");
        bool imports = importRebindPending_[slot];
        (void)gpuScene_.swapRetiredSlot(slot);
        (void)resources_.swapRetiredSlot(slot);
        imports = opaque_.culler().swapRetiredSlot(slot) || imports;
        for (VulkanIndirectViewCuller* culler : indirectViewCullers())
            imports = culler->swapRetiredSlot(slot) || imports;
        if (clusterLighting_.swapRetiredSlot(slot) && lighting_.sceneSetReady())
            lighting_.bindLightBuffers(slot);
        if (probes_.swapRetiredSlot(slot)) {
            if (lighting_.sceneSetReady()) lighting_.bindReflectionProbeBuffers(slot);
            imports = true;
        }
        if (probeEnvironmentRebindPending_[slot]) {
            lighting_.bindReflectionProbeEnvironments(slot);
            probeEnvironmentRebindPending_[slot] = false;
        }
        if (imports) rebindGraphImportedBuffers(slot);
    }

    void VulkanVertexBackend::rebuildRenderGraphAfterDeviceIdle() {
        // Temporal history survives only compatible rebuilds (ADR-0017).
        if (cpuProfiler_ && cpuProfiler_->isEnabled()) cpuProfiler_->recordCounter("render_graph.rebuilds", 1);
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
            forward_.pyramidResidency().enabled(),
            VulkanLayeredGraphConfig{ layered_.ordinary2Extent(),
                layered_.hero4Extent(), layered_.cinematic8Extent(),
                oit_.residency().enabled() }, productionGraphFeatures()));
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
            forward_.pyramidResidency().enabled(),
            VulkanLayeredGraphConfig{ layered_.ordinary2Extent(),
                layered_.hero4Extent(), layered_.cinematic8Extent(),
                oit_.residency().enabled() }, productionGraphFeatures());

        // Resize is the one accepted global stall, after candidate validation.
        vkDeviceWaitIdle(vkContext->getDevice());

        // Owners with descriptor sets over transient graph resources retire
        // them before the graph is rebuilt, and recreate them afterward.
        releaseFrameTargets();
        output_.destroyPipelines();
        ui_.resetColorFormat();

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
        ui_.setColorFormat(hdr10Composition ? VK_FORMAT_R16G16B16A16_SFLOAT
            : vkSwapchain->getImageFormat());
        if (IVulkanEditorUi* editor = editorUi())
            editor->onPresentationChanged(editorUiPresentation());
        const uint32_t newImageCount = vkSwapchain->getImageCount();
        scheduler.resetSwapchainImages(newImageCount);
        scheduler.setTransparentTargetPixelCount(
            static_cast<uint64_t>(sceneExtent_.width) *
            sceneExtent_.height);
        // The replacement target images are referenced by descriptor sets and
        // the editor immediately below; createFrameTargets establishes their
        // declared layouts (upload flush) first.
        createFrameTargets();
        if (newImageCount != oldImageCount) {
            if (IVulkanEditorUi* editor = editorUi())
                editor->onSwapchainImageCountChanged(newImageCount);
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
            layered_.ordinary2Extent();
        const VkExtent2D previousHero4AtlasExtent = layered_.hero4Extent();
        const VkExtent2D previousCinematic8AtlasExtent =
            layered_.cinematic8Extent();
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
        resizeTier(layered_.ordinary2Residency().enabled(),
            TransparencyQuality::Ordinary2,
            requestedOrdinary2AtlasExtent);
        resizeTier(layered_.hero4Residency().enabled(),
            TransparencyQuality::Hero4, requestedHero4AtlasExtent);
        resizeTier(layered_.cinematic8Residency().enabled(),
            TransparencyQuality::Cinematic8,
            requestedCinematic8AtlasExtent);
        try {
            (void)buildVulkanProductionRenderGraph(requested,
                vkSwapchain->getExtent(), vkSwapchain->getImageFormat(),
                outputTargetFormat_, outputTransport_ ==
                    Color::OutputTransport::Hdr10Pq, gBufferLayout_,
                clusterConfig_, directionalShadowResolution_,
                spotShadowAtlasResolution_,
                forward_.pyramidResidency().enabled(),
                VulkanLayeredGraphConfig{ requestedOrdinary2AtlasExtent,
                    requestedHero4AtlasExtent,
                    requestedCinematic8AtlasExtent,
                    oit_.residency().enabled() }, productionGraphFeatures());
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
        layered_.ordinary2Extent() = requestedOrdinary2AtlasExtent;
        layered_.hero4Extent() = requestedHero4AtlasExtent;
        layered_.cinematic8Extent() = requestedCinematic8AtlasExtent;
        try {
            createTargets();
            return true;
        }
        catch (const std::exception& exception) {
            diagnostic = std::string("Scene target resize failed: ") +
                exception.what();
            releaseTargets();
            sceneExtent_ = previous;
            layered_.ordinary2Extent() = previousOrdinary2AtlasExtent;
            layered_.hero4Extent() = previousHero4AtlasExtent;
            layered_.cinematic8Extent() = previousCinematic8AtlasExtent;
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

    // A graph topology change between frames, like a resize: retire every
    // slot, rebuild the graph and targets (temporal history starts over); on
    // failure restore the previous state and rebuild again.
    template <class Restore>
    bool VulkanVertexBackend::rebuildFrameTargets(Restore&& restore,
        std::string& diagnostic, const char* what) {
        scheduler.waitForAllFrames();
        releaseFrameTargets();
        try {
            createFrameTargets();
            registerEditorTargetTextures();
            return true;
        }
        catch (const std::exception& exception) {
            diagnostic = std::string(what) + " switch failed: " + exception.what();
            releaseFrameTargets();
            restore();
            createFrameTargets();
            registerEditorTargetTextures();
            return false;
        }
    }

    bool VulkanVertexBackend::setAntiAliasing(AntiAliasingMode mode,
        std::string& diagnostic) {
        diagnostic.clear();
        if (!initialized_ || frameOpen_) {
            diagnostic = "Anti-aliasing can only change between frames of an initialized backend";
            return false;
        }
        if (mode == antiAliasing_) return true;
        const AntiAliasingMode previous = antiAliasing_;
        antiAliasing_ = mode;
        return rebuildFrameTargets([&] { antiAliasing_ = previous; }, diagnostic, "Anti-aliasing");
    }

    bool VulkanVertexBackend::setBloom(const BloomSettings& settings,
        std::string& diagnostic) {
        diagnostic.clear();
        if (!initialized_ || frameOpen_) {
            diagnostic = "Bloom can only change between frames of an initialized backend";
            return false;
        }
        // Intensity and shaping apply from the next frame; turning bloom on
        // or off (or a new level count) changes the graph.
        const BloomSettings previous = bloom_.settings();
        const uint32_t previousLevels = bloom_.graphLevels();
        bloom_.configure(settings);
        if (bloom_.graphLevels() == previousLevels) return true;
        return rebuildFrameTargets([&] { bloom_.configure(previous); }, diagnostic, "Bloom");
    }

    bool VulkanVertexBackend::setExposure(ExposureMode mode,
        const AutoExposureSettings& settings, std::string& diagnostic) {
        diagnostic.clear();
        if (!initialized_ || frameOpen_) {
            diagnostic = "Exposure can only change between frames of an initialized backend";
            return false;
        }
        // Settings apply from the next frame; the mode declares or removes
        // the exposure passes (a graph change).
        const ExposureMode previousMode = exposure_.mode();
        const AutoExposureSettings previous = exposure_.settings();
        exposure_.configure(mode, settings);
        if (mode == previousMode) return true;
        return rebuildFrameTargets([&] { exposure_.configure(previousMode, previous); },
            diagnostic, "Exposure");
    }

    FrameTopologyPreparation VulkanVertexBackend::prepareFrameTopology(
        const FrameTopologyRequirements& requirements) {
        if (frameOpen_) {
            throw std::logic_error(
                "Frame topology preparation is only valid between frames");
        }
        // M7.10.0: the layered owner prepares the transparency topology.
        frameTopologyPrewarm_ = layered_.prepareTopology(requirements, sceneExtent_);
        return frameTopologyPrewarm_;
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
        if (IVulkanEditorUi* editor = editorUi()) editor->onFrameTargetsReleased();
    }

    void VulkanVertexBackend::registerEditorTargetTextures() {
        if (IVulkanEditorUi* editor = editorUi()) editor->onFrameTargetsCreated();
    }

    VulkanEditorUiPresentation VulkanVertexBackend::editorUiPresentation()
        const noexcept {
        return { .colorFormat = ui_.colorFormat(),
            .imageCount = vkSwapchain->getImageCount(),
            .transport = outputTransport_, .paperWhiteNits = paperWhiteNits_ };
    }

    void VulkanVertexBackend::releaseFrameTargets() {
        // Only between frames, after every slot retired: shared descriptor
        // sets and scene targets must be unreferenced.
        releaseEditorTargetTextures();
        lighting_.releaseSceneSet();
        for (IVulkanFeature* feature : features()) feature->onGraphReleased();
        forward_.clearDescriptors();
        opaque_.clearDescriptors();
        layered_.clearDescriptors();
        oit_.clearDescriptors();
        frameTargets.cleanup();
        // ADR-0017: the rebuilt plan adopts compatible history (TAA, exposure).
        renderGraph_.retainHistoryForRebuild();
        renderGraph_.cleanupAfterDeviceIdle();
    }

    void VulkanVertexBackend::createFrameTargets() {
        rebuildRenderGraphAfterDeviceIdle();
        initFrameTargets();
        if (outputTransport_ == Color::OutputTransport::Hdr10Pq)
            output_.rebuildHdr10Targets();
        // Establish the targets' declared layouts before descriptors (and
        // the editor) reference them.
        uploadContext.flush();
        lighting_.rebuildSceneSet();
        forward_.rebuildDescriptors();
        opaque_.rebuildDescriptors();
        layered_.rebuildDescriptors();
        oit_.rebuildDescriptors();
        output_.rebuildDescriptors();
    }

    void VulkanVertexBackend::initFrameTargets() {
        frameTargets.init(vkContext->getDevice(), sceneExtent_,
            VulkanFrameScheduler::FramesInFlight,
            outputTransport_ == Color::OutputTransport::Hdr10Pq,
            forward_.pyramidResidency().enabled(),
            VulkanLayeredGraphConfig{ layered_.ordinary2Extent(),
                layered_.hero4Extent(), layered_.cinematic8Extent(),
                oit_.residency().enabled() },
            renderGraph_, graphIds_);
    }

    // ==============================================================================
    // 3. THE FRAME PIPELINE (Data-Driven Execution)
    // ==============================================================================

    FrameStatus VulkanVertexBackend::beginFrame() {
        frameOpen_ = false;
        opaque_.beginFrame();
        casterRevisions_.beginFrame();
        probes_.beginFrame();
        layered_.beginFrame();
        telemetry_.beginFrame();
        CpuScope beginFrameScope(cpuProfiler_, "cpu.renderer.begin_frame");
        // M7R R4d.3: submitted without a CPU wait; this frame's submission
        // waits on the upload timelines instead (legacy-blocking flushes).
        uploadContext.submitAsync();
        layered_.applyTopologyChange(sceneExtent_);
        const uint32_t completedFrameIndex = scheduler.currentFrameIndex();
        const VulkanFrameBegin frame = scheduler.beginFrame(vkSwapchain->getSwapchain());
        const uint32_t frameSlot = scheduler.currentFrameIndex();
        resourceAllocator.beginFrame();
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
            // R4c.2: capacity-growth replacements parked for this slot.
            swapRetiredSlot(completedFrameIndex);
            // The acquired swapchain image for this slot (R3b.6).
            if (frame.status != FrameStatus::RecreateSwapchain) {
                renderGraph_.bindExternalImage(frameSlot, graphIds_.swapchain,
                    swapchainGraphImage(frame.imageIndex),
                    RenderGraph::Access::Present,
                    ExternalSyncPolicy::discardOnFirstUse());
            }
            if (!renderGraph_.validateFrame(completedFrameIndex)) {
                throw std::runtime_error(
                    "Graph-owned frame targets failed executor validation");
            }
        }
        if (frame.status == FrameStatus::RecreateSwapchain) {
            return frame.status;
        }
        // M7R R4d.3: queue-family acquires of submitted uploads, before any
        // pass, and the upload timeline values this frame waits on.
        {
            const VulkanUploadContext::FrameWaits uploadWaits =
                uploadContext.recordFrameAcquires(frame.commandBuffer);
            scheduler.addFrameWait(uploadWaits.transferTimeline,
                uploadWaits.transferValue);
            scheduler.addFrameWait(uploadWaits.graphicsTimeline,
                uploadWaits.graphicsValue);
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
        // M7R R4b.5: before any graph pass (qualification alias poison).
        extensionHooks_.notify({ .point = VulkanHookPoint::FrameGraphBegin,
            .cmd = currentCmd, .slot = scheduler.currentFrameIndex() });
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
        const ShadowCasterSubmission& shadowCasters) {
        const VulkanIndirectScene scene =
            indirectScene(scheduler.currentFrameIndex());
        uint64_t revision = 0;
        {
            CpuScope revisionScope(cpuProfiler_, "cpu.shadow.caster_revision");
            revision = casterRevisions_.casterRevision(scene, shadowCasters,
                vulkanCasterMaterials(resources_));
        }
        if constexpr (kQualificationBuild)
            extensionHooks_.observeCasterRevision({
                .stream = VulkanCasterRevisionStream::Shadow,
                .frameSerial = scheduler.lastSubmittedSerial() + 1u,
                .scene = &scene, .resources = &resources_,
                .casters = &shadowCasters, .revisions = { &revision, 1u } });
        return revision;
    }

    std::array<uint64_t, kDirectionalShadowCascadeCount>
        VulkanVertexBackend::getDirectionalShadowCasterRevisions(
            const ShadowCasterSubmission& shadowCasters,
            const DirectionalShadowCascadePlan& plan) {
        const VulkanIndirectScene scene =
            indirectScene(scheduler.currentFrameIndex());
        std::array<uint64_t, kDirectionalShadowCascadeCount> revisions{};
        {
            CpuScope revisionScope(cpuProfiler_,
                "cpu.shadow.directional.caster_revision");
            revisions = casterRevisions_.directionalRevisions(scene,
                shadowCasters, vulkanCasterMaterials(resources_), plan);
        }
        if constexpr (kQualificationBuild)
            extensionHooks_.observeCasterRevision({
                .stream = VulkanCasterRevisionStream::DirectionalShadow,
                .ordinal = casterRevisions_.lastDirectionalOrdinal(),
                .frameSerial = scheduler.lastSubmittedSerial() + 1u,
                .scene = &scene, .resources = &resources_,
                .casters = &shadowCasters, .plan = &plan, .revisions = revisions });
        return revisions;
    }

    void VulkanVertexBackend::prepareDepthPyramidHistory(
        const OpaqueSubmission& opaque,
        std::span<const DrawPacket> opaqueForwardQueue) {
        if (!frameOpen_)
            throw std::logic_error(
                "Depth-pyramid history preparation requires an open frame");
        opaque_.prepareDepthHistory(opaque, opaqueForwardQueue);
    }

    void VulkanVertexBackend::submitFrame(const RenderFrame& frame) {
        if (!frameOpen_)
            throw std::logic_error("submitFrame requires an open frame");
        if (frame.lights == nullptr || frame.reflectionProbes == nullptr)
            throw std::invalid_argument(
                "A render frame needs its light and reflection-probe packets");
        const auto stageComplete = [&frame](RenderFrameStage stage) {
            if (frame.stageObserver != nullptr)
                frame.stageObserver->onRenderFrameStage(stage);
        };
        // View and output state first (the camera uniforms encode the debug
        // view), as the separate setters were called before extraction.
        applyOutputSettings(frame.output);
        debugView_ = frame.debugView;
        // M9 G1: History is keyed by this frame's view before any pass runs.
        renderGraph_.beginViewExecution(frame.history);
        updateCamera(frame.view, frame.history);
        output_.setGridOverlay(frame.gridOverlay);

        submitDirectionalShadows(frame.directionalShadows.casters,
            frame.directionalShadows.shadows);
        stageComplete(RenderFrameStage::DirectionalShadows);
        submitSpotShadows(frame.spotShadows.casters, frame.spotShadows.shadows);
        stageComplete(RenderFrameStage::SpotShadows);
        submitPointShadows(frame.pointShadows.casters, frame.pointShadows.shadows);
        stageComplete(RenderFrameStage::PointShadows);
        if (frame.submitReflectionProbeCaptures) {
            submitReflectionProbeCaptures(frame.probeCasters,
                frame.probeCaptureSchedule, *frame.lights);
            stageComplete(RenderFrameStage::ReflectionProbeCaptures);
        }

        {
            CpuScope historyScope(cpuProfiler_, "cpu.render.prepare.depth_history");
            prepareDepthPyramidHistory(frame.opaque, frame.forwardOpaqueQueue);
        }
        submitOpaqueQueue(frame.opaque, frame.selectionQueue, frame.wireframe);
        // The camera position, matrices and planes are the view record's
        // (bit-identical to the former submitLightingPass arguments).
        submitLightingPass(glm::vec3(frame.view.cameraPosition), frame.view.view,
            frame.view.projection, frame.view.jitteredProjection, frame.view.depthRange.x,
            frame.view.depthRange.y, *frame.lights, *frame.reflectionProbes);
        stageComplete(RenderFrameStage::Lighting);
        // M9.5: auto-exposure adapts over the view's time, with the manual EV
        // as compensation; its passes drain with the output transform.
        exposure_.stage({ frame.viewDeltaSeconds, output_.manualExposure() });
        // M9.2: TAA pre-exposes with the output's manual EV, or (M9.5) with
        // last frame's adapted exposure; its pass drains with the output.
        taa_.stageFrame(frame.view, frame.output.manualExposureEv,
            frame.viewDeltaSeconds, sceneExtent_, taaTuning_,
            view_.globalSet(scheduler.currentFrameIndex()), exposure_);
        submitForwardQueues(frame.forwardOpaqueQueue,
            frame.forwardOpaquePreviousTransforms, frame.sortedSurfaceQueue,
            frame.compatibilityTransparentQueue, frame.instanceTransforms, frame);
        stageComplete(RenderFrameStage::SceneLinearComplete);
        submitOutputPass();
        stageComplete(RenderFrameStage::OutputComplete);
        submitUIPass();
    }

    void VulkanVertexBackend::submitDirectionalShadows(
        const ShadowCasterSubmission& shadowCasters,
        std::span<const DirectionalShadowFramePacket> shadows) {
        if (!frameOpen_)
            throw std::logic_error(
                "Directional shadows require an open frame");
        // R3c.5 drain point: clip upload, compaction and cascades. The VSM
        // clip key is the submission's caster revision (no classification of
        // static and dynamic casters is claimed).
        const uint64_t virtualShadowCasterRevision =
            shadows_.virtualShadows().initialized()
            ? getShadowCasterRevision(shadowCasters) : 0u;
        shadows_.submit(shadowCasters, shadows, virtualShadowCasterRevision);
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

    void VulkanVertexBackend::submitOpaqueQueue(const OpaqueSubmission& opaque,
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
            .opaque = opaque,
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

    std::span<const ReflectionProbeCaptureCompletion>
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
        // R4c.2: slots that swapped at once rebind now; the others at their
        // retirement (swapRetiredSlot).
        if (clusterLighting_.prepare(requiredCapacity) && lighting_.sceneSetReady())
            for (uint32_t slot = 0; slot < VulkanFrameScheduler::FramesInFlight; ++slot)
                if (!clusterLighting_.slotSwapPending(slot))
                    lighting_.bindLightBuffers(slot);
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
            rebindIdleSlotImports();
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
        casterRevisions_.publishScene(scene);
        opaque_.publishScene(scene);
        if constexpr (kQualificationBuild) {
            const std::array<uint64_t, 3> membership{
                scene.shadowConsumerMembershipRevision,
                scene.probeConsumerMembershipRevision,
                scene.mainOpaqueConsumerMembershipRevision };
            extensionHooks_.observeCasterRevision({
                .stream = VulkanCasterRevisionStream::Membership,
                .frameSerial = scheduler.lastSubmittedSerial() + 1u,
                .tables = &scene, .revisions = membership });
        }
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
            forward_.pyramidResidency().enabled(), debugView_);
    }

    void VulkanVertexBackend::submitLightingPass(const glm::vec3& cameraPos,
        const glm::mat4& view, const glm::mat4& proj, const glm::mat4& rasterProj,
        float nearPlane, float farPlane,
        const LightingFramePacket& lights,
        const ReflectionProbeGpuFramePacket& reflectionProbes) {
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.lighting");
        layered_.setViewProjection(proj * view);
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        clusterLighting_.uploadFrame(frameIndex, view, proj, nearPlane, farPlane,
            lights, sceneExtent_, lighting_.environmentSettings());
        probes_.uploadFrame(frameIndex, view, proj, nearPlane, farPlane,
            reflectionProbes, sceneExtent_);
        // R3c.1 drain points: "lighting.probe-cluster" and the cluster build
        // run here, where they were recorded imperatively.
        clusterLighting_.recordFrame(frameIndex, view, proj, nearPlane, farPlane,
            sceneExtent_, lights.stats.activeLightCount);
        // R3c.8 drain point: "lighting".
        lighting_.record({
            .cameraPosition = cameraPos,
            .view = view,
            // M9 G5b: reconstruction from (jittered) depth uses the raster
            // projection; equal to `proj` when jitter is off.
            .projection = rasterProj,
            .debugView = debugView_,
        });
    }

    void VulkanVertexBackend::recordDeepLayeredValidationHook(
        TransparencyQuality quality) {
        if (!extensionHooks_.graphHooks().layeredValidation) return;
        const VulkanDeepLayeredHookPayload payload = layered_.deepHookPayload(quality);
        hooks_.runPassHook(quality == TransparencyQuality::Hero4
                ? VulkanHookPasses::PassHook::Hero4Validation
                : VulkanHookPasses::PassHook::Cinematic8Validation,
            { .point = VulkanHookPoint::DeepLayeredValidation,
                .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                .payload = payload });
    }

    void VulkanVertexBackend::submitForwardQueues(
        std::span<const DrawPacket> opaqueForwardQueue,
        std::span<const glm::mat4> opaqueForwardPreviousTransforms,
        std::span<const DrawPacket> sortedSurfaceQueue,
        std::span<const DrawPacket> compatibilityTransparentQueue,
        std::span<const glm::mat4> instanceTransforms, const RenderFrame& frame) {
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

        // Residency demand (layered tiers, WeightedOIT), then the frame's
        // layered plans; the owners stage their inputs for the drains below.
        layered_.observe(compatibilityTransparentQueue);
        const VulkanWeightedOitFeature::FrameDecision weightedOit =
            oit_.observe(sortedSurfaceQueue, instanceTransforms);
        const uint32_t frameIndex = scheduler.currentFrameIndex();
        const VkDescriptorSet globalSet = view_.globalSet(frameIndex);
        const VkDescriptorSet sceneSet = lighting_.sceneSet(frameIndex);
        layered_.prepare({
            .compatibilityTransparentQueue = compatibilityTransparentQueue,
            .commandBuffer = currentCmd,
            .globalSet = globalSet,
            .sceneSet = sceneSet,
            .debugView = debugView_,
        });
        forward_.stage({
            .opaqueForwardQueue = opaqueForwardQueue,
            .opaqueForwardPreviousTransforms = opaqueForwardPreviousTransforms,
            .sortedSurfaceQueue = sortedSurfaceQueue,
            .compatibilityTransparentQueue = compatibilityTransparentQueue,
            .sortedSurfacePreviousTransforms = frame.sortedSurfacePreviousTransforms,
            .compatibilityPreviousTransforms = frame.compatibilityPreviousTransforms,
            .skipWeightedOit = weightedOit.executionEnabled,
            .globalSet = globalSet,
            .sceneSet = sceneSet,
            .debugView = debugView_,
        });

        // R3c.9 drain point: "forward-opaque".
        forward_.recordOpaque();
        // R3c.5 drain point: VSM depth-demand marking and request readback.
        shadows_.recordVirtualShadowDemand();
        // R3c.9 drain point: "transparent.refraction-pyramids".
        forward_.recordRefractionPyramids(!compatibilityTransparentQueue.empty());
        if (opaque_.depthPyramidEnabled()) {
            // R3c.7 drain point: "depth.occlusion-pyramid.build".
            opaque_.recordDepthPyramid();
            // R3c.4 drain point (a no-op unless the hook is declared).
            hooks_.runPassHook(VulkanHookPasses::PassHook::DepthPyramidValidation,
                { .point = VulkanHookPoint::DepthPyramidValidation,
                    .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                    .payload = VulkanDepthPyramidHookPayload{ retainedRenderView_ } });
        }
        // R3c.9 drain point: "transparent.sorted.forward".
        forward_.recordSorted();
        if (telemetry_.collecting()) {
            telemetry_.counters().transparentSortedPackets = sortedSurfaceQueue.size() -
                (weightedOit.executionEnabled
                    ? weightedOit.packetCount : 0u);
        }
        // R3c.9 drain points: the layered tiers, with the validation hooks
        // between them.
        if (layered_.ordinary2Active()) {
            layered_.recordOrdinary2Captures();
            layered_.recordOrdinary2LocalComposition();
            hooks_.runPassHook(VulkanHookPasses::PassHook::Ordinary2Validation,
                { .point = VulkanHookPoint::Ordinary2Validation,
                    .cmd = currentCmd, .slot = scheduler.currentFrameIndex(),
                    .payload = layered_.ordinary2HookPayload() });
            layered_.recordOrdinary2SceneResolve();
        }
        const bool hero4Active = layered_.deepActive(TransparencyQuality::Hero4);
        const bool cinematic8Active =
            layered_.deepActive(TransparencyQuality::Cinematic8);
        for (const TransparencyQuality quality :
                { TransparencyQuality::Hero4, TransparencyQuality::Cinematic8 }) {
            if (!layered_.deepActive(quality)) continue;
            layered_.recordDeepCaptures(quality);
            layered_.recordDeepLocalComposition(quality);
            recordDeepLayeredValidationHook(quality);
        }
        if (hero4Active || cinematic8Active) layered_.recordDeepSceneResolve();

        if (telemetry_.collecting()) {
            telemetry_.counters().transparentBackgroundPackets = 0u;
            telemetry_.counters().transparentForegroundPackets = 0u;
            telemetry_.counters().transparentNonemptyBuckets = 0u;
        }
        // R3c.9 drain point: "transparent.compatibility.forward".
        forward_.recordCompatibility();

        // R3c.3 drain point: "transparent.oit.{accumulate,resolve}", inside
        // the transparent pipeline-statistics bracket as before.
        oit_.record({
            .sortedSurfaceQueue = sortedSurfaceQueue,
            .instanceTransforms = instanceTransforms,
            .execute = weightedOit.executionEnabled && weightedOit.packetCount != 0u,
            .globalSet = globalSet,
            .sceneSet = sceneSet,
            .debugView = debugView_,
        });

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
            .motionVectorView = debugView_ == RenderDebugView::MotionVectors,
            .bloomIntensity = bloom_.composite().intensity,
            .bloomAdditive = bloom_.composite().additive,
        });
        // R3c.4 drain point: final-output captures and the retained views.
        const IVulkanEditorUi* editor = editorUi();
        VulkanCaptureHookPayload finalSource =
            captureSource(outputTransport_ == Color::OutputTransport::SdrSrgb
                ? FrameCapturePoint::FinalSdr : FrameCapturePoint::FinalOutput);
        // M9.2: the post chain's scene colour (TAA history slot or scene.color).
        finalSource.sceneResolved = &renderGraph_.image(scheduler.currentFrameIndex(),
            graphIds_.resolvedSceneColor);
        finalSource.sceneResolvedFormat = frameTargets.format();
        if (graphIds_.exposureCurrent.isValid()) {   // M9.5
            finalSource.exposureState = &renderGraph_.buffer(scheduler.currentFrameIndex(),
                graphIds_.exposureCurrent);
            finalSource.exposureMetering = &renderGraph_.buffer(
                scheduler.currentFrameIndex(), graphIds_.exposureMetering);
        }
        hooks_.runFinalCapture(currentCmd, finalSource,
            editor != nullptr && editor->retainedViewsEnabled());
    }

    void VulkanVertexBackend::submitUIPass() {
        CpuScope recordScope(cpuProfiler_, "cpu.render.record.ui");
        // R3c.10 drain point: the UI pass (clear, the editor bridge's UI).
        ui_.record(vkSwapchain->getExtent());
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

} // namespace Iridium
