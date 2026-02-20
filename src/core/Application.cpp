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

    app->cameraSpeed += (float)yoffset * 0.5f;

    if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
    if (app->cameraSpeed > 10.0f) app->cameraSpeed = 10.0f;

    std::cout << "Camera Speed: " << app->cameraSpeed << std::endl;
}

void Application::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    auto* app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));

    if (ImGui::GetIO().WantCaptureMouse) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);
        app->selectEntityAtMouse(xpos, ypos);
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            app->isRightMouseButtonDown = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            app->firstMouse = true;
        }
        else if (action == GLFW_RELEASE) {
            app->isRightMouseButtonDown = false;
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

void Application::initWindow() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(WIDTH, HEIGHT, "Iridium Engine", nullptr, nullptr);
    glfwSetWindowUserPointer(window, this);

    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
}

void Application::initVulkan() {
    vkContext = new VkContext(enableValidation, window);
    vkSwapchain = new VkSwapchain(vkContext, window);
    vkRenderPass = new VkRenderPassWrapper(vkContext, vkSwapchain);
    vkPipeline = new VkGraphicsPipeline(vkContext, vkSwapchain, vkRenderPass);
    vkSyncObjects = new VkSyncObjects(vkContext, vkSwapchain->getImageCount());

    vkCommandManager = new VkCommandManager(vkContext, nullptr, vkPipeline, vkSwapchain->getImageCount());

    createDepthResources();
    transitionImageLayout(depthImage, VK_FORMAT_D32_SFLOAT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    vkFramebuffer = new VkFramebufferWrapper(vkContext, vkSwapchain, vkRenderPass, depthImageView);

    assetManager = new AssetManager(vkContext, vkCommandManager);

    std::string modelPath = std::string(PROJECT_ROOT_DIR) + "assets/models/alfa_romeo/scene.gltf";
    mainModel = assetManager->getModel(modelPath);

    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();

    editor.init(
        vkContext->getInstance(),
        vkContext->getDevice(),
        vkContext->getPhysicalDevice(),
        vkContext->getGraphicsQueue(),
        vkRenderPass->getRenderPass(),
        window,
        vkCommandManager->getCommandPool()
    );
}

void Application::drawFrame(Registry& registry, const glm::mat4& view, const glm::mat4& proj) {
    VkFence currentFence = vkSyncObjects->getInFlightFence(currentFrame);
    VkFence inFlightFence = vkSyncObjects->getInFlightFence(currentFrame);

    vkWaitForFences(vkContext->getDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX);
    frameDeletionQueues[currentFrame].flush();

    updateUniformBuffer(currentFrame, view, proj);

    uint32_t imageIndex;
    vkAcquireNextImageKHR(vkContext->getDevice(), vkSwapchain->getSwapchain(), UINT64_MAX,
        vkSyncObjects->getImageAvailableSemaphore(currentFrame), VK_NULL_HANDLE, &imageIndex);

    if (imageIndex >= imagesInFlight.size()) {
        imagesInFlight.resize(vkSwapchain->getImageCount(), VK_NULL_HANDLE);
    }

    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(vkContext->getDevice(), 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight[imageIndex] = currentFence;

    vkResetFences(vkContext->getDevice(), 1, &currentFence);

    updateUniformBuffer(imageIndex, view, proj);

    vkCommandManager->recordCommands(
        imageIndex,
        vkRenderPass,
        vkFramebuffer,
        vkPipeline,
        vkSwapchain->getExtent(),
        registry,
        globalDescriptorSets,
        &editor
    );

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

void Application::createUniformBuffers() {
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

        vkMapMemory(vkContext->getDevice(), uniformBuffersMemory[i], 0, bufferSize, 0, &uniformBuffersMapped[i]);
    }
}

void Application::updateUniformBuffer(uint32_t currentImage, const glm::mat4& view, const glm::mat4& proj) {
    UniformBufferObject ubo{};
    ubo.model = glm::mat4(1.0f);
    ubo.view = view;
    ubo.proj = proj;
    memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
}

void Application::createDescriptorPool() {
    uint32_t imageCount = static_cast<uint32_t>(vkSwapchain->getImageCount());
    uint32_t materialCount = static_cast<uint32_t>(mainModel->materials.size());
    uint32_t maxSets = (1 + materialCount) * imageCount;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = imageCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = materialCount * imageCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = maxSets;

    if (vkCreateDescriptorPool(vkContext->getDevice(), &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor pool!");
    }
}

void Application::createDescriptorSets() {
    uint32_t imageCount = static_cast<uint32_t>(vkSwapchain->getImageCount());
    globalDescriptorSets.resize(imageCount);
    std::vector<VkDescriptorSetLayout> globalLayouts(imageCount, vkPipeline->getGlobalSetLayout());

    VkDescriptorSetAllocateInfo globalAllocInfo{};
    globalAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    globalAllocInfo.descriptorPool = descriptorPool;
    globalAllocInfo.descriptorSetCount = imageCount;
    globalAllocInfo.pSetLayouts = globalLayouts.data();

    if (vkAllocateDescriptorSets(vkContext->getDevice(), &globalAllocInfo, globalDescriptorSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate global descriptor sets!");
    }

    for (size_t i = 0; i < imageCount; i++) {
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

    size_t numMaterials = mainModel->materials.size();
    if (numMaterials == 0) return;

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

        int imgIdx = material.textureIndex;
        if (imgIdx < 0 || imgIdx >= static_cast<int>(mainModel->textures.size())) {
            imgIdx = 0;
        }

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

void Application::createDepthResources() {
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
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

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &depthImageView);
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
    if (ImGui::GetIO().WantTextInput) return;

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
    float aspectRatio = vkSwapchain->getExtent().width / (float)vkSwapchain->getExtent().height;
    glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 1000.0f);

    float x = (2.0f * static_cast<float>(mouseX)) / WIDTH - 1.0f;
    float y = 1.0f - (2.0f * static_cast<float>(mouseY)) / HEIGHT;

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
        float aspectRatio = vkSwapchain->getExtent().width / (float)vkSwapchain->getExtent().height;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 1000.0f);
        glm::mat4 editorProj = proj;

        proj[1][1] *= -1;

        editor.update(registry, assetManager, view, editorProj);
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

void Application::cleanup() {
    vkDeviceWaitIdle(vkContext->getDevice());
    editor.cleanup(vkContext->getDevice());

    delete assetManager;

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
    delete vkContext;

    glfwDestroyWindow(window);
    glfwTerminate();
}