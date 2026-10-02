#include "renderer/rhi/RenderBackendFactory.h"

#include "renderer/vulkan/VulkanVertexBackend.h"

#include <stdexcept>

namespace Iridium {

    std::unique_ptr<IRenderBackend> createRenderBackend(
        const RenderBackendCreateInfo& createInfo) {
        switch (createInfo.api) {
        case RenderBackendApi::Vulkan: {
            auto backend = std::make_unique<VulkanVertexBackend>();
            for (IRenderBackendExtension* extension : createInfo.extensions)
                backend->attachExtension(extension);
            return backend;
        }
        case RenderBackendApi::DirectX12:
            throw std::runtime_error("DirectX12 backend is not compiled yet");
        }

        throw std::runtime_error("Unknown render backend API");
    }

} // namespace Iridium
