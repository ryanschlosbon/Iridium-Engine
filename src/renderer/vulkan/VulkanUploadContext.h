#pragma once

#include "VulkanResourceAllocator.h"
#include "VulkanStagingRing.h"
#include "renderer/rhi/RenderBackendConfig.h"
#include "renderer/rhi/RenderBackendRuntimeInfo.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    class CpuProfiler;

    // Resource uploads (M7R R4d).
    //
    // LegacyBlocking (the pre-R4d path): commands begin lazily on the first
    // enqueue into one graphics command buffer; every upload allocates its own
    // staging buffer; flush submits once, waits on the batch fence and destroys
    // the staging buffers.
    //
    // Otherwise staging bytes come from one persistently mapped ring
    // (UploadStaging, 64 MiB by default); an upload larger than a quarter of
    // the ring, or one the open batch leaves no room for, gets a dedicated
    // staging buffer. Recorded work goes to an upload lane: a command pool, a
    // few command buffers and a timeline semaphore per queue. Ring batches,
    // dedicated staging and command buffers are reused once their lane's
    // timeline has passed the value their batch signalled. When the ring is
    // full the CPU waits for the oldest submitted batch
    // (cpu.renderer.upload_wait).
    //
    // Auto with a separate upload family (R4d.3): uploads into fresh
    // destinations (state Undefined) are recorded on the transfer lane as
    // copy + release (queue-family ownership transfer to graphics, carrying
    // the final layout); the matching acquires are recorded at the start of
    // the next frame command buffer (recordFrameAcquires). Everything else
    // (non-fresh destinations, layout-only transitions) and every upload in
    // Graphics mode records on the graphics lane. submitAsync() submits both
    // lanes without a CPU wait; the frame submission waits on the values
    // recordFrameAcquires returns. flush() stays bounded and blocking (init,
    // target rebuilds, error paths, cleanup).
    class VulkanUploadContext final {
    public:
        static constexpr VkDeviceSize DefaultStagingRingBytes =
            VkDeviceSize{ 64 } * 1024 * 1024;
        static constexpr uint32_t LaneCommandBufferCount = 4;

        struct Queues {
            VkQueue graphics = VK_NULL_HANDLE;
            uint32_t graphicsFamily = 0;
            VkQueue transfer = VK_NULL_HANDLE;
            uint32_t transferFamily = 0;
        };
        struct Options {
            UploadQueueMode mode = UploadQueueMode::Auto;
            // Without timeline semaphores every mode is LegacyBlocking.
            bool timelineSemaphore = false;
            bool synchronization2 = false;
            VkDeviceSize stagingRingBytes = DefaultStagingRingBytes;
        };

        VulkanUploadContext() = default;
        VulkanUploadContext(const VulkanUploadContext&) = delete;
        VulkanUploadContext& operator=(const VulkanUploadContext&) = delete;

        // The pre-R4d entry point: LegacyBlocking on the graphics queue.
        void init(VkDevice device, VkQueue graphicsQueue, uint32_t graphicsQueueFamily,
            VulkanResourceAllocator& allocator, CpuProfiler* cpuProfiler);
        void init(VkDevice device, const Queues& queues,
            VulkanResourceAllocator& allocator, CpuProfiler* cpuProfiler,
            const Options& options);
        void cleanup();

        void enqueueBufferUpload(VulkanBufferResource& destination, std::span<const std::byte> data,
            ResourceState finalState);
        void enqueueImageUpload(VulkanImageResource& destination, std::span<const std::byte> data,
            ResourceState finalState);
        void enqueueTransition(VulkanImageResource& destination, ResourceState state);
        // Bounded and blocking: every upload enqueued so far has completed on
        // return (pending acquires are recorded in a graphics-lane batch).
        void flush();

        // The frame-boundary submit (VulkanVertexBackend::beginFrame): submits
        // the open lane batches, each signalling its timeline, without a CPU
        // wait. LegacyBlocking flushes as before.
        void submitAsync();

        // Timeline values a frame submission must wait on (ALL_COMMANDS);
        // zero values when nothing is pending.
        struct FrameWaits {
            VkSemaphore transferTimeline = VK_NULL_HANDLE;
            uint64_t transferValue = 0;
            VkSemaphore graphicsTimeline = VK_NULL_HANDLE;
            uint64_t graphicsValue = 0;
        };
        // Records the queue-family acquires of every submitted release at the
        // start of `frameCommands` (before any pass) and hands that frame the
        // waits for every batch submitted so far.
        [[nodiscard]] FrameWaits recordFrameAcquires(VkCommandBuffer frameCommands);

        // The lowest deletion-queue key that keeps a resource alive until the
        // frame waiting on every upload enqueued or submitted so far has
        // completed (0 when no upload is outstanding): that frame is the next
        // one to begin. VulkanFrameScheduler::setRetireFloor.
        [[nodiscard]] uint64_t retireFloor(uint64_t lastSubmittedSerial,
            bool frameRecording) const noexcept;
        static uint64_t retireFloorThunk(const void* user,
            uint64_t lastSubmittedSerial, bool frameRecording) noexcept {
            return static_cast<const VulkanUploadContext*>(user)->retireFloor(
                lastSubmittedSerial, frameRecording);
        }

        [[nodiscard]] bool hasPendingWork() const noexcept;
        // The mode in effect (LegacyBlocking when timelines are unavailable).
        [[nodiscard]] UploadQueueMode mode() const noexcept { return mode_; }
        [[nodiscard]] BackendUploadTelemetry telemetry() const noexcept {
            return {
                totalSubmittedBytes_, totalSubmittedBatches_,
                totalSubmitAndWaitNanoseconds_, ringWaits_,
                dedicatedStagingUploads_, asyncSubmits_
            };
        }

        // ---- device-test introspection (no effect on behaviour) ----------
        [[nodiscard]] const StagingRingAllocator& stagingRing() const noexcept {
            return ring_;
        }
        [[nodiscard]] uint64_t ringWaitCount() const noexcept { return ringWaits_; }
        [[nodiscard]] uint64_t dedicatedStagingCount() const noexcept {
            return dedicatedStagingUploads_;
        }
        [[nodiscard]] size_t liveDedicatedStagingCount() const noexcept {
            return dedicatedStaging_.size();
        }
        // Whether fresh uploads use the separate transfer lane.
        [[nodiscard]] bool usesTransferQueue() const noexcept {
            return lanes_[TransferLane].active();
        }
        [[nodiscard]] uint32_t transferQueueFamily() const noexcept {
            return lanes_[TransferLane].family;
        }
        [[nodiscard]] VkDeviceSize stagingRingBytes() const noexcept {
            return ring_.capacity();
        }
        [[nodiscard]] uint64_t asyncSubmitCount() const noexcept { return asyncSubmits_; }

    private:
        enum LaneIndex : uint32_t { TransferLane = 0, GraphicsLane = 1, LaneCount = 2 };

        struct Lane {
            VkQueue queue = VK_NULL_HANDLE;
            uint32_t family = 0;
            VkCommandPool pool = VK_NULL_HANDLE;
            std::array<VkCommandBuffer, LaneCommandBufferCount> commands{};
            // The timeline value each command buffer's last submission signals.
            std::array<uint64_t, LaneCommandBufferCount> commandValues{};
            uint32_t nextCommand = 0;
            // The command buffer being recorded, or UINT32_MAX.
            uint32_t open = UINT32_MAX;
            VkSemaphore timeline = VK_NULL_HANDLE;
            uint64_t lastSignaled = 0;
            uint64_t completed = 0;

            [[nodiscard]] bool active() const noexcept { return pool != VK_NULL_HANDLE; }
            [[nodiscard]] bool recording() const noexcept { return open != UINT32_MAX; }
        };

        struct DedicatedStaging {
            VulkanBufferResource buffer;
            StagingRetireKey key{};
            bool sealed = false;
        };

        struct StagingSpan {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
        };

        void initLane(Lane& lane, VkQueue queue, uint32_t family);
        void destroyLane(Lane& lane) noexcept;
        VkCommandBuffer beginLane(Lane& lane);
        // Ends and submits the lane's open command buffer, signalling
        // lastSignaled + 1 (after waiting `wait` >= waitValue when given);
        // returns the value (0 when nothing was open).
        uint64_t submitLane(Lane& lane, VkSemaphore wait = VK_NULL_HANDLE,
            uint64_t waitValue = 0);
        // Submits the open lane batches and seals their staging.
        void submitOpenBatches();
        void recordAcquires(VkCommandBuffer commands);
        void publish(VkCommandBuffer commands, VulkanBufferResource& destination,
            ResourceState finalState, bool release);
        void publish(VkCommandBuffer commands, VulkanImageResource& destination,
            ResourceState finalState, bool release);
        void refreshCompleted() noexcept;
        void waitLaneValue(Lane& lane, uint64_t value);
        void reclaim() noexcept;
        void sealBatch(StagingRetireKey key) noexcept;
        StagingSpan stage(std::span<const std::byte> data, VkDeviceSize alignment);

        void flushLegacy();
        void flushTimeline();
        [[nodiscard]] VkCommandBuffer legacyCommands();
        void enqueueBufferUploadLegacy(VulkanBufferResource& destination,
            std::span<const std::byte> data, ResourceState finalState);
        void enqueueImageUploadLegacy(VulkanImageResource& destination,
            std::span<const std::byte> data, ResourceState finalState);

        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphicsQueue_ = VK_NULL_HANDLE;
        uint32_t graphicsQueueFamily_ = 0;
        VulkanResourceAllocator* allocator_ = nullptr;
        UploadQueueMode mode_ = UploadQueueMode::LegacyBlocking;
        bool synchronization2_ = false;

        // LegacyBlocking.
        VkCommandPool commandPool_ = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
        VkFence fence_ = VK_NULL_HANDLE;
        bool batchOpen_ = false;
        std::vector<VulkanBufferResource> stagingBuffers_;

        // Timeline modes.
        std::array<Lane, LaneCount> lanes_{};
        VulkanBufferResource ringBuffer_{};
        StagingRingAllocator ring_;
        std::vector<DedicatedStaging> dedicatedStaging_;
        std::vector<VkBufferImageCopy> imageRegions_;
        uint64_t ringWaits_ = 0;
        uint64_t dedicatedStagingUploads_ = 0;
        uint64_t asyncSubmits_ = 0;
        // Acquires of the releases recorded on the open transfer batch, and of
        // submitted releases no frame or flush has acquired yet.
        std::vector<VkBufferMemoryBarrier> openBufferAcquires_;
        std::vector<VkImageMemoryBarrier> openImageAcquires_;
        std::vector<VkBufferMemoryBarrier> releasedBufferAcquires_;
        std::vector<VkImageMemoryBarrier> releasedImageAcquires_;
        // Submitted lane values no frame submission has waited on yet.
        uint64_t frameWaitTransfer_ = 0;
        uint64_t frameWaitGraphics_ = 0;

        uint64_t pendingBytes_ = 0;
        uint64_t totalSubmittedBytes_ = 0;
        uint64_t totalSubmittedBatches_ = 0;
        uint64_t totalSubmitAndWaitNanoseconds_ = 0;
        CpuProfiler* cpuProfiler_ = nullptr;
    };

} // namespace Iridium
