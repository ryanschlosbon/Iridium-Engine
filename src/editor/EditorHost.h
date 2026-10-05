#pragma once

// M7R R5a.2 (design section 3.1): the editor as the application sees it. It
// owns the EditorSystem and its ImGui build, the dual-view cadence and the
// retained views, the asset preview (resolution, framing, preview lighting
// world and sun, environment selection and its diagnostics) and the
// EditorViewState extraction reads. Runtime state changes leave it only as
// EditorFrameRequests. This header is ImGui-free.

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "core/types/AssetGuid.h"
#include "ecs/Entity.h"
#include "editor/EditorFrameRequests.h"
#include "editor/EditorViewCadence.h"
#include "extraction/EditorViewState.h"
#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/rhi/ViewportGridOverlay.h"
#include "scene/SceneWorld.h"

class EditorSystem;
class Registry;
struct GLFWwindow;

namespace Iridium {

    class AssetCatalog;
    class AssetCatalogService;
    class AssetEnvironmentPreparationService;
    class AssetManager;
    class AssetModelPreparationService;
    class AssetRuntimeService;
    class AssetThumbnailService;
    class CpuProfiler;
    namespace Tasks { class TaskSystem; }
    class EditorAssetDocumentService;
    class EditorSceneDocumentService;
    class EditorTransactionService;
    class EngineLog;
    class IEditorRenderBridge;
    struct LoadedEnvironmentAsset;
    struct ModelAsset;

    // What the editor reads of the asset integration. Pointers stay valid
    // until EditorHost::cleanup.
    struct EditorHostAssets {
        AssetManager* manager = nullptr;
        AssetCatalog* catalog = nullptr;
        AssetCatalogService* catalogService = nullptr;
        AssetModelPreparationService* modelPreparation = nullptr;
        AssetEnvironmentPreparationService* environmentPreparation = nullptr;
        AssetThumbnailService* thumbnails = nullptr;
        AssetRuntimeService* runtime = nullptr;
        const std::shared_ptr<ModelAsset>* mainModel = nullptr;
        const std::map<AssetGuid, LoadedEnvironmentAsset>* loadedEnvironments =
            nullptr;
    };

    struct EditorHostInit {
        GLFWwindow* window = nullptr;
        IEditorRenderBridge* bridge = nullptr;
        bool showProfiler = false;
        bool showMaterialDiagnostics = false;
        Color::OutputTransport outputTransport = Color::OutputTransport::SdrSrgb;
        float manualExposureEv = 0.0f;
        float paperWhiteNits = 203.0f;
        float peakNits = 1000.0f;
        const ProjectShadowSettings* shadowSettings = nullptr;
        const ProjectReflectionProbeSettings* reflectionProbeSettings = nullptr;
        EditorHostAssets assets{};
        EngineLog* log = nullptr;
        EditorSceneDocumentService* sceneDocuments = nullptr;
        EditorTransactionService* transactions = nullptr;
        // Editor background work (menu-bar scans) runs on it (M7R R5b.2).
        Tasks::TaskSystem* tasks = nullptr;
    };

    // The scene camera the editor build draws against (gizmos, picking).
    struct EditorBuildInputs {
        Registry* registry = nullptr;
        glm::vec3 cameraPosition{ 0.0f };
        glm::vec3 cameraFront{ 0.0f, 0.0f, -1.0f };
        glm::vec3 cameraUp{ 0.0f, 1.0f, 0.0f };
        float verticalFovDegrees = 45.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
        // The frame's render aspect.
        float aspect = 16.0f / 9.0f;
        RenderExtent renderExtent{};
        uint64_t measuredFrameCount = 0;
        // Benchmark presentation: the lit scene full-window, no editor UI.
        bool fullscreenScenePresentation = false;
        bool colorValidationOverlay = false;
    };

    struct EditorViewSelection {
        bool dualViews = false;
        // 0 = scene view, 1 = asset view.
        uint32_t renderView = 0;
    };

    class EditorHost final {
    public:
        explicit EditorHost(CpuProfiler& profiler);
        ~EditorHost();

        EditorHost(const EditorHost&) = delete;
        EditorHost& operator=(const EditorHost&) = delete;

        // --- Startup ---
        void init(const EditorHostInit& init);
        void setOutputTransportStatus(Color::OutputTransport requested,
            Color::OutputTransport effective,
            const std::array<bool, 3>& supported, std::string diagnostic);
        void setAntiAliasingStatus(AntiAliasingMode active, std::string diagnostic);
        void setDebugView(RenderDebugView view);
        // --open-asset-viewer: opens the ready model or material asset; throws
        // when it is not one.
        void openConfiguredAssetViewer(AssetGuid asset);
        // Deterministic and hidden-window runs keep the user's imgui.ini.
        void disableLayoutPersistence();
        void setSelectedEntity(Entity entity);
        // The active document's preview model, framed on first resolution;
        // requests preparation when it is not resident.
        [[nodiscard]] std::shared_ptr<ModelAsset> resolveAssetPreview(
            RenderExtent renderExtent, uint64_t measuredFrameCount);
        [[nodiscard]] EditorAssetDocumentService& assetDocuments() noexcept;

        // --- Per frame, in drawFrame order ---
        // Dual-view cadence: which view renders this frame.
        [[nodiscard]] EditorViewSelection chooseView(
            bool fullscreenScenePresentation);
        // The preview lighting world (with its sun updated from the asset
        // viewer) when the asset view renders, else null.
        [[nodiscard]] SceneWorld* previewLightingWorld();
        [[nodiscard]] Entity selectedEntity() const noexcept;
        // The environment binding for the rendered view: the scene's, or the
        // asset viewer's HDRI once resident (with the viewer's diagnostic).
        [[nodiscard]] EnvironmentLightingHandles selectViewEnvironment(
            const EnvironmentLightingHandles& sceneEnvironment,
            uint64_t applicationFrameIndex);
        void prepareRetainedViews(bool dualViews, uint32_t renderView);
        // The editor state extraction reads; resolves the active preview.
        [[nodiscard]] EditorViewState viewState(float aspect,
            RenderExtent renderExtent, uint64_t measuredFrameCount);
        // Builds the editor UI (after beginFrame) and returns its requests,
        // with the post-build view state.
        [[nodiscard]] EditorFrameRequests build(const EditorBuildInputs& inputs);
        [[nodiscard]] ViewportGridOverlay viewportGridOverlay(
            const glm::mat4& view, const glm::mat4& projection) const;
        // After endFrame: the dual-view cadence records the rendered view.
        void viewRendered(uint32_t renderView);

        // --- Input routing and run state ---
        [[nodiscard]] bool assetViewerFocused() const noexcept;
        [[nodiscard]] bool sceneViewportHovered() const noexcept;
        [[nodiscard]] RenderDebugView debugView() const;

        void cleanup();

    private:
        CpuProfiler& cpuProfiler_;
        std::unique_ptr<::EditorSystem> editor_;
        IEditorRenderBridge* bridge_ = nullptr;
        EditorHostAssets assets_{};
        EditorViewScheduler editorViewScheduler_;
        SceneWorld previewLightingWorld_;
        Entity previewSun_ = NULL_ENTITY;
        AssetGuid framedPreviewDocumentGuid_;
        std::string framedPreviewCookKey_;
        uint64_t framedPreviewRevision_ = 0;
        uint64_t framedPreviewSession_ = 0;
    };

} // namespace Iridium
