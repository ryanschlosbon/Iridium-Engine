#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "../rhi/IRenderBackend.h"
#include "../rhi/ResourcePool.h"
#include "../rhi/GpuSceneUploadPlanner.h"
#include "../rhi/GpuSceneIndirect.h"
#include "../rhi/GpuSceneLod.h"

#include "VkContext.h"
#include "VkSwapchain.h"
#include "DescriptorAllocator.h"

// Pipelines & Passes
#include "VkGraphicsPipeline.h"
#include "VkLightingPipeline.h"
#include "VulkanPipelineCache.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceAllocator.h"
#include "VulkanCommandList.h"
#include "VulkanUploadContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanFrameTargets.h"
#include "VulkanSceneDescriptors.h"
#include "VulkanProductionRenderGraph.h"
#include "VulkanBackendExtension.h"
#include "VulkanEditorUi.h"
#include "VulkanClusterLightingFeature.h"
#include "VulkanOutputFeature.h"
#include "VulkanTemporalAntiAliasingFeature.h"
#include "VulkanWeightedOitFeature.h"
#include "VulkanHookPasses.h"
#include "VulkanShadowFeature.h"
#include "VulkanLocalShadowFeature.h"
#include "VulkanReflectionProbeFeature.h"
#include "VulkanShadowCasters.h"
#include "VulkanCasterRevisions.h"
#include "VulkanExtensionHooks.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanGpuSceneState.h"
#include "VulkanResourceRegistry.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanIndirectViewCuller.h"
#include "VulkanOpaqueIndirectCuller.h"
#include "VulkanOpaqueFeature.h"
#include "VulkanDeferredLightingFeature.h"
#include "VulkanViewUniforms.h"
#include "VulkanForwardFeature.h"
#include "VulkanLayeredTransparencyFeature.h"
#include "VulkanUiFeature.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanTransparencyPyramid.h"
#include "VulkanDepthPyramid.h"
#include "VulkanLayeredInterfaceCapturePass.h"
#include "VulkanLayeredLocalCompositionPass.h"
#include "VulkanLayeredSceneResolvePass.h"
#include "VulkanWeightedOitPass.h"
#include "VulkanOutputPass.h"
#include "VulkanHdrEncodePass.h"
#include "VulkanIndexedTextureTable.h"
#include "VulkanClusteredLightingPipeline.h"
#include "VulkanReflectionProbePipeline.h"
#include "VulkanReflectionProbeCapturePass.h"
#include "VulkanReflectionProbeCaptureTargets.h"
#include "VulkanDirectionalShadowMap.h"
#include "VulkanSpotShadowAtlas.h"
#include "VulkanPointShadowPools.h"
#include "VulkanVirtualShadowResources.h"
#include "renderer/transparency/TransparencyPyramidResidency.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "renderer/transparency/Ordinary2Atlas.h"

#include "utils/DeletionQueue.h"
#include "core/BuildFeatures.h"

namespace Iridium {

    // ==============================================================================
    // THE CONCRETE BACKEND
    // ==============================================================================

    class VulkanVertexBackend : public IRenderBackend {
    private:

        // --- 1. THE SUBSYSTEMS (Composition) ---
        // We moved all of these pointers out of Application.cpp and into here.
        std::unique_ptr<VkContext> vkContext;
        VulkanResourceAllocator resourceAllocator;
        VulkanUploadContext uploadContext;
        VulkanFrameScheduler scheduler;
        std::unique_ptr<VkSwapchain> vkSwapchain;
        DescriptorAllocator descriptorAllocator;
        VulkanPipelineLibrary pipelineLibrary;
        VulkanMeshLayouts meshLayouts;

        // --- MISSING RAW IMAGE ARRAYS ---
        VulkanFrameTargets frameTargets;
        VulkanRenderGraphExecutor renderGraph_;
        // Pass/resource ids of the bound plan, refreshed on every rebuild.
        VulkanProductionGraphIds graphIds_{};
        // R3c.9: forward surfaces and refraction pyramids; layered glass.
        VulkanForwardFeature forward_;
        VulkanLayeredTransparencyFeature layered_;
        // R3c.3: WeightedOIT accumulation/resolve and instance capacity.
        VulkanWeightedOitFeature oit_;
        // R3c.4: validation readback, scene-color and final capture hooks.
        VulkanHookPasses hooks_;
        FrameTopologyPreparation frameTopologyPrewarm_{};

