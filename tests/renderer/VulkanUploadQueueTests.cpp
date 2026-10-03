// M7R R4d: the upload path's device-free parts.
//   - Upload queue family selection: a dedicated transfer family with
//     (1,1,1) image transfer granularity, else a graphics-free compute family,
//     else the graphics family.
//   - The staging ring's bookkeeping (R4d.2): aligned contiguous allocations,
//     wrap past the end, batches retired oldest first only once both lanes'
//     timelines have passed their key, a full ring, and the fixed batch list
//     merging instead of allocating.

#include "renderer/vulkan/VulkanQueueSelection.h"
#include "renderer/vulkan/VulkanStagingRing.h"

#include <iostream>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (" << __FILE__ << ':' \
                << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

namespace {

    using namespace Iridium;

    VkQueueFamilyProperties family(VkQueueFlags flags, uint32_t count = 1,
        VkExtent3D granularity = { 1, 1, 1 }) {
        VkQueueFamilyProperties properties{};
        properties.queueFlags = flags;
        properties.queueCount = count;
        properties.minImageTransferGranularity = granularity;
        return properties;
    }

    constexpr VkQueueFlags Graphics = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
        VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
    constexpr VkQueueFlags Transfer = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
    constexpr VkQueueFlags Compute = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;

    bool testQueueSelection() {
        // NVIDIA-shaped: graphics 0, transfer 1, compute 2, video 3.
        std::vector<VkQueueFamilyProperties> nvidia{ family(Graphics, 16),
            family(Transfer, 2), family(Compute, 8),
            family(VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT, 1, { 0, 0, 0 }) };
        VulkanTransferQueueChoice choice = selectVulkanTransferQueueFamily(nvidia, 0);
        CHECK(choice.family == 1u);
        CHECK(choice.kind == VulkanTransferQueueKind::DedicatedTransfer);
        CHECK(vulkanTransferQueueKindName(choice.kind) == "dedicated-transfer");

        // A transfer family with coarse granularity is skipped; compute wins
        // even when listed before it.
        std::vector<VkQueueFamilyProperties> coarse{ family(Graphics),
            family(Compute), family(Transfer, 1, { 8, 8, 1 }) };
        choice = selectVulkanTransferQueueFamily(coarse, 0);
        CHECK(choice.family == 1u);
        CHECK(choice.kind == VulkanTransferQueueKind::AsyncCompute);

        // The dedicated family wins over an earlier compute family.
        std::vector<VkQueueFamilyProperties> ordered{ family(Compute),
            family(Graphics), family(Transfer) };
        choice = selectVulkanTransferQueueFamily(ordered, 1);
        CHECK(choice.family == 2u);
        CHECK(choice.kind == VulkanTransferQueueKind::DedicatedTransfer);

        // Only graphics (or families without queues): the graphics family.
        std::vector<VkQueueFamilyProperties> single{ family(Graphics),
            family(Transfer, 0) };
        choice = selectVulkanTransferQueueFamily(single, 0);
        CHECK(choice.family == 0u);
        CHECK(choice.kind == VulkanTransferQueueKind::Graphics);
        CHECK(vulkanTransferQueueKindName(choice.kind) == "graphics");

        // A second graphics family is never an upload family.
        std::vector<VkQueueFamilyProperties> twoGraphics{ family(Graphics),
            family(Graphics) };
        choice = selectVulkanTransferQueueFamily(twoGraphics, 0);
        CHECK(choice.kind == VulkanTransferQueueKind::Graphics);
        CHECK(choice.family == 0u);
        return true;
    }

