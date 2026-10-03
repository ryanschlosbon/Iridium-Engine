#pragma once

// M7R R3c.10: the Vulkan side of IEditorRenderBridge. A backend extension
// that implements IVulkanEditorUi (renderer/vulkan_imgui) receives the events
// that drive editor texture registration and contributes UI recording inside
// the backend-owned "ui" pass. The backend has no editor-library dependency:
// without an editor UI the "ui" pass still clears and presents.
//
// Event order (each at the point where the backend used to do the work):
//   onUiDeviceReady          end of init, before any extension's
//                            onBackendInitialized; followed by
//                            onFrameTargetsCreated
//   onFrameTargetsReleased   before the frame targets are destroyed (resize,
//                            transport and topology changes)
//   onFrameTargetsCreated    after they were recreated and their layouts
//                            established
//   onPresentationChanged    the swapchain and the UI colour format were
//                            replaced (transport change or recreate)
//   onSwapchainImageCountChanged  after the replacement targets exist
//   onDisplayColorChanged    live output settings changed
//   recordUi                 inside the "ui" pass's rendering of every frame
//   onUiShutdown             after device idle and the extensions'
//                            onBeforeDeviceDestroy, before the backend
//                            destroys its resources
// Retained editor views consume "final-capture-hook": prepareRetainedViewImages
// initializes newly created images and copyRetainedView copies the output.

#include "renderer/color/OutputTransformConfig.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Iridium {

    class VulkanFrameScheduler;
    class VulkanFrameTargets;
    class VulkanFrameTelemetry;
    class VulkanResourceAllocator;
    class VulkanResourceRegistry;

    // Stable from onUiDeviceReady until onUiShutdown.
    struct VulkanEditorUiDevice {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator = nullptr;
        VulkanFrameScheduler* scheduler = nullptr;
        VulkanFrameTargets* frameTargets = nullptr;
        // Texture payloads; the registry releases a retired texture's editor
        // descriptor through the release hook the editor UI installs.
        VulkanResourceRegistry* resources = nullptr;
        // Draw and pipeline-bind counters of the frame being recorded.
        VulkanFrameTelemetry* telemetry = nullptr;
        // The depth-pyramid history follows the retained view being rendered.
        void* backend = nullptr;
        void (*selectRetainedView)(void* backend, uint32_t view) = nullptr;
    };

    // Swapchain-dependent state: init and every transport change.
    struct VulkanEditorUiPresentation {
        // The format of the "ui" pass's one colour attachment (cleared): the
        // swapchain's, or RGBA16F for HDR10 composition. The pass records
        // with dynamic rendering (M7R R4a).
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        uint32_t imageCount = 0;
        Color::OutputTransport transport = Color::OutputTransport::SdrSrgb;
        float paperWhiteNits = 203.0f;
    };

    class IVulkanEditorUi {
    public:
        virtual void onUiDeviceReady(const VulkanEditorUiDevice& device,
            const VulkanEditorUiPresentation& presentation) = 0;
        virtual void onFrameTargetsCreated() = 0;
        virtual void onFrameTargetsReleased() = 0;
        virtual void onPresentationChanged(
            const VulkanEditorUiPresentation& presentation) = 0;
        virtual void onSwapchainImageCountChanged(uint32_t imageCount) = 0;
        virtual void onDisplayColorChanged(Color::OutputTransport transport,
            float paperWhiteNits) = 0;

        // Retained editor views ("final-capture-hook" consumer).
        [[nodiscard]] virtual bool retainedViewsEnabled() const noexcept = 0;
        virtual void prepareRetainedViewImages(VkCommandBuffer commandBuffer) = 0;
        virtual void copyRetainedView(VkCommandBuffer commandBuffer) = 0;

        // Inside the "ui" pass's dynamic rendering, after its clear.
        virtual void recordUi(VkCommandBuffer commandBuffer) = 0;

        virtual void onUiShutdown() noexcept = 0;

    protected:
        ~IVulkanEditorUi() = default;
    };

} // namespace Iridium
