#include "VulkanShadowFeature.h"

#include "VulkanExtensionHooks.h"
#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanGpuSceneState.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceRegistry.h"
#include "VulkanUploadContext.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace Iridium {

    void VulkanShadowFeature::initVirtualShadows(VkDevice device,
        VulkanResourceAllocator& allocator, const VkPhysicalDeviceLimits& limits,
        const VirtualShadowResourceConfig& config) {
        clipPageSize_ = config.pageSizeTexels;
        virtualShadows_.init(device, allocator, limits, config, FrameCount);
    }

    void VulkanShadowFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        map_.init(context.device, context.allocator, context.uploads,
            context.descriptors, context.resources.textureTable().materialViewLayout(),
            context.resources.textureTable().samplerLayout(),
            context.meshLayouts.getGpuSceneSetLayout(), resolution_);
        culler_.init(setup_.services, IndirectViewKind::DirectionalShadow,
            createIndirectViewPipeline(context.device,
                IndirectViewKind::DirectionalShadow, map_.renderSetLayout(),
                context.meshLayouts.getGpuSceneSetLayout(), setup_.indirectLayout),
            setup_.indirectLayout,
            activeIndirectOracle(context.extensions,
                VulkanIndirectOracleView::DirectionalShadow));
        culler_.resize(512u, context.frameOpen);
    }

    void VulkanShadowFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        clipUploadPass_ = ids.virtualShadowClipUpload;
        compactPass_ = ids.directionalIndirect.compact;
        drawPass_ = ids.shadowDirectional;
        depthMarkPass_ = ids.virtualShadowDepthMark;
        requestReadbackPass_ = ids.virtualShadowRequestReadback;
        workingSet_ = ids.virtualShadowWorkingSet;
        if (virtualShadows_.initialized()) {
            for (uint32_t frame = 0; frame < FrameCount; ++frame) {
                const auto& buffer = virtualShadows_.workingSet(frame);
                context_->graph.bindExternalBuffer(frame, workingSet_,
                    buffer.buffer, buffer.size);
            }
            depthBindings_.fill(VK_NULL_HANDLE);
        }
    }

    void VulkanShadowFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // The VSM passes are declared only with the VSM working set.
        if (clipUploadPass_.isValid())
            graph.registerPass(clipUploadPass_, { this, nullptr, &executeClipUpload });
        // R3b.7: the compute -> indirect barrier is the executor's, at
        // "shadow.directional".
        graph.registerPass(compactPass_, { this, &compactActive, &executeCompact });
        graph.registerPass(drawPass_, { this, &drawActive, &executeDraw,
            "gpu.shadow.directional", GpuRangePlacement::AfterBarriers });
        if (depthMarkPass_.isValid())
            graph.registerPass(depthMarkPass_, { this, nullptr, &executeDepthMark,
                "gpu.shadow.virtual.depth-demand", GpuRangePlacement::BeforeBarriers });
        if (requestReadbackPass_.isValid())
            graph.registerPass(requestReadbackPass_, { this, nullptr,
                &executeRequestReadback });
    }

    void VulkanShadowFeature::destroy() noexcept {
        if (context_ != nullptr) culler_.destroy(context_->device);
        map_.cleanup();
        virtualShadows_.cleanup();
        context_ = nullptr;
    }

    void VulkanShadowFeature::growIndirectCapacity(uint32_t primitiveCapacity) {
        if (primitiveCapacity > culler_.primitiveCapacity())
            culler_.resize(primitiveCapacity, context_->frameOpen);
    }

    void VulkanShadowFeature::publishVirtualShadowClips(
        const ShadowCasterSubmission& casters,
        std::span<const DirectionalShadowFramePacket> shadows, uint32_t frameIndex) {
        CpuProfiler* const profiler = context_->profiler;
        CpuScope virtualClipScope(profiler, "cpu.render.virtual-shadow.clip-publication");
        clipPlan_.reset();
        bool gridRejected = false;
        const VulkanIndirectScene scene = context_->gpuScene.indirectScene(frameIndex);
        if (!shadows.empty()) {
            casterBoundsScratch_.clear();
            visitIndirectCasters(scene, casters, GpuSceneConsumerShadow,
                [&](const VulkanResolvedCaster& caster) {
                    casterBoundsScratch_.push_back({
                        caster.boundsSphereCenterWorld, caster.boundsSphereRadiusWorld});
                });
            DirectionalVirtualShadowClipConfig clipConfig{};
            clipConfig.lightOwner = shadows.front().selection.owner;
            clipConfig.lightForward = shadows.front().selection.lightForward;
            clipConfig.focusWorld = glm::vec3(
                context_->gpuScene.views()[frameIndex].cameraPosition);
            clipConfig.pageSizeTexels = clipPageSize_;
            clipConfig.virtualResolutionTexels = clipPageSize_ * 128u;
            // No static/dynamic classification is claimed: invalidate both
            // layers conservatively with the complete caster-content key.
            clipConfig.staticCasterRevision = shadowCasterRevision(scene,
                context_->resources, casters);
            clipConfig.dynamicCasterRevision = clipConfig.staticCasterRevision;
            clipPlan_ = clipPublisher_.publish(clipConfig, casterBoundsScratch_);
            if (clipPlan_) {
                DirectionalVirtualShadowMarkConfig markConfig{};
                markConfig.pageSizeTexels = clipPageSize_;
                markConfig.maximumUniquePageRequests =
                    virtualShadows_.info().requestCapacity;
                try {
                    (void)buildVirtualShadowFullViewPageGrid(markConfig, clipPlan_->levels(),
                        virtualShadows_.info().workingSetLayout.scratchCapacity);
                } catch (const std::invalid_argument&) {
                    // Never partially scan a view or publish an unaddressable
                    // clip stack. Conventional shadows remain the fallback.
                    clipPlan_.reset();
                    gridRejected = true;
                }
            }
        } else {
            clipPublisher_.reset();
        }
        if (profiler) {
            profiler->recordCounter("shadow.virtual.clip.available", clipPlan_.has_value());
            profiler->recordCounter("shadow.virtual.clip.grid_rejected", gridRejected);
            profiler->recordCounter("shadow.virtual.clip.levels",
                clipPlan_ ? clipPlan_->levelCount : 0);
            profiler->recordCounter("shadow.virtual.clip.projection_revision",
                clipPlan_ ? clipPlan_->clips[0].projectionRevision : 0);
            profiler->recordCounter("shadow.virtual.clip.caster_bounds",
                shadows.empty() ? 0 : casterBoundsScratch_.size());
        }
        frameClipPlans_[frameIndex] = clipPlan_;
        stagedClips_ = {};
        if (const auto& packet = frameClipPlans_[frameIndex])
            for (uint32_t i = 0; i < packet->levelCount; ++i)
                stagedClips_[i] = packDirectionalVirtualShadowClipLevel(packet->clips[i]);
        context_->graph.drainRegisteredThrough(clipUploadPass_);
    }

    void VulkanShadowFeature::submit(const ShadowCasterSubmission& casters,
        std::span<const DirectionalShadowFramePacket> shadows) {
        const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
        map_.updateFrame(frameIndex, shadows);
        // Publish live CPU clip packets even on conventional cache-hit frames.
        // Compute dispatch remains separately graph-gated.
        if (virtualShadows_.initialized())
            publishVirtualShadowClips(casters, shadows, frameIndex);
        stagedShadows_ = shadows;
        stagedCompact_ = false;
        stagedDraw_ = false;
        const bool hasUpdates = std::any_of(shadows.begin(), shadows.end(),
            [](const DirectionalShadowFramePacket& shadow) {
                return shadow.updateMask != 0u;
            });
        if (!hasUpdates) {
            // Both passes are skipped.
            context_->graph.drainRegisteredThrough(drawPass_);
            return;
        }
        for (const DirectionalShadowFramePacket& shadow : shadows)
            if (shadow.resolution != map_.resolution())
                throw std::invalid_argument(
                    "Directional shadow packet resolution does not match storage");

        CpuScope recordScope(context_->profiler, "cpu.render.record.shadow.directional");
        const VulkanIndirectScene scene = context_->gpuScene.indirectScene(frameIndex);
        resolveCasters(scene, casters, GpuSceneConsumerShadow, *scratch_);
        // R3b.7: compaction is its own pass ("shadow.directional.compact").
        auto& counters = context_->telemetry.counters();
        stagedIndirectValid_ = culler_.plan({
            .primitiveIndices = casters.gpuScenePrimitiveIndices,
            .membershipRevision = casters.membershipRevision,
            .lodErrorThreshold = settings_.lodErrorThreshold,
            .lodMaximumLevel = settings_.lodMaximumLevel,
            .forceDirectGBufferReference = settings_.forceDirectGBufferReference,
            .forceDirectShadowReference = settings_.forceDirectShadowReference,
            .scene = scene,
            .assets = vulkanIndirectAssets(*context_),
        }, directionalShadowWork(shadows), frameIndex);
        counters.shadowDirectionalIndirectFallbackReason =
            static_cast<uint32_t>(culler_.fallbackReason());
        if (culler_.membershipCacheHit())
            counters.shadowDirectionalMembershipCacheHit = 1u;
        stagedCompact_ = stagedIndirectValid_;
        stagedDraw_ = true;
        context_->graph.drainRegisteredThrough(drawPass_);
    }

    void VulkanShadowFeature::recordVirtualShadowDemand() {
        if (!virtualShadows_.initialized()) return;
        context_->graph.drainRegisteredThrough(requestReadbackPass_);
    }

    void VulkanShadowFeature::executeClipUpload(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanShadowFeature*>(owner);
        const uint32_t frameIndex = context.frame.frameIndex;
        const auto& range = self.virtualShadows_.info().workingSetLayout.clipLevels;
        if (range.size != sizeof(self.stagedClips_))
            throw std::logic_error("Virtual-shadow clip upload does not match the working-set ABI");
        vkCmdUpdateBuffer(context.commandBuffer,
            self.virtualShadows_.workingSet(frameIndex).buffer,
            range.offset, sizeof(self.stagedClips_), self.stagedClips_.data());
        if (CpuProfiler* profiler = self.context_->profiler)
            profiler->recordCounter("shadow.virtual.clip.upload_bytes",
                sizeof(self.stagedClips_));
    }

    bool VulkanShadowFeature::compactActive(void* owner, const VulkanFrameRecordContext&) {
        return static_cast<const VulkanShadowFeature*>(owner)->stagedCompact_;
    }

    void VulkanShadowFeature::executeCompact(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanShadowFeature*>(owner);
        const uint32_t frame = context.frame.frameIndex;
        self.context_->telemetry.counters().dispatchRecorded +=
            self.culler_.recordCompaction(context.commandBuffer, frame, {
                .set0 = self.map_.renderDescriptor(frame),
                .gpuScene = self.context_->gpuScene.descriptorSets()[frame],
            });
    }

    bool VulkanShadowFeature::drawActive(void* owner, const VulkanFrameRecordContext&) {
        return static_cast<const VulkanShadowFeature*>(owner)->stagedDraw_;
    }

    void VulkanShadowFeature::executeDraw(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanShadowFeature*>(owner);
        const VulkanFeatureContext& feature = *self.context_;
        VulkanCasterScratch& scratch = *self.scratch_;
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const bool indirectValid = self.stagedIndirectValid_;
        VulkanFrameTelemetry& telemetry = feature.telemetry;
        auto& counters = telemetry.counters();
        IVulkanIndirectOracle* const shadowOracle = activeIndirectOracle(
            feature.extensions, VulkanIndirectOracleView::DirectionalShadow);
        counters.shadowDirectionalDirectFallback =
            static_cast<uint64_t>(std::count_if(
                scratch.casters.begin(), scratch.casters.end(),
                [indirectValid](const VulkanResolvedCaster& caster) {
                    return !indirectValid || caster.gpuScenePrimitiveIndex ==
                        InvalidGpuSceneIndex;
                }));
        const VkPipelineLayout layout = self.map_.pipelineLayout();
        const VkDescriptorSet shadowSet = self.map_.renderDescriptor(frameIndex);

        for (const DirectionalShadowFramePacket& shadow : self.stagedShadows_) {
            scratch.visibility.resize(scratch.casters.size());
            for (size_t casterIndex = 0;
                casterIndex < scratch.casters.size(); ++casterIndex) {
                const VulkanResolvedCaster& caster = scratch.casters[casterIndex];
                const bool requiresCpuVisibility =
                    shadowOracle != nullptr || !indirectValid ||
                    caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex;
                if (!requiresCpuVisibility) {
                    scratch.visibility[casterIndex] = 0u;
                    continue;
                }
                const uint32_t visibleMask = directionalShadowCasterCascadeMask(
                    shadow.plan, caster.boundsSphereCenterWorld,
                    caster.boundsSphereRadiusWorld, shadow.updateMask);
                scratch.visibility[casterIndex] = static_cast<uint8_t>(visibleMask);
                if (telemetry.collecting()) {
                    counters.shadowDirectionalCastersTested +=
                        std::popcount(shadow.updateMask);
                    counters.shadowDirectionalCastersCulled +=
                        std::popcount(shadow.updateMask & ~visibleMask);
                }
            }
            for (uint32_t cascade = 0;
                cascade < kDirectionalShadowCascadeCount; ++cascade) {
                if ((shadow.updateMask & (1u << cascade)) == 0u) continue;
                self.map_.beginCascade(cmd, shadow.shadowIndex, cascade);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    layout, 0, 1, &shadowSet, 0, nullptr);
                const uint32_t layer = shadow.shadowIndex *
                    kDirectionalShadowCascadeCount + cascade;
                const uint8_t cascadeBit = static_cast<uint8_t>(1u << cascade);
                bool materialDescriptorsBound = false;
                if (indirectValid) {
                    vkCmdBindDescriptorSets(cmd,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                        3u, 1u, &feature.gpuScene.descriptorSets()[frameIndex],
                        0u, nullptr);
                    if (self.culler_.anyAlphaMaskedBin()) {
                        feature.resources.bindMaterialDescriptors(cmd, frameIndex, layout);
                        materialDescriptorsBound = true;
                    }
                    const uint32_t workIndex = self.culler_.workIndex(layer);
                    counters.shadowDirectionalIndirectBins +=
                        self.culler_.recordDraws(cmd, frameIndex, workIndex, {
                            .layout = layout,
                            .owner = &self.map_,
                            .pipeline = [](const void* owner, bool alphaMasked,
                                bool doubleSided) {
                                return static_cast<const VulkanDirectionalShadowMap*>(
                                    owner)->pipeline(alphaMasked, doubleSided, true);
                            },
                            .pushSlotWord = true,
                            .slotWord = layer,
                        });
                    if (shadowOracle != nullptr) {
                        const DensityLodContext lodContext{
                            shadow.plan.cascades[cascade].worldUnitsPerTexel,
                            self.settings_.lodErrorThreshold };
                        self.culler_.emitExpectations(*shadowOracle,
                            frameIndex, workIndex, scratch.casters,
                            scratch.visibility, cascadeBit,
                            densityLodMetric(lodContext));
                        recordIndirectOracleDraws(feature, scratch, cascadeBit,
                            counters.drawShadowDirectional,
                            counters.shadowDirectionalIndirectCommands,
                            counters.drawShadowDirectionalAlphaMask);
                    }
                }
                recordShadowDirectDraws(feature, cmd, frameIndex, scratch, {
                    .layout = layout,
                    .pipelineOwner = &self.map_,
                    .pipeline = [](const void* owner, bool alphaMasked,
                        bool doubleSided) {
                        return static_cast<const VulkanDirectionalShadowMap*>(
                            owner)->pipeline(alphaMasked, doubleSided);
                    },
                    .slotWord = layer,
                    .visibilityBit = cascadeBit,
                    .indirectValid = indirectValid,
                    .drawCounter = &counters.drawShadowDirectional,
                    .alphaMaskCounter = &counters.drawShadowDirectionalAlphaMask,
                }, materialDescriptorsBound);
                self.map_.endCascade(cmd);
            }
        }
    }

    void VulkanShadowFeature::executeDepthMark(void* owner, VulkanPassContext& context) {
        auto& self = *static_cast<VulkanShadowFeature*>(owner);
        const uint32_t slot = context.frame.frameIndex;
        const auto& packet = self.frameClipPlans_[slot];
        if (!packet) {
            // No clip is read for zero marks; still overwrite output/counts.
            self.virtualShadows_.markingPass().recordCompaction(context.commandBuffer,
                slot, 0, 1);
            return;
        }
        const VulkanFrameContextTargets& targets = self.context_->frameTargets.get(slot);
        const auto& view = self.context_->gpuScene.views()[slot];
        if (self.depthBindings_[slot] != targets.depth.view) {
            self.virtualShadows_.fullViewPass().bindDepth(slot, targets.depth.view,
                self.virtualShadows_.depthSampler(), context.frame.sceneExtent,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            self.depthBindings_[slot] = targets.depth.view;
        }
        DirectionalVirtualShadowMarkConfig markConfig{};
        markConfig.pageSizeTexels = self.clipPageSize_;
        markConfig.maximumUniquePageRequests =
            self.virtualShadows_.info().requestCapacity;
        const auto cells = self.virtualShadows_.fullViewPass().record(
            context.commandBuffer, slot, markConfig, packet->levels(),
            view.inverseView * view.inverseProjection);
        self.virtualShadows_.markingPass().recordCompaction(context.commandBuffer,
            slot, cells, packet->levelCount);
    }

    void VulkanShadowFeature::executeRequestReadback(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanShadowFeature*>(owner);
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t slot = context.frame.frameIndex;
        if constexpr (kQualificationBuild) {
            // Explicit qualification copies the exact depth consumed above.
            const auto& packet = self.frameClipPlans_[slot];
            const VulkanExtensionHooks& hooks = self.context_->extensions;
            if (packet && hooks.graphHooks().virtualShadowDepthSnapshot) {
                const VulkanFrameContextTargets& targets =
                    self.context_->frameTargets.get(slot);
                const auto& view = self.context_->gpuScene.views()[slot];
                hooks.notify({ .point = VulkanHookPoint::VirtualShadowDepthSnapshot,
                    .cmd = cmd, .slot = slot,
                    .payload = VulkanVirtualShadowDepthPayload{ &targets.depth,
                        context.frame.sceneExtent,
                        view.inverseView * view.inverseProjection } });
            }
        }
        const auto& layout = self.virtualShadows_.info().workingSetLayout;
        const auto& readback = self.virtualShadows_.requestReadback(slot);
        const std::array<VkBufferCopy, 2> copies{{
            {layout.telemetry.offset, 0, layout.telemetry.size},
            {layout.outputRequests.offset, layout.telemetry.size, layout.outputRequests.size}}};
        vkCmdCopyBuffer(cmd, self.virtualShadows_.workingSet(slot).buffer,
            readback.buffer, 2, copies.data());
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = readback.buffer;
        barrier.size = readback.size;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &barrier, 0, nullptr);
        self.readbackPending_[slot] = true;
    }

    void VulkanShadowFeature::collectVirtualShadowRequests(uint32_t slot) {
        if (!readbackPending_[slot]) return;
        const auto& buffer = virtualShadows_.requestReadback(slot);
        PackedDirectionalVirtualShadowGpuCompactionTelemetry telemetry{};
        std::memcpy(&telemetry, buffer.mapped, sizeof(telemetry));
        const auto& info = virtualShadows_.info();
        const auto& packet = frameClipPlans_[slot];
        if (telemetry.abiMismatchMarks || telemetry.invalidLevelMarks ||
            telemetry.outputRequestCount > info.requestCapacity ||
            telemetry.outputRequestCount > telemetry.uniquePagesBeforeCapacity ||
            telemetry.requestCapacityOverflow != telemetry.uniquePagesBeforeCapacity - telemetry.outputRequestCount ||
            (!packet && telemetry.outputRequestCount))
            throw std::runtime_error("Live virtual-shadow request telemetry violates the published slot contract");
        const auto* bytes = static_cast<const std::byte*>(buffer.mapped) + info.workingSetLayout.telemetry.size;
        IVulkanIndirectOracle* oracle = packet
            ? activeIndirectOracle(context_->extensions,
                VulkanIndirectOracleView::VirtualShadowDepth)
            : nullptr;
        if (oracle != nullptr && !oracle->beginVirtualShadowVerify({
                .slot = slot,
                .levels = packet->levels(),
                .pageSizeTexels = clipPageSize_,
                .requestCapacity = info.requestCapacity,
                .uniquePagesBeforeCapacity = telemetry.uniquePagesBeforeCapacity,
                .outputRequestCount = telemetry.outputRequestCount,
                .requestCapacityOverflow = telemetry.requestCapacityOverflow,
                .requestCapacityDroppedSamples =
                    telemetry.requestCapacityDroppedSamples }))
            oracle = nullptr;
        for (uint32_t i = 0; i < telemetry.outputRequestCount; ++i) {
            PackedDirectionalVirtualShadowGpuRequest request{};
            std::memcpy(&request, bytes + i * sizeof(request), sizeof(request));
            if (!packet || request.selectedLevelIndex >= packet->levelCount)
                throw std::runtime_error("Live virtual-shadow request selects an unpublished clip");
            const auto ownedRequest = unpackDirectionalVirtualShadowGpuRequest(request,
                packet->levels(), clipPageSize_);
            if (oracle != nullptr) oracle->verifyVirtualShadowRequest(i, ownedRequest);
        }
        if (CpuProfiler* profiler = context_->profiler) {
            profiler->recordCounter("shadow.virtual.requests.unique", telemetry.uniquePagesBeforeCapacity);
            profiler->recordCounter("shadow.virtual.requests.output", telemetry.outputRequestCount);
            profiler->recordCounter("shadow.virtual.requests.overflow", telemetry.requestCapacityOverflow);
            profiler->recordCounter("shadow.virtual.requests.dropped_samples", telemetry.requestCapacityDroppedSamples);
            profiler->recordCounter("shadow.virtual.requests.validated_slot", slot);
            profiler->recordCounter("shadow.virtual.requests.readback_bytes", info.requestedReadbackBytes);
            profiler->recordCounter("shadow.virtual.oracle.compared_requests",
                oracle != nullptr ? oracle->comparedVirtualShadowRequests() : 0);
            profiler->recordCounter("shadow.virtual.oracle.validated", oracle != nullptr);
        }
        readbackPending_[slot] = false;
    }

} // namespace Iridium
