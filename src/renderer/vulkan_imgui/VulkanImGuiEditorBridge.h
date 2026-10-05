#pragma once

// M7R R3c.10: the Dear ImGui editor bridge for the Vulkan backend
// (iridium_vulkan_imgui). The editor host creates it, attaches
// backendExtension() through RenderBackendCreateInfo::extensions, and keeps it
// alive until the backend is destroyed. It owns the ImGui context, the GLFW
// and Vulkan ImGui backends, their descriptor pool and color-managed fragment
// shader, the editor's target/texture registrations and the retained editor
// views. This header has no ImGui or Vulkan dependency.

#include "renderer/rhi/EditorRenderBridge.h"

#include <memory>

struct GLFWwindow;

namespace Iridium {

    // `window` must outlive the bridge; ImGui's GLFW backend installs its
    // input callbacks on it when the backend device is ready.
    [[nodiscard]] std::unique_ptr<IEditorRenderBridge>
        createVulkanImGuiEditorBridge(GLFWwindow* window);

} // namespace Iridium
