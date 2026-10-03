#pragma once

// M7R R3c.1: clustered lighting as a feature owner. Owns the clustered-light
// build pipeline, the reflection-probe clustering pipeline, the per-slot light
// records and active lists, the cluster parameters, fallback candidates and
// diagnostics readback, and registers the callbacks of
// "lighting.probe-cluster", "lighting.cluster.{clear,count,scan,fill,finalize}"
// and (when declared) "lighting.cluster.readback". The scene-descriptor
// bindings of these buffers stay with the lighting set's owner
// (VulkanDeferredLightingFeature, R3c.8), which reads them through the
// *Descriptors() queries.

#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/LightingTypes.h"

#include "VulkanClusteredLightingPipeline.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanReflectionProbePipeline.h"
#include "VulkanResourceAllocator.h"
#include "VulkanSceneDescriptors.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Iridium {

    class VulkanClusterLightingFeature final : public IVulkanFeature {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;
        using FrameBufferInfos = std::array<VkDescriptorBufferInfo, FrameCount>;

        VulkanClusterLightingFeature() = default;
        VulkanClusterLightingFeature(const VulkanClusterLightingFeature&) = delete;
        VulkanClusterLightingFeature& operator=(const VulkanClusterLightingFeature&) = delete;

        // Before create(): the grid and the device's light-record bound.
        void configure(const ClusterGridConfig& config,
            uint32_t maximumLightRecords) noexcept {
            config_ = config;
            lightRecordMaximumCapacity_ = maximumLightRecords;
        }

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void onGraphReleased() override;
        void onFrameSlotRetired(uint32_t frameIndex) override;
        void destroy() noexcept override;

        // Light-record capacity (frame boundary only). Returns true when the
        // record buffers were replaced, so the lighting set must rebind them.
        void createLightRecordBuffers(uint32_t capacity);
        [[nodiscard]] bool prepare(uint32_t requiredCapacity);

        // Shadow-data slot of each light, published by the spot/point shadow
        // submissions; a change re-uploads every light record.
        void publishSpotShadowSlots(const std::vector<uint32_t>& slots);
        void publishPointShadowSlots(const std::vector<uint32_t>& slots);

        // Per-frame staging (submitLightingPass): uploads the light records,
        // fallback candidates and cluster parameters of the slot.
        void uploadFrame(uint32_t frameIndex, const glm::mat4& view,
            const glm::mat4& projection, float nearPlane, float farPlane,
            const LightingFramePacket& lights, VkExtent2D sceneExtent,
            const EnvironmentLightingSettings& environment);
        // The slot's light records and active list (also uploaded by the probe
        // capture before its faces are lit).
        void uploadLights(uint32_t frameIndex, const LightingFramePacket& lights);
        // Drain points: stage the dispatch sizes, then run the registered
        // passes through "lighting.probe-cluster" / the last cluster pass.
        void recordProbeCluster(uint32_t clusterCount);
        void recordClusters(uint32_t frameIndex, uint32_t clusterCount,
            uint32_t activeLightCount);

        // The reflection-probe clustering pipeline; its descriptors bind the
        // probe buffers (backend-owned until R3c.6).
        [[nodiscard]] VulkanReflectionProbePipeline& probeClusterPipeline() noexcept {
            return probeClusters_;
        }

        // Lighting-set bindings.
        [[nodiscard]] FrameBufferInfos lightRecordDescriptors() const noexcept;
        [[nodiscard]] std::array<VulkanClusterSceneBufferDescriptors, FrameCount>
            sceneClusterDescriptors() const;

        [[nodiscard]] uint32_t lightRecordCapacity() const noexcept {
            return lightRecordCapacity_;
        }
        [[nodiscard]] uint32_t lightRecordMaximumCapacity() const noexcept {
            return lightRecordMaximumCapacity_;
        }
        [[nodiscard]] LightingUploadTelemetry uploadTelemetry() const noexcept {
            return { lightUploadBytes_, lightUploadRangeCount_,
                activeLightCount_, lightRecordCapacity_ };
        }
        [[nodiscard]] const ClusteredLightingTelemetry& clusterTelemetry() const noexcept {
            return clusterTelemetry_;
        }

    private:
        struct ShadowSlotMapping {
            std::vector<uint32_t> slots;
            uint64_t revision = 1;
            std::array<uint64_t, FrameCount> uploaded{};
            void publish(const std::vector<uint32_t>& next);
        };

        void rebuildClusterDescriptors();
        void updateParameters(uint32_t frameIndex, const glm::mat4& view,
            const glm::mat4& projection, float nearPlane, float farPlane,
            uint32_t activeLightCount, VkExtent2D sceneExtent,
            const EnvironmentLightingSettings& environment);
        void updateFallbackCandidates(uint32_t frameIndex, const glm::mat4& view,
            const LightingFramePacket& lights);
        void collectDiagnostics(uint32_t frameIndex) noexcept;

        // Callback bodies (VulkanPassCallbacks; owner = this).
        static bool probeClusterActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeProbeCluster(void* owner, VulkanPassContext& context);
        static void executeClear(void* owner, VulkanPassContext& context);
        static void executeCount(void* owner, VulkanPassContext& context);
        static void executeScan(void* owner, VulkanPassContext& context);
        static void executeFill(void* owner, VulkanPassContext& context);
        static void executeFinalize(void* owner, VulkanPassContext& context);
        static void executeReadback(void* owner, VulkanPassContext& context);

        const VulkanFeatureContext* context_ = nullptr;
        ClusterGridConfig config_{};
        VulkanClusteredLightingPipeline clusters_;
        VulkanReflectionProbePipeline probeClusters_;
        VulkanClusterGraphIds ids_{};
        RenderGraph::PassId probeClusterPass_{};
        RenderGraph::PassId readbackPass_{};

        std::array<VulkanBufferResource, FrameCount> lightRecordBuffers_{};
        std::array<VulkanBufferResource, FrameCount> activeLightSlotBuffers_{};
        std::array<VulkanBufferResource, FrameCount> fallbackCandidateBuffers_{};
        std::array<VulkanBufferResource, FrameCount> parameterBuffers_{};
        std::array<VulkanBufferResource, FrameCount> diagnosticReadbackBuffers_{};
        std::array<bool, FrameCount> diagnosticReadbackPending_{};
        std::array<uint32_t, FrameCount> submittedClusterCounts_{};
        ClusteredLightingTelemetry clusterTelemetry_{};
        std::array<std::vector<uint64_t>, FrameCount> uploadedLightRevisions_{};
        std::array<uint64_t, FrameCount> uploadedActiveListRevisions_{};
        ShadowSlotMapping spotShadowSlots_;
        ShadowSlotMapping pointShadowSlots_;
        std::vector<PackedGpuLight> patchedLightRecordsScratch_;
        std::vector<uint32_t> fallbackSelectionScratch_;
        std::vector<LightRecordRange> lightUploadRanges_;
        uint32_t lightRecordCapacity_ = 0;
        uint32_t lightRecordMaximumCapacity_ = 0;
        uint32_t activeLightCount_ = 0;
        uint64_t lightUploadBytes_ = 0;
        uint32_t lightUploadRangeCount_ = 0;

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        uint32_t stagedProbeClusterCount_ = 0;
        uint32_t stagedClusterCount_ = 0;
        uint32_t stagedActiveLightCount_ = 0;
    };

} // namespace Iridium