        // G-Buffer Raw Images

        // R3c.8: deferred lighting (render pass, pipeline, the lighting set,
        // environment products) and the camera uniforms/global sets.
        VulkanDeferredLightingFeature lighting_;
        VulkanViewUniforms view_;
        // R3c.1: clustered lighting and probe clustering.
        VulkanClusterLightingFeature clusterLighting_;
        // R3c.6: probe capture, probe buffers and environment tables.
        VulkanReflectionProbeFeature probes_;
        // R3c.5: directional cascades + M7.8 VSM, and spot/point shadows.
        VulkanShadowFeature shadows_;
        VulkanLocalShadowFeature localShadows_;


        // Translucency Pass Raw Images

        // Depth Pass

        // R3c.2: output transform, HDR10 encode, LUT, exposure, grid overlay.
        VulkanOutputFeature output_;
        VulkanTemporalAntiAliasingFeature taa_;   // M9.2

        // R3c.10: the UI pass (clear, the editor bridge's contribution,
        // present). The editor bridge (renderer/vulkan_imgui) is an attached
        // extension; it owns ImGui, the editor textures and the retained
        // views. The depth-pyramid history follows the retained view it
        // selects.
        VulkanUiFeature ui_;
        uint32_t retainedRenderView_ = 0;
        [[nodiscard]] IVulkanEditorUi* editorUi() const noexcept {
            return extensionHooks_.editorUi();
        }
        [[nodiscard]] VulkanEditorUiPresentation editorUiPresentation() const noexcept;

        // GPU-driven shadow/probe caster compaction (M7R R3a). The shared
        // 3-binding indirect set layout is owned here; each culler owns its
        // pipeline, sets, buffers, scratch and validation slots.
        VulkanCullerDevice cullerDevice_{};
        VkDescriptorSetLayout indirectCullerSetLayout_ = VK_NULL_HANDLE;
        // Caster scratch shared by the shadow owners and the probe capture.
        VulkanCasterScratch casterScratch_;
        // M7R R5c.1: the shadow submission's change-driven revisions (local
        // shadows, probe scene revision, VSM clip key, directional cascades).
        VulkanShadowCasterRevisions casterRevisions_;
        // R3c.7: G-buffer pass and pipelines, the main-view opaque culler,
        // the G-buffer draw loops and the depth pyramid with its history.
        VulkanOpaqueFeature opaque_;
        using ResolvedShadowCaster = VulkanResolvedCaster;

        // The slot's CPU GPU-scene mirror with the published counts.
        [[nodiscard]] VulkanIndirectScene indirectScene(
            uint32_t frame) const noexcept;
        // Geometry/material/pipeline payload lookups for the indirect cullers.
        [[nodiscard]] VulkanIndirectAssetResolver indirectAssets() const noexcept;
        [[nodiscard]] VulkanIndirectViewSettings shadowViewSettings() const noexcept;
        static constexpr uint32_t MaximumOpaqueIndirectCommandCapacity = 65536u;

        // --- 2. SHARED STATE (M7R R3c.0) ---
        // Vaults, samplers and material descriptors; the GPU-scene tables;
        // the per-frame counters; the attached extensions. Feature owners
        // reach them through featureContext_.
        VulkanResourceRegistry resources_;
        VulkanGpuSceneState gpuScene_;
        VulkanFrameTelemetry telemetry_;
        VulkanExtensionHooks extensionHooks_;
        // R4c.4: created right after the device, saved and destroyed at
        // cleanup before the device.
        VulkanPipelineCache pipelineCache_;
        std::optional<VulkanFeatureContext> featureContext_;


