#include "VulkanForwardFeature.h"

#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanLayeredTransparencyFeature.h"
#include "VulkanMeshLayouts.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanResourceRegistry.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/transparency/WeightedOit.h"

namespace Iridium {

    void VulkanForwardFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        pyramid_.init(context.device, context.pipelineCache, context.descriptors,
            context.meshLayouts.getGlobalSetLayout());
        passes_[static_cast<size_t>(Queue::OpaqueForward)] = { this, Queue::OpaqueForward,
            {}, "gpu.forward.opaque", false, RenderPassClass::Forward, 2, false, false };
        passes_[static_cast<size_t>(Queue::Sorted)] = { this, Queue::Sorted,
            {}, "gpu.transparency.sorted.forward", true, RenderPassClass::Transparent, 1,
            false, true };
        passes_[static_cast<size_t>(Queue::Compatibility)] = { this, Queue::Compatibility,
            {}, "gpu.transparency.compatibility.forward", false, RenderPassClass::Forward, 1,
            true, false };
    }

    void VulkanForwardFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        passes_[static_cast<size_t>(Queue::OpaqueForward)].pass = ids.forwardOpaque;
        passes_[static_cast<size_t>(Queue::Sorted)].pass = ids.sortedForward;
        passes_[static_cast<size_t>(Queue::Compatibility)].pass = ids.compatibilityForward;
        pyramidPass_ = ids.refractionPyramids;
    }

    void VulkanForwardFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // Each forward pass's range wraps its barriers and its draws. R4a:
        // dynamic rendering; scene.color (and depth between forward-opaque and
        // compatibility) get the same-access attachment re-barrier that the
        // render passes' external dependencies used to provide.
        for (ForwardPass& pass : passes_) {
            graph.registerPass(pass.pass, { &pass, &forwardActive, &executeForward,
                pass.gpuRange, GpuRangePlacement::BeforeBarriers, true });
        }
        // Declared only while the refraction pyramids are resident.
        if (pyramidPass_.isValid())
            graph.registerPass(pyramidPass_, { this, &pyramidsActive, &executePyramids,
                "gpu.transparency.refraction-pyramids", GpuRangePlacement::BeforeBarriers });
    }

    void VulkanForwardFeature::destroy() noexcept {
        pyramid_.cleanup();
        context_ = nullptr;
    }

    void VulkanForwardFeature::rebuildDescriptors() {
        pyramid_.rebuild(context_->frameTargets);
    }

    void VulkanForwardFeature::recordOpaque() {
        context_->graph.drainRegisteredThrough(
            passes_[static_cast<size_t>(Queue::OpaqueForward)].pass);
    }

    void VulkanForwardFeature::recordRefractionPyramids(bool required) {
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        pyramidResidency_.observe(required);
        if (pyramidResidency_.requiresFallback(required) && telemetry.collecting()) {
            ++telemetry.counters().transparencyPyramidFallbackFrames;
        }
        stagedPyramids_ = required;
        // A no-op unless the pyramids are resident (declared).
        context_->graph.drainRegisteredThrough(pyramidPass_);
    }

    void VulkanForwardFeature::recordSorted() {
        context_->graph.drainRegisteredThrough(
            passes_[static_cast<size_t>(Queue::Sorted)].pass);
    }

    void VulkanForwardFeature::recordCompatibility() {
        context_->graph.drainRegisteredThrough(
            passes_[static_cast<size_t>(Queue::Compatibility)].pass);
    }

    std::span<const DrawPacket> VulkanForwardFeature::queue(Queue queue) const noexcept {
        switch (queue) {
        case Queue::OpaqueForward: return staged_.opaqueForwardQueue;
        case Queue::Sorted: return staged_.sortedSurfaceQueue;
        case Queue::Compatibility: return staged_.compatibilityTransparentQueue;
        case Queue::Count: break;
        }
        return {};
    }

    bool VulkanForwardFeature::skipped(const ForwardPass& pass, const DrawPacket& packet,
        std::span<const DrawPacket> queue) const noexcept {
        if (pass.skipWeightedOit && staged_.skipWeightedOit && isWeightedOitPacket(packet))
            return true;
        return pass.skipResolvedLayered && layered_->isPacketResolved(
            static_cast<uint32_t>(&packet - queue.data()));
    }

    bool VulkanForwardFeature::forwardActive(void* owner, const VulkanFrameRecordContext&) {
        const auto& pass = *static_cast<const ForwardPass*>(owner);
        const std::span<const DrawPacket> queue = pass.self->queue(pass.queue);
        for (const DrawPacket& packet : queue)
            if (!pass.self->skipped(pass, packet, queue)) return true;
        return false;
    }

    void VulkanForwardFeature::executeForward(void* owner, VulkanPassContext& context) {
        const auto& pass = *static_cast<const ForwardPass*>(owner);
        pass.self->recordForward(pass, context);
    }

    bool VulkanForwardFeature::pyramidsActive(void* owner, const VulkanFrameRecordContext&) {
        return static_cast<const VulkanForwardFeature*>(owner)->stagedPyramids_;
    }

    void VulkanForwardFeature::executePyramids(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanForwardFeature*>(owner);
        VulkanFrameTelemetry& telemetry = self.context_->telemetry;
        const uint32_t dispatches = self.pyramid_.record(context.commandBuffer,
            context.frame.frameIndex, self.staged_.globalSet, self.context_->frameTargets);
        if (telemetry.collecting())
            telemetry.counters().dispatchRecorded += dispatches;
        if (telemetry.collecting()) {
            ++telemetry.counters().transparencyPyramidBuilds;
            telemetry.counters().transparencyPyramidMipDispatches += dispatches;
        }
    }

    void VulkanForwardFeature::recordForward(const ForwardPass& pass,
        VulkanPassContext& context) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frame = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const VulkanPipelineLibrary& pipelineLibrary = context_->pipelines;
        const std::span<const DrawPacket> queue = this->queue(pass.queue);

        // R4a: scene colour LOAD/STORE; depth LOAD/STORE (forward-opaque,
        // compatibility) or read-only LOAD/NONE (sorted), from the graph.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, frameTargets.extent() };
        context.beginRendering(rendering);

        const VkViewport viewport{ 0.0f, 0.0f,
            static_cast<float>(frameTargets.extent().width),
            static_cast<float>(frameTargets.extent().height),
            0.0f, 1.0f };
        const VkRect2D scissor{ { 0, 0 }, frameTargets.extent() };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PipelineHandle lastBoundPipeline{};
        MaterialHandle lastBoundMaterial{};
        GeometryHandle lastBoundGeometry{};
        VkPipelineLayout activeLayout = VK_NULL_HANDLE;
        const VkDescriptorSet sceneSet = staged_.sceneSet;
        const VkDescriptorSet globalSet = staged_.globalSet;

        const std::span<const glm::mat4> previousTransforms =
            pass.queue == Queue::OpaqueForward ? staged_.opaqueForwardPreviousTransforms
            : pass.queue == Queue::Sorted ? staged_.sortedSurfacePreviousTransforms
            : staged_.compatibilityPreviousTransforms;
        for (size_t packetIndex = 0; packetIndex < queue.size(); ++packetIndex) {
            const DrawPacket& packet = queue[packetIndex];
            if (skipped(pass, packet, queue)) continue;
            auto* geometry = resources.geometries().get(packet.geometry);
            auto* material = resources.materials().get(packet.material);
            const bool mirrored = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0;
            const PipelineHandle effectivePipeline = mirrored
                ? material ? material->mirroredPipeline : PipelineHandle{}
                : packet.pipeline;
            const VulkanPipelineRecord* record =
                pipelineLibrary.get(effectivePipeline);
            if (!geometry || !material || !record ||
                record->pipeline == VK_NULL_HANDLE ||
                record->pipelineLayout == VK_NULL_HANDLE ||
                record->renderPass != pass.expectedPassClass ||
                record->colorAttachmentCount != pass.expectedColorAttachments) {
                continue;
            }

            if (effectivePipeline != lastBoundPipeline) {
                vkCmdBindPipeline(cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
                telemetry.recordPipelineBind(effectivePipeline.id);
                activeLayout = record->pipelineLayout;
                vkCmdBindDescriptorSets(cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                    0, 1, &globalSet, 0, nullptr);
                vkCmdBindDescriptorSets(cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                    3u,
                    1, &sceneSet, 0, nullptr);
                lastBoundPipeline = effectivePipeline;
                lastBoundMaterial = MaterialHandle{};
            }
            if (packet.material != lastBoundMaterial) {
                resources.bindMaterialDescriptors(cmd, frame, activeLayout);
                telemetry.recordMaterialBind(packet.material);
                lastBoundMaterial = packet.material;
            }
            if (packet.geometry != lastBoundGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0, 1,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd,
                    geometry->indexBuffer.buffer, 0,
                    toVkIndexType(geometry->indexFormat));
                lastBoundGeometry = packet.geometry;
            }

            CanonicalMotionPushConstants push{};
            push.mesh.renderMatrix = packet.worldTransform;
            push.mesh.materialIndex = packet.material.getIndex();
            push.mesh.padding[0] = static_cast<uint32_t>(staged_.debugView);
            push.mesh.padding[1] = mirrored ? 1u : 0u;
            // M9.1: forward-opaque writes velocity from last frame's transform;
            // M9.8e: transparency compares the same motion with the velocity.
            push.previousRenderMatrix = packetIndex < previousTransforms.size()
                ? previousTransforms[packetIndex] : packet.worldTransform;
            vkCmdPushConstants(cmd, activeLayout,
                VK_SHADER_STAGE_VERTEX_BIT |
                    VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(CanonicalMotionPushConstants), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1,
                packet.firstIndex, 0, 0);
            telemetry.recordDraw(telemetry.counters().drawTransparentForward,
                packet.indexCount / 3);
            if (telemetry.collecting()) {
                const MaterialClosureClass closure =
                    static_cast<MaterialClosureClass>(
                        material->packed.closureClass);
                if (closure == MaterialClosureClass::StandardForward) {
                    ++telemetry.counters().drawStandardForward;
                }
                else if (closure == MaterialClosureClass::ComplexForward) {
                    ++telemetry.counters().drawComplexForward;
                    for (uint32_t lobe = 0;
                        lobe < material->packed.complexLobeCount; ++lobe) {
                        const uint32_t type =
                            material->packed.complexLobes[lobe].type;
                        if (type < telemetry.counters().complexLobeDraws.size()) {
                            ++telemetry.counters().complexLobeDraws[type];
                        }
                    }
                }
                else if (closure == MaterialClosureClass::Unlit) {
                    ++telemetry.counters().drawUnlitForward;
                }
            }
        }
        context.endRendering();
    }

} // namespace Iridium
