// M7R R4d: the upload path's device-free parts.
//   - Upload queue family selection: a dedicated transfer family with
//     (1,1,1) image transfer granularity, else a graphics-free compute family,
//     else the graphics family.

#include "renderer/vulkan/VulkanQueueSelection.h"

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

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[] = {
        { "upload queue family selection", testQueueSelection },
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