        // --- 3. RUNTIME STATE ---
        uint32_t currentImageIndex = 0;
        VkCommandBuffer currentCmd = VK_NULL_HANDLE;
        bool initialized_ = false;
        bool cleaned_ = false;
        bool frameOpen_ = false;
        CpuProfiler* cpuProfiler_ = nullptr;
        bool forceDirectGBufferReference_ = false;
        bool forceDirectShadowReference_ = false;
        // M7R R4b.4: compile the production graph with transient aliasing.
        bool renderGraphAliasing_ = true;
        AntiAliasingMode antiAliasing_ = AntiAliasingMode::None;   // M9.2
        TemporalAntiAliasingTuning taaTuning_{};
        // M7R R4d: --upload-queue.
        UploadQueueMode uploadQueueMode_ = UploadQueueMode::Auto;
        float experimentalShadowLodErrorTexels_ = 0.0f;
        uint32_t shadowLodMaximumLevel_ = 15u;
        float experimentalProbeLodErrorPixels_ = 0.0f;
        uint32_t probeLodMaximumLevel_ = 15u;
        uint64_t externalSwapchainRequestedPeakBytes_ = 0;
        uint64_t externalSwapchainPeakImageCount_ = 0;
        RenderDebugView debugView_ = RenderDebugView::Final;
        VkExtent2D sceneExtent_{};
        GBufferLayout gBufferLayout_ = GBufferLayout::CanonicalReference;
        ClusterGridConfig clusterConfig_{};
        uint32_t directionalShadowResolution_ = 4096;
        uint32_t spotShadowAtlasResolution_ = 8192;
        std::array<uint32_t, 3> pointShadowCapacities_{
            kPointShadowPool256Capacity, kPointShadowPool512Capacity,
            kPointShadowPool1024Capacity };
		Color::OutputTransport outputTransport_ = Color::OutputTransport::SdrSrgb;
		Color::OutputTransport requestedOutputTransport_ =
			Color::OutputTransport::SdrSrgb;
		VkFormat outputTargetFormat_ = VulkanSdrOutputFormat;
        float paperWhiteNits_ = 203.0f;
        float peakNits_ = 1000.0f;
        bool selectionOutlineActive_ = false;


        [[nodiscard]] VulkanCullerServices cullerServices();
        [[nodiscard]] VulkanIndirectViewSettings probeViewSettings() const noexcept;
        // The view cullers in collection order: directional, spot, point,
        // reflection probe.
        [[nodiscard]] std::array<VulkanIndirectViewCuller*, kIndirectViewKindCount>
            indirectViewCullers() noexcept {
            return { &shadows_.culler(), &localShadows_.spotCuller(),
                &localShadows_.pointCuller(), &probes_.culler() };
        }
        // Device telemetry (+ oracle verdict) of every view's retired slot.
        void collectIndirectViewValidations(uint32_t frameIndex);

