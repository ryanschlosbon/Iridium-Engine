#include "VulkanLayeredTransparencyFeature.h"

#include "VulkanExtensionHooks.h"
#include "VulkanFrameScheduler.h"
#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceRegistry.h"
#include "VulkanWeightedOitFeature.h"
#include "profiling/CpuProfiler.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/transparency/LayeredGlass.h"
#include "renderer/transparency/WeightedOit.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {
        constexpr std::array<const char*, 4> Hero4CaptureGpuRanges{
            "gpu.transparency.layered.hero4.interface.0.capture",
            "gpu.transparency.layered.hero4.interface.1.capture",
            "gpu.transparency.layered.hero4.interface.2.capture",
            "gpu.transparency.layered.hero4.interface.3.capture",
        };
        constexpr std::array<const char*, 8> Cinematic8CaptureGpuRanges{
            "gpu.transparency.layered.cinematic8.interface.0.capture",
            "gpu.transparency.layered.cinematic8.interface.1.capture",
            "gpu.transparency.layered.cinematic8.interface.2.capture",
            "gpu.transparency.layered.cinematic8.interface.3.capture",
            "gpu.transparency.layered.cinematic8.interface.4.capture",
            "gpu.transparency.layered.cinematic8.interface.5.capture",
            "gpu.transparency.layered.cinematic8.interface.6.capture",
            "gpu.transparency.layered.cinematic8.interface.7.capture",
        };
        constexpr std::array<const char*, 4> Hero4TerminationGpuRanges{
            "gpu.transparency.layered.hero4.interface.0.terminate-tiles",
            "gpu.transparency.layered.hero4.interface.1.terminate-tiles",
            "gpu.transparency.layered.hero4.interface.2.terminate-tiles",
            "gpu.transparency.layered.hero4.interface.3.terminate-tiles",
        };
        constexpr std::array<const char*, 8> Cinematic8TerminationGpuRanges{
            "gpu.transparency.layered.cinematic8.interface.0.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.1.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.2.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.3.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.4.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.5.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.6.terminate-tiles",
            "gpu.transparency.layered.cinematic8.interface.7.terminate-tiles",
        };
        constexpr std::array<TransparencyQuality, 2> DeepQualities{
            TransparencyQuality::Hero4, TransparencyQuality::Cinematic8 };
    }

    uint32_t VulkanLayeredTransparencyFeature::tierSlot(TransparencyQuality quality) {
        if (quality == TransparencyQuality::Hero4) return 0u;
        if (quality == TransparencyQuality::Cinematic8) return 1u;
        throw std::invalid_argument("Deep layered tiers are Hero4 or Cinematic8");
    }

    bool VulkanLayeredTransparencyFeature::deepActive(TransparencyQuality quality) const noexcept {
        const VkExtent2D& extent = quality == TransparencyQuality::Hero4
            ? hero4Extent_ : cinematic8Extent_;
        return extent.width != 0u && extent.height != 0u;
    }

    void VulkanLayeredTransparencyFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        const VulkanMeshLayouts& meshLayouts = context.meshLayouts;
        interfaceCapture_.init(context.device, context.pipelineCache, context.descriptors,
            meshLayouts.getGlobalSetLayout(),
            context.resources.textureTable().materialViewLayout(),
            context.resources.textureTable().samplerLayout());
        localComposition_.init(context.device, context.pipelineCache, context.descriptors,
            meshLayouts.getGlobalSetLayout(),
            context.resources.textureTable().materialViewLayout(),
            context.resources.textureTable().samplerLayout(), lightingSetLayout_);
        sceneResolve_.init(context.device, context.pipelineCache, context.descriptors,
            meshLayouts.getGlobalSetLayout());
        ordinary2Captures_ = { { { this, false }, { this, true } } };
        for (uint32_t tier = 0; tier < deepTiers_.size(); ++tier) {
            DeepTier& deep = deepTiers_[tier];
            deep.tier = { this, DeepQualities[tier], 0u };
            for (uint32_t index = 0; index < MaximumInterfaces; ++index)
                deep.interfaces[index] = { this, DeepQualities[tier], index };
        }
    }

    void VulkanLayeredTransparencyFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        ordinary2EntryPass_ = ids.ordinary2EntryCapture;
        ordinary2ExitPass_ = ids.ordinary2ExitCapture;
        ordinary2LocalComposePass_ = ids.ordinary2LocalCompose;
        ordinary2ResolvePass_ = ids.ordinary2ComposeHook;
        deepTiers_[0].ids = ids.hero4;
        deepTiers_[1].ids = ids.cinematic8;
        deepResolvePass_ = ids.deepComposeHook;
    }

    void VulkanLayeredTransparencyFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // Declared only while the tier's atlas is resident. Every range opens
        // before its pass's barriers.
        constexpr GpuRangePlacement Before = GpuRangePlacement::BeforeBarriers;
        // R4a: the captures and local compositions record with dynamic
        // rendering. The executor's transitions to ColorAttachment and
        // DepthAttachmentWrite replace the render passes' external
        // dependencies; CLEAR on the preserved layout equals the old
        // UNDEFINED initial layout.
        constexpr bool Dynamic = true;
        if (ordinary2EntryPass_.isValid()) {
            graph.registerPass(ordinary2EntryPass_, { &ordinary2Captures_[0],
                &ordinary2CaptureActive, &executeOrdinary2Capture,
                "gpu.transparency.layered.entry.capture", Before, Dynamic });
            graph.registerPass(ordinary2ExitPass_, { &ordinary2Captures_[1],
                &ordinary2CaptureActive, &executeOrdinary2Capture,
                "gpu.transparency.layered.exit.capture", Before, Dynamic });
            graph.registerPass(ordinary2LocalComposePass_, { this, &ordinary2PassActive,
                &executeOrdinary2LocalComposition,
                "gpu.transparency.layered.local-compose", Before, Dynamic });
            // The compose hook's scene.color write re-barriers after the
            // sorted forward pass (same ColorAttachment access).
            graph.registerPass(ordinary2ResolvePass_, { this, &ordinary2PassActive,
                &executeOrdinary2SceneResolve,
                "gpu.transparency.layered.scene-resolve", Before, Dynamic });
        }
        for (uint32_t tier = 0; tier < deepTiers_.size(); ++tier) {
            DeepTier& deep = deepTiers_[tier];
            const bool hero4 = DeepQualities[tier] == TransparencyQuality::Hero4;
            for (uint32_t index = 0; index < MaximumInterfaces; ++index) {
                if (deep.ids.interfaceCapture[index].isValid())
                    graph.registerPass(deep.ids.interfaceCapture[index], {
                        &deep.interfaces[index], &deepTierActive, &executeDeepCapture,
                        hero4 ? Hero4CaptureGpuRanges[index]
                            : Cinematic8CaptureGpuRanges[index], Before, Dynamic });
                if (deep.ids.terminateTiles[index].isValid())
                    graph.registerPass(deep.ids.terminateTiles[index], {
                        &deep.interfaces[index], &deepTierActive, &executeDeepTermination,
                        hero4 ? Hero4TerminationGpuRanges[index]
                            : Cinematic8TerminationGpuRanges[index], Before });
            }
            if (deep.ids.localCompose.isValid())
                graph.registerPass(deep.ids.localCompose, { &deep.tier, &deepTierActive,
                    &executeDeepLocalComposition, hero4
                        ? "gpu.transparency.layered.hero4.local-compose"
                        : "gpu.transparency.layered.cinematic8.local-compose", Before,
                    Dynamic });
        }
        // The resident tier set selects the resolve's range (and the hook's name).
        if (deepResolvePass_.isValid()) {
            const bool hero4Active = deepActive(TransparencyQuality::Hero4);
            const bool cinematic8Active = deepActive(TransparencyQuality::Cinematic8);
            graph.registerPass(deepResolvePass_, { this, &deepResolveActive,
                &executeDeepSceneResolve, hero4Active && cinematic8Active
                    ? "gpu.transparency.layered.deep.scene-resolve"
                    : hero4Active
                        ? "gpu.transparency.layered.hero4.scene-resolve"
                        : "gpu.transparency.layered.cinematic8.scene-resolve", Before,
                Dynamic });
        }
    }

    void VulkanLayeredTransparencyFeature::destroy() noexcept {
        sceneResolve_.cleanup();
        localComposition_.cleanup();
        interfaceCapture_.cleanup();
        context_ = nullptr;
    }

    void VulkanLayeredTransparencyFeature::rebuildDescriptors() {
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        interfaceCapture_.rebuildDescriptors(frameTargets);
        localComposition_.rebuildDescriptors(frameTargets);
        sceneResolve_.rebuildDescriptors(frameTargets);
    }

    void VulkanLayeredTransparencyFeature::clearDescriptors() noexcept {
        interfaceCapture_.clearDescriptors();
        localComposition_.clearDescriptors();
        sceneResolve_.clearDescriptors();
    }

    void VulkanLayeredTransparencyFeature::observe(
        std::span<const DrawPacket> compatibilityTransparentQueue) {
        const bool requiresOrdinary2Atlas = std::ranges::any_of(
            compatibilityTransparentQueue, isOrdinary2LayeredGlassPacket);
        ordinary2Residency_.observe(requiresOrdinary2Atlas);
        const bool requiresHero4Atlas = std::ranges::any_of(
            compatibilityTransparentQueue, [](const DrawPacket& packet) {
                return isLayeredGlassPacket(packet,
                    TransparencyQuality::Hero4);
            });
        const bool requiresCinematic8Atlas = std::ranges::any_of(
            compatibilityTransparentQueue, [](const DrawPacket& packet) {
                return isLayeredGlassPacket(packet,
                    TransparencyQuality::Cinematic8);
            });
        hero4Residency_.observe(requiresHero4Atlas);
        cinematic8Residency_.observe(requiresCinematic8Atlas);
    }

    void VulkanLayeredTransparencyFeature::prepare(const FrameInputs& inputs) {
        staged_ = inputs;
        CpuProfiler* const profiler = context_->profiler;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        const std::span<const DrawPacket> compatibilityTransparentQueue =
            inputs.compatibilityTransparentQueue;
        const bool ordinary2CaptureTopologyActive = ordinary2Active();
        bool ordinary2PreparedThisFrame = false;
        // Active Ordinary2 topology prepares the fixed-capacity draw plan every
        // frame. When inactive, profiler frames retain the earlier demand probe
        // without changing topology or recording commands.
        if ((ordinary2CaptureTopologyActive || telemetry.collecting()) &&
            viewProjectionValid_) {
            CpuScope preparationScope(profiler,
                ordinary2CaptureTopologyActive
                    ? "cpu.render.prepare.ordinary2"
                    : "cpu.render.prepare.ordinary2_probe");
            const VkExtent2D extent = context_->frameTargets.extent();
            ordinary2RequestCollector_.collect(compatibilityTransparentQueue,
                viewProjection_, extent.width, extent.height);
            (void)ordinary2AtlasPlan_.prepare(
                ordinary2RequestCollector_.requests(),
                extent.width, extent.height);
            (void)ordinary2CaptureDrawPlan_.prepare(
                ordinary2AtlasPlan_.decisions(),
                compatibilityTransparentQueue,
                ordinary2AtlasPlan_.atlasExtent());
            ordinary2PreparedThisFrame = true;
            if (telemetry.collecting()) {
                const Ordinary2RequestCollectionStats& collection =
                    ordinary2RequestCollector_.stats();
                const Ordinary2AtlasStats& atlas = ordinary2AtlasPlan_.stats();
                const Ordinary2CaptureDrawStats& capture =
                    ordinary2CaptureDrawPlan_.stats();
                auto& counters = telemetry.counters();
                ++counters.ordinary2ProbeFrames;
                counters.ordinary2CandidatePackets = collection.candidatePacketCount;
                counters.ordinary2ProjectedPackets = collection.projectedPacketCount;
                counters.ordinary2ProjectionCulledPackets = collection.culledPacketCount;
                counters.ordinary2InvalidBoundsFallbackPackets =
                    collection.invalidBoundsFallbackCount;
                counters.ordinary2NearPlaneFallbackPackets =
                    collection.nearPlaneFallbackCount;
                counters.ordinary2UnsafeProjectionFallbackPackets =
                    collection.unsafeProjectionFallbackCount;
                counters.ordinary2RequestCapacityFallbackPackets =
                    collection.requestCapacityFallbackCount;
                counters.ordinary2AtlasAcceptedPackets = atlas.acceptedPacketCount;
                counters.ordinary2AtlasAcceptedIslands = atlas.acceptedIslandCount;
                counters.ordinary2AtlasRejectedPackets =
                    atlas.requestCount - atlas.acceptedPacketCount;
                counters.ordinary2AtlasAllocatedTexels = atlas.allocatedTexelCount;
                counters.ordinary2CapturePreparedDraws = capture.preparedDrawCount;
                counters.ordinary2CapturePreparationFallbackPackets =
                    capture.invalidPacketIndexCount +
                    capture.incompatiblePacketCount +
                    capture.invalidPlacementCount;
            }
        }

        std::span<const Ordinary2CaptureDraw> captureDraws{};
        if (ordinary2CaptureTopologyActive && ordinary2PreparedThisFrame)
            captureDraws = ordinary2CaptureDrawPlan_.draws();
        prepareOrdinary2ResolvedPacketIndices(captureDraws);
        stagedCaptureDraws_ = captureDraws;

        const bool hero4CaptureTopologyActive = deepActive(TransparencyQuality::Hero4);
        const bool cinematic8CaptureTopologyActive =
            deepActive(TransparencyQuality::Cinematic8);
        std::span<const LayeredCaptureDraw> deepCaptureDraws{};
        if ((hero4CaptureTopologyActive || cinematic8CaptureTopologyActive) &&
            viewProjectionValid_) {
            CpuScope preparationScope(profiler, "cpu.render.prepare.layered_deep");
            uint32_t activeTierMask = 0u;
            if (hero4CaptureTopologyActive)
                activeTierMask |= 1u << layeredQualityTierIndex(
                    TransparencyQuality::Hero4);
            if (cinematic8CaptureTopologyActive)
                activeTierMask |= 1u << layeredQualityTierIndex(
                    TransparencyQuality::Cinematic8);
            const VkExtent2D extent = context_->frameTargets.extent();
            deepRequestCollector_.collect(
                compatibilityTransparentQueue, viewProjection_,
                extent.width, extent.height, activeTierMask);
            (void)deepAtlasPlan_.prepare(
                deepRequestCollector_.requests(),
                extent.width, extent.height);
            const std::array<Ordinary2AtlasExtent,
                kLayeredQualityTierCount> residentExtents{
                Ordinary2AtlasExtent{},
                { hero4Extent_.width, hero4Extent_.height },
                { cinematic8Extent_.width, cinematic8Extent_.height },
            };
            (void)deepCaptureDrawPlan_.prepare(
                deepAtlasPlan_.decisions(),
                compatibilityTransparentQueue, residentExtents);
            deepCaptureDraws = deepCaptureDrawPlan_.draws();
            if (telemetry.collecting()) {
                const LayeredRequestCollectionStats& collection =
                    deepRequestCollector_.stats();
                const LayeredAtlasStats& atlas = deepAtlasPlan_.stats();
                const LayeredCaptureDrawStats& capture =
                    deepCaptureDrawPlan_.stats();
                auto& counters = telemetry.counters();
                counters.deepLayeredCandidatePackets = collection.candidatePacketCount;
                counters.deepLayeredProjectedPackets = collection.projectedPacketCount;
                counters.deepLayeredAtlasAcceptedPackets = atlas.acceptedPacketCount;
                counters.deepLayeredAtlasAcceptedIslands = atlas.acceptedIslandCount;
                counters.deepLayeredAtlasRejectedPackets =
                    atlas.requestCount - atlas.acceptedPacketCount;
                counters.deepLayeredCapturePreparedDraws = capture.preparedDrawCount;
                counters.deepLayeredCapturePreparationFallbackPackets =
                    capture.invalidPacketIndexCount +
                    capture.incompatiblePacketCount +
                    capture.invalidPlacementCount;
            }
        }
        prepareDeepResolvedPacketIndices(deepCaptureDraws);
        stagedDeepDraws_ = deepCaptureDraws;
        for (uint32_t tier = 0; tier < tierHasDraws_.size(); ++tier) {
            const TransparencyQuality quality = DeepQualities[tier];
            tierHasDraws_[tier] = std::ranges::any_of(deepCaptureDraws,
                [quality](const LayeredCaptureDraw& draw) {
                    return draw.quality == quality;
                });
        }
    }

    void VulkanLayeredTransparencyFeature::prepareOrdinary2ResolvedPacketIndices(
        std::span<const Ordinary2CaptureDraw> draws) {
        ordinary2ResolvedPacketCount_ = static_cast<uint32_t>((std::min)(
            draws.size(), ordinary2ResolvedDraws_.size()));
        std::copy_n(draws.begin(), ordinary2ResolvedPacketCount_,
            ordinary2ResolvedDraws_.begin());
        std::sort(ordinary2ResolvedDraws_.begin(),
            ordinary2ResolvedDraws_.begin() + ordinary2ResolvedPacketCount_,
            [](const Ordinary2CaptureDraw& lhs,
                const Ordinary2CaptureDraw& rhs) {
                return lhs.packetIndex < rhs.packetIndex;
            });
    }

    bool VulkanLayeredTransparencyFeature::isOrdinary2PacketResolved(
        uint32_t packetIndex) const noexcept {
        const auto begin = ordinary2ResolvedDraws_.begin();
        const auto end = begin + ordinary2ResolvedPacketCount_;
        const auto found = std::lower_bound(begin, end, packetIndex,
            [](const Ordinary2CaptureDraw& draw, uint32_t index) {
                return draw.packetIndex < index;
            });
        return found != end && found->packetIndex == packetIndex;
    }

    void VulkanLayeredTransparencyFeature::prepareDeepResolvedPacketIndices(
        std::span<const LayeredCaptureDraw> draws) {
        deepResolvedPacketCount_ = 0u;
        for (const LayeredCaptureDraw& draw : draws) {
            if ((draw.quality != TransparencyQuality::Hero4 &&
                    draw.quality != TransparencyQuality::Cinematic8) ||
                deepResolvedPacketCount_ >= deepResolvedDraws_.size()) {
                continue;
            }
            deepResolvedDraws_[deepResolvedPacketCount_++] = draw;
        }
        std::sort(deepResolvedDraws_.begin(),
            deepResolvedDraws_.begin() + deepResolvedPacketCount_,
            [](const LayeredCaptureDraw& lhs,
                const LayeredCaptureDraw& rhs) {
                return lhs.packetIndex < rhs.packetIndex;
            });
    }

    bool VulkanLayeredTransparencyFeature::isDeepPacketResolved(
        uint32_t packetIndex) const noexcept {
        const auto begin = deepResolvedDraws_.begin();
        const auto end = begin + deepResolvedPacketCount_;
        const auto found = std::lower_bound(begin, end, packetIndex,
            [](const LayeredCaptureDraw& draw, uint32_t index) {
                return draw.packetIndex < index;
            });
        return found != end && found->packetIndex == packetIndex;
    }

    bool VulkanLayeredTransparencyFeature::isPacketResolved(
        uint32_t packetIndex) const noexcept {
        return isOrdinary2PacketResolved(packetIndex) ||
            isDeepPacketResolved(packetIndex);
    }

    // ------------------------------------------------------------------
    // Drain points. Each validates its pass's resident inputs where the
    // imperative path did (before the pass begins), then drains it.
    // ------------------------------------------------------------------

    void VulkanLayeredTransparencyFeature::recordOrdinary2Captures() {
        const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
        for (const bool exitCapture : { false, true }) {
            if (!stagedCaptureDraws_.empty()) {
                if (ordinary2Extent_.width == 0u ||
                    ordinary2Extent_.height == 0u ||
                    interfaceCapture_.pipeline() == VK_NULL_HANDLE ||
                    interfaceCapture_.descriptorFrameCount() <= frameIndex) {
                    throw std::logic_error(
                        "Ordinary2 capture recording requires resident atlas targets");
                }
            }
            context_->graph.drainRegisteredThrough(
                exitCapture ? ordinary2ExitPass_ : ordinary2EntryPass_);
        }
    }

    void VulkanLayeredTransparencyFeature::recordOrdinary2LocalComposition() {
        if (!stagedCaptureDraws_.empty()) {
            const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
            if (ordinary2Extent_.width == 0u ||
                ordinary2Extent_.height == 0u ||
                localComposition_.pipeline() == VK_NULL_HANDLE ||
                localComposition_.descriptorFrameCount() <= frameIndex) {
                throw std::logic_error(
                    "Ordinary2 local composition requires resident atlas targets");
            }
        }
        context_->graph.drainRegisteredThrough(ordinary2LocalComposePass_);
    }

    VulkanOrdinary2HookPayload VulkanLayeredTransparencyFeature::ordinary2HookPayload()
        const noexcept {
        return { ordinary2Extent_, static_cast<uint32_t>(stagedCaptureDraws_.size()),
            static_cast<uint32_t>(ordinary2AtlasPlan_.workIdentities().size()) };
    }

    void VulkanLayeredTransparencyFeature::recordOrdinary2SceneResolve() {
        if (!stagedCaptureDraws_.empty()) {
            const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
            if (sceneResolve_.pipeline() == VK_NULL_HANDLE ||
                sceneResolve_.descriptorFrameCount() <= frameIndex) {
                throw std::logic_error(
                    "Ordinary2 scene resolve requires resident atlas descriptors");
            }
        }
        context_->graph.drainRegisteredThrough(ordinary2ResolvePass_);
    }

    void VulkanLayeredTransparencyFeature::recordDeepCaptures(TransparencyQuality quality) {
        const DeepTier& deep = deepTiers_[tierSlot(quality)];
        const uint32_t interfaceCount = layeredQualityTierContract(
            quality).maximumInterfaceCount;
        const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
        const VulkanFrameContextTargets& targets = context_->frameTargets.get(frameIndex);
        const VulkanFrameContextTargets::DeepLayeredTier& tier =
            quality == TransparencyQuality::Hero4 ? targets.hero4 : targets.cinematic8;
        const bool hasTierDraws = tierHasDraws(quality);
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < interfaceCount; ++interfaceIndex) {
            if (hasTierDraws) {
                if (!tier.active() || interfaceIndex >= tier.interfaceCount ||
                    interfaceCapture_.pipeline() == VK_NULL_HANDLE ||
                    interfaceCapture_.descriptorInterfaceCount(
                        frameIndex, quality) != tier.interfaceCount) {
                    throw std::logic_error(
                        "Deep layered capture requires a complete resident tier");
                }
            }
            context_->graph.drainRegisteredThrough(deep.ids.interfaceCapture[interfaceIndex]);
            if (deepLayeredTerminationInterface(interfaceIndex, interfaceCount)) {
                if (!tier.active() ||
                    interfaceCapture_.tileTerminationPipeline() == VK_NULL_HANDLE) {
                    throw std::logic_error(
                        "Layered tile termination requires a resident tier");
                }
                context_->graph.drainRegisteredThrough(deep.ids.terminateTiles[interfaceIndex]);
            }
        }
    }

    void VulkanLayeredTransparencyFeature::recordDeepLocalComposition(
        TransparencyQuality quality) {
        const DeepTier& deep = deepTiers_[tierSlot(quality)];
        if (tierHasDraws(quality)) {
            const uint32_t interfaceCount = layeredQualityTierContract(
                quality).maximumInterfaceCount;
            const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
            const VulkanFrameContextTargets& targets =
                context_->frameTargets.get(frameIndex);
            const VulkanFrameContextTargets::DeepLayeredTier& tier =
                quality == TransparencyQuality::Hero4 ? targets.hero4 : targets.cinematic8;
            if (!tier.active() || tier.interfaceCount != interfaceCount ||
                localComposition_.deepPipeline() == VK_NULL_HANDLE ||
                localComposition_.deepResidualPipeline() == VK_NULL_HANDLE ||
                localComposition_.deepDescriptorInterfaceCount(
                    frameIndex, quality) != interfaceCount) {
                throw std::logic_error(
                    "Deep local composition requires a complete resident tier");
            }
        }
        context_->graph.drainRegisteredThrough(deep.ids.localCompose);
    }

    VulkanDeepLayeredHookPayload VulkanLayeredTransparencyFeature::deepHookPayload(
        TransparencyQuality quality) const {
        const uint32_t interfaceCount = layeredQualityTierContract(
            quality).maximumInterfaceCount;
        if (quality != TransparencyQuality::Hero4 &&
            quality != TransparencyQuality::Cinematic8) {
            throw std::invalid_argument(
                "Deep validation requires Hero4 or Cinematic8");
        }
        const uint32_t drawCount = static_cast<uint32_t>(
            std::ranges::count_if(stagedDeepDraws_,
                [quality](const LayeredCaptureDraw& draw) {
                    return draw.quality == quality;
                }));
        return { quality, interfaceCount, drawCount,
            static_cast<uint32_t>(deepAtlasPlan_.workIdentities().size()) };
    }

    void VulkanLayeredTransparencyFeature::recordDeepSceneResolve() {
        const uint32_t frameIndex = context_->scheduler.currentFrameIndex();
        if (!stagedDeepDraws_.empty() && deepResolvedPacketCount_ != 0u) {
            if (sceneResolve_.pipeline() == VK_NULL_HANDLE) {
                throw std::logic_error(
                    "Deep scene resolve requires a resident pipeline");
            }
        }
        resolveRecorded_ = false;
        context_->graph.drainRegisteredThrough(deepResolvePass_);
        // After the pass's range closes, as before.
        if (resolveRecorded_)
            context_->extensions.notify({ .point = VulkanHookPoint::DeepLayeredResolveCounts,
                .cmd = staged_.commandBuffer, .slot = frameIndex,
                .payload = VulkanDeepResolveCountsPayload{ resolveDrawCounts_ } });
    }

    // ------------------------------------------------------------------
    // Pass callbacks.
    // ------------------------------------------------------------------

    bool VulkanLayeredTransparencyFeature::ordinary2PassActive(void* owner,
        const VulkanFrameRecordContext&) {
        return !static_cast<const VulkanLayeredTransparencyFeature*>(owner)->
            stagedCaptureDraws_.empty();
    }

    bool VulkanLayeredTransparencyFeature::ordinary2CaptureActive(void* owner,
        const VulkanFrameRecordContext& frame) {
        return ordinary2PassActive(static_cast<const Ordinary2Slot*>(owner)->self, frame);
    }

    void VulkanLayeredTransparencyFeature::executeOrdinary2Capture(void* owner,
        VulkanPassContext& context) {
        const auto& slot = *static_cast<const Ordinary2Slot*>(owner);
        slot.self->drawOrdinary2Capture(context, slot.exit);
    }

    void VulkanLayeredTransparencyFeature::executeOrdinary2LocalComposition(void* owner,
        VulkanPassContext& context) {
        static_cast<VulkanLayeredTransparencyFeature*>(owner)->drawOrdinary2LocalComposition(
            context);
    }

    void VulkanLayeredTransparencyFeature::executeOrdinary2SceneResolve(void* owner,
        VulkanPassContext& context) {
        static_cast<VulkanLayeredTransparencyFeature*>(owner)->drawOrdinary2SceneResolve(
            context);
    }

    bool VulkanLayeredTransparencyFeature::deepTierActive(void* owner,
        const VulkanFrameRecordContext&) {
        const auto& slot = *static_cast<const DeepSlot*>(owner);
        return slot.self->tierHasDraws(slot.quality);
    }

    void VulkanLayeredTransparencyFeature::executeDeepCapture(void* owner,
        VulkanPassContext& context) {
        const auto& slot = *static_cast<const DeepSlot*>(owner);
        slot.self->drawDeepCapture(context, slot.quality, slot.interfaceIndex);
    }

    void VulkanLayeredTransparencyFeature::executeDeepTermination(void* owner,
        VulkanPassContext& context) {
        const auto& slot = *static_cast<const DeepSlot*>(owner);
        VulkanLayeredTransparencyFeature& self = *slot.self;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameContextTargets& targets = self.context_->frameTargets.get(frameIndex);
        const VulkanFrameContextTargets::DeepLayeredTier& tier =
            slot.quality == TransparencyQuality::Hero4 ? targets.hero4 : targets.cinematic8;
        self.interfaceCapture_.recordTileTermination(context.commandBuffer,
            frameIndex, slot.quality, slot.interfaceIndex, tier.atlasExtent);
        self.context_->telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredTileTermination));
    }

    void VulkanLayeredTransparencyFeature::executeDeepLocalComposition(void* owner,
        VulkanPassContext& context) {
        const auto& slot = *static_cast<const DeepSlot*>(owner);
        slot.self->drawDeepLocalComposition(context, slot.quality);
    }

    bool VulkanLayeredTransparencyFeature::deepResolveActive(void* owner,
        const VulkanFrameRecordContext&) {
        const auto& self = *static_cast<const VulkanLayeredTransparencyFeature*>(owner);
        return !self.stagedDeepDraws_.empty() && self.deepResolvedPacketCount_ != 0u;
    }

    void VulkanLayeredTransparencyFeature::executeDeepSceneResolve(void* owner,
        VulkanPassContext& context) {
        static_cast<VulkanLayeredTransparencyFeature*>(owner)->drawDeepSceneResolve(
            context);
    }

    // ------------------------------------------------------------------
    // Recording (inside the passes; ranges and barriers are the executor's).
    // ------------------------------------------------------------------

    void VulkanLayeredTransparencyFeature::drawOrdinary2Capture(VulkanPassContext& context,
        bool exitCapture) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const std::span<const DrawPacket> packets = staged_.compatibilityTransparentQueue;
        const std::span<const Ordinary2CaptureDraw> draws = stagedCaptureDraws_;

        // R4a: identity (uint 0) and depth (1.0) CLEAR/STORE from the graph.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, ordinary2Extent_ };
        context.beginRendering(rendering);

        const VkPipelineLayout layout =
            interfaceCapture_.pipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            interfaceCapture_.pipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredInterfaceCapture));
        const VkDescriptorSet globalSet = staged_.globalSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        resources.bindMaterialDescriptors(cmd, frameIndex, layout);
        const VkDescriptorSet captureSet =
            interfaceCapture_.descriptorSet(frameIndex, exitCapture);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &captureSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        for (const Ordinary2CaptureDraw& draw : draws) {
            if (draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                resources.geometries().get(packet.geometry);
            if (geometry == nullptr || resources.materials().get(packet.material) == nullptr)
                continue;
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd, geometry->indexBuffer.buffer,
                    0u, toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            LayeredInterfaceCapturePushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.workTableIndex = draw.workTableIndex;
            push.flags |= kLayeredCaptureRequirePairedOrientation;
            if ((packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0u)
                push.flags |= kLayeredCaptureMirrored;
            if (exitCapture)
                push.flags |= kLayeredCaptureHasPrevious;
            push.packedViewportOffset = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            if (exitCapture) {
                telemetry.recordDraw(telemetry.counters().ordinary2CaptureExitDraws,
                    packet.indexCount / 3u);
            }
            else {
                telemetry.recordDraw(telemetry.counters().ordinary2CaptureEntryDraws,
                    packet.indexCount / 3u);
            }
        }
        context.endRendering();
    }

    void VulkanLayeredTransparencyFeature::drawDeepCapture(VulkanPassContext& context,
        TransparencyQuality quality, uint32_t interfaceIndex) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const std::span<const DrawPacket> packets = staged_.compatibilityTransparentQueue;
        const std::span<const LayeredCaptureDraw> draws = stagedDeepDraws_;
        const VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        const VulkanFrameContextTargets::DeepLayeredTier* tier = quality ==
                TransparencyQuality::Hero4
            ? &targets.hero4 : &targets.cinematic8;

        // R4a: identity (uint 0) and depth (1.0) CLEAR/STORE from the graph.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, tier->atlasExtent };
        context.beginRendering(rendering);

        const VkPipelineLayout layout =
            interfaceCapture_.pipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            interfaceCapture_.pipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredInterfaceCapture));
        const VkDescriptorSet globalSet = staged_.globalSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        resources.bindMaterialDescriptors(cmd, frameIndex, layout);
        const VkDescriptorSet captureSet =
            interfaceCapture_.descriptorSet(frameIndex, quality,
                interfaceIndex);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &captureSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        for (const LayeredCaptureDraw& draw : draws) {
            if (draw.quality != quality || draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                resources.geometries().get(packet.geometry);
            if (geometry == nullptr ||
                resources.materials().get(packet.material) == nullptr) {
                continue;
            }
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            LayeredInterfaceCapturePushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.workTableIndex = draw.workTableIndex;
            if ((packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0u) {
                push.flags |= kLayeredCaptureMirrored;
            }
            if (interfaceIndex != 0u)
                push.flags |= kLayeredCaptureHasPrevious;
            if (interfaceIndex >= 2u)
                push.flags |= kLayeredCaptureHasTerminationMask;
            push.packedViewportOffset = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            telemetry.recordDraw(telemetry.counters().deepLayeredInterfaceDraws,
                packet.indexCount / 3u);
        }
        context.endRendering();
    }

    void VulkanLayeredTransparencyFeature::drawDeepLocalComposition(
        VulkanPassContext& context, TransparencyQuality quality) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        VulkanFrameScheduler& scheduler = context_->scheduler;
        const std::span<const DrawPacket> packets = staged_.compatibilityTransparentQueue;
        const std::span<const LayeredCaptureDraw> draws = stagedDeepDraws_;
        const uint32_t interfaceCount =
            layeredQualityTierContract(quality).maximumInterfaceCount;
        const uint32_t debugView = static_cast<uint32_t>(staged_.debugView);
        const VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);
        const VulkanFrameContextTargets::DeepLayeredTier& tier = quality ==
                TransparencyQuality::Hero4
            ? targets.hero4 : targets.cinematic8;

        // R4a: local colour (0,0,0,0) CLEAR/STORE from the graph.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, tier.atlasExtent };
        context.beginRendering(rendering);

        const VkPipelineLayout layout =
            localComposition_.deepPipelineLayout();
        const VkDescriptorSet globalSet = staged_.globalSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        resources.bindMaterialDescriptors(cmd, frameIndex, layout);
        const VkDescriptorSet sceneSet = staged_.sceneSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &sceneSet, 0u, nullptr);
        const VkDescriptorSet interfaceSet =
            localComposition_.deepDescriptorSet(frameIndex, quality);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 4u, 1u, &interfaceSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        // The bounded tail is evaluated first into the cleared local atlas.
        // Only semantic entries strictly behind the final stored interface
        // survive the residual shader. This compresses arbitrarily many
        // uncaptured interfaces into deterministic non-refractive operators
        // without adding another interface image or per-frame allocation.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            localComposition_.deepResidualPipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredResidualComposition));
        const uint32_t residualQuerySlot = quality ==
                TransparencyQuality::Hero4
            ? 0u : 1u;
        const bool residualQueryActive =
            scheduler.beginLayeredResidualQuery(residualQuerySlot);
        for (const LayeredCaptureDraw& draw : draws) {
            if (draw.quality != quality || draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                resources.geometries().get(packet.geometry);
            if (geometry == nullptr ||
                resources.materials().get(packet.material) == nullptr) {
                continue;
            }
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.padding[0] = draw.workTableIndex;
            const uint32_t mirrored = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[1] = mirrored | (interfaceCount << 16u) |
                (debugView << 24u);
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            telemetry.recordDraw(telemetry.counters().deepLayeredResidualProbeDraws,
                packet.indexCount / 3u);
        }
        if (residualQueryActive)
            scheduler.endLayeredResidualQuery(residualQuerySlot);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            localComposition_.deepPipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredLocalComposition));
        // Captured slots are front-to-back. Rerasterizing them in reverse
        // makes premultiplied over blending deterministic per pixel without a
        // global object sort. The fragment shader accepts entry slots only and
        // validates the exact captured depth before evaluating shared transport.
        for (uint32_t interfaceIndex = interfaceCount;
            interfaceIndex-- > 0u;) {
            for (const LayeredCaptureDraw& draw : draws) {
                if (draw.quality != quality ||
                    draw.packetIndex >= packets.size()) {
                    continue;
                }
                const DrawPacket& packet = packets[draw.packetIndex];
                const VulkanGeometryPayload* geometry =
                    resources.geometries().get(packet.geometry);
                if (geometry == nullptr ||
                    resources.materials().get(packet.material) == nullptr) {
                    continue;
                }
                const VkViewport viewport{
                    -static_cast<float>(draw.viewportOffsetX),
                    -static_cast<float>(draw.viewportOffsetY),
                    static_cast<float>(frameTargets.extent().width),
                    static_cast<float>(frameTargets.extent().height),
                    0.0f, 1.0f };
                const VkRect2D scissor{
                    { static_cast<int32_t>(draw.atlasX),
                        static_cast<int32_t>(draw.atlasY) },
                    { draw.width, draw.height } };
                vkCmdSetViewport(cmd, 0u, 1u, &viewport);
                vkCmdSetScissor(cmd, 0u, 1u, &scissor);
                if (packet.geometry != lastGeometry) {
                    const VkDeviceSize offset = geometry->vertexOffset;
                    vkCmdBindVertexBuffers(cmd, 0u, 1u,
                        &geometry->vertexBuffer.buffer, &offset);
                    vkCmdBindIndexBuffer(cmd,
                        geometry->indexBuffer.buffer, 0u,
                        toVkIndexType(geometry->indexFormat));
                    lastGeometry = packet.geometry;
                }
                CanonicalMeshPushConstants push{};
                push.renderMatrix = packet.worldTransform;
                push.materialIndex = packet.material.getIndex();
                push.padding[0] = draw.workTableIndex;
                const uint32_t mirrored = (packet.transparentWorkFlags &
                    TransparentWorkMirrored) != 0u ? 1u : 0u;
                push.padding[1] = mirrored | (interfaceIndex << 8u) |
                    (interfaceCount << 16u) |
                    (debugView << 24u);
                push.padding[2] = packLayeredViewportOffset(
                    draw.viewportOffsetX, draw.viewportOffsetY);
                vkCmdPushConstants(cmd, layout,
                    VK_SHADER_STAGE_VERTEX_BIT |
                        VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                    packet.firstIndex, 0, 0u);
                telemetry.recordDraw(telemetry.counters().deepLayeredLocalCompositionDraws,
                    packet.indexCount / 3u);
            }
        }
        context.endRendering();
    }

    void VulkanLayeredTransparencyFeature::drawOrdinary2LocalComposition(
        VulkanPassContext& context) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const std::span<const DrawPacket> packets = staged_.compatibilityTransparentQueue;
        const std::span<const Ordinary2CaptureDraw> draws = stagedCaptureDraws_;

        // R4a: local colour (0,0,0,0) CLEAR/STORE from the graph.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, ordinary2Extent_ };
        context.beginRendering(rendering);

        const VkPipelineLayout layout =
            localComposition_.pipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            localComposition_.pipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredLocalComposition));
        const VkDescriptorSet globalSet = staged_.globalSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        resources.bindMaterialDescriptors(cmd, frameIndex, layout);
        const VkDescriptorSet sceneSet = staged_.sceneSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 3u, 1u, &sceneSet, 0u, nullptr);
        const VkDescriptorSet interfaceSet =
            localComposition_.descriptorSet(frameIndex);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 4u, 1u, &interfaceSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        // Reverse the stable capture order. Ordinary2 stores one shell, while
        // this order is the bounded back-to-front contract extended by M6.6.
        for (auto iterator = draws.rbegin(); iterator != draws.rend();
            ++iterator) {
            const Ordinary2CaptureDraw& draw = *iterator;
            if (draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                resources.geometries().get(packet.geometry);
            if (geometry == nullptr ||
                resources.materials().get(packet.material) == nullptr)
                continue;
            const VkViewport viewport{
                -static_cast<float>(draw.viewportOffsetX),
                -static_cast<float>(draw.viewportOffsetY),
                static_cast<float>(frameTargets.extent().width),
                static_cast<float>(frameTargets.extent().height),
                0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(draw.atlasX),
                    static_cast<int32_t>(draw.atlasY) },
                { draw.width, draw.height } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = packet.material.getIndex();
            push.padding[0] = draw.workTableIndex;
            push.padding[1] = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            telemetry.recordDraw(telemetry.counters().ordinary2LocalCompositionDraws,
                packet.indexCount / 3u);
        }
        context.endRendering();
    }

    void VulkanLayeredTransparencyFeature::drawOrdinary2SceneResolve(
        VulkanPassContext& context) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const std::span<const DrawPacket> packets = staged_.compatibilityTransparentQueue;
        const uint32_t debugView = static_cast<uint32_t>(staged_.debugView);

        // R4a: scene colour LOAD/STORE, opaque depth read-only (LOAD/NONE).
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, frameTargets.extent() };
        context.beginRendering(rendering);

        const VkPipelineLayout layout =
            sceneResolve_.pipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            sceneResolve_.pipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredSceneResolve));
        const VkDescriptorSet globalSet = staged_.globalSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        const VkDescriptorSet localSet =
            sceneResolve_.descriptorSet(frameIndex);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 1u, 1u, &localSet, 0u, nullptr);

        GeometryHandle lastGeometry{};
        const VkExtent2D sceneExtent = frameTargets.extent();
        // Packet-index order is the frontend's stable back-to-front order.
        for (uint32_t resolvedIndex = 0u;
            resolvedIndex < ordinary2ResolvedPacketCount_; ++resolvedIndex) {
            const Ordinary2CaptureDraw& draw =
                ordinary2ResolvedDraws_[resolvedIndex];
            if (draw.packetIndex >= packets.size())
                continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                resources.geometries().get(packet.geometry);
            if (geometry == nullptr)
                continue;
            const int64_t screenX = static_cast<int64_t>(draw.atlasX) +
                draw.viewportOffsetX;
            const int64_t screenY = static_cast<int64_t>(draw.atlasY) +
                draw.viewportOffsetY;
            if (screenX < 0 || screenY < 0 ||
                screenX >= sceneExtent.width || screenY >= sceneExtent.height)
                continue;
            const uint32_t scissorWidth = (std::min)(draw.width,
                sceneExtent.width - static_cast<uint32_t>(screenX));
            const uint32_t scissorHeight = (std::min)(draw.height,
                sceneExtent.height - static_cast<uint32_t>(screenY));
            if (scissorWidth == 0u || scissorHeight == 0u)
                continue;
            const VkViewport viewport{ 0.0f, 0.0f,
                static_cast<float>(sceneExtent.width),
                static_cast<float>(sceneExtent.height), 0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(screenX),
                    static_cast<int32_t>(screenY) },
                { scissorWidth, scissorHeight } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &scissor);
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd, geometry->indexBuffer.buffer,
                    0u, toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = draw.workTableIndex;
            push.padding[0] = (debugView << 8u) | (2u << 16u);
            push.padding[1] = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            telemetry.recordDraw(telemetry.counters().ordinary2SceneResolveDraws,
                packet.indexCount / 3u);
        }
        context.endRendering();
    }

    void VulkanLayeredTransparencyFeature::drawDeepSceneResolve(
        VulkanPassContext& context) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;
        VulkanResourceRegistry& resources = context_->resources;
        const std::span<const DrawPacket> packets = staged_.compatibilityTransparentQueue;
        const uint32_t debugView = static_cast<uint32_t>(staged_.debugView);
        const VulkanFrameContextTargets& targets = frameTargets.get(frameIndex);

        // R4a: scene colour LOAD/STORE, opaque depth read-only (LOAD/NONE).
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, frameTargets.extent() };
        context.beginRendering(rendering);

        const VkPipelineLayout layout =
            sceneResolve_.pipelineLayout();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            sceneResolve_.pipeline());
        telemetry.recordPipelineBind(pipelineIdentity(
            FixedPipelineIdentity::LayeredSceneResolve));
        const VkDescriptorSet globalSet = staged_.globalSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 0u, 1u, &globalSet, 0u, nullptr);
        GeometryHandle lastGeometry{};
        TransparencyQuality lastQuality = TransparencyQuality::Ordinary2;
        const VkExtent2D sceneExtent = frameTargets.extent();
        std::array<uint32_t, kLayeredQualityTierCount>
            sceneResolveDrawCounts{};
        for (uint32_t resolvedIndex = 0u;
            resolvedIndex < deepResolvedPacketCount_; ++resolvedIndex) {
            const LayeredCaptureDraw& draw =
                deepResolvedDraws_[resolvedIndex];
            const bool tierActive = draw.quality ==
                    TransparencyQuality::Hero4
                ? targets.hero4.active()
                : draw.quality == TransparencyQuality::Cinematic8 &&
                    targets.cinematic8.active();
            if (!tierActive ||
                sceneResolve_.descriptorFrameCount(draw.quality) <=
                    frameIndex) {
                throw std::logic_error(
                    "Deep scene resolve requires resident tier descriptors");
            }
            if (draw.packetIndex >= packets.size()) continue;
            const DrawPacket& packet = packets[draw.packetIndex];
            const VulkanGeometryPayload* geometry =
                resources.geometries().get(packet.geometry);
            if (geometry == nullptr) continue;
            const int64_t screenX = static_cast<int64_t>(draw.atlasX) +
                draw.viewportOffsetX;
            const int64_t screenY = static_cast<int64_t>(draw.atlasY) +
                draw.viewportOffsetY;
            if (screenX < 0 || screenY < 0 ||
                screenX >= sceneExtent.width || screenY >= sceneExtent.height) {
                continue;
            }
            const uint32_t scissorWidth = (std::min)(draw.width,
                sceneExtent.width - static_cast<uint32_t>(screenX));
            const uint32_t scissorHeight = (std::min)(draw.height,
                sceneExtent.height - static_cast<uint32_t>(screenY));
            if (scissorWidth == 0u || scissorHeight == 0u) continue;
            const VkViewport viewport{ 0.0f, 0.0f,
                static_cast<float>(sceneExtent.width),
                static_cast<float>(sceneExtent.height), 0.0f, 1.0f };
            const VkRect2D scissor{
                { static_cast<int32_t>(screenX),
                    static_cast<int32_t>(screenY) },
                { scissorWidth, scissorHeight } };
            vkCmdSetViewport(cmd, 0u, 1u, &viewport);
            vkCmdSetScissor(cmd, 0u, 1u, &scissor);
            if (draw.quality != lastQuality) {
                const VkDescriptorSet localSet =
                    sceneResolve_.descriptorSet(frameIndex,
                        draw.quality);
                vkCmdBindDescriptorSets(cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1u, 1u,
                    &localSet, 0u, nullptr);
                lastQuality = draw.quality;
            }
            if (packet.geometry != lastGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(cmd, 0u, 1u,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(cmd,
                    geometry->indexBuffer.buffer, 0u,
                    toVkIndexType(geometry->indexFormat));
                lastGeometry = packet.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = packet.worldTransform;
            push.materialIndex = draw.workTableIndex;
            push.padding[0] = 1u |
                (debugView << 8u) |
                (layeredQualityTierContract(draw.quality).
                    maximumInterfaceCount << 16u);
            push.padding[1] = (packet.transparentWorkFlags &
                TransparentWorkMirrored) != 0u ? 1u : 0u;
            push.padding[2] = packLayeredViewportOffset(
                draw.viewportOffsetX, draw.viewportOffsetY);
            vkCmdPushConstants(cmd, layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, packet.indexCount, 1u,
                packet.firstIndex, 0, 0u);
            telemetry.recordDraw(telemetry.counters().deepLayeredSceneResolveDraws,
                packet.indexCount / 3u);
            ++sceneResolveDrawCounts[layeredQualityTierIndex(draw.quality)];
        }
        context.endRendering();
        resolveDrawCounts_ = sceneResolveDrawCounts;
        resolveRecorded_ = true;
    }

    // ------------------------------------------------------------------
    // The transparency topology (M7.10.0: moved from the backend).
    // ------------------------------------------------------------------

    void VulkanLayeredTransparencyFeature::applyTopologyChange(
        VkExtent2D sceneExtent,
        std::optional<VkExtent2D> requestedOrdinary2AtlasExtent,
        std::optional<VkExtent2D> requestedHero4AtlasExtent,
        std::optional<VkExtent2D> requestedCinematic8AtlasExtent) {
        TransparencyPyramidResidency& pyramids = *topology_.pyramids;
        VulkanWeightedOitFeature& weightedOit = *topology_.weightedOit;
        const VkExtent2D previousOrdinary2AtlasExtent = ordinary2Extent_;
        const VkExtent2D previousHero4AtlasExtent = hero4Extent_;
        const VkExtent2D previousCinematic8AtlasExtent = cinematic8Extent_;
        const auto requestedExtent = [&](std::optional<VkExtent2D> explicitExtent,
                const TransparencyPyramidResidency& residency,
                TransparencyQuality quality) {
            if (explicitExtent) return *explicitExtent;
            if (!residency.requestedEnabled()) return VkExtent2D{};
            const Ordinary2AtlasExtent capacity = layeredAtlasCapacityExtent(
                sceneExtent.width, sceneExtent.height, quality);
            return VkExtent2D{ capacity.width, capacity.height };
        };
        const VkExtent2D nextOrdinary2AtlasExtent = requestedExtent(
            requestedOrdinary2AtlasExtent, ordinary2Residency_,
            TransparencyQuality::Ordinary2);
        const VkExtent2D nextHero4AtlasExtent = requestedExtent(
            requestedHero4AtlasExtent, hero4Residency_,
            TransparencyQuality::Hero4);
        const VkExtent2D nextCinematic8AtlasExtent = requestedExtent(
            requestedCinematic8AtlasExtent, cinematic8Residency_,
            TransparencyQuality::Cinematic8);
        const auto extentChanged = [](VkExtent2D lhs, VkExtent2D rhs) {
            return lhs.width != rhs.width || lhs.height != rhs.height;
        };
        const bool ordinary2AtlasChange = extentChanged(
            previousOrdinary2AtlasExtent, nextOrdinary2AtlasExtent);
        const bool hero4AtlasChange = extentChanged(
            previousHero4AtlasExtent, nextHero4AtlasExtent);
        const bool cinematic8AtlasChange = extentChanged(
            previousCinematic8AtlasExtent, nextCinematic8AtlasExtent);
        if (!pyramids.changePending() &&
            !ordinary2Residency_.changePending() &&
            !hero4Residency_.changePending() &&
            !cinematic8Residency_.changePending() &&
            !weightedOit.residency().changePending() &&
            !ordinary2AtlasChange && !hero4AtlasChange &&
            !cinematic8AtlasChange)
            return;

        CpuProfiler* profiler = context_->profiler;
        CpuScope topologyChangeScope(profiler,
            "cpu.renderer.transparency_topology_change");

        const bool previousEnabled = pyramids.enabled();
        const bool previousOrdinary2Enabled = ordinary2Residency_.enabled();
        const bool previousHero4Enabled = hero4Residency_.enabled();
        const bool previousCinematic8Enabled = cinematic8Residency_.enabled();
        const bool previousWeightedOitEnabled = weightedOit.residency().enabled();
        const auto releaseTargets = [&] { topology_.releaseTargets(topology_.owner); };
        const auto createTargets = [&] { topology_.createTargets(topology_.owner); };
        VulkanFrameTelemetry& telemetry = context_->telemetry;

        // This executes only between frames. All shared descriptor sets and
        // scene targets must be unreferenced before the topology is retired.
        {
            CpuScope waitScope(profiler,
                "cpu.renderer.transparency_topology_wait");
            context_->scheduler.waitForAllFrames();
        }
        try {
            CpuScope rebuildScope(profiler,
                "cpu.renderer.transparency_topology_rebuild");
            releaseTargets();
            pyramids.publishRequested();
            ordinary2Residency_.publishRequested();
            hero4Residency_.publishRequested();
            cinematic8Residency_.publishRequested();
            weightedOit.residency().publishRequested();
            weightedOit.setInstanceCapacity(weightedOit.residency().enabled()
                ? kWeightedOitMaximumInstanceCount : 0u);
            ordinary2Extent_ = nextOrdinary2AtlasExtent;
            hero4Extent_ = nextHero4AtlasExtent;
            cinematic8Extent_ = nextCinematic8AtlasExtent;
            createTargets();
            if (telemetry.collecting())
                ++telemetry.counters().transparencyPyramidTopologyRebuilds;
        }
        catch (const std::exception& exception) {
            if (telemetry.collecting())
                ++telemetry.counters().transparencyPyramidTopologyRebuildFailures;
            try {
                CpuScope restoreScope(profiler,
                    "cpu.renderer.transparency_topology_restore");
                releaseTargets();
                pyramids.restore(previousEnabled);
                ordinary2Residency_.restore(previousOrdinary2Enabled);
                hero4Residency_.restore(previousHero4Enabled);
                cinematic8Residency_.restore(previousCinematic8Enabled);
                weightedOit.residency().restore(previousWeightedOitEnabled);
                weightedOit.setInstanceCapacity(previousWeightedOitEnabled
                    ? kWeightedOitMaximumInstanceCount : 0u);
                ordinary2Extent_ = previousOrdinary2AtlasExtent;
                hero4Extent_ = previousHero4AtlasExtent;
                cinematic8Extent_ = previousCinematic8AtlasExtent;
                createTargets();
            }
            catch (const std::exception& restoreException) {
                throw std::runtime_error(std::string(
                    "Transparency topology rebuild failed: ") +
                    exception.what() + "; restoring the previous topology "
                    "failed: " + restoreException.what());
            }
        }
    }

    FrameTopologyPreparation VulkanLayeredTransparencyFeature::prepareTopology(
        const FrameTopologyRequirements& requirements, VkExtent2D sceneExtent) {
        TransparencyPyramidResidency& pyramids = *topology_.pyramids;
        VulkanWeightedOitFeature& weightedOit = *topology_.weightedOit;
        FrameTopologyPreparation result{
            .requested = requirements.refractionPyramids ||
                requirements.ordinary2LayeredInterfaces ||
                requirements.hero4LayeredInterfaces ||
                requirements.cinematic8LayeredInterfaces ||
                requirements.weightedOit,
        };
        const bool previousPyramids = pyramids.enabled();
        const VkExtent2D previousOrdinary2AtlasExtent = ordinary2Extent_;
        const VkExtent2D previousHero4AtlasExtent = hero4Extent_;
        const VkExtent2D previousCinematic8AtlasExtent = cinematic8Extent_;
        const bool previousWeightedOit = weightedOit.residency().enabled();
        const bool requirePyramids = requirements.refractionPyramids ||
            requirements.ordinary2LayeredInterfaces ||
            requirements.hero4LayeredInterfaces ||
            requirements.cinematic8LayeredInterfaces;
        VkExtent2D requestedOrdinary2AtlasExtent = ordinary2Extent_;
        VkExtent2D requestedHero4AtlasExtent = hero4Extent_;
        VkExtent2D requestedCinematic8AtlasExtent = cinematic8Extent_;
        const auto requireTier = [&](bool required,
                TransparencyQuality quality, VkExtent2D& requestedExtent,
                const char* name) {
            if (!required) return;
            const Ordinary2AtlasExtent capacity = layeredAtlasCapacityExtent(
                sceneExtent.width, sceneExtent.height, quality);
            if (capacity.empty()) {
                throw std::runtime_error(std::string(name) +
                    " startup topology requires a tile-sized scene extent");
            }
            requestedExtent = { capacity.width, capacity.height };
        };
        requireTier(requirements.ordinary2LayeredInterfaces,
            TransparencyQuality::Ordinary2, requestedOrdinary2AtlasExtent,
            "Ordinary2");
        requireTier(requirements.hero4LayeredInterfaces,
            TransparencyQuality::Hero4, requestedHero4AtlasExtent, "Hero4");
        requireTier(requirements.cinematic8LayeredInterfaces,
            TransparencyQuality::Cinematic8,
            requestedCinematic8AtlasExtent, "Cinematic8");
        const auto extentChanged = [](VkExtent2D lhs, VkExtent2D rhs) {
            return lhs.width != rhs.width || lhs.height != rhs.height;
        };
        const bool ordinary2Change = extentChanged(
            requestedOrdinary2AtlasExtent, ordinary2Extent_);
        const bool hero4Change = extentChanged(
            requestedHero4AtlasExtent, hero4Extent_);
        const bool cinematic8Change = extentChanged(
            requestedCinematic8AtlasExtent, cinematic8Extent_);
        if (!requirePyramids && !requirements.weightedOit) return result;

        if (requirePyramids) {
            pyramids.observe(true);
            ordinary2Residency_.observe(requirements.ordinary2LayeredInterfaces);
            hero4Residency_.observe(requirements.hero4LayeredInterfaces);
            cinematic8Residency_.observe(requirements.cinematic8LayeredInterfaces);
        }
        weightedOit.residency().observe(requirements.weightedOit);
        if (!pyramids.changePending() &&
            !ordinary2Residency_.changePending() &&
            !hero4Residency_.changePending() &&
            !cinematic8Residency_.changePending() &&
            !weightedOit.residency().changePending() &&
            !ordinary2Change && !hero4Change && !cinematic8Change)
            return result;

        const auto start = std::chrono::steady_clock::now();
        applyTopologyChange(sceneExtent,
            requestedOrdinary2AtlasExtent, requestedHero4AtlasExtent,
            requestedCinematic8AtlasExtent);
        result.durationNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        result.changed =
            previousPyramids != pyramids.enabled() ||
            previousOrdinary2AtlasExtent.width != ordinary2Extent_.width ||
            previousOrdinary2AtlasExtent.height != ordinary2Extent_.height ||
            previousHero4AtlasExtent.width != hero4Extent_.width ||
            previousHero4AtlasExtent.height != hero4Extent_.height ||
            previousCinematic8AtlasExtent.width != cinematic8Extent_.width ||
            previousCinematic8AtlasExtent.height != cinematic8Extent_.height ||
            previousWeightedOit != weightedOit.residency().enabled();
        return result;
    }

} // namespace Iridium
