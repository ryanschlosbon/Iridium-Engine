#pragma once

// M7R R3c.0: the attached backend extensions (R2.7) as one service: their
// graph-hook declarations, the oracle, digest and editor-UI services they
// provide, and hook dispatch. Moved out of VulkanVertexBackend unchanged;
// feature owners reach it through VulkanFeatureContext.

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
        // M7R R5c.1: null unless the caster-revision oracle is requested.
        [[nodiscard]] IVulkanCasterRevisionObserver* casterRevisionObserver()
            const noexcept {
            return casterRevisionObserver_;
        }
        // M7.10.0: reports one caster-revision evaluation to that observer,
        // if any. Callers guard the call with kQualificationBuild, so other
        // builds neither build the sample nor make the call.
        void observeCasterRevision(const VulkanCasterRevisionSample& sample) const {
            if (casterRevisionObserver_ != nullptr)
                casterRevisionObserver_->observeCasterRevision(sample);
        }
        // Null without an attached editor bridge (headless and test hosts).
        [[nodiscard]] IVulkanEditorUi* editorUi() const noexcept {
            return editorUi_;
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
        IVulkanCasterRevisionObserver* casterRevisionObserver_ = nullptr;
        IVulkanEditorUi* editorUi_ = nullptr;
        VulkanGraphHooks graphHooks_ = VulkanGraphHooks::none();
    };

} // namespace Iridium
