#include "VkRenderPass.h"
#include <stdexcept>
#include <array>

VkRenderPassWrapper::VkRenderPassWrapper(VkContext* context, VkSwapchain* swapchain) : context(context) {
	createRenderPass(swapchain->getImageFormat());
}

VkRenderPassWrapper::~VkRenderPassWrapper() {
	vkDestroyRenderPass(context->getDevice(), renderPass, nullptr);
}

void VkRenderPassWrapper::createRenderPass(VkFormat swapChainImageFormat) {
	// Attachment Description
	// This describes what we are drawing on
	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = swapChainImageFormat; // Format must match the swapchain
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT; // No AA yet

	// What to do at the start and end of the frame
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // Clear the framebuffer at the start
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // Store the framebuffer contents to memory at end

	// Layout Transitions
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Don't care about previous layout
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Ready for presentation

	// Subpass Reference
	// A render pass can have multiple subpasses (e.g. Geometry -> Lighting)
	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0; // The index of the attachment in the array below
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // Optimal layout for color attachment

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; // Graphics pipeline
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef; // Reference to the color attachment

	// Subpass Dependency (Synchronization)
	// Wait for the Swapchain to give us an image before we draw
	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;	// Anything outside this pass
	dependency.dstSubpass = 0; // Our subpass

	// Wait at the "Color Output" stage
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0; 

	// Block the "Color Output" stage until we are ready
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; // Wait until we can write to the color attachment

	// Create the Render Pass
	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments = &colorAttachment;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	if (vkCreateRenderPass(context->getDevice(), &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
		throw std::runtime_error("failed to create render pass!");
	}
}