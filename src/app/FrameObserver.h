#pragma once

// Application-side observer contract (M7R R2.6). The qualification harness
// (src/qualification/harness) implements IFrameObserver; production code never
// depends on it. An Application without an observer runs the interactive editor.
//
// Hook order inside Application::run():
//
//   runPolicy()                     once, at the start of run()
//   onStartup(Configure)            before the backend exists
//   backendExtensions()             once, when the backend is created
//   onStartup(BackendReady)         backend + AssetManager exist; before asset services
//                                   (allocations here shift texture/material indices)
//   onStartup(ContentLoad)          only when runPolicy().ownsStartupContent; replaces
//                                   the interactive startup model/environment load
//   onStartup(ScenePrerequisites)   startup model known; before topology prewarm
//   onStartup(TopologyReady)        after prepareFrameTopology
//   onStartup(SceneConstruction)    after the interactive startup entity (if any),
//                                   before the editor default light and sky
//   onStartup(SceneComplete)        after the editor defaults, inside scene timing
//   onStartup(Ready)                after scene timing, before the asset-viewer wait
//
//   per frame:
//   onFrameBegin(PreSceneUpdate)    after input, before asset swaps/ticks/transforms
//   onFrameBegin(PostSceneUpdate)   after transforms; scene-extent resizes happen here,
//                                   between frames
//   onFrameBegin(BackendFrameOpened) after beginFrame succeeded and the GPU scene was
//                                   published (not called on a swapchain recreate)
//   onFrameSubmit(SceneLinearReady) after the forward queues  } reported from
//   onFrameSubmit(OutputReady)      after the output pass,    } submitFrame's
//                                   before the UI pass        } stage boundaries
//   onFrameEnd                      after endFrame succeeded
//
//   onShutdown(RunComplete)         main loop finished normally; backend alive
//   onShutdown(ReleaseResources)    first step of cleanup (success and failure paths)
//   onShutdown(Finalize)            after cleanup on the success path; backend gone

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "app/ApplicationConfig.h"
#include "core/types/AssetGuid.h"
#include "ecs/Entity.h"
#include "material/TransparencyPolicy.h"
#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/RenderBackendExtension.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/rhi/ShadowTypes.h"

namespace Iridium {

    class AssetManager;
    class CpuProfiler;
    class SceneWorld;
    struct LoadedEnvironmentAsset;
    struct ModelAsset;

    // Reference routes and qualification settings that change how the
    // Application drives the renderer. Only an observer selects them (the
    // qualification harness, from its command-line options); the defaults are
    // the production routes.
    struct AppRenderRouting {
        // Direct GBuffer draws with no main-view frustum rejection, plus
        // conventional shadows and direct probe capture.
        bool forceDirectGBufferReference = false;
        // Conventional directional/spot/point shadow submission only.
        bool forceDirectShadowReference = false;
        // Direct probe capture while other consumers stay automatic.
        bool forceDirectProbeCaptureReference = false;
        // Deterministic WeightedOIT draw permutation; 0 is production order.
        uint64_t weightedOitOrderSeed = 0;
        // Physically withheld finer LOD index ranges (0 keeps every level).
        uint32_t gpuLodMinimumResidentLevel = 0;
        // M7R R5c.5 ExtractionVerifier: the full-walk GPU-scene observation
        // beside the change-driven one, compared every frame.
        bool verifyGpuSceneObservations = false;
    };

    // How the Application runs when an observer owns the run. Defaults are the
    // interactive editor.
    struct AppRunPolicy {
        // In-memory asset catalog, no source workers, no ImGui ini, no editor input
        // or editor transparency overrides, no editor-driven scene resizes.
        bool deterministicContent = false;
        // Present the lit scene full-window instead of building the editor UI.
        bool fullscreenScenePresentation = false;
        // Draw the color-validation overlay over the fullscreen presentation.
        bool colorValidationOverlay = false;
        // The observer loads the startup model/environment and constructs the
        // startup scene; the Application creates no default entities.
        bool ownsStartupContent = false;
        AppRenderRouting routing{};
    };

    enum class StartupPhase : uint8_t {
        Configure,
        BackendReady,
        ContentLoad,
        ScenePrerequisites,
        TopologyReady,
        SceneConstruction,
        SceneComplete,
        Ready,
    };

    enum class FrameBeginPhase : uint8_t {
        PreSceneUpdate,
        PostSceneUpdate,
        BackendFrameOpened,
    };

    enum class FrameSubmitPoint : uint8_t {
        SceneLinearReady,
        OutputReady,
    };

    enum class ShutdownPhase : uint8_t {
        RunComplete,
        ReleaseResources,
        Finalize,
    };

    // Per-frame requests an observer may make; reset before PreSceneUpdate.
    struct AppFrameRequests {
        // Camera history reset revision for the scene view (0 when unset).
        std::optional<uint64_t> viewHistoryResetRevision;
        bool suppressGridOverlay = false;
    };

    struct AppCamera {
        glm::vec3 position{ 0.0f, 0.0f, 3.0f };
        glm::vec3 front{ 0.0f, 0.0f, -1.0f };
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        float verticalFovDegrees = 45.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
    };

    struct AppStartupTimings {
        uint64_t totalNanoseconds = 0;
        uint64_t windowNanoseconds = 0;
        uint64_t backendNanoseconds = 0;
        uint64_t editorNanoseconds = 0;
        uint64_t manifestVerificationNanoseconds = 0;
        uint64_t modelLoadNanoseconds = 0;
        uint64_t environmentCreationNanoseconds = 0;
        uint64_t sceneConstructionNanoseconds = 0;
        uint64_t frameTopologyPrewarmNanoseconds = 0;
    };

