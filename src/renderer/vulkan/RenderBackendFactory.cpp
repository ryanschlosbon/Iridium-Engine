#include "renderer/rhi/RenderBackendFactory.h"

#include "renderer/vulkan/VulkanVertexBackend.h"

#include <stdexcept>

namespace Iridium {

    namespace {
        // Set once from main() before any backend is created (R2.7 -> R2.9).
        constinit DefaultRenderBackendExtensionFactory
            defaultExtensionFactory = nullptr;
    }

    void setDefaultRenderBackendExtensionFactory(
        DefaultRenderBackendExtensionFactory factory) noexcept {
        defaultExtensionFactory = factory;
    }

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

    std::unique_ptr<IRenderBackend> createRenderBackend(RenderBackendApi api) {
        if (api != RenderBackendApi::Vulkan)
            return createRenderBackend(RenderBackendCreateInfo{ .api = api });
        auto backend = std::make_unique<VulkanVertexBackend>();
        if (defaultExtensionFactory != nullptr) {
            if (std::unique_ptr<IRenderBackendExtension> extension =
                    defaultExtensionFactory(api))
                backend->adoptExtension(std::move(extension));
        }
        return backend;
    }

} // namespace Iridium
