#include "HeadlessVulkanDevice.h"

#include "renderer/vulkan/VulkanQueueSelection.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace Iridium::Test {
namespace {

    void check(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed: " +
                std::to_string(static_cast<int>(result)));
    }

} // namespace

HeadlessVulkanDevice::HeadlessVulkanDevice() : HeadlessVulkanDevice(Options{}) {}

HeadlessVulkanDevice::HeadlessVulkanDevice(const Options& options) {
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const std::array instanceExtensions{ VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME };
    VkApplicationInfo application{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application.pApplicationName = options.applicationName;
    application.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    debug.pfnUserCallback = &HeadlessVulkanDevice::message;
    debug.pUserData = this;
    const VkValidationFeatureEnableEXT synchronization =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
    validation.enabledValidationFeatureCount =
        options.synchronizationValidation ? 1u : 0u;
    validation.pEnabledValidationFeatures = &synchronization;
    validation.pNext = &debug;

    VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = &layer;
    instanceInfo.enabledExtensionCount =
        static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    instanceInfo.pNext = &validation;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance");
    const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr)
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT unavailable");
    check(createMessenger(instance_, &debug, nullptr, &messenger_),
        "vkCreateDebugUtilsMessengerEXT");

    uint32_t deviceCount = 0;
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr),
        "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()),
        "vkEnumeratePhysicalDevices");
    // Prefer a discrete GPU (the reference system), then any graphics+compute device.
    for (int pass = 0; pass < 2 && physical_ == VK_NULL_HANDLE; ++pass) {
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (pass == 0 &&
                properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                families.data());
            constexpr VkQueueFlags Required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            for (uint32_t family = 0; family < familyCount; ++family) {
                if ((families[family].queueFlags & Required) == Required) {
                    physical_ = candidate;
                    queueFamily_ = family;
                    break;
                }
            }
            if (physical_ != VK_NULL_HANDLE) break;
        }
    }
    if (physical_ == VK_NULL_HANDLE)
        throw std::runtime_error("No Vulkan 1.3 graphics+compute device");
    vkGetPhysicalDeviceProperties(physical_, &properties_);
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    std::cout << "Device: " << properties_.deviceName << '\n';

    // Mirror the feature set the production VkContext enables, so validation
    // judges shaders and pipelines against the same device contract.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_, &supported);
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;
    features.fillModeNonSolid = VK_TRUE;
    features.independentBlend = VK_TRUE;
    features.dualSrcBlend = VK_TRUE;   // M9.8e reactive coverage
    features.imageCubeArray = VK_TRUE;
    features.multiDrawIndirect = supported.multiDrawIndirect;
    features.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;

    VkPhysicalDeviceVulkan13Features supported13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features supported12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    supported12.pNext = &supported13;
    VkPhysicalDeviceFeatures2 supported2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    supported2.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(physical_, &supported2);
    if (!supported12.runtimeDescriptorArray ||
        !supported12.descriptorBindingPartiallyBound ||
        !supported12.descriptorBindingSampledImageUpdateAfterBind ||
        !supported12.descriptorBindingVariableDescriptorCount ||
        !supported12.shaderSampledImageArrayNonUniformIndexing)
        throw std::runtime_error("Device lacks the production descriptor-indexing set");
    VkPhysicalDeviceVulkan12Features enabled12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    enabled12.runtimeDescriptorArray = VK_TRUE;
    enabled12.descriptorBindingPartiallyBound = VK_TRUE;
    enabled12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    enabled12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    enabled12.drawIndirectCount = supported12.drawIndirectCount;
    // M7R R4d: timeline semaphores exactly when supported, as VkContext does.
    timelineSemaphore_ = supported12.timelineSemaphore == VK_TRUE;
    enabled12.timelineSemaphore = supported12.timelineSemaphore;
    // M7R R3: synchronization2 is enabled exactly when supported, as VkContext
    // does, so the graph executor's vkCmdPipelineBarrier2 path is validated.
    synchronization2_ = supported13.synchronization2 == VK_TRUE;
    // M7R R4a: dynamic rendering likewise, for the executor's rendering plans.
    dynamicRendering_ = supported13.dynamicRendering == VK_TRUE;
    VkPhysicalDeviceVulkan13Features enabled13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    enabled13.synchronization2 = supported13.synchronization2;
    enabled13.dynamicRendering = supported13.dynamicRendering;
    enabled12.pNext = &enabled13;

    VkPhysicalDeviceDescriptorIndexingProperties indexing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties2.pNext = &indexing;
    vkGetPhysicalDeviceProperties2(physical_, &properties2);
    maxUpdateAfterBindDescriptors_ = indexing.maxUpdateAfterBindDescriptorsInAllPools;

    std::vector<const char*> deviceExtensions;
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount,
        available.data());
    for (const VkExtensionProperties& extension : available) {
        if (std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
            deviceExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            memoryBudget_ = true;
        }
    }

    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount,
            families.data());
        transferQueueFamily_ =
            selectVulkanTransferQueueFamily(families, queueFamily_).family;
    }
    const float priority = 1.0f;
    std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
    for (VkDeviceQueueCreateInfo& queueInfo : queueInfos) {
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
    }
    queueInfos[0].queueFamilyIndex = queueFamily_;
    queueInfos[1].queueFamilyIndex = transferQueueFamily_;
    VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.pNext = &enabled12;
    deviceInfo.queueCreateInfoCount =
        transferQueueFamily_ == queueFamily_ ? 1u : 2u;
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.pEnabledFeatures = &features;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    check(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    vkGetDeviceQueue(device_, transferQueueFamily_, 0, &transferQueue_);

    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_),
        "vkCreateCommandPool");
}

