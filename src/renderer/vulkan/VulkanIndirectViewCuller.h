#pragma once

// VulkanIndirectViewCuller (M7R R3a): the GPU-driven shadow/probe caster
// compaction shared by the directional, spot, point and reflection-probe views.
//
// One culler per view kind, configured by IndirectViewKindConfig (shader,
// push-constant width, consumer, membership cache, capacity rule, compaction
// placement, telemetry names). Each frame:
//   plan             bins the GPU-scene casters (or reuses the cached
//                    membership), enumerates the kind's work items, commits the
//                    validation slot and writes the candidate/count buffers;
//   recordCompaction records the host->compute barrier and, for batched kinds,
//                    every work item's compaction dispatch and the
//                    compute->indirect barrier;
//   recordWorkItem   (per-work-item kinds: the probe) one face's dispatch;
//   recordDraws      the indirect-count draws of one work item;
//   emitExpectations qualification-oracle commands for one work item;
//   collect          device telemetry and the oracle verdict once the slot's
//                    fence has retired.
// Everything recorded goes through VulkanCullerCommands; everything allocated
// through VulkanCullerResources. Steady frames allocate nothing: scratch is
// reserved in resize().

#include "VulkanIndirectCullerShared.h"
#include "renderer/rhi/ReflectionProbeCapture.h"
#include "renderer/rhi/ShadowTypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    enum class IndirectViewKind : uint8_t {
        DirectionalShadow,
        SpotShadow,
        PointShadow,
        ReflectionProbe,
    };
    inline constexpr uint32_t kIndirectViewKindCount = 4u;

    enum class CompactionPlacement : uint8_t {
        BatchedAtPrepare, // every work item dispatched by recordCompaction
        PerWorkItem,      // the caller records each item with recordWorkItem
    };

    // One compaction dispatch: push word 5, its work index (count/command
    // region), push word 9.
    struct IndirectWorkItem {
        uint32_t slotWord = 0;
        uint32_t workIndex = 0;
        uint32_t extraWord = 0;
    };

    // What a kind's work enumerator fills.
    struct IndirectWorkSink {
        std::span<uint32_t> workIndices{};           // per slot word; preset invalid
        std::vector<IndirectWorkItem>* items = nullptr; // batched dispatch order
        uint32_t workCount = 0;
    };

    struct IndirectWorkEnumeration {
        const void* packets = nullptr;
        size_t packetCount = 0;
        // Returns None, or the reason the whole submission falls back.
        GpuSceneIndirectFallbackReason (*enumerate)(const void* packets,
            size_t packetCount, IndirectWorkSink& sink) = nullptr;
    };

    [[nodiscard]] IndirectWorkEnumeration directionalShadowWork(
        std::span<const DirectionalShadowFramePacket> shadows) noexcept;
    [[nodiscard]] IndirectWorkEnumeration spotShadowWork(
        std::span<const SpotShadowFramePacket> shadows) noexcept;
    [[nodiscard]] IndirectWorkEnumeration pointShadowWork(
        std::span<const PointShadowFramePacket> shadows) noexcept;
    [[nodiscard]] IndirectWorkEnumeration reflectionProbeWork(
        std::span<const ReflectionProbeCaptureScheduleEntry> captures) noexcept;

    struct IndirectViewKindConfig {
        IndirectViewKind kind = IndirectViewKind::DirectionalShadow;
        VulkanIndirectOracleView oracleView = VulkanIndirectOracleView::DirectionalShadow;
        VulkanIndirectStreamView streamView = VulkanIndirectStreamView::DirectionalShadow;
        const char* subject = "";         // "directional-shadow", ... (messages)
        const char* shader = "";          // compact compute SPIR-V (project-relative)
        const char* pipelineName = "";    // "<subject> compact"
        uint32_t pushWords = 9;
        uint32_t consumerMask = 0;        // GPU-scene consumer bit
        bool membershipCache = false;     // reuse bins while the membership revision holds
        bool forceDirectIncludesShadowReference = false;
        // Command capacity: primitive x maximumWorkCount, capped at the local
        // maximum, or primitive x maximumWorkCount uncapped.
        bool capCommandsAtLocalMaximum = false;
        uint32_t maximumWorkCount = 0;
        uint32_t workSlotCount = 0;       // size of the per-slot work-index table
        CompactionPlacement placement = CompactionPlacement::BatchedAtPrepare;
        const char* compactGpuRange = nullptr; // batched kinds only
    };

    [[nodiscard]] const IndirectViewKindConfig& indirectViewKindConfig(
        IndirectViewKind kind) noexcept;

    // Commands with more than this many entries never fit a local-light view.
    inline constexpr uint32_t kLocalShadowIndirectMaximumCommandCount = 1u << 20;

    struct VulkanCullerServices {
        VulkanCullerCommands commands{};
        VulkanCullerResources resources{};
        VulkanIndirectCapabilities capabilities{};
        CpuProfiler* profiler = nullptr;
        // The attached oracle: collect hands it every retired slot.
        IVulkanIndirectOracle* oracle = nullptr;
        IVulkanIndirectStreamObserver* streamObserver = nullptr;
        // The CPU GPU-scene mirror of a frame slot (collect).
        const void* sceneOwner = nullptr;
        VulkanIndirectScene (*scene)(const void* owner, uint32_t slot) = nullptr;
        // Upper bound of resize()'s primitive capacity.
        uint32_t maximumPrimitiveCapacity = 0;
    };

    struct VulkanCompactPipeline {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        void destroy(VkDevice device) noexcept;
    };

    // Set 0 (the view's own layout), the GPU-scene tables, the shared
    // 3-binding indirect set.
    [[nodiscard]] VulkanCompactPipeline createIndirectViewPipeline(VkDevice device,
        IndirectViewKind kind, VkDescriptorSetLayout set0Layout,
        VkDescriptorSetLayout gpuSceneLayout, VkDescriptorSetLayout indirectLayout);

    struct IndirectViewInputs {
        std::span<const uint32_t> primitiveIndices{};
        uint64_t membershipRevision = 0;
        float lodErrorThreshold = 0.0f;
        uint32_t lodMaximumLevel = 15u;
        bool forceDirectGBufferReference = false;
        bool forceDirectShadowReference = false;
        VulkanIndirectScene scene{};
        VulkanIndirectAssetResolver assets{};
    };

    struct IndirectCompactionSets {
        VkDescriptorSet set0 = VK_NULL_HANDLE;
        // Per-work-item kinds bind set 0 with one dynamic offset.
        uint32_t set0DynamicOffset = 0;
        VkDescriptorSet gpuScene = VK_NULL_HANDLE;
    };

    struct IndirectDrawBinder {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        const void* owner = nullptr;
        VkPipeline (*pipeline)(const void* owner, bool alphaMasked,
            bool doubleSided) = nullptr;
        // Shadow views push CanonicalMeshPushConstants{ padding[0] = slotWord }.
        bool pushSlotWord = false;
        uint32_t slotWord = 0;
    };

    // CPU LOD selection for one expected command (qualification oracle).
    struct IndirectLodMetric {
        const void* context = nullptr;
        uint32_t (*select)(const void* context, const VulkanIndirectScene& scene,
            const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) = nullptr;
    };

    class VulkanIndirectViewCuller {
    public:
        struct Bin {
            uint32_t commandBegin = 0;
            uint32_t commandCount = 0;
            GeometryHandle geometry;
            VkBuffer vertexBuffer = VK_NULL_HANDLE;
            VkBuffer indexBuffer = VK_NULL_HANDLE;
            VkIndexType indexType = VK_INDEX_TYPE_UINT32;
            bool alphaMasked = false;
            bool doubleSided = false;
        };

        // Allocates the per-frame indirect sets; the culler owns `pipeline`.
        void init(const VulkanCullerServices& services, IndirectViewKind kind,
            const VulkanCompactPipeline& pipeline,
            VkDescriptorSetLayout indirectLayout,
            IVulkanIndirectOracle* activeOracle);
        // Frees sets and buffers, destroys the pipeline (device may be null
        // when it was never created on a device).
        void destroy(VkDevice device) noexcept;

        // Capacity growth at a frame boundary: allocates, drains every frame
        // when replacing live buffers, collects this view's pending slots,
        // replaces the buffers and rebinds. The only deferred-deletion site.
        void resize(uint32_t primitiveCapacity, bool frameOpen);

        // False selects the direct fallback for this frame (fallbackReason()).
        [[nodiscard]] bool plan(const IndirectViewInputs& inputs,
            const IndirectWorkEnumeration& work, uint32_t frame);
        // After a successful plan(); returns the dispatches recorded.
        uint32_t recordCompaction(VkCommandBuffer cmd, uint32_t frame,
            const IndirectCompactionSets& sets);
        uint32_t recordWorkItem(VkCommandBuffer cmd, uint32_t frame,
            const IndirectCompactionSets& sets, const IndirectWorkItem& item);
        // Returns the bins drawn.
        uint32_t recordDraws(VkCommandBuffer cmd, uint32_t frame,
            uint32_t workIndex, const IndirectDrawBinder& binder);
        // Expected commands for one work item's casters whose `visibility`
        // entry has `visibilityBit` set.
        void emitExpectations(IVulkanIndirectOracle& oracle, uint32_t frame,
            uint32_t workIndex, std::span<const VulkanResolvedCaster> casters,
            std::span<const uint8_t> visibility, uint8_t visibilityBit,
            const IndirectLodMetric& lod) const;
        void collect(uint32_t frame);

        [[nodiscard]] const IndirectViewKindConfig& config() const noexcept {
            return *config_;
        }
        [[nodiscard]] GpuSceneIndirectFallbackReason fallbackReason() const noexcept {
            return fallbackReason_;
        }
        [[nodiscard]] bool membershipCacheHit() const noexcept { return cacheHit_; }
        [[nodiscard]] std::span<const Bin> bins() const noexcept { return bins_; }
        [[nodiscard]] std::span<const GpuSceneIndirectCandidate> candidates()
            const noexcept { return candidates_; }
        [[nodiscard]] bool anyAlphaMaskedBin() const noexcept;
        [[nodiscard]] uint32_t workIndex(uint32_t slotWord) const noexcept {
            return slotWord < workIndices_.size() ? workIndices_[slotWord]
                : InvalidGpuSceneIndex;
        }
        [[nodiscard]] uint32_t primitiveCapacity() const noexcept {
            return primitiveCapacity_;
        }
        [[nodiscard]] const VulkanIndirectBufferSet& buffers() const noexcept {
            return buffers_;
        }

    private:
        struct PendingValidation {
            uint64_t profileFrameId = 0;
            std::vector<uint32_t> countCapacities;
            std::vector<uint32_t> commandOffsets;
            bool pending = false;
        };

        [[nodiscard]] bool reject(GpuSceneIndirectFallbackReason reason);
        [[nodiscard]] VulkanIndirectStreamTap stream(uint32_t frame) const noexcept {
            return { services_.streamObserver, config_->streamView, frame };
        }

        VulkanCullerServices services_{};
        const IndirectViewKindConfig* config_ = &indirectViewKindConfig(
            IndirectViewKind::DirectionalShadow);
        IVulkanIndirectOracle* activeOracle_ = nullptr;
        VulkanCompactPipeline pipeline_{};
        std::array<VkDescriptorSet, kIndirectCullerFramesInFlight> sets_{};
        VulkanIndirectBufferSet buffers_{};
        uint32_t primitiveCapacity_ = 0;
        uint32_t commandCapacity_ = 0;
        uint32_t countCapacity_ = 0;

        // Membership (bins and candidates) and its cache key.
        std::vector<Bin> bins_;
        std::vector<GpuSceneIndirectCandidate> candidates_;
        std::vector<GpuSceneIndirectCandidate> unsortedCandidates_;
        std::vector<uint32_t> binCursorScratch_;
        std::vector<uint32_t> primitiveBinScratch_;
        uint64_t membershipRevision_ = 0;
        uint32_t membershipLodErrorBits_ = 0;
        uint32_t membershipMaximumLod_ = 0;

        // This frame's plan.
        std::vector<uint32_t> workIndices_;
        std::vector<IndirectWorkItem> workItems_;
        GpuSceneIndirectFallbackReason fallbackReason_ =
            GpuSceneIndirectFallbackReason::None;
        bool cacheHit_ = false;
        uint32_t commandStride_ = 0; // commands per work item
        uint32_t lodErrorBits_ = 0;
        GpuSceneCapacityRequirements published_{};
        VulkanIndirectScene scene_{};

        std::array<PendingValidation, kIndirectCullerFramesInFlight> pending_{};
    };

} // namespace Iridium
