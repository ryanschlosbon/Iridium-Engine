#include "VulkanUiFeature.h"

#include "VkContext.h"
#include "VulkanEditorUi.h"
#include "VulkanFrameTargets.h"

namespace Iridium {

    void VulkanUiFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
    }

    void VulkanUiFeature::createRenderPass(VkFormat format, bool hdr10Composition) {
        hdr10Composition_ = hdr10Composition;
        renderPass_ = std::make_unique<VkUIRenderPass>(&context_->vk, format,
            !hdr10Composition);
    }

    void VulkanUiFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        uiPass_ = ids.ui;
    }

    void VulkanUiFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // gpu.ui starts after the pass's barriers, as it was recorded
        // imperatively before R3c.10.
        graph.registerPass(uiPass_, { this, nullptr, &executeUi, "gpu.ui",
            GpuRangePlacement::AfterBarriers });
    }

    void VulkanUiFeature::destroy() noexcept {
        renderPass_.reset();
        editorUi_ = nullptr;
        context_ = nullptr;
    }

    void VulkanUiFeature::record(VkExtent2D swapchainExtent) {
        stagedSwapchainExtent_ = swapchainExtent;
        context_->graph.drainRegisteredThrough(uiPass_);
    }

    void VulkanUiFeature::executeUi(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanUiFeature*>(owner);
        VulkanFrameTargets& targets = self.context_->frameTargets;
        VkRenderPassBeginInfo uiPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        uiPassInfo.renderPass = self.renderPass_->getRenderPass();
        uiPassInfo.framebuffer = self.hdr10Composition_
            ? targets.get(context.frame.frameIndex).uiCompositionFramebuffer
            : targets.uiFramebuffer(context.frame.imageIndex);
        uiPassInfo.renderArea.extent = self.stagedSwapchainExtent_;

        VkClearValue uiClearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        uiPassInfo.clearValueCount = 1;
        uiPassInfo.pClearValues = &uiClearColor;

        vkCmdBeginRenderPass(context.commandBuffer, &uiPassInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        if (self.editorUi_ != nullptr) self.editorUi_->recordUi(context.commandBuffer);
        vkCmdEndRenderPass(context.commandBuffer);
    }

} // namespace Iridium
