#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include "VkContext.h"
#include "VkSwapchain.h"
#include "VkRenderPass.h"

class VkFramebufferWrapper {
public:
	VkFramebufferWrapper(VkContext* context, VkSwapchain* swapchain, VkRenderPassWrapper* renderPass);
	~VkFramebufferWrapper();

	VkFramebuffer getFramebuffer(int index) const { return framebuffers[index]; }
private:
	VkContext* context;
	std::vector<VkFramebuffer> framebuffers;

	void createFramebuffers(VkSwapchain* swapchain, VkRenderPassWrapper* renderPass);
};