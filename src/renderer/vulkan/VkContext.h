#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <string>


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
	VkContext(bool enableValidation, bool enableDebugUtils,
		bool enablePipelineStatistics, GLFWwindow* window);
	~VkContext();

	// Getters: The rest of the engine will need these handles later.
	VkDevice getDevice() const { return device; };
	VkPhysicalDevice getPhysicalDevice() const { return physicalDevice;  }
	VkSurfaceKHR getSurface() const { return surface; }
	VkQueue getGraphicsQueue() const { return graphicsQueue; }
	VkQueue getPresentQueue() const { return presentQueue; }
	SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
	VkInstance getInstance() const { return instance; }
	uint32_t getGraphicsQueueFamily() const { return graphicsQueueFamilyIndex; }
	double getTimestampPeriodNanoseconds() const { return timestampPeriodNanoseconds; }
	uint32_t getTimestampValidBits() const { return timestampValidBits; }
	bool hasMemoryBudget() const { return memoryBudgetEnabled; }
	bool hasDebugUtils() const { return debugUtilsEnabled; }
	bool hasPipelineStatistics() const { return pipelineStatisticsEnabled; }
	bool hasSwapchainColorspace() const { return swapchainColorspaceEnabled; }
	bool hasHdrMetadata() const { return hdrMetadataEnabled; }
	bool hasDescriptorIndexing() const { return descriptorIndexingEnabled; }
	uint32_t getMaxIndexedTextureViews() const { return maxIndexedTextureViews; }
	uint32_t getMaxIndexedSamplers() const { return maxIndexedSamplers; }
	uint32_t getMaxUpdateAfterBindDescriptors() const {
		return maxUpdateAfterBindDescriptors;
	}
	uint32_t getLoaderApiVersion() const { return loaderApiVersion; }
	const VkPhysicalDeviceProperties& getPhysicalDeviceProperties() const {
		return physicalDeviceProperties;
	}
	const VkPhysicalDeviceIDProperties& getPhysicalDeviceIdProperties() const {
		return physicalDeviceIdProperties;
	}
	const VkPhysicalDeviceDriverProperties& getPhysicalDeviceDriverProperties() const {
		return physicalDeviceDriverProperties;
	}
	const std::vector<std::string>& getActiveTools() const { return activeTools; }

	bool enableValidationLayers;
private:
	bool requestDebugUtils = false;
	bool debugUtilsEnabled = false;
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

	uint32_t graphicsQueueFamilyIndex;
	double timestampPeriodNanoseconds = 0.0;
	uint32_t timestampValidBits = 0;
	bool memoryBudgetEnabled = false;
	bool pipelineStatisticsRequested = false;
	bool pipelineStatisticsEnabled = false;
	bool swapchainColorspaceEnabled = false;
	bool hdrMetadataEnabled = false;
	bool descriptorIndexingEnabled = false;
	uint32_t maxIndexedTextureViews = 0;
	uint32_t maxIndexedSamplers = 0;
	uint32_t maxUpdateAfterBindDescriptors = 0;
	uint32_t loaderApiVersion = VK_API_VERSION_1_0;
	VkPhysicalDeviceProperties physicalDeviceProperties{};
	VkPhysicalDeviceIDProperties physicalDeviceIdProperties{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceDriverProperties physicalDeviceDriverProperties{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	std::vector<std::string> activeTools;

	// Internal Setup Functions
	// These break the massive intialization process into small steps.
	void createInstance();
	void setupDebugMessenger();
	void createSurface(GLFWwindow* window);
	void pickPhysicalDevice();
	void createLogicalDevice();
	// Helpers
	bool isDeviceSuitable(VkPhysicalDevice device);
	QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
	bool checkDeviceExtensionSupport(VkPhysicalDevice device);
};
