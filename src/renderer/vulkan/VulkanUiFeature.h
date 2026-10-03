#pragma once

// M7R R3c.10: the backend-owned UI pass ("ui-present", or "ui-compose" for
// HDR10 composition) as a feature owner. Owns the UI render pass, registers
// the pass callback and records the clear, the attached editor UI's
// contribution (IVulkanEditorUi::recordUi, renderer/vulkan_imgui) and the end
// of the render pass. Without an editor UI the pass still clears the
// swapchain image (or the composition target) and the frame presents.

#include "VulkanFeatureContext.h"
#include "VulkanRenderGraphExecutor.h"
#include "VkUIRenderPass.h"

#include <vulkan/vulkan.h>

#include <memory>

namespace Iridium {

    class IVulkanEditorUi;

    class VulkanUiFeature final : public IVulkanFeature {
    public:
        VulkanUiFeature() = default;
        VulkanUiFeature(const VulkanUiFeature&) = delete;
        VulkanUiFeature& operator=(const VulkanUiFeature&) = delete;

        // Null for a backend without an editor bridge.
        void setEditorUi(IVulkanEditorUi* editorUi) noexcept { editorUi_ = editorUi; }

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // Swapchain-dependent render pass (init and transport changes): it
        // presents directly, or leaves the HDR10 composition target for the
        // encode pass.
        void createRenderPass(VkFormat format, bool hdr10Composition);
        void destroyRenderPass() noexcept { renderPass_.reset(); }
        [[nodiscard]] VkRenderPass renderPass() const noexcept {
            return renderPass_ ? renderPass_->getRenderPass() : VK_NULL_HANDLE;
        }

        // Drain point (submitUIPass).
        void record(VkExtent2D swapchainExtent);

    private:
        static void executeUi(void* owner, VulkanPassContext& context);

        const VulkanFeatureContext* context_ = nullptr;
        std::unique_ptr<VkUIRenderPass> renderPass_;
        bool hdr10Composition_ = false;
        IVulkanEditorUi* editorUi_ = nullptr;
        RenderGraph::PassId uiPass_{};

        // Staged for the frame's callback (see VulkanFeatureContext.h).
        VkExtent2D stagedSwapchainExtent_{};
    };

} // namespace Iridium
