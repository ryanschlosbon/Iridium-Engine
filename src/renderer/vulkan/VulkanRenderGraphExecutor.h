#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/graph/RenderGraphAliasing.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <array>
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

        // ---- M7R R4b.4 transient aliasing (PhysicalResourceSlot::aliased) ----
        // The memory an aliased slot's image needs (planning input). The
        // defaults serve device-free fakes: a nominal requirement from the
        // descriptor, heaps that own no memory, and aliased slots created as
        // ordinary resources.
        [[nodiscard]] virtual RenderGraph::TransientMemoryRequirement aliasRequirement(
            const RenderGraph::PhysicalResourceSlot& slot);
        // One block for a planned heap; `requestedBytes` is what its members
        // would need unaliased (accounting only).
        [[nodiscard]] virtual VulkanAliasHeapResource createAliasHeap(
            const RenderGraph::AliasHeap& heap, uint64_t requestedBytes);
        // After every resource bound into the heap is destroyed.
        virtual void destroyAliasHeap(VulkanAliasHeapResource& heap) noexcept;
        [[nodiscard]] virtual VulkanGraphPhysicalResource createAliased(
            const RenderGraph::PhysicalResourceSlot& slot,
            const VulkanAliasHeapResource& heap, uint64_t offset);
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
        // vkGetDeviceImageMemoryRequirements restricted to the legacy memory
        // type; one dedicated VMA block per heap (accounted under the
        // factory's category); vmaCreateAliasingImage2 at the planned offset.
        [[nodiscard]] RenderGraph::TransientMemoryRequirement aliasRequirement(
            const RenderGraph::PhysicalResourceSlot& slot) override;
        [[nodiscard]] VulkanAliasHeapResource createAliasHeap(
            const RenderGraph::AliasHeap& heap, uint64_t requestedBytes) override;
        void destroyAliasHeap(VulkanAliasHeapResource& heap) noexcept override;
        [[nodiscard]] VulkanGraphPhysicalResource createAliased(
            const RenderGraph::PhysicalResourceSlot& slot,
            const VulkanAliasHeapResource& heap, uint64_t offset) override;

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
        // R4b.4: with `plan`, every frame slot gets its own heaps and the
        // graph's aliased slots are created inside them at the planned
        // offsets. Heaps retire with the frame's resources.
        void rebuild(const RenderGraph::CompiledGraph& graph,
            const RenderGraph::AliasPlan* plan = nullptr);
        void onFrameFenceCompleted(uint32_t frameIndex);
        void cleanupAfterDeviceIdle() noexcept;

        [[nodiscard]] uint32_t frameCount() const noexcept {
            return static_cast<uint32_t>(active_.size());
        }
        [[nodiscard]] size_t activeResourceCount(uint32_t frameIndex) const;
        [[nodiscard]] size_t retiredResourceCount(uint32_t frameIndex) const;
        // Resources' requested bytes (aliased images included).
        [[nodiscard]] uint64_t requestedBytes() const noexcept;
        // Dedicated resources' committed bytes plus every alias heap's.
        [[nodiscard]] uint64_t committedBytes() const noexcept;
        // Alias heaps only (active and retired).
        [[nodiscard]] uint64_t aliasHeapCommittedBytes() const noexcept;
        [[nodiscard]] const VulkanGraphPhysicalResource& resource(
            uint32_t frameIndex, uint32_t physicalSlot) const;
        [[nodiscard]] std::span<const VulkanAliasHeapResource> aliasHeaps(
            uint32_t frameIndex) const;

    private:
        struct FrameResources {
            std::vector<VulkanGraphPhysicalResource> resources;
            std::vector<VulkanAliasHeapResource> heaps;
        };
        void destroyFrame(FrameResources& frame) noexcept;

        VulkanGraphResourceFactory* factory_ = nullptr;
        std::vector<FrameResources> active_;
        // Several retired generations may wait for one fence.
        std::vector<std::vector<FrameResources>> retired_;
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
        // R4b.4 transient aliasing (zero without aliased slots). Heap count
        // and peak are per frame slot; the byte totals cover every slot.
        // committedBytes above already includes aliasHeapCommittedBytes.
        bool transientAliasing = false;
        uint32_t aliasHeapCount = 0;
        uint32_t aliasedResourceCount = 0;
        uint64_t aliasedRequestedBytes = 0;
        uint64_t aliasHeapCommittedBytes = 0;
        uint64_t aliasPeakLiveBytes = 0;
    };

    // bindExternalImage target for a binding shared by every frame slot; its
    // tracked state persists across slots (persistent and history-like imports).
    inline constexpr uint32_t VulkanGlobalBinding = UINT32_MAX;

    // R4a retired RenderPassManaged (render passes that transitioned the
    // swapchain and shadow maps themselves): those imports are executor-owned.
    enum class ExternalSyncMode : uint8_t {
        // The executor emits the barriers and tracks the state (default).
        ExecutorOwned,
        // Declared for ordering only; the owning pass records its own barriers.
        OwnerManaged,
    };

    struct ExternalSyncPolicy {
        ExternalSyncMode mode = ExternalSyncMode::ExecutorOwned;
        // R4a, ExecutorOwned only: the image's contents are not needed. Its
        // first use in each frame must be a write and transitions from
        // UNDEFINED, while the source scope keeps the tracked access (the
        // History pattern), e.g. the swapchain after acquire (current =
        // Present: BOTTOM_OF_PIPE, which in sync2 chains with the acquire
        // semaphore wait). Such bindings may share one image across frame
        // slots, since no state carries between them.
        bool discard = false;

        [[nodiscard]] static constexpr ExternalSyncPolicy executorOwned() noexcept {
            return {};
        }
        [[nodiscard]] static constexpr ExternalSyncPolicy discardOnFirstUse() noexcept {
            return { ExternalSyncMode::ExecutorOwned, true };
        }
        [[nodiscard]] static constexpr ExternalSyncPolicy ownerManaged() noexcept {
            return { ExternalSyncMode::OwnerManaged };
        }
    };

    // M7R R3b: the executor records every graph barrier through this seam.
    // The default sink forwards to vkCmdPipelineBarrier/vkCmdPipelineBarrier2;
    // tests inject a recording sink to compare emitted dependencies without a
    // device. This is a Vulkan-side recording seam, not an RHI hook. R4a adds
    // the dynamic-rendering scopes the executor begins for migrated passes;
    // their defaults record the Vulkan commands.
    class VulkanBarrierSink {
    public:
        virtual ~VulkanBarrierSink() = default;
        virtual void pipelineBarrier(VkCommandBuffer commandBuffer,
            VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages,
            std::span<const VkBufferMemoryBarrier> buffers,
            std::span<const VkImageMemoryBarrier> images) = 0;
        virtual void pipelineBarrier2(VkCommandBuffer commandBuffer,
            const VkDependencyInfo& dependency) = 0;
        virtual void beginRendering(VkCommandBuffer commandBuffer,
            const VkRenderingInfo& rendering);
        virtual void endRendering(VkCommandBuffer commandBuffer);
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

    // ---- R4a dynamic rendering ------------------------------------------------

    inline constexpr uint32_t VulkanMaxColorAttachments = 8;
    // Depth usages one pass may choose from per scope (the point-shadow pass
    // writes three pools).
    inline constexpr uint32_t VulkanMaxDepthAttachments = 4;

    // One attachment of a pass's rendering plan, from its graph usage.
    struct VulkanRenderingAttachmentPlan {
        uint32_t logicalResourceIndex = RenderGraph::InvalidIndex;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // The usage's bit-exact clear value (colour bits or depth/stencil).
        VkClearValue clearValue{};
        VkExtent2D extent{};
    };

    // Built at rebuild for every pass (fixed size, no frame allocation).
    // Colour attachments follow usage-declaration order, which matches the
    // framebuffer order of the render passes they replace. Depth candidates
    // are the pass's DepthAttachmentWrite/DepthAttachmentRead usages in
    // declaration order; each scope binds one (read-only depth:
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL, LOAD, STORE_OP_NONE).
    struct VulkanPassRenderingPlan {
        std::array<VulkanRenderingAttachmentPlan, VulkanMaxColorAttachments> color{};
        uint32_t colorCount = 0;
        std::array<VulkanRenderingAttachmentPlan, VulkanMaxDepthAttachments> depth{};
        uint32_t depthCount = 0;
        // false: more than VulkanMaxColorAttachments colour or
        // VulkanMaxDepthAttachments depth usages; beginRendering then throws.
        bool valid = true;

        [[nodiscard]] bool empty() const noexcept { return colorCount == 0 && depthCount == 0; }
        [[nodiscard]] bool hasDepth() const noexcept { return depthCount != 0; }
        // The default render area of a scope binding depth candidate
        // `depthIndex`: the smallest extent of its attachments.
        [[nodiscard]] VkExtent2D renderExtent(uint32_t depthIndex = 0) const noexcept {
            VkExtent2D result{ UINT32_MAX, UINT32_MAX };
            const auto include = [&](VkExtent2D extent) {
                result.width = extent.width < result.width ? extent.width : result.width;
                result.height = extent.height < result.height ? extent.height : result.height;
            };
            for (uint32_t index = 0; index < colorCount; ++index) include(color[index].extent);
            if (depthIndex < depthCount) include(depth[depthIndex].extent);
            return result.width == UINT32_MAX ? VkExtent2D{} : result;
        }
    };

    // Per-call adjustments of a pass's plan.
    struct VulkanRenderingOverrides {
        // A zero extent renders the plan's full extent.
        VkRect2D renderArea{};
        uint32_t layerCount = 1;
        // VK_NULL_HANDLE keeps the resource's view; otherwise a per-layer view
        // (shadow cascades, point faces) of the planned image.
        std::array<VkImageView, VulkanMaxColorAttachments> colorViews{};
        VkImageView depthView = VK_NULL_HANDLE;
        // Which depth candidate the scope binds (declaration order).
        uint32_t depthIndex = 0;
        // Clearing attachments LOAD instead; the pass clears regions itself
        // with vkCmdClearAttachments (spot tiles, point faces).
        bool loadInsteadOfClear = false;
    };

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

        // R4a: begins/ends a dynamic-rendering scope over the pass's planned
        // attachments (this frame slot's views). Only for passes registered
        // with VulkanPassCallbacks::dynamicRendering; scopes may repeat inside
        // one callback (per layer) but must not nest or stay open past it.
        void beginRendering(const VulkanRenderingOverrides& overrides = {});
        void endRendering();
        [[nodiscard]] const VulkanPassRenderingPlan& renderingPlan() const;
    };

    // Where a pass's GPU timestamp range starts relative to its barriers; all
    // three placements exist in the imperative backend today. AroundBarriers
    // measures only the pass's transitions (e.g. gpu.output.graph_transition):
    // it closes before the execute callback runs.
    enum class GpuRangePlacement : uint8_t {
        BeforeBarriers,
        AfterBarriers,
        AroundBarriers,
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
        // R4a: the pass records with dynamic rendering (migrated from its
        // render pass). Same-access ColorAttachment/DepthAttachmentWrite usages
        // then get a memory-only re-barrier (attachment write -> attachment
        // read/write, layout unchanged), the ordering a render pass's external
        // dependency used to provide; beginRendering is allowed only here.
        bool dynamicRendering = false;
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
        // M9 G1: re-keys History validity for the frame's view once the view
        // is known (after extraction). Must precede the frame's first pass.
        void beginViewExecution(const RenderGraph::ViewHistoryContext& view);
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
        // Drops a slot's binding (same retired-slot rule). Owners replacing
        // buffers unbind every slot first, so a recycled handle never trips
        // the aliasing check against a destroyed one. The resource stays
        // tracked while any slot is bound.
        void unbindExternalBuffer(uint32_t frameIndex, RenderGraph::GraphResourceId id);

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
        // Explicit drain point (R3c): runs the registered passes from the
        // cursor through `last` (inclusive) in compiled order. It throws if
        // that would skip an unregistered pass, if `last` is not registered or
        // was already handled; an invalid id (undeclared pass) is a no-op.
        void drainRegisteredThrough(RenderGraph::PassId last);
        // Drains the remaining registered passes, then requires every pass to
        // have been handled. R4a: then records the frame-end export
        // transitions (the compiled transitions at passOrderIndex ==
        // passCount): every exported resource whose tracked state differs from
        // its final access moves there in one dependency on the frame's
        // command buffer. Owner-managed imports are left to their owners.
        void finishFrameExecution();
        // R4a: the dynamic-rendering plan of a compiled pass.
        [[nodiscard]] const VulkanPassRenderingPlan& renderingPlan(
            RenderGraph::PassId pass) const;

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
        // R4b.4: the active plan's alias heaps of one frame slot (empty
        // without aliased slots). Valid until the next rebuild; a heap's
        // memory is reused by that slot's frames only.
        [[nodiscard]] std::span<const VulkanAliasHeapResource> aliasHeaps(
            uint32_t frameIndex) const;

    private:
        friend struct VulkanPassContext;

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
        // R4a: per pass, built at rebuild; exported logicals for frame end.
        std::vector<VulkanPassRenderingPlan> renderingPlans_;
        std::vector<uint32_t> exportedResources_;
        uint32_t callbackPass_ = RenderGraph::InvalidIndex;
        bool renderingOpen_ = false;
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
        // M9 G2: HistoryViewSetCount sets, set-major. Set 0 is created with
        // the plan; later sets are created the first time a view selects them
        // (ensureHistorySet) and retired with the plan.
        std::vector<VulkanGraphPhysicalResource> historyResources_;
        std::vector<RenderGraph::Access> historyAccess_;   // per set and slot
        std::vector<uint8_t> historyParity_;        // per set and pair: slot most recently written
        std::vector<uint8_t> historyWriterBegun_;   // this frame, per pair
        std::vector<uint8_t> historyDiscarded_;     // invalid previous already UNDEFINED, per pair
        uint32_t historySlotsPerSet_ = 0;
        uint32_t historyPairsPerSet_ = 0;
        uint32_t activeHistorySet_ = 0;
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
            // policy.discard: this frame's first use is still to come.
            bool discardPending = false;
        };
        // [frameCount + 1][logical]; the last row holds global bindings.
        std::vector<std::vector<ExternalImageBinding>> externalImages_;
        std::vector<uint8_t> externalImageScope_;   // 0 none, 1 per frame, 2 global
        // R4b.4 transient aliasing, built at rebuild. An aliased slot starts
        // every frame Undefined; its first use (which must be its first-use
        // writer) transitions from UNDEFINED after the tracked accesses of
        // the slots that occupied its memory earlier in the frame
        // (AliasPlan::aliasPredecessors, by physical slot).
        std::vector<uint8_t> aliasedSlot_;              // per physical slot
        std::vector<uint32_t> aliasedSlots_;            // indices of aliased slots
        std::vector<uint32_t> aliasPredecessorFirst_;   // physicalSlotCount + 1
        std::vector<uint32_t> aliasPredecessorSlots_;
        uint32_t aliasHeapCount_ = 0;
        uint64_t aliasedRequestedBytes_ = 0;            // per frame slot
        uint64_t aliasPeakLiveBytes_ = 0;               // per frame slot
        void queueAliasedFirstUse(const RenderGraph::CompiledResource& resource,
            const RenderGraph::CompiledUsage& usage);

        [[nodiscard]] const RenderGraph::CompiledGraph& executingGraph() const;
        [[nodiscard]] const RenderGraph::CompiledGraph& boundGraph() const;
        void beginPassAt(VkCommandBuffer commandBuffer, uint32_t passOrder);
        void queuePhysicalTransition(uint32_t physicalSlot, RenderGraph::Access access,
            bool attachmentRebarrier = false);
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
        void selectHistoryView(const RenderGraph::ViewHistoryContext& view);
        void ensureHistorySet(uint32_t set);
        void queueHistoryUsage(const RenderGraph::CompiledResource& resource,
            RenderGraph::Access access, bool attachmentRebarrier = false);
        void queueExternalImageUsage(ExternalImageBinding& binding,
            const RenderGraph::CompiledUsage& usage, bool attachmentRebarrier = false);
        uint32_t queueFrameEndExports(bool record);
        void beginPassRendering(RenderGraph::PassId pass, VkCommandBuffer commandBuffer,
            const VulkanRenderingOverrides& overrides);
        void endPassRendering(RenderGraph::PassId pass, VkCommandBuffer commandBuffer);
        [[nodiscard]] const ExternalImageBinding* externalImageBinding(
            uint32_t frameIndex, uint32_t logical) const noexcept;
        void destroyHistoryResources() noexcept;
    };

} // namespace Iridium
