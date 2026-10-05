#pragma once

// M9.2 native temporal anti-aliasing (1:1, DLAA-class): the feature owner of
// the "temporal.taa.resolve" compute pass and its "taa.history" History pair.
// The resolve reads this frame's jittered scene colour, depth and velocity
// plus the pair's previous slot, and writes the resolved scene-linear colour
// into the pair's current slot, which the post chain (bloom, exposure,
// output transform) reads as the frame's scene colour.
//
// History images change per frame (parity) and per retained view (set), so
// the per-frame-slot descriptor set is rewritten inside the execute callback,
// after the frame's view selected its history set and before the dispatch.

#include "VulkanFeatureContext.h"
#include "VulkanRenderGraphExecutor.h"

#include "renderer/rhi/TemporalUpscaleInputs.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace Iridium {

    class VulkanTemporalAntiAliasingFeature final : public IVulkanFeature {
    public:
        // The frame's resolve request: the vendor-neutral contract every
        // temporal provider consumes (renderer/rhi/TemporalUpscaleInputs.h),
        // plus this backend's view uniforms.
        struct FrameInputs {
            TemporalUpscaleInputs request{};
            VkDescriptorSet globalSet = VK_NULL_HANDLE;
            // M9.5: last frame's adapted exposure state (binding 5, always a
            // valid buffer); the resolve pre-exposes with it when set.
            VkBuffer exposureState = VK_NULL_HANDLE;
            bool exposureFromState = false;
        };

        VulkanTemporalAntiAliasingFeature() = default;
        VulkanTemporalAntiAliasingFeature(const VulkanTemporalAntiAliasingFeature&) = delete;
        VulkanTemporalAntiAliasingFeature& operator=(
            const VulkanTemporalAntiAliasingFeature&) = delete;

        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void onGraphReleased() override;
        void destroy() noexcept override;

        // Before the frame's first drain that reaches the resolve.
        void stage(const FrameInputs& inputs) noexcept { staged_ = inputs; }
        [[nodiscard]] bool active() const noexcept { return resolvePass_.isValid(); }
        // Whether this frame's resolve found valid history (after it ran).
        [[nodiscard]] bool historyWasValid() const noexcept { return lastHistoryValid_; }

    private:
        static void executeResolve(void* owner, VulkanPassContext& context);
        void writeDescriptors(uint32_t frameIndex, VkImageView previous,
            VkImageView current) const;

        const VulkanFeatureContext* context_ = nullptr;
        VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkSampler linearSampler_ = VK_NULL_HANDLE;
        VkSampler pointSampler_ = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, VulkanFrameScheduler::FramesInFlight> sets_{};
        RenderGraph::PassId resolvePass_{};
        RenderGraph::GraphResourceId historyPrevious_{};
        RenderGraph::GraphResourceId historyCurrent_{};
        RenderGraph::GraphResourceId sceneColor_{};
        RenderGraph::GraphResourceId depth_{};
        RenderGraph::GraphResourceId velocity_{};
        FrameInputs staged_{};
        bool lastHistoryValid_ = false;
    };

} // namespace Iridium
