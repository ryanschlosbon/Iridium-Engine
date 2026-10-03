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
#include "VkRenderPass.h"
#include "VkForwardRenderPass.h"
#include "VkUIRenderPass.h"
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
#include "VulkanClusterLightingFeature.h"
#include "VulkanOutputFeature.h"
#include "VulkanWeightedOitFeature.h"
#include "VulkanExtensionHooks.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanGpuSceneState.h"
#include "VulkanResourceRegistry.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanIndirectViewCuller.h"
#include "VulkanOpaqueIndirectCuller.h"
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

        // G-Buffer Pass (Opaque)
        std::unique_ptr<VkRenderPassWrapper> gBufferPass;
        std::unique_ptr<VkGraphicsPipeline> gBufferPipeline;

        // --- MISSING RAW IMAGE ARRAYS ---
        VulkanFrameTargets frameTargets;
        VulkanRenderGraphExecutor renderGraph_;
        // Pass/resource ids of the bound plan, refreshed on every rebuild.
        VulkanProductionGraphIds graphIds_{};
        VulkanTransparencyPyramid transparencyPyramid_;
        VulkanDepthPyramid depthPyramid_;
        bool depthPyramidEnabled_ = false;
        VulkanLayeredInterfaceCapturePass layeredInterfaceCapture_;
        VulkanLayeredLocalCompositionPass layeredLocalComposition_;
        VulkanLayeredSceneResolvePass layeredSceneResolve_;
        // R3c.3: WeightedOIT accumulation/resolve and instance capacity.
        VulkanWeightedOitFeature oit_;
        TransparencyPyramidResidency transparencyPyramidResidency_;
        TransparencyPyramidResidency ordinary2AtlasResidency_;
        TransparencyPyramidResidency hero4AtlasResidency_;
        TransparencyPyramidResidency cinematic8AtlasResidency_;
        TransparencyPyramidResidency weightedOitResidency_;
        VkExtent2D ordinary2AtlasExtent_{};
        VkExtent2D hero4AtlasExtent_{};
        VkExtent2D cinematic8AtlasExtent_{};
        Ordinary2RequestCollector ordinary2RequestCollector_;
        Ordinary2AtlasPlan ordinary2AtlasPlan_;
        Ordinary2CaptureDrawPlan ordinary2CaptureDrawPlan_;
        LayeredRequestCollector deepLayeredRequestCollector_;
        LayeredAtlasPlan deepLayeredAtlasPlan_;
        LayeredCaptureDrawPlan deepLayeredCaptureDrawPlan_;
        std::array<Ordinary2CaptureDraw, kOrdinary2MaximumWorkCount>
            ordinary2ResolvedDraws_{};
        uint32_t ordinary2ResolvedPacketCount_ = 0u;
        std::array<LayeredCaptureDraw, kOrdinary2MaximumWorkCount>
            deepResolvedDraws_{};
        uint32_t deepResolvedPacketCount_ = 0u;
        FrameTopologyPreparation frameTopologyPrewarm_{};

        // G-Buffer Raw Images

        // Lighting Pass Raw Images & Descriptors
        VulkanSceneDescriptors sceneDescriptors;
        // R3c.1: clustered lighting and probe clustering.
        VulkanClusterLightingFeature clusterLighting_;
        VulkanReflectionProbeCapturePass reflectionProbeCapturePass_;
        VulkanReflectionProbeCaptureTargets reflectionProbeCaptureTargets_;
        VulkanDirectionalShadowMap directionalShadow_;
        VulkanSpotShadowAtlas spotShadow_;
        VulkanPointShadowPools pointShadow_;
        VulkanVirtualShadowResources virtualShadowResources_;
        DirectionalVirtualShadowClipPublisher virtualShadowClipPublisher_;
        std::optional<DirectionalVirtualShadowClipPlan> virtualShadowClipPlan_;
        std::array<std::optional<DirectionalVirtualShadowClipPlan>, VulkanFrameScheduler::FramesInFlight>
            virtualShadowFrameClipPlans_;
        std::vector<VirtualShadowCasterBounds> virtualShadowCasterBoundsScratch_;
        uint32_t virtualShadowClipPageSize_ = 128;
        std::array<VkImageView, VulkanFrameScheduler::FramesInFlight> virtualShadowDepthBindings_{};
        std::array<bool, VulkanFrameScheduler::FramesInFlight> virtualShadowReadbackPending_{};
        void collectVirtualShadowRequests(uint32_t slot);
        EnvironmentLightingHandles environmentLighting_;
        EnvironmentLightingSettings environmentLightingSettings_;
        TextureHandle neutralEnvironmentCube_;
        TextureHandle neutralEnvironmentBrdfLut_;


        // Translucency Pass Raw Images

        // Depth Pass

        // Deferred Lighting Pass
        VkRenderPass lightingRenderPass = VK_NULL_HANDLE;
        std::unique_ptr<VkLightingPipeline> lightingPipeline;

        // Translucency Passes

        std::unique_ptr<VkForwardRenderPass> forwardPass;
        std::unique_ptr<VkForwardRenderPass> transparentPass;

        // R3c.2: output transform, HDR10 encode, LUT, exposure, grid overlay.
        VulkanOutputFeature output_;

        // UI Pass
        std::unique_ptr<VkUIRenderPass> uiPass;

        // --- IMGUI STATE ---
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> uiSceneTextures;
        std::array<VulkanImageResource, 2> retainedViewImages_{};
        std::array<VkDescriptorSet, 2> retainedViewDescriptors_{};
        VkSampler retainedViewSampler_ = VK_NULL_HANDLE;
        uint32_t retainedRenderView_ = 0;
        bool retainedViewsEnabled_ = false;
        void destroyRetainedViews();
        void publishRetainedView();
        std::vector<VkDescriptorSet> uiDepthTextures;
        std::vector<uint32_t> imguiFragmentShaderCode_;

        // Global Camera Data
        std::vector<VulkanBufferResource> uniformBuffers;
        std::vector<VkDescriptorSet> globalDescriptorSets;

        // GPU-driven shadow/probe caster compaction (M7R R3a). The shared
        // 3-binding indirect set layout is owned here; each culler owns its
        // pipeline, sets, buffers, scratch and validation slots.
        VulkanCullerDevice cullerDevice_{};
        VkDescriptorSetLayout indirectCullerSetLayout_ = VK_NULL_HANDLE;
        VulkanIndirectViewCuller directionalCuller_;
        VulkanIndirectViewCuller spotCuller_;
        VulkanIndirectViewCuller pointCuller_;
        VulkanIndirectViewCuller probeCuller_;
        // Main-view opaque compaction (sibling of the view cullers).
        VulkanOpaqueIndirectCuller opaqueCuller_;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeRecordBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeActiveSlotBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeParameterBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeClusterHeaderBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            reflectionProbeClusterIndexBuffers_{};
        using ResolvedShadowCaster = VulkanResolvedCaster;

        std::vector<uint32_t> spotShadowMappingScratch_;
        std::vector<uint32_t> pointShadowMappingScratch_;
        std::vector<ResolvedShadowCaster> shadowCasterScratch_;
        std::vector<uint8_t> directionalShadowCasterMaskScratch_;

        [[nodiscard]] bool resolveGpuSceneCaster(uint32_t primitiveIndex,
            uint32_t consumerMask,
            ResolvedShadowCaster& caster) const noexcept;
        // The slot's CPU GPU-scene mirror with the published counts.
        [[nodiscard]] VulkanIndirectScene indirectScene(
            uint32_t frame) const noexcept;
        // Geometry/material/pipeline payload lookups for the indirect cullers.
        [[nodiscard]] VulkanIndirectAssetResolver indirectAssets() const noexcept;
        template<typename Visitor>
        void visitShadowCasters(const ShadowCasterSubmission& submission,
            Visitor&& visitor) const;
        template<typename Visitor>
        void visitReflectionProbeCasters(
            const ReflectionProbeCasterSubmission& submission,
            Visitor&& visitor) const;
        static constexpr uint32_t MaximumOpaqueIndirectCommandCapacity = 65536u;
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedReflectionProbeRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedReflectionProbeActiveListRevisions_{};
        std::vector<ReflectionProbeRecordRange>
            reflectionProbeUploadRanges_;
        std::vector<EnvironmentLightingHandles>
            reflectionProbeEnvironments_;
        uint32_t reflectionProbeRecordCapacity_ = 0;
        uint32_t reflectionProbeRecordMaximumCapacity_ = 0;
        uint32_t reflectionProbeClusterCapacity_ = 0;
        uint32_t reflectionProbeReferenceCapacity_ = 0;
        struct PendingReflectionProbeCapture {
            SceneEntityUuid owner;
            uint64_t captureTicket = 0;
            std::vector<VkDescriptorSet> filterDescriptors;
            VulkanReflectionProbeCaptureReadback bakedReadback;
            uint32_t resolution = 0;
            uint32_t mipLevels = 0;
        };
        std::vector<PendingReflectionProbeCapture>
            pendingReflectionProbeCaptures_;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash>
            capturedReflectionProbeSlots_;
        ReflectionProbeCaptureTelemetry reflectionProbeCaptureTelemetry_{};
        uint32_t reflectionProbePrefilterSampleCount_ = 256;

        // --- 2. SHARED STATE (M7R R3c.0) ---
        // Vaults, samplers and material descriptors; the GPU-scene tables;
        // the per-frame counters; the attached extensions. Feature owners
        // reach them through featureContext_.
        VulkanResourceRegistry resources_;
        VulkanGpuSceneState gpuScene_;
        VulkanFrameTelemetry telemetry_;
        VulkanExtensionHooks extensionHooks_;
        std::optional<VulkanFeatureContext> featureContext_;


        // --- 3. RUNTIME STATE ---
        uint32_t currentImageIndex = 0;
        VkCommandBuffer currentCmd = VK_NULL_HANDLE;
        bool initialized_ = false;
        bool cleaned_ = false;
        bool frameOpen_ = false;
        ViewHistoryContext currentViewHistory_{};
        uint64_t currentProjectionRevision_ = 1;
        uint64_t currentDepthContentRevision_ = 1;
        DepthPyramidHistoryDecision currentDepthHistoryDecision_{};
        bool depthHistoryPrepared_ = false;
        bool imguiInitialized_ = false;
        CpuProfiler* cpuProfiler_ = nullptr;
        bool forceDirectGBufferReference_ = false;
        bool forceDirectShadowReference_ = false;
        float experimentalShadowLodErrorTexels_ = 0.0f;
        uint32_t shadowLodMaximumLevel_ = 15u;
        bool depthOcclusionQueryEnabled_ = false;
        bool depthOcclusionRejectionEnabled_ = false;
        float experimentalGpuLodErrorPixels_ = 0.0f;
        uint32_t gpuLodMaximumLevel_ = 15u;
        float gpuLodHysteresisFraction_ = 0.15f;
        float experimentalProbeLodErrorPixels_ = 0.0f;
        uint32_t probeLodMaximumLevel_ = 15u;
        glm::mat4 ordinary2ViewProjection_{ 1.0f };
        bool ordinary2ViewProjectionValid_ = false;
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
        bool finalCaptureHookRecorded_ = false;
        // "probe.capture" begun or skipped this frame (R3b.8).
        bool probeCaptureHandled_ = false;


        // Private helpers that Application.cpp no longer needs to worry about
        void createUniformBuffers();
        void createGpuSceneCullPipeline();
        [[nodiscard]] VulkanCullerServices cullerServices();
        void createDirectionalShadowIndirectPipeline();
        [[nodiscard]] bool prepareDirectionalShadowIndirectSubmission(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows);
        void createSpotShadowIndirectPipeline();
        [[nodiscard]] bool prepareSpotShadowIndirectSubmission(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const SpotShadowFramePacket> shadows);
        void createPointShadowIndirectPipeline();
        [[nodiscard]] bool preparePointShadowIndirectSubmission(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const PointShadowFramePacket> shadows);
        void createReflectionProbeIndirectPipeline();
        // The view cullers in collection order: directional, spot, point,
        // reflection probe.
        [[nodiscard]] std::array<VulkanIndirectViewCuller*, kIndirectViewKindCount>
            indirectViewCullers() noexcept {
            return { &directionalCuller_, &spotCuller_, &pointCuller_,
                &probeCuller_ };
        }
        // Device telemetry (+ oracle verdict) of every view's retired slot.
        void collectIndirectViewValidations(uint32_t frameIndex);
        // Draw counters for the GPU-scene casters an oracle-checked shadow
        // work item drew (visibility[i] & visibilityBit).
        void recordIndirectOracleDraws(std::span<const uint8_t> visibility,
            uint8_t visibilityBit, uint64_t& drawCounter,
            uint64_t& commandCounter, uint64_t& alphaMaskCounter);
        [[nodiscard]] bool prepareReflectionProbeIndirectSubmission(
            const ReflectionProbeCasterSubmission& probeCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures);
        void recordReflectionProbeIndirectDispatch(uint32_t frameIndex,
            uint32_t faceRecord, uint32_t excludedInstanceIndex);

        [[nodiscard]] bool prepareOpaqueIndirectSubmission(
            std::span<const DrawPacket> opaqueQueue);
        void bindLightRecordBuffers();
        void bindSceneClusterBuffers();
        void createNeutralEnvironmentProducts();
        void bindEnvironmentProducts(uint32_t frame = UINT32_MAX);
        std::array<EnvironmentLightingHandles, VulkanFrameScheduler::FramesInFlight> frameEnvironments_{};
        void bindDirectionalShadowDescriptors();
        void bindSpotShadowDescriptors();
        void bindPointShadowDescriptors();
        void createReflectionProbeBuffers(uint32_t recordCapacity,
            uint32_t clusterCapacity, uint32_t referenceCapacity);
        void bindReflectionProbeBuffers();
        void bindReflectionProbeEnvironments();
        void uploadReflectionProbesForFrame(uint32_t frameIndex,
            const ReflectionProbeGpuFramePacket& probes);
        void updateReflectionProbeParameters(uint32_t frameIndex,
            const glm::mat4& view, const glm::mat4& projection,
            float nearPlane, float farPlane, uint32_t activeProbeCount);
        // Feature owners in registration (and graph) order.
        [[nodiscard]] std::array<IVulkanFeature*, 3> features() noexcept {
            return { &clusterLighting_, &output_, &oit_ };
        }
        void initFrameTargets();
        void rebuildRenderGraphAfterDeviceIdle();
        // R3b.6 imported images: swapchain (per frame) and shadow maps (global).
        void bindGraphImportedImages();
        // R3b.7 imported buffers: culler indirect command/count buffers and
        // probe-cluster buffers (per slot). Waits for every frame in flight.
        void bindGraphImportedBuffers();
        // R3b.9: the depth-pyramid history import follows the retained view;
        // `reset` after the history images are rebuilt.
        void bindDepthPyramidHistory(bool reset);
        uint32_t depthHistoryBoundView_ = UINT32_MAX;
        std::array<RenderGraph::Access, VulkanDepthPyramid::HistoryViewCount>
            depthHistoryAccess_{};
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
        void updateUniformBuffer(const glm::mat4& view, const glm::mat4& proj);
        void createLightingRenderPass();
        void emitFrameCounters();
        void bindMaterialDescriptors(VkPipelineLayout layout);
        void recordOrdinary2InterfaceCapture(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws,
            bool exitCapture);
        void recordOrdinary2Captures(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws);
        void recordDeepLayeredInterfaceCapture(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality, uint32_t interfaceIndex);
        void recordDeepLayeredTileTermination(
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality, uint32_t interfaceIndex);
        void recordDeepLayeredCaptures(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality);
        void recordDeepLayeredLocalComposition(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality);
        void recordDeepLayeredSceneResolve(
            std::span<const DrawPacket> packets,
            std::span<const LayeredCaptureDraw> draws);
        void recordOrdinary2LocalComposition(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws);
        void recordOrdinary2SceneResolve(
            std::span<const DrawPacket> packets,
            std::span<const Ordinary2CaptureDraw> draws);
        void prepareOrdinary2ResolvedPacketIndices(
            std::span<const Ordinary2CaptureDraw> draws);
        [[nodiscard]] bool isOrdinary2PacketResolved(
            uint32_t packetIndex) const noexcept;
        void prepareDeepResolvedPacketIndices(
            std::span<const LayeredCaptureDraw> draws);
        [[nodiscard]] bool isDeepPacketResolved(
            uint32_t packetIndex) const noexcept;
        [[nodiscard]] bool isLayeredPacketResolved(
            uint32_t packetIndex) const noexcept;
        void recordDeepLayeredValidationHook(
            std::span<const LayeredCaptureDraw> draws,
            TransparencyQuality quality);
        // Extension hooks (R2.7). A pass hook does nothing when undeclared,
        // skips its pass when no extension wants it, or begins the GPU range
        // and pass and calls every extension that wants it.
        void runPassHook(const VulkanHookContext& context, bool declared,
            RenderGraph::PassId pass, const char* gpuRangeName);
        // Brackets a capture copy: scene-linear copies run in the declared
        // scene-color-capture-hook pass (output-transform returns scene.color
        // to SampledRead); final output runs in final-capture-hook.
        template<typename Record>
        void recordCaptureCopy(FrameCapturePoint point, Record&& record);
        void runCaptureHook(VulkanHookPoint point, FrameCapturePoint capturePoint);
        [[nodiscard]] VulkanCaptureHookPayload captureSource(
            FrameCapturePoint point);
        [[nodiscard]] VulkanBackendServices backendServices() noexcept;
        // Oracle access for expectation-emission sites: null unless this is a
        // qualification build with an attached oracle enabled for the view.
        [[nodiscard]] IVulkanIndirectOracle* activeIndirectOracle(
            VulkanIndirectOracleView view) const noexcept {
            if constexpr (kQualificationBuild) {
                IVulkanIndirectOracle* oracle = extensionHooks_.indirectOracle();
                if (oracle != nullptr && oracle->enabled(view))
                    return oracle;
            }
            return nullptr;
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
        [[nodiscard]] GpuSceneUploadTelemetry getGpuSceneUploadTelemetry()
            const noexcept override { return gpuScene_.uploadTelemetry(); }
        void prepareReflectionProbes(uint32_t requiredCapacity,
            std::span<const EnvironmentLightingHandles> environments) override;
        [[nodiscard]] std::vector<ReflectionProbeCaptureCompletion>
            finalizeReflectionProbeCaptures() override;
        [[nodiscard]] std::optional<uint32_t>
            capturedReflectionProbeEnvironmentSlot(
                SceneEntityUuid owner) const noexcept override;
        void synchronizeReflectionProbeCaptureOwners(
            std::span<const SceneEntityUuid> owners) override;
        void configureReflectionProbeCaptures(
            const ProjectReflectionProbeSettings& settings) override;
        FrameStatus beginFrame() override;
        void updateCamera(const ViewTransportRecord& view,
            ViewHistoryContext history = {}) override;
        void setDebugView(RenderDebugView view) override { debugView_ = view; }
        void setOutputSettings(float manualExposureEv, float paperWhiteNits,
            float peakNits) override;
        void setViewportGridOverlay(
            const ViewportGridOverlay& overlay) override {
            output_.setGridOverlay(overlay);
        }
        void submitDirectionalShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows) override;
        void submitSpotShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const SpotShadowFramePacket> shadows) override;
        void submitPointShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const PointShadowFramePacket> shadows) override;
        void submitReflectionProbeCaptures(
            const ReflectionProbeCasterSubmission& probeCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights) override;
        [[nodiscard]] ReflectionProbeCaptureTelemetry
            getReflectionProbeCaptureTelemetry() const noexcept override {
            return reflectionProbeCaptureTelemetry_;
        }
        [[nodiscard]] uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission& shadowCasters) const noexcept override;
        [[nodiscard]] std::array<uint64_t, kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(
                const ShadowCasterSubmission& shadowCasters,
                const DirectionalShadowCascadePlan& plan) const noexcept override;
        void prepareDepthPyramidHistory(
            std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> opaqueForwardQueue) override;

        void submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> selectionQueue, bool isWireframe) override;
        void submitLightingPass(const glm::vec3& cameraPos,
            const glm::mat4& view, const glm::mat4& proj,
            float nearPlane, float farPlane,
            const LightingFramePacket& lights,
            const ReflectionProbeGpuFramePacket& reflectionProbes) override;
        [[nodiscard]] LightingUploadTelemetry
            getLightingUploadTelemetry() const noexcept override {
            return clusterLighting_.uploadTelemetry();
        }
        [[nodiscard]] ClusteredLightingTelemetry
            getClusteredLightingTelemetry() const noexcept override {
            return clusterLighting_.clusterTelemetry();
        }
        void submitForwardQueues(std::span<const DrawPacket> opaqueForwardQueue,
            std::span<const DrawPacket> sortedSurfaceQueue,
            std::span<const DrawPacket> compatibilityTransparentQueue,
            std::span<const glm::mat4> instanceTransforms = {}) override;
        void submitOutputPass() override;
        void submitUIPass() override;

        void beginUI() override;
        void* getLitSceneTextureID() override;
        void* getGlassDepthTextureID() override;
        void* getEditorTextureID(TextureHandle texture) override;

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
        [[nodiscard]] EnvironmentLightingHandles getEnvironmentLighting() const override { return environmentLighting_; }
        void prepareRetainedViews(bool enabled, uint32_t renderView) override;
        [[nodiscard]] void* getRetainedViewTextureID(uint32_t view) override;
        void setEnvironmentLightingSettings(
            const EnvironmentLightingSettings& settings) override;
        void setOutputTransformLut(TextureHandle lutHandle) override;
    };

} // namespace Iridium
