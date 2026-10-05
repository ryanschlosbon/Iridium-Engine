#include "VulkanOutputFeature.h"

#include "VkContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanResourceRegistry.h"
#include "profiling/CpuProfiler.h"

namespace Iridium {

    void VulkanOutputFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
    }

    void VulkanOutputFeature::createPipelines(VkFormat outputFormat,
        bool hdr10Composition, VkFormat swapchainFormat) {
        outputPass_.init(context_->vk, context_->pipelineCache,
            context_->descriptors, outputFormat);
        if (hdr10Composition) {
            hdrEncodePass_.init(context_->vk, context_->pipelineCache,
                context_->descriptors, swapchainFormat);
        }
    }

    void VulkanOutputFeature::destroyPipelines() noexcept {
        hdrEncodePass_.cleanup();
        outputPass_.cleanup();
    }

    void VulkanOutputFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        bloomHookPass_ = ids.bloomHook;
        outputTransformPass_ = ids.outputTransform;
        resolvedScene_ = ids.resolvedSceneColor;
        taaActive_ = ids.taaResolve.isValid();
        exposureState_ = ids.exposureCurrent;
        bloomChain_ = ids.bloomChain;
        hdr10EncodePass_ = ids.hdr10EncodePresent;
    }

    void VulkanOutputFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // M1 exposes scene-linear color to a future bloom implementation
        // without paying for a disabled effect or changing resource versions.
        // M9.4: declared only without bloom (VulkanBloomFeature owns post.bloom).
        if (bloomHookPass_.isValid())
            graph.registerPass(bloomHookPass_, { this, &never, &executeNothing });
        // The transition range measures only output-transform's barriers;
        // the transform range opens inside the callback.
        // R4a: dynamic rendering; the executor's SampledRead/TransferSource
        // -> ColorAttachment barrier replaces the render pass's external
        // dependency.
        graph.registerPass(outputTransformPass_, { this, nullptr,
            &executeOutputTransform, "gpu.output.graph_transition",
            GpuRangePlacement::AroundBarriers, true });
        // R4a.final: dynamic rendering over the pass's planned swapchain
        // attachment (CLEAR opaque black, STORE). The swapchain is its only
        // write, so the flag adds no same-access re-barrier.
        if (hdr10EncodePass_.isValid()) {
            graph.registerPass(hdr10EncodePass_, { this, nullptr,
                &executeHdr10Encode, "gpu.output.hdr10_encode",
                GpuRangePlacement::AfterBarriers, true });
        }
    }

    void VulkanOutputFeature::onGraphReleased() {
        outputPass_.clearDescriptors();
        hdrEncodePass_.clearTargets();
    }

    void VulkanOutputFeature::destroy() noexcept {
        outputPass_.clearDescriptors();
        hdrEncodePass_.clearTargets();
        destroyPipelines();
        manualExposureEv_ = 0.0f;
        outputOperator_ = OutputTransformOperator::Aces2;
        lut_ = {};
        context_ = nullptr;
    }

    void VulkanOutputFeature::rebuildHdr10Targets() {
        hdrEncodePass_.rebuild(context_->frameTargets);
    }

    void VulkanOutputFeature::rebuildDescriptors() {
        if (VulkanTexturePayload* lut = context_->resources.textures().get(lut_);
            lut != nullptr && !lut->retired) {
            outputPass_.rebuildDescriptors(context_->frameTargets, lut->image.view,
                lut->sampler);
        }
        else {
            outputPass_.rebuildDescriptors(context_->frameTargets);
        }
    }

    void VulkanOutputFeature::setLut(TextureHandle lut) {
        lut_ = lut;
        rebuildDescriptors();
    }

    void VulkanOutputFeature::recordOutputTransform(const FrameInputs& inputs) {
        CpuScope outputRecordScope(context_->profiler,
            "cpu.render.record.output_transform");
        staged_ = inputs;
        context_->graph.drainRegisteredThrough(outputTransformPass_);
    }

    void VulkanOutputFeature::recordHdr10Encode(VkExtent2D swapchainExtent,
        float paperWhiteNits, float peakNits) {
        stagedSwapchainExtent_ = swapchainExtent;
        staged_.paperWhiteNits = paperWhiteNits;
        staged_.peakNits = peakNits;
        context_->graph.drainRegisteredThrough(hdr10EncodePass_);
    }

    bool VulkanOutputFeature::never(void*, const VulkanFrameRecordContext&) {
        return false;
    }

    void VulkanOutputFeature::executeNothing(void*, VulkanPassContext&) {}

    void VulkanOutputFeature::executeOutputTransform(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanOutputFeature*>(owner);
        const VulkanFeatureContext& shared = *self.context_;
        const uint32_t frameIndex = context.frame.frameIndex;
        VulkanGpuScope outputGpuScope(shared.scheduler, "gpu.output.transform");
        const VkExtent2D extent = shared.frameTargets.extent();
        // M9.2: this frame's resolved scene colour (TAA history parity/view set).
        if (self.taaActive_)
            self.outputPass_.setSceneView(frameIndex,
                context.graph.image(frameIndex, self.resolvedScene_).view,
                shared.frameTargets.sampler());
        // M9.5: this frame's adapted exposure (History parity/view set).
        const bool autoExposure = self.exposureState_.isValid();
        if (autoExposure)
            self.outputPass_.setExposureBuffer(frameIndex,
                context.graph.buffer(frameIndex, self.exposureState_).buffer);
        // M9.4: this frame's bloom chain (level 0 through the lod-0 sampler).
        const bool bloom = self.bloomChain_.isValid();
        if (bloom)
            self.outputPass_.setSceneView(frameIndex,
                context.graph.image(frameIndex, self.bloomChain_).view,
                shared.frameTargets.sampler(), 6);
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, extent };
        context.beginRendering(rendering);
        self.outputPass_.record(context.commandBuffer, frameIndex, extent,
            self.manualExposureEv_,
            static_cast<uint32_t>(self.outputOperator_),
            static_cast<uint32_t>(self.staged_.transport),
            self.staged_.paperWhiteNits, self.staged_.peakNits,
            self.staged_.selectionOutline, self.gridOverlay_,
            self.staged_.motionVectorView, autoExposure,
            { bloom, self.staged_.bloomIntensity, self.staged_.bloomAdditive });
        context.endRendering();
        if (shared.telemetry.collecting()) {
            shared.telemetry.recordPipelineBind(pipelineIdentity(
                FixedPipelineIdentity::OutputTransform));
            shared.telemetry.recordDraw(shared.telemetry.counters().drawOutput, 1);
        }
    }

    void VulkanOutputFeature::executeHdr10Encode(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanOutputFeature*>(owner);
        const VulkanFeatureContext& shared = *self.context_;
        VulkanRenderingOverrides rendering{};
        rendering.renderArea.extent = self.stagedSwapchainExtent_;
        context.beginRendering(rendering);
        self.hdrEncodePass_.record(context.commandBuffer, context.frame.frameIndex,
            self.stagedSwapchainExtent_, self.staged_.paperWhiteNits,
            self.staged_.peakNits);
        context.endRendering();
        if (shared.telemetry.collecting()) {
            shared.telemetry.recordPipelineBind(pipelineIdentity(
                FixedPipelineIdentity::OutputTransform));
            shared.telemetry.recordDraw(shared.telemetry.counters().drawOutput, 1);
        }
    }

} // namespace Iridium
