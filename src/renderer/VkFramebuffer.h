#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include "VkContext.h"
#include "VkSwapchain.h"
#include "VkRenderPass.h"

class VkFramebufferWrapper {
public:
	VkFramebufferWrapper(VkContext* context, VkSwapchain* swapchain, VkRenderPassWrapper* renderPass, 
		VkImageView depthImageView);
	~VkFramebufferWrapper();

	VkFramebuffer getFramebuffer(int index) const { return framebuffers[index]; }
	void createFramebuffers(VkSwapchain* swapchain,
		VkRenderPassWrapper* renderPass,
		VkImageView depthImageView);
private:
	VkContext* context;
	std::vector<VkFramebuffer> framebuffers;
};