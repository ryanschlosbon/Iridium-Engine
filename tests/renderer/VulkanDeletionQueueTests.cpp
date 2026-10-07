// M7R R4c.1: the fence-keyed deletion queue.
//   - Queue semantics without a device: FIFO collection by retire value, the
//     reserved capacity, a callback that retires while the queue collects,
//     and a late (out-of-order) key that only delays.
//   - VulkanFrameScheduler on a device (hidden GLFW window, validation with
//     synchronization validation). The R4 design's finding 3 ordering: a
//     resource used by frame N is retired between endFrame(N) and the next
//     beginFrame. The old slot-keyed defer queued it on the already-advanced
//     slot and freed it at that slot's fence (frame N-1), while frame N could
//     still be executing. Here it must survive that beginFrame and be destroyed
//     only once frame N's serial has completed; retiring inside an open frame
//     keys the frame being recorded.
//   - R4c.3: reflection-probe capture targets retire a replaced or removed
//     published cube through the queue while frames are in flight, and
//     destroy at once when none is.
//   - R4d.4: the scheduler runs on the graphics timeline (vkQueueSubmit2,
//     timeline waits, no frame fences) when the device supports it; the
//     retirement ordering and the non-waiting refresh also hold on the fence
//     path.

#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VkContext.h"
#include "renderer/vulkan/VkSwapchain.h"
#include "renderer/vulkan/VulkanDeletionQueue.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"
#include "renderer/vulkan/VulkanReflectionProbeCaptureTargets.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Iridium;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (" << __FILE__ << ':' \
                << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

namespace {

    struct CallbackLog {
        std::vector<uint64_t> order;
        // The scheduler's completed serial when each callback ran.
        std::vector<uint64_t> completedAtRun;
        const VulkanFrameScheduler* scheduler = nullptr;
        VulkanDeletionQueue* queue = nullptr;
    };

    void logCallback(void* user, const VulkanDeletionArguments& arguments) {
        auto& log = *static_cast<CallbackLog*>(user);
        log.order.push_back(arguments[0]);
        log.completedAtRun.push_back(log.scheduler != nullptr
            ? log.scheduler->completedSerial() : 0u);
    }

    // Retires one more entry from inside a collection.
    void retireAgainCallback(void* user, const VulkanDeletionArguments& arguments) {
        auto& log = *static_cast<CallbackLog*>(user);
        log.order.push_back(arguments[0]);
        log.queue->retireCallback(arguments[1], { logCallback, &log,
            { arguments[0] + 100u, 0, 0, 0 } });
    }

    VulkanDeletionCallback entry(CallbackLog& log, uint64_t id) {
        return { logCallback, &log, { id, 0, 0, 0 } };
    }

    bool testQueueCollectsInRetireOrder() {
        VulkanDeletionQueue queue;
        queue.init(VK_NULL_HANDLE, nullptr);
        CHECK(queue.capacity() >= VulkanDeletionQueue::InitialCapacity);
        CallbackLog log;
        queue.retireCallback(1, entry(log, 10));
        queue.retireCallback(1, entry(log, 11));
        queue.retireCallback(2, entry(log, 20));
        queue.retireCallback(3, entry(log, 30));
        CHECK(queue.size() == 4u);
        CHECK(queue.collect(0) == 0u);
        CHECK(log.order.empty());
        CHECK(queue.collect(1) == 2u);
        CHECK((log.order == std::vector<uint64_t>{ 10, 11 }));
        CHECK(queue.oldestRetireValue() == 2u);
        CHECK(queue.collect(3) == 2u);
        CHECK((log.order == std::vector<uint64_t>{ 10, 11, 20, 30 }));
        CHECK(queue.empty());
        // Null payloads are not queued.
        queue.retire(5, VulkanBufferResource{});
        queue.retire(5, VulkanImageResource{});
        queue.retireImageView(5, VK_NULL_HANDLE);
        queue.retireCallback(5, VulkanDeletionCallback{});
        CHECK(queue.empty());
        return true;
    }

