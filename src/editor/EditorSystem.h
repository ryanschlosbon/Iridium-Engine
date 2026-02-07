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
    void update(Registry& registry, AssetManager* assetManager);

    // This records the draw commands into the Vulkan buffer
    void render(VkCommandBuffer cmd);

private:
    VkDescriptorPool imguiPool;

    // State for the "Inspector"
    Entity selectedEntity = NULL_ENTITY;
    char importPathBuffer[256] = "assets/models/car.gltf"; // Default text
};