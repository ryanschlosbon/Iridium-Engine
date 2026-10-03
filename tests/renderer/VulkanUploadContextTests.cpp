// M7R R4d: VulkanUploadContext on a headless device under the validation
// layer with synchronization validation. Every case uploads patterned buffer
// and mip-chained image data, reads it back on the graphics queue and compares
// bytes.
//   - LegacyBlocking (the pre-R4d path) and the staging-ring paths.
//   - The ring: uploads above a quarter of the ring use dedicated staging; a
//     batch that fills the ring falls back to dedicated staging; staging is
//     reclaimed and reused once the lane timeline passes the batch.
//   - R4d.3 asynchronous uploads: submitAsync without a CPU wait, then a
//     "frame" graphics submission that records the queue-family acquires
//     first and waits on the upload timelines; on the device's transfer queue
//     (auto) and on the graphics queue (graphics, the same-family fallback).
//     A blocking flush acquires in its own graphics batch. Non-fresh
//     destinations stay on graphics. Back-to-back asynchronous batches wrap
//     the ring and wait for the oldest batch when it is full. The retire
//     floor keeps resources alive until the frame that waits on their upload.

#include "HeadlessVulkanDevice.h"
#include "TestHarness.h"

#include "renderer/vulkan/VulkanCommandList.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"
#include "renderer/vulkan/VulkanUploadContext.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

    using namespace Iridium;
    using Iridium::Test::HeadlessVulkanBuffer;
    using Iridium::Test::HeadlessVulkanDevice;

    std::unique_ptr<HeadlessVulkanDevice> sharedDevice;
    constexpr VkDeviceSize KiB = 1024;
    constexpr VkDeviceSize MiB = 1024 * KiB;

    std::vector<std::byte> pattern(size_t size, uint32_t seed) {
        std::vector<std::byte> bytes(size);
        uint32_t state = seed * 2654435761u + 1u;
        for (std::byte& value : bytes) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<std::byte>(state >> 24);
        }
        return bytes;
    }

    bool noValidationErrors(const char* stage) {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        if (gpu.validationErrors() == 0) return true;
        std::cerr << "  " << stage << ": " << gpu.validationErrors()
            << " validation error(s)\n";
        for (const std::string& message : gpu.validationMessages())
            std::cerr << "    " << message << '\n';
        return false;
    }

    struct Rig {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        VulkanResourceAllocator allocator;
        VulkanUploadContext uploads;

        explicit Rig(UploadQueueMode mode, VkDeviceSize ringBytes =
            VulkanUploadContext::DefaultStagingRingBytes) {
            gpu.resetValidationErrors();
            allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(),
                gpu.hasMemoryBudget());
            uploads.init(gpu.device(), VulkanUploadContext::Queues{
                    .graphics = gpu.queue(),
                    .graphicsFamily = gpu.queueFamily(),
                    .transfer = gpu.transferQueue(),
                    .transferFamily = gpu.transferQueueFamily(),
                }, allocator, nullptr, VulkanUploadContext::Options{
                    .mode = mode,
                    .timelineSemaphore = gpu.hasTimelineSemaphore(),
                    .synchronization2 = gpu.hasSynchronization2(),
                    .stagingRingBytes = ringBytes,
                });
        }
        ~Rig() {
            uploads.cleanup();
            vkDeviceWaitIdle(gpu.device());
            if (framePool != VK_NULL_HANDLE)
                vkDestroyCommandPool(gpu.device(), framePool, nullptr);
            for (VulkanBufferResource& buffer : buffers) allocator.destroy(buffer);
            for (VulkanImageResource& image : images) allocator.destroy(image);
            allocator.cleanup();
        }

        VulkanBufferResource& buffer(VkDeviceSize size) {
            buffers.push_back(allocator.createBuffer(size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::GeometryVertex));
            return buffers.back();
        }
        VulkanImageResource& image(uint32_t size, uint32_t mips) {
            images.push_back(allocator.createImage2D({ size, size },
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, ProfileMemoryCategory::Texture, mips));
            return images.back();
        }

        // A frame-shaped graphics submission: the upload acquires first, then
        // `record`, waiting on the upload timelines at ALL_COMMANDS (what
        // VulkanVertexBackend::beginFrame and the scheduler do).
        template <typename Record>
        void frame(Record&& record) {
            if (framePool == VK_NULL_HANDLE) {
                VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                poolInfo.queueFamilyIndex = gpu.queueFamily();
                if (vkCreateCommandPool(gpu.device(), &poolInfo, nullptr, &framePool) !=
                    VK_SUCCESS) throw std::runtime_error("frame command pool");
            }
            VkCommandBufferAllocateInfo allocation{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocation.commandPool = framePool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(gpu.device(), &allocation, &commands) != VK_SUCCESS)
                throw std::runtime_error("frame command buffer");
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(commands, &begin);
            const VulkanUploadContext::FrameWaits waits =
                uploads.recordFrameAcquires(commands);
            record(commands);
            vkEndCommandBuffer(commands);
            std::array<VkSemaphore, 2> semaphores{};
            std::array<uint64_t, 2> values{};
            std::array<VkPipelineStageFlags, 2> stages{ VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
            uint32_t count = 0;
            if (waits.transferValue != 0) {
                semaphores[count] = waits.transferTimeline;
                values[count++] = waits.transferValue;
            }
            if (waits.graphicsValue != 0) {
                semaphores[count] = waits.graphicsTimeline;
                values[count++] = waits.graphicsValue;
            }
            lastFrameWaitCount = count;
            VkTimelineSemaphoreSubmitInfo timeline{
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            timeline.waitSemaphoreValueCount = count;
            timeline.pWaitSemaphoreValues = values.data();
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.pNext = &timeline;
            submit.waitSemaphoreCount = count;
            submit.pWaitSemaphores = semaphores.data();
            submit.pWaitDstStageMask = stages.data();
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commands;
            if (vkQueueSubmit(gpu.queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
                throw std::runtime_error("frame submit");
            vkQueueWaitIdle(gpu.queue());
            vkFreeCommandBuffers(gpu.device(), framePool, 1, &commands);
        }

        template <typename Record>
        void run(bool asFrame, Record&& record) {
            if (asFrame) frame(record);
            else gpu.submitAndWait(record);
        }

        // Graphics-queue readback after the upload completed (asFrame: in a
        // frame-shaped submission that acquires and waits first).
        std::vector<std::byte> read(VulkanBufferResource& source, VkDeviceSize size,
            bool asFrame = false) {
            HeadlessVulkanBuffer host = gpu.createHostBuffer(size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            run(asFrame, [&](VkCommandBuffer commands) {
                VulkanCommandList(commands).transition(source, ResourceState::CopySource);
                VkBufferCopy region{ 0, 0, size };
                vkCmdCopyBuffer(commands, source.buffer, host.buffer, 1, &region);
            });
            std::vector<std::byte> bytes(static_cast<size_t>(size));
            std::memcpy(bytes.data(), host.mapped, bytes.size());
            gpu.destroy(host);
            return bytes;
        }
        std::vector<std::byte> read(VulkanImageResource& source, VkDeviceSize size,
            bool asFrame = false) {
            HeadlessVulkanBuffer host = gpu.createHostBuffer(size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            run(asFrame, [&](VkCommandBuffer commands) {
                VulkanCommandList(commands).transition(source, ResourceState::CopySource);
                VkDeviceSize offset = 0;
                uint32_t extent = source.extent.width;
                for (uint32_t level = 0; level < source.mipLevels; ++level) {
                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
                    region.imageExtent = { extent, extent, 1 };
                    vkCmdCopyImageToBuffer(commands, source.image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host.buffer, 1, &region);
                    offset += VkDeviceSize{ extent } * extent * 4;
                    extent = extent > 1 ? extent / 2 : 1;
                }
            });
            std::vector<std::byte> bytes(static_cast<size_t>(size));
            std::memcpy(bytes.data(), host.mapped, bytes.size());
            gpu.destroy(host);
            return bytes;
        }

        // Deques: references handed out stay valid.
        std::deque<VulkanBufferResource> buffers;
        std::deque<VulkanImageResource> images;
        VkCommandPool framePool = VK_NULL_HANDLE;
        uint32_t lastFrameWaitCount = 0;
    };

    VkDeviceSize mipChainBytes(uint32_t size, uint32_t mips) {
        VkDeviceSize bytes = 0;
        for (uint32_t level = 0; level < mips; ++level) {
            bytes += VkDeviceSize{ size } * size * 4;
            size = size > 1 ? size / 2 : 1;
        }
        return bytes;
    }

    // One buffer and one 64x64 mip-chained image per mode, uploaded, flushed
    // and read back.
    bool roundTrip(UploadQueueMode mode) {
        {
            Rig rig(mode);
            const std::vector<std::byte> bufferBytes = pattern(96 * KiB + 4, 1);
            const VkDeviceSize imageBytes = mipChainBytes(64, 7);
            const std::vector<std::byte> imageData = pattern(
                static_cast<size_t>(imageBytes), 2);
            VulkanBufferResource& buffer = rig.buffer(bufferBytes.size());
            VulkanImageResource& image = rig.image(64, 7);
            rig.uploads.enqueueBufferUpload(buffer, bufferBytes, ResourceState::VertexBuffer);
            rig.uploads.enqueueImageUpload(image, imageData, ResourceState::ShaderResource);
            IRIDIUM_CHECK(rig.uploads.hasPendingWork());
            rig.uploads.flush();
            IRIDIUM_CHECK(!rig.uploads.hasPendingWork());
            IRIDIUM_CHECK(buffer.state == ResourceState::VertexBuffer);
            IRIDIUM_CHECK(image.state == ResourceState::ShaderResource);
            IRIDIUM_CHECK(rig.read(buffer, bufferBytes.size()) == bufferBytes);
            IRIDIUM_CHECK(rig.read(image, imageBytes) == imageData);
            if (mode != UploadQueueMode::LegacyBlocking) {
                IRIDIUM_CHECK(rig.uploads.mode() == mode);
                IRIDIUM_CHECK(rig.uploads.stagingRing().usedBytes() == 0u);
                IRIDIUM_CHECK(rig.uploads.dedicatedStagingCount() == 0u);
            }
            IRIDIUM_CHECK(rig.uploads.telemetry().submittedBatches == 1u);
            IRIDIUM_CHECK(rig.uploads.telemetry().submittedBytes ==
                bufferBytes.size() + imageBytes);
        }
        return noValidationErrors("round trip");
    }

    bool testLegacyRoundTrip() { return roundTrip(UploadQueueMode::LegacyBlocking); }
    bool testGraphicsRoundTrip() { return roundTrip(UploadQueueMode::Graphics); }
    // Fresh uploads on the transfer queue, acquired by the blocking flush.
    bool testTransferBlockingRoundTrip() { return roundTrip(UploadQueueMode::Auto); }

    // submitAsync, then a frame that acquires, waits and reads back.
    bool asyncRoundTrip(UploadQueueMode mode) {
        {
            Rig rig(mode);
            IRIDIUM_CHECK(rig.uploads.usesTransferQueue() ==
                (mode == UploadQueueMode::Auto &&
                    rig.gpu.transferQueueFamily() != rig.gpu.queueFamily()));
            const std::vector<std::byte> bufferBytes = pattern(80 * KiB, 21);
            const VkDeviceSize imageBytes = mipChainBytes(128, 8);
            const std::vector<std::byte> imageData = pattern(
                static_cast<size_t>(imageBytes), 22);
            VulkanBufferResource& buffer = rig.buffer(bufferBytes.size());
            VulkanImageResource& image = rig.image(128, 8);
            rig.uploads.enqueueBufferUpload(buffer, bufferBytes, ResourceState::VertexBuffer);
            rig.uploads.enqueueImageUpload(image, imageData, ResourceState::ShaderResource);
            // Open work: a resource retired now must outlive the next frame.
            IRIDIUM_CHECK(rig.uploads.retireFloor(5, false) == 6u);
            IRIDIUM_CHECK(rig.uploads.retireFloor(5, true) == 7u);
            rig.uploads.submitAsync();
            IRIDIUM_CHECK(rig.uploads.asyncSubmitCount() == 1u);
            // Submitted, not yet waited by a frame.
            IRIDIUM_CHECK(rig.uploads.retireFloor(5, false) == 6u);
            IRIDIUM_CHECK(rig.read(buffer, bufferBytes.size(), true) == bufferBytes);
            IRIDIUM_CHECK(rig.lastFrameWaitCount == 1u);
            IRIDIUM_CHECK(rig.uploads.retireFloor(5, false) == 0u);
            IRIDIUM_CHECK(rig.read(image, imageBytes) == imageData);

            // A second upload into the (now non-fresh) buffer stays on the
            // graphics lane, also in auto.
            const std::vector<std::byte> second = pattern(80 * KiB, 23);
            rig.uploads.enqueueBufferUpload(buffer, second, ResourceState::VertexBuffer);
            rig.uploads.submitAsync();
            IRIDIUM_CHECK(rig.read(buffer, second.size(), true) == second);
            IRIDIUM_CHECK(rig.lastFrameWaitCount == 1u);
            IRIDIUM_CHECK(rig.uploads.retireFloor(9, false) == 0u);
            IRIDIUM_CHECK(rig.uploads.ringWaitCount() == 0u);
        }
        return noValidationErrors("async round trip");
    }

    bool testTransferAsyncRoundTrip() { return asyncRoundTrip(UploadQueueMode::Auto); }
    bool testGraphicsAsyncRoundTrip() { return asyncRoundTrip(UploadQueueMode::Graphics); }

    // Back-to-back asynchronous batches in a 1 MiB ring: batches wrap, and a
    // full ring waits (counted) for the oldest submitted batch.
    bool testAsyncRingWrapAndWait() {
        {
            Rig rig(UploadQueueMode::Auto, 1 * MiB);
            std::vector<std::vector<std::byte>> data;
            std::vector<VulkanBufferResource*> targets;
            for (uint32_t batch = 0; batch < 6; ++batch) {
                for (uint32_t index = 0; index < 2; ++index) {
                    data.push_back(pattern(200 * KiB, 200 + batch * 2 + index));
                    targets.push_back(&rig.buffer(200 * KiB));
                    rig.uploads.enqueueBufferUpload(*targets.back(), data.back(),
                        ResourceState::VertexBuffer);
                }
                rig.uploads.submitAsync();
            }
            // 2.4 MiB through a 1 MiB ring in six 400 KiB batches: never
            // dedicated; the ring wraps behind live batches. Whether a full
            // ring had to wait depends on how fast the transfer queue retired
            // the oldest batch (usually it does wait).
            IRIDIUM_CHECK(rig.uploads.dedicatedStagingCount() == 0u);
            IRIDIUM_CHECK(rig.uploads.stagingRing().wrapCount() >= 1u);
            std::cout << "  ring waits: " << rig.uploads.ringWaitCount()
                << ", wraps: " << rig.uploads.stagingRing().wrapCount() << '\n';
            for (size_t index = 0; index < targets.size(); ++index)
                IRIDIUM_CHECK(rig.read(*targets[index], 200 * KiB, index == 0) ==
                    data[index]);
        }
        return noValidationErrors("async ring wrap and wait");
    }

    // A 1 MiB ring: uploads above 256 KiB use dedicated staging; a batch that
    // fills the ring falls back to dedicated staging for the rest; later
    // batches reuse the reclaimed ring.
    bool testRingOversizedAndWrap() {
        {
            Rig rig(UploadQueueMode::Graphics, 1 * MiB);
            IRIDIUM_CHECK(rig.uploads.stagingRing().capacity() == 1 * MiB);

            const std::vector<std::byte> large = pattern(300 * KiB, 3);
            VulkanBufferResource& oversized = rig.buffer(large.size());
            rig.uploads.enqueueBufferUpload(oversized, large, ResourceState::VertexBuffer);
            IRIDIUM_CHECK(rig.uploads.dedicatedStagingCount() == 1u);
            IRIDIUM_CHECK(rig.uploads.stagingRing().usedBytes() == 0u);
            IRIDIUM_CHECK(rig.uploads.liveDedicatedStagingCount() == 1u);

            // Five 250 KiB uploads in one batch: four fit, the fifth does not.
            std::vector<std::vector<std::byte>> data;
            std::vector<VulkanBufferResource*> targets;
            for (uint32_t index = 0; index < 5; ++index) {
                data.push_back(pattern(250 * KiB, 10 + index));
                targets.push_back(&rig.buffer(250 * KiB));
            }
            for (uint32_t index = 0; index < 5; ++index)
                rig.uploads.enqueueBufferUpload(*targets[index], data[index],
                    ResourceState::VertexBuffer);
            IRIDIUM_CHECK(rig.uploads.dedicatedStagingCount() == 2u);
            IRIDIUM_CHECK(rig.uploads.ringWaitCount() == 0u);
            rig.uploads.flush();
            IRIDIUM_CHECK(rig.uploads.liveDedicatedStagingCount() == 0u);
            IRIDIUM_CHECK(rig.uploads.stagingRing().usedBytes() == 0u);
            IRIDIUM_CHECK(rig.read(oversized, large.size()) == large);
            for (uint32_t index = 0; index < 5; ++index)
                IRIDIUM_CHECK(rig.read(*targets[index], 250 * KiB) == data[index]);

            // Three batches of three 200 KiB uploads reuse the ring.
            for (uint32_t batch = 0; batch < 3; ++batch) {
                std::vector<std::vector<std::byte>> batchData;
                std::vector<VulkanBufferResource*> batchTargets;
                for (uint32_t index = 0; index < 3; ++index) {
                    batchData.push_back(pattern(200 * KiB, 100 + batch * 3 + index));
                    batchTargets.push_back(&rig.buffer(200 * KiB));
                    rig.uploads.enqueueBufferUpload(*batchTargets.back(),
                        batchData.back(), ResourceState::VertexBuffer);
                }
                rig.uploads.flush();
                for (uint32_t index = 0; index < 3; ++index)
                    IRIDIUM_CHECK(rig.read(*batchTargets[index], 200 * KiB) ==
                        batchData[index]);
            }
            IRIDIUM_CHECK(rig.uploads.dedicatedStagingCount() == 2u);
        }
        return noValidationErrors("ring oversized and wrap");
    }

} // namespace

int main() {
    try {
        sharedDevice = std::make_unique<HeadlessVulkanDevice>(HeadlessVulkanDevice::Options{
            "Iridium upload context", true });
    } catch (const std::exception& exception) {
        std::cerr << "headless Vulkan device unavailable: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "Graphics family " << sharedDevice->queueFamily() << ", upload family "
        << sharedDevice->transferQueueFamily() << ", timeline semaphores "
        << (sharedDevice->hasTimelineSemaphore() ? "on" : "off") << '\n';
    constexpr Iridium::Test::TestCase tests[] = {
        { "legacy-blocking round trip", testLegacyRoundTrip },
        { "graphics staging-ring round trip", testGraphicsRoundTrip },
        { "transfer queue round trip, blocking flush", testTransferBlockingRoundTrip },
        { "ring: oversized, open-batch overflow, reuse", testRingOversizedAndWrap },
        { "transfer queue asynchronous frame acquire", testTransferAsyncRoundTrip },
        { "graphics fallback asynchronous frame wait", testGraphicsAsyncRoundTrip },
        { "asynchronous ring wrap and full-ring wait", testAsyncRingWrapAndWait },
    };
    const int result = Iridium::Test::runTests(tests);
    sharedDevice.reset();
    return result;
}
