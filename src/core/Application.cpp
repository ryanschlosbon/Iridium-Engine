#include "Application.h"
#include "scene/Components.h"

// Third-party and system includes
#include <glm/gtc/matrix_transform.hpp> 
#include <glm/gtc/type_ptr.hpp>    
#include <iostream>
#include <stdexcept> 
#include <cstring>   
#include <chrono>
#include <algorithm>

#include "backends/imgui_impl_vulkan.h"

const int WIDTH = 1280;
const int HEIGHT = 720;

void Application::run() {
    initWindow();
    initVulkan();
    mainLoop();
    cleanup();
}

void Application::mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    auto* app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));

    if (ImGui::GetIO().WantCaptureMouse && !app->isRightMouseButtonDown && !app->isMiddleMouseButtonDown) {
        return;
    }

    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);

    if (!app->isRightMouseButtonDown && !app->isMiddleMouseButtonDown) {
        app->firstMouse = true;
        return;
    }

    if (app->firstMouse) {
        app->lastX = xpos;
        app->lastY = ypos;
        app->firstMouse = false;
        return;
    }

    float xoffset = xpos - app->lastX;
    float yoffset = app->lastY - ypos;

    app->lastX = xpos;
    app->lastY = ypos;

    if (std::abs(xoffset) > 100.0f || std::abs(yoffset) > 100.0f) {
        return;
    }

    if (app->isRightMouseButtonDown) {
        float sensitivity = app->mouseSensitivity;

        app->yaw += xoffset * sensitivity;
        app->pitch += yoffset * sensitivity;

        if (app->pitch > 89.0f) app->pitch = 89.0f;
        if (app->pitch < -89.0f) app->pitch = -89.0f;

        glm::vec3 front;
        front.x = cos(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
        front.y = sin(glm::radians(app->pitch));
        front.z = sin(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
        app->cameraFront = glm::normalize(front);
    }

    if (app->isMiddleMouseButtonDown) {
        float panSpeed = app->cameraSpeed * 0.005f;

        glm::vec3 cameraRight = glm::normalize(glm::cross(app->cameraFront, app->cameraUp));
        glm::vec3 cameraTrueUp = glm::normalize(glm::cross(cameraRight, app->cameraFront));

        app->cameraPos -= cameraRight * (xoffset * panSpeed);
        app->cameraPos -= cameraTrueUp * (yoffset * panSpeed);
    }
}

void Application::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    auto* app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));

    if (!app->editor.getViewportPanel().isFocused) {
        return;
    }

    app->cameraSpeed += (float)yoffset * 0.5f;

    if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
    if (app->cameraSpeed > 10.0f) app->cameraSpeed = 10.0f;

    std::cout << "Camera Speed: " << app->cameraSpeed << std::endl;
}

void Application::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    auto* app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
    ViewportPanel& viewport = app->editor.getViewportPanel();

    // 1. Focus Gate: Only process clicks if the viewport is the active window
    if (!viewport.isFocused) return;

    // 2. UI Guard: If clicking a button/combo-box, abort selection
    if (ImGui::IsAnyItemHovered()) return;

    // --- LEFT CLICK: Selection ---
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        app->selectEntityAtMouse(viewport.mouseX, viewport.mouseY);
    }

    // --- MIDDLE CLICK: Panning ---
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS) {
            app->isMiddleMouseButtonDown = true;
            app->firstMouse = true; // Prevents "jumping" when pan starts
        }
        else if (action == GLFW_RELEASE) {
            app->isMiddleMouseButtonDown = false;
        }
    }
}

void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
    app->framebufferResized = true;
}

void Application::initWindow() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(WIDTH, HEIGHT, "Iridium Engine", nullptr, nullptr);
    glfwSetWindowUserPointer(window, this);

    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);

    glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
}

