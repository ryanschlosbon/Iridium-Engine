#pragma once

// M7R R3c.5: directional shadows as a feature owner. Owns the cascade map
// (render pass, pipelines, per-slot shadow data), its indirect view culler and
// the caster draw loop with the direct fallback, plus the default-off M7.8
// virtual-shadow resources (clip publication, depth-demand marking and the
// request readback). Registers "shadow.virtual.clip-upload",
// "shadow.directional.compact", "shadow.directional" (gpu.shadow.directional
// after its barriers), "shadow.virtual.depth-mark" (gpu.shadow.virtual.
// depth-demand before its barriers) and "shadow.virtual.request-readback".
// The lighting set's shadow bindings stay with its owner
// (VulkanDeferredLightingFeature, R3c.8), which reads them through map().

#include "VulkanDirectionalShadowMap.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanIndirectViewCuller.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanShadowCasters.h"
#include "VulkanVirtualShadowResources.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/VirtualShadowMap.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanShadowFeature final : public IVulkanFeature {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;

        VulkanShadowFeature() = default;
        VulkanShadowFeature(const VulkanShadowFeature&) = delete;
        VulkanShadowFeature& operator=(const VulkanShadowFeature&) = delete;

        // Before create(): map resolution, view settings, the shared caster
        // scratch and the culler setup.
        void configure(uint32_t resolution, const VulkanIndirectViewSettings& settings,
            VulkanCasterScratch& scratch, const VulkanIndirectViewSetup& setup) noexcept {
            resolution_ = resolution;
            settings_ = settings;
            scratch_ = &scratch;
            setup_ = setup;
        }
        // M7.8 VSM (default-off), right after the allocator exists.
        void initVirtualShadows(VkDevice device, VulkanResourceAllocator& allocator,
            const VkPhysicalDeviceLimits& limits,
            const VirtualShadowResourceConfig& config);

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // Drain points (see VulkanFeatureContext.h). submit stages the frame's
        // packets and records the clip upload, compaction and cascades;
        // recordVirtualShadowDemand records the VSM marking and readback
        // after the opaque forward pass.
        void submit(const ShadowCasterSubmission& casters,
            std::span<const DirectionalShadowFramePacket> shadows);
        void recordVirtualShadowDemand();

        // Retired slots: the VSM request readback (and its oracle verdict).
        void collectVirtualShadowRequests(uint32_t slot);
        // Capacity growth (frame boundary; the caller rebinds the graph's
        // imported buffers afterwards).
        void growIndirectCapacity(uint32_t primitiveCapacity);

        [[nodiscard]] const VulkanDirectionalShadowMap& map() const noexcept { return map_; }
        [[nodiscard]] VulkanIndirectViewCuller& culler() noexcept { return culler_; }
        [[nodiscard]] const VulkanVirtualShadowResources& virtualShadows() const noexcept {
            return virtualShadows_;
        }

    private:
        static void executeClipUpload(void* owner, VulkanPassContext& context);
        static bool compactActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeCompact(void* owner, VulkanPassContext& context);
        static bool drawActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeDraw(void* owner, VulkanPassContext& context);
        static void executeDepthMark(void* owner, VulkanPassContext& context);
        static void executeRequestReadback(void* owner, VulkanPassContext& context);

        void publishVirtualShadowClips(const ShadowCasterSubmission& casters,
            std::span<const DirectionalShadowFramePacket> shadows, uint32_t frameIndex);

        const VulkanFeatureContext* context_ = nullptr;
        uint32_t resolution_ = 4096;
        VulkanIndirectViewSettings settings_{};
        VulkanCasterScratch* scratch_ = nullptr;
        VulkanIndirectViewSetup setup_{};
        VulkanDirectionalShadowMap map_;
        VulkanIndirectViewCuller culler_;

        RenderGraph::PassId clipUploadPass_{};
        RenderGraph::PassId compactPass_{};
        RenderGraph::PassId drawPass_{};
        RenderGraph::PassId depthMarkPass_{};
        RenderGraph::PassId requestReadbackPass_{};
        RenderGraph::GraphResourceId workingSet_{};

        // M7.8 virtual shadows.
        VulkanVirtualShadowResources virtualShadows_;
        DirectionalVirtualShadowClipPublisher clipPublisher_;
        std::optional<DirectionalVirtualShadowClipPlan> clipPlan_;
        std::array<std::optional<DirectionalVirtualShadowClipPlan>, FrameCount> frameClipPlans_;
        std::vector<VirtualShadowCasterBounds> casterBoundsScratch_;
        uint32_t clipPageSize_ = 128;
        std::array<VkImageView, FrameCount> depthBindings_{};
        std::array<bool, FrameCount> readbackPending_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        std::span<const DirectionalShadowFramePacket> stagedShadows_{};
        std::array<PackedDirectionalVirtualShadowClipLevel, 16> stagedClips_{};
        bool stagedCompact_ = false;
        bool stagedDraw_ = false;
        bool stagedIndirectValid_ = false;
    };

} // namespace Iridium
