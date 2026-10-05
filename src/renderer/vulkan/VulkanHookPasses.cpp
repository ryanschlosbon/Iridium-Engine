#include "VulkanHookPasses.h"

#include "VulkanExtensionHooks.h"
#include "VulkanFrameScheduler.h"

#include <stdexcept>

namespace Iridium {

    void VulkanHookPasses::create(const VulkanFeatureContext& context) {
        context_ = &context;
        constexpr std::array<const char*, static_cast<size_t>(PassHook::Count)> ranges{
            "gpu.depth.occlusion-pyramid.validation-readback",
            "gpu.transparency.layered.validation-readback",
            "gpu.transparency.layered.hero4.validation-readback",
            "gpu.transparency.layered.cinematic8.validation-readback",
        };
        for (size_t index = 0; index < passHooks_.size(); ++index)
            passHooks_[index] = { this, {}, ranges[index], {} };
    }

    void VulkanHookPasses::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        passHooks_[static_cast<size_t>(PassHook::DepthPyramidValidation)].pass =
            ids.depthPyramidValidationHook;
        passHooks_[static_cast<size_t>(PassHook::Ordinary2Validation)].pass =
            ids.ordinary2ValidationHook;
        passHooks_[static_cast<size_t>(PassHook::Hero4Validation)].pass =
            ids.hero4.validationReadbackHook;
        passHooks_[static_cast<size_t>(PassHook::Cinematic8Validation)].pass =
            ids.cinematic8.validationReadbackHook;
        sceneCapturePass_ = ids.sceneColorCaptureHook;
        finalCapturePass_ = ids.finalCaptureHook;
    }

    void VulkanHookPasses::registerPasses(VulkanRenderGraphExecutor& graph) {
        // A pass hook's range wraps its barriers and the extension work.
        for (PassHookSlot& slot : passHooks_) {
            if (!slot.pass.isValid()) continue;
            graph.registerPass(slot.pass, { &slot, &passHookActive, &executePassHook,
                slot.gpuRange, GpuRangePlacement::BeforeBarriers });
        }
        if (sceneCapturePass_.isValid())
            graph.registerPass(sceneCapturePass_, { this, &sceneCaptureActive,
                &executeSceneCapture });
        // Always declared: retained editor views also copy output through it.
        graph.registerPass(finalCapturePass_, { this, &finalCaptureActive,
            &executeFinalCapture });
    }

    void VulkanHookPasses::destroy() noexcept {
        context_ = nullptr;
    }

    void VulkanHookPasses::runPassHook(PassHook hook, const VulkanHookContext& context) {
        PassHookSlot& slot = passHooks_[static_cast<size_t>(hook)];
        slot.staged = context;
        context_->graph.drainRegisteredThrough(slot.pass);
    }

    void VulkanHookPasses::runSceneColorCapture(VkCommandBuffer commandBuffer,
        const VulkanCaptureHookPayload& source) {
        const VulkanHookContext context{ .point = VulkanHookPoint::SceneColorComplete,
            .cmd = commandBuffer, .slot = context_->scheduler.currentFrameIndex() };
        if (!sceneCapturePass_.isValid()) {
            // R3b.5: the declared hook pass moves scene.color to
            // TransferSource; output-transform's begin returns it.
            if (context_->extensions.anyWants(context))
                throw std::logic_error(
                    "Scene-linear capture requires the scene-color-capture-hook pass");
            return;
        }
        stagedScene_ = context;
        stagedSceneSource_ = source;
        context_->graph.drainRegisteredThrough(sceneCapturePass_);
    }

    void VulkanHookPasses::runFinalCapture(VkCommandBuffer commandBuffer,
        const VulkanCaptureHookPayload& source, bool retainedViews) {
        stagedFinal_ = { .point = VulkanHookPoint::FinalCaptureHook,
            .cmd = commandBuffer, .slot = context_->scheduler.currentFrameIndex() };
        stagedFinalSource_ = source;
        stagedFinalWanted_ = context_->extensions.anyWants(stagedFinal_);
        stagedRetainedViews_ = retainedViews && finalConsumer_.copy != nullptr;
        // Without a capture, retained views initialize new images before the
        // pass begins; after a capture they do so after its copy (as recorded
        // before R3c.4).
        if (stagedRetainedViews_ && !stagedFinalWanted_)
            finalConsumer_.prepare(finalConsumer_.owner, commandBuffer);
        context_->graph.drainRegisteredThrough(finalCapturePass_);
    }

    bool VulkanHookPasses::passHookActive(void* owner, const VulkanFrameRecordContext&) {
        const auto& slot = *static_cast<const PassHookSlot*>(owner);
        return slot.self->context_->extensions.anyWants(slot.staged);
    }

    void VulkanHookPasses::executePassHook(void* owner, VulkanPassContext&) {
        const auto& slot = *static_cast<const PassHookSlot*>(owner);
        slot.self->context_->extensions.notify(slot.staged);
    }

    bool VulkanHookPasses::sceneCaptureActive(void* owner, const VulkanFrameRecordContext&) {
        const auto& self = *static_cast<const VulkanHookPasses*>(owner);
        return self.context_->extensions.anyWants(self.stagedScene_);
    }

    void VulkanHookPasses::executeSceneCapture(void* owner, VulkanPassContext&) {
        auto& self = *static_cast<VulkanHookPasses*>(owner);
        VulkanHookContext context = self.stagedScene_;
        context.payload = self.stagedSceneSource_;
        self.context_->extensions.notify(context);
    }

    bool VulkanHookPasses::finalCaptureActive(void* owner, const VulkanFrameRecordContext&) {
        const auto& self = *static_cast<const VulkanHookPasses*>(owner);
        return self.stagedFinalWanted_ || self.stagedRetainedViews_;
    }

    void VulkanHookPasses::executeFinalCapture(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanHookPasses*>(owner);
        if (self.stagedFinalWanted_) {
            VulkanHookContext capture = self.stagedFinal_;
            capture.payload = self.stagedFinalSource_;
            self.context_->extensions.notify(capture);
        }
        if (self.stagedRetainedViews_) {
            if (self.stagedFinalWanted_)
                self.finalConsumer_.prepare(self.finalConsumer_.owner, context.commandBuffer);
            self.finalConsumer_.copy(self.finalConsumer_.owner, context.commandBuffer);
        }
    }

} // namespace Iridium
