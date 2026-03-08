#pragma once
#include "assets/AssetManager.h"
#include "scene/SceneSerializer.h"
#include "ecs/Entity.h"
#include "panels/EditorPanel.h" 
#include "panels/core/ViewPortPanel.h"
#include "EditorUIState.h"
#include <vector>
#include <memory>

class AssetManager;
struct ModelAsset;
class EditorPanel;

class EditorSystem {
public:
    ~EditorSystem();

    void init(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice,
        VkQueue graphicsQueue, VkRenderPass renderPass, GLFWwindow* window, VkCommandPool cmdPool);

    void cleanup(VkDevice device);

    void update(Registry& registry, AssetManager* assetManager,
        const glm::mat4& view, const glm::mat4& proj, VkDescriptorSet sceneTexture, VkDescriptorSet glassDepthTexture);

    void render(VkCommandBuffer cmd);

    int currentGizmoOperation = 0;
    Entity getSelectedEntity() { return selectedEntity; }
    void setSelectedEntity(Entity e) { selectedEntity = e; }

    void setSceneTexture(VkDescriptorSet texture) {
        currentSceneTexture = texture;
    }
    ViewportPanel& getViewportPanel() { return viewportPanel; }

    int currentRenderMode = 0;
    int currentToolMode = 0;

private:
    VkDescriptorPool imguiPool;
    Entity selectedEntity = NULL_ENTITY;
    EditorUIState uiState;
    ViewportPanel viewportPanel;
    VkDescriptorSet currentSceneTexture = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<EditorPanel>> panels;
};