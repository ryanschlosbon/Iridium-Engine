#pragma once

#include "ecs/Entity.h"
#include "panels/EditorPanel.h" 
#include "panels/core/ViewPortPanel.h"
#include "vendor/imguizmo/ImGuizmo.h"
#include "EditorUIState.h"
#include <vector>
#include <memory>

// Forward declarations to keep the header clean and API-agnostic
namespace Iridium { class AssetManager; }
struct GLFWwindow;

class EditorSystem {
public:
    ~EditorSystem();

    // Removed Vulkan-specific initialization parameters
    void init(GLFWwindow* window);

    // Backend now handles the physical Vulkan cleanup; this cleans up UI state
    void cleanup();

    // update now uses API-agnostic void* for texture handles
    void update(Registry& registry, Iridium::AssetManager* assetManager,
        const glm::mat4& viewInput, const glm::mat4& projInput,
        void* sceneTextureID, void* glassDepthTextureID);

    Entity getSelectedEntity() { return selectedEntity; }
    void setSelectedEntity(Entity e) { selectedEntity = e; }

    ViewportPanel& getViewportPanel() { return viewportPanel; }

    int currentRenderMode = 0;
    ImGuizmo::OPERATION currentGizmoOperation = ImGuizmo::TRANSLATE;

private:
    Entity selectedEntity = NULL_ENTITY;
    EditorUIState uiState;
    ViewportPanel viewportPanel;

    // The list of active editor panels
    std::vector<std::unique_ptr<EditorPanel>> panels;
};