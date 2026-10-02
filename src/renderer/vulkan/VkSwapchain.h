#pragma once

#include "VkContext.h"
#include "renderer/vulkan/VulkanOutputTransport.h"
#include <vector>
#include <vulkan/vulkan.h>

class VkSwapchain {
	public:
		// The Constructor
		VkSwapchain(VkContext* context, GLFWwindow* window,
			Iridium::Color::OutputTransport requestedTransport =
				Iridium::Color::OutputTransport::SdrSrgb,
			VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
		~VkSwapchain();

		// Getters for the raw handles
		VkSwapchainKHR getSwapchain() const { return swapChain; }
		VkFormat getImageFormat() const { return swapChainImageFormat; }
		VkColorSpaceKHR getColorSpace() const { return swapChainColorSpace; }
		VkPresentModeKHR getPresentMode() const { return swapChainPresentMode; }
		VkExtent2D getExtent() const { return swapChainExtent; }
		const std::vector<VkImageView>& getImageViews() const { return swapChainImageViews; }
		uint32_t getImageCount() const { return static_cast<uint32_t>(swapChainImages.size()); }
        void setHdrMetadata(float peakNits) const;
		const Iridium::VulkanOutputTransportSelection& getOutputTransportSelection() const {
			return outputTransportSelection;
		}
		const std::vector<Iridium::Color::OutputTransport>& getSupportedOutputTransports() const {
			return supportedOutputTransports;
		}
	
	private:
		VkContext* context;	// Reference to the core engine
		VkSwapchainKHR swapChain;

		// The image buffer
		std::vector<VkImage> swapChainImages;
		std::vector<VkImageView> swapChainImageViews; // How we look at the images

		// We save these because we need them to configure the rendering pipeline later
		VkFormat swapChainImageFormat;
		VkColorSpaceKHR swapChainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		VkPresentModeKHR swapChainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
		VkExtent2D swapChainExtent;
		Iridium::VulkanOutputTransportSelection outputTransportSelection{};
		std::vector<Iridium::Color::OutputTransport> supportedOutputTransports;
		Iridium::Color::OutputTransport requestedTransport_ =
			Iridium::Color::OutputTransport::SdrSrgb;

		// Helper Functions
		VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);

		// The main builder function
		void createSwapchain(GLFWwindow* window, VkSwapchainKHR oldSwapchain);
		void createImageViews();
};
