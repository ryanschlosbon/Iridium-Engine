#define GLFW_INCLUDE_VULKAN 
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> 
#include <glm/gtc/type_ptr.hpp>         

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
	VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    VkDescriptorPool descriptorPool;
    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    glm::vec2 squarePos = { 0.0f, 0.0f };
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    std::vector<VkDescriptorSet> descriptorSets;
    uint32_t currentFrame = 0;
    std::vector<VkFence> imagesInFlight;

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
    float lastFrame = 0.0f;

    // Define a Cube (24 vertices, 4 per face)
    const std::vector<Vertex> vertices = {
        // Front face (Red)
        {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},

        // Back face (Cyan)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}},

        // Left face (Green)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},

        // Right face (Magenta)
        {{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}},

        // Top face (Blue)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},

        // Bottom face (Yellow)
        {{-0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}}
    };

    // 36 Indices (6 faces * 2 triangles * 3 vertices)
    const std::vector<uint16_t> indices = {
        // Front face (Red) - Vertices 0,1,2,3
        0, 1, 2, 2, 3, 0,

        // Back face (Cyan) - Vertices 4,5,6,7
        4, 7, 6, 6, 5, 4,

        // Left face (Green) - Vertices 8,9,10,11
        8, 9, 10, 10, 11, 8,

        // Right face (Magenta) - Vertices 12,13,14,15
        12, 14, 13, 14, 12, 15,

        // Top face (Blue) - Vertices 16,17,18,19
        16, 17, 18, 18, 19, 16,

        // Bottom face (Yellow) - Vertices 20,21,22,23
        20, 23, 22, 22, 21, 20
    };

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
        // Initialize the Context
        // true = Enable Validation Layers
        // window = pass the GLFW window so the context can create the Surface
        vkContext = new VkContext(true, window);
        vkSwapchain = new VkSwapchain(vkContext, window);
        vkRenderPass = new VkRenderPassWrapper(vkContext, vkSwapchain);
        vkPipeline = new VkGraphicsPipeline(vkContext, vkSwapchain, vkRenderPass);

        createDepthResources();

        vkFramebuffer = new VkFramebufferWrapper(vkContext, vkSwapchain, vkRenderPass, depthImageView);
        vkCommandManager = new VkCommandManager(vkContext, vkFramebuffer, vkPipeline, vkSwapchain->getImageCount());
        vkSyncObjects = new VkSyncObjects(vkContext, vkSwapchain->getImageCount());
        imagesInFlight.resize(vkSwapchain->getImageCount(), VK_NULL_HANDLE);

        createVertexBuffer();
        createIndexBuffer();

        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
    }

    void drawFrame(MeshPushConstants constants) {
        // 1. Wait for CPU-GPU sync (Frame 0 or 1)
        VkFence currentFence = vkSyncObjects->getInFlightFence(currentFrame);
        vkWaitForFences(vkContext->getDevice(), 1, &currentFence, VK_TRUE, UINT64_MAX);

        // 2. Acquire the next image
        uint32_t imageIndex;
        vkAcquireNextImageKHR(
            vkContext->getDevice(),
            vkSwapchain->getSwapchain(),
            UINT64_MAX,
            vkSyncObjects->getImageAvailableSemaphore(currentFrame),
            VK_NULL_HANDLE,
            &imageIndex
        );

        VkSemaphore signalSem = vkSyncObjects->getRenderFinishedSemaphore(imageIndex);

        if (imageIndex >= imagesInFlight.size()) {
            // If this hits, the swapchain likely resized, and we need to handle it.
            // For now, let's just resize the tracker to match.
            imagesInFlight.resize(vkSwapchain->getImageCount(), VK_NULL_HANDLE);
        }

        // 3. Image-in-Flight check: Ensures we don't reuse an image still being presented
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(vkContext->getDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = currentFence;

        // 4. Reset the fence now that we are moving forward
        vkResetFences(vkContext->getDevice(), 1, &currentFence);

        // 5. Update data and record commands
        updateUniformBuffer(imageIndex);
        vkCommandManager->recordCommands(imageIndex, vkRenderPass, vkFramebuffer, vkPipeline,
            vkSwapchain->getExtent(), vertexBuffer, indexBuffer,
            static_cast<uint32_t>(indices.size()), constants, descriptorSets);

        // 6. Submit the work
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        // Wait for the image to be available before writing to the Color Attachment
        VkSemaphore waitSemaphores[] = { vkSyncObjects->getImageAvailableSemaphore(currentFrame) };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT }; // <--- DON'T MISS THIS
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &vkCommandManager->getCommandBuffer(imageIndex);

        // Signal that rendering is done using the IMAGE's dedicated semaphore
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
        presentInfo.pWaitSemaphores = signalSemaphores; // Wait for the render to finish

        VkSwapchainKHR swapChains[] = { vkSwapchain->getSwapchain() };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;

        vkQueuePresentKHR(vkContext->getPresentQueue(), &presentInfo);

        // Cycle the frame index (0 -> 1 -> 0)
        currentFrame = (currentFrame + 1) % VkSyncObjects::MAX_FRAMES_IN_FLIGHT;
    }

    void createVertexBuffer() {
		VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

        // usage: It's a vertex buffer
		// properties: HOST_VISIBLE (CPU can write to it) | HOST_COHERENT (Changes are visible immediately)
		vkContext->createBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
			vertexBuffer, vertexBufferMemory);

        // Map the memory
        void* data;
		vkMapMemory(vkContext->getDevice(), vertexBufferMemory, 0, bufferSize, 0, &data);

        // Copy the vertex data
        memcpy(data, vertices.data(), (size_t)bufferSize);

		// Unmap the memory (to flush the writes)
		vkUnmapMemory(vkContext->getDevice(), vertexBufferMemory);
    }

    void createIndexBuffer() {
        VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();
        vkContext->createBuffer(bufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            indexBuffer, indexBufferMemory);

        void* data;
        vkMapMemory(vkContext->getDevice(), indexBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices.data(), (size_t)bufferSize);
        vkUnmapMemory(vkContext->getDevice(), indexBufferMemory);
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
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

        UniformBufferObject ubo{};

        // 1. Rotation (Rotate around the Z axis)
        ubo.model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(1.0f, 1.0f, 0.0f));

		// 2. View (uses camera position and direction)
        ubo.view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

        // 3. Perspective (FOV, Aspect Ratio, Near plane, Far plane)
        float aspectRatio = vkSwapchain->getExtent().width / (float)vkSwapchain->getExtent().height;
        ubo.proj = glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 10.0f);

        // 4. Vulkan Y-Axis Flip Fix
        ubo.proj[1][1] *= -1;

        memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
    }

    void createDescriptorPool() {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = static_cast<uint32_t>(vkSwapchain->getImageCount());

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = static_cast<uint32_t>(vkSwapchain->getImageCount());

        if (vkCreateDescriptorPool(vkContext->getDevice(), &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor pool!");
        }
    }

    void createDescriptorSets() {
        std::vector<VkDescriptorSetLayout> layouts(vkSwapchain->getImageCount(), 
            vkPipeline->getDescriptorSetLayout());

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(vkSwapchain->getImageCount());
        allocInfo.pSetLayouts = layouts.data();

        descriptorSets.resize(vkSwapchain->getImageCount());
        if (vkAllocateDescriptorSets(vkContext->getDevice(), &allocInfo, descriptorSets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate descriptor sets!");
        }

        // Connect each buffer to its set
        for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = descriptorSets[i];
            descriptorWrite.dstBinding = 0; // Matches 'layout(binding = 0)' in shader
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(vkContext->getDevice(), 1, &descriptorWrite, 0, nullptr);
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

    void mainLoop() {
        double lastTime = glfwGetTime();
        int nbFrames = 0;

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // 1. Calculate Delta Time (Time per frame)
            float currentFrameTime = (float)glfwGetTime();
            deltaTime = currentFrameTime - lastFrame;
            lastFrame = currentFrameTime;

            // 2. Camera Controls (WASD)
            float velocity = cameraSpeed * deltaTime;

            // Move Forward/Backward
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                cameraPos += cameraSpeed * cameraFront * deltaTime;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                cameraPos -= cameraSpeed * cameraFront * deltaTime;

            // Move Left/Right (Cross product gets the vector perpendicular to "Forward" and "Up")
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                cameraPos -= glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                cameraPos += glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;

            // 3. Optional: Vertical Movement (Space/Shift)
            if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
                cameraPos += cameraUp * velocity;
            if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                cameraPos -= cameraUp * velocity;

            // Pass empty constants for now (or keep using them for the object position if you like)
            MeshPushConstants constants{};
            constants.offset = squarePos; // We keep this if you still want the object to move separately
            constants.scale = glm::vec2(1.0f);

            // Measure FPS
            double currentTime = glfwGetTime();
            nbFrames++;

            if (currentTime - lastTime >= 1.0) {
                // Create title string: "Iridium Engine - [ 144 FPS ]"
                std::string title = "Iridium Engine - [ " + std::to_string(nbFrames) + " FPS ]";
                glfwSetWindowTitle(window, title.c_str());

                // Reset
                nbFrames = 0;
                lastTime += 1.0;
            }

            drawFrame(constants);
        }
        vkDeviceWaitIdle(vkContext->getDevice());
    }

    void cleanup() {
        vkDestroyImageView(vkContext->getDevice(), depthImageView, nullptr);
        vkDestroyImage(vkContext->getDevice(), depthImage, nullptr);
        vkFreeMemory(vkContext->getDevice(), depthImageMemory, nullptr);

        for (size_t i = 0; i < uniformBuffers.size(); i++) {
            vkDestroyBuffer(vkContext->getDevice(), uniformBuffers[i], nullptr);
            vkFreeMemory(vkContext->getDevice(), uniformBuffersMemory[i], nullptr);
        }

        vkDestroyDescriptorPool(vkContext->getDevice(), descriptorPool, nullptr);
        vkDestroyBuffer(vkContext->getDevice(), indexBuffer, nullptr);
        vkFreeMemory(vkContext->getDevice(), indexBufferMemory, nullptr);
        vkDestroyBuffer(vkContext->getDevice(), vertexBuffer, nullptr);
        vkFreeMemory(vkContext->getDevice(), vertexBufferMemory, nullptr);

        delete vkSyncObjects;
        delete vkCommandManager;
        delete vkFramebuffer;
        delete vkPipeline;
        delete vkRenderPass;
        delete vkSwapchain;
        delete vkContext;

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