#pragma once

// M7R R3c.0: the attached backend extensions (R2.7) as one service: their
// graph-hook declarations, the oracle and digest services they provide, and
// hook dispatch. Moved out of VulkanVertexBackend unchanged; feature owners
// reach it through VulkanFeatureContext.

#include "VulkanBackendExtension.h"

#include <span>
#include <vector>

namespace Iridium {

    class VulkanExtensionHooks final {
    public:
        // Factory-only, before init(). Does not take ownership; the first
        // extension providing each service serves it.
        void attach(IRenderBackendExtension* extension, bool backendInitialized);
        // Start of init(): every extension sees the configuration, then the
        // hook declarations are fixed for the process.
        void configure(const RenderBackendConfig& config);

        [[nodiscard]] const VulkanGraphHooks& graphHooks() const noexcept {
            return graphHooks_;
        }
        [[nodiscard]] IVulkanIndirectOracle* indirectOracle() const noexcept {
            return indirectOracle_;
        }
        [[nodiscard]] IVulkanIndirectStreamObserver* indirectStreamObserver()
            const noexcept {
            return indirectStreamObserver_;
        }
        [[nodiscard]] std::span<IVulkanBackendExtension* const> extensions()
            const noexcept {
            return extensions_;
        }

        [[nodiscard]] bool anyWants(const VulkanHookContext& context) const;
        // Calls every extension that wants the hook.
        void notify(const VulkanHookContext& context) const;

        void onBackendInitialized(const VulkanBackendServices& services) const;
        void onFrameSlotRetired(uint32_t slot) const;
        void onBeforeDeviceDestroy() const;

    private:
        std::vector<IVulkanBackendExtension*> extensions_;
        IVulkanIndirectOracle* indirectOracle_ = nullptr;
        IVulkanIndirectStreamObserver* indirectStreamObserver_ = nullptr;
        VulkanGraphHooks graphHooks_ = VulkanGraphHooks::none();
    };

} // namespace Iridium
