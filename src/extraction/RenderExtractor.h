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
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ecs/Entity.h"
#include "extraction/EditorViewState.h"
#include "extraction/ParallelDrawSort.h"
#include "extraction/PreviousTransformCache.h"
#include "extraction/GpuSceneObservation.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/lighting/LocalShadow.h"
#include "renderer/lighting/ReflectionProbe.h"
#include "renderer/rhi/CompactDrawSort.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneVisibility.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/ReflectionProbeCapture.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/RenderFrame.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/rhi/ViewMotion.h"
#include "renderer/rhi/ViewportGridOverlay.h"
#include "renderer/scene/GpuScenePublisher.h"
#include "scene/SceneWorld.h"

struct MeshComponent;
struct RenderInstanceBatchComponent;
struct TransformComponent;

namespace Iridium {

    namespace Tasks { class TaskSystem; }
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
        // Qualification builds only (--qualification-extraction-verifier):
        // run the full-walk GPU-scene observation beside the change-driven
        // one every frame and fail on any difference (M7R R5c.5).
        bool verifyGpuSceneObservations = false;
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
        // M9 G5b: apply the per-view sub-pixel jitter sequence.
        bool temporalJitter = false;
        uint32_t temporalJitterSequenceLength = 8;
        // M9.5: the frame clock (seconds) the per-view time delta derives
        // from (simulated time under deterministic content).
        double timeSeconds = 0.0;
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
        // With a task system, classification and draw-packet extraction run
        // as frame-critical parallel work (M7R R5c.7); without one, serially.
        // Their output is identical either way.
        RenderExtractor(CpuProfiler& profiler, SceneWorld& scene,
            const ProjectShadowSettings& shadowSettings,
            const ProjectReflectionProbeSettings& probeSettings,
            Tasks::TaskSystem* tasks = nullptr);
        ~RenderExtractor();

        RenderExtractor(const RenderExtractor&) = delete;
        RenderExtractor& operator=(const RenderExtractor&) = delete;

        // After the backend exists: the GPU-scene publisher and its capacity.
        void attachBackend(IRenderBackend& backend,
            const RenderExtractorPolicy& policy);
        // After the asset manager exists (material override bindings).
        void attachAssets(AssetManager& assets);

        // --- GPU-scene publication (before beginFrame) ---
        // Observes the scene's mesh instances (change-driven; see
        // GpuSceneObservation.h), synchronizes the persistent publication and
        // grows backend capacity; the tables are published right after
        // beginFrame. `changedTransforms` is the transform system's journal
        // for this frame.
        void prepareGpuScenePublication(Entity selectedEntity,
            std::span<const Entity> changedTransforms);
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
        // M7R R5c.7: the submeshes of one model that a GPU-scene owner without
        // material overrides can draw (valid material index, valid material
        // and pipeline, transparent queue), in ascending order. Extraction
        // visits only these for such owners; the opaque ones go through the
        // GPU scene. The list is a function of the per-submesh material
        // indices and the per-material "draws transparent" bits, which are
        // compared exactly with the model on its first use in every frame.
        struct TransparentSubmeshList {
            const ModelAsset* model = nullptr;
            std::vector<int32_t> materialIndices;
            std::vector<uint32_t> indexCounts;
            std::vector<uint8_t> materialDrawsTransparent;
            std::vector<uint32_t> submeshes;
            // Sum over every submesh of indexCount / 3 (submesh.requested and
            // geometry.triangle.source_requested count all submeshes).
            uint64_t sourceTriangles = 0;
            uint64_t usedFrame = 0;
        };
        // The index of the model's list in transparentSubmeshLists_,
        // revalidated on its first use in a frame.
        [[nodiscard]] uint32_t transparentSubmeshes(const ModelAsset& model);
        void evictTransparentSubmeshLists();

