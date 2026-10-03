#include "VulkanWeightedOitFeature.h"

#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceRegistry.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/transparency/WeightedOit.h"

#include <cstring>
#include <stdexcept>

namespace Iridium {

    void VulkanWeightedOitFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        pass_.init(context.device, context.descriptors,
            context.meshLayouts.getForwardPipelineLayout());
    }

    void VulkanWeightedOitFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        accumulatePass_ = ids.oitAccumulate;
        resolvePass_ = ids.oitResolve;
    }

    void VulkanWeightedOitFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // Declared only while WeightedOIT is resident.
        if (!accumulatePass_.isValid()) return;
        graph.registerPass(accumulatePass_, { this, &active, &executeAccumulation,
            "gpu.transparency.oit.accumulate", GpuRangePlacement::AfterBarriers });
        graph.registerPass(resolvePass_, { this, &active, &executeResolve,
            "gpu.transparency.oit.resolve", GpuRangePlacement::AfterBarriers });
    }

    void VulkanWeightedOitFeature::destroy() noexcept {
        pass_.clearDescriptors();
        if (context_ != nullptr) {
            for (VulkanBufferResource& buffer : instanceBuffers_)
                context_->allocator.destroy(buffer);
        }
        pass_.cleanup();
        instanceCapacity_ = 0u;
        context_ = nullptr;
    }

    void VulkanWeightedOitFeature::rebuildDescriptors() {
        pass_.rebuildDescriptors(context_->frameTargets);
    }

    void VulkanWeightedOitFeature::setInstanceCapacity(uint32_t capacity) {
        if (capacity == instanceCapacity_) return;
        if (capacity > kWeightedOitMaximumInstanceCount) {
            throw std::out_of_range(
                "WeightedOIT instance capacity exceeds the production bound");
        }

        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> replacement{};
        if (capacity != 0u) {
            try {
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(
                    weightedOitInstanceStreamBytes(capacity));
                for (VulkanBufferResource& buffer : replacement) {
                    buffer = context_->allocator.createBuffer(bytes,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::Uniform);
                }
            }
            catch (...) {
                for (VulkanBufferResource& buffer : replacement)
                    context_->allocator.destroy(buffer);
                throw;
            }
        }

        for (VulkanBufferResource& buffer : instanceBuffers_)
            context_->allocator.destroy(buffer);
        instanceBuffers_ = replacement;
        instanceCapacity_ = capacity;
    }

    void VulkanWeightedOitFeature::record(const FrameInputs& inputs) {
        staged_ = inputs;
        if (staged_.execute) prepareInstances(context_->scheduler.currentFrameIndex());
        context_->graph.drainRegisteredThrough(resolvePass_);
    }

    void VulkanWeightedOitFeature::prepareInstances(uint32_t oitFrameIndex) {
        VulkanResourceRegistry& resources = context_->resources;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanBufferResource& instanceBuffer =
            instanceBuffers_[oitFrameIndex];
        if (instanceBuffer.mapped == nullptr ||
            instanceBuffer.size < weightedOitInstanceStreamBytes(
                instanceCapacity_)) {
            throw std::logic_error(
                "WeightedOIT instance stream is not resident");
        }
        const uint32_t oitQueueSize = static_cast<uint32_t>(
            staged_.sortedSurfaceQueue.size());
        preparedInstanceCount_ = 0u;
        for (uint32_t ordinal = 0u; ordinal < oitQueueSize; ++ordinal) {
            const uint32_t packetIndex = weightedOitPermutationIndex(
                ordinal, oitQueueSize, orderSeed_);
            const DrawPacket& packet = staged_.sortedSurfaceQueue[packetIndex];
            if (!isWeightedOitPacket(packet) ||
                resources.geometries().get(packet.geometry) == nullptr ||
                resources.materials().get(packet.material) == nullptr) {
                continue;
            }
            if (packet.firstInstanceTransform == UINT32_MAX) {
                std::memcpy(static_cast<std::byte*>(instanceBuffer.mapped) +
                        weightedOitInstanceStreamBytes(
                            preparedInstanceCount_),
                    &packet.worldTransform, sizeof(packet.worldTransform));
            }
            else {
                std::memcpy(static_cast<std::byte*>(instanceBuffer.mapped) +
                        weightedOitInstanceStreamBytes(
                            preparedInstanceCount_),
                    staged_.instanceTransforms.data() +
                        packet.firstInstanceTransform,
                    weightedOitInstanceStreamBytes(packet.instanceCount));
            }
            preparedInstanceCount_ += packet.instanceCount;
        }
        if (telemetry.collecting()) {
            telemetry.counters().weightedOitInstances = preparedInstanceCount_;
            telemetry.counters().weightedOitInstanceUploadBytes =
                weightedOitInstanceStreamBytes(preparedInstanceCount_);
        }

    }

    bool VulkanWeightedOitFeature::active(void* owner, const VulkanFrameRecordContext&) {
        return static_cast<const VulkanWeightedOitFeature*>(owner)->staged_.execute;
    }

    void VulkanWeightedOitFeature::executeAccumulation(void* owner,
        VulkanPassContext& context) {
        static_cast<VulkanWeightedOitFeature*>(owner)->recordAccumulation(context);
    }

    void VulkanWeightedOitFeature::executeResolve(void* owner,
        VulkanPassContext& context) {
        static_cast<VulkanWeightedOitFeature*>(owner)->recordResolve(context);
    }

    void VulkanWeightedOitFeature::recordAccumulation(VulkanPassContext& context) {
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanResourceRegistry& resources = context_->resources;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        const uint32_t oitFrameIndex = context.frame.frameIndex;
        const VkCommandBuffer commandBuffer = context.commandBuffer;
        const VulkanFrameContextTargets& oitTargets =
            frameTargets.get(oitFrameIndex);
        const VkExtent2D oitExtent = frameTargets.extent();
        const VkViewport viewport{ 0.0f, 0.0f,
            static_cast<float>(oitExtent.width),
            static_cast<float>(oitExtent.height), 0.0f, 1.0f };
        const VkRect2D scissor{ { 0, 0 }, oitExtent };
        VulkanBufferResource& instanceBuffer = instanceBuffers_[oitFrameIndex];
        const uint32_t oitQueueSize = static_cast<uint32_t>(
            staged_.sortedSurfaceQueue.size());
        std::array<VkClearValue, 2> clears{};
        clears[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        clears[1].color = { { 1.0f, 0.0f, 0.0f, 0.0f } };
        VkRenderPassBeginInfo accumulationInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        accumulationInfo.renderPass =
            pass_.accumulationRenderPass();
        accumulationInfo.framebuffer =
            oitTargets.weightedOitAccumulationFramebuffer;
        accumulationInfo.renderArea.extent = oitExtent;
        accumulationInfo.clearValueCount = static_cast<uint32_t>(
            clears.size());
        accumulationInfo.pClearValues = clears.data();
        vkCmdBeginRenderPass(commandBuffer, &accumulationInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(commandBuffer, 0u, 1u, &viewport);
        vkCmdSetScissor(commandBuffer, 0u, 1u, &scissor);

        const VkPipelineLayout accumulationLayout =
            pass_.accumulationPipelineLayout();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pass_.accumulationPipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::WeightedOitAccumulation));
        const VkDescriptorSet globalSet =
            staged_.globalSet;
        const VkDescriptorSet sceneSet = staged_.sceneSet;
        vkCmdBindDescriptorSets(commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS, accumulationLayout,
            0u, 1u, &globalSet, 0u, nullptr);
        vkCmdBindDescriptorSets(commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS, accumulationLayout,
            3u, 1u, &sceneSet, 0u, nullptr);

        const VkDeviceSize instanceOffset = 0u;
        vkCmdBindVertexBuffers(commandBuffer, 1u, 1u,
            &instanceBuffer.buffer, &instanceOffset);
        MaterialHandle lastMaterial{};
        GeometryHandle lastGeometry{};
        const DrawPacket* batchPacket = nullptr;
        VulkanGeometryPayload* batchGeometry = nullptr;
        VulkanMaterialPayload* batchMaterial = nullptr;
        uint32_t batchFirstInstance = 0u;
        uint32_t batchInstanceCount = 0u;
        const auto sameBatchState = [](const DrawPacket& lhs,
                const DrawPacket& rhs) {
            constexpr uint32_t MirroredMask = TransparentWorkMirrored;
            return lhs.geometry == rhs.geometry &&
                lhs.material == rhs.material &&
                lhs.indexCount == rhs.indexCount &&
                lhs.firstIndex == rhs.firstIndex &&
                (lhs.transparentWorkFlags & MirroredMask) ==
                    (rhs.transparentWorkFlags & MirroredMask);
        };
        const auto flushBatch = [&] {
            if (batchInstanceCount == 0u || batchPacket == nullptr ||
                batchGeometry == nullptr || batchMaterial == nullptr) {
                return;
            }
            if (batchPacket->material != lastMaterial) {
                resources.bindMaterialDescriptors(commandBuffer, oitFrameIndex, accumulationLayout);
                telemetry.recordMaterialBind(batchPacket->material);
                lastMaterial = batchPacket->material;
            }
            if (batchPacket->geometry != lastGeometry) {
                const VkDeviceSize offset = batchGeometry->vertexOffset;
                vkCmdBindVertexBuffers(commandBuffer, 0u, 1u,
                    &batchGeometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(commandBuffer,
                    batchGeometry->indexBuffer.buffer, 0u,
                    toVkIndexType(batchGeometry->indexFormat));
                lastGeometry = batchPacket->geometry;
            }
            const bool mirrored = (batchPacket->transparentWorkFlags &
                TransparentWorkMirrored) != 0u;
            CanonicalMeshPushConstants push{};
            push.renderMatrix = glm::mat4(1.0f);
            push.materialIndex = batchPacket->material.getIndex();
            push.padding[0] = static_cast<uint32_t>(staged_.debugView);
            push.padding[1] = mirrored ? 1u : 0u;
            vkCmdPushConstants(commandBuffer, accumulationLayout,
                VK_SHADER_STAGE_VERTEX_BIT |
                    VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(commandBuffer, batchPacket->indexCount,
                batchInstanceCount, batchPacket->firstIndex, 0,
                batchFirstInstance);
            telemetry.recordDraw(telemetry.counters().drawWeightedOitAccumulation,
                static_cast<uint64_t>(batchPacket->indexCount / 3u) *
                    batchInstanceCount);
            if (telemetry.collecting()) {
                const MaterialClosureClass closure =
                    static_cast<MaterialClosureClass>(
                        batchMaterial->packed.closureClass);
                if (closure == MaterialClosureClass::StandardForward)
                    ++telemetry.counters().drawStandardForward;
                else if (closure == MaterialClosureClass::ComplexForward) {
                    ++telemetry.counters().drawComplexForward;
                    for (uint32_t lobe = 0u;
                        lobe < batchMaterial->packed.complexLobeCount; ++lobe) {
                        const uint32_t type =
                            batchMaterial->packed.complexLobes[lobe].type;
                        if (type < telemetry.counters().complexLobeDraws.size())
                            ++telemetry.counters().complexLobeDraws[type];
                    }
                }
                else if (closure == MaterialClosureClass::Unlit)
                    ++telemetry.counters().drawUnlitForward;
            }
            batchPacket = nullptr;
            batchGeometry = nullptr;
            batchMaterial = nullptr;
            batchInstanceCount = 0u;
        };
        uint32_t instanceCursor = 0u;
        for (uint32_t ordinal = 0u; ordinal < oitQueueSize; ++ordinal) {
            const uint32_t packetIndex = weightedOitPermutationIndex(
                ordinal, oitQueueSize, orderSeed_);
            const DrawPacket& packet = staged_.sortedSurfaceQueue[packetIndex];
            if (!isWeightedOitPacket(packet)) continue;
            VulkanGeometryPayload* geometry = resources.geometries().get(
                packet.geometry);
            VulkanMaterialPayload* material = resources.materials().get(
                packet.material);
            if (geometry == nullptr || material == nullptr) continue;
            if (batchPacket != nullptr &&
                !sameBatchState(*batchPacket, packet)) {
                flushBatch();
            }
            if (batchPacket == nullptr) {
                batchPacket = &packet;
                batchGeometry = geometry;
                batchMaterial = material;
                batchFirstInstance = instanceCursor;
            }
            batchInstanceCount += packet.instanceCount;
            instanceCursor += packet.instanceCount;
        }
        flushBatch();
        if (instanceCursor != preparedInstanceCount_) {
            throw std::logic_error(
                "WeightedOIT instance preparation changed during recording");
        }
        vkCmdEndRenderPass(commandBuffer);
    }

    void VulkanWeightedOitFeature::recordResolve(VulkanPassContext& context) {
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        const uint32_t oitFrameIndex = context.frame.frameIndex;
        const VkCommandBuffer commandBuffer = context.commandBuffer;
        const VulkanFrameContextTargets& oitTargets =
            frameTargets.get(oitFrameIndex);
        const VkExtent2D oitExtent = frameTargets.extent();
        const VkViewport viewport{ 0.0f, 0.0f,
            static_cast<float>(oitExtent.width),
            static_cast<float>(oitExtent.height), 0.0f, 1.0f };
        const VkRect2D scissor{ { 0, 0 }, oitExtent };
        VkRenderPassBeginInfo resolveInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        resolveInfo.renderPass = pass_.resolveRenderPass();
        resolveInfo.framebuffer =
            oitTargets.weightedOitResolveFramebuffer;
        resolveInfo.renderArea.extent = oitExtent;
        vkCmdBeginRenderPass(commandBuffer, &resolveInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(commandBuffer, 0u, 1u, &viewport);
        vkCmdSetScissor(commandBuffer, 0u, 1u, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pass_.resolvePipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::WeightedOitResolve));
        const VkPipelineLayout resolveLayout =
            pass_.resolvePipelineLayout();
        const VkDescriptorSet resolveSet =
            pass_.resolveDescriptorSet(oitFrameIndex);
        vkCmdBindDescriptorSets(commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS, resolveLayout,
            0u, 1u, &resolveSet, 0u, nullptr);
        const uint32_t resolveDebugView =
            static_cast<uint32_t>(staged_.debugView);
        vkCmdPushConstants(commandBuffer, resolveLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0u,
            sizeof(resolveDebugView), &resolveDebugView);
        vkCmdDraw(commandBuffer, 3u, 1u, 0u, 0u);
        telemetry.recordDraw(telemetry.counters().drawWeightedOitResolve, 1u);
        vkCmdEndRenderPass(commandBuffer);
    }

} // namespace Iridium
