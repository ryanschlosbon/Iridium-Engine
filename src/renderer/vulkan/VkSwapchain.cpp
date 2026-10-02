#include "VkSwapchain.h"
#include <limits>
#include <algorithm>
#include <stdexcept>

VkSwapchain::VkSwapchain(VkContext* context, GLFWwindow* window,
	Iridium::Color::OutputTransport requestedTransport,
	VkSwapchainKHR oldSwapchain) : context(context), requestedTransport_(requestedTransport) {
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
	supportedOutputTransports.clear();
	for (const Iridium::Color::OutputTransport transport : {
		Iridium::Color::OutputTransport::SdrSrgb,
		Iridium::Color::OutputTransport::ScRgb,
		Iridium::Color::OutputTransport::Hdr10Pq }) {
		try {
			const auto candidate = Iridium::selectVulkanOutputTransport(
				transport, availableFormats, context->hasHdrMetadata());
			if (!candidate.usedSdrFallback && candidate.effective == transport) {
				supportedOutputTransports.push_back(transport);
			}
		}
		catch (const std::runtime_error&) {
			// The actual mandatory SDR selection below supplies the fatal diagnostic.
		}
	}
	outputTransportSelection = Iridium::selectVulkanOutputTransport(
		requestedTransport_,
		availableFormats, context->hasHdrMetadata());
	return outputTransportSelection.surfaceFormat;
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
	swapChainColorSpace = surfaceFormat.colorSpace;
	swapChainPresentMode = presentMode;
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

void VkSwapchain::setHdrMetadata(float peakNits) const {
    if (!context->hasHdrMetadata() ||
        outputTransportSelection.effective !=
            Iridium::Color::OutputTransport::Hdr10Pq) {
        return;
    }
    const auto setMetadata = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
        vkGetDeviceProcAddr(context->getDevice(), "vkSetHdrMetadataEXT"));
    if (setMetadata == nullptr) {
        throw std::runtime_error(
            "VK_EXT_hdr_metadata was enabled but vkSetHdrMetadataEXT is unavailable.");
    }
    VkHdrMetadataEXT metadata{ VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    metadata.displayPrimaryRed = { 0.680f, 0.320f };
    metadata.displayPrimaryGreen = { 0.265f, 0.690f };
    metadata.displayPrimaryBlue = { 0.150f, 0.060f };
    metadata.whitePoint = { 0.3127f, 0.3290f };
    metadata.maxLuminance = peakNits;
    metadata.minLuminance = 0.0f;
    metadata.maxContentLightLevel = 0.0f;
    metadata.maxFrameAverageLightLevel = 0.0f;
    setMetadata(context->getDevice(), 1, &swapChain, &metadata);
}
