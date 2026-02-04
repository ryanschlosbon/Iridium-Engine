#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vector>
#include <optional>

// A helper struct
// Vulkan queues (Graphics, Compute, Present) are grouped into "Families".
// We need to find which family supports what.
struct QueueFamilyIndices {
	// std::optional means "this might contain a number, or it might be empty".
	// Useful because 0 is a valid index, so we can't use "0" to mean "not found".
	std::optional<uint32_t> graphicsFamily;
	std::optional<uint32_t> presentFamily;

	// We are only "complete" if we found both a graphics queue and a presentation queue.
	bool isComplete() {
		return graphicsFamily.has_value() && presentFamily.has_value();
	}
};

struct SwapChainSupportDetails {
	VkSurfaceCapabilitiesKHR capabilities;		// Min/max resolution, image coutns, etc.
	std::vector<VkSurfaceFormatKHR> formats;	// Pixel formats, color spaces, etc.
	std::vector<VkPresentModeKHR> presentModes;	// Sync modes (e.g., vsync)
};

class VkContext {
public:
	// The Constructor takes the Window pointer so we can create the Surface
	VkContext(bool enableValidation, GLFWwindow* window);
	~VkContext();

	// Getters: The rest of the engine will need these handles later.
	VkDevice getDevice() const { return device; };
	VkPhysicalDevice getPhysicalDevice() const { return physicalDevice;  }
	VkSurfaceKHR getSurface() const { return surface; }
	VkQueue getGraphicsQueue() const { return graphicsQueue; }
	VkQueue getPresentQueue() const { return presentQueue; }
	SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
	VkCommandPool getCommandPool() const { return commandPool; }

	// We need to know if validation is on to clean up properly
	bool enableValidationLayers;

private:
	// The Connection to the Driver
	VkInstance instance;
	VkDebugUtilsMessengerEXT debugMessenger;

	// The Connection to the Window
	VkSurfaceKHR surface;

	// The Physical Hardware (GPU)
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

	// The Logical Interface (The "Virtual" GPU we send commands to)
	VkDevice device;

	// The Command Ports
	VkQueue graphicsQueue;
	VkQueue presentQueue;

	VkCommandPool commandPool;


	// Internal Setup Functions
	// These break the massive intialization process into small steps.
	void createInstance();
	void setupDebugMessenger();
	void createSurface(GLFWwindow* window);
	void pickPhysicalDevice();
	void createLogicalDevice();
	void createCommandPool();

	// Helpers
	bool isDeviceSuitable(VkPhysicalDevice device);
	QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
	bool checkDeviceExtensionSupport(VkPhysicalDevice device);

};