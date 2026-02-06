#pragma once

#include "VkContext.h"
#include "VkFramebuffer.h"
#include "VkGraphicsPipeline.h"
#include "VkMesh.h"

class VkCommandManager {
public: 
	VkCommandManager(VkContext* context, VkFramebufferWrapper* framebuffer, 
		VkGraphicsPipeline* pipeline, int count);
	~VkCommandManager();

	// The function that writes the commands into the command buffer
	void recordCommands(uint32_t imageIndex, VkRenderPassWrapper* renderPass, VkFramebufferWrapper* framebuffer,
		VkGraphicsPipeline* pipeline, VkExtent2D extent, VkBuffer vertexBuffer,
		VkBuffer indexBuffer, uint32_t indexCount, MeshPushConstants constants,
		const std::vector<VkDescriptorSet>& descriptorSets);

	// Getter
	VkCommandBuffer& getCommandBuffer(int index)  { return commandBuffers[index]; }

private:
	VkContext* context;
	std::vector<VkCommandBuffer> commandBuffers;

	void createCommandBuffers(int count);
};