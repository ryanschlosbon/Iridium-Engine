#pragma once

namespace Iridium {

    // Registers VulkanQualificationExtension as the default extension that
    // the legacy createRenderBackend(RenderBackendApi) overload attaches, so
    // the capture/oracle requests still on IRenderBackend reach it (M7R R2.7).
    // An explicit call from main() in qualification builds: renderer code
    // never names this library, and no static-initialization registration is
    // relied on. R2.9 replaces this with the harness attaching its extension
    // through RenderBackendCreateInfo.
    void installQualificationBackendExtensions() noexcept;

} // namespace Iridium
