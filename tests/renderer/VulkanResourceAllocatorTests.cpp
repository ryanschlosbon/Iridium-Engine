// M7R R4b: VulkanResourceAllocator over the Vulkan Memory Allocator.
// Device-free: the legacy memory-type rule and the dedicated-image threshold.
// Headless device under the validation layer: every resource lands in exactly
// the memory type the pre-VMA allocator chose; small resources are suballocated
// and large images get their own VkDeviceMemory; persistent mappings start at
// the buffer's first byte; the engine's per-category accounting records the
// requested bytes and the VMA allocation size; the memory-budget snapshot.

#include "HeadlessVulkanDevice.h"
#include "TestHarness.h"

#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

    using namespace Iridium;
    using Iridium::Test::HeadlessVulkanDevice;

    std::unique_ptr<HeadlessVulkanDevice> sharedDevice;

    constexpr VkDeviceSize MiB = 1024 * 1024;
    constexpr VkMemoryPropertyFlags HostCoherent =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VkPhysicalDeviceMemoryProperties syntheticProperties() {
        VkPhysicalDeviceMemoryProperties properties{};
        properties.memoryTypeCount = 4;
        properties.memoryTypes[0] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
        properties.memoryTypes[1] = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 1 };
        properties.memoryTypes[2] = { HostCoherent, 1 };
        properties.memoryTypes[3] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | HostCoherent, 0 };
        properties.memoryHeapCount = 2;
        return properties;
    }

    bool testLegacyTypeRule() {
        const VkPhysicalDeviceMemoryProperties properties = syntheticProperties();
        // Lowest index whose flags include every requested flag.
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0xF,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0u);
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0xF, HostCoherent) == 2u);
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0xF,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 1u);
        // typeBits exclude candidates; ReBAR is only chosen when nothing earlier fits.
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0xE,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 3u);
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0x1, HostCoherent) == std::nullopt);
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0x0, 0) == std::nullopt);
        // Types beyond memoryTypeCount are never considered.
        IRIDIUM_CHECK(legacyMemoryTypeIndex(properties, 0x30, 0) == std::nullopt);
        return true;
    }

    bool testDedicatedThreshold() {
        IRIDIUM_CHECK(DedicatedImageAllocationThresholdBytes == 16 * MiB);
        IRIDIUM_CHECK(!usesDedicatedImageAllocation(16 * MiB - 1));
        IRIDIUM_CHECK(usesDedicatedImageAllocation(16 * MiB));
        IRIDIUM_CHECK(usesDedicatedImageAllocation(64 * MiB));
        return true;
    }

    struct BufferCase {
        VkDeviceSize size;
        VkBufferUsageFlags usage;
        VkMemoryPropertyFlags properties;
        bool mapped;
        ProfileMemoryCategory category;
    };

    struct ImageCase {
        VkExtent2D extent;
        VkFormat format;
        VkImageUsageFlags usage;
        VkImageAspectFlags aspect;
        uint32_t mips;
        uint32_t layers;
        VkImageCreateFlags flags;
        VkImageViewType viewType;
        ProfileMemoryCategory category;
    };

    constexpr std::array<BufferCase, 6> BufferCases{ {
        { 256 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, ProfileMemoryCategory::GeometryVertex },
        { 64 * 1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, ProfileMemoryCategory::GeometryIndex },
        { 4 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HostCoherent, true,
            ProfileMemoryCategory::Uniform },
        { 1 * MiB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HostCoherent, true,
            ProfileMemoryCategory::UploadStaging },
        { 96 * 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HostCoherent, true,
            ProfileMemoryCategory::CaptureReadback },
        { 300 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, ProfileMemoryCategory::GpuScene },
    } };

    constexpr std::array<ImageCase, 5> ImageCases{ {
        { { 3840, 2160 }, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, VK_IMAGE_VIEW_TYPE_2D,
            ProfileMemoryCategory::RenderGraphTransient },
        { { 2048, 2048 }, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, 1, 4, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            ProfileMemoryCategory::ShadowDirectional },
        { { 512, 512 }, VK_FORMAT_BC7_UNORM_BLOCK,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 10, 1, 0, VK_IMAGE_VIEW_TYPE_2D,
            ProfileMemoryCategory::Texture },
        { { 256, 256 }, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 9, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            VK_IMAGE_VIEW_TYPE_CUBE, ProfileMemoryCategory::Environment },
        { { 128, 128 }, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, VK_IMAGE_VIEW_TYPE_2D,
            ProfileMemoryCategory::MaterialGpu },
    } };

    struct AllocatorScope {
        VulkanResourceAllocator allocator;
        explicit AllocatorScope(HeadlessVulkanDevice& gpu) {
            allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(),
                gpu.hasMemoryBudget());
        }
        ~AllocatorScope() { allocator.cleanup(); }
    };

    VulkanBufferResource create(VulkanResourceAllocator& allocator, const BufferCase& c) {
        return allocator.createBuffer(c.size, c.usage, c.properties, c.mapped, c.category);
    }

    VulkanImageResource create(VulkanResourceAllocator& allocator, const ImageCase& c) {
        return allocator.createImage2D(c.extent, c.format, c.usage, c.aspect, c.category,
            c.mips, c.layers, c.flags, c.viewType);
    }

    VkMemoryRequirements requirementsOf(VkDevice device, const VulkanBufferResource& r) {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, r.buffer, &requirements);
        return requirements;
    }

    VkMemoryRequirements requirementsOf(VkDevice device, const VulkanImageResource& r) {
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, r.image, &requirements);
        return requirements;
    }

    VkPhysicalDeviceMemoryProperties memoryProperties(const HeadlessVulkanDevice& gpu) {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(gpu.physicalDevice(), &properties);
        return properties;
    }

    bool testMemoryTypesMatchLegacyChoice() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        const VkPhysicalDeviceMemoryProperties properties = memoryProperties(gpu);
        AllocatorScope scope(gpu);
        for (const BufferCase& c : BufferCases) {
            VulkanBufferResource buffer = create(scope.allocator, c);
            IRIDIUM_CHECK(buffer.isValid() && buffer.vmaAllocation != VK_NULL_HANDLE);
            const VkMemoryRequirements requirements = requirementsOf(gpu.device(), buffer);
            const auto expected = legacyMemoryTypeIndex(properties,
                requirements.memoryTypeBits, c.properties);
            IRIDIUM_CHECK(expected.has_value());
            IRIDIUM_CHECK_MSG(buffer.allocation.memoryTypeIndex == *expected,
                "buffer type " << buffer.allocation.memoryTypeIndex << " expected " << *expected);
            IRIDIUM_CHECK(buffer.allocation.memoryHeapIndex ==
                properties.memoryTypes[*expected].heapIndex);
            IRIDIUM_CHECK(buffer.memoryOffset % requirements.alignment == 0);
            IRIDIUM_CHECK((buffer.mapped != nullptr) == c.mapped);
            scope.allocator.destroy(buffer);
            IRIDIUM_CHECK(!buffer.isValid() && buffer.vmaAllocation == VK_NULL_HANDLE);
        }
        for (const ImageCase& c : ImageCases) {
            VulkanImageResource image = create(scope.allocator, c);
            IRIDIUM_CHECK(image.isValid() && image.vmaAllocation != VK_NULL_HANDLE);
            const VkMemoryRequirements requirements = requirementsOf(gpu.device(), image);
            const auto expected = legacyMemoryTypeIndex(properties,
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            IRIDIUM_CHECK(expected.has_value());
            IRIDIUM_CHECK_MSG(image.allocation.memoryTypeIndex == *expected,
                "image type " << image.allocation.memoryTypeIndex << " expected " << *expected);
            IRIDIUM_CHECK(image.memoryOffset % requirements.alignment == 0);
            scope.allocator.destroy(image);
        }
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0, gpu.validationErrors() << " errors");
        return true;
    }

    bool testSuballocationAndDedicatedImages() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        AllocatorScope scope(gpu);
        VulkanResourceAllocator& allocator = scope.allocator;

        // Small device-local buffers share VMA blocks at distinct, non-overlapping ranges.
        std::vector<VulkanBufferResource> buffers;
        for (int i = 0; i < 4; ++i) {
            buffers.push_back(allocator.createBuffer(64 * 1024,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                false, ProfileMemoryCategory::LightGpu));
        }
        IRIDIUM_CHECK(buffers[0].memory == buffers[1].memory);
        for (size_t a = 0; a < buffers.size(); ++a) {
            for (size_t b = a + 1; b < buffers.size(); ++b) {
                if (buffers[a].memory != buffers[b].memory) continue;
                const VkDeviceSize aEnd = buffers[a].memoryOffset +
                    requirementsOf(gpu.device(), buffers[a]).size;
                const VkDeviceSize bEnd = buffers[b].memoryOffset +
                    requirementsOf(gpu.device(), buffers[b]).size;
                IRIDIUM_CHECK(aEnd <= buffers[b].memoryOffset || bEnd <= buffers[a].memoryOffset);
            }
        }

        // Small sampled images are suballocated too.
        const ImageCase small{ { 128, 128 }, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, VK_IMAGE_VIEW_TYPE_2D,
            ProfileMemoryCategory::Texture };
        VulkanImageResource smallA = create(allocator, small);
        VulkanImageResource smallB = create(allocator, small);
        IRIDIUM_CHECK(requirementsOf(gpu.device(), smallA).size < 16 * MiB);
        IRIDIUM_CHECK(smallA.memory == smallB.memory);
        IRIDIUM_CHECK(smallA.memoryOffset != smallB.memoryOffset);

        // Images at or above the threshold own their memory: offset 0 and a
        // VkDeviceMemory no other resource is bound to.
        const ImageCase large{ { 2048, 2048 }, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 0, VK_IMAGE_VIEW_TYPE_2D,
            ProfileMemoryCategory::RenderGraphHistory };
        VulkanImageResource largeA = create(allocator, large);
        VulkanImageResource largeB = create(allocator, large);
        IRIDIUM_CHECK(usesDedicatedImageAllocation(requirementsOf(gpu.device(), largeA).size));
        IRIDIUM_CHECK(largeA.memoryOffset == 0 && largeB.memoryOffset == 0);
        IRIDIUM_CHECK(largeA.memory != largeB.memory);
        for (const VulkanBufferResource& buffer : buffers) {
            IRIDIUM_CHECK(buffer.memory != largeA.memory && buffer.memory != largeB.memory);
        }
        IRIDIUM_CHECK(smallA.memory != largeA.memory && smallA.memory != largeB.memory);

        allocator.destroy(largeB);
        allocator.destroy(largeA);
        allocator.destroy(smallB);
        allocator.destroy(smallA);
        for (VulkanBufferResource& buffer : buffers) allocator.destroy(buffer);
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0, gpu.validationErrors() << " errors");
        return true;
    }

    bool testPersistentMappings() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        AllocatorScope scope(gpu);
        VulkanResourceAllocator& allocator = scope.allocator;
        VulkanBufferResource a = allocator.createBuffer(4096,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HostCoherent, true,
            ProfileMemoryCategory::Uniform);
        VulkanBufferResource b = allocator.createBuffer(4096,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HostCoherent, true,
            ProfileMemoryCategory::Uniform);
        VulkanBufferResource unmapped = allocator.createBuffer(4096,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HostCoherent, false,
            ProfileMemoryCategory::Uniform);
        IRIDIUM_CHECK(a.mapped != nullptr && b.mapped != nullptr);
        IRIDIUM_CHECK(unmapped.mapped == nullptr);
        // Suballocated mappings point at each buffer's own first byte.
        if (a.memory == b.memory) {
            const auto delta = static_cast<std::byte*>(b.mapped) - static_cast<std::byte*>(a.mapped);
            IRIDIUM_CHECK(delta == static_cast<std::ptrdiff_t>(b.memoryOffset) -
                static_cast<std::ptrdiff_t>(a.memoryOffset));
        }
        std::array<std::byte, 64> payload{};
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i + 1);
        allocator.write(a, 128, payload);
        IRIDIUM_CHECK(std::memcmp(static_cast<std::byte*>(a.mapped) + 128, payload.data(),
            payload.size()) == 0);
        bool threw = false;
        try { allocator.write(unmapped, 0, payload); } catch (const std::logic_error&) { threw = true; }
        IRIDIUM_CHECK(threw);
        threw = false;
        try { allocator.write(a, 4096 - 32, payload); } catch (const std::out_of_range&) { threw = true; }
        IRIDIUM_CHECK(threw);
        allocator.destroy(unmapped);
        allocator.destroy(b);
        allocator.destroy(a);
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0, gpu.validationErrors() << " errors");
        return true;
    }

    bool testCategoryAccounting() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        AllocatorScope scope(gpu);
        VulkanResourceAllocator& allocator = scope.allocator;
        std::array<uint64_t, ProfileMemoryCategoryCount> requested{};
        std::array<uint64_t, ProfileMemoryCategoryCount> committed{};
        std::array<uint64_t, ProfileMemoryCategoryCount> counts{};
        std::vector<VulkanBufferResource> buffers;
        std::vector<VulkanImageResource> images;
        for (const BufferCase& c : BufferCases) {
            buffers.push_back(create(allocator, c));
            const auto index = static_cast<size_t>(c.category);
            requested[index] += c.size;
            committed[index] += requirementsOf(gpu.device(), buffers.back()).size;
            ++counts[index];
            IRIDIUM_CHECK(buffers.back().allocation.requestedBytes == c.size);
        }
        for (const ImageCase& c : ImageCases) {
            images.push_back(create(allocator, c));
            const auto index = static_cast<size_t>(c.category);
            requested[index] += images.back().allocation.requestedBytes;
            committed[index] += requirementsOf(gpu.device(), images.back()).size;
            ++counts[index];
        }
        // Requested image bytes are the texel/block sums, as before VMA.
        IRIDIUM_CHECK(images[0].allocation.requestedBytes == 3840ull * 2160 * 8);
        IRIDIUM_CHECK(images[1].allocation.requestedBytes == 2048ull * 2048 * 4 * 4);

        FrameMemoryProfile profile = allocator.memorySnapshot();
        uint64_t totalCommitted = 0;
        for (size_t index = 0; index < ProfileMemoryCategoryCount; ++index) {
            const ProfileMemoryCategorySnapshot& category = profile.categories[index];
            IRIDIUM_CHECK(category.name != nullptr);
            IRIDIUM_CHECK_MSG(category.requestedLiveBytes == requested[index], category.name);
            // Committed = the VMA allocation size, which is the resource's requirement.
            IRIDIUM_CHECK_MSG(category.committedLiveBytes == committed[index], category.name);
            IRIDIUM_CHECK_MSG(category.liveAllocationCount == counts[index], category.name);
            totalCommitted += committed[index];
        }
        IRIDIUM_CHECK(profile.engineCommittedLiveBytes == totalCommitted);
        uint64_t heapCommitted = 0;
        for (uint32_t heap = 0; heap < profile.heapCount; ++heap) {
            heapCommitted += profile.heaps[heap].engineCommittedLiveBytes;
            IRIDIUM_CHECK(profile.heaps[heap].heapSizeBytes != 0);
        }
        IRIDIUM_CHECK(heapCommitted == totalCommitted);

        // Reclassification moves the bytes; destruction releases them and keeps peaks.
        const auto texture = static_cast<size_t>(ProfileMemoryCategory::Texture);
        const auto other = static_cast<size_t>(ProfileMemoryCategory::OtherUnclassified);
        allocator.reclassify(images[2], ProfileMemoryCategory::OtherUnclassified);
        profile = allocator.memorySnapshot();
        IRIDIUM_CHECK(profile.categories[texture].committedLiveBytes == 0);
        IRIDIUM_CHECK(profile.categories[other].committedLiveBytes == committed[texture]);
        for (VulkanImageResource& image : images) allocator.destroy(image);
        for (VulkanBufferResource& buffer : buffers) allocator.destroy(buffer);
        profile = allocator.memorySnapshot();
        IRIDIUM_CHECK(profile.engineCommittedLiveBytes == 0);
        IRIDIUM_CHECK(profile.engineLiveAllocationCount == 0);
        IRIDIUM_CHECK(profile.engineCommittedPeakBytes == totalCommitted);
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0, gpu.validationErrors() << " errors");
        return true;
    }

    bool testBudgetSnapshot() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        AllocatorScope scope(gpu);
        VulkanResourceAllocator& allocator = scope.allocator;
        allocator.beginFrame();
        const FrameMemoryProfile before = allocator.memorySnapshot();
        IRIDIUM_CHECK(before.driverHeapBudgetAvailable == gpu.hasMemoryBudget());
        if (!gpu.hasMemoryBudget()) {
            std::cout << "  VK_EXT_memory_budget unavailable; budget fields stay empty\n";
            for (uint32_t heap = 0; heap < before.heapCount; ++heap)
                IRIDIUM_CHECK(!before.heaps[heap].driverBudgetAvailable);
            return true;
        }
        VulkanImageResource image = allocator.createImage2D({ 4096, 4096 },
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, ProfileMemoryCategory::RenderGraphHistory);
        const uint32_t heap = image.allocation.memoryHeapIndex;
        IRIDIUM_CHECK(heap < before.heapCount);
        IRIDIUM_CHECK(before.heaps[heap].driverBudgetAvailable);
        IRIDIUM_CHECK(before.heaps[heap].driverBudgetBytes > 0);
        IRIDIUM_CHECK(before.heaps[heap].driverBudgetBytes <= before.heaps[heap].heapSizeBytes);
        const uint64_t size = requirementsOf(gpu.device(), image).size;
        // Between refreshes VMA adds its own block changes to the fetched usage;
        // a refresh reads the driver's per-process usage, which includes the image.
        const FrameMemoryProfile estimated = allocator.memorySnapshot();
        IRIDIUM_CHECK(estimated.heaps[heap].driverUsageBytes >=
            before.heaps[heap].driverUsageBytes + size);
        allocator.beginFrame();
        const FrameMemoryProfile refreshed = allocator.memorySnapshot();
        IRIDIUM_CHECK(refreshed.heaps[heap].driverUsageBytes >=
            before.heaps[heap].driverUsageBytes + size);
        allocator.destroy(image);
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0, gpu.validationErrors() << " errors");
        return true;
    }

    bool testLifecycle() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        VulkanResourceAllocator allocator;
        bool threw = false;
        try {
            allocator.init(VK_NULL_HANDLE, gpu.physicalDevice(), gpu.device(), false);
        } catch (const std::invalid_argument&) { threw = true; }
        IRIDIUM_CHECK(threw);
        allocator.beginFrame(); // harmless before init
        for (int round = 0; round < 2; ++round) {
            allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(),
                gpu.hasMemoryBudget());
            threw = false;
            try {
                allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(), false);
            } catch (const std::logic_error&) { threw = true; }
            IRIDIUM_CHECK(threw);
            VulkanBufferResource buffer = allocator.createBuffer(1024,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            IRIDIUM_CHECK(buffer.isValid());
            allocator.destroy(buffer);
            allocator.cleanup();
        }
        return true;
    }

} // namespace

int main() {
    try {
        sharedDevice = std::make_unique<HeadlessVulkanDevice>(HeadlessVulkanDevice::Options{
            "Iridium resource allocator", false });
    } catch (const std::exception& exception) {
        std::cerr << "headless Vulkan device unavailable: " << exception.what() << '\n';
        return 1;
    }
    constexpr Iridium::Test::TestCase tests[] = {
        { "legacy memory-type rule", testLegacyTypeRule },
        { "dedicated image threshold", testDedicatedThreshold },
        { "memory types match the legacy choice", testMemoryTypesMatchLegacyChoice },
        { "suballocation and dedicated images", testSuballocationAndDedicatedImages },
        { "persistent mappings", testPersistentMappings },
        { "category accounting", testCategoryAccounting },
        { "memory-budget snapshot", testBudgetSnapshot },
        { "init and cleanup lifecycle", testLifecycle },
    };
    const int result = Iridium::Test::runTests(tests);
    sharedDevice.reset();
    return result;
}
