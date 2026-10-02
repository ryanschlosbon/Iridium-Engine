#pragma once

#include "IRenderBackend.h"

#include <cstdint>
#include <memory>

namespace Iridium {

    enum class RenderBackendApi : uint8_t {
        Vulkan,
        DirectX12,
    };

    [[nodiscard]] std::unique_ptr<IRenderBackend> createRenderBackend(RenderBackendApi api);

} // namespace Iridium
