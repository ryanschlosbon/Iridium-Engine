#pragma once

// M7R R3c.2: the single scene-linear -> display output transform as a feature
// owner. Owns the output-transform pass and pipeline, the HDR10 encode pass,
// the ACES 2 LUT binding, the manual exposure and output operator, and the
// viewport grid overlay. Registers "bloom-hook" (always skipped until M9
// bloom), "output-transform" and, for HDR10 composition,
// "hdr10-encode-present". The swapchain, transport selection and paper-white
// and peak luminance stay with the backend (they also drive the swapchain
// metadata and the editor bridge's display colour) and are staged per frame.

#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/ViewportGridOverlay.h"

#include "VulkanFeatureContext.h"
#include "VulkanHdrEncodePass.h"
#include "VulkanOutputPass.h"
#include "VulkanRenderGraphExecutor.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace Iridium {

    class VulkanOutputFeature final : public IVulkanFeature {
    public:
        struct FrameInputs {
            Color::OutputTransport transport = Color::OutputTransport::SdrSrgb;
            float paperWhiteNits = 203.0f;
            float peakNits = 1000.0f;
            bool selectionOutline = false;
        };

        VulkanOutputFeature() = default;
        VulkanOutputFeature(const VulkanOutputFeature&) = delete;
        VulkanOutputFeature& operator=(const VulkanOutputFeature&) = delete;

        // Settings (persist across frames).
        void configure(float manualExposureEv, OutputTransformOperator op) noexcept {
            manualExposureEv_ = manualExposureEv;
            outputOperator_ = op;
        }
        void setManualExposure(float manualExposureEv) noexcept {
            manualExposureEv_ = manualExposureEv;
        }
        void setGridOverlay(const ViewportGridOverlay& overlay) noexcept {
            gridOverlay_ = overlay;
        }
        [[nodiscard]] OutputTransformOperator outputOperator() const noexcept {
            return outputOperator_;
        }
        // The caller has validated the LUT and drained the frames in flight.
        void setLut(TextureHandle lut);

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void onGraphReleased() override;
        void destroy() noexcept override;

        // Transport-dependent pipelines (init and transport changes).
        void createPipelines(VkFormat outputFormat, bool hdr10Composition,
            VkFormat swapchainFormat);
        void destroyPipelines() noexcept;
        [[nodiscard]] VkRenderPass outputRenderPass() const noexcept {
            return outputPass_.renderPass();
        }
        // After the frame targets are rebuilt.
        void rebuildHdr10Targets(const std::vector<VkImageView>& swapchainViews,
            VkExtent2D extent);
        void rebuildDescriptors();

        // Drain points (submitOutputPass / submitUIPass).
        void recordOutputTransform(const FrameInputs& inputs);
        void recordHdr10Encode(VkExtent2D swapchainExtent, float paperWhiteNits,
            float peakNits);

    private:
        static bool never(void* owner, const VulkanFrameRecordContext& frame);
        static void executeNothing(void* owner, VulkanPassContext& context);
        static void executeOutputTransform(void* owner, VulkanPassContext& context);
        static void executeHdr10Encode(void* owner, VulkanPassContext& context);

        const VulkanFeatureContext* context_ = nullptr;
        VulkanOutputPass outputPass_;
        VulkanHdrEncodePass hdrEncodePass_;
        RenderGraph::PassId bloomHookPass_{};
        RenderGraph::PassId outputTransformPass_{};
        RenderGraph::PassId hdr10EncodePass_{};
        float manualExposureEv_ = 0.0f;
        OutputTransformOperator outputOperator_ = OutputTransformOperator::Aces2;
        ViewportGridOverlay gridOverlay_{};
        TextureHandle lut_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        FrameInputs staged_{};
        VkExtent2D stagedSwapchainExtent_{};
    };

} // namespace Iridium
