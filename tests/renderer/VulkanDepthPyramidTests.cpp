#include "renderer/rhi/DepthPyramid.h"
#include <vulkan/vulkan.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan failure: " + std::to_string(result));
}
std::atomic<unsigned> validationErrors = 0;
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    std::cerr << data->pMessage << '\n';
    return VK_FALSE;
}

// Headless hardware qualification, deliberately separate from portable unit tests.
struct Device {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool commands{};
    VkDescriptorPool descriptors{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorSetLayout querySetLayout{};
    VkPipelineLayout queryLayout{};
    VkPipeline queryPipeline{};
    VkSampler sampler{};
    VkShaderModule shader{};
    VkShaderModule queryShader{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t family = UINT32_MAX;

    void init() {
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const std::array extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Iridium Hi-Z parity";
        app.apiVersion = VK_API_VERSION_1_1;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback = message;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledLayerCount = 1; create.ppEnabledLayerNames = &layer;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size()); create.ppEnabledExtensionNames = extensions.data();
        const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount = 1; validation.pEnabledValidationFeatures = &sync;
        validation.pNext = &debug;
        create.pNext = &validation;
        check(vkCreateInstance(&create, nullptr, &instance));
        check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,
            "vkCreateDebugUtilsMessengerEXT"))(instance, &debug, nullptr, &messenger));
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            uint32_t size = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &size, nullptr);
            std::vector<VkQueueFamilyProperties> families(size);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &size, families.data());
            for (uint32_t i = 0; i < size; ++i) if ((families[i].queueFlags &
                (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) {
                physical = candidate; family = i; break;
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("No compute device");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cout << "Device: " << properties.deviceName << '\n';
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &pool, nullptr, &commands));
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 2; set.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &set, nullptr, &setLayout));
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,64},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,32},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,64}};
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets = 64; descriptorInfo.poolSizeCount = static_cast<uint32_t>(sizes.size()); descriptorInfo.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &descriptorInfo, nullptr, &descriptors));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &setLayout;
        pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &layout));
        std::ifstream file(std::string(PROJECT_ROOT_DIR) + "/assets/shaders/depth_pyramid_comp.spv", std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Missing depth pyramid shader");
        const auto bytes = static_cast<size_t>(file.tellg());
        if (!bytes || bytes % 4) throw std::runtime_error("Invalid shader size");
        std::vector<uint32_t> code(bytes/4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), bytes);
        if (!file) throw std::runtime_error("Incomplete shader read");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = bytes; module.pCode = code.data();
        check(vkCreateShaderModule(device, &module, nullptr, &shader));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        compute.layout = layout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline));

        const std::array queryBindings{
            VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = static_cast<uint32_t>(queryBindings.size());
        set.pBindings = queryBindings.data();
        check(vkCreateDescriptorSetLayout(device, &set, nullptr, &querySetLayout));
        VkPushConstantRange queryPush{VK_SHADER_STAGE_COMPUTE_BIT,0,12};
        pipelineLayout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &querySetLayout;
        pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &queryPush;
        check(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &queryLayout));
        std::ifstream queryFile(std::string(PROJECT_ROOT_DIR) +
            "/assets/shaders/depth_pyramid_query_comp.spv", std::ios::binary | std::ios::ate);
        if (!queryFile) throw std::runtime_error("Missing depth pyramid query shader");
        const auto queryBytes = static_cast<size_t>(queryFile.tellg());
        if (!queryBytes || queryBytes % 4) throw std::runtime_error("Invalid query shader size");
        std::vector<uint32_t> queryCode(queryBytes / 4);
        queryFile.seekg(0); queryFile.read(reinterpret_cast<char*>(queryCode.data()), queryBytes);
        if (!queryFile) throw std::runtime_error("Incomplete query shader read");
        module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = queryBytes; module.pCode = queryCode.data();
        check(vkCreateShaderModule(device, &module, nullptr, &queryShader));
        compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
            VK_SHADER_STAGE_COMPUTE_BIT,queryShader,"main",nullptr};
        compute.layout = queryLayout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr,
            &queryPipeline));
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sample.magFilter = sample.minFilter = VK_FILTER_NEAREST;
        sample.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sample.addressModeU = sample.addressModeV = sample.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device, &sample, nullptr, &sampler));
    }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u<<i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("Required memory type unavailable");
    }
    ~Device() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroySampler(device,sampler,nullptr); vkDestroyPipeline(device,pipeline,nullptr);
            vkDestroyPipeline(device,queryPipeline,nullptr);
            vkDestroyShaderModule(device,shader,nullptr); vkDestroyShaderModule(device,queryShader,nullptr);
            vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyPipelineLayout(device,queryLayout,nullptr);
            vkDestroyDescriptorPool(device,descriptors,nullptr); vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
            vkDestroyDescriptorSetLayout(device,querySetLayout,nullptr);
            vkDestroyCommandPool(device,commands,nullptr); vkDestroyDevice(device,nullptr);
        }
        if (messenger) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,
            "vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
};

