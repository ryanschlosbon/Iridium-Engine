#include "VulkanExtensionHooks.h"

#include <algorithm>
#include <stdexcept>

namespace Iridium {

    void VulkanExtensionHooks::attach(IRenderBackendExtension* extension,
        bool backendInitialized) {
        if (extension == nullptr)
            throw std::invalid_argument("Render backend extension is null");
        if (backendInitialized)
            throw std::logic_error(
                "Render backend extensions attach before init()");
        if (extension->api() != RenderBackendApi::Vulkan)
            throw std::invalid_argument(
                "Render backend extension targets another graphics API");
        auto* vulkanExtension = dynamic_cast<IVulkanBackendExtension*>(extension);
        if (vulkanExtension == nullptr)
            throw std::invalid_argument(
                "Vulkan backend extension does not implement IVulkanBackendExtension");
        extensions_.push_back(vulkanExtension);
        // The first extension providing each service serves it.
        if (indirectOracle_ == nullptr)
            indirectOracle_ = vulkanExtension->indirectOracle();
        if (indirectStreamObserver_ == nullptr)
            indirectStreamObserver_ = vulkanExtension->indirectStreamObserver();
    }

    void VulkanExtensionHooks::configure(const RenderBackendConfig& config) {
        // Extensions see the configuration before any graph or resource
        // exists; their hook declarations and oracle are fixed for the process.
        for (IVulkanBackendExtension* extension : extensions_)
            extension->configure(config);
        graphHooks_ = VulkanGraphHooks::none();
        for (IVulkanBackendExtension* extension : extensions_)
            graphHooks_ = graphHooks_ | extension->graphHooks();
    }

    bool VulkanExtensionHooks::anyWants(const VulkanHookContext& context) const {
        return std::ranges::any_of(extensions_,
            [&](const IVulkanBackendExtension* extension) {
                return extension->wantsHook(context);
            });
    }

    void VulkanExtensionHooks::notify(const VulkanHookContext& context) const {
        for (IVulkanBackendExtension* extension : extensions_)
            if (extension->wantsHook(context)) extension->onHook(context);
    }

    void VulkanExtensionHooks::onBackendInitialized(
        const VulkanBackendServices& services) const {
        for (IVulkanBackendExtension* extension : extensions_)
            extension->onBackendInitialized(services);
    }

    void VulkanExtensionHooks::onFrameSlotRetired(uint32_t slot) const {
        for (IVulkanBackendExtension* extension : extensions_)
            extension->onFrameSlotRetired(slot);
    }

    void VulkanExtensionHooks::onBeforeDeviceDestroy() const {
        for (IVulkanBackendExtension* extension : extensions_)
            extension->onBeforeDeviceDestroy();
    }

} // namespace Iridium
