#include "VkContext.h"
#include "VkCommandManager.h"
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

	createCommandPool();
}

// Destructor
VkContext::~VkContext() {
	// Cleanup happens in reverse order of creation
	vkDestroyCommandPool(device, commandPool, nullptr);

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

	// 1. MAC SPECIFIC: Enable Portability Enumeration Extension
	// This flags to the loader that we want to see MoltenVK devices.
	#ifdef __APPLE__
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		// You might also need this one for proper device properties on Mac
		extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	#endif

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

	// 2. MAC SPECIFIC: Set the Flag
	#ifdef __APPLE__
		createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	#endif

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
			this->graphicsQueueFamilyIndex = i;
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
    // 1. Queue Family Logic
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {
       indices.graphicsFamily.value(),
       indices.presentFamily.value()
    };

    float queuePriority = 1.0f;

    for (uint32_t queueFamily : uniqueQueueFamilies) {
       VkDeviceQueueCreateInfo queueCreateInfo{};
       queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
       queueCreateInfo.queueFamilyIndex = queueFamily;
       queueCreateInfo.queueCount = 1;
       queueCreateInfo.pQueuePriorities = &queuePriority;
       queueCreateInfos.push_back(queueCreateInfo);
    }

    // 2. Device Features
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
	deviceFeatures.fillModeNonSolid = VK_TRUE;
	deviceFeatures.independentBlend = VK_TRUE;

    // 3. Extensions Setup (THE MAC COMPATIBILITY FIX)
    // Start with the Swapchain extension, which is required on all platforms.
    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    // Check if the device supports the "Portability Subset" extension.
    // This is required for MoltenVK (macOS) but not for standard Vulkan drivers.
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data());

    for (const auto& extension : availableExtensions) {
        if (strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0) {
            deviceExtensions.push_back("VK_KHR_portability_subset");
            break;
        }
    }

    // 4. Create the Logical Device
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;

    // Pass the dynamically created list of extensions
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

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

    // 5. Retrieve Queue Handles
    vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
    vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
}

void VkContext::createCommandPool() {
	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;

	// RESET_COMMAND_BUFFER_BIT: Allows the command buffers to be rerecorded individually
	// Allows camera control
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	// The pool must command buffers for the graphics queue
	poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

	if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
		throw std::runtime_error("failed to create command pool!");
	}
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

// Find Memory Type
// GPUs have different types of memory (VRAM, RAM, etc.)
uint32_t VkContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
		// Check if this type is in the typeFilter and has the required properties
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	throw std::runtime_error("failed to find suitable memory type!");
}

// Create Buffer
void VkContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, 
	VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
	// Create the Buffer Handle
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // Only used by 1 queue family

	if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to create buffer!");
	}
	// Allocate Memory for the Buffer
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, buffer, &memRequirements);
	
	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate buffer memory!");
	}
	// Bind the Memory to the Buffer
	vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

void VkContext::createImage(uint32_t width, uint32_t height, VkFormat format,
	VkImageTiling tiling, VkImageUsageFlags usage,
	VkMemoryPropertyFlags properties, VkImage& image,
	VkDeviceMemory& imageMemory) {

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? 
		VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PREINITIALIZED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
		throw std::runtime_error("failed to create image!");
	}

	// Handle Memory Allocation
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(device, image, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate image memory!");
	}

	vkBindImageMemory(device, image, imageMemory, 0);
}

void VkContext::createGPUBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
	const void* data, VkBuffer& buffer, VkDeviceMemory& memory,
	VkCommandManager* commandManager) {
	// 1. Create Staging Buffer (CPU Visible)
	VkBuffer stagingBuffer;
	VkDeviceMemory stagingBufferMemory;
	createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		stagingBuffer, stagingBufferMemory);

	// 2. Map & Copy
	void* mappedData;
	vkMapMemory(device, stagingBufferMemory, 0, size, 0, &mappedData);
	memcpy(mappedData, data, (size_t)size);
	vkUnmapMemory(device, stagingBufferMemory);

	// 3. Create GPU Buffer (Device Local)
	createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		buffer, memory);

	// 4. Copy from Staging to GPU
	VkCommandBuffer commandBuffer = commandManager->beginSingleTimeCommands();

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, stagingBuffer, buffer, 1, &copyRegion);

	commandManager->endSingleTimeCommands(commandBuffer);

	// 5. Cleanup Staging
	vkDestroyBuffer(device, stagingBuffer, nullptr);
	vkFreeMemory(device, stagingBufferMemory, nullptr);
}