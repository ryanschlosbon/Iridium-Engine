#pragma once

// M7R R3c.9: the forward surfaces as a feature owner. Owns the forward and
// transparent render passes, the three forward draw passes ("forward-opaque",
// "transparent.sorted.forward" and "transparent.compatibility.forward", each
// with its gpu.* range before its barriers) and the refraction pyramids
// ("transparent.refraction-pyramids", gpu.transparency.refraction-pyramids
// before its barriers) with their residency, which selects the topology that
// declares them. The compatibility pass skips packets the layered owner
// resolved; the sorted pass skips WeightedOIT packets when the WeightedOIT
// owner executes them.

#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/PipelineTypes.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/transparency/TransparencyPyramidResidency.h"

#include "VkForwardRenderPass.h"
#include "VulkanFeatureContext.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanTransparencyPyramid.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace Iridium {

    class VulkanLayeredTransparencyFeature;

    class VulkanForwardFeature final : public IVulkanFeature {
    public:
        // Per-frame inputs (submitForwardQueues), valid until the last drain.
        struct FrameInputs {
            std::span<const DrawPacket> opaqueForwardQueue{};
            std::span<const DrawPacket> sortedSurfaceQueue{};
            std::span<const DrawPacket> compatibilityTransparentQueue{};
            // The WeightedOIT owner draws the queue's WeightedOIT packets.
            bool skipWeightedOit = false;
            VkDescriptorSet globalSet = VK_NULL_HANDLE;
            VkDescriptorSet sceneSet = VK_NULL_HANDLE;
            RenderDebugView debugView = RenderDebugView::Final;
        };

        VulkanForwardFeature() = default;
        VulkanForwardFeature(const VulkanForwardFeature&) = delete;
        VulkanForwardFeature& operator=(const VulkanForwardFeature&) = delete;

        // Before the first frame: the layered owner whose resolved packets the
        // compatibility pass skips.
        void configure(const VulkanLayeredTransparencyFeature& layered) noexcept {
            layered_ = &layered;
        }

        // IVulkanFeature. create() builds the render passes (the pipeline
        // library and the frame targets need them) and the pyramid pipelines.
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        [[nodiscard]] VkRenderPass forwardRenderPass() const noexcept {
            return forwardPass_->getRenderPass();
        }
        [[nodiscard]] VkRenderPass transparentRenderPass() const noexcept {
            return transparentPass_->getRenderPass();
        }
        [[nodiscard]] TransparencyPyramidResidency& pyramidResidency() noexcept {
            return pyramidResidency_;
        }
        [[nodiscard]] const TransparencyPyramidResidency& pyramidResidency() const noexcept {
            return pyramidResidency_;
        }

        // Pyramid descriptors over the frame targets.
        void rebuildDescriptors();
        void clearDescriptors() noexcept { pyramid_.clearDescriptors(); }

        // Per frame: stage, then the drain points in recording order.
        void stage(const FrameInputs& inputs) noexcept { staged_ = inputs; }
        void recordOpaque();
        // Observes the pyramids' residency demand, then records or skips them.
        void recordRefractionPyramids(bool required);
        void recordSorted();
        void recordCompatibility();

    private:
        enum class Queue : uint8_t { OpaqueForward, Sorted, Compatibility, Count };

        struct ForwardPass {
            VulkanForwardFeature* self = nullptr;
            Queue queue = Queue::OpaqueForward;
            RenderGraph::PassId pass{};
            const char* gpuRange = nullptr;
            bool transparent = false;
            RenderPassClass expectedPassClass = RenderPassClass::Forward;
            bool skipResolvedLayered = false;
            bool skipWeightedOit = false;
        };

        [[nodiscard]] std::span<const DrawPacket> queue(Queue queue) const noexcept;
        [[nodiscard]] bool skipped(const ForwardPass& pass, const DrawPacket& packet,
            std::span<const DrawPacket> queue) const noexcept;
        static bool forwardActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeForward(void* owner, VulkanPassContext& context);
        static bool pyramidsActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executePyramids(void* owner, VulkanPassContext& context);
        void recordForward(const ForwardPass& pass, VkCommandBuffer commandBuffer,
            uint32_t frame);

        const VulkanFeatureContext* context_ = nullptr;
        const VulkanLayeredTransparencyFeature* layered_ = nullptr;
        std::unique_ptr<VkForwardRenderPass> forwardPass_;
        std::unique_ptr<VkForwardRenderPass> transparentPass_;
        VulkanTransparencyPyramid pyramid_;
        TransparencyPyramidResidency pyramidResidency_;
        std::array<ForwardPass, static_cast<size_t>(Queue::Count)> passes_{};
        RenderGraph::PassId pyramidPass_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        FrameInputs staged_{};
        bool stagedPyramids_ = false;
    };

} // namespace Iridium
