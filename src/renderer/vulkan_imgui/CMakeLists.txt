# iridium_vulkan_imgui (M7R R3c.10): the Dear ImGui editor bridge for the
# Vulkan backend. Implements the backend-neutral IEditorRenderBridge
# (renderer/rhi/EditorRenderBridge.h) and the Vulkan-side IVulkanEditorUi
# extension. Only the editor host (iridium_app) links it; iridium_vulkan has no
# ImGui dependency.
add_library(iridium_vulkan_imgui STATIC
        VulkanImGuiEditorBridge.cpp
)
target_link_libraries(iridium_vulkan_imgui PUBLIC
        iridium_vulkan
        iridium_vendor_imgui
        glfw)
iridium_target_pch(iridium_vulkan_imgui ENGINE VULKAN)