    struct OutputTransportSwitchResult {
        std::string requested;
        std::string effective;
        std::string diagnostic;
        uint64_t durationNanoseconds = 0;
    };

    // Operations an observer may ask of the Application.
    class IAppControl {
    public:
        // Switches the live output transport now (ADR-0013) and records it as the
        // configured transport.
        virtual OutputTransportSwitchResult applyOutputTransport(
            Color::OutputTransport transport) = 0;
        // Resizes the scene render extent; on success the Application adopts the
        // backend's effective extent.
        [[nodiscard]] virtual bool resizeSceneExtent(RenderExtent requested,
            std::string& diagnostic) = 0;
        [[nodiscard]] virtual RenderExtent renderExtent() const = 0;
        // Startup content (ContentLoad): loads --cooked-model-artifact as the
        // startup model, loads --cooked-environment-artifact as the scene
        // environment, or publishes an observer-built environment. The
        // qualification hitch scenario (M7R R4c.0) also publishes environments
        // at FrameBeginPhase::PreSceneUpdate; earlier environments stay loaded.
        virtual std::shared_ptr<ModelAsset> loadCookedStartupModel() = 0;
        virtual void loadCookedStartupEnvironment() = 0;
        virtual void publishStartupEnvironment(
            LoadedEnvironmentAsset environment) = 0;

    protected:
        ~IAppControl() = default;
    };

    // One context for all startup phases; fields fill in as startup progresses.
    struct AppStartupContext {
        ApplicationConfig& config;
        CpuProfiler& profiler;
        IAppControl& control;
        SceneWorld& scene;
        AppCamera& camera;
        AppStartupTimings& timings;
        IRenderBackend* backend = nullptr;              // BackendReady+
        AssetManager* assets = nullptr;                 // BackendReady+
        RenderBackendCapabilities capabilities{};       // BackendReady+
        TransparencyExecutionMode transparencyExecutionMode =
            TransparencyExecutionMode::Classified;       // BackendReady+
        RenderExtent renderExtent{};                    // BackendReady+
        std::shared_ptr<ModelAsset> mainModel;          // ScenePrerequisites+
        AssetGuid startupModelGuid;                     // SceneConstruction+
        FrameTopologyPreparation topology{};            // TopologyReady
        // SceneConstruction: the first startup model entity (in/out).
        Entity firstEntity = NULL_ENTITY;
        // SceneConstruction output: editor selection to apply.
        std::optional<Entity> initialSelection;
        AssetGuid environmentAssetGuid;                 // SceneComplete
    };

    struct AppFrameContext {
        const ApplicationConfig& config;
        IRenderBackend& backend;
        CpuProfiler& profiler;
        SceneWorld& scene;
        AppCamera& camera;
        IAppControl& control;
        AppFrameRequests& requests;
        const std::shared_ptr<ModelAsset>& mainModel;
        uint64_t applicationFrameIndex = 0;
        // Set on measured (post-warmup) frames.
        std::optional<uint64_t> measuredFrameIndex;
        // onFrameEnd: an editor transport switch is queued for this frame.
        bool outputTransportPending = false;
    };

    // Identity of the published scene environment (retained after its GPU
    // handles are released).
    struct AppEnvironmentIdentity {
        std::filesystem::path cookedArtifact;
        AssetGuid assetGuid;
        AssetGuid sourceGuid;
        std::string cookKey;
        std::string sourcePrimaries;
        float radianceScale = 0.0f;
    };

    // Application state reported at shutdown.
    struct AppRunSnapshot {
        bool measurementStarted = false;
        uint64_t measuredFrameCount = 0;
        uint64_t measurementWallNanoseconds = 0;
        RenderExtent renderExtent{};
        const ModelAsset* mainModel = nullptr;
        AppEnvironmentIdentity environment;
        std::optional<DirectionalShadowSelection> directionalShadowSelection;
        uint32_t directionalShadowSampleableMask = 0;
        uint32_t directionalShadowOwnerCount = 0;
        AppStartupTimings startup;
        RenderDebugView debugView = RenderDebugView::Final;
    };

    struct AppShutdownContext {
        const ApplicationConfig& config;
        CpuProfiler& profiler;
        IRenderBackend* backend = nullptr;   // null at Finalize
        // ReleaseResources: false on the failure path.
        bool completed = true;
        const AppRunSnapshot& run;
    };

    class IFrameObserver {
    public:
        virtual ~IFrameObserver() = default;

        [[nodiscard]] virtual AppRunPolicy runPolicy() const { return {}; }
        // Backend extensions to attach when the backend is created (after
        // onStartup(Configure)). Not owned by the backend: they must outlive
        // the Application's backend, i.e. the observer outlives Application.
        [[nodiscard]] virtual std::span<IRenderBackendExtension* const>
            backendExtensions() { return {}; }
        virtual void onStartup(StartupPhase, AppStartupContext&) {}
        virtual void onFrameBegin(FrameBeginPhase, AppFrameContext&) {}
        virtual void onFrameSubmit(FrameSubmitPoint, AppFrameContext&) {}
        virtual void onFrameEnd(AppFrameContext&) {}
        virtual void onShutdown(ShutdownPhase, AppShutdownContext&) {}
    };

} // namespace Iridium