        // M7R R5c.7 parallel extraction. Each stage splits its input into
        // contiguous chunks in input order (mesh-pool order, instance order,
        // primitive order); every chunk writes only its own scratch, and the
        // chunks' outputs are appended to the frame's queues in chunk order,
        // so every queue holds exactly the sequence the serial loop produced.
        // Counters are integer sums (and one maximum) of the chunks'.
        enum ExtractionQueue : uint8_t {
            ExtractionOpaque,
            ExtractionForwardOpaque,
            ExtractionTransparent,
            ExtractionSortedSurface,
            ExtractionQueueCount,
        };
        struct ExtractionCounters {
            uint64_t modelRecords = 0;
            uint64_t instances = 0;
            uint64_t submeshes = 0;
            uint64_t sourceTriangles = 0;
            uint64_t sourceIndexBytes = 0;
            uint64_t arenaIndexBytes = 0;
            uint64_t arenaSavedIndexBytes = 0;
            uint64_t uint16Indices = 0;
            uint64_t uint32Indices = 0;
            uint64_t lodFallbackChains = 0;
            uint64_t lodWithheldRanges = 0;
            uint64_t lodWithheldIndexBytes = 0;
            uint64_t transparentCulled = 0;
            uint32_t maximumLodResidentBase = 0;
        };
        // One mesh entity accepted by the serial pre-pass.
        struct ExtractionItem {
            Entity entity = NULL_ENTITY;
            const MeshComponent* mesh = nullptr;
            const RenderInstanceBatchComponent* batch = nullptr;
            const TransformComponent* transform = nullptr;
            uint32_t transparentList = UINT32_MAX;
            // Submesh visits plus one (balances the chunks only).
            uint32_t visits = 1;
        };
        // A chunk's scratch; vectors keep their capacity across frames.
        struct ExtractionChunk {
            uint32_t firstItem = 0;
            uint32_t endItem = 0;
            std::array<std::vector<DrawPacket>, ExtractionQueueCount> queues;
            std::vector<DrawPacket> selection;
            // Instance-batch transforms; packets index them chunk-locally.
            std::vector<glm::mat4> instanceTransforms;
            ExtractionCounters counters;
            std::exception_ptr failure;
            // Destination offsets (mergeExtractionChunks).
            std::array<size_t, ExtractionQueueCount> queueOffsets{};
            size_t selectionOffset = 0;
            size_t transformOffset = 0;
        };
        struct ParityChunk {
            uint32_t firstPrimitive = 0;
            uint32_t endPrimitive = 0;
            std::vector<DrawPacket> forward;
            uint64_t deferredCandidates = 0;
            uint64_t deferredCandidateTriangles = 0;
            uint64_t forwardVisible = 0;
            uint64_t forwardVisibleTriangles = 0;
            std::exception_ptr failure;
            size_t forwardOffset = 0;
        };
        struct ClassifyChunk {
            uint32_t firstInstance = 0;
            uint32_t endInstance = 0;
            GpuSceneVisibilityPart part;
            std::exception_ptr failure;
        };
        // Chunk sizes (work per chunk at which a stage splits; one chunk runs
        // inline) and the chunk cap, which also bounds the worker scope
        // events per stage and frame.
        static constexpr uint64_t ExtractionVisitsPerChunk = 256;
        static constexpr uint64_t ClassifyPrimitivesPerChunk = 2048;
        static constexpr uint64_t ParityPrimitivesPerChunk = 8192;
        // Below this many packets the merge copies on the calling thread.
        static constexpr size_t MergeParallelMinimumPackets = 2048;
        static constexpr uint32_t MaximumChunks = 32;
        // Runs fn(chunk) for chunk in [0, count): inline when count is 1 or
        // there is no task system, otherwise as one frame-critical task set
        // that the main thread joins (and helps with). `scope` names the
        // worker scopes.
        template <class Fn>
        void runChunks(uint32_t count, const char* scope, Fn&& fn);
        // Chunk count for `work` units at about `unitsPerChunk` units each.
        [[nodiscard]] uint32_t chunkCountFor(uint64_t work,
            uint64_t unitsPerChunk) const noexcept;
        void classifyMainView(const glm::mat4& clipFromWorld);
        // Sizes the frame's queues and copies the chunks into them, in
        // parallel; adds the chunks' counters to `extracted`.
        void mergeExtractionChunks(uint32_t extractionChunkCount,
            uint32_t parityChunkCount, ExtractionCounters& extracted);

