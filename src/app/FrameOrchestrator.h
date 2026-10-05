#pragma once

// M7R R5a.4 (design section 3.1): the frame loop. It owns the main loop and the
// frame context, the profiler and allocation frames, the observer's frame
// phases, input routing, the per-frame order of asset integration, transforms,
// extraction and the editor build (design section 3.2), beginFrame /
// submitFrame / endFrame, the editor's frame requests (apply), and swapchain,
// output-transport and scene-extent changes. No ImGui.

#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/ApplicationConfig.h"
#include "app/FrameObserver.h"
#include "editor/EditorFrameRequests.h"
#include "editor/ViewportRenderExtent.h"
#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/RenderBackendRuntimeInfo.h"
#include "renderer/rhi/RenderFrame.h"
#include "scene/SceneWorld.h"
#include "scene/systems/TransformSystem.h"

namespace Iridium {

    class AssetIntegration;
    namespace Tasks { class TaskSystem; }
    class CpuProfiler;
    class EditorHost;
    class EngineLog;
    class RenderExtractor;

    // The Application's state the frame loop drives. References stay valid for
    // the orchestrator's lifetime.
    struct FrameOrchestratorContext {
        ApplicationConfig& config;
        CpuProfiler& profiler;
        // Its Periodic work runs on the frame tick; its counters are recorded
        // per profiled frame (M7R R5b.2).
        Tasks::TaskSystem& tasks;
        EngineLog& log;
        IFrameObserver* observer = nullptr;
        // Set by Application::run before the window exists.
        const AppRunPolicy& policy;
        SceneWorld& scene;
        AppCamera& camera;
        IAppControl& control;
        RenderExtractor& extractor;
        AssetIntegration& assets;
        EditorHost& editor;
    };

    class FrameOrchestrator final : private IRenderFrameStageObserver {
    public:
        explicit FrameOrchestrator(const FrameOrchestratorContext& context);

        FrameOrchestrator(const FrameOrchestrator&) = delete;
        FrameOrchestrator& operator=(const FrameOrchestrator&) = delete;

        // --- Startup (Application::initWindow / initRenderer) ---
        // Routes the window's resize, cursor, scroll and button callbacks here.
        void installWindowCallbacks(GLFWwindow* window);
        // Right after the backend's init: adopts its render extent.
        void adoptBackend(IRenderBackend& backend);
        // Reads the backend's output-transport state and shows it in the editor.
        void refreshOutputTransportStatus();
        // The ACES output LUT for the effective output transport.
        void initializeOutputTransformLut();

        // Runs frames until the window closes or --frame-limit is reached.
        void run();

        // --- IAppControl (delegated by the Application) ---
        OutputTransportSwitchResult switchOutputTransport(
            Color::OutputTransport requested);
        [[nodiscard]] bool resizeSceneExtent(RenderExtent requested,
            std::string& diagnostic);
        [[nodiscard]] RenderExtent renderExtent() const noexcept {
            return renderExtent_;
        }

        // --- Run state (the run snapshot) ---
        [[nodiscard]] bool measurementStarted() const noexcept {
            return measurementStarted_;
        }
        [[nodiscard]] uint64_t measuredFrameCount() const noexcept {
            return measuredFrameCount_;
        }
        [[nodiscard]] uint64_t measurementWallNanoseconds() const noexcept {
            return measurementWallNanoseconds_;
        }

        // Cleanup, before the backend: frees the output LUT.
        void releaseOutputTransformLut();

    private:
        // GLFW callbacks must be static.
        static void mouse_callback(GLFWwindow* window, double xposIn, double yposIn);
        static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
        static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
        static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

        void drawFrame(AppFrameContext& frame);
        // The only runtime-configuration writes the editor causes.
        void applyEditorFrameRequests(const EditorFrameRequests& requests);
        void processInput(GLFWwindow* window);
        void recreateSwapchain();
        void replaceOutputTransformLut(Color::OutputTransport effectiveTransport);
        void publishOutputTransportStatus();
        // M9.2c: between frames, from an editor request.
        void switchAntiAliasing(AntiAliasingMode requested);

        // IRenderFrameStageObserver
        void onRenderFrameStage(RenderFrameStage stage) override;

        ApplicationConfig& config_;
        CpuProfiler& cpuProfiler_;
        Tasks::TaskSystem& tasks_;
        EngineLog& engineLog_;
        IFrameObserver* observer_ = nullptr;
        const AppRunPolicy& policy_;
        SceneWorld& sceneWorld_;
        Registry& registry;
        AppCamera& camera_;
        IAppControl& control_;
        RenderExtractor& extractor_;
        AssetIntegration& assets_;
        EditorHost& editorHost_;
        GLFWwindow* window = nullptr;
        IRenderBackend* renderBackend = nullptr;

        bool framebufferResized = false;
        // Per-frame requests an observer may make; reset before PreSceneUpdate.
        AppFrameRequests frameRequests_{};
        TransformSystem transformSystem;
        std::vector<Entity> changedTransformEntities_;
        uint64_t changedTransformsThisFrame_ = 0;

        // --- OUTPUT ---
        TextureHandle outputTransformLut; // Pinned application-owned ACES 2 LUT.
        Color::OutputTransport outputTransformLutTransport_ =
            Color::OutputTransport::SdrSrgb;
        std::optional<Color::OutputTransport> pendingOutputTransport_;
        std::optional<AntiAliasingMode> pendingAntiAliasing_;
        uint64_t outputTransportSwitchCount_ = 0;
        RenderBackendRuntimeInfo renderRuntimeInfo_{};
        RenderExtent renderExtent_{};
        ViewportRenderExtentPolicy viewportExtentPolicy_;
        std::string viewportExtentDiagnostic_;

        // --- MEASUREMENT ---
        float deltaTime = 0.0f;
        uint64_t measuredFrameCount_ = 0;
        uint64_t measurementWallNanoseconds_ = 0;
        bool measurementStarted_ = false;

        // The frame whose observer submit points submitFrame's stage
        // boundaries report (valid while submitFrame runs).
        AppFrameContext* stageFrame_ = nullptr;

        // --- CAMERA INPUT ---
        float yaw = -90.0f;
        float pitch = 0.0f;
        float mouseSensitivity = 0.1f;
        float cameraSpeed = 2.5f;
        float lastX = 1280 / 2.0f;
        float lastY = 720 / 2.0f;
        bool firstMouse = true;
        bool isRightMouseButtonDown = false;
        bool isMiddleMouseButtonDown = false;
    };

} // namespace Iridium
