#pragma once

#include "ecs/Entity.h"
#include "panels/EditorPanel.h" 
#include "panels/core/ViewPortPanel.h"
#include "panels/windows/AssetViewerPanel.h"
#include "vendor/imguizmo/ImGuizmo.h"
#include "imgui.h"
#include "EditorUIState.h"
#include "EditorSelectionState.h"
#include "EditorAssetDocumentService.h"
#include "renderer/rhi/RenderDebugView.h"
#include <vector>
#include <memory>

// Forward declarations to keep the header clean and API-agnostic
namespace Iridium {
    class AssetCatalog;
    class AssetCatalogService;
    class AssetModelPreparationService;
    class AssetThumbnailService;
    class AssetManager;
    class AssetRuntimeService;
    class CpuProfiler;
    class EngineLog;
    class EditorSceneDocumentService;
    class EditorSceneCommandService;
    class EditorTransactionService;
}
struct GLFWwindow;

class EditorSystem {
public:
    EditorSystem();
    ~EditorSystem();

    // Removed Vulkan-specific initialization parameters
    void init(GLFWwindow* window, Iridium::CpuProfiler* cpuProfiler,
        bool showProfiler, bool showMaterialDiagnostics,
        Iridium::Color::OutputTransport outputTransport,
        float manualExposureEv, float paperWhiteNits, float peakNits,
        const Iridium::ProjectShadowSettings& shadowSettings,
        const Iridium::ProjectReflectionProbeSettings& reflectionProbeSettings,
        const Iridium::AssetCatalog* assetCatalog,
        Iridium::AssetCatalogService* assetCatalogService,
        Iridium::AssetModelPreparationService*
            assetModelPreparationService,
        Iridium::AssetThumbnailService*
            assetThumbnailService,
        Iridium::AssetRuntimeService* assetRuntimeService,
        Iridium::EngineLog* engineLog,
        Iridium::EditorSceneDocumentService* sceneDocumentService,
        Iridium::EditorTransactionService* transactionService);

    // Backend now handles the physical Vulkan cleanup; this cleans up UI state
    void cleanup();

    // update now uses API-agnostic void* for texture handles
    void update(Registry& registry, Iridium::AssetManager* assetManager,
        const glm::mat4& viewInput, const glm::mat4& projInput,
        void* sceneTextureID, void* glassDepthTextureID,
        float sceneAspect);

    Entity getSelectedEntity() { return selection_.primary; }
    void setSelectedEntity(Entity entity) {
        selection_.selectExclusive(entity);
    }

    ViewportPanel& getViewportPanel() { return viewportPanel; }
    [[nodiscard]] AssetViewerPanel& getAssetViewerPanel() noexcept {
        return assetViewerPanel_;
    }
    [[nodiscard]] const AssetViewerPanel& getAssetViewerPanel() const noexcept {
        return assetViewerPanel_;
    }
    [[nodiscard]] Iridium::RenderExtent requestedRenderExtent() const noexcept {
        return assetDocuments_.active()
            ? assetViewerPanel_.requestedRenderExtent
            : viewportPanel.requestedRenderExtent;
    }
    [[nodiscard]] Iridium::EditorAssetDocumentService& assetDocuments() noexcept {
        return assetDocuments_;
    }
    [[nodiscard]] const Iridium::EditorAssetDocumentService&
        assetDocuments() const noexcept { return assetDocuments_; }

    void setDebugView(Iridium::RenderDebugView view);
    [[nodiscard]] Iridium::RenderDebugView getDebugView() const;
    void drawColorValidationOverlay() const;
    [[nodiscard]] bool consumeOutputSettings(EditorOutputSettings& settings);
    [[nodiscard]] bool consumeShadowSettings(
        Iridium::ProjectShadowSettings& settings);
    [[nodiscard]] bool consumeReflectionProbeSettings(
        Iridium::ProjectReflectionProbeSettings& settings);

    int currentRenderMode = 0;
    ImGuizmo::OPERATION currentGizmoOperation = ImGuizmo::TRANSLATE;

private:
    Iridium::EditorSelectionState selection_;
    Iridium::EditorAssetViewerRegistry assetViewerRegistry_;
    Iridium::EditorAssetDocumentService assetDocuments_;
    AssetViewerPanel assetViewerPanel_;
    EditorUIState uiState;
    ViewportPanel viewportPanel;
    ImGuiStyle baseEditorStyle_;
    bool editorStyleCaptured_ = false;
    float editorUiScale_ = 1.0f;
    Iridium::CpuProfiler* cpuProfiler_ = nullptr;
    Iridium::EditorTransactionService* transactionService_ = nullptr;
    std::unique_ptr<Iridium::EditorSceneCommandService> sceneCommands_;

    // The list of active editor panels
    std::vector<std::unique_ptr<EditorPanel>> panels;
};
