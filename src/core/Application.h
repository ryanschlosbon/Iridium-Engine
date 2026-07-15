#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLFW/glfw3.h> 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <memory>
#include <string>

// --- ENGINE SUBSYSTEMS ---
#include "assets/AssetManager.h"  
#include "ecs/Registry.h"
#include "editor/EditorSystem.h"
#include "ecs/systems/TransformSystem.h"

// --- THE NEW RENDERING ARCHITECTURE ---
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/DrawPacket.h"

namespace Iridium {

    class Application {
    public:
        void run();

        // GLFW Callbacks must be static
        static void mouse_callback(GLFWwindow* window, double xposIn, double yposIn);
        static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
        static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
        static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

        bool wasWindowResized() const { return framebufferResized; }
        void resetWindowResizedFlag() { framebufferResized = false; }

    private:
        // --- CORE ENGINE STATE ---
        GLFWwindow* window;
        bool framebufferResized = false;

        // --- THE GRAPHICS ABSTRACTION ---
        // This single pointer replaces 40+ Vulkan variables!
        IRenderBackend* renderBackend = nullptr;

        // The Data-Driven Extraction Queues
        std::vector<DrawPacket> opaqueQueue;
        std::vector<DrawPacket> transparentQueue;
        std::vector<DrawPacket> selectionQueue;

        // --- SUBSYSTEMS ---
        AssetManager* assetManager = nullptr;
        Registry registry;
        TransformSystem transformSystem;
        EditorSystem editor;

        // --- SCENE DATA ---
        std::shared_ptr<ModelAsset> mainModel;
        TextureHandle hdriMap; // Upgraded from the Vulkan-tied 'Texture' struct

        // --- CAMERA STATE ---
        float yaw = -90.0f;
        float pitch = 0.0f;
        float mouseSensitivity = 0.1f;
        glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 3.0f);
        glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
        float cameraSpeed = 2.5f;
        float deltaTime = 0.0f;

        // --- MOUSE STATE ---
        float lastX = 1280 / 2.0f;
        float lastY = 720 / 2.0f;
        bool firstMouse = true;
        bool isRightMouseButtonDown = false;
        bool isMiddleMouseButtonDown = false;

        // --- INTERNAL FUNCTIONS ---
        void initWindow();
        void initRenderer(); // Formerly initVulkan()
        void mainLoop();
        void cleanup();

        // Notice how clean the drawFrame signature is now!
        void drawFrame();

        void processInput(GLFWwindow* window);
        void selectEntityAtMouse(double mouseX, double mouseY);
        void ProcessMeshSwaps();
        void recreateSwapchain();
    };

} // namespace Iridium