HeadlessVulkanDevice::~HeadlessVulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (commandPool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != nullptr) destroyMessenger(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

VKAPI_ATTR VkBool32 VKAPI_CALL HeadlessVulkanDevice::message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* self = static_cast<HeadlessVulkanDevice*>(user);
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 &&
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)) != 0;
    const char* text = data != nullptr && data->pMessage != nullptr ? data->pMessage : "";
    std::cerr << (error ? "[validation error] " : "[validation] ") << text << '\n';
    if (error && self != nullptr) {
        ++self->errors_;
        self->messages_.emplace_back(text);
    }
    return VK_FALSE;
}

uint32_t HeadlessVulkanDevice::memoryType(uint32_t typeBits,
    VkMemoryPropertyFlags flags) const {
    for (uint32_t index = 0; index < memory_.memoryTypeCount; ++index)
        if ((typeBits & (1u << index)) != 0 &&
            (memory_.memoryTypes[index].propertyFlags & flags) == flags) return index;
    throw std::runtime_error("No compatible Vulkan memory type");
}

HeadlessVulkanBuffer HeadlessVulkanDevice::createHostBuffer(VkDeviceSize size,
    VkBufferUsageFlags usage) const {
    HeadlessVulkanBuffer result;
    result.size = size;
    VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &result.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &allocation, nullptr, &result.memory),
        "vkAllocateMemory");
    check(vkBindBufferMemory(device_, result.buffer, result.memory, 0),
        "vkBindBufferMemory");
    check(vkMapMemory(device_, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped),
        "vkMapMemory");
    std::memset(result.mapped, 0, static_cast<size_t>(size));
    return result;
}

void HeadlessVulkanDevice::destroy(HeadlessVulkanBuffer& buffer) const noexcept {
    if (buffer.memory != VK_NULL_HANDLE) {
        if (buffer.mapped != nullptr) vkUnmapMemory(device_, buffer.memory);
        vkFreeMemory(device_, buffer.memory, nullptr);
    }
    if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer.buffer, nullptr);
    buffer = {};
}

VkShaderModule HeadlessVulkanDevice::createShaderModule(
    const std::filesystem::path& spvPath) const {
    std::ifstream input(spvPath, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("missing SPIR-V " + spvPath.string());
    const std::streamsize bytes = input.tellg();
    input.seekg(0);
    std::vector<uint32_t> words(static_cast<size_t>(bytes + 3) / 4u);
    input.read(reinterpret_cast<char*>(words.data()), bytes);
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = static_cast<size_t>(bytes);
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void HeadlessVulkanDevice::submitAndWait(
    const std::function<void(VkCommandBuffer)>& record) const {
    VkCommandBufferAllocateInfo allocation{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocation.commandPool = commandPool_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &allocation, &commands),
        "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    record(commands);
    check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
    vkFreeCommandBuffers(device_, commandPool_, 1, &commands);
}

} // namespace Iridium::Test
