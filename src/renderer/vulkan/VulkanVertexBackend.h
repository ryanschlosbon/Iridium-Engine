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

#include "VkContext.h"
#include "VkSwapchain.h"
#include "DescriptorAllocator.h"

// Pipelines & Passes
#include "VkGraphicsPipeline.h"
#include "VkLightingPipeline.h"
#include "GlassDepthPipeline.h"
#include "VkRenderPass.h"
#include "VkForwardRenderPass.h"
#include "GlassDepthRenderPass.h"
#include "VkUIRenderPass.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceAllocator.h"
#include "VulkanCommandList.h"
#include "VulkanUploadContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanFrameTargets.h"
#include "VulkanSceneDescriptors.h"
#include "VulkanRenderGraphExecutor.h"
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

#include "utils/DeletionQueue.h"

namespace Iridium {

    // ==============================================================================
    // THE INTERNAL PAYLOADS
    // These structs only exist inside the Backend. The ECS never sees them.
    // ==============================================================================

    struct VulkanGeometryPayload {
        VulkanBufferResource vertexBuffer;
        VulkanBufferResource indexBuffer;
        uint32_t indexCount = 0;
        IndexFormat indexFormat = IndexFormat::UInt32;
    };

    struct VulkanTexturePayload {
        VulkanImageResource image;
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t samplerCacheIndex = UINT32_MAX;
        VkDescriptorSet imguiDescriptor = VK_NULL_HANDLE;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        uint32_t width = 0;
        uint32_t height = 0;
        bool retired = false;
    };

    struct VulkanMaterialPayload {
        PipelineHandle pipeline;
        RenderQueue renderQueue = RenderQueue::Opaque;
        PackedGpuMaterial packed{};
        uint64_t packedRevision = 0;
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedPackedRevisions{};
    };

    // ==============================================================================
    // THE CONCRETE BACKEND
    // ==============================================================================

    class VulkanVertexBackend : public IRenderBackend {
    private:
        struct FrameCounters {
            uint64_t drawOpaque = 0;
            uint64_t drawSelection = 0;
            uint64_t drawShadowDirectional = 0;
            uint64_t drawShadowDirectionalAlphaMask = 0;
            uint64_t drawShadowSpot = 0;
            uint64_t drawShadowSpotAlphaMask = 0;
            uint64_t shadowSpotCastersTested = 0;
            uint64_t shadowSpotCastersCulled = 0;
            uint64_t drawShadowPoint = 0;
            uint64_t drawShadowPointAlphaMask = 0;
            uint64_t shadowPointCastersTested = 0;
            uint64_t shadowPointCastersCulled = 0;
            uint64_t drawLighting = 0;
            uint64_t drawOutput = 0;
            uint64_t drawTransparentDepth = 0;
            uint64_t drawTransparentForward = 0;
            uint64_t drawStandardForward = 0;
            uint64_t drawComplexForward = 0;
            uint64_t drawUnlitForward = 0;
            std::array<uint64_t, 8> complexLobeDraws{};
            uint64_t drawUi = 0;
            uint64_t dispatchRecorded = 0;
            uint64_t trianglesSubmitted = 0;
            uint64_t materialBinds = 0;
            uint64_t pipelineBinds = 0;
            uint64_t transparentBackgroundPackets = 0;
            uint64_t transparentForegroundPackets = 0;
            uint64_t transparentNonemptyBuckets = 0;
            uint64_t uiUntrackedCallbacks = 0;
            uint64_t materialUniqueOverflow = 0;
            uint64_t pipelineUniqueOverflow = 0;
        };

        static constexpr size_t MaxUniqueResourcesPerFrame = 512;

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
        VulkanIndexedTextureTable indexedTextureTable_;

        // G-Buffer Pass (Opaque)
        std::unique_ptr<VkRenderPassWrapper> gBufferPass;
        std::unique_ptr<VkGraphicsPipeline> gBufferPipeline;

        // --- MISSING RAW IMAGE ARRAYS ---
        VulkanFrameTargets frameTargets;
        VulkanRenderGraphExecutor renderGraph_;

        // G-Buffer Raw Images

        // Lighting Pass Raw Images & Descriptors
        VulkanSceneDescriptors sceneDescriptors;
        VulkanClusteredLightingPipeline clusteredLighting_;
        VulkanReflectionProbePipeline reflectionProbePipeline_;
        VulkanReflectionProbeCapturePass reflectionProbeCapturePass_;
        VulkanReflectionProbeCaptureTargets reflectionProbeCaptureTargets_;
        VulkanDirectionalShadowMap directionalShadow_;
        VulkanSpotShadowAtlas spotShadow_;
        VulkanPointShadowPools pointShadow_;
        EnvironmentLightingHandles environmentLighting_;
        EnvironmentLightingSettings environmentLightingSettings_;
        TextureHandle neutralEnvironmentCube_;
        TextureHandle neutralEnvironmentBrdfLut_;

        struct CachedSampler {
            SamplerDesc desc{};
            VkSampler sampler = VK_NULL_HANDLE;
            uint32_t referenceCount = 0;
        };
        std::vector<CachedSampler> samplerCache_;

