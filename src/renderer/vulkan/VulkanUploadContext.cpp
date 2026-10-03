#include "VulkanUploadContext.h"

#include "VulkanCommandList.h"
#include "VulkanResourceState.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <limits>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {

        // Staging offsets: a multiple of every texel block size and of the
        // 4-byte copy-offset rule of transfer-only queues.
        constexpr VkDeviceSize StagingAlignment = 256;

        void requireInitialized(VkDevice device, VkQueue queue,
            VulkanResourceAllocator* allocator) {
            if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || allocator == nullptr) {
                throw std::logic_error("VulkanUploadContext is not initialized.");
            }
        }

        [[noreturn]] void throwVkError(const char* operation, VkResult result) {
            throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                std::to_string(static_cast<int>(result)) + ".");
        }

        void check(VkResult result, const char* operation) {
            if (result != VK_SUCCESS) throwVkError(operation, result);
        }

        void destroyStagingBuffers(VulkanResourceAllocator& allocator,
            std::vector<VulkanBufferResource>& stagingBuffers) {
            for (VulkanBufferResource& staging : stagingBuffers) {
                allocator.destroy(staging);
            }
            stagingBuffers.clear();
        }

        VkDeviceSize mipByteSize(
            VkFormat format, uint32_t width, uint32_t height) {
            switch (format) {
            case VK_FORMAT_BC4_UNORM_BLOCK:
                return ((static_cast<VkDeviceSize>(width) + 3) / 4) *
                    ((static_cast<VkDeviceSize>(height) + 3) / 4) * 8;
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
                return ((static_cast<VkDeviceSize>(width) + 3) / 4) *
                    ((static_cast<VkDeviceSize>(height) + 3) / 4) * 16;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                return static_cast<VkDeviceSize>(width) * height * 4;
            case VK_FORMAT_R16G16_SFLOAT:
                return static_cast<VkDeviceSize>(width) * height * 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return static_cast<VkDeviceSize>(width) * height * 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                return static_cast<VkDeviceSize>(width) * height * 16;
            default:
                throw std::invalid_argument("Unsupported Vulkan upload image format");
            }
        }

        // Upload ABI: layer-major, with a complete largest-to-smallest mip
        // chain for each layer. Cube faces use Vulkan layer order +X,-X,+Y,
        // -Y,+Z,-Z.
        template <typename Push>
        VkDeviceSize forEachImageRegion(const VulkanImageResource& destination,
            VkDeviceSize sourceOffset, Push&& push) {
            VkDeviceSize byteOffset = 0;
            for (uint32_t layer = 0; layer < destination.arrayLayers; ++layer) {
                uint32_t width = destination.extent.width;
                uint32_t height = destination.extent.height;
                for (uint32_t level = 0; level < destination.mipLevels; ++level) {
                    VkBufferImageCopy region{};
                    region.bufferOffset = sourceOffset + byteOffset;
                    region.imageSubresource.aspectMask = destination.aspect;
                    region.imageSubresource.mipLevel = level;
                    region.imageSubresource.baseArrayLayer = layer;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = { width, height, 1 };
                    push(region);
                    byteOffset += mipByteSize(destination.format, width, height);
                    width = width > 1 ? width / 2 : 1;
                    height = height > 1 ? height / 2 : 1;
                }
            }
            return byteOffset;
        }

    } // namespace

    // ---- initialization -------------------------------------------------------

    void VulkanUploadContext::init(VkDevice device, VkQueue graphicsQueue,
        uint32_t graphicsQueueFamily, VulkanResourceAllocator& allocator,
        CpuProfiler* cpuProfiler) {
        init(device, Queues{ graphicsQueue, graphicsQueueFamily, graphicsQueue,
                graphicsQueueFamily }, allocator, cpuProfiler,
            Options{ .mode = UploadQueueMode::LegacyBlocking });
    }

    void VulkanUploadContext::init(VkDevice device, const Queues& queues,
        VulkanResourceAllocator& allocator, CpuProfiler* cpuProfiler,
        const Options& options) {
        if (device == VK_NULL_HANDLE || queues.graphics == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanUploadContext requires valid Vulkan handles.");
        }
        if (allocator_ != nullptr || device_ != VK_NULL_HANDLE || commandPool_ != VK_NULL_HANDLE ||
            commandBuffer_ != VK_NULL_HANDLE || fence_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanUploadContext was initialized more than once.");
        }

        device_ = device;
        graphicsQueue_ = queues.graphics;
        graphicsQueueFamily_ = queues.graphicsFamily;
        allocator_ = &allocator;
        cpuProfiler_ = cpuProfiler;
        mode_ = options.timelineSemaphore ? options.mode : UploadQueueMode::LegacyBlocking;
        synchronization2_ = options.synchronization2;

        try {
            if (mode_ == UploadQueueMode::LegacyBlocking) {
                VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolInfo.queueFamilyIndex = graphicsQueueFamily_;
                check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_),
                    "vkCreateCommandPool");

                VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocateInfo.commandPool = commandPool_;
                allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocateInfo.commandBufferCount = 1;
                check(vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer_),
                    "vkAllocateCommandBuffers");

                VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence");
                return;
            }

            initLane(lanes_[GraphicsLane], queues.graphics, queues.graphicsFamily);
            // Auto: fresh uploads go to a separate upload family when the
            // device has one; otherwise the same-family fallback (graphics
            // lane, no ownership transfer).
            if (mode_ == UploadQueueMode::Auto && queues.transfer != VK_NULL_HANDLE &&
                queues.transferFamily != queues.graphicsFamily)
                initLane(lanes_[TransferLane], queues.transfer, queues.transferFamily);
            openBufferAcquires_.reserve(64);
            openImageAcquires_.reserve(64);
            releasedBufferAcquires_.reserve(64);
            releasedImageAcquires_.reserve(64);
            const VkDeviceSize ringBytes = (std::max)(options.stagingRingBytes,
                VkDeviceSize{ 1024 * 1024 });
            ringBuffer_ = allocator.createBuffer(ringBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::UploadStaging);
            ring_.reset(ringBytes);
            dedicatedStaging_.reserve(16);
            imageRegions_.reserve(64);
        }
        catch (...) {
            cleanup();
            throw;
        }
    }

    void VulkanUploadContext::initLane(Lane& lane, VkQueue queue, uint32_t family) {
        lane.queue = queue;
        lane.family = family;
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &lane.pool),
            "vkCreateCommandPool(upload lane)");
        VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = lane.pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = LaneCommandBufferCount;
        check(vkAllocateCommandBuffers(device_, &allocateInfo, lane.commands.data()),
            "vkAllocateCommandBuffers(upload lane)");
        VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = 0;
        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        semaphoreInfo.pNext = &typeInfo;
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &lane.timeline),
            "vkCreateSemaphore(upload timeline)");
    }

    void VulkanUploadContext::destroyLane(Lane& lane) noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (lane.timeline != VK_NULL_HANDLE)
                vkDestroySemaphore(device_, lane.timeline, nullptr);
            if (lane.pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device_, lane.pool, nullptr);
        }
        lane = Lane{};
    }

    void VulkanUploadContext::cleanup() {
        if (device_ == VK_NULL_HANDLE) {
            stagingBuffers_.clear();
            dedicatedStaging_.clear();
            batchOpen_ = false;
            pendingBytes_ = 0;
            cpuProfiler_ = nullptr;
            return;
        }

        if (hasPendingWork()) {
            try {
                flush();
            } catch (...) {
                // Flush has already made best-effort cleanup of staging and command state.
                // Continue releasing the context handles so cleanup remains terminal and safe.
            }
        }

        if (mode_ != UploadQueueMode::LegacyBlocking) {
            // Every submitted batch must be complete before its staging and
            // command buffers go.
            for (Lane& lane : lanes_) {
                if (!lane.active() || lane.lastSignaled == 0) continue;
                try {
                    waitLaneValue(lane, lane.lastSignaled);
                } catch (...) {
                    (void)vkDeviceWaitIdle(device_);
                }
            }
        }
        if (allocator_ != nullptr) {
            for (DedicatedStaging& staging : dedicatedStaging_)
                allocator_->destroy(staging.buffer);
            allocator_->destroy(ringBuffer_);
        }
        dedicatedStaging_.clear();
        ringBuffer_ = {};
        ring_.reset(0);
        for (Lane& lane : lanes_) destroyLane(lane);
        openBufferAcquires_.clear();
        openImageAcquires_.clear();
        releasedBufferAcquires_.clear();
        releasedImageAcquires_.clear();
        frameWaitTransfer_ = 0;
        frameWaitGraphics_ = 0;

        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
        }
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        }

        device_ = VK_NULL_HANDLE;
        graphicsQueue_ = VK_NULL_HANDLE;
        graphicsQueueFamily_ = 0;
        allocator_ = nullptr;
        cpuProfiler_ = nullptr;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        fence_ = VK_NULL_HANDLE;
        batchOpen_ = false;
        pendingBytes_ = 0;
        stagingBuffers_.clear();
        mode_ = UploadQueueMode::LegacyBlocking;
    }

    bool VulkanUploadContext::hasPendingWork() const noexcept {
        if (mode_ == UploadQueueMode::LegacyBlocking)
            return batchOpen_ || !stagingBuffers_.empty();
        for (const Lane& lane : lanes_)
            if (lane.recording()) return true;
        return !releasedBufferAcquires_.empty() || !releasedImageAcquires_.empty();
    }

    void VulkanUploadContext::flush() {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (mode_ == UploadQueueMode::LegacyBlocking) flushLegacy();
        else flushTimeline();
    }

    // ---- upload lanes -----------------------------------------------------------

    VkCommandBuffer VulkanUploadContext::beginLane(Lane& lane) {
        if (lane.recording()) return lane.commands[lane.open];
        const uint32_t index = lane.nextCommand;
        if (lane.commandValues[index] > lane.completed) {
            refreshCompleted();
            if (lane.commandValues[index] > lane.completed) {
                CpuScope waitScope(cpuProfiler_, "cpu.renderer.upload_wait");
                waitLaneValue(lane, lane.commandValues[index]);
            }
        }
        VkCommandBuffer commands = lane.commands[index];
        check(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer(upload lane)");
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer(upload lane)");
        lane.open = index;
        lane.nextCommand = (index + 1) % LaneCommandBufferCount;
        return commands;
    }

    uint64_t VulkanUploadContext::submitLane(Lane& lane, VkSemaphore wait,
        uint64_t waitValue) {
        if (!lane.recording()) return 0;
        VkCommandBuffer commands = lane.commands[lane.open];
        const uint32_t index = lane.open;
        lane.open = UINT32_MAX;
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer(upload lane)");
        const uint64_t value = lane.lastSignaled + 1;
        if (synchronization2_) {
            VkCommandBufferSubmitInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            commandInfo.commandBuffer = commands;
            VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            signal.semaphore = lane.timeline;
            signal.value = value;
            signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            waitInfo.semaphore = wait;
            waitInfo.value = waitValue;
            waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            if (wait != VK_NULL_HANDLE) {
                submit.waitSemaphoreInfoCount = 1;
                submit.pWaitSemaphoreInfos = &waitInfo;
            }
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &commandInfo;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signal;
            check(vkQueueSubmit2(lane.queue, 1, &submit, VK_NULL_HANDLE),
                "vkQueueSubmit2(upload lane)");
        }
        else {
            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkTimelineSemaphoreSubmitInfo timelineInfo{
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            timelineInfo.signalSemaphoreValueCount = 1;
            timelineInfo.pSignalSemaphoreValues = &value;
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.pNext = &timelineInfo;
            if (wait != VK_NULL_HANDLE) {
                timelineInfo.waitSemaphoreValueCount = 1;
                timelineInfo.pWaitSemaphoreValues = &waitValue;
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &wait;
                submit.pWaitDstStageMask = &waitStage;
            }
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commands;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &lane.timeline;
            check(vkQueueSubmit(lane.queue, 1, &submit, VK_NULL_HANDLE),
                "vkQueueSubmit(upload lane)");
        }
        lane.lastSignaled = value;
        lane.commandValues[index] = value;
        return value;
    }

    void VulkanUploadContext::refreshCompleted() noexcept {
        for (Lane& lane : lanes_) {
            if (!lane.active() || lane.completed >= lane.lastSignaled) continue;
            uint64_t value = 0;
            if (vkGetSemaphoreCounterValue(device_, lane.timeline, &value) == VK_SUCCESS)
                lane.completed = (std::max)(lane.completed, value);
        }
    }

    void VulkanUploadContext::waitLaneValue(Lane& lane, uint64_t value) {
        if (value == 0 || lane.completed >= value) return;
        VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &lane.timeline;
        waitInfo.pValues = &value;
        check(vkWaitSemaphores(device_, &waitInfo, UINT64_MAX), "vkWaitSemaphores(upload)");
        lane.completed = (std::max)(lane.completed, value);
    }

    void VulkanUploadContext::reclaim() noexcept {
        refreshCompleted();
        const uint64_t transferCompleted = lanes_[TransferLane].completed;
        const uint64_t graphicsCompleted = lanes_[GraphicsLane].completed;
        (void)ring_.retire(transferCompleted, graphicsCompleted);
        const auto done = [&](const DedicatedStaging& staging) {
            return staging.sealed &&
                staging.key.completedBy(transferCompleted, graphicsCompleted);
        };
        for (DedicatedStaging& staging : dedicatedStaging_)
            if (done(staging)) allocator_->destroy(staging.buffer);
        std::erase_if(dedicatedStaging_, done);
    }

    void VulkanUploadContext::sealBatch(StagingRetireKey key) noexcept {
        ring_.close(key);
        for (DedicatedStaging& staging : dedicatedStaging_) {
            if (staging.sealed) continue;
            staging.key = key;
            staging.sealed = true;
        }
    }

    VulkanUploadContext::StagingSpan VulkanUploadContext::stage(
        std::span<const std::byte> data, VkDeviceSize alignment) {
        const VkDeviceSize size = static_cast<VkDeviceSize>(data.size_bytes());
        if (size <= ring_.capacity() / 4) {
            std::optional<uint64_t> offset = ring_.allocate(size, alignment);
            if (!offset) {
                reclaim();
                offset = ring_.allocate(size, alignment);
            }
            while (!offset) {
                // Full: wait for the oldest submitted batch (counted).
                const std::optional<StagingRetireKey> oldest = ring_.oldestSealedKey();
                if (!oldest) break;
                {
                    CpuScope waitScope(cpuProfiler_, "cpu.renderer.upload_wait");
                    ++ringWaits_;
                    waitLaneValue(lanes_[TransferLane], oldest->transferValue);
                    waitLaneValue(lanes_[GraphicsLane], oldest->graphicsValue);
                }
                reclaim();
                offset = ring_.allocate(size, alignment);
            }
            if (offset) {
                if (size != 0)
                    std::memcpy(static_cast<std::byte*>(ringBuffer_.mapped) + *offset,
                        data.data(), static_cast<size_t>(size));
                return { ringBuffer_.buffer, *offset };
            }
            // The open batch itself fills the ring: dedicated staging.
        }
        DedicatedStaging staging{};
        staging.buffer = allocator_->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, ProfileMemoryCategory::UploadStaging);
        try {
            allocator_->write(staging.buffer, 0, data);
            dedicatedStaging_.push_back(staging);
        }
        catch (...) {
            allocator_->destroy(staging.buffer);
            throw;
        }
        ++dedicatedStagingUploads_;
        return { staging.buffer.buffer, 0 };
    }

    void VulkanUploadContext::submitOpenBatches() {
        Lane& transfer = lanes_[TransferLane];
        Lane& graphics = lanes_[GraphicsLane];
        const uint64_t transferValue = transfer.active() ? submitLane(transfer) : 0;
        if (transferValue != 0) {
            // Their releases are submitted: a frame (or a flush) may acquire.
            releasedBufferAcquires_.insert(releasedBufferAcquires_.end(),
                openBufferAcquires_.begin(), openBufferAcquires_.end());
            releasedImageAcquires_.insert(releasedImageAcquires_.end(),
                openImageAcquires_.begin(), openImageAcquires_.end());
            openBufferAcquires_.clear();
            openImageAcquires_.clear();
            frameWaitTransfer_ = transferValue;
        }
        const uint64_t graphicsValue = submitLane(graphics);
        if (graphicsValue != 0) frameWaitGraphics_ = graphicsValue;
        if (transferValue != 0 || graphicsValue != 0)
            sealBatch({ transferValue, graphicsValue });
    }

    void VulkanUploadContext::recordAcquires(VkCommandBuffer commands) {
        if (releasedBufferAcquires_.empty() && releasedImageAcquires_.empty()) return;
        // The frame (or flush) submission waits on the transfer timeline at
        // ALL_COMMANDS, which orders the releases before these acquires.
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
            static_cast<uint32_t>(releasedBufferAcquires_.size()),
            releasedBufferAcquires_.data(),
            static_cast<uint32_t>(releasedImageAcquires_.size()),
            releasedImageAcquires_.data());
        releasedBufferAcquires_.clear();
        releasedImageAcquires_.clear();
    }

    void VulkanUploadContext::submitAsync() {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (mode_ == UploadQueueMode::LegacyBlocking) {
            flushLegacy();
            return;
        }
        if (ring_.usedBytes() != 0 || !dedicatedStaging_.empty()) reclaim();
        bool recording = false;
        for (const Lane& lane : lanes_) recording = recording || lane.recording();
        if (!recording) return;
        CpuScope submitScope(cpuProfiler_, "cpu.renderer.upload_submit");
        const auto submitStart = std::chrono::steady_clock::now();
        const uint64_t submittedBytes = pendingBytes_;
        submitOpenBatches();
        pendingBytes_ = 0;
        ++asyncSubmits_;
        ++totalSubmittedBatches_;
        totalSubmittedBytes_ += submittedBytes;
        totalSubmitAndWaitNanoseconds_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - submitStart).count());
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter("upload.bytes", submittedBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_->recordCounter("upload.batches", 1);
        }
    }

    VulkanUploadContext::FrameWaits VulkanUploadContext::recordFrameAcquires(
        VkCommandBuffer frameCommands) {
        FrameWaits waits{};
        if (mode_ == UploadQueueMode::LegacyBlocking) return waits;
        recordAcquires(frameCommands);
        if (frameWaitTransfer_ != 0) {
            waits.transferTimeline = lanes_[TransferLane].timeline;
            waits.transferValue = frameWaitTransfer_;
        }
        if (frameWaitGraphics_ != 0) {
            waits.graphicsTimeline = lanes_[GraphicsLane].timeline;
            waits.graphicsValue = frameWaitGraphics_;
        }
        frameWaitTransfer_ = 0;
        frameWaitGraphics_ = 0;
        return waits;
    }

    uint64_t VulkanUploadContext::retireFloor(uint64_t lastSubmittedSerial,
        bool frameRecording) const noexcept {
        if (mode_ == UploadQueueMode::LegacyBlocking || device_ == VK_NULL_HANDLE) return 0;
        bool outstanding = frameWaitTransfer_ != 0 || frameWaitGraphics_ != 0 ||
            !releasedBufferAcquires_.empty() || !releasedImageAcquires_.empty();
        for (const Lane& lane : lanes_) outstanding = outstanding || lane.recording();
        // Open work is submitted at the next beginFrame and waited by the
        // frame recorded then: the current one is past its acquire point.
        return outstanding ? lastSubmittedSerial + (frameRecording ? 2u : 1u) : 0u;
    }

    void VulkanUploadContext::flushTimeline() {
        refreshCompleted();
        bool outstanding = hasPendingWork();
        bool recording = false;
        for (const Lane& lane : lanes_) {
            recording = recording || lane.recording();
            outstanding = outstanding || (lane.active() && lane.completed < lane.lastSignaled);
        }
        if (!outstanding) return;
        CpuScope uploadScope(cpuProfiler_, "cpu.renderer.upload_wait");
        const auto flushStart = std::chrono::steady_clock::now();
        const uint64_t submittedBytes = pendingBytes_;
        submitOpenBatches();
        pendingBytes_ = 0;
        Lane& transfer = lanes_[TransferLane];
        Lane& graphics = lanes_[GraphicsLane];
        if (!releasedBufferAcquires_.empty() || !releasedImageAcquires_.empty()) {
            // Acquire in a graphics batch of its own, after the releases.
            recordAcquires(beginLane(graphics));
            (void)submitLane(graphics, transfer.timeline, transfer.lastSignaled);
        }
        for (Lane& lane : lanes_)
            if (lane.active()) waitLaneValue(lane, lane.lastSignaled);
        frameWaitTransfer_ = 0;
        frameWaitGraphics_ = 0;
        reclaim();

        if (!recording) return;
        ++totalSubmittedBatches_;
        totalSubmittedBytes_ += submittedBytes;
        totalSubmitAndWaitNanoseconds_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - flushStart).count());
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter("upload.bytes", submittedBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_->recordCounter("upload.batches", 1);
        }
    }

    void VulkanUploadContext::publish(VkCommandBuffer commands,
        VulkanBufferResource& destination, ResourceState finalState, bool release) {
        // After the copy: every later access on the graphics queue (no CPU
        // wait separates them any more) sees the data.
        VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = destination.buffer;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        if (release) {
            barrier.srcQueueFamilyIndex = lanes_[TransferLane].family;
            barrier.dstQueueFamilyIndex = lanes_[GraphicsLane].family;
            VkBufferMemoryBarrier acquire = barrier;
            acquire.srcAccessMask = 0;
            barrier.dstAccessMask = 0;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1, &barrier,
                0, nullptr);
            openBufferAcquires_.push_back(acquire);
        }
        else {
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &barrier,
                0, nullptr);
        }
        destination.state = finalState;
    }

    void VulkanUploadContext::publish(VkCommandBuffer commands,
        VulkanImageResource& destination, ResourceState finalState, bool release) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = getVulkanStateInfo(finalState, destination.aspect).layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = destination.image;
        barrier.subresourceRange = { destination.aspect, 0, destination.mipLevels, 0,
            destination.arrayLayers };
        if (release) {
            // The release carries the final layout; the acquire repeats it.
            barrier.srcQueueFamilyIndex = lanes_[TransferLane].family;
            barrier.dstQueueFamilyIndex = lanes_[GraphicsLane].family;
            VkImageMemoryBarrier acquire = barrier;
            acquire.srcAccessMask = 0;
            barrier.dstAccessMask = 0;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                1, &barrier);
            openImageAcquires_.push_back(acquire);
        }
        else {
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                1, &barrier);
        }
        destination.state = finalState;
    }

    // ---- enqueue ----------------------------------------------------------------

    void VulkanUploadContext::enqueueBufferUpload(VulkanBufferResource& destination,
        std::span<const std::byte> data, ResourceState finalState) {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (!destination.isValid()) {
            throw std::invalid_argument("Buffer upload destination is invalid.");
        }
        if (data.size_bytes() > destination.size) {
            throw std::out_of_range("Buffer upload data exceeds destination size.");
        }
        if (mode_ == UploadQueueMode::LegacyBlocking) {
            enqueueBufferUploadLegacy(destination, data, finalState);
            return;
        }

        const StagingSpan source = stage(data, StagingAlignment);
        // Fresh destinations have no graphics-queue history: the transfer
        // lane may write them and hand them over.
        const bool release = destination.state == ResourceState::Undefined &&
            lanes_[TransferLane].active();
        Lane& lane = lanes_[release ? TransferLane : GraphicsLane];
        VulkanCommandList commands(beginLane(lane));
        commands.transition(destination, ResourceState::CopyDestination);
        VkBufferCopy region{};
        region.srcOffset = source.offset;
        region.size = data.size_bytes();
        if (region.size != 0)
            vkCmdCopyBuffer(commands.native(), source.buffer, destination.buffer, 1, &region);
        publish(commands.native(), destination, finalState, release);
        pendingBytes_ += static_cast<uint64_t>(data.size_bytes());
    }

    void VulkanUploadContext::enqueueImageUpload(VulkanImageResource& destination,
        std::span<const std::byte> data, ResourceState finalState) {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (data.empty()) {
            throw std::invalid_argument("Image upload data must not be empty.");
        }
        if (!destination.isValid()) {
            throw std::invalid_argument("Image upload destination is invalid.");
        }
        if (mode_ == UploadQueueMode::LegacyBlocking) {
            enqueueImageUploadLegacy(destination, data, finalState);
            return;
        }

        imageRegions_.clear();
        const VkDeviceSize byteCount = forEachImageRegion(destination, 0,
            [this](const VkBufferImageCopy& region) { imageRegions_.push_back(region); });
        if (byteCount != data.size_bytes())
            throw std::invalid_argument("Image upload byte count does not match mip extents");

        const StagingSpan source = stage(data, StagingAlignment);
        for (VkBufferImageCopy& region : imageRegions_) region.bufferOffset += source.offset;
        const bool release = destination.state == ResourceState::Undefined &&
            lanes_[TransferLane].active();
        Lane& lane = lanes_[release ? TransferLane : GraphicsLane];
        VulkanCommandList commands(beginLane(lane));
        commands.transition(destination, ResourceState::CopyDestination);
        vkCmdCopyBufferToImage(commands.native(), source.buffer, destination.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(imageRegions_.size()), imageRegions_.data());
        publish(commands.native(), destination, finalState, release);
        pendingBytes_ += static_cast<uint64_t>(data.size_bytes());
    }

    void VulkanUploadContext::enqueueTransition(VulkanImageResource& destination, ResourceState state) {
        requireInitialized(device_, graphicsQueue_, allocator_);
        if (!destination.isValid()) {
            throw std::invalid_argument("Image transition destination is invalid.");
        }
        if (mode_ == UploadQueueMode::LegacyBlocking) {
            VulkanCommandList(legacyCommands()).transition(destination, state);
            return;
        }
        VulkanCommandList(beginLane(lanes_[GraphicsLane])).transition(destination, state);
    }

    // ---- LegacyBlocking (the pre-R4d path) ----------------------------------------

    void VulkanUploadContext::flushLegacy() {
        if (!hasPendingWork()) {
            return;
        }

        CpuScope uploadScope(cpuProfiler_, "cpu.renderer.upload_wait");
        const auto flushStart = std::chrono::steady_clock::now();
        const uint64_t submittedBytes = pendingBytes_;

        auto discardPending = [this]() noexcept {
            if (allocator_ != nullptr) {
                destroyStagingBuffers(*allocator_, stagingBuffers_);
            } else {
                stagingBuffers_.clear();
            }
            batchOpen_ = false;
            pendingBytes_ = 0;
        };

        auto resetCommandPoolAfterFailure = [this]() noexcept -> VkResult {
            if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE) {
                return vkResetCommandPool(device_, commandPool_, 0);
            }
            return VK_SUCCESS;
        };

        if (batchOpen_) {
            VkResult result = vkEndCommandBuffer(commandBuffer_);
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkEndCommandBuffer", result);
            }

            result = vkResetFences(device_, 1, &fence_);
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkResetFences", result);
            }

            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer_;
            result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence_);
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkQueueSubmit", result);
            }

            result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, std::numeric_limits<uint64_t>::max());
            if (result != VK_SUCCESS) {
                discardPending();
                const VkResult resetResult = resetCommandPoolAfterFailure();
                if (resetResult != VK_SUCCESS) {
                    throwVkError("vkResetCommandPool", resetResult);
                }
                throwVkError("vkWaitForFences", result);
            }
        }

        destroyStagingBuffers(*allocator_, stagingBuffers_);
        ++totalSubmittedBatches_;
        totalSubmittedBytes_ += submittedBytes;
        totalSubmitAndWaitNanoseconds_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - flushStart).count());
        if (cpuProfiler_ != nullptr) {
            cpuProfiler_->recordCounter("upload.bytes", submittedBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_->recordCounter("upload.batches", 1);
        }
        VkResult result = vkResetCommandPool(device_, commandPool_, 0);
        batchOpen_ = false;
        pendingBytes_ = 0;
        if (result != VK_SUCCESS) {
            throwVkError("vkResetCommandPool", result);
        }
    }

    VkCommandBuffer VulkanUploadContext::legacyCommands() {
        if (!batchOpen_) {
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            const VkResult result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
            if (result != VK_SUCCESS) {
                throwVkError("vkBeginCommandBuffer", result);
            }
            batchOpen_ = true;
        }
        return commandBuffer_;
    }

    void VulkanUploadContext::enqueueBufferUploadLegacy(VulkanBufferResource& destination,
        std::span<const std::byte> data, ResourceState finalState) {
        VkCommandBuffer commandBuffer = legacyCommands();
        VulkanBufferResource staging{};
        try {
            staging = allocator_->createBuffer(data.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::UploadStaging);
            allocator_->write(staging, 0, data);

            VulkanCommandList commands(commandBuffer);
            commands.transition(staging, ResourceState::CopySource);
            commands.transition(destination, ResourceState::CopyDestination);
            commands.copyBuffer(staging, destination, data.size_bytes());
            commands.transition(destination, finalState);
            stagingBuffers_.push_back(staging);
            pendingBytes_ += static_cast<uint64_t>(data.size_bytes());
            staging = {};
        } catch (...) {
            allocator_->destroy(staging);
            throw;
        }
    }

    void VulkanUploadContext::enqueueImageUploadLegacy(VulkanImageResource& destination,
        std::span<const std::byte> data, ResourceState finalState) {
        VkCommandBuffer commandBuffer = legacyCommands();
        VulkanBufferResource staging{};
        try {
            staging = allocator_->createBuffer(data.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::UploadStaging);
            allocator_->write(staging, 0, data);

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(static_cast<size_t>(destination.mipLevels) *
                destination.arrayLayers);
            const VkDeviceSize byteOffset = forEachImageRegion(destination, 0,
                [&regions](const VkBufferImageCopy& region) { regions.push_back(region); });
            if (byteOffset != data.size_bytes())
                throw std::invalid_argument("Image upload byte count does not match mip extents");

            VulkanCommandList commands(commandBuffer);
            commands.transition(staging, ResourceState::CopySource);
            commands.transition(destination, ResourceState::CopyDestination);
            vkCmdCopyBufferToImage(commandBuffer, staging.buffer, destination.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<uint32_t>(regions.size()), regions.data());
            commands.transition(destination, finalState);
            stagingBuffers_.push_back(staging);
            pendingBytes_ += static_cast<uint64_t>(data.size_bytes());
            staging = {};
        } catch (...) {
            allocator_->destroy(staging);
            throw;
        }
    }

} // namespace Iridium
