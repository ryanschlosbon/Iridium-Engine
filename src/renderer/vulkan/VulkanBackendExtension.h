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
#include "renderer/rhi/Ordinary2CaptureValidation.h"
#include "renderer/rhi/VirtualShadowMap.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "core/types/FrameCapture.h"
#include "VulkanProductionRenderGraph.h"

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace Iridium {

    class CpuProfiler;
    class VulkanResourceAllocator;
    class VulkanFrameScheduler;
    class VulkanRenderGraphExecutor;
    class VulkanFrameTargets;
    class VulkanReflectionProbeCaptureTargets;
    class VulkanDepthPyramid;
    struct VulkanImageResource;

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

    // The backend's frame-recording state at a request or hook.
    struct VulkanFrameRecording {
        bool open = false;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t slot = 0;
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

    // Temporary forwarding target for the eight capture/oracle requests that
    // are still on IRenderBackend (M7R R2.7). R2.9 deletes these methods and
    // this interface; the harness then arms the extension directly.
    class IVulkanLegacyQualificationRequests {
    public:
        virtual void captureCurrentFrame(uint64_t captureId,
            const VulkanFrameRecording& frame,
            const VulkanCaptureHookPayload& source) = 0;
        [[nodiscard]] virtual std::vector<FrameCapture> collectFrameCaptures(
            bool frameOpen, bool waitForPending) = 0;
        virtual void requestOrdinary2CaptureValidation(uint64_t validationId,
            const VulkanFrameRecording& frame) = 0;
        [[nodiscard]] virtual std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool frameOpen,
                bool waitForPending) = 0;
        virtual void requestDeepLayeredCaptureValidation(uint64_t validationId,
            TransparencyQuality quality, const VulkanFrameRecording& frame) = 0;
        [[nodiscard]] virtual std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool frameOpen,
                bool waitForPending) = 0;
        virtual void requestDepthPyramidCaptureValidation(
            uint64_t validationId, const VulkanFrameRecording& frame) = 0;
        [[nodiscard]] virtual std::vector<DepthPyramidCaptureValidationResult>
            collectDepthPyramidCaptureValidations(bool frameOpen,
                bool waitForPending) = 0;

    protected:
        ~IVulkanLegacyQualificationRequests() = default;
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
        [[nodiscard]] virtual IVulkanLegacyQualificationRequests*
            legacyQualificationRequests() noexcept { return nullptr; }
        // After the device is idle and the oracles have drained, before any
        // backend resource is destroyed.
        virtual void onBeforeDeviceDestroy() {}
    };

} // namespace Iridium
