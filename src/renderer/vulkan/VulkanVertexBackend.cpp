#include "VulkanVertexBackend.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"
#include "vendor/imguizmo/ImGuizmo.h"
#include <stdexcept>
#include <array>
#include <iostream>

namespace Iridium {

    // ==============================================================================
    // 1. SYSTEM LIFECYCLE
    // ==============================================================================

    void VulkanVertexBackend::init(GLFWwindow* window) {
        vkContext = new VkContext(true, window);
        vkSwapchain = new VkSwapchain(vkContext, window);
        vkSyncObjects = new VkSyncObjects(vkContext, vkSwapchain->getImageCount());

        descriptorAllocator.init(vkContext->getDevice());

        // 2. G-Buffer Pass
        gBufferPass = new VkRenderPassWrapper(vkContext, vkSwapchain);
        gBufferPipeline = new VkGraphicsPipeline(vkContext, vkSwapchain, gBufferPass);

        // 3. Glass Depth Pass
        glassDepthPass = new GlassDepthRenderPass();
        glassDepthPass->init(vkContext->getDevice(), VK_FORMAT_D32_SFLOAT);

        glassDepthPipeline = new GlassDepthPipeline();
        glassDepthPipeline->init(vkContext->getDevice(), glassDepthPass->getRenderPass(),
            gBufferPipeline->getPipelineLayout(),
            std::string(PROJECT_ROOT_DIR) + "assets/shaders/glass_depth_vert.spv");

        pipelineCache.init(vkContext->getDevice(),
            gBufferPass->getRenderPass(),
            glassDepthPass->getRenderPass(), // Or whatever your forward pass is!
            gBufferPipeline->getPipelineLayout());

        // 4. Lighting Pass (Fully Activated)
        createLightingRenderPass();
        lightingPipeline = new VkLightingPipeline(vkContext, lightingRenderPass);

        // 5. Forward Pass (Fully Activated)
        forwardPass = new VkForwardRenderPass(vkContext, vkSwapchain->getImageFormat(), VK_FORMAT_D32_SFLOAT);
        forwardPipeline = new VkForwardPipeline(vkContext, forwardPass, lightingPipeline->getDescriptorSetLayout());

        // 6. UI Pass
        uiPass = new VkUIRenderPass(vkContext, vkSwapchain->getImageFormat());

        // 7. Render Targets & Command Manager
        vkCommandManager = new VkCommandManager(vkContext, gBufferFramebuffers, gBufferPipeline, VkSyncObjects::MAX_FRAMES_IN_FLIGHT);
        createOffscreenRenderTargets();

        // 8. Global Camera Buffers
        createUniformBuffers();

        // --------------------------------

        // 2. Global Descriptor Sets (Camera Data)
        globalDescriptorSets.resize(VkSyncObjects::MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < VkSyncObjects::MAX_FRAMES_IN_FLIGHT; i++) {
            globalDescriptorSets[i] = descriptorAllocator.allocate(gBufferPipeline->getGlobalSetLayout());

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkWriteDescriptorSet descriptorWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            descriptorWrite.dstSet = globalDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(vkContext->getDevice(), 1, &descriptorWrite, 0, nullptr);
        }

        // 3. Lighting Descriptor Sets (G-Buffer Textures)
        uint32_t imgCount = vkSwapchain->getImageCount();
        lightingDescriptorSets.resize(imgCount);
        for (size_t i = 0; i < imgCount; i++) {
            lightingDescriptorSets[i] = descriptorAllocator.allocate(lightingPipeline->getDescriptorSetLayout());

            VkDescriptorImageInfo normalInfo{ gBufferSampler, gNormalImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo albedoInfo{ gBufferSampler, gAlbedoImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo depthInfo{ gBufferSampler, gDepthImageViews[i], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo opaqueCopyInfo{ gBufferSampler, opaqueSceneCopyViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo glassDepthInfo{ gBufferSampler, glassDepthViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            std::array<VkWriteDescriptorSet, 5> descriptorWrites{};
            // BINDING 0: Depth
            descriptorWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };

            // BINDING 1: Normal
            descriptorWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo, nullptr, nullptr };

            // BINDING 2: Albedo
            descriptorWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &albedoInfo, nullptr, nullptr };

            // (Binding 3 is the HDRI, which is updated dynamically elsewhere!)

            // BINDING 4 & 5: Glass Data
            descriptorWrites[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &opaqueCopyInfo, nullptr, nullptr };
            descriptorWrites[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &glassDepthInfo, nullptr, nullptr };

            // Update 5 descriptors instead of 3
            vkUpdateDescriptorSets(vkContext->getDevice(), 5, descriptorWrites.data(), 0, nullptr);
        }

        // 4. ImGui Initialization & UI Textures
        // Create a small pool specifically for ImGui's internal fonts and textures
        VkDescriptorPoolSize pool_sizes[] = { {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10} };
        VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 10;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = pool_sizes;
        vkCreateDescriptorPool(vkContext->getDevice(), &pool_info, nullptr, &imguiPool);

        // Init ImGui contexts
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui_ImplGlfw_InitForVulkan(window, true);

        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = vkContext->getInstance();
        init_info.PhysicalDevice = vkContext->getPhysicalDevice();
        init_info.Device = vkContext->getDevice();
        init_info.QueueFamily = vkContext->getGraphicsQueueFamily();
        init_info.Queue = vkContext->getGraphicsQueue();
        init_info.PipelineCache = VK_NULL_HANDLE;
        init_info.DescriptorPool = imguiPool;
        init_info.MinImageCount = imgCount;
        init_info.ImageCount = imgCount;
        init_info.PipelineInfoMain.RenderPass = uiPass->getRenderPass();
        ImGui_ImplVulkan_Init(&init_info);

        // Create the initial ImGui textures for the viewport!
        uint32_t currentImgCount = vkSwapchain->getImageCount();
        uiSceneTextures.resize(currentImgCount);
        uiDepthTextures.resize(currentImgCount);
        for (size_t i = 0; i < currentImgCount; i++) {
            uiSceneTextures[i] = ImGui_ImplVulkan_AddTexture(gBufferSampler, litSceneImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(gBufferSampler, glassDepthViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    
    void VulkanVertexBackend::setEnvironmentMap(TextureHandle hdriHandle) {
        auto* payload = textureVault.get(hdriHandle);
        if (!payload) return;

        for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
            VkDescriptorImageInfo hdriInfo{};
            hdriInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            hdriInfo.imageView = payload->view;
            hdriInfo.sampler = payload->sampler;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = lightingDescriptorSets[i];
            write.dstBinding = 3;
            write.dstArrayElement = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &hdriInfo;

            vkUpdateDescriptorSets(vkContext->getDevice(), 1, &write, 0, nullptr);
        }
    }

    void VulkanVertexBackend::cleanup() {
        vkDeviceWaitIdle(vkContext->getDevice());

        // 1. Destroy all Render Targets and Framebuffers
        VkDevice dev = vkContext->getDevice();
        uint32_t imageCount = vkSwapchain->getImageCount();
        destroyOffscreenRenderTargets();

        // 2. Shut down ImGui securely
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(dev, imguiPool, nullptr);
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        // Flush all pending deletions before shutting down
        for (auto& queue : frameDeletionQueues) {
            queue.flush();
        }

        // Clean Vaults (For items that were never explicitly freed by the game)
        geometryVault.forEach([this](VulkanGeometryPayload& payload) {
            vkDestroyBuffer(vkContext->getDevice(), payload.vertexBuffer, nullptr);
            vkFreeMemory(vkContext->getDevice(), payload.vertexBufferMemory, nullptr);
            vkDestroyBuffer(vkContext->getDevice(), payload.indexBuffer, nullptr);
            vkFreeMemory(vkContext->getDevice(), payload.indexBufferMemory, nullptr);
            });

        textureVault.forEach([this](VulkanTexturePayload& payload) {
            vkDestroySampler(vkContext->getDevice(), payload.sampler, nullptr);
            vkDestroyImageView(vkContext->getDevice(), payload.view, nullptr);
            vkDestroyImage(vkContext->getDevice(), payload.image, nullptr);
            vkFreeMemory(vkContext->getDevice(), payload.memory, nullptr);
            });

        for (size_t i = 0; i < uniformBuffers.size(); i++) {
            vkDestroyBuffer(vkContext->getDevice(), uniformBuffers[i], nullptr);
            vkFreeMemory(vkContext->getDevice(), uniformBuffersMemory[i], nullptr);
        }

        descriptorAllocator.cleanup();

        delete glassDepthPipeline;
        glassDepthPass->cleanup();
        delete glassDepthPass;

        delete forwardPipeline;
        delete forwardPass;

        delete lightingPipeline;
        vkDestroyRenderPass(vkContext->getDevice(), lightingRenderPass, nullptr);

        delete uiPass;
        delete gBufferPipeline;
        delete gBufferPass;

        delete vkCommandManager;
        delete vkSyncObjects;
        delete vkSwapchain;
        delete vkContext;
    }

    void VulkanVertexBackend::destroyOffscreenRenderTargets() {
        VkDevice dev = vkContext->getDevice();
        uint32_t imageCount = vkSwapchain->getImageCount();

        for (uint32_t i = 0; i < imageCount; i++) {
            // Free the ImGui textures so they can be rebuilt
            if (i < uiSceneTextures.size() && uiSceneTextures[i]) {
                ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)uiSceneTextures[i]);
                ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)uiDepthTextures[i]);
            }

            // Framebuffers
            vkDestroyFramebuffer(dev, lightingFramebuffers[i], nullptr);
            vkDestroyFramebuffer(dev, forwardFramebuffers[i], nullptr);
            vkDestroyFramebuffer(dev, uiFramebuffers[i], nullptr);
            vkDestroyFramebuffer(dev, glassDepthFramebuffers[i], nullptr);

            // G-Buffer: Normal
            vkDestroyImageView(dev, gNormalImageViews[i], nullptr);
            vkDestroyImage(dev, gNormalImages[i], nullptr);
            vkFreeMemory(dev, gNormalImageMemories[i], nullptr);

            // G-Buffer: Albedo
            vkDestroyImageView(dev, gAlbedoImageViews[i], nullptr);
            vkDestroyImage(dev, gAlbedoImages[i], nullptr);
            vkFreeMemory(dev, gAlbedoImageMemories[i], nullptr);

            // G-Buffer: Depth
            vkDestroyImageView(dev, gDepthImageViews[i], nullptr);
            vkDestroyImage(dev, gDepthImages[i], nullptr);
            vkFreeMemory(dev, gDepthImageMemories[i], nullptr);

            // Lit Scene
            vkDestroyImageView(dev, litSceneImageViews[i], nullptr);
            vkDestroyImage(dev, litSceneImages[i], nullptr);
            vkFreeMemory(dev, litSceneImageMemories[i], nullptr);

            // Glass Copies
            vkDestroyImageView(dev, opaqueSceneCopyViews[i], nullptr);
            vkDestroyImage(dev, opaqueSceneCopyImages[i], nullptr);
            vkFreeMemory(dev, opaqueSceneCopyMemories[i], nullptr);

            vkDestroyImageView(dev, glassDepthViews[i], nullptr);
            vkDestroyImage(dev, glassDepthImages[i], nullptr);
            vkFreeMemory(dev, glassDepthMemories[i], nullptr);
        }

        vkDestroySampler(dev, gBufferSampler, nullptr);
        delete gBufferFramebuffers;

        // Clear the arrays to reset their size to 0
        uiSceneTextures.clear();
        uiDepthTextures.clear();
    }

    void VulkanVertexBackend::recreateSwapchain(GLFWwindow* window) {
        // 1. Handle Minimization (Pause the engine until it's un-minimized)
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        // 2. Wait for the GPU to finish its current frame
        vkDeviceWaitIdle(vkContext->getDevice());

        // 3. Destroy the old size
        destroyOffscreenRenderTargets();
        delete vkSwapchain;

        // 4. Build the new size
        vkSwapchain = new VkSwapchain(vkContext, window);
        createOffscreenRenderTargets();

        // 5. Re-register the new ImGui Textures
        uiSceneTextures.resize(vkSwapchain->getImageCount());
        uiDepthTextures.resize(vkSwapchain->getImageCount());
        for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
            uiSceneTextures[i] = ImGui_ImplVulkan_AddTexture(gBufferSampler, litSceneImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(gBufferSampler, glassDepthViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // 6. UPDATE LIGHTING DESCRIPTORS WITH NEW IMAGE VIEWS
        for (size_t i = 0; i < vkSwapchain->getImageCount(); i++) {
            VkDescriptorImageInfo normalInfo{ gBufferSampler, gNormalImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo albedoInfo{ gBufferSampler, gAlbedoImageViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo depthInfo{ gBufferSampler, gDepthImageViews[i], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo opaqueCopyInfo{ gBufferSampler, opaqueSceneCopyViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo glassDepthInfo{ gBufferSampler, glassDepthViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            std::array<VkWriteDescriptorSet, 5> descriptorWrites{};
            // BINDING 0: Depth
            descriptorWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };

            // BINDING 1: Normal
            descriptorWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo, nullptr, nullptr };

            // BINDING 2: Albedo
            descriptorWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &albedoInfo, nullptr, nullptr };

            // (Binding 3 is the HDRI, which is updated dynamically elsewhere!)

            // BINDING 4 & 5: Glass Data
            descriptorWrites[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &opaqueCopyInfo, nullptr, nullptr };
            descriptorWrites[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, lightingDescriptorSets[i], 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &glassDepthInfo, nullptr, nullptr };

            vkUpdateDescriptorSets(vkContext->getDevice(), 5, descriptorWrites.data(), 0, nullptr);
        }
    }

    // ==============================================================================
    // 2. RESOURCE MANAGEMENT (Thread-Safe & Anti-Fragmentation)
    // ==============================================================================

    GeometryHandle VulkanVertexBackend::allocateGeometry(const void* vertexData, size_t vertexSize,
        const void* indexData, size_t indexSize) {
        VulkanGeometryPayload payload{};
        payload.indexCount = static_cast<uint32_t>(indexSize / sizeof(uint32_t));

        vkContext->createGPUBuffer(vertexSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexData,
            payload.vertexBuffer, payload.vertexBufferMemory, vkCommandManager);

        vkContext->createGPUBuffer(indexSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexData,
            payload.indexBuffer, payload.indexBufferMemory, vkCommandManager);

        return geometryVault.allocate(payload);
    }

    void VulkanVertexBackend::freeGeometry(GeometryHandle handle) {
        auto* payload = geometryVault.get(handle);
        if (payload) {
            // Capture the Vulkan pointers by value so the lambda remembers them
            VkBuffer vBuf = payload->vertexBuffer;
            VkDeviceMemory vMem = payload->vertexBufferMemory;
            VkBuffer iBuf = payload->indexBuffer;
            VkDeviceMemory iMem = payload->indexBufferMemory;
            VkDevice device = vkContext->getDevice();

            // Defer the destruction! The GPU won't crash, and the CPU won't stall.
            frameDeletionQueues[currentFrame].push_function([=]() {
                vkDestroyBuffer(device, vBuf, nullptr);
                vkFreeMemory(device, vMem, nullptr);
                vkDestroyBuffer(device, iBuf, nullptr);
                vkFreeMemory(device, iMem, nullptr);
                });

            geometryVault.free(handle);
        }
    }

    TextureHandle VulkanVertexBackend::allocateTexture(uint32_t width, uint32_t height, int channels,
        const void* pixelData, bool isHDRI) {
        VulkanTexturePayload payload{};
        payload.isHDRI = isHDRI;

        VkFormat format = isHDRI ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        size_t pixelSize = isHDRI ? sizeof(float) : sizeof(unsigned char);
        VkDeviceSize imageSize = width * height * 4 * pixelSize;

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        vkContext->createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingBufferMemory);

        void* data;
        vkMapMemory(vkContext->getDevice(), stagingBufferMemory, 0, imageSize, 0, &data);
        memcpy(data, pixelData, (size_t)imageSize);
        vkUnmapMemory(vkContext->getDevice(), stagingBufferMemory);

        vkContext->createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, payload.image, payload.memory);

        vkCommandManager->transitionImageLayout(payload.image, format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCommandManager->copyBufferToImage(stagingBuffer, payload.image, width, height);
        vkCommandManager->transitionImageLayout(payload.image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(vkContext->getDevice(), stagingBuffer, nullptr);
        vkFreeMemory(vkContext->getDevice(), stagingBufferMemory, nullptr);

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = payload.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &payload.view);

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = isHDRI ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr, &payload.sampler);

        return textureVault.allocate(payload);
    }



    void VulkanVertexBackend::freeTexture(TextureHandle handle) {
        auto* payload = textureVault.get(handle);
        if (payload) {
            VkImage img = payload->image;
            VkDeviceMemory mem = payload->memory;
            VkImageView view = payload->view;
            VkSampler sampler = payload->sampler;
            VkDevice device = vkContext->getDevice();

            frameDeletionQueues[currentFrame].push_function([=]() {
                vkDestroySampler(device, sampler, nullptr);
                vkDestroyImageView(device, view, nullptr);
                vkDestroyImage(device, img, nullptr);
                vkFreeMemory(device, mem, nullptr);
                });

            textureVault.free(handle);
        }
    }

    MaterialHandle VulkanVertexBackend::allocateMaterial(const MaterialAsset& asset) {
        // 1. Get or Create the Vulkan Pipeline from the Cache
        VkPipeline generatedPipeline = pipelineCache.getOrCreatePipeline(asset.pipelineState);

        // 2. Populate YOUR actual internal struct
        VulkanMaterialPayload mat;
        mat.pipeline = generatedPipeline;
        mat.blendMode = asset.pipelineState.blendMode;

        mat.baseColor = asset.baseColor;
        mat.metallicFactor = asset.metallic;
        mat.roughnessFactor = asset.roughness;
        mat.emissiveFactor = asset.emissive;

        // 3. Allocate the descriptors. 
        // NOTE: Check your VulkanTexturePayload struct! If the VkImageView is 
        // called something other than "imageView" (like "view"), change it below!
        VkDescriptorSet matSet = descriptorAllocator.allocateMaterialSet(
            textureVault.get(asset.albedoMap)->imageView,
            textureVault.get(asset.normalMap)->imageView,
            textureVault.get(asset.pbrMap)->imageView
        );

        mat.descriptorSets.assign(VkSyncObjects::MAX_FRAMES_IN_FLIGHT, matSet);

        // 4. Return the handle using YOUR vault's method
        return materialVault.add(mat);
    }

    void VulkanVertexBackend::freeMaterial(MaterialHandle handle) {
        // We only free the slot in the vault. 
        // The VkDescriptorSets remain in memory, attached to this slot, ready to be recycled!
        materialVault.free(handle);
    }

    // --- PRIVATE HELPERS ---

    void VulkanVertexBackend::createLightingRenderPass() {
        // This pass writes the evaluated lighting directly to the Swapchain image
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = vkSwapchain->getImageFormat();
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Outputting as Color Attachment Optimal so the Forward Pass can draw glass on top of it next
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

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(vkContext->getDevice(), &renderPassInfo, nullptr, &lightingRenderPass) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create lighting render pass!");
        }
    }

    void VulkanVertexBackend::createOffscreenRenderTargets() {
        uint32_t imageCount = vkSwapchain->getImageCount();
        VkExtent2D extent = vkSwapchain->getExtent();

        // Resize all vectors
        gNormalImages.resize(imageCount);   gNormalImageMemories.resize(imageCount);   gNormalImageViews.resize(imageCount);
        gAlbedoImages.resize(imageCount);   gAlbedoImageMemories.resize(imageCount);   gAlbedoImageViews.resize(imageCount);
        litSceneImages.resize(imageCount);  litSceneImageMemories.resize(imageCount);  litSceneImageViews.resize(imageCount);
        gDepthImages.resize(imageCount);    gDepthImageMemories.resize(imageCount);    gDepthImageViews.resize(imageCount);
        
        for (size_t i = 0; i < imageCount; i++) {
            // 1. NORMAL
            vkContext->createImage(extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                gNormalImages[i], gNormalImageMemories[i]);

            VkImageViewCreateInfo normViewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            normViewInfo.image = gNormalImages[i];
            normViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            normViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            normViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            normViewInfo.subresourceRange.levelCount = 1;
            normViewInfo.subresourceRange.layerCount = 1;
            vkCreateImageView(vkContext->getDevice(), &normViewInfo, nullptr, &gNormalImageViews[i]);

            // 2. ALBEDO
            vkContext->createImage(extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                gAlbedoImages[i], gAlbedoImageMemories[i]);

            VkImageViewCreateInfo albedoViewInfo = normViewInfo;
            albedoViewInfo.image = gAlbedoImages[i];
            vkCreateImageView(vkContext->getDevice(), &albedoViewInfo, nullptr, &gAlbedoImageViews[i]);

            // 2.5 MAIN SCENE DEPTH BUFFER
            vkContext->createImage(extent.width, extent.height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                gDepthImages[i], gDepthImageMemories[i]);

            VkImageViewCreateInfo depthViewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            depthViewInfo.image = gDepthImages[i];
            depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
            depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depthViewInfo.subresourceRange.levelCount = 1;
            depthViewInfo.subresourceRange.layerCount = 1;
            vkCreateImageView(vkContext->getDevice(), &depthViewInfo, nullptr, &gDepthImageViews[i]);

            // 3. THE FINAL LIT SCENE
            vkContext->createImage(extent.width, extent.height, vkSwapchain->getImageFormat(),
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                litSceneImages[i], litSceneImageMemories[i]);

            VkImageViewCreateInfo litViewInfo = normViewInfo;
            litViewInfo.image = litSceneImages[i];
            litViewInfo.format = vkSwapchain->getImageFormat();
            vkCreateImageView(vkContext->getDevice(), &litViewInfo, nullptr, &litSceneImageViews[i]);
        }

        opaqueSceneCopyImages.resize(imageCount); opaqueSceneCopyMemories.resize(imageCount); opaqueSceneCopyViews.resize(imageCount);
        glassDepthImages.resize(imageCount); glassDepthMemories.resize(imageCount); glassDepthViews.resize(imageCount);

        for (size_t i = 0; i < imageCount; i++) {
            // 4. THE PHOTOGRAPH (Opaque Scene Copy)
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

            // Use the backend's command manager to transition
            vkCommandManager->transitionImageLayout(opaqueSceneCopyImages[i], vkSwapchain->getImageFormat(),
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            // 5. THE SECRET DEPTH BUFFER (Glass Thickness)
            // Note: Make sure findDepthFormat is accessible here, or hardcode your standard depth format
            vkContext->createImage(extent.width, extent.height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                glassDepthImages[i], glassDepthMemories[i]);

            vkCommandManager->transitionImageLayout(glassDepthImages[i], VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            VkImageViewCreateInfo glassDepthViewInfo = copyViewInfo;
            glassDepthViewInfo.image = glassDepthImages[i];
            glassDepthViewInfo.format = VK_FORMAT_D32_SFLOAT;
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

        gBufferFramebuffers = new VkFramebufferWrapper(
            vkContext,
            gBufferPass,
            gNormalImageViews,
            gAlbedoImageViews,
            gDepthImageViews,
            vkSwapchain->getExtent()
        );

        glassDepthFramebuffers.resize(imageCount);
        for (size_t i = 0; i < imageCount; i++) {
            VkImageView attachments[] = { glassDepthViews[i] };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = glassDepthPass->getRenderPass();
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(vkContext->getDevice(), &framebufferInfo, nullptr, &glassDepthFramebuffers[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create glass depth framebuffer!");
            }
        }

        lightingFramebuffers.resize(imageCount);
        forwardFramebuffers.resize(imageCount);
        uiFramebuffers.resize(imageCount);

        for (size_t i = 0; i < imageCount; i++) {
            // 1. Lighting Framebuffer (Draws color into the Lit Scene)
            VkImageView lightingAttachments[] = { litSceneImageViews[i] };
            VkFramebufferCreateInfo lightingFbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            lightingFbInfo.renderPass = lightingRenderPass;
            lightingFbInfo.attachmentCount = 1;
            lightingFbInfo.pAttachments = lightingAttachments;
            lightingFbInfo.width = extent.width;
            lightingFbInfo.height = extent.height;
            lightingFbInfo.layers = 1;
            if (vkCreateFramebuffer(vkContext->getDevice(), &lightingFbInfo, nullptr, &lightingFramebuffers[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create lighting framebuffer");

            // 2. Forward Framebuffer (Draws glass into the Lit Scene, reads from Main Depth)
            VkImageView forwardAttachments[] = { litSceneImageViews[i], gDepthImageViews[i] };
            VkFramebufferCreateInfo forwardFbInfo = lightingFbInfo;
            forwardFbInfo.renderPass = forwardPass->getRenderPass();
            forwardFbInfo.attachmentCount = 2;
            forwardFbInfo.pAttachments = forwardAttachments;
            if (vkCreateFramebuffer(vkContext->getDevice(), &forwardFbInfo, nullptr, &forwardFramebuffers[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create forward framebuffer");

            // 3. UI Framebuffer (Draws the editor directly to the OS Swapchain window)
            // Note: Make sure your vkSwapchain->getImageViews() getter exists!
            VkImageView uiAttachments[] = { vkSwapchain->getImageViews()[i] };
            VkFramebufferCreateInfo uiFbInfo = lightingFbInfo;
            uiFbInfo.renderPass = uiPass->getRenderPass();
            uiFbInfo.attachmentCount = 1;
            uiFbInfo.pAttachments = uiAttachments;
            if (vkCreateFramebuffer(vkContext->getDevice(), &uiFbInfo, nullptr, &uiFramebuffers[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create UI framebuffer");
        }
    }

    void VulkanVertexBackend::createUniformBuffers() {
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

    // ==============================================================================
    // 3. THE FRAME PIPELINE (Data-Driven Execution)
    // ==============================================================================

    bool VulkanVertexBackend::beginFrame() {
        VkFence inFlightFence = vkSyncObjects->getInFlightFence(currentFrame);
        vkWaitForFences(vkContext->getDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX);

        // 1. Flush any deleted resources now that we know the GPU is absolutely 
        // finished with this specific frame index.
        frameDeletionQueues[currentFrame].flush();

        // 2. Acquire the next canvas from the OS
        VkResult result = vkAcquireNextImageKHR(vkContext->getDevice(), vkSwapchain->getSwapchain(),
            UINT64_MAX, vkSyncObjects->getImageAvailableSemaphore(currentFrame),
            VK_NULL_HANDLE, &currentImageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            // Note: Application.cpp will check this and call recreateSwapchain()
            return false;
        }
        else if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to acquire swap chain image!");
        }

        vkResetFences(vkContext->getDevice(), 1, &inFlightFence);

        // 3. Begin Command Buffer Recording
        currentCmd = vkCommandManager->getCommandBuffer(currentFrame);
        vkResetCommandBuffer(currentCmd, 0);

        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (vkBeginCommandBuffer(currentCmd, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        return true;
    }

    void VulkanVertexBackend::submitOpaqueQueue(const std::vector<DrawPacket>& opaqueQueue,
        const std::vector<DrawPacket>& selectionQueue, bool isWireframe) {
        VkRenderPassBeginInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpInfo.renderPass = gBufferPass->getRenderPass();
        rpInfo.framebuffer = gBufferFramebuffers->getFramebuffer(currentImageIndex);
        rpInfo.renderArea.extent = vkSwapchain->getExtent();

        std::array<VkClearValue, 3> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Normal
        clearValues[1].color = { {0.1f, 0.1f, 0.1f, 1.0f} }; // Albedo
        clearValues[2].depthStencil = { 1.0f, 0 };           // Depth

        rpInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        rpInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(currentCmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Dynamic Viewport/Scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;            // Start at the bottom
        viewport.width = (float)vkSwapchain->getExtent().width;
        viewport.height = (float)vkSwapchain->getExtent().height;       // Draw upwards!
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        vkCmdSetViewport(currentCmd, 0, 1, &viewport);
        VkRect2D scissor{ {0, 0}, rpInfo.renderArea.extent };
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);

        VkPipelineLayout layout = gBufferPipeline->getPipelineLayout();

        // ==============================================================================
        // PHASE 1: DRAW OPAQUE SCENE (Standard Depth Testing)
        // ==============================================================================

        if (isWireframe) {
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getWireframePipeline());
        }
        else {
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getPipeline());
        }

        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

        MaterialHandle lastBoundMaterial;
        bool firstOpaque = true;

        for (const auto& packet : opaqueQueue) {
            auto* geometry = geometryVault.get(packet.geometry);
            auto* material = materialVault.get(packet.material);

            if (!geometry || !material) continue;

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            MeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.baseColor = material->baseColor;
            push.metallicFactor = material->metallicFactor;
            push.roughnessFactor = material->roughnessFactor;

            // FIX: Removed the isSelected hack. We draw the real material here.
            push.emissiveFactor = material->emissiveFactor;

            vkCmdPushConstants(currentCmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);

            if (firstOpaque || packet.material != lastBoundMaterial) {
                vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &material->descriptorSets[currentFrame], 0, nullptr);
                lastBoundMaterial = packet.material;
                firstOpaque = false;
            }

            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
        }

        // ==============================================================================
        // PHASE 2: DRAW SELECTION MASKS (Depth Testing Disabled = X-Ray)
        // ==============================================================================

        if (!selectionQueue.empty()) {
            // Bind the outline pipeline we set up to ignore depth
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getOutlinePipeline());

            lastBoundMaterial = MaterialHandle{}; // Reset for the new loop
            bool firstSelection = true;

            for (const auto& packet : selectionQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);

                if (!geometry || !material) continue;

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;

                // Mask shader doesn't use these, but we fill them to satisfy the struct size
                push.baseColor = material->baseColor;
                push.metallicFactor = material->metallicFactor;
                push.roughnessFactor = material->roughnessFactor;
                push.emissiveFactor = material->emissiveFactor;

                vkCmdPushConstants(currentCmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);

                if (firstSelection || packet.material != lastBoundMaterial) {
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &material->descriptorSets[currentFrame], 0, nullptr);
                    lastBoundMaterial = packet.material;
                    firstSelection = false;
                }

                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
        }

        vkCmdEndRenderPass(currentCmd);
    }

    void VulkanVertexBackend::updateCamera(const glm::mat4& view, const glm::mat4& proj) {
        UniformBufferObject ubo{};
        ubo.model = glm::mat4(1.0f); // Handled individually via push constants
        ubo.view = view;
        ubo.proj = proj;

        // Push the matrices to the GPU!
        memcpy(uniformBuffersMapped[currentFrame], &ubo, sizeof(ubo));
    }

    void VulkanVertexBackend::submitLightingPass(const glm::vec3& cameraPos, const glm::mat4& view, const glm::mat4& proj) {
        VkRenderPassBeginInfo lightingPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        lightingPassInfo.renderPass = lightingRenderPass;
        lightingPassInfo.framebuffer = lightingFramebuffers[currentImageIndex];
        lightingPassInfo.renderArea.extent = vkSwapchain->getExtent();

        VkClearValue lightingClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        lightingPassInfo.clearValueCount = 1;
        lightingPassInfo.pClearValues = &lightingClearColor;

        vkCmdBeginRenderPass(currentCmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipeline());

        // Bind the G-Buffer Textures internally managed by the backend
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipelineLayout(),
            0, 1, &lightingDescriptorSets[currentImageIndex], 0, nullptr);

        LightingPushConstants push{};
        push.viewPos = glm::vec4(cameraPos, 1.0f);
        push.invView = glm::inverse(view);
        push.invProj = glm::inverse(proj);

        vkCmdPushConstants(currentCmd, lightingPipeline->getPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(LightingPushConstants), &push);

        // Draw the full screen triangle without vertex buffers
        vkCmdDraw(currentCmd, 3, 1, 0, 0);

        // 2. Draw the Selection Outline directly on top of it!
        // (You will need to quickly add a 'selectionPipeline' to your VkLightingPipeline 
        // class that compiles 'select.vert' and 'select.frag' using the exact same layout)
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getSelectionPipeline());
        vkCmdDraw(currentCmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(currentCmd);
    }

    void VulkanVertexBackend::submitGlassDepthPass(const std::vector<DrawPacket>& transparentQueue) {
        if (transparentQueue.empty()) return;

        VkRenderPassBeginInfo glassDepthPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        glassDepthPassInfo.renderPass = glassDepthPass->getRenderPass();
        glassDepthPassInfo.framebuffer = glassDepthFramebuffers[currentImageIndex];
        glassDepthPassInfo.renderArea.extent = vkSwapchain->getExtent();

        VkClearValue depthClearValue{};
        depthClearValue.depthStencil = { 1.0f, 0 };
        glassDepthPassInfo.clearValueCount = 1;
        glassDepthPassInfo.pClearValues = &depthClearValue;

        vkCmdBeginRenderPass(currentCmd, &glassDepthPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassDepthPipeline->getPipeline());

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;            // Start at the bottom
        viewport.width = (float)vkSwapchain->getExtent().width;
        viewport.height = (float)vkSwapchain->getExtent().height;       // Draw upwards!
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;        
        vkCmdSetViewport(currentCmd, 0, 1, &viewport);

        VkRect2D scissor{ {0, 0}, vkSwapchain->getExtent() };
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getPipelineLayout(), 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

        for (const auto& packet : transparentQueue) {
            auto* geometry = geometryVault.get(packet.geometry);
            if (!geometry) continue;

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            MeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            vkCmdPushConstants(currentCmd, gBufferPipeline->getPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);

            vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
        }

        vkCmdEndRenderPass(currentCmd);
    }

    void VulkanVertexBackend::submitTransparentQueue(const std::vector<DrawPacket>& transparentQueue) {
        if (transparentQueue.empty()) return;

        // 1. BUCKETIZE INTO BACKGROUND & FOREGROUND
        // The queue is already sorted Back-to-Front by the frontend!
        std::vector<DrawPacket> backgroundBucket;
        std::vector<DrawPacket> foregroundBucket;

        if (transparentQueue.size() > 1) {
            // Everything except the last element is background
            backgroundBucket.assign(transparentQueue.begin(), transparentQueue.end() - 1);
            // The last element is the absolute closest piece of glass
            foregroundBucket.push_back(transparentQueue.back());
        }
        else {
            foregroundBucket.push_back(transparentQueue.back());
        }

        // 2. THE REUSABLE RENDER LAMBDA
        auto executeGlassLayer = [&](const std::vector<DrawPacket>& glassBucket) {
            if (glassBucket.empty()) return;

            // --- A. VRAM PHOTOGRAPH: COPY LIT SCENE ---
            VkImage litSceneImage = litSceneImages[currentImageIndex];
            VkImage opaqueSceneCopy = opaqueSceneCopyImages[currentImageIndex];

            VkImageMemoryBarrier litSrcBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            litSrcBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            litSrcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            litSrcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            litSrcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            litSrcBarrier.image = litSceneImage;
            litSrcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            litSrcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            litSrcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            VkImageMemoryBarrier copyDstBarrier = litSrcBarrier;
            copyDstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            copyDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copyDstBarrier.image = opaqueSceneCopy;
            copyDstBarrier.srcAccessMask = 0;
            copyDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            VkImageMemoryBarrier copyBarriers[] = { litSrcBarrier, copyDstBarrier };
            vkCmdPipelineBarrier(currentCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 2, copyBarriers);

            VkImageCopy imageCopyRegion{};
            imageCopyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            imageCopyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            imageCopyRegion.extent = { vkSwapchain->getExtent().width, vkSwapchain->getExtent().height, 1 };

            vkCmdCopyImage(currentCmd, litSceneImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                opaqueSceneCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopyRegion);

            VkImageMemoryBarrier litDstBarrier = litSrcBarrier;
            litDstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            litDstBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            litDstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            litDstBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            VkImageMemoryBarrier copyReadBarrier = copyDstBarrier;
            copyReadBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copyReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            copyReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            copyReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            VkImageMemoryBarrier endBarriers[] = { litDstBarrier, copyReadBarrier };
            vkCmdPipelineBarrier(currentCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, endBarriers);

            // --- B. GLASS DEPTH PASS ---
            VkRenderPassBeginInfo glassDepthPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            glassDepthPassInfo.renderPass = glassDepthPass->getRenderPass();
            glassDepthPassInfo.framebuffer = glassDepthFramebuffers[currentImageIndex];
            glassDepthPassInfo.renderArea.extent = vkSwapchain->getExtent();

            VkClearValue depthClearValue{};
            depthClearValue.depthStencil = { 1.0f, 0 };
            glassDepthPassInfo.clearValueCount = 1;
            glassDepthPassInfo.pClearValues = &depthClearValue;

            vkCmdBeginRenderPass(currentCmd, &glassDepthPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkPipelineLayout gLayout = gBufferPipeline->getPipelineLayout();
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassDepthPipeline->getPipeline());

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;            // Start at the bottom
            viewport.width = (float)vkSwapchain->getExtent().width;
            viewport.height = (float)vkSwapchain->getExtent().height;       // Draw upwards!
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            vkCmdSetViewport(currentCmd, 0, 1, &viewport);
            VkRect2D scissor{ {0, 0}, vkSwapchain->getExtent() };
            vkCmdSetScissor(currentCmd, 0, 1, &scissor);

            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

            for (const auto& packet : glassBucket) {
                auto* geometry = geometryVault.get(packet.geometry);
                if (!geometry) continue;

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                vkCmdPushConstants(currentCmd, gLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
            vkCmdEndRenderPass(currentCmd);

            // --- C. FORWARD TRANSLUCENCY PASS ---
            VkRenderPassBeginInfo forwardPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            forwardPassInfo.renderPass = forwardPass->getRenderPass();
            forwardPassInfo.framebuffer = forwardFramebuffers[currentImageIndex];
            forwardPassInfo.renderArea.extent = vkSwapchain->getExtent();

            vkCmdBeginRenderPass(currentCmd, &forwardPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkPipelineLayout fLayout = forwardPipeline->getPipelineLayout();
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipeline->getPipeline());

            vkCmdSetViewport(currentCmd, 0, 1, &viewport);
            vkCmdSetScissor(currentCmd, 0, 1, &scissor);

            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);
            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fLayout, 2, 1, &lightingDescriptorSets[currentImageIndex], 0, nullptr);

            MaterialHandle lastBoundMaterial;
            bool firstGlass = true;

            for (const auto& packet : glassBucket) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                if (!geometry || !material) continue;

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.baseColor = material->baseColor;
                push.metallicFactor = material->metallicFactor;
                push.roughnessFactor = material->roughnessFactor;
                push.emissiveFactor = material->emissiveFactor;

                vkCmdPushConstants(currentCmd, fLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);

                if (firstGlass || packet.material != lastBoundMaterial) {
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fLayout, 1, 1, &material->descriptorSets[currentFrame], 0, nullptr);
                    lastBoundMaterial = packet.material;
                    firstGlass = false;
                }

                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
            vkCmdEndRenderPass(currentCmd);
            };

        // 3. EXECUTE THE PASSES
        executeGlassLayer(backgroundBucket);
        executeGlassLayer(foregroundBucket);

        // Transition lit scene back for ImGui reading
        VkImageMemoryBarrier finalLitBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        finalLitBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        finalLitBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        finalLitBarrier.image = litSceneImages[currentImageIndex];
        finalLitBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        finalLitBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        finalLitBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(currentCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &finalLitBarrier);
    }

    void VulkanVertexBackend::submitUIPass() {
        ImGui::Render();
        VkRenderPassBeginInfo uiPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        uiPassInfo.renderPass = uiPass->getRenderPass();
        uiPassInfo.framebuffer = uiFramebuffers[currentImageIndex];
        uiPassInfo.renderArea.extent = vkSwapchain->getExtent();

        VkClearValue uiClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        uiPassInfo.clearValueCount = 1;
        uiPassInfo.pClearValues = &uiClearColor;

        vkCmdBeginRenderPass(currentCmd, &uiPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Because we abstracted the UI pass, the backend just asks ImGui to record 
        // its internal vertex buffers into the current command buffer.
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data) {
            ImGui_ImplVulkan_RenderDrawData(draw_data, currentCmd);
        }

        vkCmdEndRenderPass(currentCmd);
    }

    void VulkanVertexBackend::endFrame() {
        if (vkEndCommandBuffer(currentCmd) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }

        VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkSemaphore waitSemaphores[] = { vkSyncObjects->getImageAvailableSemaphore(currentFrame) };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &currentCmd;

        VkSemaphore signalSemaphores[] = { vkSyncObjects->getRenderFinishedSemaphore(currentImageIndex) };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(vkContext->getGraphicsQueue(), 1, &submitInfo, vkSyncObjects->getInFlightFence(currentFrame)) != VK_SUCCESS) {
            throw std::runtime_error("failed to submit draw command buffer!");
        }

        VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapChains[] = { vkSwapchain->getSwapchain() };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &currentImageIndex;

        VkResult presentResult = vkQueuePresentKHR(vkContext->getPresentQueue(), &presentInfo);

        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            // Handled by Application.cpp flag
        }
        else if (presentResult != VK_SUCCESS) {
            throw std::runtime_error("Failed to present swap chain image!");
        }

        currentFrame = (currentFrame + 1) % VkSyncObjects::MAX_FRAMES_IN_FLIGHT;
    }

    // ==============================================================================
    // 4. EDITOR & UI ABSTRACTIONS
    // ==============================================================================

    void VulkanVertexBackend::beginUI() {
        // We initialize the specific backend frames here so the high-level 
        // EditorSystem doesn't need to know we are using Vulkan or GLFW.
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    void* VulkanVertexBackend::getLitSceneTextureID() {
        // uiSceneTextures is the std::vector<VkDescriptorSet> we registered with ImGui during init().
        // We cast it to void* so it can securely cross the API boundary into your ViewportPanel.
        return (void*)uiSceneTextures[currentImageIndex];
    }

    void* VulkanVertexBackend::getGlassDepthTextureID() {
        return (void*)uiDepthTextures[currentImageIndex];
    }

} 

// namespace Iridium