void Application::initVulkan() {
    vkContext = new VkContext(enableValidation, window);
    vkSwapchain = new VkSwapchain(vkContext, window);
    vkRenderPass = new VkRenderPassWrapper(vkContext, vkSwapchain);
    vkPipeline = new VkGraphicsPipeline(vkContext, vkSwapchain, vkRenderPass);
    vkSyncObjects = new VkSyncObjects(vkContext, vkSwapchain->getImageCount());
    vkCommandManager = new VkCommandManager(vkContext, vkFramebuffer, vkPipeline, VkSyncObjects::MAX_FRAMES_IN_FLIGHT);

    createDepthResources();
    createOffscreenRenderTarget();
    vkFramebuffer = new VkFramebufferWrapper(
        vkContext,
        vkRenderPass,
        sceneColorImageViews,
        depthImageViews,
        vkSwapchain->getExtent()
    );

    assetManager = new AssetManager(vkContext, vkCommandManager);

    std::string modelPath = std::string(PROJECT_ROOT_DIR) + "assets/models/alfa_romeo/scene.gltf";
    mainModel = assetManager->getModel(modelPath);

    createUniformBuffers();
    descriptorAllocator.init(vkContext->getDevice());
    createDescriptorSets();

    vkUIRenderPass = new VkUIRenderPass(vkContext, vkSwapchain->getImageFormat());
    createUIFramebuffers();

    editor.init(
        vkContext->getInstance(),
        vkContext->getDevice(),
        vkContext->getPhysicalDevice(),
        vkContext->getGraphicsQueue(),
        vkUIRenderPass->getRenderPass(),
        window,
        vkCommandManager->getCommandPool()
    );

    // Register our custom off-screen textures with Dear ImGui!
    sceneDescriptorSets.resize(vkSwapchain->getImageCount());
    for (size_t i = 0; i < sceneDescriptorSets.size(); i++) {
        sceneDescriptorSets[i] = ImGui_ImplVulkan_AddTexture(
            sceneSampler,
            sceneColorImageViews[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
    }
}

void Application::drawFrame(Registry& registry, const glm::mat4& view, const glm::mat4& proj) {
    VkFence currentFence = vkSyncObjects->getInFlightFence(currentFrame);
    VkFence inFlightFence = vkSyncObjects->getInFlightFence(currentFrame);

    vkWaitForFences(vkContext->getDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX);
    frameDeletionQueues[currentFrame].flush();

    updateUniformBuffer(currentFrame, view, proj);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(vkContext->getDevice(), vkSwapchain->getSwapchain(), 
        UINT64_MAX, vkSyncObjects->getImageAvailableSemaphore(currentFrame), VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return; // Skip the rest of this frame
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swap chain image!");
    }

    if (imageIndex >= imagesInFlight.size()) {
        imagesInFlight.resize(vkSwapchain->getImageCount(), VK_NULL_HANDLE);
    }

    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(vkContext->getDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight[imageIndex] = currentFence;

    vkResetFences(vkContext->getDevice(), 1, &currentFence);

    vkCommandManager->recordCommands(
        currentFrame,       // 0. Current frame index (CPU)
        imageIndex,         // 1. Current frame index (GPU)
        vkRenderPass,       // 2. Off-screen Pass wrapper
        vkFramebuffer,      // 3. Off-screen Framebuffer wrapper
        vkUIRenderPass,     // 4. NEW: The UI Pass wrapper
        uiFramebuffers,     // 5. NEW: The UI Framebuffer vector
        vkPipeline,         // 6. Graphics Pipeline
        vkSwapchain->getExtent(), // 7. Extent
        registry,           // 8. ECS Registry
        globalDescriptorSets[currentFrame], // 9. Camera Data
        &editor             // 10. Editor System pointer
    );

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = { vkSyncObjects->getImageAvailableSemaphore(currentFrame) };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkCommandManager->getCommandBuffer(currentFrame);

    VkSemaphore signalSemaphores[] = { vkSyncObjects->getRenderFinishedSemaphore(imageIndex) };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(vkContext->getGraphicsQueue(), 1, &submitInfo, currentFence) != VK_SUCCESS) {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = { vkSwapchain->getSwapchain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presentResult = vkQueuePresentKHR(vkContext->getPresentQueue(), &presentInfo);

    // Check both the Vulkan result and our custom GLFW boolean
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || wasWindowResized()) {
        resetWindowResizedFlag();
        recreateSwapchain();
        }
    else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swap chain image!");
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    currentFrame = (currentFrame + 1) % VkSyncObjects::MAX_FRAMES_IN_FLIGHT;
}

void Application::createUniformBuffers() {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);

    size_t frameCount = VkSyncObjects::MAX_FRAMES_IN_FLIGHT;

    uniformBuffers.resize(frameCount);
    uniformBuffersMemory.resize(frameCount);
    uniformBuffersMapped.resize(frameCount);

    for (size_t i = 0; i < frameCount; i++) {
        vkContext->createBuffer(bufferSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            uniformBuffers[i], uniformBuffersMemory[i]);

        vkMapMemory(vkContext->getDevice(), uniformBuffersMemory[i], 0, bufferSize, 0, &uniformBuffersMapped[i]);
    }
}

void Application::createUIFramebuffers() {
    uiFramebuffers.resize(vkSwapchain->getImageCount());

    for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
        // UI Framebuffers only need the Color attachment (the Swapchain image)
        VkImageView attachments[] = { vkSwapchain->getImageViews()[i] };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = vkUIRenderPass->getRenderPass();
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = vkSwapchain->getExtent().width;
        framebufferInfo.height = vkSwapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(vkContext->getDevice(), &framebufferInfo, nullptr, &uiFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create UI framebuffer!");
        }
    }
}

void Application::updateUniformBuffer(uint32_t currentImage, const glm::mat4& view, const glm::mat4& proj) {
    UniformBufferObject ubo{};
    ubo.model = glm::mat4(1.0f);
    ubo.view = view;
    ubo.proj = proj;
    memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
}

void Application::allocateMaterialDescriptors(std::shared_ptr<ModelAsset> model) {
    uint32_t frameCount = VkSyncObjects::MAX_FRAMES_IN_FLIGHT;
    size_t numMaterials = model->materials.size();
    if (numMaterials == 0) return;

    for (size_t m = 0; m < numMaterials; m++) {
        auto& material = model->materials[m];

        // If this material already has descriptors (from a previous load), skip it!
        if (!material.descriptorSets.empty()) continue;

        material.descriptorSets.resize(frameCount);

        int imgIdx = material.textureIndex;
        if (imgIdx < 0 || imgIdx >= static_cast<int>(model->textures.size())) {
            imgIdx = 0;
        }

        for (size_t i = 0; i < frameCount; i++) {
            // Dynamically allocate the Material Set
            material.descriptorSets[i] = descriptorAllocator.allocate(vkPipeline->getMaterialSetLayout());

            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = model->textures[imgIdx].view;
            imageInfo.sampler = model->textures[imgIdx].sampler;

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

void Application::createDescriptorSets() {
    uint32_t frameCount = VkSyncObjects::MAX_FRAMES_IN_FLIGHT;
    globalDescriptorSets.resize(frameCount);

    for (size_t i = 0; i < frameCount; i++) {
        // 1. Dynamically allocate the Global Set using our new class
        globalDescriptorSets[i] = descriptorAllocator.allocate(vkPipeline->getGlobalSetLayout());

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = globalDescriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(vkContext->getDevice(), 1, &descriptorWrite, 0, nullptr);
    }

    // 2. Allocate materials for the starting model
    allocateMaterialDescriptors(mainModel);
}

void Application::createDepthResources() {
    uint32_t imageCount = vkSwapchain->getImageCount();
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    depthImages.resize(imageCount);
    depthImageMemories.resize(imageCount);
    depthImageViews.resize(imageCount);

    for (size_t i = 0; i < imageCount; i++) {
        vkContext->createImage(
            vkSwapchain->getExtent().width, vkSwapchain->getExtent().height, depthFormat,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            depthImages[i], depthImageMemories[i]
        );

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &depthImageViews[i]);
    }
}

void Application::transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = vkCommandManager->beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;

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

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }
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

void Application::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
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

void Application::processInput(GLFWwindow* window) {
    ViewportPanel& viewport = editor.getViewportPanel();

    if (viewport.isHovered && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        if (!isRightMouseButtonDown) {
            isRightMouseButtonDown = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            firstMouse = true;
        }
    }
    else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE) {
        if (isRightMouseButtonDown) {
            isRightMouseButtonDown = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }

    if (!viewport.isFocused) return;

    float velocity = cameraSpeed * deltaTime;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        cameraPos += velocity * cameraFront;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        cameraPos -= velocity * cameraFront;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        cameraPos -= glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        cameraPos += glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
        cameraPos += velocity * cameraUp;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        cameraPos -= velocity * cameraUp;
}

void Application::selectEntityAtMouse(double mouseX, double mouseY) {
    if (ImGui::IsAnyItemHovered()) return;

    ViewportPanel& viewport = editor.getViewportPanel();

    // USE THE VIEWPORT SIZE, NOT THE SWAPCHAIN SIZE!
    float currentWidth = viewport.viewportWidth;
    float currentHeight = viewport.viewportHeight;

    // Prevent divide-by-zero crashes if the window is minimized
    if (currentWidth <= 0.0f || currentHeight <= 0.0f) return;

    float aspectRatio = currentWidth / currentHeight;

    glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 1000.0f);

    // NDC Calculation (Now correctly mapped to the 0.0-1.0 space of the ImGui panel)
    float x = (2.0f * static_cast<float>(mouseX)) / currentWidth - 1.0f;
    float y = 1.0f - (2.0f * static_cast<float>(mouseY)) / currentHeight;

    glm::vec4 rayClip = glm::vec4(x, y, -1.0f, 1.0f);
    glm::vec4 rayEye = glm::inverse(proj) * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
    glm::vec3 rayWorld = glm::normalize(glm::vec3(glm::inverse(view) * rayEye));

    float closestDist = 10000.0f;
    Entity closestEntity = NULL_ENTITY;

    auto* meshPool = registry.getPool<MeshComponent>();
    auto* transformPool = registry.getPool<TransformComponent>();

    for (uint32_t entity : meshPool->entities) {
        if (!transformPool->sparseMap.contains(entity)) continue;

        auto& transform = transformPool->get(entity);
        float radius = 1.5f * std::max({ transform.scale.x, transform.scale.y, transform.scale.z });

        glm::vec3 sphereCenter = transform.position;
        glm::vec3 toSphere = sphereCenter - cameraPos;
        float t = glm::dot(toSphere, rayWorld);

        if (t > 0.0f) {
            glm::vec3 closestPoint = cameraPos + (rayWorld * t);
            float dist = glm::distance(closestPoint, sphereCenter);

            if (dist < radius) {
                if (t < closestDist) {
                    closestDist = t;
                    closestEntity = entity;
                }
            }
        }
    }

    editor.setSelectedEntity(closestEntity);
}

void Application::ProcessMeshSwaps(Registry& registry, AssetManager* assetManager, uint32_t currentFrame) {
    auto* meshPool = registry.getPool<MeshComponent>();
    if (!meshPool) return;

    for (uint32_t entity : meshPool->entities) {
        auto& meshComp = meshPool->get(entity);

        if (!meshComp.requestedMeshPath.empty()) {
            try {
                std::string fullPath = std::string(PROJECT_ROOT_DIR) + meshComp.requestedMeshPath;

                if (meshComp.model) {
                    std::shared_ptr<ModelAsset> oldModel = meshComp.model;
                    frameDeletionQueues[currentFrame].push_function([oldModel]() {});
                }

                meshComp.model = assetManager->getModel(fullPath);

                allocateMaterialDescriptors(meshComp.model);

                std::cout << "Successfully swapped mesh to: " << fullPath << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "Failed to swap mesh: " << e.what() << "\n";
            }

            meshComp.requestedMeshPath.clear();
        }
    }
}

void Application::mainLoop() {
    TransformSystem transformSystem;
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
        ProcessMeshSwaps(registry, assetManager, currentFrame);

        glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

        // Use the Viewport Panel's dimensions for the Camera Aspect Ratio
        float vWidth = std::max(1.0f, editor.getViewportPanel().viewportWidth);
        float vHeight = std::max(1.0f, editor.getViewportPanel().viewportHeight);
        float aspectRatio = vWidth / vHeight;

        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 1000.0f);
        glm::mat4 editorProj = proj;

        proj[1][1] *= -1;

        editor.update(registry, assetManager, view, editorProj, sceneDescriptorSets[currentFrame]);
        transformSystem.update(registry);
        drawFrame(registry, view, proj);

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

void Application::recreateSwapchain() {
    // 1. Handle Minimization
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }

    // 2. Wait for GPU to finish
    vkDeviceWaitIdle(vkContext->getDevice());

    // 3. CLEANUP OLD
    delete vkSyncObjects;
    for (auto fb : uiFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), fb, nullptr);
    }
    delete vkUIRenderPass;

    for (auto descSet : sceneDescriptorSets) {
        ImGui_ImplVulkan_RemoveTexture(descSet);
    }

    delete vkFramebuffer;

    // Destroy the arrays of Depth AND Color resources!
    vkDestroySampler(vkContext->getDevice(), sceneSampler, nullptr);
    for (size_t i = 0; i < sceneColorImages.size(); i++) {
        vkDestroyImageView(vkContext->getDevice(), sceneColorImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), sceneColorImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), sceneColorImageMemories[i], nullptr);

        vkDestroyImageView(vkContext->getDevice(), depthImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), depthImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), depthImageMemories[i], nullptr);
    }

    // Deleting this triggers your VkSwapchain::~VkSwapchain() automatically!
    delete vkSwapchain;

    // 4. CREATE NEW 
    vkSwapchain = new VkSwapchain(vkContext, window);
    vkSyncObjects = new VkSyncObjects(vkContext, vkSwapchain->getImageCount());
    imagesInFlight.assign(vkSwapchain->getImageCount(), VK_NULL_HANDLE); // Purge the old fences
    // Recreate both Color and Depth off-screen targets!
    createOffscreenRenderTarget();
    createDepthResources();

    // 5. Register the new textures with ImGui
    sceneDescriptorSets.resize(vkSwapchain->getImageCount());
    for (size_t i = 0; i < sceneDescriptorSets.size(); i++) {
        sceneDescriptorSets[i] = ImGui_ImplVulkan_AddTexture(
            sceneSampler,
            sceneColorImageViews[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
    }

    // 6. Rebuild Framebuffers
    vkFramebuffer = new VkFramebufferWrapper(vkContext, vkRenderPass, sceneColorImageViews,
        depthImageViews, vkSwapchain->getExtent());

    // ---> NEW: Rebuild the UI Render Pass and Framebuffers <---
    vkUIRenderPass = new VkUIRenderPass(vkContext, vkSwapchain->getImageFormat());
    createUIFramebuffers();
    // ---------------------------------------------------------
}

void Application::createOffscreenRenderTarget() {
    uint32_t imageCount = vkSwapchain->getImageCount();
    VkExtent2D extent = vkSwapchain->getExtent();

    sceneColorImages.resize(imageCount);
    sceneColorImageMemories.resize(imageCount);
    sceneColorImageViews.resize(imageCount);

    for (size_t i = 0; i < imageCount; i++) {
        vkContext->createImage(
            extent.width, extent.height, vkSwapchain->getImageFormat(),
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            sceneColorImages[i], sceneColorImageMemories[i]
        );

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = sceneColorImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vkSwapchain->getImageFormat();
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &sceneColorImageViews[i]);
    }

    // Create the ONE Sampler (outside the loop)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr, &sceneSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scene sampler!");
    }

    for (size_t i = 0; i < sceneColorImages.size(); i++) {
        vkCommandManager->transitionImageLayout(
            sceneColorImages[i],
            vkSwapchain->getImageFormat(), // The format parameter doesn't matter for your function, but pass it anyway
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
    }
}
void Application::cleanup() {
    vkDeviceWaitIdle(vkContext->getDevice());
    editor.cleanup(vkContext->getDevice());

    delete assetManager;

    vkDestroySampler(vkContext->getDevice(), sceneSampler, nullptr);
    for (size_t i = 0; i < sceneColorImages.size(); i++) {
        vkDestroyImageView(vkContext->getDevice(), sceneColorImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), sceneColorImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), sceneColorImageMemories[i], nullptr);

        vkDestroyImageView(vkContext->getDevice(), depthImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), depthImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), depthImageMemories[i], nullptr);
    }

    for (size_t i = 0; i < uniformBuffers.size(); i++) {
        vkDestroyBuffer(vkContext->getDevice(), uniformBuffers[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), uniformBuffersMemory[i], nullptr);
    }

    descriptorAllocator.cleanup();

    for (auto framebuffer : uiFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), framebuffer, nullptr);
    }

    delete vkUIRenderPass;
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