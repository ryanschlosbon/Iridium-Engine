#include "VulkanClusterLightingFeature.h"

#include "VulkanFrameTelemetry.h"
#include "VulkanRenderGraphExecutor.h"
#include "renderer/rhi/LightUploadPlanner.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace Iridium {

    void VulkanClusterLightingFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        clusters_.init(context.device, context.pipelineCache, context.descriptors);
        probeClusters_.init(context.device, context.pipelineCache, context.descriptors);
    }

    void VulkanClusterLightingFeature::onGraphRebuilt(
        const VulkanProductionGraphIds& ids) {
        ids_ = ids.cluster;
        probeClusterPass_ = ids.probeCluster;
        readbackPass_ = ids.clusterReadback;
        // The cluster sets reference graph-owned buffers; the first rebuild
        // precedes the light buffers (createLightRecordBuffers binds then).
        if (lightRecordBuffers_[0].isValid()) rebuildClusterDescriptors();
    }

    void VulkanClusterLightingFeature::registerPasses(
        VulkanRenderGraphExecutor& graph) {
        // R3b.7: the dispatch's consumers receive the compute -> fragment
        // barrier from the executor.
        graph.registerPass(probeClusterPass_, { this, &probeClusterActive,
            &executeProbeCluster, "gpu.lighting.probe_cluster",
            GpuRangePlacement::BeforeBarriers });
        graph.registerPass(ids_.clear, { this, nullptr, &executeClear });
        graph.registerPass(ids_.count, { this, nullptr, &executeCount });
        graph.registerPass(ids_.scan, { this, nullptr, &executeScan });
        graph.registerPass(ids_.fill, { this, nullptr, &executeFill });
        graph.registerPass(ids_.finalize, { this, nullptr, &executeFinalize });
        // The diagnostics copy is declared only for CPU telemetry; it stays
        // inside gpu.lighting.cluster.
        if (readbackPass_.isValid())
            graph.registerPass(readbackPass_, { this, nullptr, &executeReadback });
        graph.registerRangeGroup({ "gpu.lighting.cluster", ids_.clear,
            readbackPass_.isValid() ? readbackPass_ : ids_.finalize });
    }

    void VulkanClusterLightingFeature::onGraphReleased() {
        // The clustered pass owns descriptor sets that reference transient
        // render-graph buffers; retire them before the graph's buffers.
        clusters_.clearDescriptors();
    }

    void VulkanClusterLightingFeature::onFrameSlotRetired(uint32_t frameIndex) {
        collectDiagnostics(frameIndex);
    }

    void VulkanClusterLightingFeature::destroy() noexcept {
        clusters_.clearDescriptors();
        probeClusters_.clearDescriptors();
        if (context_ != nullptr) {
            for (auto* buffers : { &lightRecordBuffers_, &activeLightSlotBuffers_,
                    &fallbackCandidateBuffers_, &parameterBuffers_,
                    &diagnosticReadbackBuffers_, &pendingLightRecordBuffers_,
                    &pendingActiveLightSlotBuffers_ })
                for (VulkanBufferResource& buffer : *buffers)
                    context_->allocator.destroy(buffer);
        }
        pendingSlots_ = {};
        clusters_.cleanup();
        probeClusters_.cleanup();
        lightRecordCapacity_ = 0;
        context_ = nullptr;
    }

    bool VulkanClusterLightingFeature::prepare(uint32_t requiredCapacity) {
        if (requiredCapacity <= lightRecordCapacity_) return false;
        if (requiredCapacity > lightRecordMaximumCapacity_) {
            throw std::overflow_error(
                "GPU light records exhausted the device storage-buffer limit");
        }
        createLightRecordBuffers(nextMaterialTableCapacity(
            lightRecordCapacity_, requiredCapacity,
            lightRecordMaximumCapacity_));
        return true;
    }

    void VulkanClusterLightingFeature::ShadowSlotMapping::publish(
        const std::vector<uint32_t>& next) {
        if (next != slots) {
            slots = next;
            ++revision;
            if (revision == 0u)
                ++revision;
        }
    }

    void VulkanClusterLightingFeature::publishSpotShadowSlots(
        const std::vector<uint32_t>& slots) {
        spotShadowSlots_.publish(slots);
    }

    void VulkanClusterLightingFeature::publishPointShadowSlots(
        const std::vector<uint32_t>& slots) {
        pointShadowSlots_.publish(slots);
    }

    VulkanClusterLightingFeature::FrameBufferInfos
        VulkanClusterLightingFeature::lightRecordDescriptors() const noexcept {
        FrameBufferInfos descriptors{};
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            descriptors[frame].buffer = lightRecordBuffers_[frame].buffer;
            descriptors[frame].range = lightRecordBuffers_[frame].size;
        }
        return descriptors;
    }

    std::array<VulkanClusterSceneBufferDescriptors,
        VulkanClusterLightingFeature::FrameCount>
        VulkanClusterLightingFeature::sceneClusterDescriptors() const {
        std::array<VulkanClusterSceneBufferDescriptors, FrameCount> descriptors{};
        const VulkanRenderGraphExecutor& graph = context_->graph;
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            const auto info = [](const VulkanBufferResource& buffer) {
                return VkDescriptorBufferInfo{ buffer.buffer, 0, buffer.size };
            };
            descriptors[frame] = {
                info(graph.buffer(frame, ids_.global)),
                info(graph.buffer(frame, ids_.headers)),
                info(graph.buffer(frame, ids_.indices)),
                info(graph.buffer(frame, ids_.fallback)),
                info(graph.buffer(frame, ids_.diagnostics)),
                { parameterBuffers_[frame].buffer, 0,
                    sizeof(PackedGpuClusterParameters) },
            };
        }
        return descriptors;
    }

    void VulkanClusterLightingFeature::rebuildClusterDescriptors() {
        FrameBufferInfos records{};
        FrameBufferInfos active{};
        FrameBufferInfos fallbackCandidates{};
        FrameBufferInfos parameters{};
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            records[frame] = { lightRecordBuffers_[frame].buffer, 0,
                lightRecordBuffers_[frame].size };
            active[frame] = { activeLightSlotBuffers_[frame].buffer, 0,
                activeLightSlotBuffers_[frame].size };
            fallbackCandidates[frame] = {
                fallbackCandidateBuffers_[frame].buffer, 0,
                fallbackCandidateBuffers_[frame].size };
            parameters[frame] = { parameterBuffers_[frame].buffer, 0,
                sizeof(PackedGpuClusterParameters) };
        }
        clusters_.rebuildDescriptors(context_->graph, ids_, records, active,
            fallbackCandidates, parameters);
    }

    void VulkanClusterLightingFeature::createLightRecordBuffers(uint32_t capacity) {
        if (capacity == 0 || capacity > lightRecordMaximumCapacity_) {
            throw std::invalid_argument(
                "GPU light record capacity is outside the device limit");
        }
        if (context_->frameOpen) {
            throw std::logic_error(
                "GPU light record buffers may grow only at a frame boundary");
        }
        const VkDeviceSize recordBytes = static_cast<VkDeviceSize>(capacity) *
            sizeof(PackedGpuLight);
        const VkDeviceSize activeBytes = static_cast<VkDeviceSize>(capacity) *
            sizeof(uint32_t);
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> recordReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> activeReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> fallbackReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> parameterReplacement{};
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight> readbackReplacement{};
        const bool createParameters = !parameterBuffers_[0].isValid();
        try {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                recordReplacement[frame] = context_->allocator.createBuffer(recordBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::LightGpu);
                activeReplacement[frame] = context_->allocator.createBuffer(activeBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::LightGpu);
                if (createParameters) {
                    fallbackReplacement[frame] = context_->allocator.createBuffer(
                        kMaximumClusterFallbackLights * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::LightGpu);
                    parameterReplacement[frame] = context_->allocator.createBuffer(
                        sizeof(PackedGpuClusterParameters),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::LightGpu);
                    readbackReplacement[frame] = context_->allocator.createBuffer(
                        64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        true, ProfileMemoryCategory::LightGpu);
                }
                std::memset(recordReplacement[frame].mapped, 0,
                    static_cast<size_t>(recordBytes));
                std::memset(activeReplacement[frame].mapped, 0,
                    static_cast<size_t>(activeBytes));
                if (createParameters) {
                    std::memset(fallbackReplacement[frame].mapped, 0xff,
                        kMaximumClusterFallbackLights * sizeof(uint32_t));
                    std::memset(parameterReplacement[frame].mapped, 0,
                        sizeof(PackedGpuClusterParameters));
                    std::memset(readbackReplacement[frame].mapped, 0, 64);
                }
            }
        }
        catch (...) {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                context_->allocator.destroy(recordReplacement[frame]);
                context_->allocator.destroy(activeReplacement[frame]);
                context_->allocator.destroy(fallbackReplacement[frame]);
                context_->allocator.destroy(parameterReplacement[frame]);
                context_->allocator.destroy(readbackReplacement[frame]);
            }
            throw;
        }
        // R4c.2: no drain. A slot that is not in flight swaps now; an
        // in-flight slot parks its replacement (an older parked one was never
        // used) until its retirement. With every slot idle (startup) the
        // cluster sets are rebuilt as before.
        bool allIdle = true;
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            context_->allocator.destroy(pendingLightRecordBuffers_[frame]);
            context_->allocator.destroy(pendingActiveLightSlotBuffers_[frame]);
            pendingSlots_[frame] = context_->scheduler.slotInFlight(frame);
            allIdle = allIdle && !pendingSlots_[frame];
        }
        if (allIdle) clusters_.clearDescriptors();
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            if (pendingSlots_[frame]) {
                pendingLightRecordBuffers_[frame] = recordReplacement[frame];
                pendingActiveLightSlotBuffers_[frame] = activeReplacement[frame];
                continue;
            }
            context_->allocator.destroy(lightRecordBuffers_[frame]);
            context_->allocator.destroy(activeLightSlotBuffers_[frame]);
            lightRecordBuffers_[frame] = recordReplacement[frame];
            activeLightSlotBuffers_[frame] = activeReplacement[frame];
        }
        if (createParameters) {
            fallbackCandidateBuffers_ = fallbackReplacement;
            parameterBuffers_ = parameterReplacement;
            diagnosticReadbackBuffers_ = readbackReplacement;
        }
        lightRecordCapacity_ = capacity;
        for (std::vector<uint64_t>& revisions : uploadedLightRevisions_) {
            revisions.assign(capacity, uint64_t{ 0 });
        }
        uploadedActiveListRevisions_.fill(0);
        spotShadowSlots_.uploaded.fill(0);
        pointShadowSlots_.uploaded.fill(0);
        diagnosticReadbackPending_.fill(false);
        lightUploadRanges_.reserve(capacity);
        fallbackSelectionScratch_.reserve(capacity);
        if (allIdle) {
            rebuildClusterDescriptors();
            return;
        }
        for (uint32_t frame = 0; frame < FrameCount; ++frame)
            if (!pendingSlots_[frame])
                clusters_.rewriteLightBuffers(frame,
                    { lightRecordBuffers_[frame].buffer, 0,
                        lightRecordBuffers_[frame].size },
                    { activeLightSlotBuffers_[frame].buffer, 0,
                        activeLightSlotBuffers_[frame].size });
    }

    bool VulkanClusterLightingFeature::swapRetiredSlot(uint32_t slot) {
        if (!pendingSlots_[slot]) return false;
        context_->allocator.destroy(lightRecordBuffers_[slot]);
        context_->allocator.destroy(activeLightSlotBuffers_[slot]);
        lightRecordBuffers_[slot] = pendingLightRecordBuffers_[slot];
        activeLightSlotBuffers_[slot] = pendingActiveLightSlotBuffers_[slot];
        pendingLightRecordBuffers_[slot] = {};
        pendingActiveLightSlotBuffers_[slot] = {};
        pendingSlots_[slot] = false;
        clusters_.rewriteLightBuffers(slot,
            { lightRecordBuffers_[slot].buffer, 0, lightRecordBuffers_[slot].size },
            { activeLightSlotBuffers_[slot].buffer, 0,
                activeLightSlotBuffers_[slot].size });
        return true;
    }

    void VulkanClusterLightingFeature::uploadFrame(uint32_t frameIndex,
        const glm::mat4& view, const glm::mat4& projection, float nearPlane,
        float farPlane, const LightingFramePacket& lights, VkExtent2D sceneExtent,
        const EnvironmentLightingSettings& environment) {
        uploadLights(frameIndex, lights);
        updateFallbackCandidates(frameIndex, view, lights);
        updateParameters(frameIndex, view, projection, nearPlane, farPlane,
            lights.stats.activeLightCount, sceneExtent, environment);
    }

    void VulkanClusterLightingFeature::recordProbeCluster(uint32_t clusterCount) {
        CpuScope probeClusterScope(context_->profiler,
            "cpu.render.record.probe_cluster");
        stagedProbeClusterCount_ = clusterCount;
        context_->graph.drainRegisteredThrough(probeClusterPass_);
    }

    void VulkanClusterLightingFeature::recordClusters(uint32_t frameIndex,
        uint32_t clusterCount, uint32_t activeLightCount) {
        CpuScope clusterRecordScope(context_->profiler, "cpu.render.record.cluster");
        stagedClusterCount_ = clusterCount;
        stagedActiveLightCount_ = activeLightCount;
        context_->graph.drainRegisteredThrough(
            readbackPass_.isValid() ? readbackPass_ : ids_.finalize);
        submittedClusterCounts_[frameIndex] = clusterCount;
    }

    void VulkanClusterLightingFeature::recordFrame(uint32_t frameIndex,
        const glm::mat4& view, const glm::mat4& projection, float nearPlane,
        float farPlane, VkExtent2D sceneExtent, uint32_t activeLightCount) {
        const ClusterGridDimensions dimensions = clusterGridDimensions(config_,
            { sceneExtent.width, sceneExtent.height, nearPlane, farPlane,
                view, projection });
        recordProbeCluster(static_cast<uint32_t>(dimensions.clusterCount()));
        recordClusters(frameIndex, static_cast<uint32_t>(dimensions.clusterCount()),
            activeLightCount);
    }

    bool VulkanClusterLightingFeature::probeClusterActive(void* owner,
        const VulkanFrameRecordContext&) {
        return static_cast<const VulkanClusterLightingFeature*>(owner)->
            stagedProbeClusterCount_ != 0u;
    }

    void VulkanClusterLightingFeature::executeProbeCluster(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        self.context_->telemetry.counters().dispatchRecorded +=
            self.probeClusters_.record(context.commandBuffer,
                context.frame.frameIndex, self.stagedProbeClusterCount_);
    }

    void VulkanClusterLightingFeature::executeClear(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        self.context_->telemetry.counters().dispatchRecorded +=
            self.clusters_.recordClear(context.commandBuffer,
                context.frame.frameIndex, self.stagedClusterCount_);
    }

    void VulkanClusterLightingFeature::executeCount(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        self.context_->telemetry.counters().dispatchRecorded +=
            self.clusters_.recordCount(context.commandBuffer,
                context.frame.frameIndex, self.stagedActiveLightCount_);
    }

    void VulkanClusterLightingFeature::executeScan(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        self.context_->telemetry.counters().dispatchRecorded +=
            self.clusters_.recordScan(context.commandBuffer,
                context.frame.frameIndex, self.stagedClusterCount_);
    }

    void VulkanClusterLightingFeature::executeFill(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        self.context_->telemetry.counters().dispatchRecorded +=
            self.clusters_.recordFill(context.commandBuffer,
                context.frame.frameIndex, self.stagedActiveLightCount_);
    }

    void VulkanClusterLightingFeature::executeFinalize(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        self.context_->telemetry.counters().dispatchRecorded +=
            self.clusters_.recordFinalize(context.commandBuffer,
                context.frame.frameIndex,
                context.graph.buffer(context.frame.frameIndex,
                    self.ids_.indirect).buffer);
    }

    void VulkanClusterLightingFeature::executeReadback(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanClusterLightingFeature*>(owner);
        const uint32_t frameIndex = context.frame.frameIndex;
        const VulkanBufferResource& diagnostics = context.graph.buffer(
            frameIndex, self.ids_.diagnostics);
        const VkBufferCopy copy{ 0, 0, 64 };
        vkCmdCopyBuffer(context.commandBuffer, diagnostics.buffer,
            self.diagnosticReadbackBuffers_[frameIndex].buffer,
            1, &copy);
        self.diagnosticReadbackPending_[frameIndex] = true;
    }

    void VulkanClusterLightingFeature::uploadLights(uint32_t frameIndex,
        const LightingFramePacket& lights) {
        CpuScope uploadScope(context_->profiler, "cpu.light.upload");
        if (frameIndex >= lightRecordBuffers_.size() ||
            lights.records.size() > lightRecordCapacity_ ||
            lights.recordRevisions.size() < lights.records.size() ||
            lights.selectionMetadata.size() < lights.records.size() ||
            lights.activeSlots.size() > lightRecordCapacity_) {
            throw std::out_of_range("GPU light packet is outside prepared capacity");
        }
        std::vector<uint64_t>& uploaded = uploadedLightRevisions_[frameIndex];
        const bool shadowMappingChanged =
            spotShadowSlots_.uploaded[frameIndex] !=
                spotShadowSlots_.revision ||
            pointShadowSlots_.uploaded[frameIndex] !=
                pointShadowSlots_.revision;
        if (shadowMappingChanged) {
            lightUploadRanges_.clear();
            if (!lights.records.empty())
                lightUploadRanges_.push_back({ 0,
                    static_cast<uint32_t>(lights.records.size()) });
        }
        else {
            buildLightUploadRanges(lights.recordRevisions.first(
                lights.records.size()), uploaded, lightUploadRanges_);
        }
        lightUploadBytes_ = 0;
        for (const LightRecordRange range : lightUploadRanges_) {
            const std::span<const PackedGpuLight> records =
                lights.records.subspan(range.firstRecord, range.recordCount);
            patchedLightRecordsScratch_.assign(records.begin(), records.end());
            for (uint32_t index = 0; index < range.recordCount; ++index) {
                const uint32_t slot = range.firstRecord + index;
                const uint32_t type = std::bit_cast<uint32_t>(
                    patchedLightRecordsScratch_[index].shapeMetadata.z) & 3u;
                uint32_t shadowDataSlot = kInvalidShadowDataSlot;
                if (type == static_cast<uint32_t>(
                        PackedGpuLightType::Spot) &&
                    slot < spotShadowSlots_.slots.size())
                    shadowDataSlot = spotShadowSlots_.slots[slot];
                else if (type == static_cast<uint32_t>(
                        PackedGpuLightType::Point) &&
                    slot < pointShadowSlots_.slots.size())
                    shadowDataSlot = pointShadowSlots_.slots[slot];
                patchedLightRecordsScratch_[index].shapeMetadata.w =
                    std::bit_cast<float>(shadowDataSlot);
            }
            context_->allocator.write(lightRecordBuffers_[frameIndex],
                static_cast<VkDeviceSize>(range.firstRecord) *
                    sizeof(PackedGpuLight),
                std::as_bytes(std::span(patchedLightRecordsScratch_)));
            for (uint32_t slot = range.firstRecord;
                slot < range.firstRecord + range.recordCount; ++slot) {
                uploaded[slot] = lights.recordRevisions[slot];
            }
            lightUploadBytes_ += static_cast<uint64_t>(range.recordCount) *
                sizeof(PackedGpuLight);
        }
        spotShadowSlots_.uploaded[frameIndex] =
            spotShadowSlots_.revision;
        pointShadowSlots_.uploaded[frameIndex] =
            pointShadowSlots_.revision;
        lightUploadRangeCount_ = static_cast<uint32_t>(lightUploadRanges_.size());

        if (uploadedActiveListRevisions_[frameIndex] !=
            lights.activeListRevision) {
            if (!lights.activeSlots.empty()) {
                context_->allocator.write(activeLightSlotBuffers_[frameIndex], 0,
                    std::as_bytes(lights.activeSlots));
            }
            uploadedActiveListRevisions_[frameIndex] =
                lights.activeListRevision;
            lightUploadBytes_ += static_cast<uint64_t>(lights.activeSlots.size()) *
                sizeof(uint32_t);
            ++lightUploadRangeCount_;
        }
        activeLightCount_ = lights.stats.activeLightCount;
    }

    void VulkanClusterLightingFeature::updateParameters(uint32_t frameIndex,
        const glm::mat4& view, const glm::mat4& projection,
        float nearPlane, float farPlane, uint32_t activeLightCount,
        VkExtent2D sceneExtent,
        const EnvironmentLightingSettings& environment) {
        if (frameIndex >= parameterBuffers_.size() ||
            !(nearPlane > 0.0f) || !(farPlane > nearPlane)) {
            throw std::invalid_argument("Invalid clustered-lighting frame parameters");
        }
        const ClusterFrameParameters frame{
            sceneExtent.width, sceneExtent.height, nearPlane, farPlane,
            view, projection };
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            config_, frame);
        const uint64_t clusterCount = dimensions.clusterCount();
        if (clusterCount > (std::numeric_limits<uint32_t>::max)()) {
            throw std::overflow_error("Cluster grid exceeds the GPU index domain");
        }
        PackedGpuClusterParameters parameters{};
        parameters.view = view;
        parameters.projection = projection;
        parameters.grid = { sceneExtent.width, sceneExtent.height,
            dimensions.tilesX, dimensions.tilesY };
        parameters.depth = { nearPlane, farPlane,
            static_cast<float>(config_.depthSlices) /
                std::log(farPlane / nearPlane), 0.0f };
        parameters.limits = { config_.depthSlices,
            config_.maximumLightsPerCluster,
            config_.maximumLightReferences,
            config_.maximumDirectionalLights };
        parameters.input = { activeLightCount,
            config_.maximumFallbackLights,
            config_.tileWidth, config_.tileHeight };
        const uint32_t environmentFlags =
            (environment.visibleToCamera ? 1u : 0u) |
            (environment.affectsLighting ? 2u : 0u);
        parameters.environment = {
            environment.lightingIntensity,
            environment.backgroundIntensity,
            environment.rotationRadians,
            static_cast<float>(environmentFlags),
        };
        context_->allocator.write(parameterBuffers_[frameIndex], 0,
            std::as_bytes(std::span(&parameters, size_t{ 1 })));
        lightUploadBytes_ += sizeof(parameters);
        ++lightUploadRangeCount_;
    }

    void VulkanClusterLightingFeature::updateFallbackCandidates(
        uint32_t frameIndex, const glm::mat4& view,
        const LightingFramePacket& lights) {
        CpuScope fallbackScope(context_->profiler, "cpu.light.cluster_fallback");
        if (frameIndex >= fallbackCandidateBuffers_.size() ||
            lights.selectionMetadata.size() < lights.records.size()) {
            throw std::out_of_range(
                "Cluster fallback inputs are outside the prepared frame");
        }
        selectClusterFallbackLights(lights, view,
            kMaximumClusterFallbackLights, fallbackSelectionScratch_);
        const size_t selectedCount = fallbackSelectionScratch_.size();
        std::array<uint32_t, kMaximumClusterFallbackLights> selected{};
        selected.fill(UINT32_MAX);
        std::copy_n(fallbackSelectionScratch_.begin(), selectedCount,
            selected.begin());
        context_->allocator.write(fallbackCandidateBuffers_[frameIndex], 0,
            std::as_bytes(std::span(selected)));
        lightUploadBytes_ += sizeof(selected);
        ++lightUploadRangeCount_;
    }

    void VulkanClusterLightingFeature::collectDiagnostics(
        uint32_t frameIndex) noexcept {
        if (frameIndex >= diagnosticReadbackBuffers_.size() ||
            !diagnosticReadbackPending_[frameIndex] ||
            diagnosticReadbackBuffers_[frameIndex].mapped == nullptr) {
            return;
        }
        std::array<uint32_t, 16> values{};
        std::memcpy(values.data(),
            diagnosticReadbackBuffers_[frameIndex].mapped,
            sizeof(values));
        const uint64_t clusterCount = submittedClusterCounts_[frameIndex];
        const uint64_t bufferBytesPerFrame =
            static_cast<uint64_t>(config_.maximumDirectionalLights) * 4u +
            clusterCount * sizeof(ClusterLightHeader) +
            static_cast<uint64_t>(config_.maximumLightReferences) * 4u +
            static_cast<uint64_t>(config_.maximumFallbackLights) * 4u +
            64u + clusterCount * 4u + clusterCount * 4u +
            clusterScanScratchElementCount(clusterCount) * 4u + 32u;
        clusterTelemetry_ = {
            .bufferBytesPerFrame = bufferBytesPerFrame,
            .clusterCount = submittedClusterCounts_[frameIndex],
            .activeLights = values[0],
            .directionalLights = values[1],
            .localLights = values[2],
            .clustersUsed = values[6],
            .maximumOccupancy = values[7],
            .requestedReferences = values[4],
            .publishedReferences = values[5],
            .fallbackLights = values[8],
            .droppedLights = values[9],
            .overflowCode = values[3],
            .available = true,
        };
        diagnosticReadbackPending_[frameIndex] = false;
    }

} // namespace Iridium
