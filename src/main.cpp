#define GLFW_INCLUDE_VULKAN 
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> 
#include <glm/gtc/type_ptr.hpp>    
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <stb_image.h>
#include <imgui_impl_vulkan.h>
#include <iostream>
#include <vector>
#include <stdexcept> 
#include <cstring>   
#include <chrono>
#include "renderer/VkContext.h" 
#include "renderer/VkSwapchain.h"
#include "renderer/VkRenderPass.h"
#include "renderer/VkFramebuffer.h"
#include "renderer/VkGraphicsPipeline.h"
#include "renderer/VkCommandManager.h"
#include "renderer/VkSyncObjects.h"
#include "renderer/VkMesh.h"
#include "assets/AssetManager.h"  
#include "scene/Registry.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/TransformComponent.h"
#include "editor/EditorSystem.h"

// CONSTANTS
const int WIDTH = 1280;
const int HEIGHT = 720;

// THE CLASS STRUCTURE
class IridiumEngine {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

    // MOUSE MOVEMENT (Look & Pan)
    static void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
        auto* app = reinterpret_cast<IridiumEngine*>(glfwGetWindowUserPointer(window));

        float xpos = static_cast<float>(xposIn);
        float ypos = static_cast<float>(yposIn);

        if (app->firstMouse) {
            app->lastX = xpos;
            app->lastY = ypos;
            app->firstMouse = false;
        }

        float xoffset = xpos - app->lastX;
        float yoffset = app->lastY - ypos; // Reversed since Y-coordinates go from bottom to top

        app->lastX = xpos;
        app->lastY = ypos;

        // MODE 1: LOOK AROUND (Right Click Held)
        if (app->isRightMouseButtonDown) {
            xoffset *= app->mouseSensitivity;
            yoffset *= app->mouseSensitivity;

            app->yaw += xoffset;
            app->pitch += yoffset;

            // Constrain Pitch so screen doesn't flip
            if (app->pitch > 89.0f) app->pitch = 89.0f;
            if (app->pitch < -89.0f) app->pitch = -89.0f;

            // Update Camera Front Vector
            glm::vec3 front;
            front.x = cos(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            front.y = sin(glm::radians(app->pitch));
            front.z = sin(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            app->cameraFront = glm::normalize(front);
        }

        // MODE 2: PANNING (Middle Click Held)
        if (app->isMiddleMouseButtonDown) {
            // Panning speed factor (can be adjusted or tied to cameraSpeed)
            float panSpeed = app->cameraSpeed * 0.005f;

            // Calculate Right and Up vectors relative to camera
            glm::vec3 cameraRight = glm::normalize(glm::cross(app->cameraFront, app->cameraUp));
            glm::vec3 cameraTrueUp = glm::normalize(glm::cross(cameraRight, app->cameraFront));

            // Move Position:
            // - Mouse Left/Right (xoffset) moves along Camera Right
            // - Mouse Up/Down (yoffset) moves along Camera Up
            app->cameraPos -= cameraRight * (xoffset * panSpeed);
            app->cameraPos -= cameraTrueUp * (yoffset * panSpeed);
        }
    }

    // Speed Control
    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto* app = reinterpret_cast<IridiumEngine*>(glfwGetWindowUserPointer(window));

        // Scroll Up (Positive Y) increases speed, Down decreases
        app->cameraSpeed += (float)yoffset * 0.5f;

        // Clamp speed so it doesn't go negative or too crazy
        if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
        if (app->cameraSpeed > 10.0f) app->cameraSpeed = 10.0f;

        std::cout << "Camera Speed: " << app->cameraSpeed << std::endl;
    }

