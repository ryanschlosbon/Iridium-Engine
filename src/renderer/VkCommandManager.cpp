#include "VkCommandManager.h"
#include "assets/AssetManager.h"
#include <stdexcept>
#include <iostream> // Add this at the top of the file

VkCommandManager::VkCommandManager(VkContext* context, VkFramebufferWrapper* framebuffer, 
	VkGraphicsPipeline* pipeline, int count)
	: context(context) {
	// Create 1 buffer for every frame (usually 3)
	createCommandBuffers(count);
}

VkCommandManager::~VkCommandManager() {
	// We don't need to explicitly free command buffers because they are destroyed with the Pool (in VkContext)
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

    // FIX: Use the valid pool from your context
    allocInfo.commandPool = context->getCommandPool();
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
    vkQueueWaitIdle(context->getGraphicsQueue());

    // FIX: Use the valid pool from your context
    vkFreeCommandBuffers(context->getDevice(), context->getCommandPool(), 1, &commandBuffer);
}

void VkCommandManager::recordCommands(uint32_t imageIndex, VkRenderPassWrapper* renderPass,
    VkFramebufferWrapper* framebuffer, VkGraphicsPipeline* pipeline, VkExtent2D extent,
    Registry& registry, // <--- USING REGISTRY
    const std::vector<VkDescriptorSet>& globalSets,
    EditorSystem* editor) {

    VkCommandBuffer cmd = commandBuffers[imageIndex];
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    // 1. Begin Render Pass
    VkRenderPassBeginInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpInfo.renderPass = renderPass->getRenderPass();
    rpInfo.framebuffer = framebuffer->getFramebuffer(imageIndex);
    rpInfo.renderArea.extent = extent;

    std::array<VkClearValue, 2> clears{};
    clears[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
    clears[1].depthStencil = { 1.0f, 0 };
    rpInfo.clearValueCount = 2;
    rpInfo.pClearValues = clears.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 2. Bind Pipeline & Global State
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getPipeline());

    VkViewport viewport{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{ {0, 0}, extent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkPipelineLayout layout = pipeline->getPipelineLayout();

    // Bind Global Camera Data (Set 0)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &globalSets[imageIndex], 0, nullptr);

    // MODE 1: WIREFRAME
    if (editor && editor->currentRenderMode == 1) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getWireframePipeline());
    }
    // MODE 0: STANDARD (Default)
    else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getPipeline());
    }

    // 3. ECS RENDER LOOP
    // Get the pools directly (since your Registry doesn't have .view())
    auto* meshPool = registry.getPool<MeshComponent>();
    auto* transformPool = registry.getPool<TransformComponent>();

    // Iterate through all entities that have a MESH
    // (We iterate meshes because we only want to draw things that exist visibly)
    for (uint32_t entity : meshPool->entities) {

        // Check: Does this mesh entity ALSO have a Transform?
        // (We can't draw it if we don't know where it is)
        if (transformPool->sparseMap.contains(entity)) {

            // A. Retrieve the live components
            auto& meshComp = meshPool->get(entity);

            if (!meshComp.enabled || !meshComp.model) continue; // Skip disabled or invalid meshes

            auto& transformComp = transformPool->get(entity);

            // B. Bind VBO/IBO (Mesh Data)
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &meshComp.model->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, meshComp.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            // C. Push the CALCULATED World Matrix (From TransformSystem)
            // This 'worldMatrix' was updated by TransformSystem::update() right before this function ran
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &transformComp.worldMatrix);

            // D. Draw the Submeshes (Materials)
            for (const auto& subMesh : meshComp.model->subMeshes) {
                auto& mat = meshComp.model->materials[subMesh.materialIndex];

                // Bind Material Textures (Set 1)
                if (!mat.descriptorSets.empty()) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &mat.descriptorSets[imageIndex], 0, nullptr);
                }

                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
            }
        }
    }

    if (editor && editor->getSelectedEntity() != NULL_ENTITY) {
        Entity selected = editor->getSelectedEntity();

        auto* meshPool = registry.getPool<MeshComponent>();
        auto* transformPool = registry.getPool<TransformComponent>();

        // OPTIMIZED: Use the ECS O(1) lookup instead of a for-loop, 
        // and safely check if the model is loaded and enabled.
        if (meshPool->has(selected) && transformPool->has(selected) &&
            meshPool->get(selected).enabled && meshPool->get(selected).model) {

            // 1. Switch to Outline Pipeline (Green, Cull Front)
            if (editor->currentRenderMode == 1) {
                // If in Wireframe Mode -> Use Wireframe Outline (The Green Cage)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getOutlineWireframePipeline());
            }
            else {
                // If in Solid Mode -> Use Solid Outline (The Green Shell)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getOutlinePipeline());
            }

            auto& meshComp = meshPool->get(selected);
            auto& transformComp = transformPool->get(selected);

            // 2. Bind Mesh (Same VBO/IBO)
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &meshComp.model->vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, meshComp.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            // 3. Push Transform (Same Matrix)
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &transformComp.worldMatrix);

            // 4. Draw (No materials needed, the shader is solid green)
            for (const auto& subMesh : meshComp.model->subMeshes) {
                vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
            }
        }
    }

    // 4. Editor UI
    if (editor) {
        editor->render(cmd);
    }

    vkCmdEndRenderPass(cmd);

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

