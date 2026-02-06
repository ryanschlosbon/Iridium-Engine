#include "VkCommandManager.h"
#include <stdexcept>

VkCommandManager::VkCommandManager(VkContext* context, VkFramebufferWrapper* framebuffer, 
	VkGraphicsPipeline* pipeline, int count)
	: context(context) {
	// Create 1 buffer for every frame (usually 3)
	createCommandBuffers(count);
}

VkCommandManager::~VkCommandManager() {
	// We don't need to explicitly free command buffers because they are destroyed with the Pool (in VkContext)
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

void VkCommandManager::recordCommands(uint32_t imageIndex, VkRenderPassWrapper* renderPass, VkFramebufferWrapper* framebuffer,
    VkGraphicsPipeline* pipeline, VkExtent2D extent, VkBuffer vertexBuffer,
    VkBuffer indexBuffer, uint32_t indexCount, MeshPushConstants constants,
    const std::vector<VkDescriptorSet>& descriptorSets) {

    // 1. Start recording the specific buffer for this frame
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffers[imageIndex], &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }

    // 2. Configure the Render Pass
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass->getRenderPass();
    renderPassInfo.framebuffer = framebuffer->getFramebuffer(static_cast<int>(imageIndex));
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = extent;

    // Clear Color (Black Background)
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Clear Color
    clearValues[1].depthStencil = { 1.0f, 0 };          // Clear Depth to 1.0

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    // --- START RENDERING ---
    vkCmdBeginRenderPass(commandBuffers[imageIndex], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffers[imageIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->getPipeline());

    // Bind the specific Descriptor Set for this frame (UBO/Matrices)
    vkCmdBindDescriptorSets(commandBuffers[imageIndex], VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline->getPipelineLayout(), 0, 1, &descriptorSets[imageIndex], 0, nullptr);

    // Upload Push Constants (Scale/Offset)
    vkCmdPushConstants(
        commandBuffers[imageIndex],
        pipeline->getPipelineLayout(),
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(MeshPushConstants),
        &constants
    );

    // Bind Geometry Buffers
    VkBuffer vertexBuffers[] = { vertexBuffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffers[imageIndex], 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffers[imageIndex], indexBuffer, 0, VK_INDEX_TYPE_UINT16);

    // Draw the Indexed Square
    vkCmdDrawIndexed(commandBuffers[imageIndex], indexCount, 1, 0, 0, 0);

    vkCmdEndRenderPass(commandBuffers[imageIndex]);
    // --- END RENDERING ---

    if (vkEndCommandBuffer(commandBuffers[imageIndex]) != VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer!");
    }
}