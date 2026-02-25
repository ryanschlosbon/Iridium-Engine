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
    descriptorAllocator.init(vkContext->getDevice());

    assetManager = new AssetManager(vkContext, vkCommandManager);

    // Plug the Application's descriptor function directly into the AssetManager!
    assetManager->onModelLoadedCallback = [this](std::shared_ptr<ModelAsset> model) {
        this->allocateMaterialDescriptors(model);
        };

    hdriMap = assetManager->loadHDRI(std::string(PROJECT_ROOT_DIR) + "assets/hdri/cobblestone_street_night_4k.hdr");

    createLightingRenderPass(); 
    vkLightingPipeline = new VkLightingPipeline(vkContext, lightingRenderPass);
    createLightingDescriptorSets();
    createLightingFramebuffers();

    // 1. Create the Pass and Pipeline
// (Assuming your lit scene format is the swapchain format, adjust if you use a custom HDR format)
    vkForwardRenderPass = new VkForwardRenderPass(vkContext, vkSwapchain->getImageFormat(), 
        findDepthFormat(vkContext->getPhysicalDevice()));
    vkForwardPipeline = new VkForwardPipeline(vkContext, vkForwardRenderPass, vkLightingPipeline->getDescriptorSetLayout());


    // 2. Create the Forward Framebuffers
    forwardFramebuffers.resize(vkSwapchain->getImageCount());
    for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
        std::array<VkImageView, 2> attachments = {
            litSceneImageViews[i], // Color: The finished lighting pass!
            depthImageViews[i]     // Depth: The G-Buffer's depth map!
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = vkForwardRenderPass->getRenderPass();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = vkSwapchain->getExtent().width;
        framebufferInfo.height = vkSwapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(vkContext->getDevice(), &framebufferInfo, nullptr, &forwardFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create forward framebuffers!");
        }
    }

    vkFramebuffer = new VkFramebufferWrapper(
        vkContext,
        vkRenderPass,
        gPositionImageViews,
        gNormalImageViews,
        gAlbedoImageViews,
        depthImageViews,
        vkSwapchain->getExtent()
    );

    std::string modelPath = std::string(PROJECT_ROOT_DIR) + "assets/models/alfa_romeo/scene.gltf";
    mainModel = assetManager->getModel(modelPath);
    createUniformBuffers();
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
            gBufferSampler,
            litSceneImageViews[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
    }
}

