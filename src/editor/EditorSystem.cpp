#include "EditorSystem.h"
#include "scene/Components.h"
#include "panels/core/SceneHierarchyPanel.h"
#include "panels/core/InspectorPanel.h"
#include "panels/core/ViewPortPanel.h"
#include "panels/menus/MenuBarPanel.h"
#include "panels/windows/ProjectSettingsPanel.h"
#include "panels/windows/ProfilerPanel.h"
#include "panels/windows/MaterialDiagnosticsPanel.h"
#include "panels/windows/AssetBrowserPanel.h"
#include "panels/windows/AssetViewerPanel.h"
#include "panels/windows/ConsolePanel.h"
#include "platform/FileDialog.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorSceneCommandService.h"
#include "editor/EditorTransactionService.h"
#include "assets/runtime/AssetRuntimeService.h"

#include <vector>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "vendor/imguizmo/ImGuizmo.h"

EditorSystem::EditorSystem()
    : assetDocuments_(&assetViewerRegistry_),
      assetViewerPanel_(&assetDocuments_, nullptr) {
    Iridium::registerCoreAssetViewers(assetViewerRegistry_);
    assetViewerRegistry_.freeze();
}
EditorSystem::~EditorSystem() = default;

void EditorSystem::setDebugView(Iridium::RenderDebugView view) {
    currentRenderMode = view == Iridium::RenderDebugView::Final
        ? 0
        : static_cast<int>(view) + 2;
}

Iridium::RenderDebugView EditorSystem::getDebugView() const {
    if (currentRenderMode < 3) {
        return Iridium::RenderDebugView::Final;
    }
    return static_cast<Iridium::RenderDebugView>(currentRenderMode - 2);
}

void EditorSystem::init(GLFWwindow* window, Iridium::CpuProfiler* cpuProfiler,
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
    Iridium::EditorTransactionService* transactionService) {
    // 1. INIT: All Vulkan Descriptor Pool and ImGui_ImplVulkan logic is gone!
    // The backend's init() function handles the heavy lifting now. We just create the panels.

    Iridium::setFileDialogOwner(window);
    uiState.showProfiler = showProfiler;
    uiState.showMaterialDiagnostics = showMaterialDiagnostics;
    uiState.outputSettings.transport = outputTransport;
    uiState.outputSettings.manualExposureEv = manualExposureEv;
    uiState.outputSettings.paperWhiteNits = paperWhiteNits;
    uiState.outputSettings.peakNits = peakNits;
    uiState.shadowSettings = shadowSettings;
    uiState.reflectionProbeSettings = reflectionProbeSettings;
    cpuProfiler_ = cpuProfiler;
    transactionService_ = transactionService;
    assetDocuments_.setRuntimePinCallback(
        [assetRuntimeService](Iridium::AssetGuid assetGuid, bool pinned) {
            if (assetRuntimeService) {
                assetRuntimeService->setPinned(assetGuid, pinned);
            }
        });
    assetViewerPanel_ = AssetViewerPanel(
        &assetDocuments_, assetRuntimeService);
    sceneCommands_.reset();
    if (sceneDocumentService && transactionService) {
        sceneCommands_ = std::make_unique<Iridium::EditorSceneCommandService>(
            *sceneDocumentService, *transactionService, selection_);
    }

    panels.push_back(std::make_unique<SceneHierarchyPanel>(
        &selection_, transactionService, sceneCommands_.get()));
    panels.push_back(std::make_unique<InspectorPanel>(
        &selection_, assetCatalog,
        assetThumbnailService, transactionService, sceneCommands_.get()));
    panels.push_back(std::make_unique<MenuBarPanel>(
        &selection_.primary, &uiState, sceneDocumentService,
        transactionService, sceneCommands_.get()));
    panels.push_back(std::make_unique<ProfilerPanel>(&uiState.showProfiler, cpuProfiler));
    panels.push_back(std::make_unique<MaterialDiagnosticsPanel>(
        &uiState.showMaterialDiagnostics, &selection_.primary));
    panels.push_back(std::make_unique<AssetBrowserPanel>(
        &uiState.showAssetBrowser, &selection_.primary, &uiState,
        assetCatalog,
        assetCatalogService,
        assetModelPreparationService,
        assetThumbnailService,
        assetRuntimeService,
        &assetDocuments_));
    panels.push_back(std::make_unique<ConsolePanel>(
        &uiState.showConsole, engineLog));
    panels.push_back(std::make_unique<ProjectSettingsPanel>(
        &uiState.showProjectSettings, &uiState.outputSettings,
        &uiState.shadowSettings, &uiState.shadowSettingsChanged,
        &uiState.reflectionProbeSettings,
        &uiState.reflectionProbeSettingsChanged));
}

bool EditorSystem::consumeOutputSettings(EditorOutputSettings& settings) {
    if (!uiState.outputSettings.changed) return false;
    uiState.outputSettings.changed = false;
    settings = uiState.outputSettings;
    settings.changed = false;
    return true;
}