        // Feature owners in registration (and graph) order.
        [[nodiscard]] std::array<IVulkanFeature*, 13> features() noexcept {
            return { &shadows_, &localShadows_, &probes_, &opaque_,
                &clusterLighting_, &lighting_, &forward_, &layered_, &taa_,
                &output_, &oit_, &hooks_, &ui_ };
        }
        // Between frames, after every slot retired (resize, transport and
        // topology changes): release and recreate the graph, the frame
        // targets and every descriptor set over them, and the editor's
        // target textures (the editor bridge's onFrameTargets* events).
        void releaseFrameTargets();
        void createFrameTargets();
        void releaseEditorTargetTextures();
        void registerEditorTargetTextures();
        void initFrameTargets();
        void rebuildRenderGraphAfterDeviceIdle();
        // R3b.6 imported images: swapchain (per frame) and shadow maps (global).
        void bindGraphImportedImages();
        // R3b.7 imported buffers: culler indirect command/count buffers and
        // probe-cluster buffers (per slot). Every slot, after a graph rebuild
        // (device idle).
        void bindGraphImportedBuffers();
        // M7R R4c.2: one retired slot's imported buffers (the owners' current
        // buffers of that slot).
        void rebindGraphImportedBuffers(uint32_t slot);
        void unbindGraphImportedBuffers(uint32_t slot);
        // After capacity growth: rebinds every slot that is not in flight now;
        // an in-flight slot rebinds at its retirement.
        void rebindIdleSlotImports();
        // beginFrame, after `slot`'s fence, collects and the executor's
        // retirement: swaps every owner's parked per-slot replacement, then
        // rebinds the slot's descriptor sets and graph imports.
        void swapRetiredSlot(uint32_t slot);
        std::array<bool, VulkanFrameScheduler::FramesInFlight> importRebindPending_{};
        // R4c.3: slots whose reflection-probe environment table changed while
        // they were in flight (rebound at their retirement).
        std::array<bool, VulkanFrameScheduler::FramesInFlight>
            probeEnvironmentRebindPending_{};
        [[nodiscard]] VulkanImageResource swapchainGraphImage(
            uint32_t imageIndex) const;
        [[nodiscard]] VulkanProductionGraphFeatures
            productionGraphFeatures() const noexcept;
        void applyTransparencyPyramidTopologyChange(
            std::optional<VkExtent2D> requestedOrdinary2AtlasExtent =
                std::nullopt,
            std::optional<VkExtent2D> requestedHero4AtlasExtent =
                std::nullopt,
            std::optional<VkExtent2D> requestedCinematic8AtlasExtent =
                std::nullopt);
        void emitFrameCounters();
        void bindMaterialDescriptors(VkPipelineLayout layout);
        // The deep tier's validation readback hook (layered payload).
        void recordDeepLayeredValidationHook(TransparencyQuality quality);
        // The capture source of a capture point (scene-linear: scene.color;
        // final: the output target).
        [[nodiscard]] VulkanCaptureHookPayload captureSource(
            FrameCapturePoint point);
        [[nodiscard]] VulkanBackendServices backendServices() noexcept;
        // Oracle access for expectation-emission sites: null unless this is a
        // qualification build with an attached oracle enabled for the view.
        [[nodiscard]] IVulkanIndirectOracle* activeIndirectOracle(
            VulkanIndirectOracleView view) const noexcept {
            return Iridium::activeIndirectOracle(extensionHooks_, view);
        }
        // Command-stream digest observer (R3a.0): null unless this is a
        // qualification build with a digest requested.
        [[nodiscard]] IVulkanIndirectStreamObserver*
            activeIndirectStreamObserver() const noexcept {
            if constexpr (kQualificationBuild)
                return extensionHooks_.indirectStreamObserver();
            return nullptr;
        }
        [[nodiscard]] FrameMemoryProfile memorySnapshot();