        // Translucency Pass Raw Images

        // Depth Pass

        // Deferred Lighting Pass
        VkRenderPass lightingRenderPass = VK_NULL_HANDLE;
        std::unique_ptr<VkLightingPipeline> lightingPipeline;

        // Translucency Passes
        std::unique_ptr<GlassDepthRenderPass> glassDepthPass;
        std::unique_ptr<GlassDepthPipeline> glassDepthPipeline;

        std::unique_ptr<VkForwardRenderPass> forwardPass;

        VulkanOutputPass outputPass;
        VulkanHdrEncodePass hdrEncodePass;

        // UI Pass
        std::unique_ptr<VkUIRenderPass> uiPass;

        // --- IMGUI STATE ---
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> uiSceneTextures;
        std::vector<VkDescriptorSet> uiDepthTextures;
        std::vector<uint32_t> imguiFragmentShaderCode_;

        // Global Camera Data
        std::vector<VulkanBufferResource> uniformBuffers;
        std::vector<VkDescriptorSet> globalDescriptorSets;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            canonicalMaterialBuffers_{};
        uint32_t canonicalMaterialCapacity_ = 0;
        uint32_t canonicalMaterialMaximumCapacity_ = 0;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            lightRecordBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            activeLightSlotBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            fallbackCandidateBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            clusterParameterBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            clusterDiagnosticReadbackBuffers_{};
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
        std::array<bool, VulkanFrameScheduler::FramesInFlight>
            clusterDiagnosticReadbackPending_{};
        std::array<uint32_t, VulkanFrameScheduler::FramesInFlight>
            submittedClusterCounts_{};
        ClusteredLightingTelemetry clusterTelemetry_{};
        std::array<std::vector<uint64_t>, VulkanFrameScheduler::FramesInFlight>
            uploadedLightRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedActiveListRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedSpotShadowMappingRevisions_{};
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedPointShadowMappingRevisions_{};
        std::vector<uint32_t> spotShadowDataSlots_;
        std::vector<uint32_t> pointShadowDataSlots_;
        std::vector<PackedGpuLight> patchedLightRecordsScratch_;
        uint64_t spotShadowMappingRevision_ = 1;
        uint64_t pointShadowMappingRevision_ = 1;
        std::vector<uint32_t> fallbackSelectionScratch_;
        std::vector<LightRecordRange> lightUploadRanges_;
        uint32_t lightRecordCapacity_ = 0;
        uint32_t lightRecordMaximumCapacity_ = 0;
        uint32_t activeLightCount_ = 0;
        uint64_t lightUploadBytes_ = 0;
        uint32_t lightUploadRangeCount_ = 0;
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

        // --- 2. THE MEMORY VAULTS ---
        // This is where the Handles are mapped to the physical Vulkan memory.
        ResourcePool<VulkanGeometryPayload, GeometryHandle> geometryVault;
        ResourcePool<VulkanTexturePayload, TextureHandle> textureVault;
        ResourcePool<VulkanMaterialPayload, MaterialHandle> materialVault;


        // --- 3. RUNTIME STATE ---
        uint32_t currentImageIndex = 0;
        VkCommandBuffer currentCmd = VK_NULL_HANDLE;
        bool initialized_ = false;
        bool cleaned_ = false;
        bool frameOpen_ = false;
        bool imguiInitialized_ = false;
        CpuProfiler* cpuProfiler_ = nullptr;
        bool collectFrameCounters_ = false;
        FrameCounters frameCounters_{};
        std::vector<uint32_t> uniqueMaterialIds_;
        std::vector<uint64_t> uniquePipelineIds_;
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
        float manualExposureEv_ = 0.0f;
        OutputTransformOperator outputOperator_ = OutputTransformOperator::Aces2;
		Color::OutputTransport outputTransport_ = Color::OutputTransport::SdrSrgb;
		VkFormat outputTargetFormat_ = VulkanSdrOutputFormat;
        float paperWhiteNits_ = 203.0f;
        float peakNits_ = 1000.0f;
        bool selectionOutlineActive_ = false;
        uint64_t retiredTextureCount_ = 0;
        TextureHandle outputTransformLut_{};
        bool finalCaptureHookRecorded_ = false;

        struct PendingFrameCapture {
            uint64_t captureId = 0;
            uint32_t frameIndex = 0;
            VkExtent2D extent{};
            VkFormat format = VK_FORMAT_UNDEFINED;
            FrameCapturePoint point = FrameCapturePoint::SceneLinear;
            VulkanBufferResource readback;
        };
        std::vector<PendingFrameCapture> pendingFrameCaptures_;
        std::vector<FrameCapture> completedFrameCaptures_;

