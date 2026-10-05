#pragma once

// VulkanOpaqueIndirectCuller (M7R R3a.6): the main-view opaque GPU-driven
// compaction. A sibling of VulkanIndirectViewCuller built on the same shared
// core (VulkanIndirectCullerShared: command/resource seams, indirect buffer
// set, resident-LOD prefix, compute pipeline helpers, stream digest tap), not a
// subclass. Its extension over the view core:
//   - binning follows the runs of the submission's sorted GPU-scene
//     primitives (GpuSceneIndirectPlan) rather than the shared (buffers,
//     alpha, sidedness) bins;
//   - a 6-binding set (candidates, commands, counts, LOD history, fused
//     occlusion results, depth-pyramid history sampler);
//   - the frustum cull pipeline and its fused-occlusion variant/fallback;
//   - 11 push words (LOD error, history capacity/serial, hysteresis,
//     occlusion flags);
//   - CPU-projected and GPU-scene depth-occlusion queries and their results;
//   - the shared main-view LOD history (one ordered table, not per frame);
//   - the visibility/LOD/occlusion validation slots.

#include "VulkanIndirectCullerShared.h"
#include "renderer/rhi/DepthPyramid.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/GpuSceneLod.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderFrame.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanDepthPyramid;

    struct VulkanOpaqueCullerConfig {
        bool depthOcclusionQuery = false;
        bool depthOcclusionRejection = false;
        float lodErrorPixels = 0.0f;       // > 0 enables GPU LOD
        uint32_t lodMaximumLevel = 15u;
        float lodHysteresisFraction = 0.15f;
        bool forceDirectGBufferReference = false;
        // Qualification oracles enabled for these views (null otherwise).
        IVulkanIndirectOracle* lodOracle = nullptr;
        IVulkanIndirectOracle* occlusionOracle = nullptr;
        // Null unless the depth pyramid is enabled.
        VulkanDepthPyramid* depthPyramid = nullptr;
    };

    struct VulkanOpaqueCullPipelines {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline cull = VK_NULL_HANDLE;
        VkPipeline fallback = VK_NULL_HANDLE; // fused-occlusion runs only
        void destroy(VkDevice device) noexcept;
    };

    // The cull set layout has 4 bindings, or 6 with fused occlusion rejection.
    [[nodiscard]] VulkanOpaqueCullPipelines createOpaqueCullPipelines(
        VkDevice device, VkPipelineCache pipelineCache, bool depthOcclusionRejection,
        VkDescriptorSetLayout globalLayout, VkDescriptorSetLayout gpuSceneLayout);

    struct OpaqueIndirectInputs {
        // M7R R5c.4b: the main-opaque submission (GPU-scene primitives in
        // draw order, read from their records; direct packets).
        const OpaqueSubmission* submission = nullptr;
        VulkanIndirectScene scene{};
        // Every GPU-scene table of the slot is mapped.
        bool sceneBuffersMapped = false;
        VulkanIndirectAssetResolver assets{};
        // Depth-occlusion queries run (enabled and the history is eligible).
        bool queryOcclusion = false;
        const ViewTransportRecord* view = nullptr;
    };

    struct OpaqueCompactionContext {
        VkDescriptorSet globalSet = VK_NULL_HANDLE;
        VkDescriptorSet gpuSceneSet = VK_NULL_HANDLE;
        uint32_t retainedView = 0;
    };

    class VulkanOpaqueIndirectCuller {
    public:
        struct Bin {
            // Index of the bin's first command in gpuOrder().
            uint32_t packetBegin = 0;
            // Position of the bin's first entry in the submission's order
            // (M7R R5c.4c: direct packets between bins are drawn there).
            uint32_t orderBegin = 0;
            uint32_t commandBegin = 0;
            uint32_t commandCount = 0;
            PipelineHandle pipeline;
            MaterialHandle material;
            GeometryHandle geometry;
        };

        void init(const VulkanCullerServices& services,
            const VulkanOpaqueCullerConfig& config,
            const VulkanOpaqueCullPipelines& pipelines);
        // The per-frame cull sets are allocated by the backend's descriptor
        // loop (allocation order is kept), then bound once.
        void allocateSet(uint32_t frame);
        void bindBuffers();
        // Destroys buffers and pipelines; the sets return with the pool.
        void destroy(VkDevice device) noexcept;
        // Frame boundary only. M7R R4c.2: new per-slot buffers and the
        // shared LOD history are created at once; a slot that is not in
        // flight is collected and swapped now, an in-flight slot keeps its
        // buffers until swapRetiredSlot. The replaced history buffer goes to
        // the deletion queue while any slot is in flight.
        void resize(uint32_t capacity, bool frameOpen);
        // At `slot`'s retirement (after collect): installs its parked
        // buffers and rewrites its set (including the current history).
        // True when the slot's command/count buffers changed.
        bool swapRetiredSlot(uint32_t slot);
        [[nodiscard]] bool slotSwapPending(uint32_t slot) const noexcept {
            return pendingSlots_[slot];
        }

        // CPU visibility/LOD/occlusion planning, oracle expectations and the
        // host writes. False selects the direct fallback.
        [[nodiscard]] bool plan(const OpaqueIndirectInputs& inputs, uint32_t frame);
        // M7R R5c.4f: the editor wireframe's bins. Every command is drawn: the
        // plan's commands go to the slot's command buffer from the host, with
        // no compaction, culling, LOD or validation. False selects the direct
        // wireframe loop.
        [[nodiscard]] bool planWireframe(const OpaqueIndirectInputs& inputs,
            uint32_t frame);
        // After a successful plan(): barriers, the cull dispatch and the
        // occlusion queries. Returns the dispatches recorded.
        uint32_t recordCompaction(VkCommandBuffer cmd, uint32_t frame,
            const OpaqueCompactionContext& context);
        void collect(uint32_t frame);

        [[nodiscard]] std::span<const Bin> bins() const noexcept { return bins_; }
        // The submission's GPU-scene primitives in draw order (the plan's
        // command order), as of the last plan().
        [[nodiscard]] std::span<const uint32_t> gpuOrder() const noexcept {
            return gpuOrder_;
        }
        [[nodiscard]] const GpuSceneIndirectPlan& indirectPlan() const noexcept {
            return indirectPlan_;
        }
        [[nodiscard]] GpuSceneLodHistory& lodHistory() noexcept { return lodHistory_; }
        [[nodiscard]] VkDeviceSize lodHistoryBufferBytes() const noexcept {
            return lodHistoryBuffer_.size;
        }
        [[nodiscard]] uint32_t commandCapacity() const noexcept {
            return commandCapacity_;
        }
        [[nodiscard]] const VulkanIndirectBufferSet& buffers() const noexcept {
            return buffers_;
        }

    private:
        struct PendingValidation {
            uint64_t profileFrameId = 0;
            std::vector<uint32_t> expectedBinCounts;
            std::vector<uint32_t> binCapacities;
            uint64_t occlusionProfileFrameId = 0;
            uint32_t gpuSceneOcclusionCandidateCount = 0;
            std::vector<uint32_t> occlusionCandidatePrimitiveIndices;
            std::vector<uint32_t> occlusionCandidateBinIndices;
            bool lodQualificationOracle = false;
            bool occlusionQualificationOracle = false;
            bool gpuSceneOcclusionPending = false;
            bool occlusionRejectionApplied = false;
            bool pending = false;
        };
        using Buffers =
            std::array<VulkanBufferResource, kIndirectCullerFramesInFlight>;

        [[nodiscard]] VulkanIndirectStreamTap stream(uint32_t frame) const noexcept {
            return { services_.streamObserver, VulkanIndirectStreamView::Opaque,
                frame };
        }
        [[nodiscard]] bool lodEnabled() const noexcept {
            return config_.lodErrorPixels > 0.0f;
        }
        void bindSlot(uint32_t frame);
        void destroySlot(uint32_t frame) noexcept;
        // gpuOrder_, the indirect plan and the bins; false when the frame
        // falls back to direct draws.
        [[nodiscard]] bool buildBins(const OpaqueIndirectInputs& inputs,
            bool wireframe);

        VulkanCullerServices services_{};
        VulkanOpaqueCullerConfig config_{};
        VulkanOpaqueCullPipelines pipelines_{};
        std::array<VkDescriptorSet, kIndirectCullerFramesInFlight> sets_{};
        VulkanIndirectBufferSet buffers_{};
        Buffers occlusionQueryBuffers_{};
        Buffers occlusionResultBuffers_{};
        Buffers gpuSceneOcclusionResultBuffers_{};
        // R4c.2: replacements parked for in-flight slots.
        VulkanIndirectBufferSet pendingBuffers_{};
        Buffers pendingOcclusionQueryBuffers_{};
        Buffers pendingOcclusionResultBuffers_{};
        Buffers pendingGpuSceneOcclusionResultBuffers_{};
        std::array<bool, kIndirectCullerFramesInFlight> pendingSlots_{};
        // Shared across the ordered graphics queue, not replicated per frame.
        VulkanBufferResource lodHistoryBuffer_{};
        GpuSceneLodHistory lodHistory_;
        uint32_t commandCapacity_ = 0;

        GpuSceneIndirectPlan indirectPlan_;
        std::vector<Bin> bins_;
        std::vector<uint32_t> gpuOrder_;
        // Per gpuOrder_ entry: its position in the submission's order, and
        // whether a direct packet precedes it since the previous entry.
        std::vector<uint32_t> gpuOrderPosition_;
        std::vector<uint8_t> gpuOrderBreak_;
        std::vector<GpuSceneIndirectCandidate> candidates_;
        std::vector<uint8_t> seenHistory_;
        std::vector<DepthPyramidDeviceQuery> occlusionQueries_;
        // This frame's plan, consumed by recordCompaction.
        GpuSceneCapacityRequirements published_{};

        std::array<PendingValidation, kIndirectCullerFramesInFlight> pending_{};
    };

} // namespace Iridium
