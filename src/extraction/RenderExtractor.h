#pragma once

// M7R R5a.3 (design section 3.1): render extraction. It turns the scene, the
// editor view state and the asset bindings into one RenderFrame per frame:
// GPU-scene publication, lights and reflection probes, the main-view
// classification and draw-packet extraction, sorts, and the shadow and probe
// capture schedules. No ImGui, editor or GLFW dependency.

#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <memory>
#include <string>
#include <vector>

#include "ecs/Entity.h"
#include "extraction/EditorViewState.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/lighting/LocalShadow.h"
#include "renderer/lighting/ReflectionProbe.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneVisibility.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/ReflectionProbeCapture.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/RenderFrame.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/rhi/ViewportGridOverlay.h"
#include "renderer/scene/GpuScenePublisher.h"
#include "scene/SceneWorld.h"

namespace Iridium {

    class AssetManager;
    struct LoadedEnvironmentAsset;
    class CpuProfiler;
    class IRenderBackend;
    struct ModelAsset;

    // Run policy: fixed for the run (the qualification observer selects the
    // reference routes; production uses the defaults).
    struct RenderExtractorPolicy {
        // Deterministic runs ignore the editor's layered-interface override.
        bool deterministicContent = false;
        bool forceDirectGBufferReference = false;
        bool forceDirectProbeCaptureReference = false;
    };

    // The scene camera (the application's free camera).
    struct RenderCamera {
        glm::vec3 position{ 0.0f, 0.0f, 3.0f };
        glm::vec3 front{ 0.0f, 0.0f, -1.0f };
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        float verticalFovDegrees = 45.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
    };

    // What the view record and the frame's view fields read besides the
    // editor view state.
    struct RenderViewInputs {
        RenderExtent renderExtent{};
        EnvironmentLightingSettings sceneEnvironmentSettings{};
        double manualExposureEv = 0.0;
        double paperWhiteNits = 203.0;
        double peakNits = 1000.0;
        // Camera history reset revision for the scene view (observer request).
        std::optional<uint64_t> viewHistoryResetRevision;
    };

    struct RenderExtractionInputs {
        bool forceWireframe = false;
        // Transforms the transform system updated this frame (a counter).
        uint64_t changedTransforms = 0;
        // The scene environment's cook key (probe-capture environment revision).
        const std::string* environmentCookKey = nullptr;
        uint64_t applicationFrameIndex = 0;
        // Receives submitFrame's stage boundaries; it forwards the shadow,
        // probe-capture and lighting stages to onRenderFrameStage.
        IRenderFrameStageObserver* stageObserver = nullptr;
    };

    class RenderExtractor final {
    public:
        // shadowSettings is read every frame (the project settings may change
        // between frames); the probe settings configure the capture scheduler.
        RenderExtractor(CpuProfiler& profiler, SceneWorld& scene,
            const ProjectShadowSettings& shadowSettings,
            const ProjectReflectionProbeSettings& probeSettings);
        ~RenderExtractor();

        RenderExtractor(const RenderExtractor&) = delete;
        RenderExtractor& operator=(const RenderExtractor&) = delete;

        // After the backend exists: the GPU-scene publisher and its capacity.
        void attachBackend(IRenderBackend& backend,
            const RenderExtractorPolicy& policy);
        // After the asset manager exists (material override bindings).
        void attachAssets(AssetManager& assets);

        // --- GPU-scene publication (before beginFrame) ---
        // Observes the scene's mesh instances, synchronizes the persistent
        // publication and grows backend capacity; the tables are published
        // right after beginFrame.
        void prepareGpuScenePublication(Entity selectedEntity);
        [[nodiscard]] const GpuScenePackedTables* gpuSceneFrame() const noexcept {
            return gpuSceneFrame_;
        }

        // --- Lights and reflection probes (before beginFrame) ---
        // Extracts and prepares the lights of `lightingWorld` (the scene, or
        // the asset preview's lighting world) and extracts, publishes and
        // prepares the scene's reflection probes over the loaded environments.
        void prepareLightsAndProbes(SceneWorld& lightingWorld,
            const std::map<AssetGuid, LoadedEnvironmentAsset>& loadedEnvironments);

