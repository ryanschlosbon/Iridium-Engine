#pragma once
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "../scene/Registry.h"
#include "../assets/AssetManager.h"
#include "scene/Entity.h"

class AssetManager;
struct ModelAsset;

class EditorSystem {
public:
    void init(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice,
        VkQueue graphicsQueue, VkRenderPass renderPass, GLFWwindow* window, VkCommandPool cmdPool);

    void cleanup(VkDevice device);

    // This renders the actual UI windows (Hierarchy, Inspector, etc.)
    void update(Registry& registry, AssetManager* assetManager,
        const glm::mat4& view, const glm::mat4& proj);

    // This records the draw commands into the Vulkan buffer
    void render(VkCommandBuffer cmd);

    int currentGizmoOperation = 0;

    Entity getSelectedEntity() { return selectedEntity; }

    int currentRenderMode = 0; // 0 = Standard, 1 = Wireframe

    void setSelectedEntity(Entity e) { selectedEntity = e; }

    int currentToolMode = 0;    // 0 = Select (No Gizmo) 1 = Translate 2 = Rotate 3 = Scale

private:
    VkDescriptorPool imguiPool;

    // State for the "Inspector"
    Entity selectedEntity = NULL_ENTITY;
    char importPathBuffer[256] = "assets/models/car.gltf"; // Default text
};