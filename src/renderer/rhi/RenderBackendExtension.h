#pragma once

#include <cstdint>

namespace Iridium {

    enum class RenderBackendApi : uint8_t {
        Vulkan,
        DirectX12,
    };

    // Backend-neutral handle for optional code that a backend factory attaches
    // to the backend it creates (M7R R2). Production code never depends on a
    // concrete extension: each backend narrows an extension to its own
    // API-specific interface (Vulkan: IVulkanBackendExtension) and rejects
    // extensions written for another API. Extensions are not owned by the
    // backend unless the factory says otherwise and must outlive it.
    class IRenderBackendExtension {
    public:
        virtual ~IRenderBackendExtension() = default;
        [[nodiscard]] virtual RenderBackendApi api() const noexcept = 0;
    };

} // namespace Iridium
