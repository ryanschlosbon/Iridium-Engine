#pragma once

// M7R R3c.5: spot and point shadows as a feature owner. Owns the spot atlas
// and the point cube pools (render passes, pipelines, per-slot shadow data),
// their indirect view cullers and the caster draw loops with the direct
// fallback. Registers "shadow.spot.compact", "shadow.spot" (gpu.shadow.spot),
// "shadow.point.compact" and "shadow.point" (gpu.shadow.point), each drawing
// range after its barriers. The lights' shadow-data slot mapping is published
// to the clustered-light owner; the lighting set's shadow bindings stay with
// its owner (the backend until R3c.8), which reads them through spot()/point().

#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanIndirectViewCuller.h"
#include "VulkanPointShadowPools.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanShadowCasters.h"
#include "VulkanSpotShadowAtlas.h"
#include "renderer/rhi/IRenderBackend.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanClusterLightingFeature;

    class VulkanLocalShadowFeature final : public IVulkanFeature {
    public:
        VulkanLocalShadowFeature() = default;
        VulkanLocalShadowFeature(const VulkanLocalShadowFeature&) = delete;
        VulkanLocalShadowFeature& operator=(const VulkanLocalShadowFeature&) = delete;

        // Before create(): atlas resolution, point pool capacities, view
        // settings, the shared caster scratch and the culler setup.
        void configure(uint32_t spotAtlasResolution,
            const std::array<uint32_t, 3>& pointPoolCapacities,
            const VulkanIndirectViewSettings& settings, VulkanCasterScratch& scratch,
            const VulkanIndirectViewSetup& setup) noexcept {
            spotAtlasResolution_ = spotAtlasResolution;
            pointPoolCapacities_ = pointPoolCapacities;
            settings_ = settings;
            scratch_ = &scratch;
            setup_ = setup;
        }

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // Drain points (see VulkanFeatureContext.h): stage the frame's packets,
        // publish the lights' shadow-data slots, record compaction and tiles
        // or faces.
        void submitSpot(const ShadowCasterSubmission& casters,
            std::span<const SpotShadowFramePacket> shadows,
            VulkanClusterLightingFeature& clusters);
        void submitPoint(const ShadowCasterSubmission& casters,
            std::span<const PointShadowFramePacket> shadows,
            VulkanClusterLightingFeature& clusters);

        // Capacity growth (frame boundary; the caller rebinds the graph's
        // imported buffers afterwards), spot then point.
        void growIndirectCapacity(uint32_t primitiveCapacity);

        [[nodiscard]] const VulkanSpotShadowAtlas& spot() const noexcept { return spot_; }
        [[nodiscard]] const VulkanPointShadowPools& point() const noexcept { return point_; }
        [[nodiscard]] VulkanIndirectViewCuller& spotCuller() noexcept { return spotCuller_; }
        [[nodiscard]] VulkanIndirectViewCuller& pointCuller() noexcept { return pointCuller_; }

    private:
        static bool spotCompactActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeSpotCompact(void* owner, VulkanPassContext& context);
        static bool spotDrawActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeSpotDraw(void* owner, VulkanPassContext& context);
        static bool pointCompactActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executePointCompact(void* owner, VulkanPassContext& context);
        static bool pointDrawActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executePointDraw(void* owner, VulkanPassContext& context);

        [[nodiscard]] bool planCuller(VulkanIndirectViewCuller& culler,
            const ShadowCasterSubmission& casters,
            const IndirectWorkEnumeration& work, uint32_t frameIndex);

        const VulkanFeatureContext* context_ = nullptr;
        uint32_t spotAtlasResolution_ = 8192;
        std::array<uint32_t, 3> pointPoolCapacities_{};
        VulkanIndirectViewSettings settings_{};
        VulkanCasterScratch* scratch_ = nullptr;
        VulkanIndirectViewSetup setup_{};
        VulkanSpotShadowAtlas spot_;
        VulkanPointShadowPools point_;
        VulkanIndirectViewCuller spotCuller_;
        VulkanIndirectViewCuller pointCuller_;
        std::vector<uint32_t> spotMappingScratch_;
        std::vector<uint32_t> pointMappingScratch_;

        RenderGraph::PassId spotCompactPass_{};
        RenderGraph::PassId spotDrawPass_{};
        RenderGraph::PassId pointCompactPass_{};
        RenderGraph::PassId pointDrawPass_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        struct Staged {
            bool compact = false;
            bool draw = false;
            bool indirectValid = false;
        };
        std::span<const SpotShadowFramePacket> stagedSpot_{};
        std::span<const PointShadowFramePacket> stagedPoint_{};
        Staged spotStaged_{};
        Staged pointStaged_{};
    };

} // namespace Iridium