    bool testQueueCapacityIsReserved() {
        VulkanDeletionQueue queue;
        queue.init(VK_NULL_HANDLE, nullptr);
        CallbackLog log;
        const size_t capacity = queue.capacity();
        // Steady retire/collect cycles below the reserve never reallocate.
        for (uint64_t frame = 1; frame <= 64; ++frame) {
            for (uint64_t item = 0; item < 8; ++item)
                queue.retireCallback(frame, entry(log, frame));
            if (frame > 2) (void)queue.collect(frame - 2);
        }
        CHECK(queue.capacity() == capacity);
        (void)queue.flush();
        CHECK(queue.empty());
        CHECK(log.order.size() == 64u * 8u);
        return true;
    }

    bool testQueueCallbackMayRetire() {
        VulkanDeletionQueue queue;
        queue.init(VK_NULL_HANDLE, nullptr);
        CallbackLog log;
        log.queue = &queue;
        // The nested entry keys serial 4: collecting 2 must leave it queued.
        queue.retireCallback(2, { retireAgainCallback, &log, { 1, 4, 0, 0 } });
        queue.retireCallback(2, entry(log, 2));
        CHECK(queue.collect(2) == 2u);
        CHECK((log.order == std::vector<uint64_t>{ 1, 2 }));
        CHECK(queue.size() == 1u);
        CHECK(queue.oldestRetireValue() == 4u);
        CHECK(queue.collect(4) == 1u);
        CHECK((log.order == std::vector<uint64_t>{ 1, 2, 101 }));
        return true;
    }

    bool testQueueLateKeyOnlyDelays() {
        VulkanDeletionQueue queue;
        queue.init(VK_NULL_HANDLE, nullptr);
        CallbackLog log;
        queue.retireCallback(5, entry(log, 5));
        queue.retireCallback(3, entry(log, 3)); // out of order
        CHECK(queue.collect(4) == 0u);          // never early
        CHECK(queue.collect(5) == 2u);
        CHECK((log.order == std::vector<uint64_t>{ 5, 3 }));
        return true;
    }

    class HiddenWindow {
    public:
        HiddenWindow() {
            if (glfwInit() != GLFW_TRUE) throw std::runtime_error("glfwInit failed");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            window_ = glfwCreateWindow(320, 180, "VulkanDeletionQueueTests",
                nullptr, nullptr);
            if (window_ == nullptr) {
                glfwTerminate();
                throw std::runtime_error("glfwCreateWindow failed");
            }
        }
        ~HiddenWindow() {
            glfwDestroyWindow(window_);
            glfwTerminate();
        }
        HiddenWindow(const HiddenWindow&) = delete;
        HiddenWindow& operator=(const HiddenWindow&) = delete;
        [[nodiscard]] GLFWwindow* get() const noexcept { return window_; }

    private:
        GLFWwindow* window_ = nullptr;
    };

    // VkContext reports validation messages on std::cerr ("[Validation]: ").
    class ValidationCapture {
    public:
        ValidationCapture() : previous_(std::cerr.rdbuf(text_.rdbuf())) {}
        ~ValidationCapture() { std::cerr.rdbuf(previous_); }
        [[nodiscard]] size_t messages() const {
            const std::string text = text_.str();
            size_t count = 0;
            for (size_t at = text.find("[Validation]"); at != std::string::npos;
                    at = text.find("[Validation]", at + 1))
                ++count;
            return count;
        }
        [[nodiscard]] std::string text() const { return text_.str(); }

    private:
        std::ostringstream text_;
        std::streambuf* previous_ = nullptr;
    };

    struct Device {
        HiddenWindow window;
        std::unique_ptr<VkContext> context;
        std::unique_ptr<VkSwapchain> swapchain;
        VulkanResourceAllocator allocator;
        VulkanFrameScheduler scheduler;

        explicit Device(bool timeline = true) {
            context = std::make_unique<VkContext>(true, false, false,
                window.get(), true);
            allocator.init(context->getInstance(), context->getPhysicalDevice(),
                context->getDevice(), context->hasMemoryBudget());
            swapchain = std::make_unique<VkSwapchain>(context.get(), window.get());
            scheduler.init(context->getDevice(), context->getGraphicsQueue(),
                context->getPresentQueue(), context->getGraphicsQueueFamily(),
                swapchain->getImageCount(), nullptr, false, 0.0, 0, false,
                false, 0, context->hasSynchronization2(),
                timeline && context->hasTimelineSemaphore());
            scheduler.attachAllocator(allocator);
        }
        ~Device() {
            vkDeviceWaitIdle(context->getDevice());
            scheduler.cleanup();
            swapchain.reset();
            allocator.cleanup();
            context.reset();
        }
    };