bool EditorSystem::consumeShadowSettings(
    Iridium::ProjectShadowSettings& settings) {
    if (!uiState.shadowSettingsChanged) return false;
    uiState.shadowSettingsChanged = false;
    settings = uiState.shadowSettings;
    return true;
}

bool EditorSystem::consumeReflectionProbeSettings(
    Iridium::ProjectReflectionProbeSettings& settings) {
    if (!uiState.reflectionProbeSettingsChanged) return false;
    uiState.reflectionProbeSettingsChanged = false;
    settings = uiState.reflectionProbeSettings;
    return true;
}

void EditorSystem::cleanup() {
    // 2. CLEANUP: ImGui shutdown is handled by the backend. We just clear our panel memory.
    assetDocuments_.closeAll();
    assetDocuments_.setRuntimePinCallback({});
    panels.clear();
    sceneCommands_.reset();
    Iridium::setFileDialogOwner(nullptr);
}

void EditorSystem::update(Registry& registry, Iridium::AssetManager* assetManager,
    const glm::mat4& viewInput, const glm::mat4& projInput,
    void* sceneTextureID, void* glassDepthTextureID,
    float sceneAspect) {

    // NOTE: ImGui_ImplVulkan_NewFrame(), ImGui_ImplGlfw_NewFrame(), and ImGui::NewFrame()
    // are now handled by renderBackend->beginUI() in Application.cpp BEFORE calling this function!

    selection_.reconcile(registry);
    const ImGuiViewport* mainViewport =
        ImGui::GetMainViewport();
    if (!editorStyleCaptured_) {
        baseEditorStyle_ =
            ImGui::GetStyle();
        editorStyleCaptured_ = true;
    }
    const float responsiveScale =
        std::clamp(
            std::min(
                mainViewport->WorkSize.x /
                    1600.0f,
                mainViewport->WorkSize.y /
                    900.0f),
            0.72f, 1.0f);
    if (std::abs(
            responsiveScale -
            editorUiScale_) > 0.01f) {
        ImGui::GetStyle() =
            baseEditorStyle_;
        ImGui::GetStyle()
            .ScaleAllSizes(
                responsiveScale);
        ImGui::GetStyle()
            .FontScaleMain =
                responsiveScale;
        editorUiScale_ =
            responsiveScale;
    }

    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpaceOverViewport(0, mainViewport, dockFlags);

    // Draw editor panels
    for (auto& panel : panels) {
        panel->OnImGuiRender(registry, assetManager);
    }

    glm::mat4 view = viewInput;
    glm::mat4 proj = projInput;

    // --- 2. CUSTOM ECS SELECTED ENTITY SEARCH ---
    auto* transformPool = registry.getPool<TransformComponent>();
    TransformComponent* selectedTransform = nullptr;

    // Check if we have a valid selected entity and if the pool exists
    if (selection_.primary != NULL_ENTITY && transformPool) {
        if (transformPool->has(selection_.primary)) {
            selectedTransform = &transformPool->get(selection_.primary);
        }
    }

    // One expensive scene target is shared between the scene and the active asset
    // document. Asset previewing never inserts an entity into the scene Registry.
    if (assetDocuments_.active()) {
        assetViewerPanel_.render(sceneTextureID, glassDepthTextureID,
            currentRenderMode, sceneAspect, assetManager);
    }
    else {
        viewportPanel.render(sceneTextureID,
            glassDepthTextureID,
            currentRenderMode,
            currentGizmoOperation,
            view,
            proj,
            selectedTransform,
            sceneAspect,
            registry,
            &selection_.primary,
            assetManager,
            transactionService_,
            sceneCommands_.get(),
            cpuProfiler_);
    }

}

void EditorSystem::drawColorValidationOverlay() const {
    ImDrawList* overlay = ImGui::GetForegroundDrawList();
    const ImVec2 origin = ImGui::GetMainViewport()->WorkPos;
    const ImVec2 minimum(origin.x + 16.0f, origin.y + 16.0f);
    const ImVec2 maximum(minimum.x + 296.0f, minimum.y + 92.0f);
    overlay->AddRectFilled(minimum, maximum, IM_COL32(12, 12, 12, 255), 4.0f);
    overlay->AddText(ImVec2(minimum.x + 8.0f, minimum.y + 6.0f),
        IM_COL32(255, 255, 255, 255), "sRGB UI reference (exposure independent)");
    constexpr ImU32 patches[] = {
        IM_COL32(64, 64, 64, 255), IM_COL32(128, 128, 128, 255),
        IM_COL32(192, 192, 192, 255), IM_COL32(255, 0, 0, 255),
        IM_COL32(0, 255, 0, 255), IM_COL32(0, 0, 255, 255),
    };
    for (int index = 0; index < 6; ++index) {
        const float left = minimum.x + 8.0f + index * 46.0f;
        overlay->AddRectFilled(ImVec2(left, minimum.y + 34.0f),
            ImVec2(left + 38.0f, minimum.y + 80.0f), patches[index]);
    }
}

// } // namespace Iridium
