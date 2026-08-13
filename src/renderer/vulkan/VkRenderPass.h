#pragma once

#include <vulkan/vulkan.h>
#include "VkContext.h"
#include "VkSwapchain.h"
#include "renderer/rhi/GBufferLayout.h"

class VkRenderPassWrapper {
public:
	VkRenderPassWrapper(VkContext* context, VkSwapchain* swapchain,
        Iridium::GBufferLayout layout);
	~VkRenderPassWrapper();

	VkRenderPass getRenderPass() const { return renderPass; }
private:
	VkContext* context;
	VkRenderPass  renderPass;

	void createRenderPass(Iridium::GBufferLayout layout);
};
