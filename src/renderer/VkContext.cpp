#include "VkContext.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <cstring>

// The list of validation layers we want (The Police)
// These catch errors. We have to call it manually
const std::vector<const char*> validationLayers = {
	"VK_LAYER_KHRONOS_validation"
};

// The list of features we need from the GPU
// "VK_KHR_swapchain" allows the GPU to send images to the Windows/Linux window system.
const std::vector<const char*> deviceExtensions = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// Proxy Function 1: Create the Messenger 
VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, 
	const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
	// Lookup the function address
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, 
		"vkCreateDebugUtilsMessengerEXT");

	// If found, call it. If not, return an error.
	if (func != nullptr) {
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	}
	else {
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
}

// Proxy Function 2: Destroy the Messenger
void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, 
	const VkAllocationCallbacks* pAllocator) {
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, 
		"vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr) {
		func(instance, debugMessenger, pAllocator);
	}
}

// The Callback Function
// The function that gets called when Vulkan finds an error.
// The message is formatted and then printed to std::cerr.
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData) {
	std::cerr << "[Validation]: " << pCallbackData->pMessage << std::endl;

	// Return VK_FALSE to tell Vulkan "Don't abort the call, let it continue"
	// (unless it's fatal, but usually we want to keep going to see more errors)
	return VK_FALSE;
}

// Constructor
VkContext::VkContext(bool enableValidation, GLFWwindow* window) : enableValidationLayers(enableValidation) {
	// Initialize the library
	createInstance();

	// Start the police if enabled
	setupDebugMessenger();

	// Connect to a window
	createSurface(window);

	// Find the GPU
	pickPhysicalDevice();

	// Turn on the GPU features
	createLogicalDevice();
}

// Destructor
VkContext::~VkContext() {
	// Cleanup happens in reverse order of creation
	// Shutdown the Logical Device
	vkDestroyDevice(device, nullptr);

	// Shutdown the Debug Messenger
	if (enableValidationLayers) {
		DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
	}

	// Destroy the Window Surface
	vkDestroySurfaceKHR(instance, surface, nullptr);

	// Finally, shutdown the Instance
	vkDestroyInstance(instance, nullptr);
}

void VkContext::createInstance() {
	// App Info
	// Optional, but allows GPU drivers to look at this name. This allows them to load optimized driver profiles
	// For now it will load the default profile
	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Iridium Engine";
	appInfo.apiVersion = VK_API_VERSION_1_3;

	// Create Info (Global Settings)
	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	// Extensions
	// Ask GLFW: "What extensions do I need to talk to this OS window?"
	uint32_t glfwExtensionCount = 0;
	const char** glfwExtensions;
	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	// Convert to a vector so we can add more later as needed
	std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

	// If we are debugging, we add the Debug Utils extension
	if (enableValidationLayers) {
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	// Layers
	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();
	}
	else {
		createInfo.enabledLayerCount = 0;
	}

	// User Vulkan Check
	// If it fails it means the user doesn't have Vulkan installed, or something is wrong with the installation
	if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
		throw std::runtime_error("failed to create instance!");
	}
}

void VkContext::setupDebugMessenger() {
	if (!enableValidationLayers) return;

	VkDebugUtilsMessengerCreateInfoEXT createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;

	// Severity: warning | Error (we usually ignore "info" and "verbose" to avoid spam
	createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | 
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	//Type: general | validation | performance
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | 
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = debugCallback;

	if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
		throw std::runtime_error("failed to set up debug messenger!");
	}
}

void VkContext::createSurface(GLFWwindow* window) {
	// GLFW handles the platform-specific ugliness (Win32 HWND / Linux XCB) for us.
	if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
	}
}

void VkContext::pickPhysicalDevice() {
	// Count the GPUs
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

	if (deviceCount == 0) {
		throw std::runtime_error("failed to find GPUs with Vulkan support!");
	}

	// Get the list of GPUs
	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

	// interview each one
	for (const auto& device : devices) {
		if (isDeviceSuitable(device)) {
			physicalDevice = device;
			break; // We found one, stop looking
		}
	}

	if (physicalDevice == VK_NULL_HANDLE) {
		throw std::runtime_error("failed to find a suitable GPU");
	}

	std::cout << "Physical Device Selected" << std::endl;
}

// Interview Logic
bool VkContext::isDeviceSuitable(VkPhysicalDevice device) {
	// Does it have the right Queues?
	QueueFamilyIndices indices = findQueueFamilies(device);

	// Does it have the right Extensions (swapchain)?
	bool extensionsSupported = checkDeviceExtensionSupport(device);

	bool swapChainAdequate = false;
	if (extensionsSupported) {
		// Only query swapchain support if extensions are supported
		SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);

		// We need at least one format and one present mode
		swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
	}

	return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

// Extension checker
bool VkContext::checkDeviceExtensionSupport(VkPhysicalDevice device) {
	// Get all extensions this specific GPU supports
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

	// Create a set of extensions we NEED (from our constant list at the top)
	std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

	// Loop through available ones and "check off" the ones we need
	for (const auto& extension : availableExtensions) {
		requiredExtensions.erase(extension.extensionName);
	}
	
	// If the required set is empty, ity means we found everything we needed.
	return requiredExtensions.empty();
}

QueueFamilyIndices VkContext::findQueueFamilies(VkPhysicalDevice device) {
	QueueFamilyIndices indices;

	// Get the properties
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	// iterate through them to find the ones we need
	int i = 0;
	for (const auto& queueFamily : queueFamilies) {
		// A. Does this family support Graphics?
		// We check the bitmask
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			indices.graphicsFamily = i;
		}

		// B. Does this family support presenting to the window?
		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
		if (presentSupport) {
			indices.presentFamily = i;
		}

		// C. Early Exit
		// If we found both, we can stop looking.
		if (indices.isComplete()) {
			break;
		}

		i++;
	}

	return indices;
}

void VkContext::createLogicalDevice() {
	// Get the indices again so we know what to create
	QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	// We use a set to ensure we don't create the same queue twice
	std::set<uint32_t> uniqueQueueFamilies = {
		indices.graphicsFamily.value(),
		indices.presentFamily.value()
	};

	// Vulkan requires us to assign a priority (0.0 to 1.0) to queues.
	// This influences scheduling if you have multple threads fighting for the GPU
	float queuePriority = 1.0f;

	// Create the info struct for every unique queue family we need
	for (uint32_t queueFamily : uniqueQueueFamilies) {
		VkDeviceQueueCreateInfo queueCreateInfo{};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	// Specify Device Features
	// for now, no special shader features are necessary.
	// Later, we will come back to enable samplerAnisotropy
	VkPhysicalDeviceFeatures deviceFeatures{};

	// Create the Logical Device
	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

	// Attach the queues
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();

	// Attach the features
	createInfo.pEnabledFeatures = &deviceFeatures;

	// Attach the extensions (Swapchain)
	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();

	// (Legacy) Set validation layers for the device
	// Modern Vulkan ignores this as it uses Instance layers, but we set it for backwards compatibility.
	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();
	}
	else {
		createInfo.enabledLayerCount = 0;
	}

	if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
		throw std::runtime_error("failed to create logical device");
	}

	// Retrieve the Queue Handles
	// The device is created. Now we ask it for the handles to the queues so we can use them later.
	vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
	vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
}

SwapChainSupportDetails VkContext::querySwapChainSupport(VkPhysicalDevice device) {
	SwapChainSupportDetails details;

	// Capabilities
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

	// Formats
	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

	if (formatCount != 0) {
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
	}

	// Present Modes
	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

	if (presentModeCount != 0) {
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
	}

	return details;
}