    bool testRingAllocatesAligned() {
        StagingRingAllocator ring;
        ring.reset(1024);
        CHECK(ring.allocate(10, 256) == std::optional<uint64_t>(0));
        CHECK(ring.allocate(10, 256) == std::optional<uint64_t>(256));
        CHECK(ring.usedBytes() == 266u);
        CHECK(ring.hasOpenAllocations());
        CHECK(!ring.allocate(2000, 1)); // larger than the ring
        ring.close({ 0, 1 });
        CHECK(!ring.hasOpenAllocations());
        CHECK(ring.sealedBatchCount() == 1u);
        // Not retired before the graphics lane reaches 1.
        CHECK(ring.retire(100, 0) == 0u);
        CHECK(ring.retire(0, 1) == 1u);
        CHECK(ring.usedBytes() == 0u);
        // An empty ring restarts at 0.
        CHECK(ring.allocate(8, 256) == std::optional<uint64_t>(0));
        return true;
    }

    bool testRingWrapsAndRetiresInOrder() {
        StagingRingAllocator ring;
        ring.reset(1000);
        CHECK(ring.allocate(400, 1) == std::optional<uint64_t>(0));
        ring.close({ 1, 0 });                                     // [0,400)
        CHECK(ring.allocate(400, 1) == std::optional<uint64_t>(400));
        ring.close({ 2, 0 });                                     // [400,800)
        // 300 bytes do not fit at the end (200 left) nor before the tail (0).
        CHECK(!ring.allocate(300, 1));
        // The second batch completing alone frees nothing: FIFO.
        CHECK(ring.retire(0, 0) == 0u);
        CHECK(ring.retire(1, 0) == 1u);                           // tail -> 400
        // Wraps: skips [800,1000) and lands at 0.
        CHECK(ring.wrapCount() == 0u);
        CHECK(ring.allocate(300, 1) == std::optional<uint64_t>(0));
        CHECK(ring.wrapCount() == 1u);
        CHECK(ring.usedBytes() == 400u + 200u + 300u);
        ring.close({ 3, 5 });
        // Wrapped: room only between head (300) and tail (400).
        CHECK(ring.allocate(100, 1) == std::optional<uint64_t>(300));
        CHECK(!ring.allocate(1, 1));                              // exactly full
        ring.close({ 4, 0 });
        CHECK(ring.oldestSealedKey()->transferValue == 2u);
        // Batch 3 needs both lanes.
        CHECK(ring.retire(3, 4) == 1u);
        CHECK(ring.oldestSealedKey()->transferValue == 3u);
        CHECK(ring.retire(4, 5) == 2u);
        CHECK(ring.usedBytes() == 0u);
        CHECK(ring.sealedBatchCount() == 0u);
        return true;
    }

    bool testRingBatchListMerges() {
        StagingRingAllocator ring;
        ring.reset(1u << 20);
        for (uint64_t batch = 1; batch <= StagingRingAllocator::MaxBatches + 3; ++batch) {
            CHECK(ring.allocate(16, 16).has_value());
            ring.close({ batch, 0 });
        }
        CHECK(ring.sealedBatchCount() == StagingRingAllocator::MaxBatches);
        // The merged newest batch retires with the latest key.
        CHECK(ring.retire(StagingRingAllocator::MaxBatches - 1, 0) ==
            StagingRingAllocator::MaxBatches - 1);
        CHECK(ring.retire(StagingRingAllocator::MaxBatches + 2, 0) == 0u);
        CHECK(ring.retire(StagingRingAllocator::MaxBatches + 3, 0) == 1u);
        CHECK(ring.usedBytes() == 0u);
        // close() without allocations seals nothing.
        ring.close({ 99, 99 });
        CHECK(ring.sealedBatchCount() == 0u);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[] = {
        { "upload queue family selection", testQueueSelection },
        { "staging ring allocates aligned", testRingAllocatesAligned },
        { "staging ring wraps and retires in order", testRingWrapsAndRetiresInOrder },
        { "staging ring batch list merges when full", testRingBatchListMerges },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        bool passed = false;
        try {
            passed = test.run();
        }
        catch (const std::exception& exception) {
            std::cerr << "  exception: " << exception.what() << '\n';
        }
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        if (!passed) ++failures;
    }
    constexpr size_t count = sizeof(tests) / sizeof(tests[0]);
    std::cout << count - failures << '/' << count << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
