#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Iridium {

    struct VulkanGraphAccessInfo {
        VkPipelineStageFlags stages = 0;
        VkAccessFlags access = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    [[nodiscard]] VulkanGraphAccessInfo getVulkanGraphAccessInfo(
        RenderGraph::Access access, RenderGraph::ResourceType type);
    [[nodiscard]] VkFormat toVkFormat(RenderGraph::Format format);
    [[nodiscard]] RenderGraph::Format toGraphFormat(VkFormat format);

    struct VulkanGraphBarrierIntent {
        uint32_t passOrderIndex = RenderGraph::InvalidIndex;
        uint32_t logicalResourceIndex = RenderGraph::InvalidIndex;
        uint32_t physicalSlot = RenderGraph::InvalidIndex;
        VulkanGraphAccessInfo before{};
        VulkanGraphAccessInfo after{};
    };

    struct VulkanGraphPhysicalResource {
        RenderGraph::ResourceType type = RenderGraph::ResourceType::Image;
        VulkanImageResource image;
        VulkanBufferResource buffer;

        [[nodiscard]] bool isValid() const noexcept {
            return type == RenderGraph::ResourceType::Image
                ? image.isValid()
                : buffer.isValid();
        }
    };

    class VulkanGraphResourceFactory {
    public:
        virtual ~VulkanGraphResourceFactory() = default;
        [[nodiscard]] virtual VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) = 0;
        virtual void destroy(VulkanGraphPhysicalResource& resource) noexcept = 0;
    };

    class VulkanAllocatorGraphResourceFactory final
        : public VulkanGraphResourceFactory {
    public:
        explicit VulkanAllocatorGraphResourceFactory(
            VulkanResourceAllocator& allocator,
            ProfileMemoryCategory category) noexcept
            : allocator_(&allocator), category_(category) {}

        [[nodiscard]] VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) override;
        void destroy(VulkanGraphPhysicalResource& resource) noexcept override;

    private:
        VulkanResourceAllocator* allocator_ = nullptr;
        ProfileMemoryCategory category_ = ProfileMemoryCategory::RenderGraphTransient;
    };

    class VulkanGraphResourcePool final {
    public:
        VulkanGraphResourcePool() = default;
        VulkanGraphResourcePool(const VulkanGraphResourcePool&) = delete;
        VulkanGraphResourcePool& operator=(const VulkanGraphResourcePool&) = delete;
        ~VulkanGraphResourcePool();

        void init(VulkanGraphResourceFactory& factory, uint32_t frameCount);
        void rebuild(const RenderGraph::CompiledGraph& graph);
        void onFrameFenceCompleted(uint32_t frameIndex);
        void cleanupAfterDeviceIdle() noexcept;

        [[nodiscard]] uint32_t frameCount() const noexcept {
            return static_cast<uint32_t>(active_.size());
        }
        [[nodiscard]] size_t activeResourceCount(uint32_t frameIndex) const;
        [[nodiscard]] size_t retiredResourceCount(uint32_t frameIndex) const;
        [[nodiscard]] uint64_t requestedBytes() const noexcept;
        [[nodiscard]] uint64_t committedBytes() const noexcept;
        [[nodiscard]] const VulkanGraphPhysicalResource& resource(
            uint32_t frameIndex, uint32_t physicalSlot) const;

    private:
        void destroyResources(std::vector<VulkanGraphPhysicalResource>& resources) noexcept;

        VulkanGraphResourceFactory* factory_ = nullptr;
        std::vector<std::vector<VulkanGraphPhysicalResource>> active_;
        std::vector<std::vector<VulkanGraphPhysicalResource>> retired_;
    };

    struct VulkanGraphStats {
        bool enabled = false;
        uint64_t topologyHash = 0;
        uint32_t passCount = 0;
        uint32_t logicalResourceCount = 0;
        uint32_t physicalSlotCount = 0;
        uint32_t barrierCount = 0;
        uint32_t frameCount = 0;
        uint64_t requestedBytes = 0;
        uint64_t committedBytes = 0;
        uint64_t rebuildCount = 0;
        uint64_t cacheMissCount = 0;
        // History-pair slots (two per pair, global; included in the bytes).
        uint32_t historySlotCount = 0;
    };

    // bindExternalImage target for a binding shared by every frame slot; its
    // tracked state persists across slots (persistent and history-like imports).
    inline constexpr uint32_t VulkanGlobalBinding = UINT32_MAX;

    enum class ExternalSyncMode : uint8_t {
        // The executor emits the barriers and tracks the state (default).
        ExecutorOwned,
        // No barriers: a render pass transitions the image. Each writing pass
        // asserts the tracked state is `initial` and leaves it `final`; reading
        // passes assert the tracked layout matches their access. Swapchain
        // (Undefined -> Present) and shadow maps (SampledRead both ways).
        RenderPassManaged,
        // Declared for ordering only; the owning pass records its own barriers.
        OwnerManaged,
    };

    struct ExternalSyncPolicy {
        ExternalSyncMode mode = ExternalSyncMode::ExecutorOwned;
        RenderGraph::Access initial = RenderGraph::Access::Undefined;
        RenderGraph::Access final = RenderGraph::Access::Undefined;

        [[nodiscard]] static constexpr ExternalSyncPolicy executorOwned() noexcept {
            return {};
        }
        [[nodiscard]] static constexpr ExternalSyncPolicy renderPassManaged(
            RenderGraph::Access initial, RenderGraph::Access final) noexcept {
            return { ExternalSyncMode::RenderPassManaged, initial, final };
        }
        [[nodiscard]] static constexpr ExternalSyncPolicy ownerManaged() noexcept {
            return { ExternalSyncMode::OwnerManaged };
        }
    };

    // M7R R3b: the executor records every graph barrier through this seam.
    // The default sink forwards to vkCmdPipelineBarrier/vkCmdPipelineBarrier2;
    // tests inject a recording sink to compare emitted dependencies without a
    // device. This is a Vulkan-side recording seam, not an RHI hook.
    class VulkanBarrierSink {
    public:
        virtual ~VulkanBarrierSink() = default;
        virtual void pipelineBarrier(VkCommandBuffer commandBuffer,
            VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages,
            std::span<const VkBufferMemoryBarrier> buffers,
            std::span<const VkImageMemoryBarrier> images) = 0;
        virtual void pipelineBarrier2(VkCommandBuffer commandBuffer,
            const VkDependencyInfo& dependency) = 0;
    };

    [[nodiscard]] VulkanBarrierSink& vulkanCommandBarrierSink() noexcept;

    // How a pass's batched dependencies are recorded. Synchronization2 issues
    // one vkCmdPipelineBarrier2 per pass; Synchronization1 is the fallback for
    // devices without the feature and records the same dependencies as one
    // vkCmdPipelineBarrier per barrier, exactly as before R3b.2.
    enum class VulkanBarrierApi : uint8_t {
        Synchronization1,
        Synchronization2,
    };

    // VkContext and HeadlessVulkanDevice enable synchronization2 exactly when
    // the physical device reports it for Vulkan 1.3, so support implies enabled.
    [[nodiscard]] bool vulkanDeviceSupportsSynchronization2(
        VkPhysicalDevice physicalDevice) noexcept;

    // The sync1 -> sync2 equivalence mapping (R3b.2): stage and access bits are
    // identical; a TOP_OF_PIPE source scope is NONE. Exposed for tests.
    [[nodiscard]] VkPipelineStageFlags2 toVulkanSourceStages2(
        VkPipelineStageFlags stages) noexcept;
    [[nodiscard]] VkPipelineStageFlags2 toVulkanDestinationStages2(
        VkPipelineStageFlags stages) noexcept;

    class VulkanRenderGraphExecutor;

    // M7R R3b.3 callback execution. The per-frame recording context handed to
    // registered passes (R3c moves its producer into the feature context).
    struct VulkanFrameRecordContext {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        uint32_t frameIndex = 0;
        uint32_t imageIndex = 0;
        bool collectCounters = false;
        VkExtent2D sceneExtent{};
    };

    struct VulkanPassContext {
        const VulkanFrameRecordContext& frame;
        VulkanRenderGraphExecutor& graph;
        RenderGraph::PassId pass;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    };

    // Where a pass's GPU timestamp range starts relative to its barriers; both
    // placements exist in the imperative backend today.
    enum class GpuRangePlacement : uint8_t {
        BeforeBarriers,
        AfterBarriers,
    };

    // Execute side of a pass, indexed by compiled order. Plain function
    // pointers: registration and dispatch never allocate.
    struct VulkanPassCallbacks {
        void* owner = nullptr;
        // false => the pass is skipped (no barriers, no range). nullptr => active.
        bool (*active)(void* owner, const VulkanFrameRecordContext& frame) = nullptr;
        void (*execute)(void* owner, VulkanPassContext& context) = nullptr;
        const char* gpuRange = nullptr;
        GpuRangePlacement placement = GpuRangePlacement::BeforeBarriers;
    };

    // One GPU range over several registered passes (e.g. gpu.lighting.cluster).
    // It opens before the first active pass of the group (before that pass's
    // own range and barriers) and closes after the last pass of the group.
    struct VulkanRangeGroup {
        const char* name = nullptr;
        RenderGraph::PassId firstPass;
        RenderGraph::PassId lastPass;
    };

    // GPU timestamp ranges for callback passes; forScheduler adapts the frame
    // scheduler. Without a sink, ranges are not recorded.
    struct VulkanGpuRangeSink {
        void* owner = nullptr;
        VulkanGpuRangeToken (*begin)(void* owner, const char* name) = nullptr;
        void (*end)(void* owner, VulkanGpuRangeToken& token) = nullptr;

        [[nodiscard]] static VulkanGpuRangeSink forScheduler(
            VulkanFrameScheduler& scheduler) noexcept;
        [[nodiscard]] bool enabled() const noexcept {
            return begin != nullptr && end != nullptr;
        }
    };

    class VulkanRenderGraphExecutor final {
    public:
        VulkanRenderGraphExecutor() = default;
        ~VulkanRenderGraphExecutor();
        VulkanRenderGraphExecutor(const VulkanRenderGraphExecutor&) = delete;
        VulkanRenderGraphExecutor& operator=(const VulkanRenderGraphExecutor&) = delete;

        // The allocator form selects Synchronization2 when the allocator's
        // device supports it; the factory form starts on Synchronization1.
        void init(VulkanResourceAllocator& allocator, uint32_t frameCount,
            ProfileMemoryCategory category = ProfileMemoryCategory::RenderGraphTransient);
        void init(VulkanGraphResourceFactory& factory, uint32_t frameCount);
        // Overrides the recording API; only valid outside frame execution.
        // Pass VkContext::hasSynchronization2() when the context is at hand.
        void setBarrierApi(VulkanBarrierApi api);
        [[nodiscard]] VulkanBarrierApi barrierApi() const noexcept { return barrierApi_; }
        void rebuild(RenderGraph::CompiledGraph graph);
        void onFrameFenceCompleted(uint32_t frameIndex);
        [[nodiscard]] bool validateFrame(uint32_t frameIndex) noexcept;
        // The single-argument form reuses the last view context (initially
        // {0, 0}), so it never invalidates History by itself.
        void beginFrameExecution(uint32_t frameIndex);
        // R3b.10: History validity is re-keyed per frame; a key change (view
        // identity, reset revision, extent, format, topology) invalidates.
        void beginFrameExecution(uint32_t frameIndex,
            const RenderGraph::ViewHistoryContext& view);
        // Whether `id`'s History pair holds valid previous contents this frame.
        // An invalid `previous` is transitioned from UNDEFINED on first use.
        [[nodiscard]] bool historyValid(RenderGraph::GraphResourceId id) const;
        // Non-owning imported image (R3b.6 wires the production imports).
        // frameOrGlobal is a frame slot (bind after its fence retired) or
        // VulkanGlobalBinding (bind outside frame execution). `current` is the
        // image's state now; the binding's tracked state then follows `policy`.
        void bindExternalImage(uint32_t frameOrGlobal, RenderGraph::GraphResourceId id,
            const VulkanImageResource& image, RenderGraph::Access current,
            ExternalSyncPolicy policy = {});
        [[nodiscard]] RenderGraph::Access externalImageAccess(uint32_t frameOrGlobal,
            RenderGraph::GraphResourceId id) const;
        // Non-owning, opt-in synchronization for persistent per-slot buffers.
        // Bind only after the slot fence retires; handles must be slot-distinct.
        // `size` must cover the declared size; for a variableSize declaration
        // the barriers cover the bound size.
        void bindExternalBuffer(uint32_t frameIndex, RenderGraph::GraphResourceId id,
            VkBuffer buffer, VkDeviceSize size,
            RenderGraph::Access initialAccess = RenderGraph::Access::Undefined);

        // Ids are resolved per rebuild through O(1) name maps over strings the
        // compiled graph owns; find* return an invalid id for unknown names and
        // passId/resourceId throw std::out_of_range. Names are for resolution,
        // diagnostics and tests only: execution is addressed by id (R3b.4).
        [[nodiscard]] RenderGraph::PassId findPass(std::string_view name) const noexcept;
        [[nodiscard]] RenderGraph::GraphResourceId findResource(
            std::string_view name) const noexcept;
        [[nodiscard]] RenderGraph::PassId passId(std::string_view name) const;
        [[nodiscard]] RenderGraph::GraphResourceId resourceId(std::string_view name) const;

        // Index-addressed execution. Registered callback passes that precede
        // `pass` in compiled order run first (drain); it throws if that would
        // skip an unregistered pass, or if `pass` itself is registered. Every
        // pass must be begun or skipped in compiled order.
        void beginPass(VkCommandBuffer commandBuffer, RenderGraph::PassId pass);
        void skipPass(RenderGraph::PassId pass);
        // Drains the remaining registered passes, then requires every pass to
        // have been handled.
        void finishFrameExecution();

        // Callback registry (R3b.3). Registrations are per compiled plan: a
        // rebuild clears them, owners re-register after it. Not allowed during
        // frame execution. Unregistering a pass also dissolves its range group
        // (rollback restores the imperative call).
        void registerPass(RenderGraph::PassId pass, const VulkanPassCallbacks& callbacks);
        void unregisterPass(RenderGraph::PassId pass);
        [[nodiscard]] bool isRegistered(RenderGraph::PassId pass) const noexcept;
        void registerRangeGroup(const VulkanRangeGroup& group);
        void setGpuRangeSink(const VulkanGpuRangeSink& sink) noexcept { rangeSink_ = sink; }
        // Context for drained callbacks this frame. Without it, the first
        // beginPass of the frame supplies {commandBuffer, frameIndex}.
        void setFrameRecordContext(const VulkanFrameRecordContext& context);
        void transitionImage(VkCommandBuffer commandBuffer,
            RenderGraph::GraphResourceId id, RenderGraph::Access access);
        void cleanupAfterDeviceIdle() noexcept;

        // nullptr restores the Vulkan command sink.
        void setBarrierSink(VulkanBarrierSink* sink) noexcept;

        [[nodiscard]] const std::vector<VulkanGraphBarrierIntent>& barriers() const noexcept {
            return barriers_;
        }
        [[nodiscard]] VulkanGraphStats stats() const noexcept;
        [[nodiscard]] const VulkanImageResource& image(uint32_t frameIndex,
            RenderGraph::GraphResourceId id) const;
        [[nodiscard]] const VulkanBufferResource& buffer(uint32_t frameIndex,
            RenderGraph::GraphResourceId id) const;
        // The compiled plan currently bound (nullptr before the first rebuild).
        [[nodiscard]] const RenderGraph::CompiledGraph* compiledGraph() const noexcept {
            return graph_;
        }

    private:
        struct TransparentStringHash {
            using is_transparent = void;
            [[nodiscard]] size_t operator()(std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
            [[nodiscard]] size_t operator()(const std::string& value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
            [[nodiscard]] size_t operator()(const char* value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };
        using NameMap = std::unordered_map<std::string_view, uint32_t,
            TransparentStringHash, std::equal_to<>>;

        std::optional<VulkanAllocatorGraphResourceFactory> allocatorFactory_;
        VulkanGraphResourcePool resources_;
        RenderGraph::CompiledGraphCache cache_;
        std::vector<VulkanGraphBarrierIntent> barriers_;
        uint64_t topologyHash_ = 0;
        uint32_t passCount_ = 0;
        uint32_t logicalResourceCount_ = 0;
        uint32_t physicalSlotCount_ = 0;
        uint64_t rebuildCount_ = 0;
        uint64_t cacheMissCount_ = 0;
        std::vector<std::vector<RenderGraph::Access>> frameAccess_;
        uint32_t executingFrame_ = RenderGraph::InvalidIndex;
        uint32_t nextPass_ = 0;
        struct ExternalBufferBinding {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            RenderGraph::Access access = RenderGraph::Access::Undefined;
        };
        std::vector<std::vector<ExternalBufferBinding>> externalBuffers_;
        std::vector<bool> externalBufferTracked_;
        std::vector<bool> frameRetired_;
        // Points into cache_; refreshed on every rebuild.
        const RenderGraph::CompiledGraph* graph_ = nullptr;
        NameMap passNames_;
        NameMap resourceNames_;
        VulkanBarrierSink* sink_ = &vulkanCommandBarrierSink();
        VulkanBarrierApi barrierApi_ = VulkanBarrierApi::Synchronization1;
        // One pass's dependency, sized at rebuild to the largest pass so steady
        // frames never allocate. batchOrder_ keeps usage order across both
        // arrays (high bit = image) for the sync1 fallback.
        static constexpr uint32_t BatchImageBit = 0x8000'0000u;
        std::vector<VkImageMemoryBarrier2> imageBatch_;
        std::vector<VkBufferMemoryBarrier2> bufferBatch_;
        std::vector<uint32_t> batchOrder_;
        uint32_t imageBatchCount_ = 0;
        uint32_t bufferBatchCount_ = 0;
        uint32_t batchOrderCount_ = 0;
        // Callback registry, sized at rebuild (indexed by compiled order).
        std::vector<VulkanPassCallbacks> callbacks_;
        std::vector<uint32_t> passGroup_;
        std::vector<VulkanRangeGroup> rangeGroups_;
        uint32_t registeredCount_ = 0;
        VulkanGpuRangeSink rangeSink_{};
        VulkanFrameRecordContext recordContext_{};
        bool hasRecordContext_ = false;
        bool inCallback_ = false;
        uint32_t openGroup_ = RenderGraph::InvalidIndex;
        VulkanGpuRangeToken groupToken_{};
        // History lifetime (R3b.10). Slots are global (not per frame slot);
        // their access state persists across frame slots.
        VulkanGraphResourceFactory* factory_ = nullptr;
        std::vector<VulkanGraphPhysicalResource> historyResources_;
        std::vector<RenderGraph::Access> historyAccess_;
        std::vector<uint8_t> historyParity_;        // slot most recently written
        std::vector<uint8_t> historyWriterBegun_;   // this frame
        std::vector<uint8_t> historyDiscarded_;     // invalid previous already UNDEFINED
        std::vector<uint32_t> passHistoryWrites_;   // pairs written, flat by pass
        std::vector<uint32_t> passHistoryWriteFirst_;   // passCount + 1 offsets
        struct RetiredHistory {
            VulkanGraphPhysicalResource resource;
            uint32_t pendingFrames = 0;   // bit per frame slot still to retire
        };
        std::vector<RetiredHistory> retiredHistory_;
        RenderGraph::HistoryValidityTracker historyValidity_;
        RenderGraph::ViewHistoryContext lastView_{};
        struct ExternalImageBinding {
            VulkanImageResource image{};
            RenderGraph::Access access = RenderGraph::Access::Undefined;
            ExternalSyncPolicy policy{};
            bool bound = false;
        };
        // [frameCount + 1][logical]; the last row holds global bindings.
        std::vector<std::vector<ExternalImageBinding>> externalImages_;
        std::vector<uint8_t> externalImageScope_;   // 0 none, 1 per frame, 2 global

        [[nodiscard]] const RenderGraph::CompiledGraph& executingGraph() const;
        [[nodiscard]] const RenderGraph::CompiledGraph& boundGraph() const;
        void beginPassAt(VkCommandBuffer commandBuffer, uint32_t passOrder);
        void queuePhysicalTransition(uint32_t physicalSlot, RenderGraph::Access access);
        void queueImageBarrier(const VulkanImageResource& image,
            const VulkanGraphAccessInfo& before, const VulkanGraphAccessInfo& after);
        void queueBufferBarrier(VkBuffer buffer, VkDeviceSize size,
            const VulkanGraphAccessInfo& before, const VulkanGraphAccessInfo& after);
        void flushBarriers(VkCommandBuffer commandBuffer);
        void requireCursorAt(uint32_t passOrder, const char* message) const;
        void noteCommandBuffer(VkCommandBuffer commandBuffer);
        void drainRegisteredBefore(uint32_t passOrder);
        void drainRegisteredAtCursor();
        void runRegisteredPass(uint32_t passOrder);
        [[nodiscard]] uint32_t historySlot(const RenderGraph::CompiledResource& resource) const noexcept;
        void queueHistoryUsage(const RenderGraph::CompiledResource& resource,
            RenderGraph::Access access);
        void queueExternalImageUsage(ExternalImageBinding& binding,
            const RenderGraph::CompiledUsage& usage);
        [[nodiscard]] const ExternalImageBinding* externalImageBinding(
            uint32_t frameIndex, uint32_t logical) const noexcept;
        void destroyHistoryResources() noexcept;
    };

} // namespace Iridium
