#include "VulkanVertexBackend.h"
#include "renderer/rhi/Mesh.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"
#include "vendor/imguizmo/ImGuizmo.h"
#include <stdexcept>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Iridium {

    namespace {
        VkIndexType toVkIndexType(IndexFormat format) {
            switch (format) {
            case IndexFormat::UInt16:
                return VK_INDEX_TYPE_UINT16;
            case IndexFormat::UInt32:
                return VK_INDEX_TYPE_UINT32;
            }
            throw std::invalid_argument("Unsupported geometry index format.");
        }
    }

    // ==============================================================================
    // 1. SYSTEM LIFECYCLE
    // ==============================================================================

    void VulkanVertexBackend::init(GLFWwindow* window) {
        if (initialized_) {
            throw std::logic_error("VulkanVertexBackend was initialized more than once.");
        }

        vkContext = std::make_unique<VkContext>(true, window);
        resourceAllocator.init(vkContext->getPhysicalDevice(), vkContext->getDevice());
        uploadContext.init(vkContext->getDevice(), vkContext->getGraphicsQueue(),
            vkContext->getGraphicsQueueFamily(), resourceAllocator);
        vkSwapchain = std::make_unique<VkSwapchain>(vkContext.get(), window);
        scheduler.init(vkContext->getDevice(), vkContext->getGraphicsQueue(),
            vkContext->getPresentQueue(), vkContext->getGraphicsQueueFamily(),
            vkSwapchain->getImageCount());

        descriptorAllocator.init(vkContext->getDevice());

        // 2. Lighting and forward pass contracts needed by the shared mesh layouts.
        createLightingRenderPass();
        lightingPipeline = std::make_unique<VkLightingPipeline>(vkContext.get(), lightingRenderPass);
        forwardPass = std::make_unique<VkForwardRenderPass>(vkContext.get(), vkSwapchain->getImageFormat(), VK_FORMAT_D32_SFLOAT);
        meshLayouts.init(vkContext->getDevice(), lightingPipeline->getDescriptorSetLayout());

        // 3. G-Buffer Pass
        gBufferPass = std::make_unique<VkRenderPassWrapper>(vkContext.get(), vkSwapchain.get());
        gBufferPipeline = std::make_unique<VkGraphicsPipeline>(vkContext.get(), vkSwapchain.get(), gBufferPass.get(),
            meshLayouts.getGBufferPipelineLayout());

        // 4. Glass Depth Pass
        glassDepthPass = std::make_unique<GlassDepthRenderPass>();
        glassDepthPass->init(vkContext->getDevice(), VK_FORMAT_D32_SFLOAT);

        glassDepthPipeline = std::make_unique<GlassDepthPipeline>();
        glassDepthPipeline->init(vkContext->getDevice(), glassDepthPass->getRenderPass(),
            meshLayouts.getGBufferPipelineLayout(),
            std::string(PROJECT_ROOT_DIR) + "assets/shaders/glass_depth_vert.spv");

        pipelineLibrary.init(vkContext->getDevice(),
            { gBufferPass->getRenderPass(), meshLayouts.getGBufferPipelineLayout(), 3 },
            { forwardPass->getRenderPass(), meshLayouts.getForwardPipelineLayout(), 1 });

        // 5. UI Pass
        uiPass = std::make_unique<VkUIRenderPass>(vkContext.get(), vkSwapchain->getImageFormat());

        // 7. Render Targets
        initFrameTargets();
        // Target descriptors declare shader-read layouts, so submit their initial
        // Undefined -> ShaderResource transitions before any descriptor or ImGui
        // registration can reference those images.
        uploadContext.flush();

        // 8. Global Camera Buffers
        createUniformBuffers();

        // --------------------------------

        // 2. Global Descriptor Sets (Camera Data)
        globalDescriptorSets.resize(VulkanFrameScheduler::FramesInFlight);
        for (size_t i = 0; i < VulkanFrameScheduler::FramesInFlight; i++) {
            globalDescriptorSets[i] = descriptorAllocator.allocate(meshLayouts.getGlobalSetLayout());

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i].buffer;
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

        // 3. Lighting descriptors (one set per swapchain image).
        const uint32_t imgCount = vkSwapchain->getImageCount();
        sceneDescriptors.init(vkContext->getDevice(), descriptorAllocator,
            lightingPipeline->getDescriptorSetLayout());
        sceneDescriptors.rebuild(frameTargets);

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
        imguiInitialized_ = true;

        // Create the initial ImGui textures for the viewport!
        uint32_t currentImgCount = vkSwapchain->getImageCount();
        uiSceneTextures.resize(currentImgCount);
        uiDepthTextures.resize(currentImgCount);
        for (size_t i = 0; i < currentImgCount; i++) {
            const VulkanPerImageTargets& targets = frameTargets.get(i);
            uiSceneTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(), targets.litScene.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(), targets.glassDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        initialized_ = true;
        cleaned_ = false;
    }
    
    void VulkanVertexBackend::setEnvironmentMap(TextureHandle hdriHandle) {
        environmentMapHandle = hdriHandle;

        auto* payload = textureVault.get(hdriHandle);
        if (!payload) return;

        const VkDescriptorImageInfo environment{
            payload->sampler, payload->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        sceneDescriptors.setEnvironment(environment);
    }

    void VulkanVertexBackend::cleanup() {
        if (!initialized_ || cleaned_) {
            return;
        }
        cleaned_ = true;

        const VkDevice device = vkContext->getDevice();
        vkDeviceWaitIdle(device);
        uploadContext.flush();
        scheduler.waitForAllFrames();

        pipelineLibrary.cleanup();

        for (VkDescriptorSet texture : uiSceneTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        for (VkDescriptorSet texture : uiDepthTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        uiSceneTextures.clear();
        uiDepthTextures.clear();
        if (imguiInitialized_) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            imguiInitialized_ = false;
        }
        if (imguiPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, imguiPool, nullptr);
            imguiPool = VK_NULL_HANDLE;
        }

        sceneDescriptors.cleanup();
        frameTargets.cleanup();

        geometryVault.forEach([this](VulkanGeometryPayload& payload) {
            resourceAllocator.destroy(payload.vertexBuffer);
            resourceAllocator.destroy(payload.indexBuffer);
            });

        textureVault.forEach([this](VulkanTexturePayload& payload) {
            vkDestroySampler(vkContext->getDevice(), payload.sampler, nullptr);
            resourceAllocator.destroy(payload.image);
            });

        for (size_t i = 0; i < uniformBuffers.size(); i++) {
            resourceAllocator.destroy(uniformBuffers[i]);
        }

        if (glassDepthPipeline) {
            glassDepthPipeline->cleanup();
            glassDepthPipeline.reset();
        }
        if (glassDepthPass) {
            glassDepthPass->cleanup();
            glassDepthPass.reset();
        }

        forwardPass.reset();

        lightingPipeline.reset();
        if (lightingRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, lightingRenderPass, nullptr);
            lightingRenderPass = VK_NULL_HANDLE;
        }

        uiPass.reset();
        gBufferPipeline.reset();
        gBufferPass.reset();

        descriptorAllocator.cleanup();
        meshLayouts.cleanup();

        scheduler.cleanup();
        uploadContext.cleanup();
        resourceAllocator.cleanup();

        vkSwapchain.reset();
        vkContext.reset();
        initialized_ = false;
    }

    void VulkanVertexBackend::recreateSwapchain(GLFWwindow* window) {
        // 1. Handle Minimization (Pause the engine until it's un-minimized)
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        const VkFormat oldImageFormat = vkSwapchain->getImageFormat();
        const uint32_t oldImageCount = vkSwapchain->getImageCount();
        auto candidate = std::make_unique<VkSwapchain>(vkContext.get(), window, vkSwapchain->getSwapchain());
        if (candidate->getImageFormat() != oldImageFormat) {
            throw std::runtime_error("Swapchain format changed; a full renderer rebuild is required.");
        }

        // Resize is the one accepted global stall, after candidate validation.
        vkDeviceWaitIdle(vkContext->getDevice());

        for (VkDescriptorSet texture : uiSceneTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        for (VkDescriptorSet texture : uiDepthTextures) {
            if (texture != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(texture);
            }
        }
        uiSceneTextures.clear();
        uiDepthTextures.clear();
        sceneDescriptors.cleanup();
        frameTargets.cleanup();

        vkSwapchain = std::move(candidate);
        const uint32_t newImageCount = vkSwapchain->getImageCount();
        scheduler.resetSwapchainImages(newImageCount);
        initFrameTargets();
        // The replacement target images are referenced by descriptor sets and
        // ImGui immediately below; establish their declared layouts first.
        uploadContext.flush();
        sceneDescriptors.init(vkContext->getDevice(), descriptorAllocator,
            lightingPipeline->getDescriptorSetLayout());
        sceneDescriptors.rebuild(frameTargets);
        if (environmentMapHandle.isValid()) {
            setEnvironmentMap(environmentMapHandle);
        }
        if (newImageCount != oldImageCount) {
            ImGui_ImplVulkan_SetMinImageCount(newImageCount);
        }

        uiSceneTextures.resize(newImageCount);
        uiDepthTextures.resize(newImageCount);
        for (size_t i = 0; i < newImageCount; i++) {
            const VulkanPerImageTargets& targets = frameTargets.get(i);
            uiSceneTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(), targets.litScene.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            uiDepthTextures[i] = ImGui_ImplVulkan_AddTexture(frameTargets.sampler(), targets.glassDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

    }

    // ==============================================================================
    // 2. RESOURCE MANAGEMENT (Thread-Safe & Anti-Fragmentation)
    // ==============================================================================

    GeometryHandle VulkanVertexBackend::allocateGeometry(const GeometryDesc& desc,
        std::span<const std::byte> vertexBytes, std::span<const std::byte> indexBytes) {
        const uint32_t indexSize = indexElementSize(desc.indexFormat);
        if (desc.vertexStride == 0 || indexSize == 0) {
            throw std::invalid_argument("Geometry format must define nonzero element sizes.");
        }
        if (indexBytes.size_bytes() % indexSize != 0) {
            throw std::invalid_argument("Geometry index data is not aligned to its index format.");
        }

        VulkanGeometryPayload payload{};
        payload.indexCount = static_cast<uint32_t>(indexBytes.size_bytes() / indexSize);
        payload.indexFormat = desc.indexFormat;

        payload.vertexBuffer = resourceAllocator.createBuffer(vertexBytes.size_bytes(),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        try {
            payload.indexBuffer = resourceAllocator.createBuffer(indexBytes.size_bytes(),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            uploadContext.enqueueBufferUpload(payload.vertexBuffer, vertexBytes,
                ResourceState::VertexBuffer);
            uploadContext.enqueueBufferUpload(payload.indexBuffer, indexBytes,
                ResourceState::IndexBuffer);
        } catch (...) {
            resourceAllocator.destroy(payload.indexBuffer);
            resourceAllocator.destroy(payload.vertexBuffer);
            throw;
        }

        return geometryVault.allocate(payload);
    }

    void VulkanVertexBackend::freeGeometry(GeometryHandle handle) {
        auto* payload = geometryVault.get(handle);
        if (payload) {
            // Capture the Vulkan pointers by value so the lambda remembers them
            // Defer the destruction! The GPU won't crash, and the CPU won't stall.
            scheduler.defer([this,
                vertex = payload->vertexBuffer, index = payload->indexBuffer]() mutable {
                resourceAllocator.destroy(vertex);
                resourceAllocator.destroy(index);
                });

            geometryVault.free(handle);
        }
    }

    TextureHandle VulkanVertexBackend::allocateTexture(const TextureDesc& desc,
        std::span<const std::byte> pixelBytes) {
        if (desc.width == 0 || desc.height == 0) {
            throw std::invalid_argument("Texture dimensions must be nonzero");
        }
        if (pixelBytes.empty()) {
            throw std::invalid_argument("Texture pixel data must be nonempty");
        }

        const uint32_t texelBytes = bytesPerTexel(desc.format);
        const size_t expectedBytes = static_cast<size_t>(desc.width) * desc.height * texelBytes;
        if (texelBytes == 0 || pixelBytes.size() != expectedBytes) {
            throw std::invalid_argument("Texture pixel data size does not match the descriptor");
        }

        VulkanTexturePayload payload{};
        payload.format = desc.format;

        VkFormat format = VK_FORMAT_UNDEFINED;
        switch (desc.format) {
        case TextureFormat::RGBA8_UNorm:
            format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case TextureFormat::RGBA8_sRGB:
            format = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case TextureFormat::RGBA32_SFloat:
            format = VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        }

        const auto toVkFilter = [](FilterMode mode) {
            return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        };
        const auto toVkAddressMode = [](SamplerAddressMode mode) {
            return mode == SamplerAddressMode::ClampToEdge
                ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };

        payload.image = resourceAllocator.createImage2D({ desc.width, desc.height }, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = toVkFilter(desc.sampler.magFilter);
        samplerInfo.minFilter = toVkFilter(desc.sampler.minFilter);
        samplerInfo.addressModeU = toVkAddressMode(desc.sampler.addressU);
        samplerInfo.addressModeV = toVkAddressMode(desc.sampler.addressV);
        samplerInfo.addressModeW = toVkAddressMode(desc.sampler.addressW);
        VkResult result = vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr, &payload.sampler);
        if (result != VK_SUCCESS) {
            resourceAllocator.destroy(payload.image);
            throw std::runtime_error("Failed to create texture sampler.");
        }

        try {
            uploadContext.enqueueImageUpload(payload.image, pixelBytes, ResourceState::ShaderResource);
        } catch (...) {
            vkDestroySampler(vkContext->getDevice(), payload.sampler, nullptr);
            resourceAllocator.destroy(payload.image);
            throw;
        }

        return textureVault.allocate(payload);
    }



    void VulkanVertexBackend::freeTexture(TextureHandle handle) {
        auto* payload = textureVault.get(handle);
        if (payload) {
            VkSampler sampler = payload->sampler;
            VulkanImageResource image = payload->image;

            scheduler.defer([this, sampler, image]() mutable {
                vkDestroySampler(vkContext->getDevice(), sampler, nullptr);
                resourceAllocator.destroy(image);
                });

            textureVault.free(handle);
        }
    }

    MaterialBinding VulkanVertexBackend::allocateMaterial(const MaterialAsset& asset) {
        auto* albedo = textureVault.get(asset.albedoMap);
        auto* normal = textureVault.get(asset.normalMap);
        auto* pbr = textureVault.get(asset.pbrMap);
        auto* emissive = textureVault.get(asset.emissiveMap);
        auto* transmission = textureVault.get(asset.transmissionMap);

        if (!albedo) {
            throw std::invalid_argument("Invalid albedo texture handle in material asset");
        }
        if (!normal) {
            throw std::invalid_argument("Invalid normal texture handle in material asset");
        }
        if (!pbr) {
            throw std::invalid_argument("Invalid PBR texture handle in material asset");
        }
        if (!emissive) {
            throw std::invalid_argument("Invalid emissive texture handle in material asset");
        }
        if (!transmission) {
            throw std::invalid_argument("Invalid transmission texture handle in material asset");
        }

        VulkanMaterialPayload mat{};
        mat.pipeline = pipelineLibrary.getOrCreatePipeline(asset.pipelineState);
        mat.renderQueue = asset.renderQueue;
        mat.baseColor = asset.baseColor;
        mat.emissiveFactor = asset.emissiveFactor;
        mat.metallicFactor = asset.metallic;
        mat.roughnessFactor = asset.roughness;
        mat.normalScale = asset.normalScale;
        mat.alphaCutoff = asset.alphaCutoff;
        mat.transmissionFactor = asset.transmissionFactor;

        for (size_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            VkDescriptorSet matSet = descriptorAllocator.allocate(meshLayouts.getMaterialSetLayout());

            std::array<VkDescriptorImageInfo, 5> imageInfos{};
            imageInfos[0] = { albedo->sampler, albedo->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            imageInfos[1] = { normal->sampler, normal->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            imageInfos[2] = { pbr->sampler, pbr->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            imageInfos[3] = { emissive->sampler, emissive->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            imageInfos[4] = { transmission->sampler, transmission->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            std::array<VkWriteDescriptorSet, 5> writes{};
            for (size_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = matSet;
                writes[binding].dstBinding = static_cast<uint32_t>(binding);
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].descriptorCount = 1;
                writes[binding].pImageInfo = &imageInfos[binding];
            }

            vkUpdateDescriptorSets(vkContext->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            mat.descriptorSets[frame] = matSet;
        }

        MaterialHandle material = materialVault.allocate(mat);
        return { material, mat.pipeline, mat.renderQueue, makeOpaqueSortKey(mat.pipeline, material) };
    }

    void VulkanVertexBackend::freeMaterial(MaterialHandle handle) {
        auto* payload = materialVault.get(handle);
        if (!payload) {
            return;
        }

        const auto descriptorSets = payload->descriptorSets;
        scheduler.defer([this, descriptorSets]() {
            descriptorAllocator.free(std::span<const VkDescriptorSet>(descriptorSets));
        });

        materialVault.free(handle);
    }

    // --- PRIVATE HELPERS ---

    void VulkanVertexBackend::createLightingRenderPass() {
        // This pass writes the evaluated lighting to the per-image lit-scene target.
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = vkSwapchain->getImageFormat();
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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

    void VulkanVertexBackend::initFrameTargets() {
        frameTargets.init(vkContext->getDevice(), resourceAllocator, *vkSwapchain,
            { gBufferPass->getRenderPass(), lightingRenderPass, forwardPass->getRenderPass(),
                glassDepthPass->getRenderPass(), uiPass->getRenderPass() });

        for (VulkanPerImageTargets& target : frameTargets.targets()) {
            uploadContext.enqueueTransition(target.opaqueCopy, ResourceState::ShaderResource);
            uploadContext.enqueueTransition(target.glassDepth, ResourceState::ShaderResource);
        }
    }

    void VulkanVertexBackend::createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);
        size_t frameCount = VulkanFrameScheduler::FramesInFlight;

        uniformBuffers.resize(frameCount);

        for (size_t i = 0; i < frameCount; i++) {
            uniformBuffers[i] = resourceAllocator.createBuffer(bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true);
        }
    }

    // ==============================================================================
    // 3. THE FRAME PIPELINE (Data-Driven Execution)
    // ==============================================================================

    FrameStatus VulkanVertexBackend::beginFrame() {
        uploadContext.flush();
        const VulkanFrameBegin frame = scheduler.beginFrame(vkSwapchain->getSwapchain());
        if (frame.status == FrameStatus::RecreateSwapchain) {
            return frame.status;
        }

        currentImageIndex = frame.imageIndex;
        currentCmd = frame.commandBuffer;
        return frame.status;
    }

    void VulkanVertexBackend::submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> selectionQueue, bool isWireframe) {
        VkRenderPassBeginInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpInfo.renderPass = gBufferPass->getRenderPass();
        rpInfo.framebuffer = frameTargets.get(currentImageIndex).gBufferFramebuffer;
        rpInfo.renderArea.extent = vkSwapchain->getExtent();

        std::array<VkClearValue, 4> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Normal
        clearValues[1].color = { {0.1f, 0.1f, 0.1f, 1.0f} }; // Albedo
        clearValues[2].color = { {0.0f, 0.0f, 0.0f, 0.0f} }; // Emissive
        clearValues[3].depthStencil = { 1.0f, 0 };            // Depth

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

        const VkPipelineLayout meshLayout = meshLayouts.getGBufferPipelineLayout();

        // ==============================================================================
        // PHASE 1: DRAW OPAQUE SCENE
        // ==============================================================================

        PipelineHandle lastBoundPipeline{};
        MaterialHandle lastBoundMaterial{};
        GeometryHandle lastBoundGeometry{};

        if (isWireframe) {
            // Editor wireframe is a deliberate fixed override, not a material PSO.
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getWireframePipeline());
            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);

            for (const auto& packet : opaqueQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                if (!geometry || !material) continue;

                if (packet.material != lastBoundMaterial) {
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                        1, 1, &material->descriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.baseColor = material->baseColor;
                push.emissiveFactor = material->emissiveFactor;
                push.metallicFactor = material->metallicFactor;
                push.roughnessFactor = material->roughnessFactor;
                push.normalScale = material->normalScale;
                push.alphaCutoff = material->alphaCutoff;
                push.transmissionFactor = material->transmissionFactor;
                vkCmdPushConstants(currentCmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(MeshPushConstants), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
        }
        else {
            VkPipelineLayout activeLayout = VK_NULL_HANDLE;

            for (const auto& packet : opaqueQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                const VulkanPipelineRecord* record = pipelineLibrary.get(packet.pipeline);
                if (!geometry || !material) continue;

                // Invalid/stale handles and non-G-buffer records are not drawable here.
                if (!record || record->pipeline == VK_NULL_HANDLE ||
                    record->pipelineLayout == VK_NULL_HANDLE ||
                    record->renderPass != RenderPassClass::GBuffer) {
                    continue;
                }

                if (packet.pipeline != lastBoundPipeline) {
                    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
                    activeLayout = record->pipelineLayout;
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    lastBoundPipeline = packet.pipeline;
                    lastBoundMaterial = MaterialHandle{};
                }
                if (packet.material != lastBoundMaterial) {
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        1, 1, &material->descriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.baseColor = material->baseColor;
                push.emissiveFactor = material->emissiveFactor;
                push.metallicFactor = material->metallicFactor;
                push.roughnessFactor = material->roughnessFactor;
                push.normalScale = material->normalScale;
                push.alphaCutoff = material->alphaCutoff;
                push.transmissionFactor = material->transmissionFactor;
                vkCmdPushConstants(currentCmd, activeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(MeshPushConstants), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
        }

        // ==============================================================================
        // PHASE 2: DRAW SELECTION MASKS (Depth Testing Disabled = X-Ray)
        // ==============================================================================

        if (!selectionQueue.empty()) {
            vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferPipeline->getOutlinePipeline());
            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);

            lastBoundMaterial = MaterialHandle{};
            lastBoundGeometry = GeometryHandle{};

            for (const auto& packet : selectionQueue) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);

                if (!geometry || !material) continue;

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.baseColor = material->baseColor;
                push.emissiveFactor = material->emissiveFactor;
                push.metallicFactor = material->metallicFactor;
                push.roughnessFactor = material->roughnessFactor;
                push.normalScale = material->normalScale;
                push.alphaCutoff = material->alphaCutoff;
                push.transmissionFactor = material->transmissionFactor;

                if (packet.material != lastBoundMaterial) {
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                        1, 1, &material->descriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                vkCmdPushConstants(currentCmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(MeshPushConstants), &push);

                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
        }

        vkCmdEndRenderPass(currentCmd);

        VulkanPerImageTargets& targets = frameTargets.get(currentImageIndex);
        VulkanCommandList commandList(scheduler.currentCommandBuffer());
        commandList.markState(targets.normal, ResourceState::ShaderResource);
        commandList.markState(targets.albedo, ResourceState::ShaderResource);
        commandList.markState(targets.emissive, ResourceState::ShaderResource);
        commandList.markState(targets.depth, ResourceState::DepthRead);
    }

    void VulkanVertexBackend::updateCamera(const glm::mat4& view, const glm::mat4& proj) {
        UniformBufferObject ubo{};
        ubo.model = glm::mat4(1.0f); // Handled individually via push constants
        ubo.view = view;
        ubo.proj = proj;

        // Push the matrices to the GPU!
        std::memcpy(uniformBuffers[scheduler.currentFrameIndex()].mapped, &ubo, sizeof(ubo));
    }

    void VulkanVertexBackend::submitLightingPass(const glm::vec3& cameraPos, const glm::mat4& view, const glm::mat4& proj) {
        VulkanPerImageTargets& targets = frameTargets.get(currentImageIndex);
        VulkanCommandList commandList(scheduler.currentCommandBuffer());
        commandList.transition(targets.litScene, ResourceState::ColorAttachment);

        VkRenderPassBeginInfo lightingPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        lightingPassInfo.renderPass = lightingRenderPass;
        lightingPassInfo.framebuffer = frameTargets.get(currentImageIndex).lightingFramebuffer;
        lightingPassInfo.renderArea.extent = vkSwapchain->getExtent();

        VkClearValue lightingClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        lightingPassInfo.clearValueCount = 1;
        lightingPassInfo.pClearValues = &lightingClearColor;

        vkCmdBeginRenderPass(currentCmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipeline());

        // Bind the G-Buffer Textures internally managed by the backend
        const VkDescriptorSet sceneSet = sceneDescriptors.get(currentImageIndex);
        vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipelineLayout(),
            0, 1, &sceneSet, 0, nullptr);

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
        commandList.markState(targets.litScene, ResourceState::ColorAttachment);
    }

    void VulkanVertexBackend::submitTransparentQueue(std::span<const DrawPacket> transparentQueue) {

        // 1. BUCKETIZE INTO BACKGROUND & FOREGROUND
        // The queue is already sorted Back-to-Front by the frontend.
        const std::span<const DrawPacket> foregroundBucket = transparentQueue.empty()
            ? std::span<const DrawPacket>{}
            : transparentQueue.last(1);
        const std::span<const DrawPacket> backgroundBucket = transparentQueue.size() > 1
            ? transparentQueue.first(transparentQueue.size() - 1)
            : std::span<const DrawPacket>{};

        // 2. THE REUSABLE RENDER LAMBDA
        auto executeGlassLayer = [&](std::span<const DrawPacket> glassBucket) {
            if (glassBucket.empty()) return;

            // --- A. VRAM PHOTOGRAPH: COPY LIT SCENE ---
            VulkanPerImageTargets& targets = frameTargets.get(currentImageIndex);
            VulkanCommandList commandList(scheduler.currentCommandBuffer());
            commandList.transition(targets.litScene, ResourceState::CopySource);
            commandList.transition(targets.opaqueCopy, ResourceState::CopyDestination);

            VkImageCopy imageCopyRegion{};
            imageCopyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            imageCopyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            imageCopyRegion.extent = { vkSwapchain->getExtent().width, vkSwapchain->getExtent().height, 1 };

            commandList.copyImage(targets.litScene, targets.opaqueCopy, imageCopyRegion);
            commandList.transition(targets.litScene, ResourceState::ColorAttachment);
            commandList.transition(targets.opaqueCopy, ResourceState::ShaderResource);

            // --- B. GLASS DEPTH PASS ---
            commandList.transition(targets.glassDepth, ResourceState::DepthWrite);
            VkRenderPassBeginInfo glassDepthPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            glassDepthPassInfo.renderPass = glassDepthPass->getRenderPass();
            glassDepthPassInfo.framebuffer = frameTargets.get(currentImageIndex).glassDepthFramebuffer;
            glassDepthPassInfo.renderArea.extent = vkSwapchain->getExtent();

            VkClearValue depthClearValue{};
            depthClearValue.depthStencil = { 1.0f, 0 };
            glassDepthPassInfo.clearValueCount = 1;
            glassDepthPassInfo.pClearValues = &depthClearValue;

            vkCmdBeginRenderPass(currentCmd, &glassDepthPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkPipelineLayout gLayout = meshLayouts.getGBufferPipelineLayout();
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

            vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);

            for (const auto& packet : glassBucket) {
                auto* geometry = geometryVault.get(packet.geometry);
                if (!geometry) continue;

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                    toVkIndexType(geometry->indexFormat));

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                vkCmdPushConstants(currentCmd, gLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);
                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
            vkCmdEndRenderPass(currentCmd);
            commandList.markState(targets.glassDepth, ResourceState::DepthWrite);
            commandList.transition(targets.glassDepth, ResourceState::ShaderResource);

            // --- C. FORWARD TRANSLUCENCY PASS ---
            VkRenderPassBeginInfo forwardPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            forwardPassInfo.renderPass = forwardPass->getRenderPass();
            forwardPassInfo.framebuffer = frameTargets.get(currentImageIndex).forwardFramebuffer;
            forwardPassInfo.renderArea.extent = vkSwapchain->getExtent();

            vkCmdBeginRenderPass(currentCmd, &forwardPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            PipelineHandle lastBoundPipeline{};
            MaterialHandle lastBoundMaterial{};
            GeometryHandle lastBoundGeometry{};
            VkPipelineLayout activeLayout = VK_NULL_HANDLE;
            const VkDescriptorSet sceneSet = sceneDescriptors.get(currentImageIndex);

            vkCmdSetViewport(currentCmd, 0, 1, &viewport);
            vkCmdSetScissor(currentCmd, 0, 1, &scissor);

            for (const auto& packet : glassBucket) {
                auto* geometry = geometryVault.get(packet.geometry);
                auto* material = materialVault.get(packet.material);
                const VulkanPipelineRecord* record = pipelineLibrary.get(packet.pipeline);
                if (!geometry || !material) continue;
                if (!record || record->pipeline == VK_NULL_HANDLE ||
                    record->pipelineLayout == VK_NULL_HANDLE ||
                    record->renderPass != RenderPassClass::Forward) {
                    continue;
                }

                if (packet.pipeline != lastBoundPipeline) {
                    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
                    activeLayout = record->pipelineLayout;
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        0, 1, &globalDescriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        2, 1, &sceneSet, 0, nullptr);
                    lastBoundPipeline = packet.pipeline;
                    lastBoundMaterial = MaterialHandle{};
                }
                if (packet.material != lastBoundMaterial) {
                    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        1, 1, &material->descriptorSets[scheduler.currentFrameIndex()], 0, nullptr);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(currentCmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(currentCmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                MeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.baseColor = material->baseColor;
                push.emissiveFactor = material->emissiveFactor;
                push.metallicFactor = material->metallicFactor;
                push.roughnessFactor = material->roughnessFactor;
                push.normalScale = material->normalScale;
                push.alphaCutoff = material->alphaCutoff;
                push.transmissionFactor = material->transmissionFactor;

                vkCmdPushConstants(currentCmd, activeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(MeshPushConstants), &push);

                vkCmdDrawIndexed(currentCmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
            }
            vkCmdEndRenderPass(currentCmd);
            };

        // 3. EXECUTE THE PASSES
        executeGlassLayer(backgroundBucket);
        executeGlassLayer(foregroundBucket);

        VulkanPerImageTargets& targets = frameTargets.get(currentImageIndex);
        VulkanCommandList commandList(scheduler.currentCommandBuffer());
        commandList.transition(targets.litScene, ResourceState::ShaderResource);
        commandList.transition(targets.glassDepth, ResourceState::ShaderResource);
    }

    void VulkanVertexBackend::submitUIPass() {
        ImGui::Render();
        VkRenderPassBeginInfo uiPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        uiPassInfo.renderPass = uiPass->getRenderPass();
        uiPassInfo.framebuffer = frameTargets.get(currentImageIndex).uiFramebuffer;
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

    FrameStatus VulkanVertexBackend::endFrame() {
        return scheduler.endFrame(vkSwapchain->getSwapchain(), currentImageIndex);
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
