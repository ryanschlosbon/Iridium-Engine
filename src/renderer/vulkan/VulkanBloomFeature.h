#pragma once

// M9.4 bloom: the feature owner of "post.bloom" (compute) and its transient
// mipped chain image "bloom.chain" (RGBA16F, half the scene extent, one mip
// per level).
//
// The pass reads the resolved scene colour (TAA current or scene.color) and
// runs a dual-filter chain (Jimenez, "Next Generation Post Processing in Call
// of Duty: Advanced Warfare", 2014) entirely inside one graph pass:
// - Downsample: level 0 from the scene, level i from level i-1, with the
//   13-tap filter. Level 0 averages its five 2x2 boxes with Karis (1 / (1 +
//   luma)) weights against fireflies, sanitises non-finite input and applies
//   the optional soft-knee threshold.
// - Upsample: level i += tent3x3(level i+1), from the coarsest level back up;
//   the last step scales level 0 by 1 / levels, so the chain holds the mean of
//   the blurred levels (each carries the scene's energy).
// Every dispatch but the last is followed by a memory barrier inside the pass
// (ADR-0016 item 6, as the refraction and depth pyramids). Every texel of
// every mip is written before it is read, so the chain is declared a
// whole-resource write and may alias.
//
// The output transform samples level 0 (binding 6) with a 4-tap tent and
// composites before exposure: lerp(scene, bloom, intensity) without a
// threshold (energy-conserving), scene + intensity * bloom with one.
//
// Disabled, nothing is declared and nothing is created; "bloom-hook" keeps
// its place (the M7R topology), and the output binds a fallback it never reads.

#include "VulkanExposureFeature.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanRenderGraphExecutor.h"

#include "renderer/rhi/RenderBackendConfig.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace Iridium {

    inline constexpr uint32_t BloomMaximumLevels = 8;

    // Level 0 is half the scene extent, rounded up.
    [[nodiscard]] constexpr uint32_t bloomChainSize(uint32_t sceneSize) noexcept {
        return (std::max)((sceneSize + 1u) / 2u, 1u);
    }
    // The requested levels (1..8), limited to the chain's full mip count.
    [[nodiscard]] constexpr uint32_t bloomChainLevels(uint32_t sceneWidth,
        uint32_t sceneHeight, uint32_t requested) noexcept {
        uint32_t largest = (std::max)(bloomChainSize(sceneWidth), bloomChainSize(sceneHeight));
        uint32_t available = 0;
        while (largest != 0u) {
            ++available;
            largest >>= 1u;
        }
        return (std::min)((std::clamp)(requested, 1u, BloomMaximumLevels), available);
    }

    class VulkanBloomFeature final : public IVulkanFeature {
    public:
        // What the output transform composites with (staged per frame).
        struct Composite {
            float intensity = 0.0f;
            bool additive = false;   // a threshold is set
        };

        VulkanBloomFeature() = default;
        VulkanBloomFeature(const VulkanBloomFeature&) = delete;
        VulkanBloomFeature& operator=(const VulkanBloomFeature&) = delete;

        // Before create(); also between frames (enabled changes need a graph
        // rebuild, which the backend performs; the rest applies next frame).
        void configure(const BloomSettings& settings) noexcept { settings_ = settings; }
        // The exposure that puts the threshold in exposed units: the adapted
        // state in Auto mode, the manual multiplier otherwise.
        void setExposureSource(const VulkanExposureFeature* exposure) noexcept {
            exposure_ = exposure;
        }
        [[nodiscard]] const BloomSettings& settings() const noexcept { return settings_; }
        // The chain levels the graph declares (0: no bloom).
        [[nodiscard]] uint32_t graphLevels() const noexcept {
            return settings_.enabled ? (std::clamp)(settings_.levels, 1u, BloomMaximumLevels) : 0u;
        }
        [[nodiscard]] Composite composite() const noexcept {
            return { settings_.intensity, settings_.threshold > 0.0f };
        }

        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void onGraphReleased() override;
        void destroy() noexcept override;

        [[nodiscard]] bool active() const noexcept { return bloomPass_.isValid(); }

    private:
        static void execute(void* owner, VulkanPassContext& context);
        void createPipeline();
        void destroyPipeline() noexcept;
        void releaseViews() noexcept;

        const VulkanFeatureContext* context_ = nullptr;
        BloomSettings settings_{};
        bool temporalResolve_ = false;
        const VulkanExposureFeature* exposure_ = nullptr;
        RenderGraph::GraphResourceId exposureState_{};
        VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        // Per frame slot: set i writes level i (i < levels) and reads the
        // scene (i = 0) or the chain; set `levels` writes level 0 from the
        // chain (the last upsample).
        std::array<std::array<VkDescriptorSet, BloomMaximumLevels + 1>,
            VulkanFrameScheduler::FramesInFlight> sets_{};
        std::array<std::array<VkImageView, BloomMaximumLevels>,
            VulkanFrameScheduler::FramesInFlight> levelViews_{};
        RenderGraph::PassId bloomPass_{};
        RenderGraph::GraphResourceId sceneColor_{};
        RenderGraph::GraphResourceId chain_{};
        uint32_t levels_ = 0;
    };

} // namespace Iridium
