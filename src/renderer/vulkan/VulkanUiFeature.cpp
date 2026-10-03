#include "VulkanUiFeature.h"

#include "VkContext.h"
#include "VulkanEditorUi.h"
#include "VulkanFrameTargets.h"

namespace Iridium {

    void VulkanUiFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
    }

    void VulkanUiFeature::createRenderPass(VkFormat format, bool hdr10Composition) {
        colorFormat_ = format;
        renderPass_ = std::make_unique<VkUIRenderPass>(&context_->vk, format,
            !hdr10Composition);
    }

    void VulkanUiFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        uiPass_ = ids.ui;
    }

    void VulkanUiFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // gpu.ui starts after the pass's barriers, as it was recorded
        // imperatively before R3c.10. R4a: dynamic rendering.
        graph.registerPass(uiPass_, { this, nullptr, &executeUi, "gpu.ui",
            GpuRangePlacement::AfterBarriers, true });
    }

    void VulkanUiFeature::destroy() noexcept {
        renderPass_.reset();
        colorFormat_ = VK_FORMAT_UNDEFINED;
        editorUi_ = nullptr;
        context_ = nullptr;
    }

    void VulkanUiFeature::record(VkExtent2D swapchainExtent) {
        stagedSwapchainExtent_ = swapchainExtent;
        context_->graph.drainRegisteredThrough(uiPass_);
    }

    void VulkanUiFeature::executeUi(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanUiFeature*>(owner);
        // The planned attachment: the acquired swapchain image (bound per
        // frame, discarded on first use) or the HDR10 composition target,
        // cleared to opaque black.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea.extent = self.stagedSwapchainExtent_;
        context.beginRendering(rendering);
        if (self.editorUi_ != nullptr) self.editorUi_->recordUi(context.commandBuffer);
        context.endRendering();
    }

} // namespace Iridium
