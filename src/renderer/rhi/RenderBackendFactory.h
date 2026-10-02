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
        std::span<IRenderBackendExtension* const> extensions{};
    };

    [[nodiscard]] std::unique_ptr<IRenderBackend> createRenderBackend(
        const RenderBackendCreateInfo& createInfo);

    // Legacy entry point (M7R R2.7 -> R2.9). It attaches the extension made by
    // the registered default-extension factory, owned by the backend, so the
    // capture/oracle methods still on IRenderBackend keep working until the
    // qualification harness attaches its extension through the CreateInfo
    // overload. With no registered factory it attaches nothing.
    [[nodiscard]] std::unique_ptr<IRenderBackend> createRenderBackend(
        RenderBackendApi api);

    using DefaultRenderBackendExtensionFactory =
        std::unique_ptr<IRenderBackendExtension> (*)(RenderBackendApi api);

    // Registered explicitly by the qualification library's installer, called
    // from main(); renderer code never names the qualification library.
    // Removed with the legacy overload in R2.9.
    void setDefaultRenderBackendExtensionFactory(
        DefaultRenderBackendExtensionFactory factory) noexcept;

} // namespace Iridium