void Application::drawFrame(Registry& registry, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& editorProj) {
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

    editor.update(registry, assetManager, view, editorProj, sceneDescriptorSets[imageIndex]);

    if (imageIndex >= imagesInFlight.size()) {
        imagesInFlight.resize(vkSwapchain->getImageCount(), VK_NULL_HANDLE);
    }

    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(vkContext->getDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight[imageIndex] = currentFence;

    vkResetFences(vkContext->getDevice(), 1, &currentFence);

    vkCommandManager->recordCommands(
        currentFrame,
        imageIndex,
        vkRenderPass,
        vkFramebuffer,
        lightingRenderPass,
        lightingFramebuffers[imageIndex],
        vkLightingPipeline,
        lightingDescriptorSets[imageIndex],
        cameraPos,
        view,
        proj,
        vkUIRenderPass,
        uiFramebuffers,
        vkPipeline,
        vkSwapchain->getExtent(),
        registry,
        globalDescriptorSets[currentFrame],
        &editor,
        vkForwardRenderPass,
        vkForwardPipeline,
        forwardFramebuffers,
        litSceneImages[imageIndex],
        opaqueSceneCopyImages[imageIndex]
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

        // If this material already has descriptors, skip it!
        if (!material.descriptorSets.empty()) continue;

        material.descriptorSets.resize(frameCount);

        // Grab the 3 precise indices AssetManager loaded for us
        int albedoIdx = material.albedoTextureIndex;
        int normalIdx = material.normalTextureIndex;
        int mrIdx = material.metallicRoughnessTextureIndex;

        for (size_t i = 0; i < frameCount; i++) {
            material.descriptorSets[i] = descriptorAllocator.allocate(vkPipeline->getMaterialSetLayout());

            // 1. Assign the info directly to the material struct so it never leaves scope!
            material.albedoInfo = { model->textures[albedoIdx].sampler, model->textures[albedoIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            material.normalInfo = { model->textures[normalIdx].sampler, model->textures[normalIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            material.pbrInfo = { model->textures[mrIdx].sampler, model->textures[mrIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            std::array<VkWriteDescriptorSet, 3> descriptorWrites{};

            // Binding 0: Albedo
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = material.descriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].dstArrayElement = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pImageInfo = &material.albedoInfo; // <--- Safe Pointer

            // Binding 1: Normal
            descriptorWrites[1] = descriptorWrites[0];
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].pImageInfo = &material.normalInfo; // <--- Safe Pointer

            // Binding 2: Metallic / Roughness
            descriptorWrites[2] = descriptorWrites[0];
            descriptorWrites[2].dstBinding = 2;
            descriptorWrites[2].pImageInfo = &material.pbrInfo; // <--- Safe Pointer

            vkUpdateDescriptorSets(vkContext->getDevice(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
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
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
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
                    frameDeletionQueues[currentFrame].push_function([this, oldModel]() {
                        for (auto& mat : oldModel->materials) {
                            if (!mat.descriptorSets.empty()) {
                                // Free the sets back to your allocator so the pool doesn't dry up!
                                vkFreeDescriptorSets(vkContext->getDevice(), descriptorAllocator.getPool(),
                                    static_cast<uint32_t>(mat.descriptorSets.size()), mat.descriptorSets.data());
                                mat.descriptorSets.clear();
                            }
                        }
                        });
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

        transformSystem.update(registry);
        drawFrame(registry, view, proj, editorProj);

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
    for (auto fb : lightingFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), fb, nullptr);
    }
    for (auto fb : uiFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), fb, nullptr);
    }
    delete vkUIRenderPass;

    for (auto descSet : sceneDescriptorSets) {
        ImGui_ImplVulkan_RemoveTexture(descSet);
    }

    for (auto fb : forwardFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), fb, nullptr);
    }

    delete vkFramebuffer;

    // Destroy the arrays of Depth AND Color resources!
    // Delete the single sampler
    vkDestroySampler(vkContext->getDevice(), gBufferSampler, nullptr);

    for (size_t i = 0; i < gAlbedoImages.size(); i++) {
        // Position
        vkDestroyImageView(vkContext->getDevice(), gPositionImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), gPositionImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), gPositionImageMemories[i], nullptr);

        // Normal
        vkDestroyImageView(vkContext->getDevice(), gNormalImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), gNormalImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), gNormalImageMemories[i], nullptr);

        // Albedo
        vkDestroyImageView(vkContext->getDevice(), gAlbedoImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), gAlbedoImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), gAlbedoImageMemories[i], nullptr);

        // Depth
        vkDestroyImageView(vkContext->getDevice(), depthImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), depthImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), depthImageMemories[i], nullptr);

        // Lit
        vkDestroyImageView(vkContext->getDevice(), litSceneImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), litSceneImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), litSceneImageMemories[i], nullptr);

        // The Photograph
        vkDestroyImageView(vkContext->getDevice(), opaqueSceneCopyViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), opaqueSceneCopyImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), opaqueSceneCopyMemories[i], nullptr);

        // The Secret Depth Buffer
        vkDestroyImageView(vkContext->getDevice(), glassDepthViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), glassDepthImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), glassDepthMemories[i], nullptr);
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

    createLightingFramebuffers();
    createLightingDescriptorSets();

    // Rebuild Forward Framebuffers with the new Lit Scene and Depth images!
    forwardFramebuffers.resize(vkSwapchain->getImageCount());
    for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
        std::array<VkImageView, 2> attachments = {
            litSceneImageViews[i],
            depthImageViews[i]
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = vkForwardRenderPass->getRenderPass();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = vkSwapchain->getExtent().width;
        framebufferInfo.height = vkSwapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(vkContext->getDevice(), &framebufferInfo, nullptr, &forwardFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to recreate forward framebuffers!");
        }
    }

    // 5. Register the new textures with ImGui
    sceneDescriptorSets.resize(vkSwapchain->getImageCount());
    for (size_t i = 0; i < sceneDescriptorSets.size(); i++) {
        sceneDescriptorSets[i] = ImGui_ImplVulkan_AddTexture(
            gBufferSampler,
            litSceneImageViews[i],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
    }

    // 6. Rebuild Framebuffers
    vkFramebuffer = new VkFramebufferWrapper(
        vkContext,
        vkRenderPass,
        gPositionImageViews,
        gNormalImageViews,
        gAlbedoImageViews,
        depthImageViews,
        vkSwapchain->getExtent()
    );

    // ---> NEW: Rebuild the UI Render Pass and Framebuffers <---
    vkUIRenderPass = new VkUIRenderPass(vkContext, vkSwapchain->getImageFormat());
    createUIFramebuffers();
    // ---------------------------------------------------------
}