        // submitFrame's stages (M7R R3c.11), in recording order. Each keeps
        // the drain points, CPU scopes, pipeline-statistics bracket and GPU
        // ranges the former IRenderBackend::submit* call had.
        void applyOutputSettings(const RenderFrameOutputSettings& settings);
        void updateCamera(const ViewTransportRecord& view,
            ViewHistoryContext history);
        void submitDirectionalShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows);
        void submitSpotShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const SpotShadowFramePacket> shadows);
        void submitPointShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const PointShadowFramePacket> shadows);
        void submitReflectionProbeCaptures(
            const ReflectionProbeCasterSubmission& probeCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights);
        void prepareDepthPyramidHistory(const OpaqueSubmission& opaque,
            std::span<const DrawPacket> opaqueForwardQueue);
        void submitOpaqueQueue(const OpaqueSubmission& opaque,
            std::span<const DrawPacket> selectionQueue, bool isWireframe);
        void submitLightingPass(const glm::vec3& cameraPos,
            const glm::mat4& view, const glm::mat4& proj, const glm::mat4& rasterProj,
            float nearPlane, float farPlane,
            const LightingFramePacket& lights,
            const ReflectionProbeGpuFramePacket& reflectionProbes);
        void submitForwardQueues(std::span<const DrawPacket> opaqueForwardQueue,
            std::span<const glm::mat4> opaqueForwardPreviousTransforms,
            std::span<const DrawPacket> sortedSurfaceQueue,
            std::span<const DrawPacket> compatibilityTransparentQueue,
            std::span<const glm::mat4> instanceTransforms);
        void submitOutputPass();
        void submitUIPass();

    public:
        VulkanVertexBackend() = default;
        ~VulkanVertexBackend() override { cleanup(); }

        // Factory-only, before init(). Does not take ownership.
        void attachExtension(IRenderBackendExtension* extension);

        // --- IRenderBackend Interface Implementation ---
        void init(GLFWwindow* window, const RenderBackendConfig& config) override;
        void cleanup() override;
        void recreateSwapchain(GLFWwindow* window) override;
        void setOutputTransport(GLFWwindow* window,
            Color::OutputTransport requestedTransport) override;
        [[nodiscard]] RenderExtent getRenderExtent() const override;
        [[nodiscard]] bool resizeSceneRenderExtent(
            RenderExtent extent, std::string& diagnostic) override;
        [[nodiscard]] bool setAntiAliasing(
            AntiAliasingMode mode, std::string& diagnostic) override;
        [[nodiscard]] RenderBackendCapabilities getCapabilities() const override;
        [[nodiscard]] RenderBackendRuntimeInfo getRuntimeInfo() const override;
        [[nodiscard]] FrameTopologyPreparation prepareFrameTopology(
            const FrameTopologyRequirements& requirements) override;
        void prepareLighting(uint32_t requiredCapacity) override;
        void prepareGpuScene(
            const GpuSceneCapacityRequirements& requirements) override;
        void publishGpuScene(const GpuScenePackedTables& scene) override;
        [[nodiscard]] GpuSceneFrameSerials getGpuSceneFrameSerials()
            const noexcept override {
            return { scheduler.lastSubmittedSerial(),
                scheduler.completedSerial() };
        }
        void prepareReflectionProbes(uint32_t requiredCapacity,
            std::span<const EnvironmentLightingHandles> environments) override;
        [[nodiscard]] std::span<const ReflectionProbeCaptureCompletion>
            finalizeReflectionProbeCaptures() override;
        [[nodiscard]] std::optional<uint32_t>
            capturedReflectionProbeEnvironmentSlot(
                SceneEntityUuid owner) const noexcept override;
        void synchronizeReflectionProbeCaptureOwners(
            std::span<const SceneEntityUuid> owners) override;
        void configureReflectionProbeCaptures(
            const ProjectReflectionProbeSettings& settings) override;
        FrameStatus beginFrame() override;
        void submitFrame(const RenderFrame& frame) override;
        [[nodiscard]] RenderFrameTelemetry frameTelemetry() const noexcept override;
        [[nodiscard]] uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission& shadowCasters) override;
        [[nodiscard]] std::array<uint64_t, kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(
                const ShadowCasterSubmission& shadowCasters,
                const DirectionalShadowCascadePlan& plan) override;
        FrameStatus endFrame() override;

        // Resource Allocation
        GeometryHandle allocateGeometry(const GeometryDesc& desc,
            std::span<const std::byte> vertexBytes,
            std::span<const std::byte> indexBytes) override;
        void freeGeometry(GeometryHandle handle) override;
        GeometryArenaAllocation allocateGeometryArena(
            uint32_t vertexStride,
            std::span<const std::byte> vertexBytes,
            const GeometryArenaData& arena) override;
        void freeGeometryArena(
            std::span<const GeometryHandle> primitiveGeometry) override;

        TextureHandle allocateTexture(const TextureDesc& desc,
            std::span<const std::byte> pixelBytes) override;
        void freeTexture(TextureHandle handle) override;

        MaterialBinding allocateCanonicalMaterial(
            const CanonicalMaterialAsset& desc) override;
        void updateCanonicalMaterial(MaterialHandle handle,
            const PackedGpuMaterial& material) override;
        void freeMaterial(MaterialHandle handle) override;

        void setEnvironmentLighting(
            const EnvironmentLightingHandles& environment) override;
        [[nodiscard]] EnvironmentLightingHandles getEnvironmentLighting() const override {
            return lighting_.environment();
        }
        void setEnvironmentLightingSettings(
            const EnvironmentLightingSettings& settings) override;
        void setOutputTransformLut(TextureHandle lutHandle) override;
    };

} // namespace Iridium
