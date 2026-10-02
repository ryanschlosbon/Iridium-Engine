#include "renderer/rhi/VirtualShadowMap.h"
#include "renderer/vulkan/VulkanVirtualShadowMarkingPass.h"
#include "renderer/vulkan/VulkanVirtualShadowDepthReceiverPass.h"
#include "renderer/vulkan/VulkanVirtualShadowFullViewPass.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    void require(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed: " +
                std::to_string(result));
    }

    std::atomic<uint32_t> validationErrors = 0;
    VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 &&
            (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0)
            ++validationErrors;
        std::cerr << data->pMessage << '\n';
        return VK_FALSE;
    }

    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct Device {
        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice logical = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkShaderModule shader = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkDescriptorSetLayout compactSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout compactPipelineLayout = VK_NULL_HANDLE;
        VkPipeline compactPipeline = VK_NULL_HANDLE;
        VkShaderModule compactShader = VK_NULL_HANDLE;
        VkDescriptorSet compactDescriptorSet = VK_NULL_HANDLE;
        VkDescriptorSetLayout parallelSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout parallelPipelineLayout = VK_NULL_HANDLE;
        VkPipeline parallelPipeline = VK_NULL_HANDLE;
        VkShaderModule parallelShader = VK_NULL_HANDLE;
        VkDescriptorSet parallelDescriptorSet = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        uint32_t queueFamily = UINT32_MAX;
        uint32_t timestampValidBits = 0;
        float timestampPeriodNanoseconds = 0.0f;

        Device() {
            const char* layer = "VK_LAYER_KHRONOS_validation";
            const std::array extensions{ VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME };
            VkApplicationInfo application{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            application.pApplicationName = "Iridium virtual-shadow marking parity";
            application.apiVersion = VK_API_VERSION_1_1;
            VkDebugUtilsMessengerCreateInfoEXT debug{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            debug.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            debug.pfnUserCallback = validationMessage;
            const VkValidationFeatureEnableEXT synchronization =
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
            VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
            validation.enabledValidationFeatureCount = 1;
            validation.pEnabledValidationFeatures = &synchronization;
            debug.pNext = &validation;
            VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            create.pApplicationInfo = &application;
            create.enabledLayerCount = 1;
            create.ppEnabledLayerNames = &layer;
            create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            create.pNext = &debug;
            require(vkCreateInstance(&create, nullptr, &instance),
                "vkCreateInstance");
            require(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"))(
                    instance, &debug, nullptr, &messenger),
                "vkCreateDebugUtilsMessengerEXT");

            uint32_t deviceCount = 0;
            require(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr),
                "vkEnumeratePhysicalDevices(count)");
            std::vector<VkPhysicalDevice> devices(deviceCount);
            require(vkEnumeratePhysicalDevices(instance, &deviceCount,
                devices.data()), "vkEnumeratePhysicalDevices");
            for (VkPhysicalDevice candidate : devices) {
                uint32_t familyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate,
                    &familyCount, nullptr);
                std::vector<VkQueueFamilyProperties> families(familyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate,
                    &familyCount, families.data());
                for (uint32_t index = 0; index < familyCount; ++index) {
                    if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                        physical = candidate;
                        queueFamily = index;
                        timestampValidBits = families[index].timestampValidBits;
                        break;
                    }
                }
                if (physical != VK_NULL_HANDLE) break;
            }
            if (physical == VK_NULL_HANDLE)
                throw std::runtime_error("No Vulkan compute device");
            vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            timestampPeriodNanoseconds = properties.limits.timestampPeriod;
            std::cout << "Device: " << properties.deviceName << '\n';

            float priority = 1.0f;
            VkDeviceQueueCreateInfo queueInfo{
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            queueInfo.queueFamilyIndex = queueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            require(vkCreateDevice(physical, &deviceInfo, nullptr, &logical),
                "vkCreateDevice");
            vkGetDeviceQueue(logical, queueFamily, 0, &queue);

            VkCommandPoolCreateInfo commandInfo{
                VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            commandInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            commandInfo.queueFamilyIndex = queueFamily;
            require(vkCreateCommandPool(logical, &commandInfo, nullptr,
                &commandPool), "vkCreateCommandPool");
            const std::array bindings{
                VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            VkDescriptorSetLayoutCreateInfo setInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            setInfo.pBindings = bindings.data();
            require(vkCreateDescriptorSetLayout(logical, &setInfo, nullptr,
                &setLayout), "vkCreateDescriptorSetLayout");
            VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 };
            VkPipelineLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout;
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &push;
            require(vkCreatePipelineLayout(logical, &layoutInfo, nullptr,
                &pipelineLayout), "vkCreatePipelineLayout");

            std::ifstream file(std::string(PROJECT_ROOT_DIR) +
                "/assets/shaders/virtual_shadow_directional_mark_comp.spv",
                std::ios::binary | std::ios::ate);
            if (!file) throw std::runtime_error("Missing marking shader");
            const size_t byteCount = static_cast<size_t>(file.tellg());
            if (byteCount == 0 || byteCount % 4 != 0)
                throw std::runtime_error("Invalid marking shader");
            std::vector<uint32_t> code(byteCount / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), byteCount);
            VkShaderModuleCreateInfo moduleInfo{
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            moduleInfo.codeSize = byteCount;
            moduleInfo.pCode = code.data();
            require(vkCreateShaderModule(logical, &moduleInfo, nullptr, &shader),
                "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipelineInfo.stage = {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr };
            pipelineInfo.layout = pipelineLayout;
            require(vkCreateComputePipelines(logical, VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &pipeline),
                "vkCreateComputePipelines");

            const std::array compactBindings{
                VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            setInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setInfo.bindingCount = static_cast<uint32_t>(compactBindings.size());
            setInfo.pBindings = compactBindings.data();
            require(vkCreateDescriptorSetLayout(logical, &setInfo, nullptr,
                &compactSetLayout), "vkCreateDescriptorSetLayout(compact)");
            VkPushConstantRange compactPush{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
            layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &compactSetLayout;
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &compactPush;
            require(vkCreatePipelineLayout(logical, &layoutInfo, nullptr,
                &compactPipelineLayout), "vkCreatePipelineLayout(compact)");
            std::ifstream compactFile(std::string(PROJECT_ROOT_DIR) +
                "/assets/shaders/virtual_shadow_directional_compact_comp.spv",
                std::ios::binary | std::ios::ate);
            if (!compactFile) throw std::runtime_error("Missing compaction shader");
            const size_t compactByteCount =
                static_cast<size_t>(compactFile.tellg());
            if (compactByteCount == 0 || compactByteCount % 4 != 0)
                throw std::runtime_error("Invalid compaction shader");
            std::vector<uint32_t> compactCode(compactByteCount / 4);
            compactFile.seekg(0);
            compactFile.read(reinterpret_cast<char*>(compactCode.data()),
                compactByteCount);
            moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            moduleInfo.codeSize = compactByteCount;
            moduleInfo.pCode = compactCode.data();
            require(vkCreateShaderModule(logical, &moduleInfo, nullptr,
                &compactShader), "vkCreateShaderModule(compact)");
            pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipelineInfo.stage = {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, compactShader, "main", nullptr };
            pipelineInfo.layout = compactPipelineLayout;
            require(vkCreateComputePipelines(logical, VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &compactPipeline),
                "vkCreateComputePipelines(compact)");

            const std::array parallelBindings{
                VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                VkDescriptorSetLayoutBinding{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            setInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setInfo.bindingCount = static_cast<uint32_t>(parallelBindings.size());
            setInfo.pBindings = parallelBindings.data();
            require(vkCreateDescriptorSetLayout(logical, &setInfo, nullptr,
                &parallelSetLayout),
                "vkCreateDescriptorSetLayout(parallel compact)");
            VkPushConstantRange parallelPush{
                VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
            layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &parallelSetLayout;
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &parallelPush;
            require(vkCreatePipelineLayout(logical, &layoutInfo, nullptr,
                &parallelPipelineLayout),
                "vkCreatePipelineLayout(parallel compact)");
            std::ifstream parallelFile(std::string(PROJECT_ROOT_DIR) +
                "/assets/shaders/virtual_shadow_directional_compact_parallel_comp.spv",
                std::ios::binary | std::ios::ate);
            if (!parallelFile)
                throw std::runtime_error("Missing parallel compaction shader");
            const size_t parallelByteCount =
                static_cast<size_t>(parallelFile.tellg());
            if (parallelByteCount == 0 || parallelByteCount % 4 != 0)
                throw std::runtime_error("Invalid parallel compaction shader");
            std::vector<uint32_t> parallelCode(parallelByteCount / 4);
            parallelFile.seekg(0);
            parallelFile.read(reinterpret_cast<char*>(parallelCode.data()),
                parallelByteCount);
            moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            moduleInfo.codeSize = parallelByteCount;
            moduleInfo.pCode = parallelCode.data();
            require(vkCreateShaderModule(logical, &moduleInfo, nullptr,
                &parallelShader), "vkCreateShaderModule(parallel compact)");
            pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipelineInfo.stage = {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, parallelShader, "main", nullptr };
            pipelineInfo.layout = parallelPipelineLayout;
            require(vkCreateComputePipelines(logical, VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &parallelPipeline),
                "vkCreateComputePipelines(parallel compact)");

            VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14 };
            VkDescriptorPoolCreateInfo poolInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.maxSets = 3;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            require(vkCreateDescriptorPool(logical, &poolInfo, nullptr,
                &descriptorPool), "vkCreateDescriptorPool");
            VkDescriptorSetAllocateInfo allocate{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocate.descriptorPool = descriptorPool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &setLayout;
            require(vkAllocateDescriptorSets(logical, &allocate, &descriptorSet),
                "vkAllocateDescriptorSets");
            allocate.pSetLayouts = &compactSetLayout;
            require(vkAllocateDescriptorSets(logical, &allocate,
                &compactDescriptorSet), "vkAllocateDescriptorSets(compact)");
            allocate.pSetLayouts = &parallelSetLayout;
            require(vkAllocateDescriptorSets(logical, &allocate,
                &parallelDescriptorSet),
                "vkAllocateDescriptorSets(parallel compact)");
        }

        uint32_t memoryType(uint32_t bits,
            VkMemoryPropertyFlags required) const {
            for (uint32_t index = 0;
                index < memoryProperties.memoryTypeCount; ++index) {
                if ((bits & (1u << index)) != 0 &&
                    (memoryProperties.memoryTypes[index].propertyFlags &
                        required) == required)
                    return index;
            }
            throw std::runtime_error("No compatible Vulkan memory type");
        }

        Buffer createBuffer(VkDeviceSize size,
            VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VkMemoryPropertyFlags properties =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) const {
            Buffer result{};
            result.size = size;
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = size;
            info.usage = usage;
            require(vkCreateBuffer(logical, &info, nullptr, &result.handle),
                "vkCreateBuffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(logical, result.handle, &requirements);
            VkMemoryAllocateInfo allocation{
                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(
                requirements.memoryTypeBits, properties);
            require(vkAllocateMemory(logical, &allocation, nullptr,
                &result.memory), "vkAllocateMemory");
            require(vkBindBufferMemory(logical, result.handle, result.memory, 0),
                "vkBindBufferMemory");
            if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
                require(vkMapMemory(logical, result.memory, 0, size, 0,
                    &result.mapped), "vkMapMemory");
            return result;
        }

        Buffer createDeviceBuffer(VkDeviceSize size) const {
            return createBuffer(size,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }

        void destroy(Buffer& buffer) const {
            if (buffer.mapped != nullptr) vkUnmapMemory(logical, buffer.memory);
            if (buffer.handle != VK_NULL_HANDLE)
                vkDestroyBuffer(logical, buffer.handle, nullptr);
            if (buffer.memory != VK_NULL_HANDLE)
                vkFreeMemory(logical, buffer.memory, nullptr);
            buffer = {};
        }

        ~Device() {
            if (logical != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(logical);
                vkDestroyDescriptorPool(logical, descriptorPool, nullptr);
                vkDestroyPipeline(logical, parallelPipeline, nullptr);
                vkDestroyPipeline(logical, compactPipeline, nullptr);
                vkDestroyPipeline(logical, pipeline, nullptr);
                vkDestroyShaderModule(logical, parallelShader, nullptr);
                vkDestroyShaderModule(logical, compactShader, nullptr);
                vkDestroyShaderModule(logical, shader, nullptr);
                vkDestroyPipelineLayout(logical, parallelPipelineLayout, nullptr);
                vkDestroyPipelineLayout(logical, compactPipelineLayout, nullptr);
                vkDestroyPipelineLayout(logical, pipelineLayout, nullptr);
                vkDestroyDescriptorSetLayout(logical, parallelSetLayout, nullptr);
                vkDestroyDescriptorSetLayout(logical, compactSetLayout, nullptr);
                vkDestroyDescriptorSetLayout(logical, setLayout, nullptr);
                vkDestroyCommandPool(logical, commandPool, nullptr);
                vkDestroyDevice(logical, nullptr);
            }
            if (messenger != VK_NULL_HANDLE)
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance,
                        "vkDestroyDebugUtilsMessengerEXT"))(
                            instance, messenger, nullptr);
            if (instance != VK_NULL_HANDLE)
                vkDestroyInstance(instance, nullptr);
        }
    };

    Iridium::PackedDirectionalVirtualShadowClipLevel pack(
        const Iridium::DirectionalVirtualShadowClipLevel& source) {
        return Iridium::packDirectionalVirtualShadowClipLevel(source);
    }

    Iridium::DirectionalVirtualShadowClipLevel level(uint8_t index) {
        return {
            .lightOwner = *Iridium::SceneEntityUuid::parse(
                "019fb73d-5a60-7000-8000-000000000001"),
            .projectionRevision = 1,
            .staticCasterRevision = 2,
            .dynamicCasterRevision = 3,
            .virtualResolutionTexels = 1'024,
            .priority = index == 0 ? 10 : 5,
            .level = index,
        };
    }

    void dispatch(Device& device,
        std::span<const Iridium::DirectionalVirtualShadowClipLevel> sourceLevels,
        std::span<const Iridium::DirectionalVirtualShadowReceiverSample> sourceReceivers,
        std::span<Iridium::PackedDirectionalVirtualShadowGpuMark> output) {
        std::vector<Iridium::PackedDirectionalVirtualShadowClipLevel> levels;
        for (const auto& source : sourceLevels) levels.push_back(pack(source));
        std::vector<Iridium::PackedDirectionalVirtualShadowReceiver> receivers;
        for (const auto& source : sourceReceivers) receivers.push_back({
            .worldPosition = glm::vec4(source.worldPosition, 1.0f),
            .receiverSamples = source.receiverSamples,
        });
        Buffer levelBuffer = device.createBuffer(
            levels.size() * sizeof(levels[0]));
        Buffer receiverBuffer = device.createBuffer(
            receivers.size() * sizeof(receivers[0]));
        Buffer outputBuffer = device.createBuffer(
            output.size() * sizeof(output[0]));
        std::memcpy(levelBuffer.mapped, levels.data(), levelBuffer.size);
        std::memcpy(receiverBuffer.mapped, receivers.data(), receiverBuffer.size);
        std::memset(outputBuffer.mapped, 0, outputBuffer.size);
        const std::array infos{
            VkDescriptorBufferInfo{ levelBuffer.handle, 0, levelBuffer.size },
            VkDescriptorBufferInfo{ receiverBuffer.handle, 0, receiverBuffer.size },
            VkDescriptorBufferInfo{ outputBuffer.handle, 0, outputBuffer.size },
        };
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[index].dstSet = device.descriptorSet;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &infos[index];
        }
        vkUpdateDescriptorSets(device.logical, static_cast<uint32_t>(writes.size()),
            writes.data(), 0, nullptr);
        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocation.commandPool = device.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        require(vkAllocateCommandBuffers(device.logical, &allocation, &command),
            "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        require(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            device.pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            device.pipelineLayout, 0, 1, &device.descriptorSet, 0, nullptr);
        uint32_t coarsest = 0;
        for (const auto& source : sourceLevels)
            coarsest = (std::max)(coarsest, uint32_t{source.level});
        const std::array<uint32_t, 6> push{
            static_cast<uint32_t>(sourceReceivers.size()),
            static_cast<uint32_t>(sourceLevels.size()), 128u, 1u, coarsest,
            Iridium::VirtualShadowMapAbiVersion,
        };
        vkCmdPushConstants(command, device.pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
        vkCmdDispatch(command,
            (static_cast<uint32_t>(sourceReceivers.size()) + 63u) / 64u, 1, 1);
        VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        require(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        require(vkQueueSubmit(device.queue, 1, &submit, VK_NULL_HANDLE),
            "vkQueueSubmit");
        require(vkQueueWaitIdle(device.queue), "vkQueueWaitIdle");
        std::memcpy(output.data(), outputBuffer.mapped, outputBuffer.size);
        vkFreeCommandBuffers(device.logical, device.commandPool, 1, &command);
        device.destroy(outputBuffer);
        device.destroy(receiverBuffer);
        device.destroy(levelBuffer);
    }

    void dispatchCompaction(Device& device,
        std::span<const Iridium::DirectionalVirtualShadowClipLevel> sourceLevels,
        std::span<const Iridium::PackedDirectionalVirtualShadowGpuMark> marks,
        uint32_t maximumOutputRequests,
        std::span<Iridium::PackedDirectionalVirtualShadowGpuRequest> output,
        Iridium::PackedDirectionalVirtualShadowGpuCompactionTelemetry& telemetry) {
        if (marks.empty() || sourceLevels.empty() || output.empty() ||
            maximumOutputRequests > output.size())
            throw std::runtime_error("Invalid compaction dispatch dimensions");

        std::vector<Iridium::PackedDirectionalVirtualShadowClipLevel> levels;
        levels.reserve(sourceLevels.size());
        for (const auto& source : sourceLevels) levels.push_back(pack(source));

        Buffer levelBuffer = device.createBuffer(
            levels.size() * sizeof(levels[0]));
        Buffer markBuffer = device.createBuffer(
            marks.size() * sizeof(marks[0]));
        Buffer scratchBuffer = device.createBuffer(
            marks.size() * sizeof(Iridium::PackedDirectionalVirtualShadowGpuRequest));
        Buffer outputBuffer = device.createBuffer(
            output.size() * sizeof(output[0]));
        Buffer telemetryBuffer = device.createBuffer(sizeof(telemetry));
        std::memcpy(levelBuffer.mapped, levels.data(), levelBuffer.size);
        std::memcpy(markBuffer.mapped, marks.data(), markBuffer.size);
        std::memset(scratchBuffer.mapped, 0, scratchBuffer.size);
        std::memset(outputBuffer.mapped, 0, outputBuffer.size);
        std::memset(telemetryBuffer.mapped, 0, telemetryBuffer.size);

        const std::array infos{
            VkDescriptorBufferInfo{ levelBuffer.handle, 0, levelBuffer.size },
            VkDescriptorBufferInfo{ markBuffer.handle, 0, markBuffer.size },
            VkDescriptorBufferInfo{ scratchBuffer.handle, 0, scratchBuffer.size },
            VkDescriptorBufferInfo{ outputBuffer.handle, 0, outputBuffer.size },
            VkDescriptorBufferInfo{ telemetryBuffer.handle, 0,
                telemetryBuffer.size },
        };
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[index].dstSet = device.compactDescriptorSet;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &infos[index];
        }
        vkUpdateDescriptorSets(device.logical, static_cast<uint32_t>(writes.size()),
            writes.data(), 0, nullptr);

        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocation.commandPool = device.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        require(vkAllocateCommandBuffers(device.logical, &allocation, &command),
            "vkAllocateCommandBuffers(compact)");
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        require(vkBeginCommandBuffer(command, &begin),
            "vkBeginCommandBuffer(compact)");
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            device.compactPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            device.compactPipelineLayout, 0, 1, &device.compactDescriptorSet,
            0, nullptr);
        const std::array<uint32_t, 4> push{
            static_cast<uint32_t>(marks.size()),
            static_cast<uint32_t>(sourceLevels.size()),
            maximumOutputRequests,
            Iridium::VirtualShadowMapAbiVersion,
        };
        vkCmdPushConstants(command, device.compactPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
        vkCmdDispatch(command, 1, 1, 1);
        VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        require(vkEndCommandBuffer(command), "vkEndCommandBuffer(compact)");
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        require(vkQueueSubmit(device.queue, 1, &submit, VK_NULL_HANDLE),
            "vkQueueSubmit(compact)");
        require(vkQueueWaitIdle(device.queue), "vkQueueWaitIdle(compact)");
        std::memcpy(output.data(), outputBuffer.mapped, outputBuffer.size);
        std::memcpy(&telemetry, telemetryBuffer.mapped, sizeof(telemetry));

        vkFreeCommandBuffers(device.logical, device.commandPool, 1, &command);
        device.destroy(telemetryBuffer);
        device.destroy(outputBuffer);
        device.destroy(scratchBuffer);
        device.destroy(markBuffer);
        device.destroy(levelBuffer);
    }

    void verifyDepthReceiverChain(Device& device, bool fullView = false,
        uint32_t Width = 67, uint32_t Height = 65) {
        using namespace Iridium;
        std::vector<float> depth(Width * Height, 0.5f);
        depth[Width + 1] = 1.0f; depth[Width + 2] = 0.0f;
        depth[Width + 3] = std::numeric_limits<float>::quiet_NaN();
        depth[Width + 4] = -0.5f; depth[Width + 5] = 1.5f;
        VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkSampler sampler{};
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = {Width, Height, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        require(vkCreateImage(device.logical, &imageInfo, nullptr, &image), "depth fixture image");
        VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device.logical, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = device.memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require(vkAllocateMemory(device.logical, &allocation, nullptr, &memory), "depth fixture memory");
        require(vkBindImageMemory(device.logical, image, memory, 0), "depth fixture bind");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        require(vkCreateImageView(device.logical, &viewInfo, nullptr, &view), "depth fixture view");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        require(vkCreateSampler(device.logical, &samplerInfo, nullptr, &sampler), "depth fixture sampler");
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(device.physical, &properties);
        VirtualShadowResourceConfig config{}; config.compactedRequestCapacity = 32;
        const auto working = buildVirtualShadowGpuWorkingSetLayout(config, properties.limits.minStorageBufferOffsetAlignment);
        Buffer storage = device.createDeviceBuffer(working.totalBytes);
        Buffer upload = device.createBuffer(depth.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback = device.createBuffer(working.totalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memcpy(upload.mapped, depth.data(), upload.size);
        auto clip = level(0); clip.virtualResolutionTexels = 32'768;
        if (fullView) { clip.worldPageOriginX = -128; clip.worldPageOriginY = -64; }
        std::vector<DirectionalVirtualShadowClipLevel> sourceLevels{clip};
        // Odd full-view fixture exercises independent dense-grid offsets and
        // finest-level guard-band fallback; 4K retains the maximum single grid.
        if (fullView && Width == 67) {
            DirectionalVirtualShadowClipConfig clipConfig{};
            clipConfig.lightOwner = clip.lightOwner;
            clipConfig.projectionRevision = clip.projectionRevision;
            clipConfig.staticCasterRevision = clip.staticCasterRevision;
            clipConfig.dynamicCasterRevision = clip.dynamicCasterRevision;
            clipConfig.lightForward = {0, 0, 1};
            clipConfig.finestWorldSpan = 2;
            clipConfig.lightDepthMinimum = 0; clipConfig.lightDepthMaximum = 1;
            clipConfig.levelCount = 2;
            const auto cameraClips = buildDirectionalVirtualShadowClips(clipConfig);
            sourceLevels.assign(cameraClips.levels().begin(), cameraClips.levels().end());
        }
        std::vector<PackedDirectionalVirtualShadowClipLevel> packedClips;
        for (const auto& source : sourceLevels) packedClips.push_back(pack(source));
        const std::array buffers{storage.handle};
        const auto shaders = std::filesystem::path(PROJECT_ROOT_DIR) / "assets/shaders";
        VulkanVirtualShadowDepthReceiverPass producer; producer.init(device.logical, buffers, working, shaders);
        VulkanVirtualShadowMarkingPass chain; chain.init(device.logical, buffers, working, config, shaders);
        VulkanVirtualShadowFullViewPass full;
        if (fullView) full.init(device.logical, buffers, working, shaders);
        producer.bindDepth(0, view, sampler, {Width, Height}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (fullView) full.bindDepth(0, view, sampler, {Width, Height}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkQueryPool queryPool{};
        if (device.timestampValidBits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP; query.queryCount = 2;
            require(vkCreateQueryPool(device.logical, &query, nullptr, &queryPool), "depth query pool");
        }
        std::vector<double> durations;
        DirectionalVirtualShadowMarkPlan baselineFull{};
        for (uint32_t iteration = 0; iteration < 9; ++iteration) {
            VirtualShadowDepthReceiverRegion region{Width, Height, 1, 1, 65, 63, iteration == 1};
            if (fullView) region = {Width, Height, 0, 0, Width, Height, iteration == 1};
            glm::mat4 inverse(1);
            // Nontrivial affine camera reconstruction and singular-W rejection.
            inverse[3][0] = 0.125f; inverse[3][1] = -0.0625f;
            if (iteration == 2) inverse[2][3] = 0.125f;
            if (iteration == 3) inverse = glm::mat4(0);
            std::vector<PackedDirectionalVirtualShadowReceiver> expected;
            if (!fullView || iteration < 4)
                expected = buildVirtualShadowDepthReceivers(region, inverse, depth, Width * Height);
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = device.commandPool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
            VkCommandBuffer command{}; require(vkAllocateCommandBuffers(device.logical, &allocate, &command), "depth allocate");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; require(vkBeginCommandBuffer(command, &begin), "depth begin");
            if (iteration == 0) {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image; barrier.subresourceRange = viewInfo.subresourceRange;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
                VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; copy.imageExtent = imageInfo.extent;
                vkCmdCopyBufferToImage(command, upload.handle, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            // Same command-embedded upload used by the live frame-slot path.
            vkCmdUpdateBuffer(command, storage.handle, working.clipLevels.offset,
                packedClips.size() * sizeof(packedClips[0]), packedClips.data());
            if (queryPool) {
                vkCmdResetQueryPool(command, queryPool, 0, 2);
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 0);
            }
            DirectionalVirtualShadowMarkConfig markConfig{};
            markConfig.maximumUniquePageRequests = config.compactedRequestCapacity;
            if (fullView) {
                const auto cells = full.record(command, 0, markConfig, sourceLevels, inverse, region.reverseDepth);
                chain.recordCompaction(command, 0, cells, static_cast<uint32_t>(sourceLevels.size()));
            } else {
                producer.record(command, 0, region, inverse);
                chain.record(command, 0, static_cast<uint32_t>(expected.size()), 1, 0);
            }
            if (queryPool) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
            const VkBufferCopy resultCopy{0, 0, working.totalBytes};
            vkCmdCopyBuffer(command, storage.handle, readback.handle, 1, &resultCopy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
            require(vkEndCommandBuffer(command), "depth end");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            require(vkQueueSubmit(device.queue, 1, &submit, VK_NULL_HANDLE), "depth submit"); require(vkQueueWaitIdle(device.queue), "depth wait");
            if (queryPool && iteration >= 4) {
                std::array<uint64_t, 2> times{};
                require(vkGetQueryPoolResults(device.logical, queryPool, 0, 2,
                    sizeof(times), times.data(), sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "depth chain timestamps");
                uint64_t elapsed = times[1] - times[0];
                if (device.timestampValidBits < 64) elapsed &= (uint64_t{1} << device.timestampValidBits) - 1;
                durations.push_back(double(elapsed) * device.timestampPeriodNanoseconds / 1'000'000.0);
            }
            std::vector<DirectionalVirtualShadowReceiverSample> samples;
            samples.reserve(expected.size());
            for (size_t i = 0; i < expected.size(); ++i) {
                if (!fullView) {
                    PackedDirectionalVirtualShadowReceiver actual{};
                    std::memcpy(&actual, static_cast<std::byte*>(readback.mapped) + working.receivers.offset + i * sizeof(actual), sizeof(actual));
                    if (actual.receiverSamples != expected[i].receiverSamples ||
                        glm::length(glm::vec3(actual.worldPosition - expected[i].worldPosition)) > 1e-6f)
                        throw std::runtime_error("Depth receiver reconstruction mismatch");
                }
                samples.push_back({glm::vec3(expected[i].worldPosition), expected[i].receiverSamples});
            }
            const auto marks = fullView && iteration >= 4 ? baselineFull :
                buildDirectionalVirtualShadowReceiverMarks(markConfig, sourceLevels, samples);
            if (fullView && iteration == 0) baselineFull = marks;
            PackedDirectionalVirtualShadowGpuCompactionTelemetry telemetry{};
            std::memcpy(&telemetry, static_cast<std::byte*>(readback.mapped) + working.telemetry.offset, sizeof(telemetry));
            if (telemetry.uniquePagesBeforeCapacity != marks.uniquePagesBeforeCapacity || telemetry.outputRequestCount != marks.requests.size() ||
                telemetry.requestCapacityOverflow != marks.requestCapacityOverflow || telemetry.requestCapacityDroppedSamples != marks.requestCapacityDroppedSamples)
                throw std::runtime_error("Depth receiver chain telemetry mismatch");
            for (size_t i = 0; i < marks.requests.size(); ++i) {
                PackedDirectionalVirtualShadowGpuRequest actual{};
                std::memcpy(&actual, static_cast<std::byte*>(readback.mapped) + working.outputRequests.offset + i * sizeof(actual), sizeof(actual));
                const auto& request = marks.requests[i];
                if (actual.pageX != request.address.pageX || actual.pageY != request.address.pageY ||
                    actual.receiverSamples != request.receiverSamples || actual.priority != request.priority ||
                    actual.level != request.address.levelOrFace || actual.mip != request.address.mip || actual.requiredLayers != request.requiredLayers)
                    throw std::runtime_error("Depth receiver chain request mismatch");
            }
            vkFreeCommandBuffers(device.logical, device.commandPool, 1, &command);
        }
        full.cleanup(); producer.cleanup(); chain.cleanup(); device.destroy(readback);
        if (queryPool) vkDestroyQueryPool(device.logical, queryPool, nullptr);
        device.destroy(upload); device.destroy(storage);
        vkDestroySampler(device.logical, sampler, nullptr); vkDestroyImageView(device.logical, view, nullptr);
        vkDestroyImage(device.logical, image, nullptr); vkFreeMemory(device.logical, memory, nullptr);
        std::cout << (fullView ? "Full-view D32 direct page accumulation" : "D32 region receiver chain")
            << " matches CPU oracle (forward/reverse, invalid depth, perspective/singular camera)\n";
        if (!durations.empty()) {
            std::ranges::sort(durations);
            std::cout << "D32 " << (fullView ? Width * Height : 4'095) << "-pixel full chain GPU median: "
                << durations[durations.size() / 2] << " ms\n";
        }
    }

    void verifyPersistentChain(Device& device,
        std::span<const Iridium::DirectionalVirtualShadowClipLevel> sourceLevels,
        std::span<const Iridium::DirectionalVirtualShadowReceiverSample> receivers) {
        using namespace Iridium;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device.physical, &properties);
        VirtualShadowResourceConfig config{};
        config.compactedRequestCapacity = receivers.size() == 65'536 ? 4'096 : 32;
        const auto layout = buildVirtualShadowGpuWorkingSetLayout(config,
            properties.limits.minStorageBufferOffsetAlignment);
        std::array<Buffer, 2> slots{device.createDeviceBuffer(layout.totalBytes),
            device.createDeviceBuffer(layout.totalBytes)};
        const std::array buffers{slots[0].handle, slots[1].handle};
        VulkanVirtualShadowMarkingPass pass;
        pass.init(device.logical, buffers, layout, config,
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets/shaders");
        Buffer upload = device.createBuffer(layout.totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer readback = device.createBuffer(layout.totalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        uint32_t coarsest = 0;
        for (size_t i = 0; i < sourceLevels.size(); ++i) {
            const auto level = pack(sourceLevels[i]);
            std::memcpy(static_cast<std::byte*>(upload.mapped) + layout.clipLevels.offset +
                i * sizeof(level), &level, sizeof(level));
            coarsest = (std::max)(coarsest, uint32_t{sourceLevels[i].level});
        }
        // Reuse both immutable descriptor sets across large, small, empty, and
        // populated frames. No raw-mark readback or host compaction occurs.
        const uint32_t fullCount = static_cast<uint32_t>(receivers.size());
        const std::array<uint32_t, 11> counts{fullCount, 1, 0, 513, 0,
            fullCount, fullCount, fullCount, fullCount, fullCount, fullCount};
        VkQueryPool queryPool{};
        if (device.timestampValidBits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP; query.queryCount = 2;
            require(vkCreateQueryPool(device.logical, &query, nullptr, &queryPool), "chain query pool");
        }
        std::vector<double> durations;
        for (uint32_t iteration = 0; iteration < counts.size(); ++iteration) {
            const uint32_t count = counts[iteration];
            for (uint32_t i = 0; i < count; ++i) {
                PackedDirectionalVirtualShadowReceiver receiver{};
                receiver.worldPosition = glm::vec4(receivers[i].worldPosition, 1);
                receiver.receiverSamples = receivers[i].receiverSamples;
                std::memcpy(static_cast<std::byte*>(upload.mapped) + layout.receivers.offset +
                    i * sizeof(receiver), &receiver, sizeof(receiver));
            }
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = device.commandPool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer command{};
            require(vkAllocateCommandBuffers(device.logical, &allocate, &command), "chain allocate");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            require(vkBeginCommandBuffer(command, &begin), "chain begin");
            const uint32_t slot = iteration % 2u;
            std::array<VkBufferCopy, 2> copies{{
                {layout.clipLevels.offset, layout.clipLevels.offset,
                    sourceLevels.size() * sizeof(PackedDirectionalVirtualShadowClipLevel)},
                {layout.receivers.offset, layout.receivers.offset,
                    count * sizeof(PackedDirectionalVirtualShadowReceiver)}}};
            vkCmdCopyBuffer(command, upload.handle, slots[slot].handle,
                count ? 2 : 1, copies.data());
            if (queryPool) {
                vkCmdResetQueryPool(command, queryPool, 0, 2);
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 0);
            }
            pass.record(command, slot, count, static_cast<uint32_t>(sourceLevels.size()), coarsest);
            if (queryPool)
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            copies = {{{layout.outputRequests.offset, layout.outputRequests.offset, layout.outputRequests.size},
                {layout.telemetry.offset, layout.telemetry.offset, layout.telemetry.size}}};
            vkCmdCopyBuffer(command, slots[slot].handle, readback.handle, 2, copies.data());
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            require(vkEndCommandBuffer(command), "chain end");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            require(vkQueueSubmit(device.queue, 1, &submit, VK_NULL_HANDLE), "chain submit");
            require(vkQueueWaitIdle(device.queue), "chain wait");
            if (queryPool && iteration >= 6) {
                std::array<uint64_t, 2> times{};
                require(vkGetQueryPoolResults(device.logical, queryPool, 0, 2,
                    sizeof(times), times.data(), sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "chain timestamps");
                uint64_t elapsed = times[1] - times[0];
                if (device.timestampValidBits < 64)
                    elapsed &= (uint64_t{1} << device.timestampValidBits) - 1u;
                durations.push_back(double(elapsed) * device.timestampPeriodNanoseconds / 1'000'000.0);
            }
            DirectionalVirtualShadowMarkConfig oracleConfig{};
            oracleConfig.maximumUniquePageRequests = config.compactedRequestCapacity;
            const auto expected = buildDirectionalVirtualShadowReceiverMarks(
                oracleConfig, sourceLevels, receivers.first(count));
            PackedDirectionalVirtualShadowGpuCompactionTelemetry telemetry{};
            std::memcpy(&telemetry, static_cast<std::byte*>(readback.mapped) + layout.telemetry.offset,
                sizeof(telemetry));
            if (telemetry.uniquePagesBeforeCapacity != expected.uniquePagesBeforeCapacity ||
                telemetry.outputRequestCount != expected.requests.size() ||
                telemetry.requestCapacityOverflow != expected.requestCapacityOverflow ||
                telemetry.requestCapacityDroppedSamples != expected.requestCapacityDroppedSamples ||
                telemetry.abiMismatchMarks || telemetry.invalidLevelMarks)
                throw std::runtime_error("Persistent mark/compact chain telemetry mismatch");
            for (size_t i = 0; i < expected.requests.size(); ++i) {
                PackedDirectionalVirtualShadowGpuRequest actual{};
                std::memcpy(&actual, static_cast<std::byte*>(readback.mapped) +
                    layout.outputRequests.offset + i * sizeof(actual), sizeof(actual));
                const auto& request = expected.requests[i];
                if (actual.pageX != request.address.pageX || actual.pageY != request.address.pageY ||
                    actual.level != request.address.levelOrFace || actual.mip != request.address.mip ||
                    actual.priority != request.priority || actual.receiverSamples != request.receiverSamples ||
                    actual.requiredLayers != request.requiredLayers)
                    throw std::runtime_error("Persistent mark/compact chain request mismatch");
            }
            vkFreeCommandBuffers(device.logical, device.commandPool, 1, &command);
        }
        pass.cleanup();
        if (queryPool) vkDestroyQueryPool(device.logical, queryPool, nullptr);
        device.destroy(readback); device.destroy(upload);
        for (auto& slot : slots) device.destroy(slot);
        std::cout << "Persistent two-slot GPU mark/compact chain matches oracle across populated/empty reuse\n";
        if (!durations.empty()) {
            std::ranges::sort(durations);
            std::cout << "Persistent " << fullCount << "-receiver marking+compaction GPU median: "
                << durations[durations.size() / 2] << " ms\n";
        }
    }

    double dispatchParallelCompaction(Device& device,
        std::span<const Iridium::DirectionalVirtualShadowClipLevel> sourceLevels,
        std::span<const Iridium::PackedDirectionalVirtualShadowGpuMark> marks,
        uint32_t maximumOutputRequests,
        std::span<Iridium::PackedDirectionalVirtualShadowGpuRequest> output,
        Iridium::PackedDirectionalVirtualShadowGpuCompactionTelemetry& telemetry,
        std::array<double, 5>* stageMilliseconds = nullptr) {
        if (marks.empty() || marks.size() > 65'536 ||
            sourceLevels.empty() || output.empty() ||
            maximumOutputRequests > output.size())
            throw std::runtime_error(
                "Invalid parallel compaction dispatch dimensions");
        const uint32_t scratchCapacity = (std::max)(256u, std::bit_ceil(
            static_cast<uint32_t>(marks.size())));

        std::vector<Iridium::PackedDirectionalVirtualShadowClipLevel> levels;
        levels.reserve(sourceLevels.size());
        for (const auto& source : sourceLevels) levels.push_back(pack(source));
        Buffer levelUpload = device.createBuffer(
            levels.size() * sizeof(levels[0]),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer markUpload = device.createBuffer(
            marks.size() * sizeof(marks[0]),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer outputReadback = device.createBuffer(
            output.size() * sizeof(output[0]),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer telemetryReadback = device.createBuffer(sizeof(telemetry),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer levelBuffer = device.createDeviceBuffer(
            levels.size() * sizeof(levels[0]));
        Buffer markBuffer = device.createDeviceBuffer(
            marks.size() * sizeof(marks[0]));
        Buffer reducedBuffer = device.createDeviceBuffer(
            VkDeviceSize{ scratchCapacity } * sizeof(
                Iridium::PackedDirectionalVirtualShadowGpuRequest));
        Buffer denseBuffer = device.createDeviceBuffer(
            VkDeviceSize{ scratchCapacity } * sizeof(
                Iridium::PackedDirectionalVirtualShadowGpuRequest));
        Buffer outputBuffer = device.createDeviceBuffer(
            output.size() * sizeof(output[0]));
        Buffer telemetryBuffer = device.createDeviceBuffer(sizeof(telemetry));
        VkQueryPool queryPool = VK_NULL_HANDLE;
        if (device.timestampValidBits != 0) {
            VkQueryPoolCreateInfo queryInfo{
                VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 6;
            require(vkCreateQueryPool(device.logical, &queryInfo, nullptr,
                &queryPool), "vkCreateQueryPool(parallel compact)");
        }
        std::memcpy(levelUpload.mapped, levels.data(), levelUpload.size);
        std::memcpy(markUpload.mapped, marks.data(), markUpload.size);

        const std::array infos{
            VkDescriptorBufferInfo{ levelBuffer.handle, 0, levelBuffer.size },
            VkDescriptorBufferInfo{ markBuffer.handle, 0, markBuffer.size },
            VkDescriptorBufferInfo{ reducedBuffer.handle, 0,
                reducedBuffer.size },
            VkDescriptorBufferInfo{ denseBuffer.handle, 0, denseBuffer.size },
            VkDescriptorBufferInfo{ outputBuffer.handle, 0, outputBuffer.size },
            VkDescriptorBufferInfo{ telemetryBuffer.handle, 0,
                telemetryBuffer.size },
        };
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[index].dstSet = device.parallelDescriptorSet;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &infos[index];
        }
        vkUpdateDescriptorSets(device.logical, static_cast<uint32_t>(writes.size()),
            writes.data(), 0, nullptr);

        VkCommandBufferAllocateInfo allocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocation.commandPool = device.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        require(vkAllocateCommandBuffers(device.logical, &allocation, &command),
            "vkAllocateCommandBuffers(parallel compact)");
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        require(vkBeginCommandBuffer(command, &begin),
            "vkBeginCommandBuffer(parallel compact)");
        VkBufferCopy copy{};
        copy.size = levelBuffer.size;
        vkCmdCopyBuffer(command, levelUpload.handle, levelBuffer.handle,
            1, &copy);
        copy.size = markBuffer.size;
        vkCmdCopyBuffer(command, markUpload.handle, markBuffer.handle,
            1, &copy);
        VkMemoryBarrier uploadBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        uploadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        uploadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            1, &uploadBarrier, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            device.parallelPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            device.parallelPipelineLayout, 0, 1,
            &device.parallelDescriptorSet, 0, nullptr);
        if (queryPool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(command, queryPool, 0, 6);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                queryPool, 0);
        }

        auto dispatchStage = [&](uint32_t stage, uint32_t itemCount,
            uint32_t mergeWidth = 0) {
            const std::array<uint32_t, 8> push{
                stage,
                static_cast<uint32_t>(marks.size()),
                static_cast<uint32_t>(sourceLevels.size()),
                scratchCapacity,
                maximumOutputRequests,
                Iridium::VirtualShadowMapAbiVersion,
                mergeWidth,
                0u,
            };
            vkCmdPushConstants(command, device.parallelPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
            vkCmdDispatch(command, (itemCount + 255u) / 256u, 1, 1);
        };
        auto computeBarrier = [&] {
            VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                1, &barrier, 0, nullptr, 0, nullptr);
        };
        auto hierarchicalSort = [&](uint32_t localStage,
            uint32_t mergeAToBStage, uint32_t mergeBToAStage) {
            dispatchStage(localStage, scratchCapacity);
            computeBarrier();
            bool sourceA = true;
            for (uint32_t width = 256u; width < scratchCapacity;
                width <<= 1u) {
                dispatchStage(sourceA ? mergeAToBStage : mergeBToAStage,
                    scratchCapacity, width);
                computeBarrier();
                sourceA = !sourceA;
            }
            if (!sourceA) {
                dispatchStage(5, scratchCapacity);
                computeBarrier();
            }
        };

        dispatchStage(0, scratchCapacity);
        computeBarrier();
        dispatchStage(1, static_cast<uint32_t>(marks.size()));
        computeBarrier();
        if (queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool, 1);
        hierarchicalSort(2, 3, 4);
        if (queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool, 2);
        dispatchStage(6, static_cast<uint32_t>(marks.size()));
        computeBarrier();
        dispatchStage(7, scratchCapacity);
        computeBarrier();
        if (queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool, 3);
        hierarchicalSort(8, 9, 10);
        if (queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool, 4);
        dispatchStage(11, scratchCapacity);
        computeBarrier();
        dispatchStage(12, 256);
        computeBarrier();
        dispatchStage(13, scratchCapacity);
        if (queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                queryPool, 5);

        VkMemoryBarrier readbackBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        readbackBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        readbackBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &readbackBarrier,
            0, nullptr, 0, nullptr);
        copy.size = outputBuffer.size;
        vkCmdCopyBuffer(command, outputBuffer.handle, outputReadback.handle,
            1, &copy);
        copy.size = telemetryBuffer.size;
        vkCmdCopyBuffer(command, telemetryBuffer.handle,
            telemetryReadback.handle, 1, &copy);
        VkMemoryBarrier hostBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier,
            0, nullptr, 0, nullptr);
        require(vkEndCommandBuffer(command),
            "vkEndCommandBuffer(parallel compact)");
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        require(vkQueueSubmit(device.queue, 1, &submit, VK_NULL_HANDLE),
            "vkQueueSubmit(parallel compact)");
        require(vkQueueWaitIdle(device.queue),
            "vkQueueWaitIdle(parallel compact)");
        std::memcpy(output.data(), outputReadback.mapped, outputReadback.size);
        std::memcpy(&telemetry, telemetryReadback.mapped, sizeof(telemetry));
        double gpuMilliseconds = 0.0;
        if (queryPool != VK_NULL_HANDLE) {
            std::array<uint64_t, 6> timestamps{};
            require(vkGetQueryPoolResults(device.logical, queryPool, 0, 6,
                sizeof(timestamps), timestamps.data(), sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "vkGetQueryPoolResults(parallel compact)");
            uint64_t elapsed = timestamps[5] - timestamps[0];
            if (device.timestampValidBits < 64)
                elapsed &= (uint64_t{ 1 } << device.timestampValidBits) - 1u;
            gpuMilliseconds = static_cast<double>(elapsed) *
                device.timestampPeriodNanoseconds / 1'000'000.0;
            if (stageMilliseconds != nullptr) {
                for (size_t index = 0; index < stageMilliseconds->size();
                    ++index) {
                    uint64_t stageElapsed = timestamps[index + 1] -
                        timestamps[index];
                    if (device.timestampValidBits < 64)
                        stageElapsed &=
                            (uint64_t{ 1 } << device.timestampValidBits) - 1u;
                    (*stageMilliseconds)[index] =
                        static_cast<double>(stageElapsed) *
                        device.timestampPeriodNanoseconds / 1'000'000.0;
                }
            }
        }

        vkFreeCommandBuffers(device.logical, device.commandPool, 1, &command);
        device.destroy(telemetryBuffer);
        device.destroy(outputBuffer);
        device.destroy(denseBuffer);
        device.destroy(reducedBuffer);
        device.destroy(markBuffer);
        device.destroy(levelBuffer);
        device.destroy(telemetryReadback);
        device.destroy(outputReadback);
        device.destroy(markUpload);
        device.destroy(levelUpload);
        if (queryPool != VK_NULL_HANDLE)
            vkDestroyQueryPool(device.logical, queryPool, nullptr);
        return gpuMilliseconds;
    }

} // namespace

namespace {
    // Private resources keep the allocator qualification independent of live
    // marking buffers and the renderer's still-experimental page table.
    void verifyResidencyReference(Device& device) {
        using namespace Iridium;
        struct Resources {
            Device& device;
            VkDescriptorSetLayout setLayout{};
            VkPipelineLayout layout{};
            VkDescriptorPool pool{};
            VkShaderModule shader{};
            VkPipeline pipeline{};
            VkCommandBuffer command{};
            VkQueryPool queries{};
            std::array<Buffer,4> buffers{};
            ~Resources() {
                for(auto& buffer:buffers) if(buffer.handle) device.destroy(buffer);
                if(command) vkFreeCommandBuffers(device.logical,device.commandPool,1,&command);
                if(queries) vkDestroyQueryPool(device.logical,queries,nullptr);
                if(pipeline) vkDestroyPipeline(device.logical,pipeline,nullptr);
                if(shader) vkDestroyShaderModule(device.logical,shader,nullptr);
                if(pool) vkDestroyDescriptorPool(device.logical,pool,nullptr);
                if(layout) vkDestroyPipelineLayout(device.logical,layout,nullptr);
                if(setLayout) vkDestroyDescriptorSetLayout(device.logical,setLayout,nullptr);
            }
        } resources{device};
        std::array<VkDescriptorSetLayoutBinding,4> bindings{};
        for(uint32_t i=0;i<4;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount=4;setInfo.pBindings=bindings.data();
        require(vkCreateDescriptorSetLayout(device.logical,&setInfo,nullptr,&resources.setLayout),"residency set layout");
        VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT,0,24};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&resources.setLayout;
        layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&pushRange;
        require(vkCreatePipelineLayout(device.logical,&layoutInfo,nullptr,&resources.layout),"residency pipeline layout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets=1;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&poolSize;
        require(vkCreateDescriptorPool(device.logical,&poolInfo,nullptr,&resources.pool),"residency descriptor pool");
        VkDescriptorSetAllocateInfo setAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocate.descriptorPool=resources.pool;setAllocate.descriptorSetCount=1;setAllocate.pSetLayouts=&resources.setLayout;
        VkDescriptorSet set{};
        require(vkAllocateDescriptorSets(device.logical,&setAllocate,&set),"residency descriptor set");
        std::ifstream file(std::string(PROJECT_ROOT_DIR)+"/assets/shaders/virtual_shadow_residency_reference_comp.spv",std::ios::binary|std::ios::ate);
        if(!file) throw std::runtime_error("Missing residency shader");
        const auto bytes=static_cast<size_t>(file.tellg());
        if(bytes==0 || bytes%4) throw std::runtime_error("Invalid residency shader");
        std::vector<uint32_t> code(bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);
        if(!file) throw std::runtime_error("Truncated residency shader");
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize=bytes;shaderInfo.pCode=code.data();
        require(vkCreateShaderModule(device.logical,&shaderInfo,nullptr,&resources.shader),"residency shader");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,resources.shader,"main",nullptr};
        pipelineInfo.layout=resources.layout;
        require(vkCreateComputePipelines(device.logical,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&resources.pipeline),"residency pipeline");
        constexpr uint32_t StorageCapacity=VirtualShadowResidencyReferenceCapacity+1;
        resources.buffers={device.createBuffer(StorageCapacity*sizeof(PackedVirtualShadowResidencyRequest)),
            device.createBuffer(StorageCapacity*sizeof(PackedVirtualShadowResidentPage)),
            device.createBuffer(StorageCapacity*sizeof(PackedVirtualShadowPageMapping)),
            device.createBuffer(sizeof(PackedVirtualShadowResidencyTelemetry))};
        std::array<VkDescriptorBufferInfo,4> infos{};
        std::array<VkWriteDescriptorSet,4> writes{};
        for(uint32_t i=0;i<4;++i) {
            infos[i]={resources.buffers[i].handle,0,resources.buffers[i].size};
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;
            writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].descriptorCount=1;writes[i].pBufferInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device.logical,4,writes.data(),0,nullptr);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=device.commandPool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        require(vkAllocateCommandBuffers(device.logical,&allocate,&resources.command),"residency command buffer");
        if(device.timestampValidBits) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType=VK_QUERY_TYPE_TIMESTAMP;queryInfo.queryCount=2;
            require(vkCreateQueryPool(device.logical,&queryInfo,nullptr,&resources.queries),"residency timestamps");
        }
        auto execute=[&](const std::array<uint32_t,6>& push) {
            require(vkResetCommandBuffer(resources.command,0),"reset residency command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            require(vkBeginCommandBuffer(resources.command,&begin),"begin residency command");
            if(resources.queries) {
                vkCmdResetQueryPool(resources.command,resources.queries,0,2);
                vkCmdWriteTimestamp(resources.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,resources.queries,0);
            }
            vkCmdBindPipeline(resources.command,VK_PIPELINE_BIND_POINT_COMPUTE,resources.pipeline);
            vkCmdBindDescriptorSets(resources.command,VK_PIPELINE_BIND_POINT_COMPUTE,resources.layout,0,1,&set,0,nullptr);
            vkCmdPushConstants(resources.command,resources.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push.data());
            vkCmdDispatch(resources.command,1,1,1);
            if(resources.queries) vkCmdWriteTimestamp(resources.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,resources.queries,1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(resources.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            require(vkEndCommandBuffer(resources.command),"end residency command");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&resources.command;
            require(vkQueueSubmit(device.queue,1,&submit,VK_NULL_HANDLE),"submit residency command");
            require(vkQueueWaitIdle(device.queue),"wait residency command");
            if(resources.queries) {
                std::array<uint64_t,2> ticks{};
                require(vkGetQueryPoolResults(device.logical,resources.queries,0,2,sizeof(ticks),ticks.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"residency timestamps readback");
                const uint64_t mask=device.timestampValidBits==64 ? UINT64_MAX : (uint64_t{1}<<device.timestampValidBits)-1;
                return double((ticks[1]-ticks[0])&mask)*device.timestampPeriodNanoseconds/1'000'000.0;
            }
            return -1.0;
        };
        auto makeRequest=[](int64_t x,int32_t priority=0) {
            VirtualShadowPageRequest request{};
            request.address.lightOwner=*SceneEntityUuid::parse("019fb73d-5a60-7000-8000-000000000001");
            request.address.projectionRevision=(uint64_t{1}<<40)+7;
            request.address.pageX=x;request.address.pageY=-9;
            request.staticCasterRevision=(uint64_t{1}<<39)+11;request.dynamicCasterRevision=13;
            request.receiverSamples=7;request.priority=priority;return request;
        };
        uint32_t caseCount=0;
        double lastGpuMilliseconds=-1.0;
        auto verify=[&](std::vector<VirtualShadowPageRequest> requests,
            const std::vector<VirtualShadowResidentPage>& current,VirtualShadowPagePolicy policy,uint64_t frame) {
            const auto oracle=buildVirtualShadowPagePlan(policy,requests,current,frame);
            std::ranges::sort(requests,[](const auto& a,const auto& b) {
                if(a.priority!=b.priority) return a.priority>b.priority;
                if(a.receiverSamples!=b.receiverSamples) return a.receiverSamples>b.receiverSamples;
                return a.address<b.address;
            });
            for(auto& buffer:resources.buffers) std::memset(buffer.mapped,0,buffer.size);
            auto* packedRequests=static_cast<PackedVirtualShadowResidencyRequest*>(resources.buffers[0].mapped);
            auto* packedResidents=static_cast<PackedVirtualShadowResidentPage*>(resources.buffers[1].mapped);
            for(size_t i=0;i<requests.size();++i) packedRequests[i]=packVirtualShadowResidencyRequest(requests[i]);
            for(const auto& resident:current) packedResidents[resident.physicalPage]=packVirtualShadowResidentPage(resident,policy.maximumPhysicalPages);
            lastGpuMilliseconds=execute({uint32_t(requests.size()),policy.maximumPhysicalPages,policy.maximumPageUpdatesPerFrame,
                uint32_t(policy.cacheStaticCasters)|(uint32_t(policy.deterministicConventionalFallback)<<1),uint32_t(frame),uint32_t(frame>>32)});
            const auto& telemetry=*static_cast<PackedVirtualShadowResidencyTelemetry*>(resources.buffers[3].mapped);
            const PackedVirtualShadowResidencyTelemetry expected{oracle.uniqueRequests,oracle.cacheHits,oracle.pagesAllocated,
                oracle.pagesEvicted,oracle.pagesToRender,oracle.missingPages,oracle.updateBudgetOverflow,oracle.physicalPoolOverflow};
            if(std::memcmp(&telemetry,&expected,sizeof(expected))) throw std::runtime_error("Residency GPU telemetry mismatch");
            const auto* mappings=static_cast<const PackedVirtualShadowPageMapping*>(resources.buffers[2].mapped);
            for(size_t i=0;i<requests.size();++i) {
                const auto actual=unpackVirtualShadowPageMapping(mappings[i],policy.maximumPhysicalPages);
                const auto found=std::ranges::find(oracle.mappings,actual.address,&VirtualShadowPageMapping::address);
                if(found==oracle.mappings.end() || actual.staticCasterRevision!=found->staticCasterRevision ||
                    actual.dynamicCasterRevision!=found->dynamicCasterRevision || actual.physicalPage!=found->physicalPage ||
                    actual.receiverSamples!=found->receiverSamples || actual.state!=found->state || actual.missingAction!=found->missingAction ||
                    actual.requiredLayers!=found->requiredLayers || actual.updateLayers!=found->updateLayers)
                    throw std::runtime_error("Residency GPU mapping mismatch");
            }
            size_t occupied=0;
            for(uint32_t i=0;i<policy.maximumPhysicalPages;++i) if(packedResidents[i].key.abiVersion!=0) {
                ++occupied;
                const auto found=std::ranges::find(oracle.nextResidency,i,&VirtualShadowResidentPage::physicalPage);
                if(found==oracle.nextResidency.end()) throw std::runtime_error("Unexpected GPU resident");
                const auto expectedResident=packVirtualShadowResidentPage(*found,policy.maximumPhysicalPages);
                if(std::memcmp(&packedResidents[i],&expectedResident,sizeof(expectedResident))) throw std::runtime_error("Residency GPU content mismatch");
            }
            if(occupied!=oracle.nextResidency.size()) throw std::runtime_error("GPU resident count mismatch");
            ++caseCount;
            return oracle;
        };
        VirtualShadowPagePolicy policy{};policy.maximumPhysicalPages=4;policy.maximumPageUpdatesPerFrame=4;
        const uint64_t frame=(uint64_t{1}<<40)+17;
        auto a=makeRequest(-4),b=makeRequest(int64_t{1}<<34,1),c=makeRequest(-(int64_t{1}<<34),2);
        auto initial=verify({a,b,c},{},policy,frame);
        const std::array<uint32_t,3> complete{0,1,2};
        // This CPU completion constructs a known raster-complete fixture only;
        // the GPU test does not claim that any atlas content has been rendered.
        commitVirtualShadowPageUpdates(initial,complete,frame);
        verify({a,b,c},initial.nextResidency,policy,frame+1);
        auto staticChange=a;staticChange.staticCasterRevision++;
        auto dynamicChange=b;dynamicChange.dynamicCasterRevision++;
        verify({staticChange,dynamicChange,c},initial.nextResidency,policy,frame+2);
        policy.maximumPageUpdatesPerFrame=1;
        verify({staticChange,dynamicChange,makeRequest(100,3)},initial.nextResidency,policy,frame+3);
        policy.maximumPageUpdatesPerFrame=4;policy.cacheStaticCasters=false;
        verify({a,b,c},initial.nextResidency,policy,frame+4);
        policy.cacheStaticCasters=true;policy.deterministicConventionalFallback=false;
        policy.maximumPhysicalPages=3;policy.maximumPageUpdatesPerFrame=3;
        verify({a,b,c,makeRequest(99,7)},initial.nextResidency,policy,frame+5);
        auto victims=initial.nextResidency;
        for(auto& victim:victims) {victim.lastPriority=0;victim.lastUsedFrame=frame-1;}
        victims[1].lastUsedFrame=frame-(uint64_t{1}<<33);
        verify({a,makeRequest(99,7)},victims,policy,frame+6);
        verify({},victims,policy,frame+7);
        auto spot=a,point=a,otherOwner=a,otherRevision=a,otherMip=a,otherLevel=a,otherY=a;
        spot.address.projection=VirtualShadowProjection::Spot;
        point.address.projection=VirtualShadowProjection::PointFace;point.address.levelOrFace=5;
        otherOwner.address.lightOwner=*SceneEntityUuid::parse("019fb73d-5a60-7000-8000-000000000002");
        otherRevision.address.projectionRevision++;otherMip.address.mip=65535;
        otherLevel.address.levelOrFace=255;otherY.address.pageY=std::numeric_limits<int64_t>::min();
        policy.maximumPhysicalPages=8;policy.maximumPageUpdatesPerFrame=8;
        verify({spot,point,otherOwner,otherRevision,otherMip,otherLevel,otherY,a},{},policy,frame+8);
        std::vector<VirtualShadowPageRequest> capacityRequests;
        for(uint32_t i=0;i<VirtualShadowResidencyReferenceCapacity;++i) capacityRequests.push_back(makeRequest(int64_t(i)-128));
        policy.maximumPhysicalPages=VirtualShadowResidencyReferenceCapacity;policy.maximumPageUpdatesPerFrame=64;
        std::array<double,5> durations{};
        for(auto& duration:durations) {
            verify(capacityRequests,{},policy,frame+9);duration=lastGpuMilliseconds;
        }
        std::ranges::sort(durations);
        std::cout<<"Serial residency reference 256 requests/256 slots, 64 updates GPU median: "<<durations[2]<<" ms\n";
        // Rejection must not partially mutate residency; mapping output is
        // deliberately unspecified when invalidInput is set.
        const auto saved=std::vector<std::byte>(resources.buffers[1].size);
        std::vector<std::byte> before=saved;
        std::memcpy(before.data(),resources.buffers[1].mapped,before.size());
        auto* input=static_cast<PackedVirtualShadowResidencyRequest*>(resources.buffers[0].mapped);
        input[0]=packVirtualShadowResidencyRequest(a);input[1]=input[0];
        execute({2,3,3,3,uint32_t(frame),uint32_t(frame>>32)});
        auto checkRejected=[&] {
            const auto* telemetry=static_cast<PackedVirtualShadowResidencyTelemetry*>(resources.buffers[3].mapped);
            if(telemetry->invalidInput!=1 || std::memcmp(before.data(),resources.buffers[1].mapped,before.size()))
                throw std::runtime_error("Invalid residency input mutated state");
        };
        checkRejected();
        input[0].key.abiVersion=77;
        execute({1,3,3,3,uint32_t(frame),uint32_t(frame>>32)});checkRejected();
        input[0]=packVirtualShadowResidencyRequest(a);input[1]=packVirtualShadowResidencyRequest(c);
        execute({2,3,3,3,uint32_t(frame),uint32_t(frame>>32)});checkRejected();
        execute({0,StorageCapacity,64,3,uint32_t(frame),uint32_t(frame>>32)});checkRejected();
        execute({StorageCapacity,256,64,3,uint32_t(frame),uint32_t(frame>>32)});checkRejected();
        std::cout<<"GPU residency reference: "<<caseCount<<" oracle dispatches and 5 atomic rejection cases passed\n";
    }
}

int main(int argc, char** argv) {
    using namespace Iridium;
    try {
        Device device;
        verifyResidencyReference(device);
        if(argc==2 && std::string(argv[1])=="--residency-only") {
            if(validationErrors.load()!=0) throw std::runtime_error("Vulkan residency validation reported errors");
            return 0;
        }
        {
            auto clip = level(0);
            clip.virtualResolutionTexels = 1024;
            clip.worldPageOriginX = -4; clip.worldPageOriginY = -3;
            const VirtualShadowPageMapping mapping{
                .address = {.lightOwner = clip.lightOwner, .projectionRevision = clip.projectionRevision},
                .staticCasterRevision = clip.staticCasterRevision,
                .dynamicCasterRevision = clip.dynamicCasterRevision,
                .physicalPage = 5, .state = VirtualShadowPageState::PendingRaster,
                .requiredLayers = VirtualShadowPageLayerAll, .updateLayers = VirtualShadowPageLayerAll};
            const auto atlas = buildVirtualShadowAtlasLayout(128, 4, 10, 4096);
            const auto raster = buildDirectionalVirtualShadowPageRasterRegion(atlas, clip, mapping);
            // Reuse the independent GPU projector to test the exact cropped
            // homogeneous volume, including border-only points and depth planes.
            auto cropped = clip;
            cropped.worldToShadowClip = raster.worldToPageClip;
            cropped.virtualResolutionTexels = 128;
            cropped.worldPageOriginX = cropped.worldPageOriginY = 0;
            const std::array samples{
                DirectionalVirtualShadowReceiverSample{{0.125f, -0.125f, 0.5f}, 1},
                DirectionalVirtualShadowReceiverSample{{-0.006f, -0.125f, 0.5f}, 1},
                DirectionalVirtualShadowReceiverSample{{-0.01f, -0.125f, 0.5f}, 1},
                DirectionalVirtualShadowReceiverSample{{0.125f, -0.125f, -0.1f}, 1},
                DirectionalVirtualShadowReceiverSample{{0.125f, -0.125f, 1.1f}, 1}};
            std::array<PackedDirectionalVirtualShadowGpuMark, samples.size()> projected{};
            dispatch(device, std::span(&cropped, 1), samples, projected);
            for (size_t i = 0; i < samples.size(); ++i) {
                const bool cpuInside = virtualShadowPageMayContainCaster(raster, {samples[i].worldPosition, 0});
                const bool gpuInside = projected[i].status == DirectionalVirtualShadowGpuMarkStatus::Mapped;
                if (cpuInside != gpuInside)
                    throw std::runtime_error("Cropped page volume disagrees with independent GPU projection");
            }
            --clip.worldPageOriginX; clip.worldToShadowClip[3][0] += 0.25f;
            cropped.worldToShadowClip = buildDirectionalVirtualShadowPageRasterRegion(atlas, clip, mapping).worldToPageClip;
            std::array<PackedDirectionalVirtualShadowGpuMark, samples.size()> scrolled{};
            dispatch(device, std::span(&cropped, 1), samples, scrolled);
            for (size_t i = 0; i < samples.size(); ++i)
                if (projected[i].status != scrolled[i].status || projected[i].pageX != scrolled[i].pageX ||
                    projected[i].pageY != scrolled[i].pageY)
                    throw std::runtime_error("Cropped page GPU coverage changed during snapped clip scroll");
            std::cout << "Page crop/border/depth volume matches independent GPU projection and survives clip scroll\n";
        }
        std::vector<DirectionalVirtualShadowClipLevel> levels{
            level(0), level(1)
        };
        const std::vector<DirectionalVirtualShadowReceiverSample> receivers{
            { .worldPosition = { 0.0f, 0.0f, 0.5f }, .receiverSamples = 2 },
            { .worldPosition = { 0.9f, 0.0f, 0.5f }, .receiverSamples = 3 },
            { .worldPosition = { 0.0f, 0.0f, -0.1f }, .receiverSamples = 5 },
            { .worldPosition = {
                std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.5f },
                .receiverSamples = 7 },
        };
        std::vector<PackedDirectionalVirtualShadowGpuMark> marks(receivers.size());
        dispatch(device, levels, receivers, marks);
        const auto center = buildDirectionalVirtualShadowReceiverMarks(
            {}, levels, std::span(receivers.data(), 1));
        const auto edge = buildDirectionalVirtualShadowReceiverMarks(
            {}, levels, std::span(receivers.data() + 1, 1));
        if (center.requests.size() != 1 || edge.requests.size() != 1 ||
            marks[0].status != DirectionalVirtualShadowGpuMarkStatus::Mapped ||
            marks[0].pageX != center.requests[0].address.pageX ||
            marks[0].pageY != center.requests[0].address.pageY ||
            marks[0].selectedLevelIndex != 0 ||
            marks[1].status != DirectionalVirtualShadowGpuMarkStatus::Mapped ||
            marks[1].pageX != edge.requests[0].address.pageX ||
            marks[1].pageY != edge.requests[0].address.pageY ||
            marks[1].selectedLevelIndex != 1 ||
            marks[2].status != DirectionalVirtualShadowGpuMarkStatus::Unmapped ||
            marks[3].status !=
                DirectionalVirtualShadowGpuMarkStatus::InvalidReceiver)
            throw std::runtime_error("GPU/CPU receiver marking mismatch");

        const DirectionalVirtualShadowReceiverSample stableReceiver{
            .worldPosition = { 0.25f, 0.0f, 0.5f }, .receiverSamples = 1 };
        std::array<PackedDirectionalVirtualShadowGpuMark, 1> baseline{};
        dispatch(device, levels, std::span(&stableReceiver, 1), baseline);
        for (auto& clip : levels) {
            clip.worldToShadowClip[3][0] = -0.25f;
            clip.worldPageOriginX = 1;
        }
        std::array<PackedDirectionalVirtualShadowGpuMark, 1> scrolled{};
        dispatch(device, levels, std::span(&stableReceiver, 1), scrolled);
        if (baseline[0].pageX != scrolled[0].pageX ||
            baseline[0].pageY != scrolled[0].pageY ||
            baseline[0].status != DirectionalVirtualShadowGpuMarkStatus::Mapped ||
            scrolled[0].status != DirectionalVirtualShadowGpuMarkStatus::Mapped)
            throw std::runtime_error("GPU clip scroll changed world-page identity");

        levels = { level(0), level(1) };
        const std::vector<DirectionalVirtualShadowReceiverSample>
            compactionReceivers{
                { .worldPosition = { 0.0f, 0.0f, 0.5f },
                    .receiverSamples = 2 },
                { .worldPosition = { 0.02f, 0.02f, 0.5f },
                    .receiverSamples = 3 },
                { .worldPosition = { 0.75f, 0.0f, 0.5f },
                    .receiverSamples = 4 },
                { .worldPosition = { -0.75f, 0.0f, 0.5f },
                    .receiverSamples = 1 },
                { .worldPosition = { 0.9f, 0.0f, 0.5f },
                    .receiverSamples = 6 },
            };
        std::vector<PackedDirectionalVirtualShadowGpuMark> compactionMarks(
            compactionReceivers.size());
        dispatch(device, levels, compactionReceivers, compactionMarks);
        DirectionalVirtualShadowMarkConfig compactConfig{};
        compactConfig.maximumUniquePageRequests = 2;
        const DirectionalVirtualShadowMarkPlan cpuCompaction =
            buildDirectionalVirtualShadowReceiverMarks(compactConfig, levels,
                compactionReceivers);
        std::array<PackedDirectionalVirtualShadowGpuRequest, 2> gpuRequests{};
        PackedDirectionalVirtualShadowGpuCompactionTelemetry gpuTelemetry{};
        dispatchCompaction(device, levels, compactionMarks,
            compactConfig.maximumUniquePageRequests, gpuRequests, gpuTelemetry);
        if (gpuTelemetry.uniquePagesBeforeCapacity !=
                cpuCompaction.uniquePagesBeforeCapacity ||
            gpuTelemetry.outputRequestCount != cpuCompaction.requests.size() ||
            gpuTelemetry.requestCapacityOverflow !=
                cpuCompaction.requestCapacityOverflow ||
            gpuTelemetry.requestCapacityDroppedSamples !=
                cpuCompaction.requestCapacityDroppedSamples)
            throw std::runtime_error("GPU compaction telemetry mismatch");
        for (size_t index = 0; index < cpuCompaction.requests.size(); ++index) {
            const auto& gpu = gpuRequests[index];
            const auto& cpu = cpuCompaction.requests[index];
            if (gpu.pageX != cpu.address.pageX ||
                gpu.pageY != cpu.address.pageY ||
                gpu.level != cpu.address.levelOrFace ||
                gpu.mip != cpu.address.mip ||
                gpu.receiverSamples != cpu.receiverSamples ||
                gpu.priority != cpu.priority ||
                gpu.requiredLayers != cpu.requiredLayers)
                throw std::runtime_error("GPU compacted request mismatch");
        }
        std::array<PackedDirectionalVirtualShadowGpuRequest, 2>
            parallelRequests{};
        PackedDirectionalVirtualShadowGpuCompactionTelemetry
            parallelTelemetry{};
        dispatchParallelCompaction(device, levels, compactionMarks,
            compactConfig.maximumUniquePageRequests, parallelRequests,
            parallelTelemetry);
        if (std::memcmp(parallelRequests.data(), gpuRequests.data(),
                sizeof(gpuRequests)) != 0 ||
            std::memcmp(&parallelTelemetry, &gpuTelemetry,
                sizeof(gpuTelemetry)) != 0)
            throw std::runtime_error(
                "Parallel GPU compaction disagrees with reference");

        auto invalidMarks = std::array{
            compactionMarks[0], compactionMarks[1] };
        ++invalidMarks[0].abiVersion;
        invalidMarks[1].selectedLevelIndex = 99;
        gpuRequests = {};
        gpuTelemetry = {};
        dispatchCompaction(device, levels, invalidMarks, 2, gpuRequests,
            gpuTelemetry);
        if (gpuTelemetry.outputRequestCount != 0 ||
            gpuTelemetry.uniquePagesBeforeCapacity != 0 ||
            gpuTelemetry.abiMismatchMarks != 1 ||
            gpuTelemetry.invalidLevelMarks != 1)
            throw std::runtime_error("GPU compaction rejection mismatch");
        parallelRequests = {};
        parallelTelemetry = {};
        dispatchParallelCompaction(device, levels, invalidMarks, 2,
            parallelRequests, parallelTelemetry);
        if (std::memcmp(&parallelTelemetry, &gpuTelemetry,
                sizeof(gpuTelemetry)) != 0)
            throw std::runtime_error(
                "Parallel GPU compaction rejection mismatch");

        const std::array saturationReceivers{
            DirectionalVirtualShadowReceiverSample{
                .worldPosition = { 0.0f, 0.0f, 0.5f },
                .receiverSamples = std::numeric_limits<uint32_t>::max() },
            DirectionalVirtualShadowReceiverSample{
                .worldPosition = { 0.0f, 0.0f, 0.5f },
                .receiverSamples = 1 },
        };
        std::array<PackedDirectionalVirtualShadowGpuMark, 2> saturationMarks{};
        dispatch(device, levels, saturationReceivers, saturationMarks);
        std::array<PackedDirectionalVirtualShadowGpuRequest, 1>
            saturationOutput{};
        gpuTelemetry = {};
        dispatchCompaction(device, levels, saturationMarks, 1,
            saturationOutput, gpuTelemetry);
        if (saturationOutput[0].receiverSamples !=
                std::numeric_limits<uint32_t>::max() ||
            gpuTelemetry.outputRequestCount != 1)
            throw std::runtime_error("GPU compaction did not saturate samples");
        std::array<PackedDirectionalVirtualShadowGpuRequest, 1>
            parallelSaturationOutput{};
        parallelTelemetry = {};
        dispatchParallelCompaction(device, levels, saturationMarks, 1,
            parallelSaturationOutput, parallelTelemetry);
        if (parallelSaturationOutput[0].receiverSamples !=
                std::numeric_limits<uint32_t>::max() ||
            parallelTelemetry.outputRequestCount != 1)
            throw std::runtime_error(
                "Parallel GPU compaction did not saturate samples");

        auto orderingLevels = levels;
        orderingLevels[0].level = 1;
        orderingLevels[0].mip = 0;
        orderingLevels[1].level = 0;
        orderingLevels[1].mip = 1;
        orderingLevels[1].priority = orderingLevels[0].priority;
        std::array<PackedDirectionalVirtualShadowGpuMark, 2> orderingMarks{};
        for (uint32_t index = 0; index < orderingMarks.size(); ++index) {
            orderingMarks[index].pageX = 4;
            orderingMarks[index].pageY = 4;
            orderingMarks[index].selectedLevelIndex =
                static_cast<int32_t>(index);
            orderingMarks[index].status =
                DirectionalVirtualShadowGpuMarkStatus::Mapped;
            orderingMarks[index].receiverSamples = 1;
        }
        std::array<PackedDirectionalVirtualShadowGpuRequest, 2>
            orderingOutput{};
        gpuTelemetry = {};
        dispatchCompaction(device, orderingLevels, orderingMarks, 2,
            orderingOutput, gpuTelemetry);
        if (orderingOutput[0].mip != 0 || orderingOutput[0].level != 1 ||
            orderingOutput[1].mip != 1 || orderingOutput[1].level != 0)
            throw std::runtime_error("GPU compaction address order mismatch");
        std::array<PackedDirectionalVirtualShadowGpuRequest, 2>
            parallelOrderingOutput{};
        parallelTelemetry = {};
        dispatchParallelCompaction(device, orderingLevels, orderingMarks, 2,
            parallelOrderingOutput, parallelTelemetry);
        if (std::memcmp(parallelOrderingOutput.data(), orderingOutput.data(),
                sizeof(orderingOutput)) != 0)
            throw std::runtime_error(
                "Parallel GPU compaction address order mismatch");

        std::vector<DirectionalVirtualShadowReceiverSample> stressReceivers;
        stressReceivers.reserve(4'096);
        for (uint32_t index = 0; index < 4'096; ++index) {
            const int32_t x = static_cast<int32_t>(index % 47u) - 23;
            const int32_t y = static_cast<int32_t>((index / 47u) % 47u) - 23;
            stressReceivers.push_back({
                .worldPosition = {
                    static_cast<float>(x) / 31.0f,
                    static_cast<float>(y) / 31.0f,
                    0.5f },
                .receiverSamples = 1u + (index % 7u),
            });
        }
        std::vector<PackedDirectionalVirtualShadowGpuMark> stressMarks(
            stressReceivers.size());
        DirectionalVirtualShadowClipConfig cameraClipConfig{};
        cameraClipConfig.lightOwner = levels.front().lightOwner;
        cameraClipConfig.projectionRevision = 71;
        cameraClipConfig.lightForward = {0, 0, 1};
        cameraClipConfig.finestWorldSpan = 2;
        cameraClipConfig.lightDepthMinimum = 0;
        cameraClipConfig.lightDepthMaximum = 1;
        cameraClipConfig.levelCount = 2;
        const std::array cameraReceivers{
            DirectionalVirtualShadowReceiverSample{{0.1f, 0.1f, 0.5f}, 1},
            DirectionalVirtualShadowReceiverSample{{0.999f, 0.1f, 0.5f}, 2},
            DirectionalVirtualShadowReceiverSample{{3, 0, 0.5f}, 1}};
        std::array<PackedDirectionalVirtualShadowGpuMark, 3> cameraMarks{};
        for (const float focusX : {0.0f, 0.001f, 0.02f, -0.02f}) {
            cameraClipConfig.focusWorld.x = focusX;
            const auto cameraPlan = buildDirectionalVirtualShadowClips(cameraClipConfig);
            dispatch(device, cameraPlan.levels(), cameraReceivers, cameraMarks);
            for (size_t i = 0; i < cameraReceivers.size(); ++i) {
                const auto oracle = buildDirectionalVirtualShadowReceiverMarks({}, cameraPlan.levels(),
                    std::span(cameraReceivers.data() + i, 1));
                if (oracle.requests.empty()) {
                    if (cameraMarks[i].status != DirectionalVirtualShadowGpuMarkStatus::Unmapped)
                        throw std::runtime_error("Camera clip outside-coverage mismatch");
                } else {
                    const auto& address = oracle.requests.front().address;
                    if (cameraMarks[i].status != DirectionalVirtualShadowGpuMarkStatus::Mapped ||
                        cameraMarks[i].pageX != address.pageX || cameraMarks[i].pageY != address.pageY ||
                        cameraPlan.clips[cameraMarks[i].selectedLevelIndex].level != address.levelOrFace)
                        throw std::runtime_error("Camera clip scroll/guard GPU parity mismatch");
                }
            }
        }
        std::cout << "Camera-driven clips match GPU oracle across subpage/positive/negative scroll and coarse guard fallback\n";
        verifyDepthReceiverChain(device);
        verifyDepthReceiverChain(device, true);
        verifyDepthReceiverChain(device, true, 3'840, 2'160);
        verifyPersistentChain(device, levels, stressReceivers);
        auto fullLevel = level(0);
        fullLevel.virtualResolutionTexels = 32'768;
        std::vector<DirectionalVirtualShadowReceiverSample> fullReceivers(65'536);
        for (uint32_t i = 0; i < fullReceivers.size(); ++i) {
            fullReceivers[i].worldPosition = {
                (float(i % 256u) + 0.5f) / 128.0f - 1.0f,
                (float(i / 256u) + 0.5f) / 128.0f - 1.0f, 0.5f};
            fullReceivers[i].receiverSamples = 1;
        }
        verifyPersistentChain(device, std::span(&fullLevel, 1), fullReceivers);
        dispatch(device, levels, stressReceivers, stressMarks);
        DirectionalVirtualShadowMarkConfig stressConfig{};
        stressConfig.maximumUniquePageRequests = 32;
        const auto stressCpu = buildDirectionalVirtualShadowReceiverMarks(
            stressConfig, levels, stressReceivers);
        std::vector<PackedDirectionalVirtualShadowGpuRequest> stressOutput(
            stressConfig.maximumUniquePageRequests);
        parallelTelemetry = {};
        dispatchParallelCompaction(device, levels, stressMarks,
            stressConfig.maximumUniquePageRequests, stressOutput,
            parallelTelemetry);
        if (parallelTelemetry.uniquePagesBeforeCapacity !=
                stressCpu.uniquePagesBeforeCapacity ||
            parallelTelemetry.outputRequestCount != stressCpu.requests.size() ||
            parallelTelemetry.requestCapacityOverflow !=
                stressCpu.requestCapacityOverflow ||
            parallelTelemetry.requestCapacityDroppedSamples !=
                stressCpu.requestCapacityDroppedSamples)
            throw std::runtime_error(
                "Parallel GPU stress telemetry mismatch");
        for (size_t index = 0; index < stressCpu.requests.size(); ++index) {
            const auto& gpu = stressOutput[index];
            const auto& cpu = stressCpu.requests[index];
            if (gpu.pageX != cpu.address.pageX ||
                gpu.pageY != cpu.address.pageY ||
                gpu.level != cpu.address.levelOrFace ||
                gpu.mip != cpu.address.mip ||
                gpu.receiverSamples != cpu.receiverSamples ||
                gpu.priority != cpu.priority ||
                gpu.requiredLayers != cpu.requiredLayers)
                throw std::runtime_error(
                    "Parallel GPU stress request mismatch");
        }

        std::vector<PackedDirectionalVirtualShadowGpuMark> capacityMarks(
            65'536);
        for (uint32_t index = 0; index < capacityMarks.size(); ++index) {
            auto& mark = capacityMarks[index];
            mark.pageX = static_cast<int32_t>(index % 256u) - 128;
            mark.pageY = static_cast<int32_t>(index / 256u) - 128;
            mark.selectedLevelIndex = static_cast<int32_t>(index & 1u);
            mark.status = DirectionalVirtualShadowGpuMarkStatus::Mapped;
            mark.receiverSamples = 1;
        }
        constexpr uint32_t CapacityOutputCount = 4'096;
        std::vector<PackedDirectionalVirtualShadowGpuRequest> capacityOutput(
            CapacityOutputCount);
        dispatchParallelCompaction(device, levels, capacityMarks,
            CapacityOutputCount, capacityOutput, parallelTelemetry);
        std::vector<double> capacityDurations;
        std::array<std::vector<double>, 5> capacityStageDurations{};
        capacityDurations.reserve(5);
        for (uint32_t run = 0; run < 5; ++run) {
            parallelTelemetry = {};
            std::array<double, 5> stages{};
            capacityDurations.push_back(dispatchParallelCompaction(device,
                levels, capacityMarks, CapacityOutputCount, capacityOutput,
                parallelTelemetry, &stages));
            for (size_t stage = 0; stage < stages.size(); ++stage)
                capacityStageDurations[stage].push_back(stages[stage]);
        }
        std::ranges::sort(capacityDurations);
        std::array<double, 5> capacityStageMedians{};
        for (size_t stage = 0; stage < capacityStageDurations.size(); ++stage) {
            std::ranges::sort(capacityStageDurations[stage]);
            capacityStageMedians[stage] = capacityStageDurations[stage][2];
        }
        if (parallelTelemetry.uniquePagesBeforeCapacity != 65'536 ||
            parallelTelemetry.outputRequestCount != CapacityOutputCount ||
            parallelTelemetry.requestCapacityOverflow !=
                65'536 - CapacityOutputCount ||
            parallelTelemetry.requestCapacityDroppedSamples !=
                65'536 - CapacityOutputCount)
            throw std::runtime_error(
                "Parallel GPU capacity telemetry mismatch");
        std::cout << "Parallel 65,536-mark compaction GPU median: "
            << capacityDurations[capacityDurations.size() / 2] << " ms\n";
        std::cout << "  project=" << capacityStageMedians[0]
            << " address-sort=" << capacityStageMedians[1]
            << " reduce/prepare=" << capacityStageMedians[2]
            << " rank-sort=" << capacityStageMedians[3]
            << " finalize=" << capacityStageMedians[4] << " ms\n";
        if (validationErrors.load() != 0)
            throw std::runtime_error("Vulkan validation reported errors");
        std::cout << "GPU directional marking and reference/parallel bounded "
            "compaction match the CPU oracle\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