void Application::createOffscreenRenderTarget() {
    uint32_t imageCount = vkSwapchain->getImageCount();
    VkExtent2D extent = vkSwapchain->getExtent();

    // Resize all vectors
    gPositionImages.resize(imageCount); gPositionImageMemories.resize(imageCount); gPositionImageViews.resize(imageCount);
    gNormalImages.resize(imageCount);   gNormalImageMemories.resize(imageCount);   gNormalImageViews.resize(imageCount);
    gAlbedoImages.resize(imageCount);   gAlbedoImageMemories.resize(imageCount);   gAlbedoImageViews.resize(imageCount);

    litSceneImages.resize(imageCount);
    litSceneImageMemories.resize(imageCount);
    litSceneImageViews.resize(imageCount);

    VkFormat floatFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // High precision for Pos/Norm
    VkFormat albedoFormat = VK_FORMAT_R8G8B8A8_UNORM;     // Standard color for Albedo

    for (size_t i = 0; i < imageCount; i++) {
        // 1. POSITION
        vkContext->createImage(extent.width, extent.height, floatFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            gPositionImages[i], gPositionImageMemories[i]);

        VkImageViewCreateInfo posViewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        posViewInfo.image = gPositionImages[i]; posViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; posViewInfo.format = floatFormat;
        posViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; posViewInfo.subresourceRange.levelCount = 1; posViewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(vkContext->getDevice(), &posViewInfo, nullptr, &gPositionImageViews[i]);

        // 2. NORMAL
        vkContext->createImage(extent.width, extent.height, floatFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            gNormalImages[i], gNormalImageMemories[i]);

        VkImageViewCreateInfo normViewInfo = posViewInfo;
        normViewInfo.image = gNormalImages[i]; normViewInfo.format = floatFormat;
        vkCreateImageView(vkContext->getDevice(), &normViewInfo, nullptr, &gNormalImageViews[i]);

        // 3. ALBEDO
        vkContext->createImage(extent.width, extent.height, albedoFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            gAlbedoImages[i], gAlbedoImageMemories[i]);

        VkImageViewCreateInfo albedoViewInfo = posViewInfo;
        albedoViewInfo.image = gAlbedoImages[i]; albedoViewInfo.format = albedoFormat;
        vkCreateImageView(vkContext->getDevice(), &albedoViewInfo, nullptr, &gAlbedoImageViews[i]);

        // 4. THE FINAL LIT SCENE
        vkContext->createImage(extent.width, extent.height, vkSwapchain->getImageFormat(), 
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT 
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            litSceneImages[i], litSceneImageMemories[i]);

        VkImageViewCreateInfo litViewInfo = posViewInfo;
        litViewInfo.image = litSceneImages[i];
        litViewInfo.format = vkSwapchain->getImageFormat();
        vkCreateImageView(vkContext->getDevice(), &litViewInfo, nullptr, &litSceneImageViews[i]);
    }

    // Resize the new arrays
    opaqueSceneCopyImages.resize(imageCount); opaqueSceneCopyMemories.resize(imageCount); opaqueSceneCopyViews.resize(imageCount);
    glassDepthImages.resize(imageCount); glassDepthMemories.resize(imageCount); glassDepthViews.resize(imageCount);

    for (size_t i = 0; i < imageCount; i++) {
        // --- 1. THE PHOTOGRAPH (Opaque Scene Copy) ---
        // Same format as litSceneImages!
        vkContext->createImage(extent.width, extent.height, vkSwapchain->getImageFormat(), VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            opaqueSceneCopyImages[i], opaqueSceneCopyMemories[i]);

        VkImageViewCreateInfo copyViewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        copyViewInfo.image = opaqueSceneCopyImages[i];
        copyViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        copyViewInfo.format = vkSwapchain->getImageFormat();
        copyViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyViewInfo.subresourceRange.levelCount = 1;
        copyViewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(vkContext->getDevice(), &copyViewInfo, nullptr, &opaqueSceneCopyViews[i]);

        transitionImageLayout(opaqueSceneCopyImages[i], vkSwapchain->getImageFormat(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // --- 2. THE SECRET DEPTH BUFFER (Glass Thickness) ---
        // Must use your engine's standardized depth format!
        vkContext->createImage(extent.width, extent.height, findDepthFormat(vkContext->getPhysicalDevice()), VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            glassDepthImages[i], glassDepthMemories[i]);

        VkImageViewCreateInfo glassDepthViewInfo = copyViewInfo;
        glassDepthViewInfo.image = glassDepthImages[i];
        glassDepthViewInfo.format = findDepthFormat(vkContext->getPhysicalDevice());
        glassDepthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCreateImageView(vkContext->getDevice(), &glassDepthViewInfo, nullptr, &glassDepthViews[i]);
    }

    // Create ONE Sampler for the G-Buffer
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr, &gBufferSampler);
}

void Application::createLightingDescriptorSets() {
    uint32_t imageCount = vkSwapchain->getImageCount();
    
    // ONLY allocate if the vector is empty
    if (lightingDescriptorSets.empty()) {
        lightingDescriptorSets.resize(imageCount);
        for (size_t i = 0; i < imageCount; i++) {
            lightingDescriptorSets[i] = descriptorAllocator.allocate(vkLightingPipeline->getDescriptorSetLayout());
        }
    }
    for (size_t i = 0; i < imageCount; i++) {
        // Allocate the set using the layout from your new pipeline
        lightingDescriptorSets[i] = descriptorAllocator.allocate(vkLightingPipeline->getDescriptorSetLayout());

        VkDescriptorImageInfo posInfo{ gBufferSampler, gPositionImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo normInfo{ gBufferSampler, gNormalImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo albedoInfo{ gBufferSampler, gAlbedoImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo hdriInfo{ hdriMap.sampler, hdriMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo copyInfo{ gBufferSampler, opaqueSceneCopyViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo glassDepthInfo{ gBufferSampler, glassDepthViews[i], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };

        std::array<VkWriteDescriptorSet, 6> descriptorWrites{};

        // Binding 0: Position
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = lightingDescriptorSets[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pImageInfo = &posInfo;

        // Binding 1: Normal
        descriptorWrites[1] = descriptorWrites[0]; // Copy base struct
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].pImageInfo = &normInfo;

        // Binding 2: Albedo
        descriptorWrites[2] = descriptorWrites[0]; // Copy base struct
        descriptorWrites[2].dstBinding = 2;
        descriptorWrites[2].pImageInfo = &albedoInfo;

        // Binding 3: HDRI Skybox
        descriptorWrites[3] = descriptorWrites[0];
        descriptorWrites[3].dstBinding = 3;
        descriptorWrites[3].pImageInfo = &hdriInfo;

        // Binding 4: Opaque Scene Copy
        descriptorWrites[4] = descriptorWrites[0];
        descriptorWrites[4].dstBinding = 4;
        descriptorWrites[4].pImageInfo = &copyInfo;

        // Binding 5: Glass Depth
        descriptorWrites[5] = descriptorWrites[0];
        descriptorWrites[5].dstBinding = 5;
        descriptorWrites[5].pImageInfo = &glassDepthInfo;

        vkUpdateDescriptorSets(vkContext->getDevice(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
}

void Application::createLightingRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = vkSwapchain->getImageFormat();
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // Clear old frames
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(vkContext->getDevice(), &renderPassInfo, nullptr, &lightingRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create lighting render pass!");
    }
}

void Application::createLightingFramebuffers() {
    lightingFramebuffers.resize(vkSwapchain->getImageCount());
    for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
        VkImageView attachments[] = { litSceneImageViews[i] };

        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = lightingRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = vkSwapchain->getExtent().width;
        framebufferInfo.height = vkSwapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(vkContext->getDevice(), &framebufferInfo, nullptr, &lightingFramebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create lighting framebuffer!");
        }
    }
}

void Application::cleanup() {
    vkDeviceWaitIdle(vkContext->getDevice());
    editor.cleanup(vkContext->getDevice());

    delete assetManager;

    // Delete the single sampler
    vkDestroySampler(vkContext->getDevice(), gBufferSampler, nullptr);

    if (hdriMap.image != VK_NULL_HANDLE) {
        vkDestroySampler(vkContext->getDevice(), hdriMap.sampler, nullptr);
        vkDestroyImageView(vkContext->getDevice(), hdriMap.view, nullptr);
        vkDestroyImage(vkContext->getDevice(), hdriMap.image, nullptr);
        vkFreeMemory(vkContext->getDevice(), hdriMap.memory, nullptr);
    }

    for (size_t i = 0; i < gAlbedoImages.size(); i++) {
        // Position
        vkDestroyImageView(vkContext->getDevice(), gPositionImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), gPositionImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), gPositionImageMemories[i], nullptr);

        // Normal
        vkDestroyImageView(vkContext->getDevice(), gNormalImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), gNormalImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), gNormalImageMemories[i], nullptr);

        // Albedo
        vkDestroyImageView(vkContext->getDevice(), gAlbedoImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), gAlbedoImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), gAlbedoImageMemories[i], nullptr);

        // Depth
        vkDestroyImageView(vkContext->getDevice(), depthImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), depthImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), depthImageMemories[i], nullptr);

        // Lit
        vkDestroyImageView(vkContext->getDevice(), litSceneImageViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), litSceneImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), litSceneImageMemories[i], nullptr);

        // The Photograph
        vkDestroyImageView(vkContext->getDevice(), opaqueSceneCopyViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), opaqueSceneCopyImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), opaqueSceneCopyMemories[i], nullptr);

        // The Secret Depth Buffer
        vkDestroyImageView(vkContext->getDevice(), glassDepthViews[i], nullptr);
        vkDestroyImage(vkContext->getDevice(), glassDepthImages[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), glassDepthMemories[i], nullptr);
    }

    for (size_t i = 0; i < uniformBuffers.size(); i++) {
        vkDestroyBuffer(vkContext->getDevice(), uniformBuffers[i], nullptr);
        vkFreeMemory(vkContext->getDevice(), uniformBuffersMemory[i], nullptr);
    }

    descriptorAllocator.cleanup();

    for (auto fb : lightingFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), fb, nullptr);
    }

    delete vkLightingPipeline;

    if (lightingRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vkContext->getDevice(), lightingRenderPass, nullptr);
    }

    for (auto framebuffer : uiFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), framebuffer, nullptr);
    }

    for (auto fb : forwardFramebuffers) {
        vkDestroyFramebuffer(vkContext->getDevice(), fb, nullptr);
    }
    delete vkForwardPipeline;
    delete vkForwardRenderPass;

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