struct Fixture {
    Device& d;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> allocations;
    VkBuffer buffer{};
    void* mapped{};
    VkDeviceMemory bufferMemory{};
    VkCommandBuffer command{};
    VkImageView pyramidFullView{};
    explicit Fixture(Device& device) : d(device) {
        images.reserve(2); views.reserve(33); allocations.reserve(3);
    }
    ~Fixture() {
        vkDeviceWaitIdle(d.device);
        if (command) vkFreeCommandBuffers(d.device,d.commands,1,&command);
        vkResetDescriptorPool(d.device,d.descriptors,0);
        if (mapped) vkUnmapMemory(d.device,bufferMemory);
        vkDestroyBuffer(d.device,buffer,nullptr);
        for (auto view : views) vkDestroyImageView(d.device,view,nullptr);
        for (auto image : images) vkDestroyImage(d.device,image,nullptr);
        for (auto memory : allocations) vkFreeMemory(d.device,memory,nullptr);
    }
    void image(uint32_t width, uint32_t height, VkFormat format, uint32_t mips) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width,height,1};
        info.mipLevels = mips; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (format == VK_FORMAT_R32_SFLOAT) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        VkImage image{}; check(vkCreateImage(d.device,&info,nullptr,&image)); images.push_back(image);
        VkMemoryRequirements needs{}; vkGetImageMemoryRequirements(d.device,image,&needs);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = needs.size; memory.memoryTypeIndex = d.memoryType(needs.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory allocation{}; check(vkAllocateMemory(d.device,&memory,nullptr,&allocation)); allocations.push_back(allocation);
        check(vkBindImageMemory(d.device,image,allocation,0));
        for (uint32_t mip = 0; mip < mips; ++mip) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
            view.subresourceRange = {static_cast<VkImageAspectFlags>(format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),mip,1,0,1};
            VkImageView handle{}; check(vkCreateImageView(d.device,&view,nullptr,&handle)); views.push_back(handle);
        }
        // The second image is always the complete R32 pyramid. Keep the first
        // source image's per-mip indexing unchanged when it is also R32.
        if (format == VK_FORMAT_R32_SFLOAT && images.size() == 2u) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1};
            check(vkCreateImageView(d.device,&view,nullptr,&pyramidFullView));
            views.push_back(pyramidFullView);
        }
    }
    void barrier(VkImage image, VkImageAspectFlags aspect, uint32_t mip, uint32_t count,
        VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags source, VkAccessFlags destination,
        VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = source; b.dstAccessMask = destination; b.oldLayout = oldLayout; b.newLayout = newLayout;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {aspect,mip,count,0,1};
        vkCmdPipelineBarrier(command,sourceStage,destinationStage,0,0,nullptr,0,nullptr,1,&b);
    }
    void run(Iridium::DepthPyramidExtent extent, bool reverse, bool deviceDepth) {
        using namespace Iridium;
        const auto convention = reverse ? DeviceDepthConvention::ReverseZeroToOne : DeviceDepthConvention::ForwardZeroToOne;
        std::vector<float> source(size_t(extent.width)*extent.height);
        for (size_t i=0;i<source.size();++i) source[i] = float((i*37)%997)/997;
        source.back() = reverse ? 0.0f : 1.0f;
        if (extent.width == 15 && extent.height == 1) {
            std::fill(source.begin(), source.end(), reverse ? .8f : .2f);
            source[4] = reverse ? 0.0f : 1.0f; // Original odd-partition regression.
        }
        if (!deviceDepth && source.size() >= 5) {
            source[0] = std::numeric_limits<float>::quiet_NaN(); source[1] = -1;
            source[2] = 2; source[3] = std::numeric_limits<float>::infinity();
        }
        DepthPyramidReference reference; reference.build(extent,convention,source);
        const uint32_t mips = reference.mipCount();
        std::vector<DepthPyramidDeviceQuery> queries;
        const auto addQuery = [&](uint32_t x0, uint32_t y0, uint32_t x1,
            uint32_t y1, float nearest, float bias) {
            queries.push_back(packDepthPyramidDeviceQuery(
                {x0, y0, x1, y1, nearest, bias}));
        };
        addQuery(0, 0, extent.width, extent.height, 0.5f, 0.0f);
        addQuery(0, 0, 1, 1, reverse ? 0.0f : 1.0f, 0.00001f);
        addQuery(extent.width - 1u, extent.height - 1u, extent.width,
            extent.height, reverse ? 1.0f : 0.0f, 0.01f);
        for (uint32_t seed = 0; seed < 127u; ++seed) {
            const uint32_t x0 = (seed * 37u) % extent.width;
            const uint32_t y0 = (seed * 53u) % extent.height;
            const uint32_t width = 1u + (seed * 11u) % (extent.width - x0);
            const uint32_t height = 1u + (seed * 17u) % (extent.height - y0);
            addQuery(x0, y0, x0 + width, y0 + height,
                static_cast<float>((seed * 29u) % 101u) / 100.0f,
                static_cast<float>(seed % 5u) * 0.00001f);
        }
        std::vector<VkDeviceSize> offsets(mips);
        VkDeviceSize bytes = source.size()*sizeof(float);
        for (uint32_t mip=0;mip<mips;++mip) { offsets[mip]=bytes; bytes += reference.mip(mip).size_bytes(); }
        const auto align = [](VkDeviceSize value, VkDeviceSize alignment) {
            return (value + alignment - 1u) & ~(alignment - 1u);
        };
        const VkDeviceSize queryOffset = align(bytes, 256u);
        const VkDeviceSize queryBytes = queries.size() * sizeof(DepthPyramidDeviceQuery);
        const VkDeviceSize resultOffset = align(queryOffset + queryBytes, 256u);
        const VkDeviceSize resultBytes = queries.size() * sizeof(DepthPyramidDeviceResult);
        bytes = resultOffset + resultBytes;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size=bytes; bufferInfo.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(d.device,&bufferInfo,nullptr,&buffer));
        VkMemoryRequirements needs{}; vkGetBufferMemoryRequirements(d.device,buffer,&needs);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=needs.size;
        allocation.memoryTypeIndex=d.memoryType(needs.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(d.device,&allocation,nullptr,&bufferMemory)); allocations.push_back(bufferMemory);
        check(vkBindBufferMemory(d.device,buffer,bufferMemory,0)); check(vkMapMemory(d.device,bufferMemory,0,bytes,0,&mapped));
        std::memcpy(mapped,source.data(),source.size()*sizeof(float));
        std::memcpy(static_cast<char*>(mapped) + queryOffset, queries.data(),
            static_cast<size_t>(queryBytes));
        image(extent.width,extent.height,deviceDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT,1);
        image(extent.width,extent.height,VK_FORMAT_R32_SFLOAT,mips);
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool=d.commands; commandInfo.commandBufferCount=1;
        check(vkAllocateCommandBuffers(d.device,&commandInfo,&command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command,&begin));
        const VkImageAspectFlags aspect=deviceDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier(images[0],aspect,0,1,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy upload{}; upload.imageSubresource={aspect,0,0,1}; upload.imageExtent={extent.width,extent.height,1};
        vkCmdCopyBufferToImage(command,buffer,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload);
        barrier(images[0],aspect,0,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(images[1],VK_IMAGE_ASPECT_COLOR_BIT,0,mips,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        for (uint32_t mip=0;mip<mips;++mip) {
            VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            sets.descriptorPool=d.descriptors; sets.descriptorSetCount=1; sets.pSetLayouts=&d.setLayout;
            VkDescriptorSet set{}; check(vkAllocateDescriptorSets(d.device,&sets,&set));
            std::array<VkDescriptorImageInfo,2> descriptors{{{d.sampler,views[mip],mip ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE,views[mip+1],VK_IMAGE_LAYOUT_GENERAL}}};
            std::array<VkWriteDescriptorSet,2> writes{};
            for (uint32_t b=0;b<2;++b) {
                writes[b]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[b].dstSet=set; writes[b].dstBinding=b; writes[b].descriptorCount=1;
                writes[b].descriptorType=b ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].pImageInfo=&descriptors[b];
            }
            vkUpdateDescriptorSets(d.device,2,writes.data(),0,nullptr);
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,d.pipeline);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,d.layout,0,1,&set,0,nullptr);
            uint32_t reversed=reverse; vkCmdPushConstants(command,d.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&reversed);
            auto size=depthPyramidMipExtent(extent,mip);
            vkCmdDispatch(command,(size.width+7)/8,(size.height+7)/8,1);
            barrier(images[1],VK_IMAGE_ASPECT_COLOR_BIT,mip,1,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        VkDescriptorSetAllocateInfo querySetInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        querySetInfo.descriptorPool=d.descriptors; querySetInfo.descriptorSetCount=1;
        querySetInfo.pSetLayouts=&d.querySetLayout;
        VkDescriptorSet querySet{}; check(vkAllocateDescriptorSets(d.device,&querySetInfo,&querySet));
        const VkDescriptorImageInfo queryImage{d.sampler,pyramidFullView,VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkDescriptorBufferInfo,2> queryBuffers{{
            {buffer,queryOffset,queryBytes},{buffer,resultOffset,resultBytes}}};
        std::array<VkWriteDescriptorSet,3> queryWrites{};
        queryWrites[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; queryWrites[0].dstSet=querySet;
        queryWrites[0].dstBinding=0; queryWrites[0].descriptorCount=1;
        queryWrites[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        queryWrites[0].pImageInfo=&queryImage;
        for (uint32_t binding=1;binding<3;++binding) {
            queryWrites[binding]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            queryWrites[binding].dstSet=querySet; queryWrites[binding].dstBinding=binding;
            queryWrites[binding].descriptorCount=1;
            queryWrites[binding].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            queryWrites[binding].pBufferInfo=&queryBuffers[binding-1u];
        }
        vkUpdateDescriptorSets(d.device,static_cast<uint32_t>(queryWrites.size()),
            queryWrites.data(),0,nullptr);
        VkMemoryBarrier queryHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        queryHost.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        queryHost.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&queryHost,0,nullptr,0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,d.queryPipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,d.queryLayout,
            0,1,&querySet,0,nullptr);
        const std::array<uint32_t,3> queryParameters{
            static_cast<uint32_t>(queries.size()),reverse ? 1u : 0u,
            DepthPyramidAbiVersion};
        vkCmdPushConstants(command,d.queryLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,
            sizeof(queryParameters),queryParameters.data());
        vkCmdDispatch(command,(queryParameters[0]+63u)/64u,1,1);
        barrier(images[1],VK_IMAGE_ASPECT_COLOR_BIT,0,mips,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (uint32_t mip=0;mip<mips;++mip) {
            auto size=depthPyramidMipExtent(extent,mip);
            VkBufferImageCopy copy{}; copy.bufferOffset=offsets[mip]; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,1}; copy.imageExtent={size.width,size.height,1};
            vkCmdCopyImageToBuffer(command,images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        }
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,0,nullptr);
        check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        check(vkQueueSubmit(d.queue,1,&submit,VK_NULL_HANDLE)); check(vkQueueWaitIdle(d.queue));
        for (uint32_t mip=0;mip<mips;++mip) {
            const auto* actual=reinterpret_cast<const float*>(static_cast<const char*>(mapped)+offsets[mip]);
            for (size_t i=0;i<reference.mip(mip).size();++i)
                if (actual[i] != reference.mip(mip)[i]) throw std::runtime_error("Depth mismatch at mip " + std::to_string(mip) + " texel " + std::to_string(i));
        }
        const auto* deviceResults=reinterpret_cast<const DepthPyramidDeviceResult*>(
            static_cast<const char*>(mapped)+resultOffset);
        for (size_t index=0;index<queries.size();++index) {
            const auto& packed=queries[index];
            const auto expected=reference.test({packed.minimumX,packed.minimumY,
                packed.maximumX,packed.maximumY,packed.nearestDepth,packed.depthBias});
            const auto& actual=deviceResults[index];
            if (actual.abiVersion != DepthPyramidAbiVersion ||
                actual.mipLevel != expected.mipLevel ||
                actual.sampledTexels != expected.sampledTexels ||
                actual.tested != static_cast<uint32_t>(expected.tested) ||
                actual.farthestOccluderDepth != expected.farthestOccluderDepth ||
                actual.occluded != static_cast<uint32_t>(expected.occluded))
                throw std::runtime_error("Depth query mismatch at query " +
                    std::to_string(index));
        }
        std::cout << extent.width << 'x' << extent.height << " reverse=" << reverse <<
            " D32=" << deviceDepth << " all " << mips << " mips and " <<
            queries.size() << " queries exact\n";
    }
};
}
int main() {
    try {
        { Device device; device.init();
          for (const auto extent : {Iridium::DepthPyramidExtent{1,1},{15,1},{15,3},{7,5},{1,15},{127,73},{3840,2160}})
          for (bool reverse : {false,true}) for (bool d32 : {false,true}) {
              Fixture fixture(device); fixture.run(extent,reverse,d32);
          }
        }
        if (validationErrors) throw std::runtime_error("Vulkan validation errors: " + std::to_string(validationErrors.load()));
        std::cout << "Vulkan depth pyramid parity passed (validation enabled)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
