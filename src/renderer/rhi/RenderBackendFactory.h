#pragma once

#include "IRenderBackend.h"
#include "RenderBackendExtension.h"

#include <memory>
#include <span>

namespace Iridium {

    struct RenderBackendCreateInfo {
        RenderBackendApi api = RenderBackendApi::Vulkan;
        // Attached before init(), in order. Non-owning: each extension must
        // outlive the backend. An extension for another API is rejected.
        // Production runs attach none; the qualification harness supplies its
        // extension through IFrameObserver::backendExtensions().
        std::span<IRenderBackendExtension* const> extensions{};
    };

    [[nodiscard]] std::unique_ptr<IRenderBackend> createRenderBackend(
        const RenderBackendCreateInfo& createInfo);

} // namespace Iridium
