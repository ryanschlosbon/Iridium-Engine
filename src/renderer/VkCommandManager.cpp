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
    const std::vector<RenderObject>& renderQueue, const std::vector<VkDescriptorSet>& globalSets,
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

    // 2. Bind Pipeline & Dynamic State
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getPipeline());

    VkViewport viewport{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{ {0, 0}, extent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkPipelineLayout layout = pipeline->getPipelineLayout();

    // 3. GLOBAL CAMERA (Set 0) - Bind ONCE per frame
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &globalSets[imageIndex], 0, nullptr);

    // 4. RENDER LOOP (The Optimized Batch)
    for (const auto& obj : renderQueue) {
        // A. Bind VBO/IBO once for the entire car
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &obj.model->vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, obj.model->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Pass the Entity's world transform (like car position)
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &obj.transform);

        // 3. Loop through ONLY the merged "Super Meshes"
        for (const auto& subMesh : obj.model->subMeshes) {
            auto& mat = obj.model->materials[subMesh.materialIndex];

            // Bind texture once per material type
            if (!mat.descriptorSets.empty()) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &mat.descriptorSets[imageIndex], 0, nullptr);
            }

            // ONE draw call for all "Chrome" parts, ONE for all "Rubber", etc.
            vkCmdDrawIndexed(cmd, subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
        }
    }

    // 5. Editor UI
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

