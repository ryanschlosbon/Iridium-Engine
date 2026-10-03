#include "VulkanIndirectViewCuller.h"

#include "VulkanReflectionProbeCapturePass.h"
#include "profiling/CpuProfiler.h"
#include "renderer/rhi/Mesh.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace Iridium {

    namespace {
        constexpr uint32_t SpotShadowIndirectMaximumWorkCount = 64u;
        constexpr uint32_t PointShadowIndirectMaximumWorkCount = 192u;

        constexpr std::array<IndirectViewKindConfig, kIndirectViewKindCount>
            KindConfigs{ {
            {
                .kind = IndirectViewKind::DirectionalShadow,
                .oracleView = VulkanIndirectOracleView::DirectionalShadow,
                .streamView = VulkanIndirectStreamView::DirectionalShadow,
                .subject = "directional-shadow",
                .shader = "assets/shaders/directional_shadow_compact_comp.spv",
                .pipelineName = "directional-shadow compact",
                .pushWords = 9u,
                .consumerMask = GpuSceneConsumerShadow,
                .membershipCache = true,
                .forceDirectIncludesShadowReference = true,
                .capCommandsAtLocalMaximum = false,
                .maximumWorkCount = kDirectionalShadowLayerCount,
                .workSlotCount = kDirectionalShadowLayerCount,
                .placement = CompactionPlacement::BatchedAtPrepare,
                .compactGpuRange = "gpu.shadow.directional.compact",
            },
            {
                .kind = IndirectViewKind::SpotShadow,
                .oracleView = VulkanIndirectOracleView::SpotShadow,
                .streamView = VulkanIndirectStreamView::SpotShadow,
                .subject = "spot-shadow",
                .shader = "assets/shaders/spot_shadow_compact_comp.spv",
                .pipelineName = "spot-shadow compact",
                .pushWords = 9u,
                .consumerMask = GpuSceneConsumerShadow,
                .membershipCache = true,
                .forceDirectIncludesShadowReference = true,
                .capCommandsAtLocalMaximum = true,
                .maximumWorkCount = SpotShadowIndirectMaximumWorkCount,
                .workSlotCount = kSpotShadowEntryCapacity,
                .placement = CompactionPlacement::BatchedAtPrepare,
                .compactGpuRange = "gpu.shadow.spot.compact",
            },
            {
                .kind = IndirectViewKind::PointShadow,
                .oracleView = VulkanIndirectOracleView::PointShadow,
                .streamView = VulkanIndirectStreamView::PointShadow,
                .subject = "point-shadow",
                .shader = "assets/shaders/point_shadow_compact_comp.spv",
                .pipelineName = "point-shadow compact",
                .pushWords = 10u,
                .consumerMask = GpuSceneConsumerShadow,
                .membershipCache = true,
                .forceDirectIncludesShadowReference = true,
                .capCommandsAtLocalMaximum = true,
                .maximumWorkCount = PointShadowIndirectMaximumWorkCount,
                .workSlotCount = kPointShadowEntryCapacity * 6u,
                .placement = CompactionPlacement::BatchedAtPrepare,
                .compactGpuRange = "gpu.shadow.point.compact",
            },
            {
                .kind = IndirectViewKind::ReflectionProbe,
                .oracleView = VulkanIndirectOracleView::ReflectionProbe,
                .streamView = VulkanIndirectStreamView::ReflectionProbe,
                .subject = "reflection-probe",
                .shader = "assets/shaders/reflection_probe_capture_compact_comp.spv",
                .pipelineName = "reflection-probe compact",
                .pushWords = 10u,
                .consumerMask = GpuSceneConsumerProbe,
                .membershipCache = false,
                .forceDirectIncludesShadowReference = false,
                .capCommandsAtLocalMaximum = true,
                .maximumWorkCount = VulkanReflectionProbeCapturePass::MaximumFaceRecords,
                .workSlotCount = 0u,
                .placement = CompactionPlacement::PerWorkItem,
                .compactGpuRange = nullptr,
            },
        } };

        // Profile counter names per view, in emission order.
        struct TelemetryNames {
            const char* deviceCommands;
            const char* oracleCommands;
            const char* mismatchedBins;
            const char* deviceTriangles;
            const char* oracleTriangles;
            const char* deviceReducedCommands;
            const char* oracleReducedCommands;
            const char* mismatchedCommandRegions;
            const char* overflowCommands;
            const char* qualificationOracle;
        };
        constexpr std::array<TelemetryNames, kIndirectViewKindCount> Telemetry{ {
            { "shadow.directional.indirect.device_commands",
                "shadow.directional.indirect.oracle_commands",
                "shadow.directional.indirect.mismatched_bins",
                "shadow.directional.lod.device_triangles",
                "shadow.directional.lod.oracle_triangles",
                "shadow.directional.lod.device_reduced_commands",
                "shadow.directional.lod.oracle_reduced_commands",
                "shadow.directional.lod.mismatched_command_regions",
                "shadow.directional.indirect.overflow_commands",
                "shadow.directional.indirect.qualification_oracle" },
            { "shadow.spot.indirect.device_commands",
                "shadow.spot.indirect.oracle_commands",
                "shadow.spot.indirect.mismatched_bins",
                "shadow.spot.lod.device_triangles",
                "shadow.spot.lod.oracle_triangles",
                "shadow.spot.lod.device_reduced_commands",
                "shadow.spot.lod.oracle_reduced_commands",
                "shadow.spot.lod.mismatched_command_regions",
                "shadow.spot.indirect.overflow_commands",
                "shadow.spot.indirect.qualification_oracle" },
            { "shadow.point.indirect.device_commands",
                "shadow.point.indirect.oracle_commands",
                "shadow.point.indirect.mismatched_bins",
                "shadow.point.lod.device_triangles",
                "shadow.point.lod.oracle_triangles",
                "shadow.point.lod.device_reduced_commands",
                "shadow.point.lod.oracle_reduced_commands",
                "shadow.point.lod.mismatched_command_regions",
                "shadow.point.indirect.overflow_commands",
                "shadow.point.indirect.qualification_oracle" },
            { "probe.capture.indirect.device_commands",
                "probe.capture.indirect.oracle_commands",
                "probe.capture.indirect.mismatched_bins",
                "probe.capture.lod.device_triangles",
                "probe.capture.lod.oracle_triangles",
                "probe.capture.lod.device_reduced_commands",
                "probe.capture.lod.oracle_reduced_commands",
                "probe.capture.lod.mismatched_command_regions",
                "probe.capture.indirect.overflow_commands",
                "probe.capture.indirect.qualification_oracle" },
        } };

        // Work enumerators. Each assigns work indices in submission order and
        // lists the batched dispatches in the order the views recorded them.
        GpuSceneIndirectFallbackReason enumerateDirectional(const void* packets,
            size_t count, IndirectWorkSink& sink) {
            const auto shadows = std::span(
                static_cast<const DirectionalShadowFramePacket*>(packets), count);
            for (const DirectionalShadowFramePacket& shadow : shadows) {
                for (uint32_t cascade = 0;
                        cascade < kDirectionalShadowCascadeCount; ++cascade) {
                    if ((shadow.updateMask & (1u << cascade)) == 0u) continue;
                    const uint32_t layer = shadow.shadowIndex *
                        kDirectionalShadowCascadeCount + cascade;
                    if (layer >= sink.workIndices.size() ||
                        sink.workIndices[layer] != InvalidGpuSceneIndex)
                        return GpuSceneIndirectFallbackReason::InvalidPacket;
                    sink.workIndices[layer] = sink.workCount++;
                }
            }
            // Dispatches run in layer order.
            for (uint32_t layer = 0; layer < sink.workIndices.size(); ++layer) {
                const uint32_t workIndex = sink.workIndices[layer];
                if (workIndex == InvalidGpuSceneIndex) continue;
                sink.items->push_back({ layer, workIndex, 0u });
            }
            return GpuSceneIndirectFallbackReason::None;
        }

        GpuSceneIndirectFallbackReason enumerateSpot(const void* packets,
            size_t count, IndirectWorkSink& sink) {
            const auto shadows = std::span(
                static_cast<const SpotShadowFramePacket*>(packets), count);
            for (const SpotShadowFramePacket& shadow : shadows) {
                if (!shadow.update) continue;
                if (shadow.shadowDataSlot >= sink.workIndices.size() ||
                    sink.workIndices[shadow.shadowDataSlot] != InvalidGpuSceneIndex)
                    return GpuSceneIndirectFallbackReason::InvalidPacket;
                if (sink.workCount >= SpotShadowIndirectMaximumWorkCount)
                    return GpuSceneIndirectFallbackReason::CapacityExceeded;
                sink.workIndices[shadow.shadowDataSlot] = sink.workCount++;
            }
            for (const SpotShadowFramePacket& shadow : shadows) {
                if (!shadow.update) continue;
                sink.items->push_back({ shadow.shadowDataSlot,
                    sink.workIndices[shadow.shadowDataSlot], 0u });
            }
            return GpuSceneIndirectFallbackReason::None;
        }

        GpuSceneIndirectFallbackReason enumeratePoint(const void* packets,
            size_t count, IndirectWorkSink& sink) {
            const auto shadows = std::span(
                static_cast<const PointShadowFramePacket*>(packets), count);
            for (const PointShadowFramePacket& shadow : shadows) {
                if (!shadow.update) continue;
                if (shadow.shadowDataSlot >= kPointShadowEntryCapacity)
                    return GpuSceneIndirectFallbackReason::InvalidPacket;
                for (uint32_t face = 0; face < 6u; ++face) {
                    const uint32_t faceSlot = shadow.shadowDataSlot * 6u + face;
                    if (faceSlot >= sink.workIndices.size() ||
                        sink.workIndices[faceSlot] != InvalidGpuSceneIndex)
                        return GpuSceneIndirectFallbackReason::InvalidPacket;
                    if (sink.workCount >= PointShadowIndirectMaximumWorkCount)
                        return GpuSceneIndirectFallbackReason::CapacityExceeded;
                    sink.workIndices[faceSlot] = sink.workCount++;
                }
            }
            for (const PointShadowFramePacket& shadow : shadows) {
                if (!shadow.update) continue;
                for (uint32_t face = 0; face < 6u; ++face) {
                    const uint32_t faceSlot = shadow.shadowDataSlot * 6u + face;
                    sink.items->push_back({ faceSlot, sink.workIndices[faceSlot],
                        shadow.resolution });
                }
            }
            return GpuSceneIndirectFallbackReason::None;
        }

        // One work item per scheduled face, recorded per face by the caller.
        GpuSceneIndirectFallbackReason enumerateProbe(const void* packets,
            size_t count, IndirectWorkSink& sink) {
            const auto captures = std::span(
                static_cast<const ReflectionProbeCaptureScheduleEntry*>(packets),
                count);
            for (const ReflectionProbeCaptureScheduleEntry& capture : captures)
                sink.workCount += std::popcount(
                    static_cast<uint32_t>(capture.scheduledFaceMask));
            return sink.workCount >
                    VulkanReflectionProbeCapturePass::MaximumFaceRecords
                ? GpuSceneIndirectFallbackReason::CapacityExceeded
                : GpuSceneIndirectFallbackReason::None;
        }
    }

    const IndirectViewKindConfig& indirectViewKindConfig(
        IndirectViewKind kind) noexcept {
        return KindConfigs[static_cast<size_t>(kind)];
    }

    IndirectWorkEnumeration directionalShadowWork(
        std::span<const DirectionalShadowFramePacket> shadows) noexcept {
        return { shadows.data(), shadows.size(), enumerateDirectional };
    }
    IndirectWorkEnumeration spotShadowWork(
        std::span<const SpotShadowFramePacket> shadows) noexcept {
        return { shadows.data(), shadows.size(), enumerateSpot };
    }
    IndirectWorkEnumeration pointShadowWork(
        std::span<const PointShadowFramePacket> shadows) noexcept {
        return { shadows.data(), shadows.size(), enumeratePoint };
    }
    IndirectWorkEnumeration reflectionProbeWork(
        std::span<const ReflectionProbeCaptureScheduleEntry> captures) noexcept {
        return { captures.data(), captures.size(), enumerateProbe };
    }

    void VulkanCompactPipeline::destroy(VkDevice device) noexcept {
        if (device == VK_NULL_HANDLE) {
            *this = {};
            return;
        }
        if (pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, layout, nullptr);
        *this = {};
    }

    VulkanCompactPipeline createIndirectViewPipeline(VkDevice device,
        IndirectViewKind kind, VkDescriptorSetLayout set0Layout,
        VkDescriptorSetLayout gpuSceneLayout, VkDescriptorSetLayout indirectLayout) {
        const IndirectViewKindConfig& config = indirectViewKindConfig(kind);
        const std::array<VkDescriptorSetLayout, 3> setLayouts{
            set0Layout, gpuSceneLayout, indirectLayout };
        VulkanCompactPipeline result{};
        result.layout = createComputePipelineLayout(device, setLayouts,
            config.pushWords, config.pipelineName);
        try {
            result.pipeline = createComputePipeline(device, result.layout,
                config.shader, config.pipelineName);
        }
        catch (...) {
            result.destroy(device);
            throw;
        }
        return result;
    }

    void VulkanIndirectViewCuller::init(const VulkanCullerServices& services,
        IndirectViewKind kind, const VulkanCompactPipeline& pipeline,
        VkDescriptorSetLayout indirectLayout, IVulkanIndirectOracle* activeOracle) {
        services_ = services;
        config_ = &indirectViewKindConfig(kind);
        activeOracle_ = activeOracle;
        pipeline_ = pipeline;
        for (VkDescriptorSet& set : sets_)
            set = services_.resources.allocateSet(services_.resources.user,
                indirectLayout);
        workIndices_.assign(config_->workSlotCount, InvalidGpuSceneIndex);
        workItems_.reserve(config_->maximumWorkCount);
    }

    void VulkanIndirectViewCuller::destroy(VkDevice device) noexcept {
        for (VkDescriptorSet& set : sets_) {
            if (set != VK_NULL_HANDLE && services_.resources.freeSet != nullptr)
                services_.resources.freeSet(services_.resources.user, set);
            set = VK_NULL_HANDLE;
        }
        pipeline_.destroy(device);
        if (services_.resources.destroyBuffer != nullptr)
            buffers_.destroy(services_.resources);
        primitiveCapacity_ = 0;
        commandCapacity_ = 0;
        countCapacity_ = 0;
        bins_.clear();
        candidates_.clear();
        unsortedCandidates_.clear();
        binCursorScratch_.clear();
        primitiveBinScratch_.clear();
        membershipRevision_ = 0;
        pending_ = {};
    }

    void VulkanIndirectViewCuller::resize(uint32_t primitiveCapacity,
        bool frameOpen) {
        if (frameOpen || primitiveCapacity == 0u ||
            primitiveCapacity > services_.maximumPrimitiveCapacity)
            throw std::invalid_argument(std::string(config_->subject) +
                " indirect capacity is invalid at this frame boundary");
        const uint64_t requestedWorkCapacity =
            static_cast<uint64_t>(primitiveCapacity) * config_->maximumWorkCount;
        const uint32_t commandCapacity = config_->capCommandsAtLocalMaximum
            ? static_cast<uint32_t>((std::min)(requestedWorkCapacity,
                static_cast<uint64_t>(kLocalShadowIndirectMaximumCommandCount)))
            : primitiveCapacity * config_->maximumWorkCount;
        const uint32_t countCapacity = commandCapacity;
        VulkanIndirectBufferSet buffers = VulkanIndirectBufferSet::create(
            services_.resources, commandCapacity, countCapacity, primitiveCapacity);
        if (primitiveCapacity_ != 0u)
            services_.resources.waitForAllFrames(services_.resources.user);
        for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight; ++frame)
            collect(frame);
        buffers_.destroy(services_.resources);
        buffers_ = buffers;
        primitiveCapacity_ = primitiveCapacity;
        commandCapacity_ = commandCapacity;
        countCapacity_ = countCapacity;
        membershipRevision_ = 0u;
        bins_.reserve(primitiveCapacity);
        candidates_.reserve(primitiveCapacity);
        unsortedCandidates_.reserve(primitiveCapacity);
        binCursorScratch_.reserve(primitiveCapacity);
        primitiveBinScratch_.reserve(primitiveCapacity);
        if (sets_[0] != VK_NULL_HANDLE)
            bindIndirectBufferSet(services_.resources, sets_, buffers_);
    }

    bool VulkanIndirectViewCuller::reject(GpuSceneIndirectFallbackReason reason) {
        fallbackReason_ = reason;
        bins_.clear();
        candidates_.clear();
        unsortedCandidates_.clear();
        binCursorScratch_.clear();
        primitiveBinScratch_.clear();
        membershipRevision_ = 0u;
        return false;
    }

    bool VulkanIndirectViewCuller::anyAlphaMaskedBin() const noexcept {
        return std::ranges::any_of(bins_,
            [](const Bin& bin) { return bin.alphaMasked; });
    }

    bool VulkanIndirectViewCuller::plan(const IndirectViewInputs& inputs,
        const IndirectWorkEnumeration& work, uint32_t frame) {
        const IndirectViewKindConfig& config = *config_;
        std::fill(workIndices_.begin(), workIndices_.end(), InvalidGpuSceneIndex);
        workItems_.clear();
        fallbackReason_ = GpuSceneIndirectFallbackReason::None;
        cacheHit_ = false;
        if (!config.membershipCache) {
            bins_.clear();
            candidates_.clear();
            unsortedCandidates_.clear();
        }

        const size_t requested = inputs.primitiveIndices.size();
        if (requested == 0u) return false;
        const uint32_t lodErrorBits = std::bit_cast<uint32_t>(
            inputs.lodErrorThreshold);
        const bool reuseMembership = config.membershipCache &&
            inputs.membershipRevision != 0u &&
            membershipRevision_ == inputs.membershipRevision &&
            membershipLodErrorBits_ == lodErrorBits &&
            membershipMaximumLod_ == inputs.lodMaximumLevel;
        if (config.membershipCache && !reuseMembership) {
            bins_.clear();
            candidates_.clear();
            unsortedCandidates_.clear();
        }
        const GpuSceneIndirectPolicy policy{
            .multiDrawIndirect = services_.capabilities.multiDrawIndirect,
            .drawIndirectFirstInstance =
                services_.capabilities.drawIndirectFirstInstance,
            .drawIndirectCount = services_.capabilities.drawIndirectCount,
            .maxDrawIndirectCount = (std::min)(
                services_.capabilities.maxDrawIndirectCount, primitiveCapacity_),
            .minimumCommandCount = 8u,
            .forceDirectReference = inputs.forceDirectGBufferReference ||
                (config.forceDirectIncludesShadowReference &&
                    inputs.forceDirectShadowReference),
        };
        if (const GpuSceneIndirectFallbackReason reason =
                evaluateIndirectPolicy(policy, requested, primitiveCapacity_);
            reason != GpuSceneIndirectFallbackReason::None)
            return reject(reason);

        const VulkanIndirectScene& scene = inputs.scene;
        if (!reuseMembership) {
            primitiveBinScratch_.assign(scene.published.primitives,
                InvalidGpuSceneIndex);
            for (const uint32_t primitiveIndex : inputs.primitiveIndices) {
                VulkanResolvedCaster caster{};
                if (!resolveIndirectCaster(scene, primitiveIndex,
                        config.consumerMask, caster) ||
                    primitiveIndex >= scene.primitives.size() ||
                    primitiveIndex >= primitiveBinScratch_.size())
                    return reject(GpuSceneIndirectFallbackReason::InvalidPacket);
                const GpuScenePrimitiveRecord& primitive =
                    scene.primitives[primitiveIndex];
                if (primitive.binding.y >= scene.geometries.size())
                    return reject(GpuSceneIndirectFallbackReason::InvalidPacket);
                const GpuSceneGeometryRecord& packedGeometry =
                    scene.geometries[primitive.binding.y];
                VulkanIndirectGeometry geometry{};
                VulkanIndirectMaterial material{};
                if (!inputs.assets.geometry(inputs.assets.owner,
                        caster.geometry, geometry) ||
                    !inputs.assets.material(inputs.assets.owner,
                        caster.material, material) ||
                    geometry.vertexBuffer == VK_NULL_HANDLE ||
                    geometry.indexBuffer == VK_NULL_HANDLE ||
                    packedGeometry.draw.x != caster.firstIndex ||
                    packedGeometry.draw.y != caster.indexCount ||
                    packedGeometry.draw.w !=
                        static_cast<uint32_t>(geometry.indexFormat))
                    return reject(GpuSceneIndirectFallbackReason::InvalidPacket);
                const bool alphaMasked = material.alphaMode == 1u;
                const bool doubleSided = material.doubleSided != 0u;
                const VkIndexType indexType = toVkIndexType(geometry.indexFormat);
                auto bin = std::find_if(bins_.begin(), bins_.end(),
                    [&](const Bin& candidate) {
                        return candidate.vertexBuffer == geometry.vertexBuffer &&
                            candidate.indexBuffer == geometry.indexBuffer &&
                            candidate.indexType == indexType &&
                            candidate.alphaMasked == alphaMasked &&
                            candidate.doubleSided == doubleSided;
                    });
                if (bin == bins_.end()) {
                    bins_.push_back({
                        .geometry = caster.geometry,
                        .vertexBuffer = geometry.vertexBuffer,
                        .indexBuffer = geometry.indexBuffer,
                        .indexType = indexType,
                        .alphaMasked = alphaMasked,
                        .doubleSided = doubleSided,
                    });
                    bin = std::prev(bins_.end());
                }
                const uint32_t binIndex = static_cast<uint32_t>(
                    std::distance(bins_.begin(), bin));
                const uint32_t maximumLod = inputs.lodErrorThreshold > 0.0f
                    ? residentLodPrefix(scene.geometries, scene.instances,
                        primitive, inputs.lodMaximumLevel, geometry, inputs.assets)
                    : 0u;
                ++bin->commandCount;
                unsortedCandidates_.push_back({
                    .primitiveIndex = primitiveIndex,
                    .binIndex = binIndex,
                    .maximumLod = maximumLod,
                });
                primitiveBinScratch_[primitiveIndex] = binIndex;
            }

            uint32_t commandBegin = 0u;
            for (Bin& bin : bins_) {
                bin.commandBegin = commandBegin;
                commandBegin += bin.commandCount;
            }
            candidates_.resize(unsortedCandidates_.size());
            binCursorScratch_.clear();
            for (const Bin& bin : bins_)
                binCursorScratch_.push_back(bin.commandBegin);
            for (const GpuSceneIndirectCandidate& source : unsortedCandidates_) {
                const Bin& bin = bins_[source.binIndex];
                const uint32_t destination =
                    binCursorScratch_[source.binIndex]++;
                candidates_[destination] = {
                    .primitiveIndex = source.primitiveIndex,
                    .binIndex = source.binIndex,
                    .commandBase = bin.commandBegin,
                    .commandCapacity = bin.commandCount,
                    .maximumLod = source.maximumLod,
                };
            }
            if (config.membershipCache) {
                membershipRevision_ = inputs.membershipRevision;
                membershipLodErrorBits_ = lodErrorBits;
                membershipMaximumLod_ = inputs.lodMaximumLevel;
            }
        }
        else {
            cacheHit_ = true;
        }

        uint32_t commandBegin = 0u;
        for (const Bin& bin : bins_) commandBegin += bin.commandCount;

        IndirectWorkSink sink{ .workIndices = workIndices_, .items = &workItems_ };
        if (const GpuSceneIndirectFallbackReason reason =
                work.enumerate(work.packets, work.packetCount, sink);
            reason != GpuSceneIndirectFallbackReason::None)
            return reject(reason);
        const uint32_t workCount = sink.workCount;
        const uint64_t requiredCommands =
            static_cast<uint64_t>(commandBegin) * workCount;
        const uint64_t requiredCounts =
            static_cast<uint64_t>(bins_.size()) * workCount;
        if (workCount == 0u || requiredCommands > commandCapacity_ ||
            requiredCounts > countCapacity_)
            return reject(GpuSceneIndirectFallbackReason::CapacityExceeded);

        PendingValidation& validation = pending_[frame];
        if (validation.pending)
            throw std::logic_error(std::string(config.subject) +
                " indirect validation slot is still in flight");
        validation.profileFrameId = services_.profiler != nullptr &&
                services_.profiler->isFrameOpen()
            ? services_.profiler->currentFrameId() : 0u;
        if (activeOracle_ != nullptr)
            activeOracle_->beginShadowWork(config.oracleView, frame,
                static_cast<size_t>(requiredCounts));
        validation.countCapacities.clear();
        validation.commandOffsets.clear();
        validation.countCapacities.reserve(static_cast<size_t>(requiredCounts));
        validation.commandOffsets.reserve(static_cast<size_t>(requiredCounts));
        for (uint32_t work = 0; work < workCount; ++work)
            for (const Bin& bin : bins_) {
                validation.countCapacities.push_back(bin.commandCount);
                validation.commandOffsets.push_back(
                    work * commandBegin + bin.commandBegin);
            }
        validation.pending = true;

        commandStride_ = commandBegin;
        lodErrorBits_ = lodErrorBits;
        published_ = scene.published;
        scene_ = scene;

        const VulkanIndirectStreamTap tap = stream(frame);
        tap.begin(buffers_.commands[frame].buffer, buffers_.counts[frame].buffer);
        std::memcpy(buffers_.candidates[frame].mapped, candidates_.data(),
            candidates_.size() * sizeof(GpuSceneIndirectCandidate));
        std::memset(buffers_.counts[frame].mapped, 0,
            static_cast<size_t>(requiredCounts) * sizeof(uint32_t));
        tap.hostWrite(VulkanIndirectStreamHostTarget::Candidates,
            buffers_.candidates[frame].mapped,
            candidates_.size() * sizeof(GpuSceneIndirectCandidate));
        tap.hostWrite(VulkanIndirectStreamHostTarget::Counts,
            buffers_.counts[frame].mapped,
            static_cast<size_t>(requiredCounts) * sizeof(uint32_t));
        return true;
    }

    uint32_t VulkanIndirectViewCuller::recordCompaction(VkCommandBuffer cmd,
        uint32_t frame, const IndirectCompactionSets& sets) {
        const VulkanCullerCommands& commands = services_.commands;
        const VulkanIndirectStreamTap tap = stream(frame);
        constexpr VkAccessFlags hostSrc = VK_ACCESS_HOST_WRITE_BIT;
        constexpr VkAccessFlags hostDst =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        commands.memoryBarrier(commands.user, cmd, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, hostSrc, hostDst);
        tap.barrier(VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, hostSrc, hostDst);
        if (config_->placement != CompactionPlacement::BatchedAtPrepare)
            return 0u;

        VulkanGpuRangeToken range = commands.beginGpuRange(commands.user,
            config_->compactGpuRange);
        tap.gpuRange(config_->compactGpuRange);
        commands.bindPipeline(commands.user, cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_.pipeline);
        tap.bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline);
        const std::array<VkDescriptorSet, 3> boundSets{
            sets.set0, sets.gpuScene, sets_[frame] };
        commands.bindDescriptorSets(commands.user, cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout, 0u,
            static_cast<uint32_t>(boundSets.size()), boundSets.data(), 0u, nullptr);
        tap.bindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout,
            0u, boundSets);
        uint32_t dispatches = 0u;
        for (const IndirectWorkItem& item : workItems_) {
            const std::array<uint32_t, 10> parameters{
                static_cast<uint32_t>(candidates_.size()),
                published_.transforms,
                published_.instances,
                published_.primitives,
                published_.geometries,
                item.slotWord,
                item.workIndex * static_cast<uint32_t>(bins_.size()),
                item.workIndex * commandStride_,
                lodErrorBits_,
                item.extraWord,
            };
            const uint32_t pushBytes = config_->pushWords * sizeof(uint32_t);
            commands.pushConstants(commands.user, cmd, pipeline_.layout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0u, pushBytes, parameters.data());
            tap.pushConstants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                parameters.data(), pushBytes);
            const uint32_t groups = (parameters[0] + 63u) / 64u;
            commands.dispatch(commands.user, cmd, groups, 1u, 1u);
            tap.dispatch(groups, 1u, 1u);
            ++dispatches;
        }
        commands.endGpuRange(commands.user, range);
        constexpr VkAccessFlags drawSrc = VK_ACCESS_SHADER_WRITE_BIT;
        constexpr VkAccessFlags drawDst = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        commands.memoryBarrier(commands.user, cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, drawSrc, drawDst);
        tap.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, drawSrc, drawDst);
        return dispatches;
    }

    uint32_t VulkanIndirectViewCuller::recordWorkItem(VkCommandBuffer cmd,
        uint32_t frame, const IndirectCompactionSets& sets,
        const IndirectWorkItem& item) {
        const VulkanCullerCommands& commands = services_.commands;
        const VulkanIndirectStreamTap tap = stream(frame);
        commands.bindPipeline(commands.user, cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_.pipeline);
        tap.bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline);
        commands.bindDescriptorSets(commands.user, cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout, 0u, 1u, &sets.set0,
            1u, &sets.set0DynamicOffset);
        tap.bindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout,
            0u, { &sets.set0, 1u }, { &sets.set0DynamicOffset, 1u });
        const std::array<VkDescriptorSet, 2> boundSets{ sets.gpuScene, sets_[frame] };
        commands.bindDescriptorSets(commands.user, cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout, 1u,
            static_cast<uint32_t>(boundSets.size()), boundSets.data(), 0u, nullptr);
        tap.bindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout,
            1u, boundSets);
        const uint32_t candidateCount = static_cast<uint32_t>(candidates_.size());
        const std::array<uint32_t, 10> parameters{
            candidateCount,
            published_.transforms,
            published_.instances,
            published_.primitives,
            published_.geometries,
            item.slotWord,
            item.workIndex * static_cast<uint32_t>(bins_.size()),
            item.workIndex * candidateCount,
            lodErrorBits_,
            item.extraWord,
        };
        const uint32_t pushBytes = config_->pushWords * sizeof(uint32_t);
        commands.pushConstants(commands.user, cmd, pipeline_.layout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, pushBytes, parameters.data());
        tap.pushConstants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
            parameters.data(), pushBytes);
        const uint32_t groups = (candidateCount + 63u) / 64u;
        commands.dispatch(commands.user, cmd, groups, 1u, 1u);
        tap.dispatch(groups, 1u, 1u);
        constexpr VkAccessFlags drawSrc = VK_ACCESS_SHADER_WRITE_BIT;
        constexpr VkAccessFlags drawDst = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        commands.memoryBarrier(commands.user, cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, drawSrc, drawDst);
        tap.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, drawSrc, drawDst);
        return 1u;
    }

    uint32_t VulkanIndirectViewCuller::recordDraws(VkCommandBuffer cmd,
        uint32_t frame, uint32_t workIndex, const IndirectDrawBinder& binder) {
        const VulkanCullerCommands& commands = services_.commands;
        const VulkanIndirectStreamTap tap = stream(frame);
        const uint32_t commandRegion =
            workIndex * static_cast<uint32_t>(candidates_.size());
        const uint32_t countRegion = workIndex * static_cast<uint32_t>(bins_.size());
        const VkBuffer commandBuffer = buffers_.commands[frame].buffer;
        const VkBuffer countBuffer = buffers_.counts[frame].buffer;
        VkPipeline activePipeline = VK_NULL_HANDLE;
        uint32_t drawn = 0u;
        for (uint32_t binIndex = 0; binIndex < bins_.size(); ++binIndex) {
            const Bin& bin = bins_[binIndex];
            const VkPipeline pipeline = binder.pipeline(binder.owner,
                bin.alphaMasked, bin.doubleSided);
            if (pipeline != activePipeline) {
                commands.bindPipeline(commands.user, cmd,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                activePipeline = pipeline;
            }
            commands.bindVertexBuffer(commands.user, cmd, bin.vertexBuffer, 0u);
            commands.bindIndexBuffer(commands.user, cmd, bin.indexBuffer, 0u,
                bin.indexType);
            if (binder.pushSlotWord) {
                CanonicalMeshPushConstants push{};
                push.padding[0] = binder.slotWord;
                commands.pushConstants(commands.user, cmd, binder.layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
            }
            const VkDeviceSize commandOffset =
                static_cast<VkDeviceSize>(commandRegion + bin.commandBegin) *
                sizeof(GpuSceneIndexedIndirectCommand);
            const VkDeviceSize countOffset =
                static_cast<VkDeviceSize>(countRegion + binIndex) * sizeof(uint32_t);
            commands.drawIndexedIndirectCount(commands.user, cmd, commandBuffer,
                commandOffset, countBuffer, countOffset, bin.commandCount,
                sizeof(GpuSceneIndexedIndirectCommand));
            tap.indirectDraw(pipeline, bin.vertexBuffer, bin.indexBuffer,
                bin.indexType, binder.pushSlotWord ? binder.slotWord : UINT32_MAX,
                commandBuffer, commandOffset, countBuffer, countOffset,
                bin.commandCount);
            ++drawn;
        }
        return drawn;
    }

    void VulkanIndirectViewCuller::emitExpectations(IVulkanIndirectOracle& oracle,
        uint32_t frame, uint32_t workIndex,
        std::span<const VulkanResolvedCaster> casters,
        std::span<const uint8_t> visibility, uint8_t visibilityBit,
        const IndirectLodMetric& lod) const {
        const PendingValidation& validation = pending_[frame];
        const uint32_t countRegion = workIndex * static_cast<uint32_t>(bins_.size());
        for (size_t casterIndex = 0; casterIndex < casters.size(); ++casterIndex) {
            const VulkanResolvedCaster& caster = casters[casterIndex];
            if (caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex ||
                (visibility[casterIndex] & visibilityBit) == 0u)
                continue;
            uint32_t binIndex = InvalidGpuSceneIndex;
            if (caster.gpuScenePrimitiveIndex < primitiveBinScratch_.size())
                binIndex = primitiveBinScratch_[caster.gpuScenePrimitiveIndex];
            if (binIndex >= bins_.size()) continue;
            const size_t countIndex = countRegion + binIndex;
            if (countIndex >= validation.countCapacities.size()) continue;
            const uint32_t primitiveIndex = caster.gpuScenePrimitiveIndex;
            const GpuScenePrimitiveRecord& primitive =
                scene_.primitives[primitiveIndex];
            const Bin& commandBin = bins_[binIndex];
            const auto candidateBegin = candidates_.begin() + commandBin.commandBegin;
            const auto candidateEnd = candidateBegin + commandBin.commandCount;
            const auto candidate = std::find_if(candidateBegin, candidateEnd,
                [&](const GpuSceneIndirectCandidate& value) {
                    return value.primitiveIndex == primitiveIndex;
                });
            if (candidate == candidateEnd)
                throw std::logic_error(std::string(config_->subject) +
                    " LOD oracle lost its candidate");
            const uint32_t geometryIndex = lod.select(lod.context, scene_,
                primitive, candidate->maximumLod);
            const GpuSceneGeometryRecord& selected = scene_.geometries[geometryIndex];
            oracle.expectShadowCommand(config_->oracleView, frame, countIndex, {
                .indexCount = selected.draw.y,
                .instanceCount = 1u,
                .firstIndex = selected.draw.x,
                .vertexOffset = std::bit_cast<int32_t>(selected.draw.z),
                .firstInstance = primitiveIndex,
            });
        }
    }

    void VulkanIndirectViewCuller::collect(uint32_t frame) {
        PendingValidation& validation = pending_[frame];
        if (!validation.pending) return;
        const TelemetryNames& names = Telemetry[static_cast<size_t>(config_->kind)];
        const auto* counts = static_cast<const uint32_t*>(
            buffers_.counts[frame].mapped);
        const auto* commands = static_cast<const GpuSceneIndexedIndirectCommand*>(
            buffers_.commands[frame].mapped);
        const VulkanIndirectScene scene =
            services_.scene(services_.sceneOwner, frame);
        stream(frame).retire({ .counts = counts, .commands = commands,
            .countCapacities = validation.countCapacities,
            .commandOffsets = validation.commandOffsets,
            .primitives = scene.primitives,
            .instances = scene.instances,
            .transforms = scene.transforms });
        uint64_t deviceCommands = 0u;
        uint64_t overflowCommands = 0u;
        uint64_t deviceTriangles = 0u;
        uint64_t deviceReducedCommands = 0u;
        const auto baseIndexCount = [&](uint32_t primitiveIndex) {
            if (primitiveIndex >= scene.primitives.size()) return 0u;
            const uint32_t geometryIndex =
                scene.primitives[primitiveIndex].binding.y;
            return geometryIndex < scene.geometries.size()
                ? scene.geometries[geometryIndex].draw.y : 0u;
        };
        for (size_t index = 0; index < validation.countCapacities.size(); ++index) {
            const uint32_t capacity = validation.countCapacities[index];
            const uint32_t deviceCount = counts != nullptr ? counts[index] : 0u;
            const uint32_t submitted = (std::min)(deviceCount, capacity);
            deviceCommands += submitted;
            const uint32_t commandOffset = index < validation.commandOffsets.size()
                ? validation.commandOffsets[index] : 0u;
            for (uint32_t commandIndex = 0u;
                    commands != nullptr && commandIndex < submitted; ++commandIndex) {
                const GpuSceneIndexedIndirectCommand command =
                    commands[commandOffset + commandIndex];
                deviceTriangles += command.indexCount / 3u;
                deviceReducedCommands += command.indexCount <
                    baseIndexCount(command.firstInstance) ? 1u : 0u;
            }
            overflowCommands += deviceCount > capacity
                ? static_cast<uint64_t>(deviceCount - capacity) : 0u;
        }
        VulkanIndirectOracleResult oracle{};
        if (services_.oracle != nullptr) {
            oracle = services_.oracle->verifyShadowWork(config_->oracleView, frame, {
                .counts = counts,
                .commands = commands,
                .countCapacities = validation.countCapacities,
                .commandOffsets = validation.commandOffsets,
                .primitives = scene.primitives,
                .geometries = scene.geometries,
            });
        }
        CpuProfiler* const profiler = services_.profiler;
        if (validation.profileFrameId != 0u && profiler != nullptr) {
            const uint64_t frameId = validation.profileFrameId;
            (void)profiler->attachCounter(frameId, names.deviceCommands,
                deviceCommands);
            (void)profiler->attachCounter(frameId, names.oracleCommands,
                oracle.oracleCommands);
            (void)profiler->attachCounter(frameId, names.mismatchedBins,
                oracle.mismatchedBins);
            (void)profiler->attachCounter(frameId, names.deviceTriangles,
                deviceTriangles);
            (void)profiler->attachCounter(frameId, names.oracleTriangles,
                oracle.oracleTriangles);
            (void)profiler->attachCounter(frameId, names.deviceReducedCommands,
                deviceReducedCommands);
            (void)profiler->attachCounter(frameId, names.oracleReducedCommands,
                oracle.oracleReducedCommands);
            (void)profiler->attachCounter(frameId, names.mismatchedCommandRegions,
                oracle.mismatchedCommandRegions);
            (void)profiler->attachCounter(frameId, names.overflowCommands,
                overflowCommands);
            (void)profiler->attachCounter(frameId, names.qualificationOracle,
                oracle.validated ? 1u : 0u);
        }
        validation.pending = false;
        validation.commandOffsets.clear();
        if (oracle.mismatchedBins != 0u || oracle.mismatchedCommandRegions != 0u ||
            overflowCommands != 0u) {
            std::ostringstream diagnostic;
            diagnostic << config_->subject
                << " device commands disagree with the CPU visibility/LOD oracle"
                << " (bins=" << oracle.mismatchedBins
                << ", command_regions=" << oracle.mismatchedCommandRegions
                << ", overflow=" << overflowCommands
                << ", device_triangles=" << deviceTriangles
                << ", oracle_triangles=" << oracle.oracleTriangles
                << ", device_reduced=" << deviceReducedCommands
                << ", oracle_reduced=" << oracle.oracleReducedCommands << ')';
            throw std::runtime_error(diagnostic.str());
        }
    }

} // namespace Iridium
