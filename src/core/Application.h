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
#include "renderer/DescriptorAllocator.h"
#include "renderer/VkUIRenderPass.h"
#include "renderer/VkLightingPipeline.h"
#include "renderer/VkForwardRenderPass.h"
#include "renderer/VkForwardPipeline.h"
#include "renderer/GlassDepthRenderPass.h"
#include "renderer/GlassDepthPipeline.h"

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
    GLFWwindow* window;
    VkContext* vkContext;
    VkSwapchain* vkSwapchain;
    VkRenderPassWrapper* vkRenderPass;
    VkFramebufferWrapper* vkFramebuffer;
    VkGraphicsPipeline* vkPipeline;
    VkCommandManager* vkCommandManager;
    VkSyncObjects* vkSyncObjects;
    DescriptorAllocator descriptorAllocator;
    VkUIRenderPass* vkUIRenderPass;
    std::vector<VkFramebuffer> uiFramebuffers;

    // --- G-BUFFER RENDER TARGETS ---
    std::vector<VkImage> gNormalImages;
    std::vector<VkDeviceMemory> gNormalImageMemories;
    std::vector<VkImageView> gNormalImageViews;

    std::vector<VkImage> gAlbedoImages;
    std::vector<VkDeviceMemory> gAlbedoImageMemories;
    std::vector<VkImageView> gAlbedoImageViews;

    VkSampler gBufferSampler;

    // --- UPGRADED DEPTH RESOURCES ---
    std::vector<VkImage> depthImages;
    std::vector<VkDeviceMemory> depthImageMemories;
    std::vector<VkImageView> depthImageViews;

    // --- LIGHTING PASS RESOURCES ---
    VkLightingPipeline* vkLightingPipeline;
    VkRenderPass lightingRenderPass;
    std::vector<VkFramebuffer> lightingFramebuffers;
    std::vector<VkDescriptorSet> lightingDescriptorSets;

    // The Final Lit Scene (This goes to ImGui!)
    std::vector<VkImage> litSceneImages;
    std::vector<VkDeviceMemory> litSceneImageMemories;
    std::vector<VkImageView> litSceneImageViews;

    // The Photograph (For Refraction)
    std::vector<VkImage> opaqueSceneCopyImages;
    std::vector<VkDeviceMemory> opaqueSceneCopyMemories;
    std::vector<VkImageView> opaqueSceneCopyViews;

    // The Back-Face Depth (For Thickness)
    std::vector<VkImage> glassDepthImages;
    std::vector<VkDeviceMemory> glassDepthMemories;
    std::vector<VkImageView> glassDepthViews;

    Iridium::GlassDepthRenderPass* glassDepthRenderPass = nullptr;
    Iridium::GlassDepthPipeline* glassDepthPipeline = nullptr;
    std::vector<VkFramebuffer> glassDepthFramebuffers;

    // Forward Pipeline for Glass
    VkForwardRenderPass* vkForwardRenderPass = nullptr;
    VkForwardPipeline* vkForwardPipeline = nullptr;
    std::vector<VkFramebuffer> forwardFramebuffers;

    // The special ImGui pointers that lets the UI draw our Vulkan texture
    std::vector<VkDescriptorSet> sceneDescriptorSets;
    std::vector<VkDescriptorSet> glassDepthUITextures;

    std::vector<Texture> modelTextures;
    std::vector<int> materialToTextureMap;
    VkImageView textureImageView;
    VkSampler textureSampler;

    EditorSystem editor;
    std::vector<VkDescriptorSet> globalDescriptorSets;
    bool enableValidation = true;
    TransformSystem transformSystem;
    Registry registry;
    DeletionQueue frameDeletionQueues[VkSyncObjects::MAX_FRAMES_IN_FLIGHT];

    glm::vec2 squarePos = { 0.0f, 0.0f };
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    uint32_t currentFrame = 0;
    std::vector<VkFence> imagesInFlight;
    bool framebufferResized = false;

    AssetManager* assetManager;
    std::shared_ptr<ModelAsset> mainModel;
    Texture hdriMap;

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
    void drawFrame(Registry& registry, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& editorProj);
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
    void recreateSwapchain();
    void allocateMaterialDescriptors(std::shared_ptr<ModelAsset> model);
    void createOffscreenRenderTarget();
    void createUIFramebuffers();
    void createLightingDescriptorSets();
    void createLightingRenderPass();
    void createLightingFramebuffers();

    // Add these above your initVulkan() function
    VkFormat findSupportedFormat(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
                return format;
            }
            else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
                return format;
            }
        }
        throw std::runtime_error("failed to find supported format!");
    }

    VkFormat findDepthFormat(VkPhysicalDevice physicalDevice) {
        return findSupportedFormat(
            physicalDevice,
            { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
        );
    }
};