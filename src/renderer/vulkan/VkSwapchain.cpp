#include "VkSwapchain.h"
#include <limits>
#include <algorithm>
#include <stdexcept>

VkSwapchain::VkSwapchain(VkContext* context, GLFWwindow* window, VkSwapchainKHR oldSwapchain) : context(context) {
	createSwapchain(window, oldSwapchain);
	createImageViews();
}

VkSwapchain::~VkSwapchain() {
	// Destroy Image Views
	for (auto imageView : swapChainImageViews) {
		vkDestroyImageView(context->getDevice(), imageView, nullptr);
	}

	// Destroy Swapchain
	vkDestroySwapchainKHR(context->getDevice(), swapChain, nullptr);
}

// Choose the Color Format
VkSurfaceFormatKHR VkSwapchain::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
	for (const auto& availableFormat : availableFormats) {
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
			availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return availableFormat;
		}
	}
	return availableFormats[0];
}

// Choose the Sync Mode
VkPresentModeKHR VkSwapchain::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
	// Search for Mailbox (Triple Buffering, Low Latency, No Tearing)
	for (const auto& availablePresentMode : availablePresentModes) {
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			return availablePresentMode;
		}
	}
	// Fallback to FIFO if Mailbox not supported
	return VK_PRESENT_MODE_FIFO_KHR;
}

// Choose the Resolution
VkExtent2D VkSwapchain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window) {
	// If Vulkan already knows the window size, use it
	if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
		return capabilities.currentExtent;
	}
	else {
		// If Vulkan returns "MAX_UINT", it means we can set the resolution ourselves, as it doesn't know it yet.
		// So we ask GLFW for the actual window size.
		int width, height;
		glfwGetFramebufferSize(window, &width, &height);

		VkExtent2D actualExtent = {
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height)
		};

		// Clamp the size so we don't try to create a swapchain bigger than the GPU supports
		actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width,
			capabilities.maxImageExtent.width);
		actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, 
			capabilities.maxImageExtent.height);

		return actualExtent;
	}
}

void VkSwapchain::createSwapchain(GLFWwindow* window, VkSwapchainKHR oldSwapchain) {
	// Query GPU details
	SwapChainSupportDetails swapChainSupport = context->querySwapChainSupport(context->getPhysicalDevice());

	VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
	VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
	VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, window);

	// Decide how many images to create
	// We are using triple buffering so we need atleast 3
	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;

	// Make sure we don't exceed the maximum
	if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
		imageCount = swapChainSupport.capabilities.maxImageCount;
	}

	// Configure the Swapchain Structure
	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = context->getSurface();

	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;	// Always 1 unless using VR
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;	// Rendering color to it

	// Handle Queue Families
	// If Graphics Queue and Present Queue are different, we need "Concurrent" mode
	// If they are the same (most GPUs), we use "Exclusive" mode (faster)
	VkQueue graphicsQueue = context->getGraphicsQueue();
	VkQueue presentQueue = context->getPresentQueue();

	// We need to access the indices from Context to compare them
	// In standard PCs, they are the same, so we will assume Exclusive mode.
	createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	createInfo.queueFamilyIndexCount = 0;	// Optional
	createInfo.pQueueFamilyIndices = nullptr;	// Optional

	createInfo.preTransform = swapChainSupport.capabilities.currentTransform;	// No rotation
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;	// Ignore window alpha channel
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;	// Don't render pixels hidden behind other windows

	createInfo.oldSwapchain = oldSwapchain;

	if (vkCreateSwapchainKHR(context->getDevice(), &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
		throw std::runtime_error("failed to create swap chain!");
	}

	// Retrieve the Images
	// The driver created them, we are just pulling the handles (pointers)
	vkGetSwapchainImagesKHR(context->getDevice(), swapChain, &imageCount, nullptr);
	swapChainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(context->getDevice(), swapChain, &imageCount, swapChainImages.data());

	// Save settings for later
	swapChainImageFormat = surfaceFormat.format;
	swapChainExtent = extent;
}

void VkSwapchain::createImageViews() {
	swapChainImageViews.resize(swapChainImages.size());

	for (size_t i = 0; i < swapChainImages.size(); i++) {
		VkImageViewCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		createInfo.image = swapChainImages[i];

		// Treat it as a standard 2D texture
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format = swapChainImageFormat;

		// Default mapping (r->r, g->g, b->b, etc.)
		createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		// How many layers (just 1, no mipmaps yet)
		createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.levelCount = 1;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(context->getDevice(), &createInfo, nullptr, &swapChainImageViews[i]) != VK_SUCCESS) {
			throw std::runtime_error("failed to create image views!");
		}
	}
}
