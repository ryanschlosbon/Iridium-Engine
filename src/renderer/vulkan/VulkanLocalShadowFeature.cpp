#include "VulkanLocalShadowFeature.h"

#include "VkContext.h"
#include "VulkanClusterLightingFeature.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanGpuSceneState.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceRegistry.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <stdexcept>

namespace Iridium {

    void VulkanLocalShadowFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        const VkDescriptorSetLayout materialLayout =
            context.resources.textureTable().materialViewLayout();
        const VkDescriptorSetLayout samplerLayout =
            context.resources.textureTable().samplerLayout();
        const VkDescriptorSetLayout gpuSceneLayout =
            context.meshLayouts.getGpuSceneSetLayout();
        if (context.vk.getPhysicalDeviceProperties().limits.maxUniformBufferRange <
            sizeof(VulkanSpotShadowData)) {
            throw std::runtime_error(
                "Vulkan uniform-buffer range cannot hold the spot shadow table");
        }
        spot_.init(context.device, context.allocator, context.uploads,
            context.descriptors, materialLayout, samplerLayout, gpuSceneLayout,
            spotAtlasResolution_);
        if (setup_.indirectLayout == VK_NULL_HANDLE)
            throw std::logic_error(
                "spot-shadow indirect resources require the shared shadow layout");
        spotCuller_.init(setup_.services, IndirectViewKind::SpotShadow,
            createIndirectViewPipeline(context.device, IndirectViewKind::SpotShadow,
                spot_.renderSetLayout(), gpuSceneLayout, setup_.indirectLayout),
            setup_.indirectLayout,
            activeIndirectOracle(context.extensions, VulkanIndirectOracleView::SpotShadow));
        spotCuller_.resize(512u, context.frameOpen);
        if (context.vk.getPhysicalDeviceProperties().limits.maxUniformBufferRange <
            sizeof(VulkanPointShadowData)) {
            throw std::runtime_error(
                "Vulkan uniform-buffer range cannot hold the point shadow table");
        }
        point_.init(context.device, context.allocator, context.uploads,
            context.descriptors, materialLayout, samplerLayout, gpuSceneLayout,
            pointPoolCapacities_);
        if (setup_.indirectLayout == VK_NULL_HANDLE)
            throw std::logic_error(
                "point-shadow indirect resources require the shared shadow layout");
        pointCuller_.init(setup_.services, IndirectViewKind::PointShadow,
            createIndirectViewPipeline(context.device, IndirectViewKind::PointShadow,
                point_.renderSetLayout(), gpuSceneLayout, setup_.indirectLayout),
            setup_.indirectLayout,
            activeIndirectOracle(context.extensions, VulkanIndirectOracleView::PointShadow));
        pointCuller_.resize(512u, context.frameOpen);
    }

    void VulkanLocalShadowFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        spotCompactPass_ = ids.spotIndirect.compact;
        spotDrawPass_ = ids.shadowSpot;
        pointCompactPass_ = ids.pointIndirect.compact;
        pointDrawPass_ = ids.shadowPoint;
    }

    void VulkanLocalShadowFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // R3b.7: the compute -> indirect barriers are the executor's, at the
        // drawing passes.
        graph.registerPass(spotCompactPass_, { this, &spotCompactActive,
            &executeSpotCompact });
        // R4a: dynamic rendering. The executor moves each map whole
        // SampledRead -> DepthAttachmentWrite (contents kept: tiles and faces
        // not rendered this frame stay valid); the next reader moves it back.
        // The spot atlas is one rendering instance with per-tile clears.
        graph.registerPass(spotDrawPass_, { this, &spotDrawActive, &executeSpotDraw,
            "gpu.shadow.spot", GpuRangePlacement::AfterBarriers, true });
        graph.registerPass(pointCompactPass_, { this, &pointCompactActive,
            &executePointCompact });
        // Point faces are distinct layers: one rendering instance per face.
        graph.registerPass(pointDrawPass_, { this, &pointDrawActive, &executePointDraw,
            "gpu.shadow.point", GpuRangePlacement::AfterBarriers, true });
    }

    void VulkanLocalShadowFeature::destroy() noexcept {
        if (context_ != nullptr) {
            spotCuller_.destroy(context_->device);
            pointCuller_.destroy(context_->device);
        }
        spot_.cleanup();
        point_.cleanup();
        context_ = nullptr;
    }

    void VulkanLocalShadowFeature::growIndirectCapacity(uint32_t primitiveCapacity) {
        if (primitiveCapacity > spotCuller_.primitiveCapacity())
            spotCuller_.resize(primitiveCapacity, context_->frameOpen);
        if (primitiveCapacity > pointCuller_.primitiveCapacity())
            pointCuller_.resize(primitiveCapacity, context_->frameOpen);
    }

    bool VulkanLocalShadowFeature::planCuller(VulkanIndirectViewCuller& culler,
        const ShadowCasterSubmission& casters, const IndirectWorkEnumeration& work,
        uint32_t frameIndex) {
        return culler.plan({
            .primitiveIndices = casters.gpuScenePrimitiveIndices,
            .membershipRevision = casters.membershipRevision,
            .lodErrorThreshold = settings_.lodErrorThreshold,
            .lodMaximumLevel = settings_.lodMaximumLevel,
            .forceDirectGBufferReference = settings_.forceDirectGBufferReference,
            .forceDirectShadowReference = settings_.forceDirectShadowReference,
            .scene = context_->gpuScene.indirectScene(frameIndex),
            .assets = vulkanIndirectAssets(*context_),
        }, work, frameIndex);
    }

    void VulkanLocalShadowFeature::submitSpot(const ShadowCasterSubmission& casters,
        std::span<const SpotShadowFramePacket> shadows,
        VulkanClusterLightingFeature& clusters) {
        const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
        spot_.updateFrame(frameIndex, shadows);

        spotMappingScratch_.assign(clusters.lightRecordCapacity(),
            kInvalidShadowDataSlot);
        for (const SpotShadowFramePacket& shadow : shadows) {
            if (shadow.lightSlot >= clusters.lightRecordCapacity() ||
                shadow.shadowDataSlot >= kSpotShadowEntryCapacity)
                throw std::out_of_range(
                    "Spot shadow light or data slot is invalid");
            if (shadow.sampleable)
                spotMappingScratch_[shadow.lightSlot] = shadow.shadowDataSlot;
        }
        clusters.publishSpotShadowSlots(spotMappingScratch_);

        stagedSpot_ = shadows;
        spotStaged_ = {};
        const bool hasUpdates = std::ranges::any_of(shadows,
            [](const SpotShadowFramePacket& shadow) { return shadow.update; });
        if (!hasUpdates) {
            // Both passes are skipped.
            context_->graph.drainRegisteredThrough(spotDrawPass_);
            return;
        }
        CpuScope recordScope(context_->profiler, "cpu.render.record.shadow.spot");
        resolveCasters(context_->gpuScene.indirectScene(frameIndex), casters,
            GpuSceneConsumerShadow, *scratch_);
        auto& counters = context_->telemetry.counters();
        spotStaged_.indirectValid = planCuller(spotCuller_, casters,
            spotShadowWork(shadows), frameIndex);
        counters.shadowSpotIndirectFallbackReason =
            static_cast<uint32_t>(spotCuller_.fallbackReason());
        if (spotCuller_.membershipCacheHit())
            counters.shadowSpotMembershipCacheHit = 1u;
        spotStaged_.compact = spotStaged_.indirectValid;
        spotStaged_.draw = true;
        context_->graph.drainRegisteredThrough(spotDrawPass_);
    }

    void VulkanLocalShadowFeature::submitPoint(const ShadowCasterSubmission& casters,
        std::span<const PointShadowFramePacket> shadows,
        VulkanClusterLightingFeature& clusters) {
        const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
        point_.updateFrame(frameIndex, shadows);

        pointMappingScratch_.assign(clusters.lightRecordCapacity(),
            kInvalidShadowDataSlot);
        for (const PointShadowFramePacket& shadow : shadows) {
            if (shadow.lightSlot >= clusters.lightRecordCapacity() ||
                shadow.shadowDataSlot >= kPointShadowEntryCapacity)
                throw std::out_of_range(
                    "Point shadow light or data slot is invalid");
            if (shadow.sampleable)
                pointMappingScratch_[shadow.lightSlot] = shadow.shadowDataSlot;
        }
        clusters.publishPointShadowSlots(pointMappingScratch_);

        stagedPoint_ = shadows;
        pointStaged_ = {};
        const bool hasUpdates = std::ranges::any_of(shadows,
            [](const PointShadowFramePacket& shadow) { return shadow.update; });
        if (!hasUpdates) {
            // Both passes are skipped.
            context_->graph.drainRegisteredThrough(pointDrawPass_);
            return;
        }
        CpuScope recordScope(context_->profiler, "cpu.render.record.shadow.point");
        resolveCasters(context_->gpuScene.indirectScene(frameIndex), casters,
            GpuSceneConsumerShadow, *scratch_);
        auto& counters = context_->telemetry.counters();
        pointStaged_.indirectValid = planCuller(pointCuller_, casters,
            pointShadowWork(shadows), frameIndex);
        counters.shadowPointIndirectFallbackReason =
            static_cast<uint32_t>(pointCuller_.fallbackReason());
        if (pointCuller_.membershipCacheHit())
            counters.shadowPointMembershipCacheHit = 1u;
        pointStaged_.compact = pointStaged_.indirectValid;
        pointStaged_.draw = true;
        context_->graph.drainRegisteredThrough(pointDrawPass_);
    }

    bool VulkanLocalShadowFeature::spotCompactActive(void* owner,
        const VulkanFrameRecordContext&) {
        return static_cast<const VulkanLocalShadowFeature*>(owner)->spotStaged_.compact;
    }

    void VulkanLocalShadowFeature::executeSpotCompact(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanLocalShadowFeature*>(owner);
        const uint32_t frame = context.frame.frameIndex;
        self.context_->telemetry.counters().dispatchRecorded +=
            self.spotCuller_.recordCompaction(context.commandBuffer, frame, {
                .set0 = self.spot_.renderDescriptor(frame),
                .gpuScene = self.context_->gpuScene.descriptorSets()[frame],
            });
    }

    bool VulkanLocalShadowFeature::spotDrawActive(void* owner,
        const VulkanFrameRecordContext&) {
        return static_cast<const VulkanLocalShadowFeature*>(owner)->spotStaged_.draw;
    }

    void VulkanLocalShadowFeature::executeSpotDraw(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanLocalShadowFeature*>(owner);
        const VulkanFeatureContext& feature = *self.context_;
        VulkanCasterScratch& scratch = *self.scratch_;
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const bool indirectValid = self.spotStaged_.indirectValid;
        VulkanFrameTelemetry& telemetry = feature.telemetry;
        auto& counters = telemetry.counters();
        IVulkanIndirectOracle* const shadowOracle = activeIndirectOracle(
            feature.extensions, VulkanIndirectOracleView::SpotShadow);
        counters.shadowSpotDirectFallback =
            static_cast<uint64_t>(std::count_if(
                scratch.casters.begin(), scratch.casters.end(),
                [indirectValid](const VulkanResolvedCaster& caster) {
                    return !indirectValid || caster.gpuScenePrimitiveIndex ==
                        InvalidGpuSceneIndex;
                }));
        const VkPipelineLayout layout = self.spot_.pipelineLayout();
        const VkDescriptorSet shadowSet = self.spot_.renderDescriptor(frameIndex);
        context.beginRendering();
        for (const SpotShadowFramePacket& shadow : self.stagedSpot_) {
            if (!shadow.update) continue;
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
                const bool visible = shadowCasterSphereIntersectsClipVolume(
                    shadow.worldToShadowClip,
                    caster.boundsSphereCenterWorld,
                    caster.boundsSphereRadiusWorld);
                scratch.visibility[casterIndex] = visible ? 1u : 0u;
                if (telemetry.collecting()) {
                    ++counters.shadowSpotCastersTested;
                    counters.shadowSpotCastersCulled += visible ? 0u : 1u;
                }
            }
            self.spot_.beginTile(cmd, shadow);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                0, 1, &shadowSet, 0, nullptr);
            bool materialDescriptorsBound = false;
            if (indirectValid) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    3u, 1u, &feature.gpuScene.descriptorSets()[frameIndex],
                    0u, nullptr);
                if (self.spotCuller_.anyAlphaMaskedBin()) {
                    feature.resources.bindMaterialDescriptors(cmd, frameIndex, layout);
                    materialDescriptorsBound = true;
                }
                const uint32_t workIndex =
                    self.spotCuller_.workIndex(shadow.shadowDataSlot);
                counters.shadowSpotIndirectBins +=
                    self.spotCuller_.recordDraws(cmd, frameIndex, workIndex, {
                        .layout = layout,
                        .owner = &self.spot_,
                        .pipeline = [](const void* owner, bool alphaMasked,
                            bool doubleSided) {
                            return static_cast<const VulkanSpotShadowAtlas*>(
                                owner)->pipeline(alphaMasked, doubleSided, true);
                        },
                        .pushSlotWord = true,
                        .slotWord = shadow.shadowDataSlot,
                    });
                if (shadowOracle != nullptr) {
                    const PerspectiveLodContext lodContext{
                        shadow.worldToShadowClip,
                        glm::vec2(static_cast<float>(shadow.tileSize)),
                        self.settings_.lodErrorThreshold };
                    self.spotCuller_.emitExpectations(*shadowOracle, frameIndex,
                        workIndex, scratch.casters, scratch.visibility, 1u,
                        perspectiveLodMetric(lodContext));
                    recordIndirectOracleDraws(feature, scratch, 1u,
                        counters.drawShadowSpot, counters.shadowSpotIndirectCommands,
                        counters.drawShadowSpotAlphaMask);
                }
            }
            recordShadowDirectDraws(feature, cmd, frameIndex, scratch, {
                .layout = layout,
                .pipelineOwner = &self.spot_,
                .pipeline = [](const void* owner, bool alphaMasked, bool doubleSided) {
                    return static_cast<const VulkanSpotShadowAtlas*>(owner)->
                        pipeline(alphaMasked, doubleSided);
                },
                .slotWord = shadow.shadowDataSlot,
                .visibilityBit = 1u,
                .indirectValid = indirectValid,
                .drawCounter = &counters.drawShadowSpot,
                .alphaMaskCounter = &counters.drawShadowSpotAlphaMask,
            }, materialDescriptorsBound);
        }
        context.endRendering();
    }

    bool VulkanLocalShadowFeature::pointCompactActive(void* owner,
        const VulkanFrameRecordContext&) {
        return static_cast<const VulkanLocalShadowFeature*>(owner)->pointStaged_.compact;
    }

    void VulkanLocalShadowFeature::executePointCompact(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanLocalShadowFeature*>(owner);
        const uint32_t frame = context.frame.frameIndex;
        self.context_->telemetry.counters().dispatchRecorded +=
            self.pointCuller_.recordCompaction(context.commandBuffer, frame, {
                .set0 = self.point_.renderDescriptor(frame),
                .gpuScene = self.context_->gpuScene.descriptorSets()[frame],
            });
    }

    bool VulkanLocalShadowFeature::pointDrawActive(void* owner,
        const VulkanFrameRecordContext&) {
        return static_cast<const VulkanLocalShadowFeature*>(owner)->pointStaged_.draw;
    }

    void VulkanLocalShadowFeature::executePointDraw(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanLocalShadowFeature*>(owner);
        const VulkanFeatureContext& feature = *self.context_;
        VulkanCasterScratch& scratch = *self.scratch_;
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const bool indirectValid = self.pointStaged_.indirectValid;
        VulkanFrameTelemetry& telemetry = feature.telemetry;
        auto& counters = telemetry.counters();
        IVulkanIndirectOracle* const shadowOracle = activeIndirectOracle(
            feature.extensions, VulkanIndirectOracleView::PointShadow);
        counters.shadowPointDirectFallback =
            static_cast<uint64_t>(std::count_if(
                scratch.casters.begin(), scratch.casters.end(),
                [indirectValid](const VulkanResolvedCaster& caster) {
                    return !indirectValid || caster.gpuScenePrimitiveIndex ==
                        InvalidGpuSceneIndex;
                }));
        const VkPipelineLayout layout = self.point_.pipelineLayout();
        const VkDescriptorSet shadowSet = self.point_.renderDescriptor(frameIndex);
        for (const PointShadowFramePacket& shadow : self.stagedPoint_) {
            if (!shadow.update) continue;
            for (uint32_t face = 0; face < 6u; ++face) {
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
                    const bool visible = shadowCasterSphereIntersectsClipVolume(
                        shadow.worldToShadowClip[face],
                        caster.boundsSphereCenterWorld,
                        caster.boundsSphereRadiusWorld);
                    scratch.visibility[casterIndex] = visible ? 1u : 0u;
                    if (telemetry.collecting()) {
                        ++counters.shadowPointCastersTested;
                        counters.shadowPointCastersCulled += visible ? 0u : 1u;
                    }
                }
                const VulkanPointShadowPools::FaceTarget target =
                    self.point_.faceTarget(shadow, face);
                VulkanRenderingOverrides rendering{};
                rendering.depthIndex = target.depthIndex;
                rendering.depthView = target.view;
                context.beginRendering(rendering);
                self.point_.beginFace(cmd, shadow);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                    0, 1, &shadowSet, 0, nullptr);
                const uint32_t faceSlot = shadow.shadowDataSlot * 6u + face;
                bool materialDescriptorsBound = false;
                if (indirectValid) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        layout, 3u, 1u, &feature.gpuScene.descriptorSets()[frameIndex],
                        0u, nullptr);
                    if (self.pointCuller_.anyAlphaMaskedBin()) {
                        feature.resources.bindMaterialDescriptors(cmd, frameIndex, layout);
                        materialDescriptorsBound = true;
                    }
                    const uint32_t workIndex = self.pointCuller_.workIndex(faceSlot);
                    counters.shadowPointIndirectBins +=
                        self.pointCuller_.recordDraws(cmd, frameIndex, workIndex, {
                            .layout = layout,
                            .owner = &self.point_,
                            .pipeline = [](const void* owner,
                                bool alphaMasked, bool doubleSided) {
                                return static_cast<const VulkanPointShadowPools*>(
                                    owner)->pipeline(alphaMasked, doubleSided, true);
                            },
                            .pushSlotWord = true,
                            .slotWord = faceSlot,
                        });
                    if (shadowOracle != nullptr) {
                        const RadialLodContext lodContext{ shadow.lightPosition,
                            static_cast<float>(shadow.resolution),
                            self.settings_.lodErrorThreshold };
                        self.pointCuller_.emitExpectations(*shadowOracle, frameIndex,
                            workIndex, scratch.casters, scratch.visibility, 1u,
                            radialLodMetric(lodContext));
                        recordIndirectOracleDraws(feature, scratch, 1u,
                            counters.drawShadowPoint,
                            counters.shadowPointIndirectCommands,
                            counters.drawShadowPointAlphaMask);
                    }
                }
                recordShadowDirectDraws(feature, cmd, frameIndex, scratch, {
                    .layout = layout,
                    .pipelineOwner = &self.point_,
                    .pipeline = [](const void* owner, bool alphaMasked,
                        bool doubleSided) {
                        return static_cast<const VulkanPointShadowPools*>(owner)->
                            pipeline(alphaMasked, doubleSided);
                    },
                    .slotWord = faceSlot,
                    .visibilityBit = 1u,
                    .indirectValid = indirectValid,
                    .drawCounter = &counters.drawShadowPoint,
                    .alphaMaskCounter = &counters.drawShadowPointAlphaMask,
                }, materialDescriptorsBound);
                context.endRendering();
            }
        }
    }

} // namespace Iridium
