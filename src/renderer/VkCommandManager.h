#pragma once

#include "VkContext.h"
#include "VkFramebuffer.h"
#include "VkGraphicsPipeline.h"

class VkCommandManager {
public: 
	VkCommandManager(VkContext* context, VkFramebufferWrapper* framebuffer, VkGraphicsPipeline* pipeline);
	~VkCommandManager();

	// The function that writes the commands into the command buffer
	void recordCommands(VkRenderPassWrapper* renderPass, VkFramebufferWrapper* framebuffer, 
		VkGraphicsPipeline* pipeline, VkExtent2D extent);

	//Getter
	VkCommandBuffer& getCommandBuffer(int index)  { return commandBuffers[index]; }

private:
	VkContext* context;
	std::vector<VkCommandBuffer> commandBuffers;

	void createCommandBuffers(int count);
};