#include "RenderBackendFactory.h"

#include "renderer/vulkan/VulkanVertexBackend.h"

#include <stdexcept>

namespace Iridium {

    std::unique_ptr<IRenderBackend> createRenderBackend(RenderBackendApi api) {
        switch (api) {
        case RenderBackendApi::Vulkan:
            return std::make_unique<VulkanVertexBackend>();
        case RenderBackendApi::DirectX12:
            throw std::runtime_error("DirectX12 backend is not compiled yet");
        }

        throw std::runtime_error("Unknown render backend API");
    }

} // namespace Iridium
