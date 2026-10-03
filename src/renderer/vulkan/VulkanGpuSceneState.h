#pragma once

// M7R R3c.0: the per-slot GPU-scene tables (transforms, instances,
// primitives, geometries), their descriptor sets, the CPU mirrors the
// cullers and oracles read, the published counts and the upload bookkeeping,
// moved out of VulkanVertexBackend unchanged.

#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneUploadPlanner.h"
#include "renderer/rhi/Mesh.h"

#include "VulkanFrameScheduler.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Iridium {

    class CpuProfiler;

    class VulkanGpuSceneState final {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;
        using FrameBuffers = std::array<VulkanBufferResource, FrameCount>;

        // Upload heaps can be uncached/write-combined. CPU validation must use
        // an owned mirror updated by the exact same per-context dirty ranges.
        struct CpuMirror {
            std::vector<GpuSceneAffineTransform> transforms;
            std::vector<GpuSceneInstanceRecord> instances;
            std::vector<GpuScenePrimitiveRecord> primitives;
            std::vector<GpuSceneGeometryRecord> geometries;
            std::vector<GpuScenePrimitiveIdentity> primitiveIdentities;
        };

        VulkanGpuSceneState() = default;
        VulkanGpuSceneState(const VulkanGpuSceneState&) = delete;
        VulkanGpuSceneState& operator=(const VulkanGpuSceneState&) = delete;

        // Derives the per-table maximum capacity from the device storage range.
        void init(VkDevice device, VulkanResourceAllocator& allocator,
            VulkanFrameScheduler& scheduler, CpuProfiler* profiler,
            const bool& frameOpen, uint64_t maxStorageBufferRange);
        // Frame boundary only. M7R R4c.2: the replacement tables are created
        // at once; a slot that is not in flight swaps now, an in-flight slot
        // keeps its tables until swapRetiredSlot (no drain).
        void createBuffers(const GpuSceneCapacityRequirements& capacity);
        void setDescriptorSet(uint32_t frame, VkDescriptorSet set) {
            descriptorSets_.at(frame) = set;
        }
        void bindBuffers();
        // At `slot`'s retirement: installs its parked tables and rewrites its
        // set. True when the slot changed.
        bool swapRetiredSlot(uint32_t slot);
        [[nodiscard]] bool slotSwapPending(uint32_t slot) const noexcept {
            return pendingSlots_[slot];
        }
        // Grows every table geometrically to cover `requirements` (frame
        // boundary only).
        void prepare(const GpuSceneCapacityRequirements& requirements);
        // Throws when `scene` is outside the prepared capacity or ABI.
        void validatePublication(const GpuScenePackedTables& scene) const;
        // Uploads the dirty ranges of `scene` into slot `frame` and its mirror.
        void publish(const GpuScenePackedTables& scene, uint32_t frame);

        // The slot's CPU GPU-scene mirror with the published counts.
        [[nodiscard]] VulkanIndirectScene indirectScene(uint32_t frame) const noexcept;
        [[nodiscard]] const CpuMirror& mirror(uint32_t frame) const noexcept {
            return cpuMirrors_[frame];
        }
        [[nodiscard]] bool buffersMapped(uint32_t frame) const noexcept {
            return primitiveBuffers_[frame].mapped != nullptr &&
                geometryBuffers_[frame].mapped != nullptr &&
                instanceBuffers_[frame].mapped != nullptr &&
                transformBuffers_[frame].mapped != nullptr;
        }
        [[nodiscard]] const std::array<VkDescriptorSet, FrameCount>&
            descriptorSets() const noexcept { return descriptorSets_; }
        // The camera of each slot, as updateCamera published it.
        [[nodiscard]] std::array<ViewTransportRecord, FrameCount>& views() noexcept {
            return cpuViews_;
        }
        [[nodiscard]] const std::array<ViewTransportRecord, FrameCount>&
            views() const noexcept { return cpuViews_; }
        [[nodiscard]] const GpuSceneCapacityRequirements& capacity() const noexcept {
            return capacity_;
        }
        [[nodiscard]] const GpuSceneUploadTelemetry& uploadTelemetry() const noexcept {
            return uploadTelemetry_;
        }
        [[nodiscard]] uint64_t publishedEpoch() const noexcept {
            return publishedEpoch_;
        }

        void destroy() noexcept;
        void reset() noexcept;

    private:
        void bindSlot(uint32_t frame);
        void destroySlot(std::array<FrameBuffers*, 4> tables, uint32_t frame) noexcept;

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        VulkanFrameScheduler* scheduler_ = nullptr;
        CpuProfiler* profiler_ = nullptr;
        const bool* frameOpen_ = nullptr;

        FrameBuffers transformBuffers_{};
        FrameBuffers instanceBuffers_{};
        FrameBuffers primitiveBuffers_{};
        FrameBuffers geometryBuffers_{};
        // R4c.2: replacements parked for in-flight slots.
        FrameBuffers pendingTransformBuffers_{};
        FrameBuffers pendingInstanceBuffers_{};
        FrameBuffers pendingPrimitiveBuffers_{};
        FrameBuffers pendingGeometryBuffers_{};
        std::array<bool, FrameCount> pendingSlots_{};
        std::array<VkDescriptorSet, FrameCount> descriptorSets_{};
        std::array<CpuMirror, FrameCount> cpuMirrors_;
        std::array<ViewTransportRecord, FrameCount> cpuViews_;
        std::array<std::vector<uint64_t>, FrameCount> uploadedTransformRevisions_{};
        std::array<std::vector<uint64_t>, FrameCount> uploadedInstanceRevisions_{};
        std::array<std::vector<uint64_t>, FrameCount> uploadedPrimitiveRevisions_{};
        std::array<std::vector<uint64_t>, FrameCount> uploadedGeometryRevisions_{};
        std::vector<GpuSceneRecordRange> uploadRanges_;
        GpuSceneUploadTelemetry uploadTelemetry_{};
        GpuSceneCapacityRequirements capacity_{};
        GpuSceneCapacityRequirements maximumCapacity_{};
        GpuSceneCapacityRequirements publishedCounts_{};
        uint64_t publishedEpoch_ = 1;
    };

} // namespace Iridium
