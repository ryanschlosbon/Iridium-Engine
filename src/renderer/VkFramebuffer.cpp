#include "VkFramebuffer.h"
#include <stdexcept>

VkFramebufferWrapper::VkFramebufferWrapper(VkContext* context, VkSwapchain* swapchain, 
	VkRenderPassWrapper* renderPass) : context(context) {
	createFramebuffers(swapchain, renderPass);
}

VkFramebufferWrapper::~VkFramebufferWrapper(){
	for (auto framebuffer : framebuffers) {
		vkDestroyFramebuffer(context->getDevice(), framebuffer, nullptr);
	}
}

void VkFramebufferWrapper::createFramebuffers(VkSwapchain* swapchain, VkRenderPassWrapper* renderPass) {
	// Get the list of Image Views
	const std::vector<VkImageView>& swapChainImageViews = swapchain->getImageViews();

	framebuffers.resize(swapChainImageViews.size());

	// Loop through every image and create a framebuffer for it
	for (size_t i = 0; i < swapChainImageViews.size(); i++) {
		// The attachment is the specific image view we are rendering to
		VkImageView attachments[] = {
			swapChainImageViews[i]
		};

		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;

		// Link to the Instruction Set (Render Pass)
		framebufferInfo.renderPass = renderPass->getRenderPass();

		// Link to the Paper (Attachments)
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = attachments;

		// Size much match the swapchain size
		framebufferInfo.width = swapchain->getExtent().width;
		framebufferInfo.height = swapchain->getExtent().height;
		framebufferInfo.layers = 1; // Always 1 unless using VR

		if (vkCreateFramebuffer(context->getDevice(), &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
			throw std::runtime_error("failed to create framebuffer!");
		}
	}
}