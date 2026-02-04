#pragma once

#include <vulkan/vulkan.h>
#include "VkContext.h"
#include "VkSwapchain.h"

class VkRenderPassWrapper {
public:
	VkRenderPassWrapper(VkContext* context, VkSwapchain* swapchain);
	~VkRenderPassWrapper();

	VkRenderPass getRenderPass() const { return renderPass; }
private:
	VkContext* context;
	VkRenderPass  renderPass;

	void createRenderPass(VkFormat swapChainImageFormat);
};