    // Toggle Logic
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        auto* app = reinterpret_cast<IridiumEngine*>(glfwGetWindowUserPointer(window));

        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_PRESS) {
                app->isRightMouseButtonDown = true;
                // Hide cursor when looking
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                // Reset firstMouse to prevent "jump" when clicking
                app->firstMouse = true;
            }
            else if (action == GLFW_RELEASE) {
                app->isRightMouseButtonDown = false;
                // Show cursor again
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }

        if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == GLFW_PRESS) {
                app->isMiddleMouseButtonDown = true;
                app->firstMouse = true;
            }
            else if (action == GLFW_RELEASE) {
                app->isMiddleMouseButtonDown = false;
            }
        }
    }

private:
    GLFWwindow* window;
    VkContext* vkContext; // use a pointer so it can be easily created and destroyed
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

    glm::vec2 squarePos = { 0.0f, 0.0f };
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    uint32_t currentFrame = 0;
    std::vector<VkFence> imagesInFlight;

    AssetManager* assetManager;
    std::shared_ptr<ModelAsset> mainModel;

    // Mouse State
    float lastX = WIDTH / 2.0f;
    float lastY = HEIGHT / 2.0f;
    bool firstMouse = true;
    bool isRightMouseButtonDown = false;
    bool isMiddleMouseButtonDown = false;

    // Camera Angles
    float yaw = -90.0f; // -90.0f points to -Z (default forward)
    float pitch = 0.0f;
    float mouseSensitivity = 0.1f;

    // Camera State
    glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 3.0f); // Start back a bit
    glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f); // Looking towards -Z
    glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);  // Y is up
    float cameraSpeed = 2.5f;
    float deltaTime = 0.0f; // Time between frames

    void initWindow() {
        glfwInit();

        // We are using Vulkan, so we must tell GLFW "No API please".
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        // Resize is disabled for now because handling window resizing 
        // in Vulkan requires rebuilding the entire swapchain (complex).
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        window = glfwCreateWindow(WIDTH, HEIGHT, "Iridium Engine", nullptr, nullptr);

        glfwSetWindowUserPointer(window, this);

        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
    }

    void initVulkan() {
        vkContext = new VkContext(enableValidation, window);
        vkSwapchain = new VkSwapchain(vkContext, window);
        vkRenderPass = new VkRenderPassWrapper(vkContext, vkSwapchain);
        vkPipeline = new VkGraphicsPipeline(vkContext, vkSwapchain, vkRenderPass);
        vkSyncObjects = new VkSyncObjects(vkContext, vkSwapchain->getImageCount());

        // CommandManager must exist before AssetManager
        vkCommandManager = new VkCommandManager(vkContext, nullptr, vkPipeline, vkSwapchain->getImageCount());

        createDepthResources();
        transitionImageLayout(depthImage, VK_FORMAT_D32_SFLOAT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        vkFramebuffer = new VkFramebufferWrapper(vkContext, vkSwapchain, vkRenderPass, depthImageView);

        // --- ASSET MANAGEMENT ---
        assetManager = new AssetManager(vkContext, vkCommandManager);

        std::string modelPath = std::string(PROJECT_ROOT_DIR) + "assets/models/alfa_romeo/scene.gltf";
        mainModel = assetManager->getModel(modelPath);

        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();

        // --- EDITOR INITIALIZATION ---
        // This MUST happen after vkRenderPass and vkCommandManager are valid
        editor.init(
            vkContext->getInstance(),
            vkContext->getDevice(),
            vkContext->getPhysicalDevice(),
            vkContext->getGraphicsQueue(),
            vkRenderPass->getRenderPass(),
            window,
            vkCommandManager->getCommandPool() // Use the manager here for clarity
        );
    }

    void drawFrame(const std::vector<RenderObject>& renderQueue) {
        // 1. Wait for CPU-GPU sync (Frame 0 or 1)
        VkFence currentFence = vkSyncObjects->getInFlightFence(currentFrame);

        VkFence inFlightFence = vkSyncObjects->getInFlightFence(currentFrame);

        vkWaitForFences(vkContext->getDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX);

        updateUniformBuffer(currentFrame);

        // 2. Acquire the next image
        uint32_t imageIndex;
        vkAcquireNextImageKHR(vkContext->getDevice(), vkSwapchain->getSwapchain(), UINT64_MAX,
            vkSyncObjects->getImageAvailableSemaphore(currentFrame), VK_NULL_HANDLE, &imageIndex);

        // Handle resize logic (tracker update)
        if (imageIndex >= imagesInFlight.size()) {
            imagesInFlight.resize(vkSwapchain->getImageCount(), VK_NULL_HANDLE);
        }

        // 3. Image-in-Flight check
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(vkContext->getDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = currentFence;

        // 4. Reset the fence now that we are moving forward
        vkResetFences(vkContext->getDevice(), 1, &currentFence);

        // 5. Update data
        updateUniformBuffer(imageIndex);

        // FIX: Do NOT create a local renderQueue here. 
        // We use the 'renderQueue' argument passed into the function.

        vkCommandManager->recordCommands(
            imageIndex,
            vkRenderPass,
            vkFramebuffer,
            vkPipeline,
            vkSwapchain->getExtent(),
            renderQueue,
            globalDescriptorSets,
            &editor
        );

        // 6. Submit the work
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { vkSyncObjects->getImageAvailableSemaphore(currentFrame) };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &vkCommandManager->getCommandBuffer(imageIndex);

        VkSemaphore signalSemaphores[] = { vkSyncObjects->getRenderFinishedSemaphore(imageIndex) };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(vkContext->getGraphicsQueue(), 1, &submitInfo, currentFence) != VK_SUCCESS) {
            throw std::runtime_error("failed to submit draw command buffer!");
        }

        // 7. Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapChains[] = { vkSwapchain->getSwapchain() };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;

        vkQueuePresentKHR(vkContext->getPresentQueue(), &presentInfo);

        currentFrame = (currentFrame + 1) % VkSyncObjects::MAX_FRAMES_IN_FLIGHT;
    }

    void createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);

        size_t imageCount = vkSwapchain->getImageCount();
        uniformBuffers.resize(imageCount);
        uniformBuffersMemory.resize(imageCount);
        uniformBuffersMapped.resize(imageCount);

        for (size_t i = 0; i < imageCount; i++) {
            vkContext->createBuffer(bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                uniformBuffers[i], uniformBuffersMemory[i]);

            // Keep the memory mapped for the entire life of the app for high performance
            vkMapMemory(vkContext->getDevice(), uniformBuffersMemory[i], 0, bufferSize, 0, &uniformBuffersMapped[i]);
        }
    }

    void updateUniformBuffer(uint32_t currentImage) {
        UniformBufferObject ubo{};

        // 1. Restore the Model Matrix (Identity is fine for the car itself)
        ubo.model = glm::mat4(1.0f);

        // 2. Restore the Camera View (This enables WASD/Mouse)
        ubo.view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

        // 3. Restore Perspective (This fixes the "flat" look)
        float aspectRatio = vkSwapchain->getExtent().width / (float)vkSwapchain->getExtent().height;
        ubo.proj = glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 1000.0f);

        // Vulkan Y-Flip
        ubo.proj[1][1] *= -1;

        memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
    }

    void createDescriptorPool() {
        uint32_t imageCount = static_cast<uint32_t>(vkSwapchain->getImageCount());
        uint32_t materialCount = static_cast<uint32_t>(mainModel->materials.size());

        // 1. Calculate Limits
        // We need one set for the Global Camera PER FRAME
        // Plus one set for EACH Material PER FRAME
        uint32_t maxSets = (1 + materialCount) * imageCount;

        std::array<VkDescriptorPoolSize, 2> poolSizes{};

        // 2. Uniform Buffers (Only needed for Global Sets now)
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = imageCount; // We only need a few of these!

        // 3. Combined Image Samplers (Only needed for Material Sets)
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = materialCount * imageCount;

        // 4. Create the Pool
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        // Crucial: Make sure we have enough room for BOTH types of sets
        poolInfo.maxSets = maxSets;

        if (vkCreateDescriptorPool(vkContext->getDevice(), &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor pool!");
        }
    }

    void createDescriptorSets() {
        uint32_t imageCount = static_cast<uint32_t>(vkSwapchain->getImageCount());

        // Resize the vector to hold one set per frame
        globalDescriptorSets.resize(imageCount);

        // Get the layout we created for Set 0
        std::vector<VkDescriptorSetLayout> globalLayouts(imageCount, vkPipeline->getGlobalSetLayout());

        VkDescriptorSetAllocateInfo globalAllocInfo{};
        globalAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        globalAllocInfo.descriptorPool = descriptorPool;
        globalAllocInfo.descriptorSetCount = imageCount;
        globalAllocInfo.pSetLayouts = globalLayouts.data();

        if (vkAllocateDescriptorSets(vkContext->getDevice(), &globalAllocInfo, globalDescriptorSets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate global descriptor sets!");
        }

        // Update Global Sets with the Uniform Buffer
        for (size_t i = 0; i < imageCount; i++) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = globalDescriptorSets[i];
            descriptorWrite.dstBinding = 0; // Binding 0 in Set 0
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(vkContext->getDevice(), 1, &descriptorWrite, 0, nullptr);
        }

        size_t numMaterials = mainModel->materials.size();
        if (numMaterials == 0) return;

        // Get the layout we created for Set 1
        std::vector<VkDescriptorSetLayout> materialLayouts(imageCount, vkPipeline->getMaterialSetLayout());

        for (size_t m = 0; m < numMaterials; m++) {
            auto& material = mainModel->materials[m];

            VkDescriptorSetAllocateInfo materialAllocInfo{};
            materialAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            materialAllocInfo.descriptorPool = descriptorPool;
            materialAllocInfo.descriptorSetCount = imageCount;
            materialAllocInfo.pSetLayouts = materialLayouts.data();

            material.descriptorSets.resize(imageCount);

            if (vkAllocateDescriptorSets(vkContext->getDevice(), &materialAllocInfo, material.descriptorSets.data()) != VK_SUCCESS) {
                throw std::runtime_error("failed to allocate descriptor sets for material " + std::to_string(m));
            }

            // Determine which texture to use
            int imgIdx = material.textureIndex;
            if (imgIdx < 0 || imgIdx >= static_cast<int>(mainModel->textures.size())) {
                imgIdx = 0; // Fallback
            }

            // Update Material Sets with the Texture
            for (size_t i = 0; i < imageCount; i++) {
                VkDescriptorImageInfo imageInfo{};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView = mainModel->textures[imgIdx].view;
                imageInfo.sampler = mainModel->textures[imgIdx].sampler;

                VkWriteDescriptorSet descriptorWrite{};
                descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrite.dstSet = material.descriptorSets[i];

                descriptorWrite.dstBinding = 0;

                descriptorWrite.dstArrayElement = 0;
                descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrite.descriptorCount = 1;
                descriptorWrite.pImageInfo = &imageInfo;

                vkUpdateDescriptorSets(vkContext->getDevice(), 1, &descriptorWrite, 0, nullptr);
            }
        }
    }

    void createDepthResources() {
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT; // Standard high-precision depth format

        // Create the Image
        vkContext->createImage(
            vkSwapchain->getExtent().width,
            vkSwapchain->getExtent().height,
            depthFormat,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            depthImage,
            depthImageMemory
        );

        // Create the View
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; // It's a depth view!
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &depthImageView);
    }

    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
        VkCommandBuffer commandBuffer = vkCommandManager->beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;

        // Handle Aspect Mask
        if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        else {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags sourceStage;
        VkPipelineStageFlags destinationStage;

        // Transition for Depth Buffer
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        }
        // Transition for Textures
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else {
            throw std::invalid_argument("unsupported layout transition!");
        }

        vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkCommandManager->endSingleTimeCommands(commandBuffer);
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
        VkCommandBuffer commandBuffer = vkCommandManager->beginSingleTimeCommands();

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { width, height, 1 };

        vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        vkCommandManager->endSingleTimeCommands(commandBuffer);
    }

    void processInput(GLFWwindow* window) {
        // Calculate velocity based on time, not frame rate
        float velocity = cameraSpeed * deltaTime; // <--- ADD THIS

        // Forward/Backward
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            cameraPos += velocity * cameraFront;  // <--- USE velocity HERE
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            cameraPos -= velocity * cameraFront;

        // Left/Right
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            cameraPos -= glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            cameraPos += glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;

        // Up/Down
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            cameraPos += velocity * cameraUp;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
            cameraPos -= velocity * cameraUp;
    }

    void mainLoop() {
        Registry registry;
        Entity car = registry.createEntity();

        registry.addComponent<TransformComponent>(car, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
        registry.addComponent<MeshComponent>(car, mainModel);

        float lastFrameTime = 0.0f;

        while (!glfwWindowShouldClose(window)) {
            float currentFrameTime = static_cast<float>(glfwGetTime());
            deltaTime = currentFrameTime - lastFrameTime;
            lastFrameTime = currentFrameTime;

            glfwPollEvents();
            processInput(window);

            // --- UPDATE SYSTEM ---
            // 1. Only call this ONCE per frame
            editor.update(registry, assetManager);

            // 2. Build the render queue from the ECS
            std::vector<RenderObject> renderQueue;
            auto* meshPool = registry.getPool<MeshComponent>();
            auto* transformPool = registry.getPool<TransformComponent>();

            for (size_t i = 0; i < meshPool->components.size(); i++) {
                Entity e = meshPool->entities[i];
                if (transformPool->sparseMap.contains(e) && meshPool->components[i].enabled) {
                    RenderObject obj;
                    obj.model = meshPool->components[i].model;
                    obj.transform = transformPool->get(e).mat4();
                    renderQueue.push_back(obj);
                }
            }

            // --- RENDER SYSTEM ---
            drawFrame(renderQueue);

            // Update Window Title (FPS)
            static double lastFpsUpdate = 0;
            static int frameCount = 0;
            frameCount++;
            if (currentFrameTime - lastFpsUpdate >= 1.0) {
                std::string title = "Iridium Engine | " + std::to_string(frameCount) + " FPS";
                glfwSetWindowTitle(window, title.c_str());
                frameCount = 0;
                lastFpsUpdate = currentFrameTime;
            }
        }
        vkDeviceWaitIdle(vkContext->getDevice());
    }

    void cleanup() {
        vkDeviceWaitIdle(vkContext->getDevice());
        editor.cleanup(vkContext->getDevice());

        // 1. AssetManager destructor cleans up all Models, Buffers, and Textures automatically.
        delete assetManager;

        // 2. Clean up "Global" resources that belong to the Engine, not the Assets.
        vkDestroyImageView(vkContext->getDevice(), depthImageView, nullptr);
        vkDestroyImage(vkContext->getDevice(), depthImage, nullptr);
        vkFreeMemory(vkContext->getDevice(), depthImageMemory, nullptr);

        for (size_t i = 0; i < uniformBuffers.size(); i++) {
            vkDestroyBuffer(vkContext->getDevice(), uniformBuffers[i], nullptr);
            vkFreeMemory(vkContext->getDevice(), uniformBuffersMemory[i], nullptr);
        }

        vkDestroyDescriptorPool(vkContext->getDevice(), descriptorPool, nullptr);

        delete vkSyncObjects;
        delete vkCommandManager;
        delete vkFramebuffer;
        delete vkPipeline;
        delete vkRenderPass;
        delete vkSwapchain;
        delete vkContext; // Context goes last (mostly)

        glfwDestroyWindow(window);
        glfwTerminate();
    }
};

int main() {
    IridiumEngine app;
    try {
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}