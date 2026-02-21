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
    VkUIRenderPass* uiPass,
    const std::vector<VkFramebuffer>& uiFramebuffers,
    VkGraphicsPipeline* pipeline,
    VkExtent2D extent,
    Registry& registry,
    VkDescriptorSet globalDescriptorSet,
    EditorSystem* editor) {

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

    std::array<VkClearValue, 2> clears{};
    clears[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
    clears[1].depthStencil = { 1.0f, 0 };
    rpInfo.clearValueCount = 2;
    rpInfo.pClearValues = clears.data();

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

            // Push World Matrix
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &transformComp.worldMatrix);

            for (const auto& subMesh : meshComp.model->subMeshes) {
                auto& mat = meshComp.model->materials[subMesh.materialIndex];
                if (!mat.descriptorSets.empty()) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                        &mat.descriptorSets[currentFrame], 0, nullptr);
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
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &transformComp.worldMatrix);

            for (const auto& subMesh : meshComp.model->subMeshes) {
                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
            }
        }
    }

    vkCmdEndRenderPass(cmd); // END OF PASS 1


    // === PASS 2: UI SWAPCHAIN ===
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