        // --- Reflection-probe captures ---
        // Applies changed project probe settings to the capture scheduler.
        void configureProbeCaptures(
            const ProjectReflectionProbeSettings& settings);
        void markCapturePublished(
            const ReflectionProbeCaptureCompletion& completion);

        // --- Main view and extraction (after beginFrame) ---
        // Clears the frame's queues.
        void clearQueues();
        // Before the editor build: the shadow/probe consumer references and
        // the camera, from the previous frame's editor state.
        void beginView(const EditorViewState& preBuildView,
            const RenderCamera& camera, float aspect);
        // After the editor build: the asset preview's camera and image fit,
        // the view record, the environment settings and the frame's view,
        // history and output fields.
        void finalizeView(const EditorViewState& view,
            const RenderViewInputs& inputs);
        [[nodiscard]] const glm::mat4& viewMatrix() const noexcept {
            return viewMatrix_;
        }
        [[nodiscard]] const glm::mat4& projectionMatrix() const noexcept {
            return projMatrix_;
        }
        void setGridOverlay(const ViewportGridOverlay& overlay);
        // Classification, draw-packet extraction, counters, sorts, transparent
        // intervals, shadow and probe-capture schedules and the frame's queues.
        // The frame stays valid until releaseFrame.
        [[nodiscard]] const RenderFrame& extract(
            const RenderExtractionInputs& inputs);
        // submitFrame's stage boundaries: cache completion and counters.
        void onRenderFrameStage(RenderFrameStage stage);

        [[nodiscard]] const std::optional<DirectionalShadowSelection>&
            activeDirectionalShadowSelection() const noexcept {
            return activeDirectionalShadowSelection_;
        }
        [[nodiscard]] uint32_t activeDirectionalShadowSampleableMask() const noexcept {
            return activeDirectionalShadowSampleableMask_;
        }
        [[nodiscard]] uint32_t activeDirectionalShadowOwnerCount() const noexcept {
            return activeDirectionalShadowOwnerCount_;
        }

        // After submitFrame (or when the frame does not open): releases the
        // frame's packets. They are re-created every frame, as the former
        // drawFrame locals were, so steady-frame allocations are unchanged.
        void releaseFrame();

    private:
        void usePreviewCamera(const EditorViewState& view);
        // Directional cascades, spot atlas tiles and point cubes against the
        // cache schedulers, then scene probe captures; fills the frame's
        // shadow and capture fields.
        void scheduleShadowsAndCaptures(
            const ShadowCasterSubmission& shadowCasters,
            const ReflectionProbeCasterSubmission& probeCasters,
            const glm::mat4& viewMatrix, const glm::vec3& renderCameraPosition,
            float renderVerticalFovDegrees, float aspect,
            float renderCameraNearPlane, float renderCameraFarPlane,
            bool assetPreviewActive, const std::string& environmentCookKey,
            uint64_t applicationFrameIndex, RenderFrame& renderFrame);

        CpuProfiler& cpuProfiler_;
        SceneWorld& sceneWorld_;
        Registry& registry;
        IRenderBackend* renderBackend = nullptr;
        AssetManager* assetManager_ = nullptr;
        RenderExtractorPolicy policy_{};

        // The data-driven extraction queues.
        std::vector<DrawPacket> opaqueQueue;
        std::vector<DrawPacket> forwardOpaqueQueue;
        std::vector<DrawPacket> transparentQueue;
        std::vector<DrawPacket> sortedSurfaceQueue;
        std::vector<TransparentIntervalEndpoint>
            transparentIntervalEndpointScratch;
        std::vector<float> transparentIntervalNearScratch;
        std::vector<uint32_t> transparentIntervalFenwickScratch;
        std::vector<DrawPacket> selectionQueue;
        // Dense GPU-scene primitive references are the production shadow path;
        // shadowCasterQueue contains compatibility packets only.
        std::vector<DrawPacket> shadowCasterQueue;
        std::vector<DrawPacket> probeCasterQueue_;
        // Backend-neutral, frame-local transforms referenced by DrawPacket
        // instance ranges. Existing single-instance packets leave this empty.
        std::vector<glm::mat4> forwardInstanceTransforms_;
        GpuSceneVisibilityResult gpuSceneVisibility_;

