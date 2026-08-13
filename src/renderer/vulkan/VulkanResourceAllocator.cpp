#include "VulkanResourceAllocator.h"

#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {
        void requireInitialized(VkPhysicalDevice physicalDevice, VkDevice device) {
            if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
                throw std::logic_error("VulkanResourceAllocator is not initialized.");
            }
        }

        [[noreturn]] void throwVkError(const char* operation, VkResult result) {
            throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                std::to_string(static_cast<int>(result)) + ".");
        }

        uint64_t imageRequestedBytes(VkExtent2D extent, VkFormat format,
            uint32_t mipLevels, uint32_t arrayLayers) noexcept {
            uint64_t bytesPerTexel = 0;
            uint64_t bytesPerBlock = 0;
            switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_D32_SFLOAT:
                bytesPerTexel = 4;
                break;
            case VK_FORMAT_R16G16_SFLOAT:
                bytesPerTexel = 4;
                break;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                bytesPerTexel = 8;
                break;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                bytesPerTexel = 16;
                break;
            case VK_FORMAT_BC4_UNORM_BLOCK:
                bytesPerBlock = 8;
                break;
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
                bytesPerBlock = 16;
                break;
            default:
                break;
            }
            uint64_t result = 0;
            for (uint32_t level = 0; level < mipLevels; ++level) {
                result += bytesPerBlock != 0
                    ? ((static_cast<uint64_t>(extent.width) + 3) / 4) *
                        ((static_cast<uint64_t>(extent.height) + 3) / 4) * bytesPerBlock
                    : static_cast<uint64_t>(extent.width) * extent.height * bytesPerTexel;
                extent.width = extent.width > 1 ? extent.width / 2 : 1;
                extent.height = extent.height > 1 ? extent.height / 2 : 1;
            }
            return result * arrayLayers;
        }
    } // namespace

    void VulkanResourceAllocator::init(VkPhysicalDevice physicalDevice, VkDevice device,
        bool memoryBudgetAvailable) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanResourceAllocator requires valid Vulkan devices.");
        }
        if (physicalDevice_ != VK_NULL_HANDLE || device_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanResourceAllocator was initialized more than once.");
        }
        physicalDevice_ = physicalDevice;
        device_ = device;
        memoryProfile_ = MemoryProfileAccumulator{};
        memoryBudgetAvailable_ = memoryBudgetAvailable;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
    }

    void VulkanResourceAllocator::cleanup() {
        // Resources are intentionally not tracked. Callers must explicitly destroy every
        // resource before cleanup; this method only releases allocator device references.
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        memoryProperties_ = {};
        memoryBudgetAvailable_ = false;
    }

    uint32_t VulkanResourceAllocator::findMemoryType(
        uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        requireInitialized(physicalDevice_, device_);

        for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) != 0 &&
                (memoryProperties_.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("Failed to find a suitable Vulkan memory type.");
    }

    VulkanBufferResource VulkanResourceAllocator::createBuffer(
        VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties, bool persistentlyMapped,
        ProfileMemoryCategory category) {
        requireInitialized(physicalDevice_, device_);

        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VulkanBufferResource resource{};
        VkResult result = vkCreateBuffer(device_, &bufferInfo, nullptr, &resource.buffer);
        if (result != VK_SUCCESS) {
            throwVkError("vkCreateBuffer", result);
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        try {
            allocateInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, memoryProperties);
        } catch (...) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            throw;
        }

        result = vkAllocateMemory(device_, &allocateInfo, nullptr, &resource.memory);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            throwVkError("vkAllocateMemory", result);
        }
        result = vkBindBufferMemory(device_, resource.buffer, resource.memory, 0);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            vkFreeMemory(device_, resource.memory, nullptr);
            throwVkError("vkBindBufferMemory", result);
        }
        if (persistentlyMapped) {
            result = vkMapMemory(device_, resource.memory, 0, size, 0, &resource.mapped);
            if (result != VK_SUCCESS) {
                vkDestroyBuffer(device_, resource.buffer, nullptr);
                vkFreeMemory(device_, resource.memory, nullptr);
                throwVkError("vkMapMemory", result);
            }
        }
        resource.size = size;
        const uint32_t memoryTypeIndex = allocateInfo.memoryTypeIndex;
        const uint32_t memoryHeapIndex =
            memoryProperties_.memoryTypes[memoryTypeIndex].heapIndex;
        memoryProfile_.recordAllocation(resource.allocation, category,
            static_cast<uint64_t>(size), static_cast<uint64_t>(requirements.size),
            memoryTypeIndex, memoryHeapIndex);
        return resource;
    }

    VulkanImageResource VulkanResourceAllocator::createImage2D(
        VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
        VkImageAspectFlags aspect, ProfileMemoryCategory category,
        uint32_t mipLevels, uint32_t arrayLayers, VkImageCreateFlags flags,
        VkImageViewType viewType) {
        requireInitialized(physicalDevice_, device_);
        if (mipLevels == 0) throw std::invalid_argument("image mip count must be nonzero");
        if (arrayLayers == 0) throw std::invalid_argument("image layer count must be nonzero");
        if (viewType == VK_IMAGE_VIEW_TYPE_CUBE &&
            (arrayLayers != 6 || extent.width != extent.height ||
                (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0)) {
            throw std::invalid_argument(
                "cube images require six square cube-compatible layers");
        }

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.flags = flags;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = arrayLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VulkanImageResource resource{};
        VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &resource.image);
        if (result != VK_SUCCESS) {
            throwVkError("vkCreateImage", result);
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, resource.image, &requirements);
        VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocateInfo.allocationSize = requirements.size;
        try {
            allocateInfo.memoryTypeIndex = findMemoryType(
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        } catch (...) {
            vkDestroyImage(device_, resource.image, nullptr);
            throw;
        }

        result = vkAllocateMemory(device_, &allocateInfo, nullptr, &resource.memory);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            throwVkError("vkAllocateMemory", result);
        }
        result = vkBindImageMemory(device_, resource.image, resource.memory, 0);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            vkFreeMemory(device_, resource.memory, nullptr);
            throwVkError("vkBindImageMemory", result);
        }

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = resource.image;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.subresourceRange = { aspect, 0, mipLevels, 0, arrayLayers };
        result = vkCreateImageView(device_, &viewInfo, nullptr, &resource.view);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            vkFreeMemory(device_, resource.memory, nullptr);
            throwVkError("vkCreateImageView", result);
        }

        resource.extent = extent;
        resource.format = format;
        resource.aspect = aspect;
        resource.mipLevels = mipLevels;
        resource.arrayLayers = arrayLayers;
        resource.viewType = viewType;
        const uint32_t memoryTypeIndex = allocateInfo.memoryTypeIndex;
        const uint32_t memoryHeapIndex =
            memoryProperties_.memoryTypes[memoryTypeIndex].heapIndex;
        memoryProfile_.recordAllocation(resource.allocation, category,
            imageRequestedBytes(extent, format, mipLevels, arrayLayers),
            static_cast<uint64_t>(requirements.size), memoryTypeIndex,
            memoryHeapIndex);
        return resource;
    }

    void VulkanResourceAllocator::destroy(VulkanBufferResource& resource) noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (resource.mapped != nullptr && resource.memory != VK_NULL_HANDLE) {
                vkUnmapMemory(device_, resource.memory);
            }
            if (resource.buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, resource.buffer, nullptr);
            }
            if (resource.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, resource.memory, nullptr);
            }
        }
        memoryProfile_.recordFree(resource.allocation);
        resource = {};
    }

    void VulkanResourceAllocator::destroy(VulkanImageResource& resource) noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (resource.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, resource.view, nullptr);
            }
            if (resource.image != VK_NULL_HANDLE) {
                vkDestroyImage(device_, resource.image, nullptr);
            }
            if (resource.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, resource.memory, nullptr);
            }
        }
        memoryProfile_.recordFree(resource.allocation);
        resource = {};
    }

    void VulkanResourceAllocator::write(
        VulkanBufferResource& resource, VkDeviceSize offset, std::span<const std::byte> data) {
        if (resource.mapped == nullptr) {
            throw std::logic_error("Vulkan buffer memory must be mapped before writing.");
        }
        if (offset > resource.size || data.size_bytes() > resource.size - offset) {
            throw std::out_of_range("Vulkan buffer write is outside the resource range.");
        }
        std::memcpy(static_cast<std::byte*>(resource.mapped) + offset,
            data.data(), data.size_bytes());
    }

    void VulkanResourceAllocator::reclassify(VulkanImageResource& resource,
        ProfileMemoryCategory category) noexcept {
        memoryProfile_.reclassify(resource.allocation, category);
    }

    FrameMemoryProfile VulkanResourceAllocator::memorySnapshot() const noexcept {
        FrameMemoryProfile result = memoryProfile_.snapshot();
        result.heapCount = std::min<uint32_t>(memoryProperties_.memoryHeapCount,
            static_cast<uint32_t>(result.heaps.size()));
        for (uint32_t index = 0; index < result.heapCount; ++index) {
            result.heaps[index].heapSizeBytes =
                static_cast<uint64_t>(memoryProperties_.memoryHeaps[index].size);
            result.heaps[index].flags = memoryProperties_.memoryHeaps[index].flags;
        }

        if (memoryBudgetAvailable_ && physicalDevice_ != VK_NULL_HANDLE) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
            properties.pNext = &budget;
            vkGetPhysicalDeviceMemoryProperties2(physicalDevice_, &properties);
            result.driverHeapBudgetAvailable = true;
            for (uint32_t index = 0; index < result.heapCount; ++index) {
                result.heaps[index].driverBudgetBytes =
                    static_cast<uint64_t>(budget.heapBudget[index]);
                result.heaps[index].driverUsageBytes =
                    static_cast<uint64_t>(budget.heapUsage[index]);
                result.heaps[index].driverBudgetAvailable = true;
            }
        }
        return result;
    }

} // namespace Iridium
