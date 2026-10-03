#include "VulkanResourceAllocator.h"

#include "VulkanVma.h"

#include <algorithm>
#include <array>
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

        constexpr uint64_t imageRequestedBytes(VkExtent2D extent, VkFormat format,
            uint32_t mipLevels, uint32_t arrayLayers) noexcept {
            uint64_t bytesPerTexel = 0;
            uint64_t bytesPerBlock = 0;
            switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_R32_SFLOAT:
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
        static_assert(imageRequestedBytes({1, 1}, VK_FORMAT_R32_SFLOAT, 1, 1) == 4);
        static_assert(imageRequestedBytes({15, 3}, VK_FORMAT_R32_SFLOAT, 4, 1) == 224);
    } // namespace

    std::optional<uint32_t> legacyMemoryTypeIndex(
        const VkPhysicalDeviceMemoryProperties& properties, uint32_t typeBits,
        VkMemoryPropertyFlags requiredFlags) noexcept {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) != 0 &&
                (properties.memoryTypes[i].propertyFlags & requiredFlags) == requiredFlags) {
                return i;
            }
        }
        return std::nullopt;
    }

    void VulkanResourceAllocator::init(VkInstance instance, VkPhysicalDevice physicalDevice,
        VkDevice device, bool memoryBudgetAvailable) {
        if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
            device == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanResourceAllocator requires valid Vulkan devices.");
        }
        if (physicalDevice_ != VK_NULL_HANDLE || device_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanResourceAllocator was initialized more than once.");
        }
        VkPhysicalDeviceProperties deviceProperties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
        // The instance requests 1.3; the device's version caps what VMA may use.
        const uint32_t apiVersion = std::min<uint32_t>(VK_API_VERSION_1_3,
            VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(deviceProperties.apiVersion),
                VK_API_VERSION_MINOR(deviceProperties.apiVersion), 0));

        VmaAllocatorCreateInfo createInfo{};
        createInfo.flags = memoryBudgetAvailable
            ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
        createInfo.physicalDevice = physicalDevice;
        createInfo.device = device;
        createInfo.instance = instance;
        createInfo.vulkanApiVersion = apiVersion;
        VmaAllocator vma = VK_NULL_HANDLE;
        const VkResult result = vmaCreateAllocator(&createInfo, &vma);
        if (result != VK_SUCCESS) {
            throwVkError("vmaCreateAllocator", result);
        }

        physicalDevice_ = physicalDevice;
        device_ = device;
        vma_ = vma;
        frameIndex_ = 0;
        memoryProfile_ = MemoryProfileAccumulator{};
        memoryBudgetAvailable_ = memoryBudgetAvailable;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
    }

    void VulkanResourceAllocator::cleanup() {
        // Resources are intentionally not tracked. Callers must explicitly destroy every
        // resource before cleanup (VMA asserts this in Debug builds).
        if (vma_ != VK_NULL_HANDLE) {
            vmaDestroyAllocator(vma_);
        }
        vma_ = VK_NULL_HANDLE;
        frameIndex_ = 0;
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        memoryProperties_ = {};
        memoryBudgetAvailable_ = false;
    }

    void VulkanResourceAllocator::beginFrame() noexcept {
        if (vma_ != VK_NULL_HANDLE) {
            vmaSetCurrentFrameIndex(vma_, ++frameIndex_);
        }
    }

    uint32_t VulkanResourceAllocator::findMemoryType(
        uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        requireInitialized(physicalDevice_, device_);
        const std::optional<uint32_t> index =
            legacyMemoryTypeIndex(memoryProperties_, typeFilter, properties);
        if (!index) {
            throw std::runtime_error("Failed to find a suitable Vulkan memory type.");
        }
        return *index;
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
        uint32_t memoryTypeIndex = 0;
        try {
            memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, memoryProperties);
        } catch (...) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            throw;
        }

        // Exactly the legacy type. A persistent mapping starts at the buffer's
        // first byte, as vkMapMemory(memory, 0, size) did.
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.flags = persistentlyMapped ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0;
        allocationInfo.usage = VMA_MEMORY_USAGE_UNKNOWN;
        allocationInfo.requiredFlags = memoryProperties;
        allocationInfo.memoryTypeBits = 1u << memoryTypeIndex;
        VmaAllocationInfo info{};
        result = vmaAllocateMemoryForBuffer(vma_, resource.buffer, &allocationInfo,
            &resource.vmaAllocation, &info);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            throwVkError("vmaAllocateMemoryForBuffer", result);
        }
        result = vmaBindBufferMemory(vma_, resource.vmaAllocation, resource.buffer);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            vmaFreeMemory(vma_, resource.vmaAllocation);
            throwVkError("vmaBindBufferMemory", result);
        }
        if (persistentlyMapped && info.pMappedData == nullptr) {
            vkDestroyBuffer(device_, resource.buffer, nullptr);
            vmaFreeMemory(vma_, resource.vmaAllocation);
            throwVkError("vmaAllocateMemoryForBuffer (persistent mapping)",
                VK_ERROR_MEMORY_MAP_FAILED);
        }
        resource.memory = info.deviceMemory;
        resource.memoryOffset = info.offset;
        resource.mapped = persistentlyMapped ? info.pMappedData : nullptr;
        resource.size = size;
        vmaSetAllocationName(vma_, resource.vmaAllocation,
            profileMemoryCategoryName(category));
        const uint32_t memoryHeapIndex =
            memoryProperties_.memoryTypes[info.memoryType].heapIndex;
        memoryProfile_.recordAllocation(resource.allocation, category,
            static_cast<uint64_t>(size), static_cast<uint64_t>(info.size),
            info.memoryType, memoryHeapIndex);
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
        uint32_t memoryTypeIndex = 0;
        try {
            memoryTypeIndex = findMemoryType(
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        } catch (...) {
            vkDestroyImage(device_, resource.image, nullptr);
            throw;
        }

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.flags = usesDedicatedImageAllocation(requirements.size)
            ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        allocationInfo.usage = VMA_MEMORY_USAGE_UNKNOWN;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        allocationInfo.memoryTypeBits = 1u << memoryTypeIndex;
        VmaAllocationInfo info{};
        result = vmaAllocateMemoryForImage(vma_, resource.image, &allocationInfo,
            &resource.vmaAllocation, &info);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            throwVkError("vmaAllocateMemoryForImage", result);
        }
        result = vmaBindImageMemory(vma_, resource.vmaAllocation, resource.image);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            vmaFreeMemory(vma_, resource.vmaAllocation);
            throwVkError("vmaBindImageMemory", result);
        }

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = resource.image;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.subresourceRange = { aspect, 0, mipLevels, 0, arrayLayers };
        result = vkCreateImageView(device_, &viewInfo, nullptr, &resource.view);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            vmaFreeMemory(vma_, resource.vmaAllocation);
            throwVkError("vkCreateImageView", result);
        }

        resource.memory = info.deviceMemory;
        resource.memoryOffset = info.offset;
        resource.extent = extent;
        resource.format = format;
        resource.aspect = aspect;
        resource.mipLevels = mipLevels;
        resource.arrayLayers = arrayLayers;
        resource.viewType = viewType;
        vmaSetAllocationName(vma_, resource.vmaAllocation,
            profileMemoryCategoryName(category));
        const uint32_t memoryHeapIndex =
            memoryProperties_.memoryTypes[info.memoryType].heapIndex;
        memoryProfile_.recordAllocation(resource.allocation, category,
            imageRequestedBytes(extent, format, mipLevels, arrayLayers),
            static_cast<uint64_t>(info.size), info.memoryType, memoryHeapIndex);
        return resource;
    }

    // ---- M7R R4b.4 transient aliasing --------------------------------------

    namespace {
        VkImageCreateInfo imageCreateInfo(VkExtent2D extent, VkFormat format,
            VkImageUsageFlags usage, uint32_t mipLevels, uint32_t arrayLayers,
            VkImageCreateFlags flags) {
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
            return imageInfo;
        }
    } // namespace

    VkMemoryRequirements VulkanResourceAllocator::imageMemoryRequirements(
        VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
        uint32_t mipLevels, uint32_t arrayLayers, VkImageCreateFlags flags) const {
        requireInitialized(physicalDevice_, device_);
        if (mipLevels == 0 || arrayLayers == 0)
            throw std::invalid_argument("image mip and layer counts must be nonzero");
        const VkImageCreateInfo imageInfo = imageCreateInfo(extent, format, usage,
            mipLevels, arrayLayers, flags);
        VkMemoryRequirements requirements{};
        if (deviceApiVersion_ >= VK_API_VERSION_1_3) {
            // Core 1.3: the requirements of an image that does not exist yet.
            VkDeviceImageMemoryRequirements query{
                VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS };
            query.pCreateInfo = &imageInfo;
            VkMemoryRequirements2 result{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
            vkGetDeviceImageMemoryRequirements(device_, &query, &result);
            requirements = result.memoryRequirements;
        }
        else {
            VkImage image = VK_NULL_HANDLE;
            const VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &image);
            if (result != VK_SUCCESS) throwVkError("vkCreateImage", result);
            vkGetImageMemoryRequirements(device_, image, &requirements);
            vkDestroyImage(device_, image, nullptr);
        }
        // Exactly the type createImage2D binds to (no silent type change).
        requirements.memoryTypeBits = 1u << findMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        return requirements;
    }

    VulkanAliasHeapResource VulkanResourceAllocator::createAliasHeap(VkDeviceSize size,
        VkDeviceSize alignment, uint32_t typeBits, ProfileMemoryCategory category,
        uint64_t requestedBytes) {
        requireInitialized(physicalDevice_, device_);
        if (size == 0 || typeBits == 0 || alignment == 0 ||
            (alignment & (alignment - 1)) != 0)
            throw std::invalid_argument(
                "Alias heap needs a size, memory types and a power-of-two alignment");
        const uint32_t memoryTypeIndex = findMemoryType(typeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // Whole 64 KiB pages, so a buffer over the entire heap (poison fills)
        // fits whatever rounding the buffer's own requirements apply.
        constexpr VkDeviceSize Page = VkDeviceSize{ 64 } * 1024;
        const VkDeviceSize granule = (std::max)(alignment, Page);
        size = (size + granule - 1) / granule * granule;
        VkMemoryRequirements requirements{};
        requirements.size = size;
        requirements.alignment = alignment;
        requirements.memoryTypeBits = 1u << memoryTypeIndex;
        // One dedicated block per heap: offsets are the planner's, not VMA's.
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        allocationInfo.usage = VMA_MEMORY_USAGE_UNKNOWN;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        allocationInfo.memoryTypeBits = requirements.memoryTypeBits;
        VulkanAliasHeapResource heap{};
        VmaAllocationInfo info{};
        const VkResult result = vmaAllocateMemory(vma_, &requirements, &allocationInfo,
            &heap.vmaAllocation, &info);
        if (result != VK_SUCCESS) throwVkError("vmaAllocateMemory (alias heap)", result);
        heap.memory = info.deviceMemory;
        heap.offset = info.offset;
        heap.size = size;
        heap.memoryTypeIndex = info.memoryType;
        vmaSetAllocationName(vma_, heap.vmaAllocation, profileMemoryCategoryName(category));
        memoryProfile_.recordAllocation(heap.allocation, category, requestedBytes,
            static_cast<uint64_t>(info.size), info.memoryType,
            memoryProperties_.memoryTypes[info.memoryType].heapIndex);
        return heap;
    }

    VulkanImageResource VulkanResourceAllocator::createAliasingImage2D(
        const VulkanAliasHeapResource& heap, VkDeviceSize offset, VkExtent2D extent,
        VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
        uint32_t mipLevels, uint32_t arrayLayers, VkImageCreateFlags flags,
        VkImageViewType viewType) {
        requireInitialized(physicalDevice_, device_);
        if (!heap.isValid()) throw std::invalid_argument("Aliasing image needs a valid heap");
        const VkMemoryRequirements requirements = imageMemoryRequirements(extent, format,
            usage, mipLevels, arrayLayers, flags);
        if ((requirements.memoryTypeBits & (1u << heap.memoryTypeIndex)) == 0 ||
            offset % requirements.alignment != 0 || offset > heap.size ||
            requirements.size > heap.size - offset)
            throw std::invalid_argument(
                "Aliasing image does not fit its heap placement or memory type");

        const VkImageCreateInfo imageInfo = imageCreateInfo(extent, format, usage,
            mipLevels, arrayLayers, flags);
        VulkanImageResource resource{};
        VkResult result = vmaCreateAliasingImage2(vma_, heap.vmaAllocation, offset,
            &imageInfo, &resource.image);
        if (result != VK_SUCCESS) throwVkError("vmaCreateAliasingImage2", result);

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = resource.image;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.subresourceRange = { aspect, 0, mipLevels, 0, arrayLayers };
        result = vkCreateImageView(device_, &viewInfo, nullptr, &resource.view);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device_, resource.image, nullptr);
            throwVkError("vkCreateImageView", result);
        }
        resource.memory = heap.memory;
        resource.memoryOffset = heap.offset + offset;
        resource.extent = extent;
        resource.format = format;
        resource.aspect = aspect;
        resource.mipLevels = mipLevels;
        resource.arrayLayers = arrayLayers;
        resource.viewType = viewType;
        // Statistics only (valid = false: never counted, recordFree is a
        // no-op); the heap's profile record owns the memory.
        resource.allocation.category = ProfileMemoryCategory::RenderGraphTransient;
        resource.allocation.requestedBytes =
            imageRequestedBytes(extent, format, mipLevels, arrayLayers);
        resource.allocation.memoryTypeIndex = heap.memoryTypeIndex;
        resource.allocation.memoryHeapIndex =
            memoryProperties_.memoryTypes[heap.memoryTypeIndex].heapIndex;
        return resource;
    }

    VulkanBufferResource VulkanResourceAllocator::createAliasingBuffer(
        const VulkanAliasHeapResource& heap, VkBufferUsageFlags usage) {
        requireInitialized(physicalDevice_, device_);
        if (!heap.isValid()) throw std::invalid_argument("Aliasing buffer needs a valid heap");
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = heap.size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VulkanBufferResource resource{};
        const VkResult result = vmaCreateAliasingBuffer2(vma_, heap.vmaAllocation, 0,
            &bufferInfo, &resource.buffer);
        if (result != VK_SUCCESS) throwVkError("vmaCreateAliasingBuffer2", result);
        resource.memory = heap.memory;
        resource.memoryOffset = heap.offset;
        resource.size = heap.size;
        return resource;
    }

    void VulkanResourceAllocator::destroy(VulkanAliasHeapResource& heap) noexcept {
        if (device_ != VK_NULL_HANDLE && heap.vmaAllocation != VK_NULL_HANDLE)
            vmaFreeMemory(vma_, heap.vmaAllocation);
        memoryProfile_.recordFree(heap.allocation);
        heap = {};
    }

    void VulkanResourceAllocator::destroy(VulkanBufferResource& resource) noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (resource.buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, resource.buffer, nullptr);
            }
            // Frees (and, for a persistent mapping, unmaps) the VMA allocation.
            if (resource.vmaAllocation != VK_NULL_HANDLE) {
                vmaFreeMemory(vma_, resource.vmaAllocation);
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
            if (resource.vmaAllocation != VK_NULL_HANDLE) {
                vmaFreeMemory(vma_, resource.vmaAllocation);
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
        if (vma_ != VK_NULL_HANDLE && resource.vmaAllocation != VK_NULL_HANDLE) {
            vmaSetAllocationName(vma_, resource.vmaAllocation,
                profileMemoryCategoryName(category));
        }
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

        if (memoryBudgetAvailable_ && vma_ != VK_NULL_HANDLE) {
            // VMA's view of VK_EXT_memory_budget: the driver's heap budget and
            // usage as of its last refresh (beginFrame), plus the blocks VMA has
            // allocated or freed since.
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(vma_, budgets.data());
            result.driverHeapBudgetAvailable = true;
            for (uint32_t index = 0; index < result.heapCount; ++index) {
                result.heaps[index].driverBudgetBytes =
                    static_cast<uint64_t>(budgets[index].budget);
                result.heaps[index].driverUsageBytes =
                    static_cast<uint64_t>(budgets[index].usage);
                result.heaps[index].driverBudgetAvailable = true;
            }
        }
        return result;
    }

} // namespace Iridium