        void usePreviewCamera(const EditorViewState& view);
        // The M7.2 parity packet of a published primitive (forward-opaque,
        // selection and the direct probe-capture reference still use packets).
        [[nodiscard]] DrawPacket gpuSceneParityPacket(uint32_t primitiveIndex,
            bool cpuVisible) const;
        // Sorts the direct opaque packets and fills opaqueOrder_.
        void buildOpaqueOrder(std::span<const uint32_t> gpuScenePrimitives);
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
        // M7R R5c.3: key scratch shared by the four queue sorts (keeps its
        // capacity across frames).
        CompactDrawSortScratch drawSortScratch_;
        // M7R R5c.4b: opaqueQueue holds direct packets only; the main view's
        // opaque draw order (OpaqueSubmission::order), the cached order of the
        // main-opaque list (rebuilt when its membership revision changes) and
        // the scratch of the mixed permutation.
        std::vector<uint32_t> opaqueOrder_;
        std::vector<uint32_t> gpuSceneOpaqueOrder_;
        uint64_t gpuSceneOpaqueOrderRevision_ = 0;
        std::vector<DrawPacket> opaqueDirectScratch_;
        // M7R R5c.7: the transparent sorts and the interval count.
        TransparentDrawSorter transparentSorter_;
        std::vector<DrawPacket> selectionQueue;
        // Dense GPU-scene primitive references are the production shadow path;
        // shadowCasterQueue contains compatibility packets only.
        std::vector<DrawPacket> shadowCasterQueue;
        std::vector<DrawPacket> probeCasterQueue_;
        // Backend-neutral, frame-local transforms referenced by DrawPacket
        // instance ranges. Existing single-instance packets leave this empty.
        std::vector<glm::mat4> forwardInstanceTransforms_;
        GpuSceneVisibilityResult gpuSceneVisibility_;
        // M7R R5c.7: per-model transparent submesh lists (dense) and their
        // index by model address; the frame counter that ages them out.
        std::vector<TransparentSubmeshList> transparentSubmeshLists_;
        std::unordered_map<const ModelAsset*, uint32_t>
            transparentSubmeshListIndex_;
        const ModelAsset* lastTransparentSubmeshModel_ = nullptr;
        uint32_t lastTransparentSubmeshList_ = UINT32_MAX;
        uint64_t extractionFrame_ = 0;
        // M7R R5c.7 parallel extraction state (see ExtractionChunk).
        Tasks::TaskSystem* tasks_ = nullptr;
        std::vector<ExtractionItem> extractionItems_;
        std::vector<ExtractionChunk> extractionChunks_;
        std::vector<ParityChunk> parityChunks_;
        std::vector<ClassifyChunk> classifyChunks_;

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
        // M9 G4: previous world transforms for direct and forward-opaque
        // packets (parallel to their queues).
        [[nodiscard]] glm::mat4 previousWorldFor(const DrawPacket& packet);
        void resolvePreviousTransforms(std::span<const DrawPacket> packets,
            std::vector<glm::mat4>& previous);
        PreviousTransformCache previousTransforms_;
        std::vector<glm::mat4> opaqueDirectPrevious_;
        std::vector<glm::mat4> forwardOpaquePrevious_;
        std::vector<glm::mat4> sortedSurfacePrevious_;
        std::vector<glm::mat4> compatibilityPrevious_;
        // M9 G2: per-retained-view cut detection and History context.
        ViewMotionTracker viewMotion_{};
        ViewMotionResult lastViewMotion_{};

        std::unique_ptr<GpuScenePublisher> gpuScenePublisher_;
        GpuSceneObservation gpuSceneObservation_;
        // Qualification ExtractionVerifier (null otherwise).
        std::unique_ptr<GpuSceneObservation> observationVerifier_;
        uint64_t verifiedObservationFrames_ = 0;
        void verifyGpuSceneObservation(const GpuSceneObservationInputs& inputs);
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
        // M7R R5c.8: the schedules' former per-frame locals, kept for their
        // capacity (steady frames allocate nothing).
        std::vector<LocalShadowCacheInput> spotCacheInputs_;
        std::vector<LocalShadowCacheInput> pointCacheInputs_;
        std::vector<ReflectionProbeCaptureRequest> probeCaptureRequests_;
        std::vector<SceneEntityUuid> runtimeCaptureOwners_;
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
