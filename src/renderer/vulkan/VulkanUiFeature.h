#pragma once

// M7R R3c.10: the backend-owned UI pass ("ui-present", or "ui-compose" for
// HDR10 composition) as a feature owner. Registers the pass callback, which
// records (R4a: dynamic rendering over the pass's planned attachment) the
// clear and the attached editor UI's contribution (IVulkanEditorUi::recordUi,
// renderer/vulkan_imgui). Without an editor UI the pass still clears the
// swapchain image (or the composition target) and the frame presents; the
// executor moves the swapchain to PRESENT at frame end.

#include "VulkanFeatureContext.h"
#include "VulkanRenderGraphExecutor.h"

#include <vulkan/vulkan.h>

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

        // Swapchain-dependent state (init and transport changes): the UI
        // colour format (the swapchain's, or RGBA16F for HDR10 composition).
        void setColorFormat(VkFormat format) noexcept { colorFormat_ = format; }
        void resetColorFormat() noexcept { colorFormat_ = VK_FORMAT_UNDEFINED; }
        [[nodiscard]] VkFormat colorFormat() const noexcept { return colorFormat_; }

        // Drain point (submitUIPass).
        void record(VkExtent2D swapchainExtent);

    private:
        static void executeUi(void* owner, VulkanPassContext& context);

        const VulkanFeatureContext* context_ = nullptr;
        VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
        IVulkanEditorUi* editorUi_ = nullptr;
        RenderGraph::PassId uiPass_{};

        // Staged for the frame's callback (see VulkanFeatureContext.h).
        VkExtent2D stagedSwapchainExtent_{};
    };

} // namespace Iridium
