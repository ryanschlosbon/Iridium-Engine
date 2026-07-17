#include "EditorSystem.h"
#include "scene/Components.h"
#include "panels/core/SceneHierarchyPanel.h"
#include "panels/core/InspectorPanel.h"
#include "panels/core/ViewPortPanel.h"
#include "panels/menus/MenuBarPanel.h"
#include "panels/windows/ProjectSettingsPanel.h"
#include "platform/FileDialog.h"

#include <vector>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "vendor/imguizmo/ImGuizmo.h"

EditorSystem::~EditorSystem() = default;

void EditorSystem::init(GLFWwindow* window) {
    // 1. INIT: All Vulkan Descriptor Pool and ImGui_ImplVulkan logic is gone!
    // The backend's init() function handles the heavy lifting now. We just create the panels.

    Iridium::setFileDialogOwner(window);

    panels.push_back(std::make_unique<SceneHierarchyPanel>(&selectedEntity));
    panels.push_back(std::make_unique<InspectorPanel>(&selectedEntity));
    panels.push_back(std::make_unique<MenuBarPanel>(&selectedEntity, &uiState));
    panels.push_back(std::make_unique<ProjectSettingsPanel>(&uiState.showProjectSettings));
}

void EditorSystem::cleanup() {
    // 2. CLEANUP: ImGui shutdown is handled by the backend. We just clear our panel memory.
    panels.clear();
    Iridium::setFileDialogOwner(nullptr);
}

void EditorSystem::update(Registry& registry, Iridium::AssetManager* assetManager,
    const glm::mat4& viewInput, const glm::mat4& projInput,
    void* sceneTextureID, void* glassDepthTextureID) {

    // NOTE: ImGui_ImplVulkan_NewFrame(), ImGui_ImplGlfw_NewFrame(), and ImGui::NewFrame()
    // are now handled by renderBackend->beginUI() in Application.cpp BEFORE calling this function!

    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);

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
    if (selectedEntity != NULL_ENTITY && transformPool) {
        if (transformPool->has(selectedEntity)) {
            selectedTransform = &transformPool->get(selectedEntity);
        }
    }

    // --- 3. RENDER THE VIEWPORT PANEL ---
    // We now pass the void* IDs directly into the viewport panel!
    viewportPanel.render(sceneTextureID,
        glassDepthTextureID,
        currentRenderMode,
        currentGizmoOperation,
        view,
        proj,
        selectedTransform);

}

// } // namespace Iridium