    // One frame: fills `buffer` (when given) and presents the acquired image.
    bool recordFrame(Device& device, const VulkanBufferResource* buffer,
        void (*insideFrame)(void*) = nullptr, void* user = nullptr) {
        const VulkanFrameBegin begin =
            device.scheduler.beginFrame(device.swapchain->getSwapchain());
        CHECK(begin.status == FrameStatus::Ready);
        if (buffer != nullptr)
            vkCmdFillBuffer(begin.commandBuffer, buffer->buffer, 0,
                VK_WHOLE_SIZE, 0x5eedu);
        VkImageMemoryBarrier present{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        present.srcAccessMask = 0;
        present.dstAccessMask = 0;
        present.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.image = device.swapchain->getImages().at(begin.imageIndex);
        present.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(begin.commandBuffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
            1, &present);
        if (insideFrame != nullptr) insideFrame(user);
        (void)device.scheduler.endFrame(device.swapchain->getSwapchain(),
            begin.imageIndex);
        return true;
    }

    bool retiredBetweenFramesOutlivesItsFrame(bool timeline) {
        ValidationCapture validation;
        {
            Device device(timeline);
            if (timeline && device.scheduler.graphicsTimeline() == VK_NULL_HANDLE) {
                std::cerr << "  the device has no timeline semaphores\n";
                return false;
            }
            VulkanFrameScheduler& scheduler = device.scheduler;
            CallbackLog log;
            log.scheduler = &scheduler;
            VulkanBufferResource buffer = device.allocator.createBuffer(1u << 20,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::OtherUnclassified);

            // Frame N (serial 1, slot 0) uses the buffer.
            CHECK(recordFrame(device, &buffer));
            CHECK(scheduler.lastSubmittedSerial() == 1u);
            CHECK(scheduler.currentFrameIndex() == 1u);
            // Retired between endFrame and the next beginFrame: the frame
            // that used it is the last submitted one.
            CHECK(scheduler.retireValue() == 1u);
            scheduler.retire(buffer);
            scheduler.retireCallback(entry(log, 1));
            CHECK(scheduler.deletionQueue().size() == 2u);

            // The next beginFrame waits slot 1 (never submitted). The old
            // defer flushed here; frame N may still be executing.
            CHECK(recordFrame(device, nullptr));
            CHECK(log.order.empty());
            CHECK(scheduler.deletionQueue().size() == 2u);
            CHECK(scheduler.completedSerial() < 1u);

            // Retiring inside an open frame keys the frame being recorded.
            struct Inside {
                VulkanFrameScheduler* scheduler;
                CallbackLog* log;
                uint64_t value;
            } inside{ &scheduler, &log, 0 };
            CHECK(recordFrame(device, nullptr, [](void* user) {
                auto& state = *static_cast<Inside*>(user);
                state.value = state.scheduler->retireValue();
                state.scheduler->retireCallback(entry(*state.log, 3));
            }, &inside));
            // That beginFrame waited serial 1: the buffer and its callback
            // were destroyed then, never earlier.
            CHECK((log.order == std::vector<uint64_t>{ 1 }));
            CHECK(log.completedAtRun.front() >= 1u);
            CHECK(inside.value == 3u);
            CHECK(scheduler.deletionQueue().size() == 1u);

            CHECK(recordFrame(device, nullptr)); // waits serial 2
            CHECK(log.order.size() == 1u);
            CHECK(recordFrame(device, nullptr)); // waits serial 3
            CHECK((log.order == std::vector<uint64_t>{ 1, 3 }));
            CHECK(log.completedAtRun.back() >= 3u);
            CHECK(scheduler.deletionQueue().empty());

            // Retired after the last frame: flushed by cleanup.
            scheduler.retireCallback(entry(log, 9));
            // Presentation also waits on the frame semaphores (backend cleanup).
            vkDeviceWaitIdle(device.context->getDevice());
            scheduler.cleanup();
            CHECK((log.order == std::vector<uint64_t>{ 1, 3, 9 }));
        }
        if (validation.messages() != 0) {
            std::cout << validation.text();
            return false;
        }
        return true;
    }

    bool testRetiredBetweenFramesOutlivesItsFrame() {
        return retiredBetweenFramesOutlivesItsFrame(true);
    }
    bool testRetiredBetweenFramesFencePath() {
        return retiredBetweenFramesOutlivesItsFrame(false);
    }

    bool testCaptureTargetsRetireWhileInFlight() {
        ValidationCapture validation;
        {
            Device device;
            VulkanFrameScheduler& scheduler = device.scheduler;
            VulkanReflectionProbeCaptureTargets targets;
            targets.init(device.context->getDevice(),
                device.context->getPhysicalDevice(), device.allocator);
            targets.setDeferredDestruction(&scheduler);
            const SceneEntityUuid owner = *SceneEntityUuid::parse(
                "019fb73d-5a80-7000-8000-000000000123");

            // Nothing submitted: promotion destroys the staging at once.
            (void)targets.acquire(owner, 1, 128);
            targets.promote(owner, 1);
            CHECK(scheduler.deletionQueue().empty());
            CHECK(targets.published(owner) != nullptr);

            // A frame in flight may sample the published cube: replacing it
            // (and the staging the replacement came from) is deferred.
            CHECK(recordFrame(device, nullptr));
            (void)targets.acquire(owner, 2, 128);
            targets.promote(owner, 2);
            const size_t afterPromote = scheduler.deletionQueue().size();
            CHECK(afterPromote > 0u);
            CHECK(scheduler.deletionQueue().oldestRetireValue() == 1u);
            targets.remove(owner);
            CHECK(targets.publishedCount() == 0u);
            CHECK(targets.publishedLogicalBytes() == 0u);
            CHECK(scheduler.deletionQueue().size() == afterPromote + 1u);

            // Collected once serial 1 has completed (slot 0's next fence).
            CHECK(recordFrame(device, nullptr));
            CHECK(!scheduler.deletionQueue().empty());
            CHECK(recordFrame(device, nullptr));
            CHECK(scheduler.deletionQueue().empty());
            vkDeviceWaitIdle(device.context->getDevice());
            targets.cleanup();
        }
        if (validation.messages() != 0) {
            std::cout << validation.text();
            return false;
        }
        return true;
    }

    bool refreshCompletedSerialDoesNotWait(bool timeline) {
        ValidationCapture validation;
        {
            Device device(timeline);
            VulkanFrameScheduler& scheduler = device.scheduler;
            CHECK(recordFrame(device, nullptr));
            CHECK(recordFrame(device, nullptr));
            vkDeviceWaitIdle(device.context->getDevice());
            // Both fences have signalled but neither slot was waited.
            CHECK(scheduler.slotInFlight(0) && scheduler.slotInFlight(1));
            scheduler.refreshCompletedSerial();
            CHECK(scheduler.completedSerial() == 2u);
            CHECK(scheduler.slotInFlight(0) && scheduler.slotInFlight(1));
            scheduler.waitForAllFrames();
            CHECK(!scheduler.slotInFlight(0) && !scheduler.slotInFlight(1));
        }
        if (validation.messages() != 0) {
            std::cout << validation.text();
            return false;
        }
        return true;
    }
    bool testRefreshCompletedSerialDoesNotWait() {
        return refreshCompletedSerialDoesNotWait(true);
    }
    bool testRefreshCompletedSerialFencePath() {
        return refreshCompletedSerialDoesNotWait(false);
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    constexpr TestCase tests[] = {
        { "Queue collects in retire order", testQueueCollectsInRetireOrder },
        { "Queue capacity is reserved", testQueueCapacityIsReserved },
        { "Queue callback may retire", testQueueCallbackMayRetire },
        { "Queue late key only delays", testQueueLateKeyOnlyDelays },
        { "Retired between frames outlives its frame",
            testRetiredBetweenFramesOutlivesItsFrame },
        { "Retired between frames outlives its frame (fence path)",
            testRetiredBetweenFramesFencePath },
        { "refreshCompletedSerial does not wait",
            testRefreshCompletedSerialDoesNotWait },
        { "refreshCompletedSerial does not wait (fence path)",
            testRefreshCompletedSerialFencePath },
        { "Capture targets retire while frames are in flight",
            testCaptureTargetsRetireWhileInFlight },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
