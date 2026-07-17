#include "VulkanResourceAllocator.h"

#include <cstring>
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
    } // namespace

    void VulkanResourceAllocator::init(VkPhysicalDevice physicalDevice, VkDevice device) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanResourceAllocator requires valid Vulkan devices.");
        }
        if (physicalDevice_ != VK_NULL_HANDLE || device_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanResourceAllocator was initialized more than once.");
        }
        physicalDevice_ = physicalDevice;
        device_ = device;
    }

    void VulkanResourceAllocator::cleanup() {
        // Resources are intentionally not tracked. Callers must explicitly destroy every
        // resource before cleanup; this method only releases allocator device references.
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }

    uint32_t VulkanResourceAllocator::findMemoryType(
        uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        requireInitialized(physicalDevice_, device_);

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("Failed to find a suitable Vulkan memory type.");
    }

    VulkanBufferResource VulkanResourceAllocator::createBuffer(
        VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties, bool persistentlyMapped) {
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
        return resource;
    }

    VulkanImageResource VulkanResourceAllocator::createImage2D(
        VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        requireInitialized(physicalDevice_, device_);

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
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
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { aspect, 0, 1, 0, 1 };
        result = vkCreateImageView(device_, &viewInfo, nullptr, &resource.view);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            vkFreeMemory(device_, resource.memory, nullptr);
            throwVkError("vkCreateImageView", result);
        }

        resource.extent = extent;
        resource.format = format;
        resource.aspect = aspect;
        return resource;
    }

    void VulkanResourceAllocator::destroy(VulkanBufferResource& resource) {
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
        resource = {};
    }

    void VulkanResourceAllocator::destroy(VulkanImageResource& resource) {
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

} // namespace Iridium
