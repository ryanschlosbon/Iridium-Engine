#pragma once

// The composition root (M7R R5a, design section 3.1): it constructs the
// window, the backend and the four units in order, wires them in
// initRenderer, runs the startup observer phases, implements IAppControl and
// the run snapshot by delegation, and tears everything down in cleanup.
// Frame logic lives in FrameOrchestrator, extraction in RenderExtractor, the
// editor in EditorHost and the asset services in AssetIntegration.

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>
#include <vector>

#include "app/ApplicationConfig.h"
#include "app/AssetIntegration.h"
#include "app/FrameObserver.h"
#include "app/FrameOrchestrator.h"
#include "core/EngineLog.h"
#include "core/tasks/TaskSystem.h"
#include "editor/EditorHost.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorTransactionService.h"
#include "extraction/RenderExtractor.h"
#include "profiling/CpuProfiler.h"
#include "renderer/rhi/EditorRenderBridge.h"
#include "renderer/rhi/IRenderBackend.h"
#include "scene/SceneWorld.h"

namespace Iridium {

    class Application final : private IAppControl {
    public:
        // The observer (the qualification harness in IRIDIUM_QUALIFICATION
        // builds) is optional and must outlive run(); without one the
        // Application runs the interactive editor.
        explicit Application(ApplicationConfig config = {},
            IFrameObserver* observer = nullptr);
        void run();

    private:
        // --- CORE ENGINE STATE ---
        ApplicationConfig config_;
        EngineLog engineLog_;
        CpuProfiler cpuProfiler_;
        // The engine task system (M7R R5b.2, ADR-0015): the only owner of worker
        // threads. Constructed on the main thread before every unit that uses it
        // and destroyed after them.
        Tasks::TaskSystem tasks_;
        GLFWwindow* window = nullptr;
        bool glfwInitialized_ = false;
        IFrameObserver* observer_ = nullptr;
        AppRunPolicy policy_{};

        // --- THE GRAPHICS ABSTRACTION ---
        // The editor bridge (M7R R3c.10) is attached to the backend as an
        // extension and must outlive it, so it is declared first.
        std::unique_ptr<IEditorRenderBridge> editorBridge_;
        std::vector<IRenderBackendExtension*> backendExtensions_;
        std::unique_ptr<IRenderBackend> renderBackend;

        // --- SUBSYSTEMS ---
        SceneWorld sceneWorld_;
        // GPU-scene publication and render extraction (M7R R5a.3).
        RenderExtractor extractor_;
        EditorSceneDocumentService sceneDocumentService_;
        EditorTransactionService transactionService_;
        Registry& registry;
        // Asset manager, asset services, startup content and environments
        // (M7R R5a.1).
        AssetIntegration assets_;
        // The editor UI, its views and the asset preview (M7R R5a.2).
        EditorHost editorHost_;
        AppCamera camera_{};
        RenderBackendCapabilities renderCapabilities_{};
        AppStartupTimings startupProfile_;
        // The frame loop (M7R R5a.4); declared last, it refers to the above.
        FrameOrchestrator orchestrator_;

        // --- INTERNAL FUNCTIONS ---
        void initWindow();
        void initRenderer(); // Formerly initVulkan()
        void cleanup(bool completed);

        [[nodiscard]] AppRunSnapshot makeRunSnapshot() const;
        void notifyStartup(StartupPhase phase, AppStartupContext& context);
        void notifyShutdown(ShutdownPhase phase, bool completed);

        // IAppControl
        OutputTransportSwitchResult applyOutputTransport(
            Color::OutputTransport transport) override;
        [[nodiscard]] bool resizeSceneExtent(RenderExtent requested,
            std::string& diagnostic) override;
        [[nodiscard]] RenderExtent renderExtent() const override;
        std::shared_ptr<ModelAsset> loadCookedStartupModel() override;
        void loadCookedStartupEnvironment() override;
        void publishStartupEnvironment(
            LoadedEnvironmentAsset environment) override;
    };

} // namespace Iridium
