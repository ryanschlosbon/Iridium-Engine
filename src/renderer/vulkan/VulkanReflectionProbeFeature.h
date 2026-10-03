#pragma once

// M7R R3c.6: reflection probes as a feature owner. Owns the capture pass and
// its staging/published targets, the probe view culler, the per-slot probe
// record/active/parameter/cluster buffers and their uploads, the asset and
// captured environment tables, capture scheduling (pending prefilter
// publications, owner synchronization) and the capture telemetry. Registers
// "probe.capture"; its gpu.probe.capture range opens inside the callback,
// after the culler's host -> compute barrier, as before. The lighting set's
// probe bindings and the probe-clustering descriptors stay with their owners
// (VulkanDeferredLightingFeature since R3c.8, the clustered-light owner), which read them
// through bufferDescriptors()/environmentImages() and are told to rebind
// through Bindings.

#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/ReflectionProbeCapture.h"

#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanIndirectViewCuller.h"
#include "VulkanReflectionProbeCapturePass.h"
#include "VulkanReflectionProbeCaptureTargets.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanResourceAllocator.h"
#include "VulkanSceneDescriptors.h"
#include "VulkanShadowCasters.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Iridium {

    class VulkanClusterLightingFeature;

    class VulkanReflectionProbeFeature final : public IVulkanFeature {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;
        using FrameBuffers = std::array<VulkanBufferResource, FrameCount>;
        using FrameBufferInfos = std::array<VkDescriptorBufferInfo, FrameCount>;

        // The lighting-set owner rebinds when the probe buffers are replaced
        // or the environment table changes (same points as before R3c.6).
        struct Bindings {
            void* owner = nullptr;
            void (*buffersReplaced)(void* owner) = nullptr;
            void (*environmentsChanged)(void* owner) = nullptr;
        };

        struct BufferDescriptors {
            std::array<VulkanReflectionProbeBufferDescriptors, FrameCount> scene{};
            FrameBufferInfos records{};
            FrameBufferInfos active{};
            FrameBufferInfos parameters{};
            FrameBufferInfos headers{};
            FrameBufferInfos indices{};
        };

        VulkanReflectionProbeFeature() = default;
        VulkanReflectionProbeFeature(const VulkanReflectionProbeFeature&) = delete;
        VulkanReflectionProbeFeature& operator=(const VulkanReflectionProbeFeature&) = delete;

        // Project capture settings (any time; init applies the config's).
        void configureCaptures(const ProjectReflectionProbeSettings& settings);
        // Before create(): cluster grid, view settings, the shared caster
        // scratch, the lighting set layout the capture lights with, the
        // clustered-light owner (the capture uploads its light records; its
        // probe-clustering sets reference the probe buffers) and the
        // rebinding hooks.
        void configure(const ClusterGridConfig& clusterConfig,
            const VulkanIndirectViewSettings& settings, VulkanCasterScratch& scratch,
            VkDescriptorSetLayout lightingSetLayout,
            VulkanClusterLightingFeature& clusters, const Bindings& bindings) noexcept {
            clusterConfig_ = clusterConfig;
            settings_ = settings;
            scratch_ = &scratch;
            lightingSetLayout_ = lightingSetLayout;
            clusters_ = &clusters;
            bindings_ = bindings;
        }

        // IVulkanFeature. create() builds the capture pass and targets.
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // The probe view culler (after the directional shadow owner).
        void createCuller(const VulkanIndirectViewSetup& setup);
        // The first probe buffers, sized from the device limit and the scene
        // extent's cluster grid.
        void createInitialBuffers(VkExtent2D sceneExtent);
        // Capacity growth (frame boundary; the caller rebinds the graph's
        // imported buffers afterwards).
        void growIndirectCapacity(uint32_t primitiveCapacity);

        // IRenderBackend forwards (frame boundary).
        void prepare(uint32_t requiredCapacity,
            std::span<const EnvironmentLightingHandles> environments,
            VkExtent2D sceneExtent);
        [[nodiscard]] std::vector<ReflectionProbeCaptureCompletion> finalizeCaptures();
        [[nodiscard]] std::optional<uint32_t> capturedEnvironmentSlot(
            SceneEntityUuid owner) const noexcept;
        void synchronizeCaptureOwners(std::span<const SceneEntityUuid> owners);

        // Per frame. beginFrame clears the capture's handled flag;
        // submitCaptures is the "probe.capture" drain point (it uploads the
        // slot's light records inside the pass, before the faces are lit);
        // skipCaptureIfUnhandled skips it on frames without a submission
        // (asset preview). uploadFrame stages the probe records and the
        // clustering parameters (submitLightingPass).
        void beginFrame() noexcept { captureHandled_ = false; }
        void submitCaptures(const ReflectionProbeCasterSubmission& casters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights, VkDescriptorSet sceneSet);
        void skipCaptureIfUnhandled();
        void uploadFrame(uint32_t frameIndex, const glm::mat4& view,
            const glm::mat4& projection, float nearPlane, float farPlane,
            const ReflectionProbeGpuFramePacket& probes, VkExtent2D sceneExtent);

        // Lighting-set and probe-clustering bindings.
        [[nodiscard]] BufferDescriptors bufferDescriptors() const noexcept;
        // The environment table: `fallback` everywhere, then the asset
        // environments, then the captured products in their slots.
        [[nodiscard]] std::array<VkDescriptorImageInfo, kMaximumGpuReflectionProbeEnvironments>
            environmentImages(const VkDescriptorImageInfo& fallback) const;

        [[nodiscard]] const ReflectionProbeCaptureTelemetry& telemetry() const noexcept {
            return telemetry_;
        }
        [[nodiscard]] VulkanIndirectViewCuller& culler() noexcept { return culler_; }
        [[nodiscard]] VulkanReflectionProbeCaptureTargets& captureTargets() noexcept {
            return captureTargets_;
        }
        [[nodiscard]] const FrameBuffers& clusterHeaderBuffers() const noexcept {
            return clusterHeaderBuffers_;
        }
        [[nodiscard]] const FrameBuffers& clusterIndexBuffers() const noexcept {
            return clusterIndexBuffers_;
        }

    private:
        struct PendingCapture {
            SceneEntityUuid owner;
            uint64_t captureTicket = 0;
            std::vector<VkDescriptorSet> filterDescriptors;
            VulkanReflectionProbeCaptureReadback bakedReadback;
            uint32_t resolution = 0;
            uint32_t mipLevels = 0;
        };

        static bool captureActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeCapture(void* owner, VulkanPassContext& context);

        void createBuffers(uint32_t recordCapacity, uint32_t clusterCapacity,
            uint32_t referenceCapacity);
        void notifyBuffersReplaced() const;
        void notifyEnvironmentsChanged() const;
        void uploadRecords(uint32_t frameIndex,
            const ReflectionProbeGpuFramePacket& probes);
        void updateParameters(uint32_t frameIndex, const glm::mat4& view,
            const glm::mat4& projection, float nearPlane, float farPlane,
            uint32_t activeProbeCount, VkExtent2D sceneExtent);
        // Plans the probe culler and records its host -> compute barrier.
        [[nodiscard]] bool prepareIndirect(VkCommandBuffer commandBuffer,
            uint32_t frameIndex);

        const VulkanFeatureContext* context_ = nullptr;
        ClusterGridConfig clusterConfig_{};
        VulkanIndirectViewSettings settings_{};
        VulkanCasterScratch* scratch_ = nullptr;
        VkDescriptorSetLayout lightingSetLayout_ = VK_NULL_HANDLE;
        VulkanClusterLightingFeature* clusters_ = nullptr;
        Bindings bindings_{};
        RenderGraph::PassId capturePassId_{};

        VulkanReflectionProbeCapturePass capturePass_;
        VulkanReflectionProbeCaptureTargets captureTargets_;
        VulkanIndirectViewCuller culler_;

        FrameBuffers recordBuffers_{};
        FrameBuffers activeSlotBuffers_{};
        FrameBuffers parameterBuffers_{};
        FrameBuffers clusterHeaderBuffers_{};
        FrameBuffers clusterIndexBuffers_{};
        std::array<std::vector<uint64_t>, FrameCount> uploadedRevisions_{};
        std::array<uint64_t, FrameCount> uploadedActiveListRevisions_{};
        std::vector<ReflectionProbeRecordRange> uploadRanges_;
        std::vector<EnvironmentLightingHandles> environments_;
        uint32_t recordCapacity_ = 0;
        uint32_t recordMaximumCapacity_ = 0;
        uint32_t clusterCapacity_ = 0;
        uint32_t referenceCapacity_ = 0;
        std::vector<PendingCapture> pendingCaptures_;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash> capturedSlots_;
        ReflectionProbeCaptureTelemetry telemetry_{};
        uint32_t prefilterSampleCount_ = 256;
        bool captureHandled_ = false;

        // Staged for the frame's callback (see VulkanFeatureContext.h).
        ReflectionProbeCasterSubmission stagedCasters_{};
        std::span<const ReflectionProbeCaptureScheduleEntry> stagedCaptures_{};
        const LightingFramePacket* stagedLights_ = nullptr;
        VkDescriptorSet stagedSceneSet_ = VK_NULL_HANDLE;
        bool stagedActive_ = false;
    };

} // namespace Iridium
