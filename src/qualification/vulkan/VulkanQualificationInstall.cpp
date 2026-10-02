#include "qualification/vulkan/VulkanQualificationInstall.h"

#include "qualification/vulkan/VulkanQualificationExtension.h"
#include "renderer/rhi/RenderBackendFactory.h"

#include <memory>

namespace Iridium {

    namespace {
        std::unique_ptr<IRenderBackendExtension> makeDefaultBackendExtension(
            RenderBackendApi api) {
            if (api == RenderBackendApi::Vulkan)
                return std::make_unique<VulkanQualificationExtension>();
            return nullptr;
        }
    }

    void installQualificationBackendExtensions() noexcept {
        setDefaultRenderBackendExtensionFactory(&makeDefaultBackendExtension);
    }

} // namespace Iridium
