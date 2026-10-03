#pragma once

// M7R R3c.7: the main-view opaque surface as a feature owner. Owns the
// G-buffer's fixed wireframe/selection pipelines, the
// main-view opaque culler (VulkanOpaqueIndirectCuller) and the G-buffer draw
// loops (GPU-scene indirect bins, the direct fallback, the editor wireframe
// override and the selection masks), plus the depth pyramid: its history
// eligibility, the executor-owned history import (R3b.9) and the build.
// Registers "gpu-scene.opaque.compact", "gbuffer" (gpu.gbuffer.opaque and
// gpu.gbuffer.selection open inside the rendering instance, as before) and
// "depth.occlusion-pyramid.build" (gpu.depth.occlusion-pyramid before its
// barriers).

#include "renderer/rhi/DepthPyramid.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderDebugView.h"

#include "VkGraphicsPipeline.h"
#include "VulkanDepthPyramid.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanOpaqueIndirectCuller.h"
#include "VulkanRenderGraphExecutor.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace Iridium {

    class VulkanOpaqueFeature final : public IVulkanFeature {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;

        struct Settings {
            GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference;
            bool depthPyramid = false;
            bool depthOcclusionQuery = false;
            bool depthOcclusionRejection = false;
            float lodErrorPixels = 0.0f;       // > 0 enables GPU LOD
            uint32_t lodMaximumLevel = 15u;
            float lodHysteresisFraction = 0.15f;
            bool forceDirectGBufferReference = false;
        };

        // Per-frame inputs (submitOpaqueQueue), valid until the drain.
        struct FrameInputs {
            std::span<const DrawPacket> opaqueQueue{};
            std::span<const DrawPacket> selectionQueue{};
            bool wireframe = false;
            VkDescriptorSet globalSet = VK_NULL_HANDLE;
            RenderDebugView debugView = RenderDebugView::Final;
        };

        VulkanOpaqueFeature() = default;
        VulkanOpaqueFeature(const VulkanOpaqueFeature&) = delete;
        VulkanOpaqueFeature& operator=(const VulkanOpaqueFeature&) = delete;

        // Settings first (backend init); the culler services before create().
        void configure(const Settings& settings) noexcept { settings_ = settings; }
        void setCullerServices(const VulkanCullerServices& services) noexcept {
            services_ = services;
        }

        // IVulkanFeature. create() builds the culler pipelines, the depth
        // pyramid (when enabled) and the G-buffer pass and pipelines.
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        [[nodiscard]] const Settings& settings() const noexcept { return settings_; }
        [[nodiscard]] bool depthPyramidEnabled() const noexcept {
            return settings_.depthPyramid;
        }
        [[nodiscard]] bool lodEnabled() const noexcept {
            return settings_.lodErrorPixels > 0.0f;
        }
        [[nodiscard]] VulkanOpaqueIndirectCuller& culler() noexcept { return culler_; }
        [[nodiscard]] VulkanDepthPyramid& depthPyramid() noexcept { return depthPyramid_; }
        [[nodiscard]] bool historyPrepared() const noexcept { return historyPrepared_; }

        // Frame-target descriptors (the depth pyramid's sets and a fresh
        // history import) after the targets rebuild; cleared before release.
        void rebuildDescriptors();
        void clearDescriptors() noexcept { depthPyramid_.clearDescriptors(); }

        // The editor's retained render view (selects the history image).
        void setRetainedView(uint32_t view) noexcept { retainedView_ = view; }

        // Per frame, in backend order: beginFrame (history decision reset),
        // onFrameFenceCompleted (the retired slot's history publication),
        // bindHistory (follows the retained view), updateView (camera),
        // prepareDepthHistory (required before submit when the pyramid is
        // enabled), submit (the "gpu-scene.opaque.compact" and "gbuffer"
        // drain point) and recordDepthPyramid (the build's drain
        // point, after the refraction pyramids).
        void beginFrame() noexcept {
            historyPrepared_ = false;
            historyDecision_ = {};
        }
        void onFrameFenceCompleted(uint32_t frameIndex, uint64_t completedSerial);
        void bindHistory(bool reset);
        void updateView(const ViewTransportRecord& view, ViewHistoryContext history);
        void publishScene(const GpuScenePackedTables& scene);
        void prepareDepthHistory(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> opaqueForwardQueue);
        void submit(const FrameInputs& inputs);
        void recordDepthPyramid();

    private:
        static bool compactActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeCompact(void* owner, VulkanPassContext& context);
        static void executeGBuffer(void* owner, VulkanPassContext& context);
        static void executeDepthPyramid(void* owner, VulkanPassContext& context);
        void recordGBuffer(VulkanPassContext& context);
        [[nodiscard]] DepthPyramidHistoryOwner historyOwner() const noexcept;

        const VulkanFeatureContext* context_ = nullptr;
        Settings settings_{};
        VulkanCullerServices services_{};
        std::unique_ptr<VkGraphicsPipeline> gBufferPipeline_;
        VulkanOpaqueIndirectCuller culler_;
        VulkanDepthPyramid depthPyramid_;

        RenderGraph::PassId compactPass_{};
        RenderGraph::PassId gBufferPassId_{};
        RenderGraph::PassId depthPyramidPass_{};
        RenderGraph::GraphResourceId historyImport_{};

        // Depth-pyramid history (R3b.9): the import follows the retained view;
        // each image keeps its tracked state while unbound.
        uint32_t retainedView_ = 0;
        uint32_t historyBoundView_ = UINT32_MAX;
        std::array<RenderGraph::Access, VulkanDepthPyramid::HistoryViewCount>
            historyAccess_{};
        ViewHistoryContext viewHistory_{};
        uint64_t projectionRevision_ = 1;
        uint64_t depthContentRevision_ = 1;
        DepthPyramidHistoryDecision historyDecision_{};
        bool historyPrepared_ = false;

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        FrameInputs staged_{};
        bool stagedCompact_ = false;
        bool stagedIndirectValid_ = false;
    };

} // namespace Iridium
