#pragma once

// Qualification-only digest of the backend's GPU-driven compaction streams
// (M7R R3a.0, --qualification-indirect-stream-digest). It proves that a
// refactor of the indirect cullers records exactly the same work.
//
// Per stream (one view's compaction for one frame slot) it hashes, in order:
//   - the dispatch log: GPU range names, barrier masks, compute/graphics
//     pipeline binds, descriptor-set binds, push-constant bytes, dispatch
//     group counts, helper notes and the indirect draws that consume the
//     result. Vulkan handles become stable indices in first-appearance order;
//     the stream's own command/count buffers are recorded by role.
//   - the host-written candidate, count and query bytes;
//   - after the slot's fence, every count region's device count and its
//     submitted commands, sorted (GPU append order within a (work, bin)
//     region is nondeterministic). A command's firstInstance (its GPU-scene
//     primitive slot) is replaced by the content of the primitive's instance
//     transform, because slot assignment follows entity identities that are
//     regenerated per process. The slot-exact hash is printed too
//     (device_slots) but is not part of the digest.
// Each retired stream prints one IRIDIUM_INDIRECT_STREAM_DIGEST line; finish()
// prints one aggregate line per view and one over all views.

#include "renderer/vulkan/VulkanBackendExtension.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <unordered_map>
#include <vector>

namespace Iridium {

    class VulkanIndirectStreamDigest final : public IVulkanIndirectStreamObserver {
    public:
        static constexpr uint32_t FramesInFlight =
            VulkanFrameScheduler::FramesInFlight;

        struct StreamRecord {
            View view = View::DirectionalShadow;
            uint32_t slot = 0;
            uint64_t sequence = 0;
            uint32_t dispatches = 0;
            uint32_t draws = 0;
            uint64_t logHash = 0;
            uint64_t hostHash = 0;
            uint64_t deviceHash = 0;
            uint64_t deviceSlotHash = 0;
            uint64_t digest = 0;
        };

        // `output` receives the IRIDIUM_INDIRECT_STREAM_DIGEST lines; null
        // keeps the records only (tests).
        explicit VulkanIndirectStreamDigest(std::ostream* output = nullptr) noexcept
            : output_(output) {}

        void setOutput(std::ostream* output) noexcept { output_ = output; }

        void beginStream(View view, uint32_t slot, VkBuffer commandBuffer,
            VkBuffer countBuffer) override;
        void hostWrite(View view, uint32_t slot,
            VulkanIndirectStreamHostTarget target,
            std::span<const std::byte> bytes) override;
        void gpuRange(View view, uint32_t slot, const char* name) override;
        void barrier(View view, uint32_t slot, VkPipelineStageFlags srcStages,
            VkPipelineStageFlags dstStages, VkAccessFlags srcAccess,
            VkAccessFlags dstAccess) override;
        void bindPipeline(View view, uint32_t slot,
            VkPipelineBindPoint bindPoint, VkPipeline pipeline) override;
        void bindDescriptorSets(View view, uint32_t slot,
            VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
            uint32_t firstSet, std::span<const VkDescriptorSet> sets,
            std::span<const uint32_t> dynamicOffsets) override;
        void pushConstants(View view, uint32_t slot, VkPipelineLayout layout,
            VkShaderStageFlags stages, uint32_t offset,
            std::span<const std::byte> bytes) override;
        void dispatch(View view, uint32_t slot, uint32_t groupsX,
            uint32_t groupsY, uint32_t groupsZ) override;
        void note(View view, uint32_t slot, uint32_t tag,
            std::span<const uint64_t> values) override;
        void indirectDraw(View view, uint32_t slot, VkPipeline pipeline,
            VkBuffer vertexBuffer, VkBuffer indexBuffer, VkIndexType indexType,
            uint32_t pushWord, VkBuffer commandBuffer,
            VkDeviceSize commandOffset, VkBuffer countBuffer,
            VkDeviceSize countOffset, uint32_t maxDrawCount) override;
        void retireStream(View view, uint32_t slot,
            const VulkanIndirectStreamReadback& readback) override;

        // Prints the aggregate lines (once).
        void finish();

        [[nodiscard]] const std::vector<StreamRecord>& retired() const noexcept {
            return retired_;
        }
        [[nodiscard]] static const char* viewName(View view) noexcept;

    private:
        struct Hash {
            uint64_t value = 1469598103934665603ull;
            void bytes(const void* data, size_t size) noexcept;
            void u32(uint32_t value) noexcept { bytes(&value, sizeof(value)); }
            void u64(uint64_t value) noexcept { bytes(&value, sizeof(value)); }
        };
        struct OpenStream {
            bool active = false;
            uint64_t sequence = 0;
            VkBuffer commandBuffer = VK_NULL_HANDLE;
            VkBuffer countBuffer = VK_NULL_HANDLE;
            uint32_t dispatches = 0;
            uint32_t draws = 0;
            Hash log;
            Hash host;
        };
        struct ViewTotals {
            Hash digest;
            uint64_t streams = 0;
            uint64_t orphanEvents = 0;
        };
        enum class Handle : uint8_t { Pipeline, Layout, Set, Buffer, Count };
        struct KeyedCommand {
            uint64_t key = 0;
            GpuSceneIndexedIndirectCommand command{};
        };

        // The open stream for (view, slot), or null (counted as an orphan).
        [[nodiscard]] OpenStream* open(View view, uint32_t slot) noexcept;
        [[nodiscard]] uint32_t stableIndex(Handle kind, uint64_t handle);
        template<typename T>
        [[nodiscard]] uint32_t stable(Handle kind, T handle) {
            return stableIndex(kind, reinterpret_cast<uint64_t>(handle));
        }

        std::ostream* output_ = nullptr;
        std::array<std::array<OpenStream, FramesInFlight>,
            kVulkanIndirectStreamViewCount> open_{};
        std::array<uint64_t, kVulkanIndirectStreamViewCount> nextSequence_{};
        std::array<ViewTotals, kVulkanIndirectStreamViewCount> totals_{};
        std::array<std::unordered_map<uint64_t, uint32_t>,
            static_cast<size_t>(Handle::Count)> handles_{};
        std::vector<KeyedCommand> regionScratch_;
        std::vector<StreamRecord> retired_;
        bool finished_ = false;
    };

} // namespace Iridium