        // This frame's view (beginView to extract).
        float aspect_ = 16.0f / 9.0f;
        std::span<const uint32_t> shadowGpuScenePrimitiveIndices_{};
        std::span<const uint32_t> probeGpuScenePrimitiveIndices_{};
        glm::vec3 renderCameraPosition_{ 0.0f };
        float renderCameraNearPlane_ = 0.0f;
        float renderCameraFarPlane_ = 0.0f;
        float renderVerticalFovDegrees_ = 0.0f;
        glm::mat4 viewMatrix_{ 1.0f };
        glm::mat4 projMatrix_{ 1.0f };
        const EditorViewState* view_ = nullptr;
        RenderFrame renderFrame_{};

        std::unique_ptr<GpuScenePublisher> gpuScenePublisher_;
        std::vector<GpuSceneObservedInstance> gpuSceneObservations_;
        struct GpuSceneObservationMetadata {
            SceneEntityUuid owner;
            const ModelAsset* model = nullptr;
            GeometryHandle geometry;
            std::string cookKey;
            uint64_t materialOverrideSignature = 0;
            uint64_t observationRevision = 0;
            glm::vec3 localMinimum{ 0.0f };
            glm::vec3 localMaximum{ 0.0f };
            uint32_t baseConsumerMask = 0;
            bool valid = false;
        };
        std::vector<GpuSceneObservationMetadata>
            gpuSceneObservationMetadata_;
        const GpuScenePackedTables* gpuSceneFrame_ = nullptr;
        uint32_t gpuSceneDirectFallbackCount_ = 0;

        LightExtractor lightExtractor_;
        ReflectionProbePublisher reflectionProbePublisher_;
        std::vector<EnvironmentLightingHandles>
            reflectionProbeEnvironments_;
        const std::map<AssetGuid, LoadedEnvironmentAsset>* loadedEnvironments_ =
            nullptr;
        // This frame's packets (valid until releaseFrame).
        LightingFramePacket lightingFrame_;
        ReflectionProbeFramePacket extractedProbes_;
        ReflectionProbeGpuFramePacket publishedProbes_;

        const ProjectShadowSettings& shadowSettings_;
        ReflectionProbeCaptureScheduler reflectionProbeCaptureScheduler_;
        std::array<DirectionalShadowCache,
            kDirectionalShadowLightCapacity> directionalShadowCaches_;
        StableSpotShadowAtlas spotShadowAtlas_;
        LocalShadowCacheScheduler spotShadowCache_;
        StablePointShadowPools pointShadowPools_;
        LocalShadowCacheScheduler pointShadowCache_;
        std::optional<DirectionalShadowSelection>
            activeDirectionalShadowSelection_;
        uint32_t activeDirectionalShadowSampleableMask_ = 0;
        uint32_t activeDirectionalShadowOwnerCount_ = 0;
        // Per-frame light selection scratch, kept for its capacity (R5c.6).
        std::vector<DirectionalShadowSelection> directionalShadowSelections_;
        std::vector<LocalShadowRequest> localShadowRequests_;
        // This frame's shadow packets (valid until releaseFrame).
        std::vector<DirectionalShadowFramePacket> directionalShadows_;
        std::vector<SpotShadowFramePacket> spotShadows_;
        std::vector<PointShadowFramePacket> pointShadows_;
        // Caller-side work reported at submitFrame's stage boundaries (cache
        // bookkeeping and profile counters), in the order it ran between the
        // former submit* calls (M7R R3c.11). Valid while submitFrame runs.
        struct FrameStageRecords {
            std::span<const DirectionalShadowFramePacket> directionalShadows{};
            LocalShadowAllocationStats spotAllocation{};
            const LocalShadowSchedule* spotSchedule = nullptr;
            LocalShadowAllocationStats pointAllocation{};
            const LocalShadowSchedule* pointSchedule = nullptr;
            const ReflectionProbeCaptureSchedule* probeCaptureSchedule = nullptr;
        };
        FrameStageRecords frameStage_{};
    };

} // namespace Iridium
