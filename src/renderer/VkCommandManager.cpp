#include "VkCommandManager.h"
#include "assets/AssetManager.h"
#include <stdexcept>
#include <iostream> // Add this at the top of the file

VkCommandManager::VkCommandManager(VkContext* context, VkFramebufferWrapper* framebuffer,
    VkGraphicsPipeline* pipeline, uint32_t count)
    : context(context) {

    // 1. Create the Command Pool (The Factory)
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = context->getGraphicsQueueFamily(); // Uses the new getter
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(context->getDevice(), &poolInfo, nullptr, &this->commandPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create command pool!");
    }

    // 2. Allocate the Command Buffers
    commandBuffers.resize(count);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = this->commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = count;

    if (vkAllocateCommandBuffers(context->getDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate command buffers!");
    }
}

VkCommandManager::~VkCommandManager() {
    if (commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(context->getDevice(), commandPool, nullptr);
    }
}

void collectDrawCalls(Node* node, glm::mat4 parentTransform, ModelAsset* model, std::vector<RenderPacket>& drawList) {
    glm::mat4 globalTransform = parentTransform * node->localTransform;

    if (node->meshIndex != -1) {
        const auto& subMeshIndices = model->meshToSubMeshes[node->meshIndex];
        for (int subMeshIdx : subMeshIndices) {
            RenderPacket packet{};
            packet.subMesh = &model->subMeshes[subMeshIdx];
            packet.transform = globalTransform;
            packet.materialId = packet.subMesh->materialIndex;
            packet.model = model; // <--- STORE THE MODEL

            if (packet.materialId < model->materials.size()) {
                packet.material = &model->materials[packet.materialId];
                drawList.push_back(packet);
            }
        }
    }

    for (auto& child : node->children) {
        collectDrawCalls(child.get(), globalTransform, model, drawList);
    }
}

void VkCommandManager::createCommandBuffers(int count) {
	commandBuffers.resize(count);

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = context->getCommandPool();
	// Primary means can be submitted to a queue directly. Secondary means called from other buffers.
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();

	if (vkAllocateCommandBuffers(context->getDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate command buffers!");
	}
}

VkCommandBuffer VkCommandManager::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = this->commandPool; // Now uses the valid class member
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(context->getDevice(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VkCommandManager::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);

    // CRITICAL: Prevent the nvoglv64.dll crash by waiting for the GPU to finish
    vkQueueWaitIdle(context->getGraphicsQueue());

    vkFreeCommandBuffers(context->getDevice(), commandPool, 1, &commandBuffer);
}

void VkCommandManager::recordCommands(
    uint32_t currentFrame,
    uint32_t imageIndex,
    VkRenderPassWrapper* offscreenPass,
    VkFramebufferWrapper* offscreenFramebuffer,
    VkRenderPass lightingRenderPass,
    VkFramebuffer lightingFramebuffer,
    VkLightingPipeline* lightingPipeline,
    VkDescriptorSet lightingDescriptorSet,
    glm::vec3 cameraPos,
    glm::mat4 view,
    glm::mat4 proj,
    VkUIRenderPass* uiPass,
    const std::vector<VkFramebuffer>& uiFramebuffers,
    VkGraphicsPipeline* pipeline,
    VkExtent2D extent,
    Registry& registry,
    VkDescriptorSet globalDescriptorSet,
    EditorSystem* editor,
    VkForwardRenderPass* forwardPass,
    VkForwardPipeline* forwardPipeline,
    const std::vector<VkFramebuffer>& forwardFramebuffers,
    VkImage litSceneImage,
    VkImage opaqueSceneCopy,
    VkRenderPass glassDepthRenderPass,
    VkFramebuffer glassDepthFramebuffer,
    Iridium::GlassDepthPipeline* glassDepthPipeline) {

    VkCommandBuffer cmd = commandBuffers[currentFrame];
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    // === PASS 1: OFF-SCREEN 3D SCENE ===
    // This draws the game world into your invisible sceneColorImageViews
    VkRenderPassBeginInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpInfo.renderPass = offscreenPass->getRenderPass();
    rpInfo.framebuffer = offscreenFramebuffer->getFramebuffer(imageIndex); //
    rpInfo.renderArea.extent = extent;

    // Update this array to have 4 elements instead of 2!
    std::array<VkClearValue, 4> clearValues{};

    // Background color for Position (Black)
    clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };

    // Background color for Normals (Black)
    clearValues[1].color = { {0.0f, 0.0f, 0.0f, 1.0f} };

    // Background color for Albedo (This is the actual "sky" color of your viewport)
    clearValues[2].color = { {0.1f, 0.1f, 0.1f, 1.0f} };

    // Depth clear value
    clearValues[3].depthStencil = { 1.0f, 0 };

    rpInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    rpInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 1. Set Viewport & Scissor for the Scene
    VkViewport viewport{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{ {0, 0}, extent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkPipelineLayout layout = pipeline->getPipelineLayout();

    // 2. Bind Global Camera Data (Set 0)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &globalDescriptorSet, 0, nullptr);

    // 3. Select Render Mode (Wireframe vs Solid)
    if (editor && editor->currentRenderMode == 1) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getWireframePipeline());
    }
    else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getPipeline());
    }

    // 4. ECS RENDER LOOP (Draw all meshes)
    auto* meshPool = registry.getPool<MeshComponent>();
    auto* transformPool = registry.getPool<TransformComponent>();

    for (uint32_t entity : meshPool->entities) {
        if (transformPool->sparseMap.contains(entity)) {
            auto& meshComp = meshPool->get(entity);
            if (!meshComp.enabled || !meshComp.model) continue;

            auto& transformComp = transformPool->get(entity);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &meshComp.model->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, meshComp.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            for (const auto& subMesh : meshComp.model->subMeshes) {
                auto& mat = meshComp.model->materials[subMesh.materialIndex];

                // THE FILTER: Do NOT draw glass into the G-Buffer!
                if (mat.alphaMode == AlphaMode::Blend) {
                    continue; // Skip it! We will draw this later in the Forward Pass!
                }

                // Construct the struct and pass both the matrix and the material color
                MeshPushConstants push{};
                push.renderMatrix = transformComp.worldMatrix;
                push.baseColor = mat.baseColor;
                push.metallicFactor = mat.metallicFactor;
                push.roughnessFactor = mat.roughnessFactor;

                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 
                    0, sizeof(MeshPushConstants), &push);
                if (!mat.descriptorSets.empty()) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                        &mat.descriptorSets[currentFrame], 0, nullptr);
                }
                else {
                    // THE PROOF: 
                    std::cout << "[DEBUG WARNING] Submesh " << subMesh.materialIndex
                        << " has EMPTY descriptor sets! Driver is rendering Light Grey!\n";
                }
                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
            }
        }
    }

    // 5. SELECTION OUTLINE logic
    if (editor && editor->getSelectedEntity() != NULL_ENTITY) {
        Entity selected = editor->getSelectedEntity();
        if (meshPool->has(selected) && transformPool->has(selected) &&
            meshPool->get(selected).enabled && meshPool->get(selected).model) {

            if (editor->currentRenderMode == 1) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getOutlineWireframePipeline());
            }
            else {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getOutlinePipeline());
            }

            auto& meshComp = meshPool->get(selected);
            auto& transformComp = transformPool->get(selected);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &meshComp.model->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, meshComp.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            MeshPushConstants push{};
            push.renderMatrix = transformComp.worldMatrix;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 
                0, sizeof(MeshPushConstants), &push);

            for (const auto& subMesh : meshComp.model->subMeshes) {
                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
            }
        }
    }

    vkCmdEndRenderPass(cmd); // END OF PASS 1

    // =======================================================
    // === PASS 2: DEFERRED LIGHTING PASS                  ===
    // =======================================================
    VkRenderPassBeginInfo lightingPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    lightingPassInfo.renderPass = lightingRenderPass;
    lightingPassInfo.framebuffer = lightingFramebuffer;
    lightingPassInfo.renderArea.extent = extent;

    VkClearValue lightingClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
    lightingPassInfo.clearValueCount = 1;
    lightingPassInfo.pClearValues = &lightingClearColor;

    vkCmdBeginRenderPass(cmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 1. Bind the Empty-Vertex Pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipeline());

    // 2. Bind the G-Buffer (Position, Normal, Albedo) to the fragment shader
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipeline->getPipelineLayout(),
        0, 1, &lightingDescriptorSet, 0, nullptr);

    // 3. Pass the Camera Position via Push Constants
    LightingPushConstants push{};
    push.viewPos = glm::vec4(cameraPos, 1.0f);
    push.invView = glm::inverse(view);
    push.invProj = glm::inverse(proj);
    vkCmdPushConstants(cmd, lightingPipeline->getPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(LightingPushConstants), &push);

    // 4. THE MAGIC TRICK: Draw 3 vertices with no vertex buffer!
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd); // END OF PASS 2

    // =======================================================
    // === VRAM PHOTOGRAPH: COPY LIT SCENE FOR REFRACTION  ===
    // =======================================================

    // 1. Transition Lit Scene to TRANSFER_SRC and Copy to TRANSFER_DST
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
    copyDstBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    copyDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyDstBarrier.image = opaqueSceneCopy;
    copyDstBarrier.srcAccessMask = 0;
    copyDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier copyBarriers[] = { litSrcBarrier, copyDstBarrier };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, copyBarriers);

    // 2. Perform the blazing-fast VRAM-to-VRAM copy 
    VkImageCopy imageCopyRegion{};
    imageCopyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    imageCopyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    imageCopyRegion.extent = { extent.width, extent.height, 1 };

    vkCmdCopyImage(cmd,
        litSceneImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        opaqueSceneCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &imageCopyRegion);

    // 3. Transition Lit Scene BACK to COLOR_ATTACHMENT and Copy to SHADER_READ_ONLY
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
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, endBarriers);

    // =======================================================
    // === PASS 2.5: GLASS DEPTH PASS                      ===
    // =======================================================
    VkRenderPassBeginInfo glassDepthPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    glassDepthPassInfo.renderPass = glassDepthRenderPass;
    glassDepthPassInfo.framebuffer = glassDepthFramebuffer;
    glassDepthPassInfo.renderArea.extent = extent;

    VkClearValue depthClearValue{};
    depthClearValue.depthStencil = { 1.0f, 0 };
    glassDepthPassInfo.clearValueCount = 1;
    glassDepthPassInfo.pClearValues = &depthClearValue;

    vkCmdBeginRenderPass(cmd, &glassDepthPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 1. Bind the depth-only pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassDepthPipeline->getPipeline());

    // 2. Set Viewport & Scissor (Re-use the ones from Pass 1)
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &globalDescriptorSet, 0, nullptr);

    // 3. ECS RENDER LOOP (Draw ONLY glass meshes into the depth buffer)
    if (meshPool && transformPool) {
        for (uint32_t entity : meshPool->entities) {
            if (transformPool->sparseMap.contains(entity)) {
                auto& meshComp = meshPool->get(entity);
                if (!meshComp.enabled || !meshComp.model) continue;

                auto& transformComp = transformPool->get(entity);

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &meshComp.model->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(cmd, meshComp.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                for (const auto& subMesh : meshComp.model->subMeshes) {
                    auto& mat = meshComp.model->materials[subMesh.materialIndex];

                    // THE FILTER: ONLY draw glass into the glass depth buffer!
                    if (mat.alphaMode != AlphaMode::Blend) {
                        continue;
                    }

                    // Push Constants (We only need the matrix for depth, no color/roughness!)
                    MeshPushConstants push{};
                    push.renderMatrix = transformComp.worldMatrix;

                    vkCmdPushConstants(cmd, layout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);

                    vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
                }
            }
        }
    }

    vkCmdEndRenderPass(cmd); // END OF PASS 2.5

    // =======================================================
        // === PASS 3: FORWARD TRANSLUCENCY PASS               ===
        // =======================================================
    VkRenderPassBeginInfo forwardPassInfo{};
    forwardPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    forwardPassInfo.renderPass = forwardPass->getRenderPass();
    forwardPassInfo.framebuffer = forwardFramebuffers[imageIndex];
    forwardPassInfo.renderArea.offset = { 0, 0 };
    forwardPassInfo.renderArea.extent = extent; // Uses the passed-in extent!
    forwardPassInfo.clearValueCount = 0;
    forwardPassInfo.pClearValues = nullptr;

    vkCmdBeginRenderPass(cmd, &forwardPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipeline->getPipeline());

    // SET 0: Bind Global UBO (Camera Data)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        forwardPipeline->getPipelineLayout(), 0, 1, &globalDescriptorSet, 0, nullptr);

    // SET 2: Bind Lighting UBO (HDRI Map for Reflections!)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        forwardPipeline->getPipelineLayout(), 2, 1, &lightingDescriptorSet, 0, nullptr);

    // Custom ECS Loop for Iridium Engine
    if (meshPool && transformPool) {
        for (uint32_t entity : meshPool->entities) {
            if (transformPool->sparseMap.contains(entity)) {
                auto& meshComp = meshPool->get(entity);
                if (!meshComp.enabled || !meshComp.model) continue;

                auto& transformComp = transformPool->get(entity);

                // Bind Vertex/Index buffers
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &meshComp.model->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(cmd, meshComp.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                for (const auto& subMesh : meshComp.model->subMeshes) {
                    auto& mat = meshComp.model->materials[subMesh.materialIndex];

                    // THE FILTER: ONLY draw glass!
                    if (mat.alphaMode != AlphaMode::Blend) {
                        continue;
                    }

                    // Push Constants
                    MeshPushConstants push{};
                    push.renderMatrix = transformComp.worldMatrix;
                    push.baseColor = mat.baseColor;
                    push.metallicFactor = mat.metallicFactor;
                    push.roughnessFactor = mat.roughnessFactor;

                    vkCmdPushConstants(cmd, forwardPipeline->getPipelineLayout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants), &push);

                    // SET 1: Bind Material Descriptors
                    if (!mat.descriptorSets.empty()) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            forwardPipeline->getPipelineLayout(), 1, 1, &mat.descriptorSets[currentFrame], 0, nullptr);
                    }

                    vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
                }
            }
        }
    }

    vkCmdEndRenderPass(cmd); // END OF PASS 3

    // === PASS 4: UI SWAPCHAIN ===
    // This takes the editor UI (which now samples from Pass 1) and draws it to the monitor
    VkRenderPassBeginInfo uiPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    uiPassInfo.renderPass = uiPass->getRenderPass();
    uiPassInfo.framebuffer = uiFramebuffers[imageIndex]; // The OS window framebuffers
    uiPassInfo.renderArea.extent = extent;

    VkClearValue uiClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
    uiPassInfo.clearValueCount = 1;
    uiPassInfo.pClearValues = &uiClearColor;

    vkCmdBeginRenderPass(cmd, &uiPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // DRAW THE EDITOR UI HERE
    if (editor) {
        editor->render(cmd);
    }

    vkCmdEndRenderPass(cmd); // END OF PASS 2

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer!");
    }
}

void VkCommandManager::transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands(); // Uses your existing helper

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) ? 
        VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
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
    endSingleTimeCommands(commandBuffer);
}

void VkCommandManager::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    endSingleTimeCommands(commandBuffer);
}

