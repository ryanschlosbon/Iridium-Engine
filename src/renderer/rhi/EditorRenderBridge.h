#pragma once

// M7R R3c.10: the editor's view of the renderer, outside IRenderBackend.
//
// The editor host (Application) creates one bridge for the backend API in use
// (Vulkan: renderer/vulkan_imgui) and attaches backendExtension() through
// RenderBackendCreateInfo::extensions before the backend is initialized. The
// bridge owns the editor UI library's renderer state (context, fonts,
// descriptor pool, texture registrations, retained editor views); the backend
// only offers the hooks a bridge needs (device ready, target rebuilds,
// texture retirement, swapchain changes, a UI contribution inside its "ui"
// pass). A backend without a bridge still clears and presents its UI pass.
//
// Texture ids are opaque values for the editor UI library (ImGui's
// ImTextureID). They are valid for the current frame slot until the frame
// targets are rebuilt (resize, transport or topology changes) or the texture
// is freed.

#include "core/types/RenderHandles.h"

#include <cstdint>

namespace Iridium {

    class IRenderBackendExtension;

    class IEditorRenderBridge {
    public:
        virtual ~IEditorRenderBridge() = default;

        // Attached at backend creation; non-owning, outlives the backend.
        [[nodiscard]] virtual IRenderBackendExtension& backendExtension() noexcept = 0;

        // Starts the editor UI frame. Only after beginFrame acquired a frame
        // slot, so scene texture ids refer to the slot being recorded.
        virtual void beginUI() = 0;

        // The current slot's display-output target and depth, for viewports.
        [[nodiscard]] virtual void* sceneTextureId() = 0;
        [[nodiscard]] virtual void* glassDepthTextureId() = 0;
        // A resident backend texture (material previews, thumbnails); null
        // for an unknown or retired handle.
        [[nodiscard]] virtual void* editorTextureId(TextureHandle texture) = 0;

        // Serially rendered views (scene and asset viewer) share transient
        // work and keep independent outputs. Between frames only; view
        // indices are 0 and 1. `renderView` is the view this frame renders.
        virtual void prepareRetainedViews(bool enabled, uint32_t renderView) = 0;
        [[nodiscard]] virtual void* retainedViewTextureId(uint32_t view) = 0;
    };

} // namespace Iridium
