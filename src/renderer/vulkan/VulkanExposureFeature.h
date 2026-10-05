#pragma once

// M9.5 auto-exposure: the feature owner of "post.exposure.histogram" and
// "post.exposure.adapt" (compute) and of the "exposure" History buffer pair
// (SurviveCut: the adapted state survives view cuts, but not identity,
// extent or topology changes).
//
// - Histogram: 128 log2-luminance bins over the resolved scene colour (AP1
//   scene-linear, AP1 luma). Each workgroup meters one 128x128 tile with
//   shared-memory atomics on integer counts and writes its own row, so the
//   result is order-independent (deterministic) and needs no clear.
// - Adapt (one workgroup): reduces the rows, takes the percentile-clipped
//   mean, clamps it to the EV100 limits and moves the previous adapted EV100
//   toward it at the up/down speeds over the view's time delta. Invalid
//   history (a view's first turn, rebuild, resize) adapts instantly.
//   It writes the state (exposure.current) and a metering summary.
// - Consumers read the state on the GPU only: the output transform (this
//   frame's multiplier) and the TAA resolve (the previous frame's).
//
// With ExposureMode::Manual nothing is declared and nothing is dispatched;
// the feature only owns the fallback state buffer that the output and TAA
// descriptor sets bind (never read: their shaders select the manual EV).

#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanRenderGraphExecutor.h"

#include "renderer/rhi/RenderBackendConfig.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace Iridium {

    inline constexpr uint32_t ExposureHistogramBins = 128;
    // Pixels per side of one histogram workgroup's tile (16x16 threads,
    // 8x8 pixels each).
    inline constexpr uint32_t ExposureHistogramTile = 128;
    // exposure.current/.previous: vec4(adapted log2 scene luminance, output
    // multiplier including compensation, adapted EV100, 1 = written).
    inline constexpr uint64_t ExposureStateBytes = 16;
    // exposure.metering: vec4(metered log2 luminance, target EV100, low and
    // high percentile log2 luminance), uvec4(total weight, bin 0, bin 127,
    // rows), then the 128 summed bins.
    inline constexpr uint64_t ExposureMeteringBytes = 32 + 4 * ExposureHistogramBins;

    [[nodiscard]] constexpr uint32_t exposureHistogramRowCount(
        uint32_t width, uint32_t height) noexcept {
        return ((width + ExposureHistogramTile - 1) / ExposureHistogramTile) *
            ((height + ExposureHistogramTile - 1) / ExposureHistogramTile);
    }

    class VulkanExposureFeature final : public IVulkanFeature {
    public:
        struct FrameInputs {
            float deltaSeconds = 0.0f;     // the view's time since its previous turn
            float compensationEv = 0.0f;   // the manual EV
        };
        // The state a reader of last frame's exposure binds (TAA).
        struct PreviousState {
            VkBuffer buffer = VK_NULL_HANDLE;
            bool valid = false;   // the buffer holds this view's adapted state
        };

        VulkanExposureFeature() = default;
        VulkanExposureFeature(const VulkanExposureFeature&) = delete;
        VulkanExposureFeature& operator=(const VulkanExposureFeature&) = delete;

        // Before create().
        void configure(ExposureMode mode, const AutoExposureSettings& settings) noexcept {
            mode_ = mode;
            settings_ = settings;
        }
        [[nodiscard]] ExposureMode mode() const noexcept { return mode_; }

        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void onGraphReleased() override;
        void destroy() noexcept override;

        // Before the frame's drain that reaches the exposure passes.
        void stage(const FrameInputs& inputs) noexcept { staged_ = inputs; }
        [[nodiscard]] bool active() const noexcept { return adaptPass_.isValid(); }
        // Valid between create and destroy (16 bytes, multiplier 1).
        [[nodiscard]] VkBuffer fallbackState() const noexcept { return fallback_.buffer; }
        // After the frame's view selected its history set and before the
        // adapt pass: last frame's state for this view, or the fallback.
        [[nodiscard]] PreviousState previousState(uint32_t frameIndex) const;

    private:
        static void executeHistogram(void* owner, VulkanPassContext& context);
        static void executeAdapt(void* owner, VulkanPassContext& context);
        void createPipelines();

        const VulkanFeatureContext* context_ = nullptr;
        ExposureMode mode_ = ExposureMode::Manual;
        AutoExposureSettings settings_{};
        VulkanBufferResource fallback_{};
        VkDescriptorSetLayout histogramLayout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout adaptLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout histogramPipelineLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout adaptPipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline histogramPipeline_ = VK_NULL_HANDLE;
        VkPipeline adaptPipeline_ = VK_NULL_HANDLE;
        VkSampler pointSampler_ = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, VulkanFrameScheduler::FramesInFlight> histogramSets_{};
        std::array<VkDescriptorSet, VulkanFrameScheduler::FramesInFlight> adaptSets_{};
        RenderGraph::PassId histogramPass_{};
        RenderGraph::PassId adaptPass_{};
        RenderGraph::GraphResourceId sceneColor_{};
        RenderGraph::GraphResourceId rows_{};
        RenderGraph::GraphResourceId metering_{};
        RenderGraph::GraphResourceId previous_{};
        RenderGraph::GraphResourceId current_{};
        FrameInputs staged_{};
    };

} // namespace Iridium
