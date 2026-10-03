#include "VulkanOpaqueFeature.h"

#include "VulkanExtensionHooks.h"
#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanGpuSceneState.h"
#include "VulkanMeshLayouts.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanResourceRegistry.h"
#include "VulkanShadowCasters.h"
#include "core/BuildFeatures.h"
#include "profiling/CpuProfiler.h"

#include <cmath>
#include <stdexcept>

namespace Iridium {

    namespace {
        void appendFnv1a(uint64_t& hash, const void* data, size_t size) noexcept {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        }

        // The history depth was rasterized from a complete camera transform.
        // Hashing only the projection allowed ordinary camera translation or
        // rotation to consume depth from a different view pose.
        uint64_t viewProjectionRevision(const glm::mat4& view,
            const glm::mat4& projection) noexcept {
            uint64_t hash = 1469598103934665603ull;
            appendFnv1a(hash, &view, sizeof(view));
            appendFnv1a(hash, &projection, sizeof(projection));
            return hash == 0u ? 1u : hash;
        }
    }

    void VulkanOpaqueFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        culler_.init(services_, {
                .depthOcclusionQuery = settings_.depthOcclusionQuery,
                .depthOcclusionRejection = settings_.depthOcclusionRejection,
                .lodErrorPixels = settings_.lodErrorPixels,
                .lodMaximumLevel = settings_.lodMaximumLevel,
                .lodHysteresisFraction = settings_.lodHysteresisFraction,
                .forceDirectGBufferReference = settings_.forceDirectGBufferReference,
                .lodOracle = activeIndirectOracle(context.extensions,
                    VulkanIndirectOracleView::OpaqueLod),
                .occlusionOracle = activeIndirectOracle(context.extensions,
                    VulkanIndirectOracleView::DepthOcclusion),
                .depthPyramid = &depthPyramid_,
            },
            createOpaqueCullPipelines(context.device,
                settings_.depthOcclusionRejection,
                context.meshLayouts.getGlobalSetLayout(),
                context.meshLayouts.getGpuSceneSetLayout()));
        if (settings_.depthPyramid) depthPyramid_.init(context.device,
            context.descriptors, context.allocator,
            context.meshLayouts.getGlobalSetLayout(),
            context.meshLayouts.getGpuSceneSetLayout());
        // The fixed wireframe/selection pipelines never read the swapchain.
        gBufferPass_ = std::make_unique<VkRenderPassWrapper>(&context.vk, nullptr,
            settings_.gBufferLayout);
        gBufferPipeline_ = std::make_unique<VkGraphicsPipeline>(&context.vk, nullptr,
            gBufferPass_.get(), context.meshLayouts.getGBufferPipelineLayout(),
            settings_.gBufferLayout);
    }

    void VulkanOpaqueFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        compactPass_ = ids.opaqueIndirect.compact;
        gBufferPassId_ = ids.gbuffer;
        depthPyramidPass_ = ids.depthPyramidBuild;
        historyImport_ = ids.depthPyramidHistory;
        // The history is bound after its images are rebuilt.
        historyBoundView_ = UINT32_MAX;
    }

    void VulkanOpaqueFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // R3b.7: the compute -> indirect barrier is the executor's, at "gbuffer".
        graph.registerPass(compactPass_, { this, &compactActive, &executeCompact });
        graph.registerPass(gBufferPassId_, { this, nullptr, &executeGBuffer });
        // Declared only with the depth pyramid.
        if (depthPyramidPass_.isValid())
            graph.registerPass(depthPyramidPass_, { this, nullptr, &executeDepthPyramid,
                "gpu.depth.occlusion-pyramid", GpuRangePlacement::BeforeBarriers });
    }

    void VulkanOpaqueFeature::destroy() noexcept {
        if (context_ != nullptr) culler_.destroy(context_->device);
        gBufferPipeline_.reset();
        gBufferPass_.reset();
        depthPyramid_.cleanup();
        context_ = nullptr;
    }

    void VulkanOpaqueFeature::rebuildDescriptors() {
        if (!settings_.depthPyramid) return;
        depthPyramid_.rebuild(context_->frameTargets);
        bindHistory(true);
    }

    void VulkanOpaqueFeature::onFrameFenceCompleted(uint32_t frameIndex,
        uint64_t completedSerial) {
        if (settings_.depthPyramid)
            depthPyramid_.onFrameFenceCompleted(frameIndex, completedSerial);
    }

    void VulkanOpaqueFeature::bindHistory(bool reset) {
        // R3b.9: one executor-owned global import, bound to the retained
        // view's history image. Switching views keeps each image's tracked
        // state; a rebuild (fresh images) starts both from Undefined.
        if (!settings_.depthPyramid || !historyImport_.isValid()) return;
        VulkanRenderGraphExecutor& graph = context_->graph;
        if (reset) {
            historyAccess_.fill(RenderGraph::Access::Undefined);
            historyBoundView_ = UINT32_MAX;
        }
        if (historyBoundView_ == retainedView_) return;
        if (historyBoundView_ < historyAccess_.size())
            historyAccess_[historyBoundView_] = graph.externalImageAccess(
                VulkanGlobalBinding, historyImport_);
        graph.bindExternalImage(VulkanGlobalBinding, historyImport_,
            depthPyramid_.historyImage(retainedView_),
            historyAccess_[retainedView_], ExternalSyncPolicy::executorOwned());
        historyBoundView_ = retainedView_;
    }

    void VulkanOpaqueFeature::updateView(const ViewTransportRecord& view,
        ViewHistoryContext history) {
        viewHistory_ = history;
        projectionRevision_ = viewProjectionRevision(view.view, view.projection);
        if (lodEnabled()) culler_.lodHistory().updateView(view, history);
    }

    void VulkanOpaqueFeature::publishScene(const GpuScenePackedTables& scene) {
        if (lodEnabled()) culler_.lodHistory().publish(scene);
    }

    DepthPyramidHistoryOwner VulkanOpaqueFeature::historyOwner() const noexcept {
        return {
            .viewIdentity = viewHistory_.identity,
            .sceneEpoch = retainedView_ == 0u
                ? context_->gpuScene.publishedEpoch()
                : viewHistory_.identity,
            .depthContentRevision = depthContentRevision_,
            .projectionRevision = projectionRevision_,
            .resetRevision = viewHistory_.resetRevision,
        };
    }

    void VulkanOpaqueFeature::prepareDepthHistory(
        std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> opaqueForwardQueue) {
        const uint32_t frame = context_->scheduler.currentFrameIndex();
        const VulkanIndirectScene scene = context_->gpuScene.indirectScene(frame);
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        depthContentRevision_ = shadowCasterRevision(scene, context_->resources, {
            .directPackets = opaqueQueue,
        });
        const uint64_t forwardRevision = shadowCasterRevision(scene,
            context_->resources, {
                .directPackets = opaqueForwardQueue,
            });
        appendFnv1a(depthContentRevision_, &forwardRevision,
            sizeof(forwardRevision));
        if (depthContentRevision_ == 0u)
            depthContentRevision_ = 1u;
        historyPrepared_ = true;

        if (!settings_.depthPyramid) {
            historyDecision_ = evaluateDepthPyramidHistory({});
            telemetry.counters().depthHistoryRejection = static_cast<uint32_t>(
                historyDecision_.rejection);
            return;
        }

        const auto& view = context_->gpuScene.views()[frame];
        bool projectionValid = true;
        for (uint32_t column = 0; column < 4u; ++column) {
            for (uint32_t row = 0; row < 4u; ++row) {
                projectionValid = projectionValid &&
                    std::isfinite(view.view[column][row]) &&
                    std::isfinite(view.projection[column][row]);
            }
        }
        const DepthPyramidHistoryOwner currentOwner = historyOwner();
        const auto& history = depthPyramid_.queuedHistory(retainedView_);
        const VkExtent2D extent = context_->frameTargets.extent();
        historyDecision_ = evaluateDepthPyramidHistory({
            .currentExtent = {extent.width, extent.height},
            .historyExtent = history.extent,
            .currentOwner = currentOwner,
            .historyOwner = history.owner,
            .currentFrameSerial = context_->scheduler.lastSubmittedSerial() + 1u,
            .historyFrameSerial = history.submissionSerial,
            .currentConvention = DeviceDepthConvention::ForwardZeroToOne,
            .historyConvention = history.convention,
            .enabled = true,
            .historyAvailable = history.available,
            .projectionValid = projectionValid,
        });
        telemetry.counters().depthHistoryEligible =
            historyDecision_.eligible ? 1u : 0u;
        telemetry.counters().depthHistoryRejection = static_cast<uint32_t>(
            historyDecision_.rejection);
    }

    void VulkanOpaqueFeature::submit(const FrameInputs& inputs) {
        CpuScope recordScope(context_->profiler, "cpu.render.record.gbuffer");
        staged_ = inputs;
        stagedCompact_ = false;
        stagedIndirectValid_ = false;
        // R3b.7: "gpu-scene.opaque.compact" runs or is skipped before
        // "gbuffer"; wireframe frames always skip it.
        if (!inputs.wireframe) {
            const uint32_t frame = context_->scheduler.currentFrameIndex();
            stagedIndirectValid_ = culler_.plan({
                .queue = inputs.opaqueQueue,
                .scene = context_->gpuScene.indirectScene(frame),
                .sceneBuffersMapped = context_->gpuScene.buffersMapped(frame),
                .assets = vulkanIndirectAssets(*context_),
                .queryOcclusion = settings_.depthOcclusionQuery &&
                    historyDecision_.eligible,
                .view = &context_->gpuScene.views()[frame],
            }, frame);
            stagedCompact_ = stagedIndirectValid_;
        }
        context_->graph.drainRegisteredThrough(gBufferPassId_);
    }

    void VulkanOpaqueFeature::recordDepthPyramid() {
        // A no-op unless the depth pyramid is declared.
        context_->graph.drainRegisteredThrough(depthPyramidPass_);
    }

    bool VulkanOpaqueFeature::compactActive(void* owner, const VulkanFrameRecordContext&) {
        return static_cast<const VulkanOpaqueFeature*>(owner)->stagedCompact_;
    }

    void VulkanOpaqueFeature::executeCompact(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanOpaqueFeature*>(owner);
        const uint32_t frame = context.frame.frameIndex;
        self.context_->telemetry.counters().dispatchRecorded +=
            self.culler_.recordCompaction(context.commandBuffer, frame, {
                .globalSet = self.staged_.globalSet,
                .gpuSceneSet = self.context_->gpuScene.descriptorSets()[frame],
                .retainedView = self.retainedView_,
            });
    }

    void VulkanOpaqueFeature::executeGBuffer(void* owner, VulkanPassContext& context) {
        static_cast<VulkanOpaqueFeature*>(owner)->recordGBuffer(context.commandBuffer,
            context.frame.frameIndex);
    }

    void VulkanOpaqueFeature::executeDepthPyramid(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanOpaqueFeature*>(owner);
        VulkanFrameTelemetry& telemetry = self.context_->telemetry;
        const uint32_t dispatches = self.depthPyramid_.record(context.commandBuffer,
            context.frame.frameIndex, self.retainedView_, self.historyOwner(),
            self.context_->scheduler.lastSubmittedSerial() + 1u);
        if (telemetry.collecting())
            telemetry.counters().dispatchRecorded += dispatches;
    }

    void VulkanOpaqueFeature::recordGBuffer(VkCommandBuffer cmd, uint32_t frame) {
        VulkanFrameScheduler& scheduler = context_->scheduler;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const VulkanPipelineLibrary& pipelineLibrary = context_->pipelines;
        const std::span<const DrawPacket> opaqueQueue = staged_.opaqueQueue;
        const std::span<const DrawPacket> selectionQueue = staged_.selectionQueue;
        const VkDescriptorSet globalSet = staged_.globalSet;
        const uint32_t debugView = static_cast<uint32_t>(staged_.debugView);

        VkRenderPassBeginInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpInfo.renderPass = gBufferPass_->getRenderPass();
        rpInfo.framebuffer = frameTargets.get(frame).gBufferFramebuffer;
        rpInfo.renderArea.extent = frameTargets.extent();

        std::array<VkClearValue, 6> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Normal
        clearValues[1].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // Diffuse / albedo
        clearValues[2].color = { {0.0f, 0.0f, 0.0f, 0.0f} }; // Emissive
        clearValues[3].color = { {0.0f, 0.0f, 0.0f, 1.0f} }; // F0 / roughness
        clearValues[4].color.uint32[0] = 0u;                  // Material / flags
        clearValues[5].depthStencil = { 1.0f, 0 };
        rpInfo.clearValueCount = 6;
        rpInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)frameTargets.extent().width;
        viewport.height = (float)frameTargets.extent().height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{ {0, 0}, rpInfo.renderArea.extent };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        const VkPipelineLayout meshLayout =
            context_->meshLayouts.getGBufferPipelineLayout();

        // Phase 1: the opaque scene.
        VulkanGpuRangeToken opaqueGpuRange =
            scheduler.beginGpuRange("gpu.gbuffer.opaque");

        PipelineHandle lastBoundPipeline{};
        MaterialHandle lastBoundMaterial{};
        GeometryHandle lastBoundGeometry{};

        if (staged_.wireframe) {
            // Editor wireframe is a deliberate fixed override, not a material PSO.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                gBufferPipeline_->getWireframePipeline());
            telemetry.recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::GBufferWireframe));
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                0, 1, &globalSet, 0, nullptr);

            for (const auto& packet : opaqueQueue) {
                auto* geometry = resources.geometries().get(packet.geometry);
                auto* material = resources.materials().get(packet.material);
                if (!geometry || !material) continue;

                if (packet.material != lastBoundMaterial) {
                    resources.bindMaterialDescriptors(cmd, frame, meshLayout);
                    telemetry.recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = geometry->vertexOffset;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(cmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = debugView;
                vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                telemetry.recordDraw(telemetry.counters().drawOpaque, packet.indexCount / 3);
            }
        }
        else {
            VkPipelineLayout activeLayout = VK_NULL_HANDLE;
            if (stagedIndirectValid_) {
                IVulkanIndirectStreamObserver* observer = nullptr;
                if constexpr (kQualificationBuild)
                    observer = context_->extensions.indirectStreamObserver();
                const VulkanIndirectStreamTap drawStream{
                    observer, VulkanIndirectStreamView::Opaque, frame };
                const VkDescriptorSet gpuSceneSet =
                    context_->gpuScene.descriptorSets()[frame];
                uint64_t oracleVisibleCommands = 0;
                for (uint32_t binIndex = 0;
                        binIndex < culler_.bins().size(); ++binIndex) {
                    const VulkanOpaqueIndirectCuller::Bin& bin = culler_.bins()[binIndex];
                    auto* geometry = resources.geometries().get(bin.geometry);
                    const VulkanPipelineRecord* record =
                        pipelineLibrary.get(bin.pipeline);

                    if (bin.pipeline != lastBoundPipeline) {
                        vkCmdBindPipeline(cmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            record->gpuSceneIndirectPipeline);
                        telemetry.recordPipelineBind(bin.pipeline.id);
                        activeLayout = record->pipelineLayout;
                        vkCmdBindDescriptorSets(cmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                            0u, 1u, &globalSet, 0u, nullptr);
                        vkCmdBindDescriptorSets(cmd,
                            VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                            4u, 1u, &gpuSceneSet, 0u, nullptr);
                        lastBoundPipeline = bin.pipeline;
                        lastBoundMaterial = MaterialHandle{};
                    }
                    if (bin.material != lastBoundMaterial) {
                        resources.bindMaterialDescriptors(cmd, frame, activeLayout);
                        telemetry.recordMaterialBind(bin.material);
                        lastBoundMaterial = bin.material;
                    }
                    // Commands carry exact signed base vertices; children in the
                    // same physical arena need no per-primitive buffer rebind.
                    const VkDeviceSize vertexOffset = 0;
                    vkCmdBindVertexBuffers(cmd, 0u, 1u,
                        &geometry->vertexBuffer.buffer, &vertexOffset);
                    vkCmdBindIndexBuffer(cmd,
                        geometry->indexBuffer.buffer, 0u,
                        toVkIndexType(geometry->indexFormat));

                    CanonicalMeshPushConstants push{};
                    push.materialIndex = bin.material.getIndex();
                    push.padding[0] = debugView;
                    vkCmdPushConstants(cmd, activeLayout,
                        VK_SHADER_STAGE_VERTEX_BIT |
                            VK_SHADER_STAGE_FRAGMENT_BIT,
                        0u, sizeof(push), &push);
                    vkCmdDrawIndexedIndirectCount(cmd,
                        culler_.buffers().commands[frame].buffer,
                        static_cast<VkDeviceSize>(bin.commandBegin) *
                            sizeof(GpuSceneIndexedIndirectCommand),
                        culler_.buffers().counts[frame].buffer,
                        static_cast<VkDeviceSize>(binIndex) * sizeof(uint32_t),
                        bin.commandCount,
                        sizeof(GpuSceneIndexedIndirectCommand));
                    drawStream.indirectDraw(record->gpuSceneIndirectPipeline,
                        geometry->vertexBuffer.buffer,
                        geometry->indexBuffer.buffer,
                        toVkIndexType(geometry->indexFormat),
                        push.padding[0],
                        culler_.buffers().commands[frame].buffer,
                        static_cast<VkDeviceSize>(bin.commandBegin) *
                            sizeof(GpuSceneIndexedIndirectCommand),
                        culler_.buffers().counts[frame].buffer,
                        static_cast<VkDeviceSize>(binIndex) * sizeof(uint32_t),
                        bin.commandCount);
                    for (uint32_t command = 0;
                            command < bin.commandCount; ++command) {
                        const DrawPacket& drawn = opaqueQueue[
                            bin.packetBegin + command];
                        if (cpuVisibilityOracleVisible(drawn)) {
                            telemetry.recordDraw(telemetry.counters().drawOpaque,
                                drawn.indexCount / 3u);
                            ++oracleVisibleCommands;
                        }
                    }
                }
                telemetry.counters().opaqueIndirectCommands = oracleVisibleCommands;
                telemetry.counters().opaqueIndirectBins = culler_.bins().size();
            }
            else for (const auto& packet : opaqueQueue) {
                if (!settings_.forceDirectGBufferReference && hasGpuScenePrimitive(packet) &&
                    !cpuVisibilityOracleVisible(packet)) {
                    continue;
                }
                auto* geometry = resources.geometries().get(packet.geometry);
                auto* material = resources.materials().get(packet.material);
                const VulkanPipelineRecord* record = pipelineLibrary.get(packet.pipeline);
                if (!geometry || !material) continue;

                // Invalid/stale handles and non-G-buffer records are not drawable here.
                if (!record || record->pipeline == VK_NULL_HANDLE ||
                    record->pipelineLayout == VK_NULL_HANDLE ||
                    record->renderPass != RenderPassClass::GBuffer) {
                    continue;
                }

                if (packet.pipeline != lastBoundPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, record->pipeline);
                    telemetry.recordPipelineBind(packet.pipeline.id);
                    activeLayout = record->pipelineLayout;
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeLayout,
                        0, 1, &globalSet, 0, nullptr);
                    lastBoundPipeline = packet.pipeline;
                    lastBoundMaterial = MaterialHandle{};
                }
                if (packet.material != lastBoundMaterial) {
                    resources.bindMaterialDescriptors(cmd, frame, activeLayout);
                    telemetry.recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = geometry->vertexOffset;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(cmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = debugView;
                vkCmdPushConstants(cmd, activeLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                telemetry.recordDraw(telemetry.counters().drawOpaque, packet.indexCount / 3);
            }
            if (!stagedIndirectValid_) {
                telemetry.counters().opaqueIndirectFallbackPackets = 0u;
                for (const DrawPacket& packet : opaqueQueue) {
                    if (settings_.forceDirectGBufferReference || !hasGpuScenePrimitive(packet) ||
                        cpuVisibilityOracleVisible(packet)) {
                        ++telemetry.counters().opaqueIndirectFallbackPackets;
                    }
                }
                telemetry.counters().opaqueIndirectFallbackReason =
                    static_cast<uint32_t>(culler_.indirectPlan().fallbackReason ==
                            GpuSceneIndirectFallbackReason::None
                        ? GpuSceneIndirectFallbackReason::InvalidPacket
                        : culler_.indirectPlan().fallbackReason);
            }
        }
        scheduler.endGpuRange(opaqueGpuRange);

        // Phase 2: the selection masks (depth testing disabled = X-ray).
        if (!selectionQueue.empty()) {
            VulkanGpuRangeToken selectionGpuRange =
                scheduler.beginGpuRange("gpu.gbuffer.selection");
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                gBufferPipeline_->getOutlinePipeline());
            telemetry.recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::SelectionMask));
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout,
                0, 1, &globalSet, 0, nullptr);

            lastBoundMaterial = MaterialHandle{};
            lastBoundGeometry = GeometryHandle{};

            for (const auto& packet : selectionQueue) {
                auto* geometry = resources.geometries().get(packet.geometry);
                auto* material = resources.materials().get(packet.material);

                if (!geometry || !material) continue;

                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = packet.selectionFeedback != 0
                    ? packet.selectionFeedback : 1u;

                if (packet.material != lastBoundMaterial) {
                    resources.bindMaterialDescriptors(cmd, frame, meshLayout);
                    telemetry.recordMaterialBind(packet.material);
                    lastBoundMaterial = packet.material;
                }
                if (packet.geometry != lastBoundGeometry) {
                    VkDeviceSize offset = geometry->vertexOffset;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(cmd, geometry->indexBuffer.buffer, 0,
                        toVkIndexType(geometry->indexFormat));
                    lastBoundGeometry = packet.geometry;
                }

                vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(push), &push);

                vkCmdDrawIndexed(cmd, packet.indexCount, 1, packet.firstIndex, 0, 0);
                telemetry.recordDraw(telemetry.counters().drawSelection, packet.indexCount / 3);
            }
            scheduler.endGpuRange(selectionGpuRange);
        }

        vkCmdEndRenderPass(cmd);
    }

} // namespace Iridium