        // Private helpers that Application.cpp no longer needs to worry about
        void createUniformBuffers();
        void createCanonicalMaterialBuffers(uint32_t capacity);
        void ensureCanonicalMaterialCapacity(uint32_t requiredCapacity);
        void uploadCanonicalMaterialsForFrame(uint32_t frameIndex);
        void createLightRecordBuffers(uint32_t capacity);
        void bindLightRecordBuffers();
        void bindClusterBuffers();
        void bindSceneClusterBuffers();
        void createNeutralEnvironmentProducts();
        void bindEnvironmentProducts();
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
        void uploadLightsForFrame(uint32_t frameIndex,
            const LightingFramePacket& lights);
        void updateClusterParameters(uint32_t frameIndex,
            const glm::mat4& view, const glm::mat4& projection,
            float nearPlane, float farPlane, uint32_t activeLightCount);
        void updateClusterFallbackCandidates(uint32_t frameIndex,
            const glm::mat4& view, const LightingFramePacket& lights);
        void collectClusterDiagnostics(uint32_t frameIndex) noexcept;
        void initFrameTargets();
        void rebuildRenderGraphAfterDeviceIdle();
        void updateUniformBuffer(const glm::mat4& view, const glm::mat4& proj);
        void createLightingRenderPass();
        void resetFrameCounters();
        void recordMaterialBind(MaterialHandle material);
        void recordPipelineBind(uint64_t pipelineIdentity);
        void recordDraw(uint64_t& drawCounter, uint64_t submittedTriangles);
        void emitFrameCounters();
        void bindMaterialDescriptors(VkPipelineLayout layout);
        [[nodiscard]] uint32_t acquireSampler(const SamplerDesc& desc);
        void releaseSampler(uint32_t cacheIndex) noexcept;
        void cleanupSamplerCache() noexcept;
        [[nodiscard]] uint64_t liveSamplerCount() const noexcept;
        void collectFrameCapturesForSlot(uint32_t frameIndex);
        void destroyPendingFrameCaptures() noexcept;
        [[nodiscard]] static std::optional<FrameCapturePixelFormat>
            capturePixelFormat(VkFormat format) noexcept;
        [[nodiscard]] FrameMemoryProfile memorySnapshot();

    public:
        VulkanVertexBackend() = default;
        ~VulkanVertexBackend() override { cleanup(); }

        // --- IRenderBackend Interface Implementation ---
        void init(GLFWwindow* window, const RenderBackendConfig& config) override;
        void cleanup() override;
        void recreateSwapchain(GLFWwindow* window) override;
        [[nodiscard]] RenderExtent getRenderExtent() const override;
        [[nodiscard]] bool resizeSceneRenderExtent(
            RenderExtent extent, std::string& diagnostic) override;
        [[nodiscard]] RenderBackendCapabilities getCapabilities() const override;
        [[nodiscard]] RenderBackendRuntimeInfo getRuntimeInfo() const override;
        void prepareLighting(uint32_t requiredCapacity) override;
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
        void updateCamera(const glm::mat4& view, const glm::mat4& proj) override;
        void setDebugView(RenderDebugView view) override { debugView_ = view; }
        void setOutputSettings(float manualExposureEv, float paperWhiteNits,
            float peakNits) override;
        void submitDirectionalShadows(
            std::span<const DrawPacket> shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows) override;
        void submitSpotShadows(
            std::span<const DrawPacket> shadowCasters,
            std::span<const SpotShadowFramePacket> shadows) override;
        void submitPointShadows(
            std::span<const DrawPacket> shadowCasters,
            std::span<const PointShadowFramePacket> shadows) override;
        void submitReflectionProbeCaptures(
            std::span<const DrawPacket> opaqueCasters,
            std::span<const DrawPacket> complexOpaqueCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights) override;
        [[nodiscard]] ReflectionProbeCaptureTelemetry
            getReflectionProbeCaptureTelemetry() const noexcept override {
            return reflectionProbeCaptureTelemetry_;
        }
        [[nodiscard]] uint64_t getShadowCasterRevision(
            std::span<const DrawPacket> shadowCasters) const noexcept override;

        void submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> selectionQueue, bool isWireframe) override;
        void submitLightingPass(const glm::vec3& cameraPos,
            const glm::mat4& view, const glm::mat4& proj,
            float nearPlane, float farPlane,
            const LightingFramePacket& lights,
            const ReflectionProbeGpuFramePacket& reflectionProbes) override;
        [[nodiscard]] LightingUploadTelemetry
            getLightingUploadTelemetry() const noexcept override {
            return { lightUploadBytes_, lightUploadRangeCount_,
                activeLightCount_, lightRecordCapacity_ };
        }
        [[nodiscard]] ClusteredLightingTelemetry
            getClusteredLightingTelemetry() const noexcept override {
            return clusterTelemetry_;
        }
        void submitForwardQueues(std::span<const DrawPacket> opaqueForwardQueue,
            std::span<const DrawPacket> transparentQueue) override;
        void captureCurrentFrame(uint64_t captureId,
            FrameCapturePoint point) override;
        [[nodiscard]] std::vector<FrameCapture> collectFrameCaptures(
            bool waitForPending) override;
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
        void setEnvironmentLightingSettings(
            const EnvironmentLightingSettings& settings) override;
        void setOutputTransformLut(TextureHandle lutHandle) override;
    };

} // namespace Iridium
