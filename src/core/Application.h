#pragma once

#define GLFW_INCLUDE_VULKAN 
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

// Include your engine subsystems
#include "renderer/VkContext.h" 
#include "renderer/VkSwapchain.h"
#include "renderer/VkRenderPass.h"
#include "renderer/VkFramebuffer.h"
#include "renderer/VkGraphicsPipeline.h"
#include "renderer/VkCommandManager.h"
#include "renderer/VkSyncObjects.h"
#include "assets/AssetManager.h"  
#include "ecs/Registry.h"
#include "editor/EditorSystem.h"
#include "ecs/systems/TransformSystem.h"
#include "utils/DeletionQueue.h"

class Application {
public:
    void run();

    // GLFW Callbacks must be static
    static void mouse_callback(GLFWwindow* window, double xposIn, double yposIn);
    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);

private:
    GLFWwindow* window;
    VkContext* vkContext;
    VkSwapchain* vkSwapchain;
    VkRenderPassWrapper* vkRenderPass;
    VkFramebufferWrapper* vkFramebuffer;
    VkGraphicsPipeline* vkPipeline;
    VkCommandManager* vkCommandManager;
    VkSyncObjects* vkSyncObjects;
    VkDescriptorPool descriptorPool;
    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    std::vector<Texture> modelTextures;
    std::vector<int> materialToTextureMap;
    VkImageView textureImageView;
    VkSampler textureSampler;

    EditorSystem editor;
    std::vector<VkDescriptorSet> globalDescriptorSets;
    bool enableValidation = false;
    TransformSystem transformSystem;
    Registry registry;
    DeletionQueue frameDeletionQueues[VkSyncObjects::MAX_FRAMES_IN_FLIGHT];

    glm::vec2 squarePos = { 0.0f, 0.0f };
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    uint32_t currentFrame = 0;
    std::vector<VkFence> imagesInFlight;

    AssetManager* assetManager;
    std::shared_ptr<ModelAsset> mainModel;

    // Mouse State
    float lastX = 1280 / 2.0f;
    float lastY = 720 / 2.0f;
    bool firstMouse = true;
    bool isRightMouseButtonDown = false;
    bool isMiddleMouseButtonDown = false;

    // Camera State
    float yaw = -90.0f;
    float pitch = 0.0f;
    float mouseSensitivity = 0.1f;
    glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 3.0f);
    glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
    float cameraSpeed = 2.5f;
    float deltaTime = 0.0f;

    // Internal Functions
    void initWindow();
    void initVulkan();
    void mainLoop();
    void cleanup();
    void drawFrame(Registry& registry, const glm::mat4& view, const glm::mat4& proj);
    void createUniformBuffers();
    void updateUniformBuffer(uint32_t currentImage, const glm::mat4& view, const glm::mat4& proj);
    void createDescriptorPool();
    void createDescriptorSets();
    void createDepthResources();
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
    void processInput(GLFWwindow* window);
    void selectEntityAtMouse(double mouseX, double mouseY);
    void ProcessMeshSwaps(Registry& registry, AssetManager* assetManager, uint32_t currentFrame);
};