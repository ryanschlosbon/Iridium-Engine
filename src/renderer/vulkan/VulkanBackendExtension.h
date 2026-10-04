#pragma once

// Vulkan backend extension interfaces (M7R R2.7/R2.8). Always compiled: the
// backend sees only these interfaces, never their implementations. The
// qualification library (src/qualification/vulkan) implements them and is
// attached through the backend factory; with no extension attached the backend
// declares no hook passes and runs no oracle work (the null object).

#include "renderer/rhi/RenderBackendConfig.h"
#include "renderer/rhi/RenderBackendExtension.h"
#include "renderer/rhi/DepthPyramid.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneIndirect.h"
#include "renderer/rhi/RenderFrame.h"
#include "renderer/rhi/ShadowTypes.h"
#include "renderer/rhi/VirtualShadowMap.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "core/types/FrameCapture.h"
#include "VulkanProductionRenderGraph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <variant>
#include <vector>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace Iridium {

    class CpuProfiler;
    class IVulkanEditorUi;
    class VulkanResourceAllocator;
    class VulkanFrameScheduler;
    class VulkanRenderGraphExecutor;
    class VulkanFrameTargets;
    class VulkanReflectionProbeCaptureTargets;
    class VulkanDepthPyramid;
    class VulkanResourceRegistry;
    struct VulkanImageResource;
    struct VulkanIndirectScene;

    // Points where the backend hands an open command buffer to its extensions.
    // A hook with a graph pass is declared by VulkanGraphHooks; when declared,
    // the backend begins the pass if any extension wants the hook this frame
    // and skips it otherwise.
    enum class VulkanHookPoint : uint8_t {
        // Scene-linear color is complete (after transparency). The backend
        // brackets the hook with the out-of-plan scene.color TransferSource
        // transition.
        SceneColorComplete,
        // Display output is complete; runs inside "final-capture-hook".
        FinalCaptureHook,
        DepthPyramidValidation,     // "depth.occlusion-pyramid.validation-readback-hook"
        Ordinary2Validation,        // "transparent.layered.validation-readback-hook"
        DeepLayeredValidation,      // "transparent.layered.<tier>.validation-readback-hook"
        DeepLayeredResolveCounts,   // notification only, after the deep scene resolve
        VirtualShadowDepthSnapshot, // inside "shadow.virtual.request-readback"
        // Notification only (M7R R4b.5): the frame's command buffer is open
        // and no graph pass has begun (qualification alias-heap poison).
        FrameGraphBegin,
    };

    // Stable backend objects an extension may use between
    // onBackendInitialized and onBeforeDeviceDestroy.
    struct VulkanBackendServices {
        VkDevice device = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator = nullptr;
        VulkanFrameScheduler* scheduler = nullptr;
        VulkanRenderGraphExecutor* graph = nullptr;
        VulkanFrameTargets* frameTargets = nullptr;
        VulkanReflectionProbeCaptureTargets* probeCaptureTargets = nullptr;
        // Null unless the depth pyramid is enabled for this process.
        VulkanDepthPyramid* depthPyramid = nullptr;
        CpuProfiler* profiler = nullptr;
    };

    struct VulkanCaptureHookPayload {
        FrameCapturePoint point = FrameCapturePoint::SceneLinear;
        const VulkanImageResource* source = nullptr;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    struct VulkanOrdinary2HookPayload {
        VkExtent2D atlasExtent{};
        uint32_t drawCount = 0;
        uint32_t workItemCount = 0;
    };
    struct VulkanDeepLayeredHookPayload {
        TransparencyQuality quality = TransparencyQuality::Hero4;
        uint32_t interfaceCount = 0;
        uint32_t drawCount = 0;
        uint32_t workItemCount = 0;
    };
    struct VulkanDeepResolveCountsPayload {
        std::array<uint32_t, kLayeredQualityTierCount> sceneResolveDrawCounts{};
    };
    struct VulkanDepthPyramidHookPayload {
        uint32_t retainedView = 0;
    };
    struct VulkanVirtualShadowDepthPayload {
        const VulkanImageResource* depth = nullptr;
        VkExtent2D extent{};
        glm::mat4 inverseViewProjection{ 1.0f };
    };
    using VulkanHookPayload = std::variant<std::monostate,
        VulkanCaptureHookPayload, VulkanOrdinary2HookPayload,
        VulkanDeepLayeredHookPayload, VulkanDeepResolveCountsPayload,
        VulkanDepthPyramidHookPayload, VulkanVirtualShadowDepthPayload>;

    struct VulkanHookContext {
        VulkanHookPoint point = VulkanHookPoint::SceneColorComplete;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t slot = 0;
        VulkanHookPayload payload{};
    };

    // ---------------------------------------------------------------------
    // Indirect / LOD / occlusion / VSM qualification oracle (R2.8).
    //
    // The backend keeps device telemetry, GPU work and the expectation
    // emission sites (behind `if constexpr (kQualificationBuild)` and a null
    // check); the oracle owns the expected state and the comparisons.
    // ---------------------------------------------------------------------
    enum class VulkanIndirectOracleView : uint8_t {
        DirectionalShadow,
        SpotShadow,
        PointShadow,
        ReflectionProbe,
        OpaqueLod,           // main-view GPU LOD command oracle
        DepthOcclusion,      // CPU projection + independent device query
        VirtualShadowDepth,  // full-depth VSM request/coverage oracle
    };
    inline constexpr uint32_t kVulkanShadowOracleViewCount = 4u;

    // Device readback of one shadow/probe consumer's compaction, in the
    // count-region layout the backend recorded for the slot.
    struct VulkanIndirectReadback {
        const uint32_t* counts = nullptr;
        const GpuSceneIndexedIndirectCommand* commands = nullptr;
        std::span<const uint32_t> countCapacities{};
        std::span<const uint32_t> commandOffsets{};
        std::span<const GpuScenePrimitiveRecord> primitives{};
        std::span<const GpuSceneGeometryRecord> geometries{};
    };

    struct VulkanIndirectOracleResult {
        bool validated = false;
        uint64_t oracleCommands = 0;
        uint64_t mismatchedBins = 0;
        uint64_t oracleTriangles = 0;
        uint64_t oracleReducedCommands = 0;
        uint64_t mismatchedCommandRegions = 0;
    };

    struct VulkanOpaqueLodExpectation {
        GpuSceneIndexedIndirectCommand command{};
        uint64_t baseTriangles = 0;
        uint64_t oracleTriangles = 0;
        bool reduced = false;
        bool historyValid = false;
        bool historyChanged = false;
    };

    struct VulkanOpaqueLodVerdict {
        bool active = false;
        uint64_t deviceTriangles = 0;
        uint64_t mismatchedCommands = 0;
        uint64_t baseTriangles = 0;
        uint64_t oracleTriangles = 0;
        uint64_t oracleReducedCommands = 0;
        uint64_t historyValid = 0;
        uint64_t historyReset = 0;
        uint64_t historyChanged = 0;
    };

    // CPU-projected depth-occlusion queries (qualification only).
    struct VulkanOcclusionQueryVerdict {
        bool active = false;
        uint32_t queryCount = 0;
        uint32_t projectionRejected = 0;
        uint64_t tested = 0;
        uint64_t wouldReject = 0;
        uint64_t invalidResults = 0;
    };

    struct VulkanVirtualShadowOracleInput {
        uint32_t slot = 0;
        std::span<const DirectionalVirtualShadowClipLevel> levels{};
        uint32_t pageSizeTexels = 0;
        uint32_t requestCapacity = 0;
        uint32_t uniquePagesBeforeCapacity = 0;
        uint32_t outputRequestCount = 0;
        uint32_t requestCapacityOverflow = 0;
        uint32_t requestCapacityDroppedSamples = 0;
    };

    class IVulkanIndirectOracle {
    public:
        [[nodiscard]] virtual bool enabled(
            VulkanIndirectOracleView view) const noexcept = 0;

        // Shadow and probe consumers: one batch of count regions per slot.
        virtual void beginShadowWork(VulkanIndirectOracleView view,
            uint32_t slot, size_t countRegionCount) = 0;
        virtual void expectShadowCommand(VulkanIndirectOracleView view,
            uint32_t slot, size_t countIndex,
            const GpuSceneIndexedIndirectCommand& command) = 0;
        // Consumes the slot's batch; `validated` is false when none was begun.
        [[nodiscard]] virtual VulkanIndirectOracleResult verifyShadowWork(
            VulkanIndirectOracleView view, uint32_t slot,
            const VulkanIndirectReadback& readback) = 0;

        // Opaque main view: GPU LOD commands and depth-occlusion qualification.
        virtual void beginOpaqueWork(uint32_t slot, uint32_t primitiveCount,
            uint32_t candidateCount, bool lod, bool occlusion) = 0;
        virtual void expectOpaqueVisibleCandidate(uint32_t slot,
            uint32_t candidateIndex) = 0;
        virtual void expectOpaqueOcclusionQuery(uint32_t slot,
            uint32_t candidateIndex) = 0;
        virtual void expectOpaqueProjectionRejected(uint32_t slot) = 0;
        virtual void expectOpaqueLod(uint32_t slot, uint32_t primitiveIndex,
            const VulkanOpaqueLodExpectation& expectation) = 0;
        // Collection, in this order, from the slot's retired readbacks.
        virtual void verifyOpaqueLodCommands(uint32_t slot,
            const uint32_t* counts, std::span<const uint32_t> binCapacities,
            const GpuSceneIndexedIndirectCommand* commands) = 0;
        [[nodiscard]] virtual VulkanOcclusionQueryVerdict
            verifyOcclusionQueries(uint32_t slot,
                const DepthPyramidDeviceResult* results) = 0;
        [[nodiscard]] virtual bool opaqueCandidateCpuVisible(uint32_t slot,
            uint32_t candidateIndex) const noexcept = 0;
        // Returns true when an occluded fused GPU-scene result rejected
        // CPU-visible work that the independent CPU query did not occlude.
        [[nodiscard]] virtual bool unsafeGpuSceneOcclusion(uint32_t slot,
            uint32_t candidateIndex) const noexcept = 0;
        // A fused rejection removed this primitive's command; false when the
        // primitive is outside the LOD expectation table.
        [[nodiscard]] virtual bool rejectOpaqueLodPrimitive(uint32_t slot,
            uint32_t primitiveIndex) = 0;
        [[nodiscard]] virtual VulkanOpaqueLodVerdict finishOpaqueLod(
            uint32_t slot) = 0;

        // M7.8 VSM depth oracle. The depth snapshot is the
        // VirtualShadowDepthSnapshot hook; these compare the retired slot.
        // beginVirtualShadowVerify returns false when the slot has no
        // expectation, throws on telemetry mismatch.
        [[nodiscard]] virtual bool beginVirtualShadowVerify(
            const VulkanVirtualShadowOracleInput& input) = 0;
        virtual void verifyVirtualShadowRequest(uint32_t requestIndex,
            const VirtualShadowPageRequest& request) = 0;
        [[nodiscard]] virtual uint64_t comparedVirtualShadowRequests()
            const noexcept = 0;

    protected:
        ~IVulkanIndirectOracle() = default;
    };

    // ---------------------------------------------------------------------
    // Indirect command-stream digest (M7R R3a.0, qualification only).
    //
    // Observes every GPU-driven compaction the backend records: the ordered
    // compute dispatch log, the host-written candidate/count bytes, the
    // indirect draws that consume the result and, once the slot's fence has
    // retired, the device-written command regions. A stream is one view's
    // compaction work for one frame slot; it begins when the host writes are
    // committed and retires when the backend collects the slot. The backend
    // reports the same values it passes to vkCmd*; it never changes what it
    // records because an observer is attached.
    // ---------------------------------------------------------------------
    enum class VulkanIndirectStreamView : uint8_t {
        DirectionalShadow,
        SpotShadow,
        PointShadow,
        ReflectionProbe,
        Opaque,
    };
    inline constexpr uint32_t kVulkanIndirectStreamViewCount = 5u;

    // Host-written regions of a stream's candidate/count/query buffers.
    enum class VulkanIndirectStreamHostTarget : uint8_t {
        Candidates,
        Counts,
        OcclusionQueries,
    };

    // Device readback of one retired stream, in its count-region layout:
    // count region i holds counts[i] commands (at most countCapacities[i])
    // starting at command index commandOffsets[i]. Empty commandOffsets means
    // the regions are packed back to back (prefix sums of the capacities).
    // The slot's GPU-scene tables let a command's primitive slot (its
    // firstInstance) be identified by content rather than by slot index.
    struct VulkanIndirectStreamReadback {
        const uint32_t* counts = nullptr;
        const GpuSceneIndexedIndirectCommand* commands = nullptr;
        std::span<const uint32_t> countCapacities{};
        std::span<const uint32_t> commandOffsets{};
        std::span<const GpuScenePrimitiveRecord> primitives{};
        std::span<const GpuSceneInstanceRecord> instances{};
        std::span<const GpuSceneAffineTransform> transforms{};
    };

    class IVulkanIndirectStreamObserver {
    public:
        using View = VulkanIndirectStreamView;

        // The stream's own indirect command and count buffers; draws that
        // consume them are recorded by role, not by handle.
        virtual void beginStream(View view, uint32_t slot,
            VkBuffer commandBuffer, VkBuffer countBuffer) = 0;
        virtual void hostWrite(View view, uint32_t slot,
            VulkanIndirectStreamHostTarget target,
            std::span<const std::byte> bytes) = 0;
        virtual void gpuRange(View view, uint32_t slot, const char* name) = 0;
        virtual void barrier(View view, uint32_t slot,
            VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess) = 0;
        virtual void bindPipeline(View view, uint32_t slot,
            VkPipelineBindPoint bindPoint, VkPipeline pipeline) = 0;
        virtual void bindDescriptorSets(View view, uint32_t slot,
            VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
            uint32_t firstSet, std::span<const VkDescriptorSet> sets,
            std::span<const uint32_t> dynamicOffsets) = 0;
        virtual void pushConstants(View view, uint32_t slot,
            VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
            std::span<const std::byte> bytes) = 0;
        virtual void dispatch(View view, uint32_t slot, uint32_t groupsX,
            uint32_t groupsY, uint32_t groupsZ) = 0;
        // Work recorded by a helper on the stream's behalf (depth-occlusion
        // query dispatches, the fused-occlusion history descriptor write):
        // a stable tag and its arguments.
        virtual void note(View view, uint32_t slot, uint32_t tag,
            std::span<const uint64_t> values) = 0;
        virtual void indirectDraw(View view, uint32_t slot, VkPipeline pipeline,
            VkBuffer vertexBuffer, VkBuffer indexBuffer, VkIndexType indexType,
            uint32_t pushWord, VkBuffer commandBuffer,
            VkDeviceSize commandOffset, VkBuffer countBuffer,
            VkDeviceSize countOffset, uint32_t maxDrawCount) = 0;
        virtual void retireStream(View view, uint32_t slot,
            const VulkanIndirectStreamReadback& readback) = 0;

    protected:
        ~IVulkanIndirectStreamObserver() = default;
    };

    // One stream's recording-site view of the observer: every call is a no-op
    // without one, so emission sites stay one line next to their vkCmd* call.
    struct VulkanIndirectStreamTap {
        IVulkanIndirectStreamObserver* observer = nullptr;
        VulkanIndirectStreamView view = VulkanIndirectStreamView::DirectionalShadow;
        uint32_t slot = 0;

        void begin(VkBuffer commandBuffer, VkBuffer countBuffer) const {
            if (observer) observer->beginStream(view, slot, commandBuffer,
                countBuffer);
        }
        void hostWrite(VulkanIndirectStreamHostTarget target, const void* data,
            size_t size) const {
            if (observer) observer->hostWrite(view, slot, target,
                { static_cast<const std::byte*>(data), size });
        }
        void gpuRange(const char* name) const {
            if (observer) observer->gpuRange(view, slot, name);
        }
        void barrier(VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess) const {
            if (observer) observer->barrier(view, slot, srcStages, dstStages,
                srcAccess, dstAccess);
        }
        void bindPipeline(VkPipelineBindPoint bindPoint, VkPipeline pipeline) const {
            if (observer) observer->bindPipeline(view, slot, bindPoint, pipeline);
        }
        void bindDescriptorSets(VkPipelineBindPoint bindPoint,
            VkPipelineLayout layout, uint32_t firstSet,
            std::span<const VkDescriptorSet> sets,
            std::span<const uint32_t> dynamicOffsets = {}) const {
            if (observer) observer->bindDescriptorSets(view, slot, bindPoint,
                layout, firstSet, sets, dynamicOffsets);
        }
        void pushConstants(VkPipelineLayout layout, VkShaderStageFlags stages,
            uint32_t offset, const void* data, size_t size) const {
            if (observer) observer->pushConstants(view, slot, layout, stages,
                offset, { static_cast<const std::byte*>(data), size });
        }
        void dispatch(uint32_t x, uint32_t y, uint32_t z) const {
            if (observer) observer->dispatch(view, slot, x, y, z);
        }
        void note(uint32_t tag, std::initializer_list<uint64_t> values) const {
            if (observer) observer->note(view, slot, tag,
                { values.begin(), values.size() });
        }
        void indirectDraw(VkPipeline pipeline, VkBuffer vertexBuffer,
            VkBuffer indexBuffer, VkIndexType indexType, uint32_t pushWord,
            VkBuffer commandBuffer, VkDeviceSize commandOffset,
            VkBuffer countBuffer, VkDeviceSize countOffset,
            uint32_t maxDrawCount) const {
            if (observer) observer->indirectDraw(view, slot, pipeline,
                vertexBuffer, indexBuffer, indexType, pushWord, commandBuffer,
                commandOffset, countBuffer, countOffset, maxDrawCount);
        }
        void retire(const VulkanIndirectStreamReadback& readback) const {
            if (observer) observer->retireStream(view, slot, readback);
        }
    };

    // ---------------------------------------------------------------------
    // Caster-revision equivalence (M7R R5c.1/R5c.2, qualification only).
    //
    // The backend reports every evaluation of its change-driven caster
    // revisions with the inputs the retired per-frame FNV-1a hashes read, so
    // an oracle can recompute those hashes and compare change frames. The
    // backend's revisions do not depend on whether an observer is attached.
    // ---------------------------------------------------------------------
    enum class VulkanCasterRevisionStream : uint8_t {
        Shadow,             // the shadow submission's sequence revision
        DirectionalShadow,  // one revision per cascade of one directional light
        DepthHistory,       // the main view's depth-content revision
        // M7R R5c.5: the published shadow and probe membership revisions
        // (revisions[0] shadow, revisions[1] probe), once per publication.
        Membership,
    };

    struct VulkanCasterRevisionSample {
        VulkanCasterRevisionStream stream = VulkanCasterRevisionStream::Shadow;
        // DirectionalShadow: the light's evaluation order within the frame.
        uint32_t ordinal = 0;
        // The render-submission serial of the frame being recorded.
        uint64_t frameSerial = 0;
        const VulkanIndirectScene* scene = nullptr;
        const VulkanResourceRegistry* resources = nullptr;
        // Shadow and DirectionalShadow.
        const ShadowCasterSubmission* casters = nullptr;
        // DirectionalShadow.
        const DirectionalShadowCascadePlan* plan = nullptr;
        // DepthHistory.
        std::span<const DrawPacket> opaqueQueue{};
        std::span<const DrawPacket> forwardQueue{};
        // Membership: the published tables.
        const GpuScenePackedTables* tables = nullptr;
        // One value, or one per cascade.
        std::span<const uint64_t> revisions{};
    };

    class IVulkanCasterRevisionObserver {
    public:
        virtual void observeCasterRevision(
            const VulkanCasterRevisionSample& sample) = 0;

    protected:
        ~IVulkanCasterRevisionObserver() = default;
    };

    class IVulkanBackendExtension : public IRenderBackendExtension {
    public:
        [[nodiscard]] RenderBackendApi api() const noexcept final {
            return RenderBackendApi::Vulkan;
        }

        // Start of init(), before any graph is built or resource allocated.
        virtual void configure(const RenderBackendConfig& config) { (void)config; }
        // Hook passes this extension needs declared. Read whenever the
        // production graph is rebuilt; must be stable for the process.
        [[nodiscard]] virtual VulkanGraphHooks graphHooks() const noexcept {
            return VulkanGraphHooks::none();
        }
        // End of init(); services stay valid until onBeforeDeviceDestroy.
        virtual void onBackendInitialized(const VulkanBackendServices& services) {
            (void)services;
        }
        [[nodiscard]] virtual bool wantsHook(
            const VulkanHookContext& context) const { (void)context; return false; }
        // For pass hooks the backend has begun the graph pass and GPU range.
        virtual void onHook(const VulkanHookContext& context) { (void)context; }
        // The slot's fence has retired (beginFrame, including the out-of-date
        // acquire path); its readbacks are CPU-safe.
        virtual void onFrameSlotRetired(uint32_t slot) { (void)slot; }
        [[nodiscard]] virtual IVulkanIndirectOracle* indirectOracle() noexcept {
            return nullptr;
        }
        // Non-null only while a command-stream digest is requested (fixed
        // before the backend is created).
        [[nodiscard]] virtual IVulkanIndirectStreamObserver*
            indirectStreamObserver() noexcept {
            return nullptr;
        }
        // Non-null only while the caster-revision oracle is requested (fixed
        // before the backend is created).
        [[nodiscard]] virtual IVulkanCasterRevisionObserver*
            casterRevisionObserver() noexcept {
            return nullptr;
        }
        // The editor UI contributor (M7R R3c.10, renderer/vulkan_imgui); the
        // first extension providing one serves it.
        [[nodiscard]] virtual IVulkanEditorUi* editorUi() noexcept {
            return nullptr;
        }
        // After the device is idle and the oracles have drained, before any
        // backend resource is destroyed.
        virtual void onBeforeDeviceDestroy() {}
    };

} // namespace Iridium
