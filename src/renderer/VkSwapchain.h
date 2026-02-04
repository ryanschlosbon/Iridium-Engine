#pragma once

#include "VkContext.h"
#include <vector>
#include <vulkan/vulkan.h>

class VkSwapchain {
	public:
		// The Constructor
		VkSwapchain(VkContext* context, GLFWwindow* window);
		~VkSwapchain();

		// Getters for the raw handles
		VkSwapchainKHR getSwapchain() const { return swapChain; }
		VkFormat getImageFormat() const { return swapChainImageFormat; }
		VkExtent2D getExtent() const { return swapChainExtent; }
		const std::vector<VkImageView>& getImageViews() const { return swapChainImageViews; }
	
	private:
		VkContext* context;	// Reference to the core engine
		VkSwapchainKHR swapChain;

		// The image buffer
		std::vector<VkImage> swapChainImages;
		std::vector<VkImageView> swapChainImageViews; // How we look at the images

		// We save these because we need them to configure the rendering pipeline later
		VkFormat swapChainImageFormat;
		VkExtent2D swapChainExtent;

		// Helper Functions
		VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);

		// The main builder function
		void createSwapchain(GLFWwindow* window);
		void createImageViews();
};