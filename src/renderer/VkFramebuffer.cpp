#include "VkFramebuffer.h"
#include <stdexcept>
#include <array>

VkFramebufferWrapper::VkFramebufferWrapper(VkContext* context, VkSwapchain* swapchain, 
	VkRenderPassWrapper* renderPass, VkImageView depthImageView) : context(context) {
	createFramebuffers(swapchain, renderPass, depthImageView);
}

VkFramebufferWrapper::~VkFramebufferWrapper(){
	for (auto framebuffer : framebuffers) {
		vkDestroyFramebuffer(context->getDevice(), framebuffer, nullptr);
	}
}

void VkFramebufferWrapper::createFramebuffers(VkSwapchain* swapchain,
    VkRenderPassWrapper* renderPass, VkImageView depthImageView) {
    const std::vector<VkImageView>& swapChainImageViews = swapchain->getImageViews();
    framebuffers.resize(swapChainImageViews.size());

    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        // Order must match the Render Pass! 
        // Index 0: Color, Index 1: Depth
        std::array<VkImageView, 2> attachments = {
            swapChainImageViews[i],
            depthImageView
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass->getRenderPass();

        // Attachment count is now 2
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();

        framebufferInfo.width = swapchain->getExtent().width;
        framebufferInfo.height = swapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(context->getDevice(), &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create framebuffer!");
        }
    }
}