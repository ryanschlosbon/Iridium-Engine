#include "VulkanOpaqueIndirectCuller.h"

#include "VulkanDepthPyramid.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Iridium {

    void VulkanOpaqueCullPipelines::destroy(VkDevice device) noexcept {
        if (device != VK_NULL_HANDLE) {
            if (cull != VK_NULL_HANDLE) vkDestroyPipeline(device, cull, nullptr);
            if (fallback != VK_NULL_HANDLE)
                vkDestroyPipeline(device, fallback, nullptr);
            if (layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, layout, nullptr);
            if (setLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        }
        *this = {};
    }

    VulkanOpaqueCullPipelines createOpaqueCullPipelines(VkDevice device,
        VkPipelineCache pipelineCache, bool depthOcclusionRejection,
        VkDescriptorSetLayout globalLayout,
        VkDescriptorSetLayout gpuSceneLayout) {
        VulkanOpaqueCullPipelines result{};
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (uint32_t binding = 0; binding < 5u; ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[binding].descriptorCount = 1u;
            bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        bindings[5].binding = 5u;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[5].descriptorCount = 1u;
        bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = depthOcclusionRejection
            ? static_cast<uint32_t>(bindings.size()) : 4u;
        setInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr,
                &result.setLayout) != VK_SUCCESS)
            throw std::runtime_error(
                "failed to create GPU-scene cull descriptor layout");
        try {
            const std::array<VkDescriptorSetLayout, 3> setLayouts{
                globalLayout, gpuSceneLayout, result.setLayout };
            result.layout = createComputePipelineLayout(device, setLayouts, 11u,
                "GPU-scene cull");
            constexpr const char* baseShader =
                "assets/shaders/gpu_scene_frustum_compact_comp.spv";
            constexpr const char* fusedShader =
                "assets/shaders/gpu_scene_frustum_occlusion_compact_comp.spv";
            result.cull = createComputePipeline(device, pipelineCache,
                result.layout, depthOcclusionRejection ? fusedShader : baseShader,
                "GPU-scene cull");
            if (depthOcclusionRejection)
                result.fallback = createComputePipeline(device, pipelineCache,
                    result.layout, baseShader, "GPU-scene cull");
        }
        catch (...) {
            result.destroy(device);
            throw;
        }
        return result;
    }

    void VulkanOpaqueIndirectCuller::init(const VulkanCullerServices& services,
        const VulkanOpaqueCullerConfig& config,
        const VulkanOpaqueCullPipelines& pipelines) {
        services_ = services;
        config_ = config;
        pipelines_ = pipelines;
    }

    void VulkanOpaqueIndirectCuller::allocateSet(uint32_t frame) {
        sets_[frame] = services_.resources.allocateSet(services_.resources.user,
            pipelines_.setLayout);
    }

    void VulkanOpaqueIndirectCuller::bindBuffers() {
        for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight; ++frame)
            bindSlot(frame);
    }

    void VulkanOpaqueIndirectCuller::bindSlot(uint32_t frame) {
        const VulkanCullerResources& resources = services_.resources;
        const std::array<VkDescriptorBufferInfo, 4> infos{ {
            { buffers_.candidates[frame].buffer, 0,
                buffers_.candidates[frame].size },
            { buffers_.commands[frame].buffer, 0,
                buffers_.commands[frame].size },
            { buffers_.counts[frame].buffer, 0, buffers_.counts[frame].size },
            { lodHistoryBuffer_.buffer, 0, lodHistoryBuffer_.size },
        } };
        resources.writeStorageBuffers(resources.user, sets_[frame], 0u, infos);
        if (config_.depthOcclusionRejection) {
            const VkDescriptorBufferInfo occlusionInfo{
                gpuSceneOcclusionResultBuffers_[frame].buffer, 0,
                gpuSceneOcclusionResultBuffers_[frame].size };
            resources.writeStorageBuffers(resources.user, sets_[frame], 4u,
                { &occlusionInfo, 1u });
        }
    }

    void VulkanOpaqueIndirectCuller::destroySlot(uint32_t frame) noexcept {
        const VulkanCullerResources& resources = services_.resources;
        buffers_.destroySlot(resources, frame);
        for (Buffers* group : { &occlusionQueryBuffers_,
                &occlusionResultBuffers_, &gpuSceneOcclusionResultBuffers_ })
            resources.destroyBuffer(resources.user, (*group)[frame]);
    }

    void VulkanOpaqueIndirectCuller::destroy(VkDevice device) noexcept {
        if (services_.resources.destroyBuffer != nullptr) {
            const VulkanCullerResources& resources = services_.resources;
            buffers_.destroy(resources);
            pendingBuffers_.destroy(resources);
            for (Buffers* group : { &occlusionQueryBuffers_,
                    &occlusionResultBuffers_, &gpuSceneOcclusionResultBuffers_,
                    &pendingOcclusionQueryBuffers_, &pendingOcclusionResultBuffers_,
                    &pendingGpuSceneOcclusionResultBuffers_ })
                for (VulkanBufferResource& buffer : *group)
                    resources.destroyBuffer(resources.user, buffer);
            resources.destroyBuffer(resources.user, lodHistoryBuffer_);
        }
        pendingSlots_ = {};
        pipelines_.destroy(device);
        sets_ = {};
        lodHistory_ = {};
        pending_ = {};
        commandCapacity_ = 0;
    }

    void VulkanOpaqueIndirectCuller::resize(uint32_t capacity, bool frameOpen) {
        if (frameOpen || capacity == 0u ||
            capacity > services_.maximumPrimitiveCapacity) {
            throw std::invalid_argument(
                "opaque indirect capacity is invalid at this frame boundary");
        }
        const VulkanCullerResources& resources = services_.resources;
        VulkanIndirectBufferSet buffers{};
        Buffers occlusionQueries{}, occlusionResults{}, gpuSceneOcclusionResults{};
        VulkanBufferResource history{};
        const bool depthOcclusionOracle = config_.occlusionOracle != nullptr;
        const bool standaloneOcclusionOracle = config_.depthOcclusionQuery &&
            (!config_.depthOcclusionRejection || depthOcclusionOracle);
        const uint32_t historyCapacity = lodEnabled() ? capacity : 1u;
        try {
            history = resources.createBuffer(resources.user,
                static_cast<uint64_t>(historyCapacity) *
                    sizeof(GpuSceneLodHistoryRecord),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            std::memset(history.mapped, 0, static_cast<size_t>(history.size));
            for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight;
                    ++frame) {
                buffers.commands[frame] = resources.createBuffer(resources.user,
                    static_cast<uint64_t>(capacity) *
                        sizeof(GpuSceneIndexedIndirectCommand),
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                buffers.counts[frame] = resources.createBuffer(resources.user,
                    static_cast<uint64_t>(capacity) * sizeof(uint32_t),
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                buffers.candidates[frame] = resources.createBuffer(resources.user,
                    static_cast<uint64_t>(capacity) *
                        sizeof(GpuSceneIndirectCandidate),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                if (standaloneOcclusionOracle) {
                    occlusionQueries[frame] = resources.createBuffer(
                        resources.user, static_cast<uint64_t>(capacity) *
                            sizeof(DepthPyramidDeviceQuery),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                    occlusionResults[frame] = resources.createBuffer(
                        resources.user, static_cast<uint64_t>(capacity) *
                            sizeof(DepthPyramidDeviceResult),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                }
                if (config_.depthOcclusionQuery) {
                    const uint32_t resultCapacity =
                        !config_.depthOcclusionRejection || depthOcclusionOracle
                        ? capacity : 1u;
                    gpuSceneOcclusionResults[frame] = resources.createBuffer(
                        resources.user, static_cast<uint64_t>(resultCapacity) *
                            sizeof(DepthPyramidDeviceResult),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                }
            }
        }
        catch (...) {
            resources.destroyBuffer(resources.user, history);
            buffers.destroy(resources);
            for (Buffers* group : { &occlusionQueries, &occlusionResults,
                    &gpuSceneOcclusionResults })
                for (VulkanBufferResource& buffer : *group)
                    resources.destroyBuffer(resources.user, buffer);
            throw;
        }
        // R4c.2: no drain. A slot that is not in flight is collected and
        // swapped now; an in-flight slot parks its replacement (an older
        // parked set was never used) until its retirement.
        std::array<bool, kIndirectCullerFramesInFlight> swappedNow{};
        bool anyInFlight = false;
        for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight; ++frame) {
            pendingBuffers_.destroySlot(resources, frame);
            for (Buffers* group : { &pendingOcclusionQueryBuffers_,
                    &pendingOcclusionResultBuffers_,
                    &pendingGpuSceneOcclusionResultBuffers_ })
                resources.destroyBuffer(resources.user, (*group)[frame]);
            pendingSlots_[frame] = false;
            if (resources.inFlight(frame)) {
                anyInFlight = true;
                pendingBuffers_.takeSlot(buffers, frame);
                pendingOcclusionQueryBuffers_[frame] = occlusionQueries[frame];
                pendingOcclusionResultBuffers_[frame] = occlusionResults[frame];
                pendingGpuSceneOcclusionResultBuffers_[frame] =
                    gpuSceneOcclusionResults[frame];
                pendingSlots_[frame] = true;
                continue;
            }
            if (commandCapacity_ != 0u) collect(frame);
            destroySlot(frame);
            buffers_.takeSlot(buffers, frame);
            occlusionQueryBuffers_[frame] = occlusionQueries[frame];
            occlusionResultBuffers_[frame] = occlusionResults[frame];
            gpuSceneOcclusionResultBuffers_[frame] = gpuSceneOcclusionResults[frame];
            swappedNow[frame] = true;
        }
        commandCapacity_ = capacity;
        bins_.reserve(capacity);
        indirectPlan_.commands.reserve(capacity);
        indirectPlan_.packetIndices.reserve(capacity);
        candidates_.reserve(capacity);
        if (standaloneOcclusionOracle) occlusionQueries_.reserve(capacity);
        // The history is one table shared by both slots: an in-flight slot
        // may still write the old one.
        if (anyInFlight && resources.retireBuffer != nullptr)
            resources.retireBuffer(resources.user, lodHistoryBuffer_);
        else
            resources.destroyBuffer(resources.user, lodHistoryBuffer_);
        lodHistoryBuffer_ = history;
        lodHistory_.resize(historyCapacity);
        for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight; ++frame)
            if (swappedNow[frame] && sets_[frame] != VK_NULL_HANDLE) bindSlot(frame);
    }

    bool VulkanOpaqueIndirectCuller::swapRetiredSlot(uint32_t slot) {
        if (!pendingSlots_[slot]) return false;
        destroySlot(slot);
        buffers_.takeSlot(pendingBuffers_, slot);
        occlusionQueryBuffers_[slot] = pendingOcclusionQueryBuffers_[slot];
        occlusionResultBuffers_[slot] = pendingOcclusionResultBuffers_[slot];
        gpuSceneOcclusionResultBuffers_[slot] =
            pendingGpuSceneOcclusionResultBuffers_[slot];
        pendingOcclusionQueryBuffers_[slot] = {};
        pendingOcclusionResultBuffers_[slot] = {};
        pendingGpuSceneOcclusionResultBuffers_[slot] = {};
        pendingSlots_[slot] = false;
        if (sets_[slot] != VK_NULL_HANDLE) bindSlot(slot);
        return true;
    }

    bool VulkanOpaqueIndirectCuller::plan(const OpaqueIndirectInputs& inputs,
        uint32_t frame) {
        const std::span<const DrawPacket> opaqueQueue = inputs.queue;
        const GpuSceneIndirectPolicy policy{
            .multiDrawIndirect = services_.capabilities.multiDrawIndirect,
            .drawIndirectFirstInstance =
                services_.capabilities.drawIndirectFirstInstance,
            .drawIndirectCount = services_.capabilities.drawIndirectCount,
            .maxDrawIndirectCount = (std::min)(
                services_.capabilities.maxDrawIndirectCount, commandCapacity_),
            .minimumCommandCount = 8u,
            .forceDirectReference = config_.forceDirectGBufferReference,
        };
        const GpuSceneCapacityRequirements& published = inputs.scene.published;
        if (!inputs.sceneBuffersMapped ||
            published.primitives == 0 || published.geometries == 0) {
            indirectPlan_.fallbackReason = GpuSceneIndirectFallbackReason::InvalidPacket;
            return false;
        }
        const auto primitiveRecords = inputs.scene.primitives.first(published.primitives);
        const auto geometryRecords = inputs.scene.geometries.first(published.geometries);
        const auto instanceRecords = inputs.scene.instances.first(published.instances);
        const auto transformRecords = inputs.scene.transforms.first(published.transforms);
        buildGpuSceneIndirectPlan(opaqueQueue, policy, indirectPlan_,
            primitiveRecords, geometryRecords);
        bins_.clear();
        candidates_.clear();
        bool valid = indirectPlan_.usesIndirect();
        if (lodEnabled())
            seenHistory_.assign(primitiveRecords.size(), 0);
        const VulkanIndirectAssetResolver& assets = inputs.assets;
        for (uint32_t index = 0; valid && index < opaqueQueue.size(); ++index) {
            const DrawPacket& packet = opaqueQueue[index];
            if (lodEnabled() &&
                seenHistory_[packet.firstInstanceTransform]++ != 0) {
                indirectPlan_.fallbackReason = GpuSceneIndirectFallbackReason::InvalidPacket;
                return false; // Never dispatch two writers to the same history slot.
            }
            VulkanIndirectGeometry geometry{};
            VulkanIndirectMaterial material{};
            if (!assets.geometry(assets.owner, packet.geometry, geometry) ||
                !assets.material(assets.owner, packet.material, material) ||
                !assets.gbufferIndirectPipeline(assets.owner, packet.pipeline)) {
                valid = false;
                break;
            }

            const auto& primitive = primitiveRecords[packet.firstInstanceTransform];
            const auto& packedGeometry = geometryRecords[primitive.binding.y];
            if (primitive.binding.x >= instanceRecords.size() ||
                instanceRecords[primitive.binding.x].references.x >= transformRecords.size() ||
                (packedGeometry.storage.w & GpuSceneGeometryLegacyRhiHandle) == 0 ||
                packedGeometry.storage.x != packet.geometry.id ||
                packedGeometry.draw.w != static_cast<uint32_t>(geometry.indexFormat) ||
                std::bit_cast<int32_t>(packedGeometry.draw.z) < 0 ||
                static_cast<uint64_t>(packedGeometry.draw.z) * sizeof(Vertex) != geometry.vertexOffset) {
                indirectPlan_.fallbackReason = GpuSceneIndirectFallbackReason::InvalidPacket;
                valid = false;
                break;
            }

            bool startsBin = bins_.empty();
            if (!startsBin) {
                const Bin& previous = bins_.back();
                VulkanIndirectGeometry previousGeometry{};
                const bool previousResolved = assets.geometry(assets.owner,
                    previous.geometry, previousGeometry);
                startsBin = previous.pipeline != packet.pipeline ||
                    previous.material != packet.material ||
                    !previousResolved ||
                    previousGeometry.vertexBuffer != geometry.vertexBuffer ||
                    previousGeometry.indexBuffer != geometry.indexBuffer ||
                    previousGeometry.indexFormat != geometry.indexFormat;
            }
            if (startsBin) {
                bins_.push_back({
                    .packetBegin = index,
                    .commandBegin = index,
                    .commandCount = 1u,
                    .pipeline = packet.pipeline,
                    .material = packet.material,
                    .geometry = packet.geometry,
                });
            }
            else {
                ++bins_.back().commandCount;
            }
        }
        if (!valid) return false;

        candidates_.resize(opaqueQueue.size());
        for (uint32_t binIndex = 0; binIndex < bins_.size(); ++binIndex) {
            const Bin& bin = bins_[binIndex];
            for (uint32_t command = 0; command < bin.commandCount; ++command) {
                const uint32_t packetIndex = bin.packetBegin + command;
                candidates_[packetIndex] = {
                    .primitiveIndex =
                        opaqueQueue[packetIndex].firstInstanceTransform,
                    .binIndex = binIndex,
                    .commandBase = bin.commandBegin,
                    .commandCapacity = bin.commandCount,
                };
                auto& candidate = candidates_[packetIndex];
                if (lodEnabled()) {
                    const auto history = lodHistory_.binding(candidate.primitiveIndex);
                    candidate.historySlot = history.slot;
                    candidate.historyTokenLow = history.tokenLow;
                    candidate.historyTokenHigh = history.tokenHigh;
                    VulkanIndirectGeometry base{};
                    (void)assets.geometry(assets.owner,
                        opaqueQueue[packetIndex].geometry, base);
                    candidate.maximumLod = residentLodPrefix(geometryRecords,
                        instanceRecords, primitiveRecords[candidate.primitiveIndex],
                        config_.lodMaximumLevel, base, assets);
                }
            }
        }

        PendingValidation& validation = pending_[frame];
        CpuProfiler* const profiler = services_.profiler;
        validation.profileFrameId = profiler != nullptr && profiler->isFrameOpen()
            ? profiler->currentFrameId() : 0u;
        validation.expectedBinCounts.assign(bins_.size(), 0u);
        validation.binCapacities.resize(bins_.size());
        validation.occlusionProfileFrameId = validation.profileFrameId;
        validation.gpuSceneOcclusionCandidateCount = 0u;
        validation.occlusionCandidatePrimitiveIndices.clear();
        validation.occlusionCandidateBinIndices.clear();
        validation.gpuSceneOcclusionPending = false;
        validation.occlusionRejectionApplied = false;
        const ViewTransportRecord& viewUniform = *inputs.view;
        occlusionQueries_.clear();
        std::array<float, 16> worldToClip{};
        const bool queryOcclusion = inputs.queryOcclusion;
        // Qualification-only expectation emission (R2.8): both oracles are
        // null unless explicitly enabled in a qualification build.
        IVulkanIndirectOracle* const lodOracle =
            lodEnabled() ? config_.lodOracle : nullptr;
        IVulkanIndirectOracle* const occlusionOracle =
            queryOcclusion ? config_.occlusionOracle : nullptr;
        validation.lodQualificationOracle = lodOracle != nullptr;
        validation.occlusionQualificationOracle = occlusionOracle != nullptr;
        if (lodOracle != nullptr || occlusionOracle != nullptr)
            services_.oracle->beginOpaqueWork(frame,
                static_cast<uint32_t>(primitiveRecords.size()),
                static_cast<uint32_t>(candidates_.size()),
                lodOracle != nullptr, occlusionOracle != nullptr);
        if (queryOcclusion) {
            validation.occlusionCandidatePrimitiveIndices.reserve(candidates_.size());
            validation.occlusionCandidateBinIndices.reserve(candidates_.size());
            for (const auto& candidate : candidates_) {
                validation.occlusionCandidatePrimitiveIndices.push_back(
                    candidate.primitiveIndex);
                validation.occlusionCandidateBinIndices.push_back(
                    candidate.binIndex);
            }
        }
        if (occlusionOracle != nullptr) {
            const glm::mat4 clipFromWorld =
                viewUniform.projection * viewUniform.view;
            for (uint32_t row = 0; row < 4u; ++row)
                for (uint32_t column = 0; column < 4u; ++column)
                    worldToClip[row * 4u + column] = clipFromWorld[column][row];
        }
        const DepthPyramidExtent queryExtent{
            static_cast<uint32_t>(viewUniform.renderInfo.x),
            static_cast<uint32_t>(viewUniform.renderInfo.y) };
        for (uint32_t binIndex = 0; binIndex < bins_.size(); ++binIndex) {
            const Bin& bin = bins_[binIndex];
            validation.binCapacities[binIndex] = bin.commandCount;
            for (uint32_t command = 0; command < bin.commandCount; ++command) {
                const uint32_t packetIndex = bin.packetBegin + command;
                if (!cpuVisibilityOracleVisible(opaqueQueue[packetIndex])) continue;
                if (occlusionOracle != nullptr)
                    occlusionOracle->expectOpaqueVisibleCandidate(frame, packetIndex);
                ++validation.expectedBinCounts[binIndex];
                if (occlusionOracle != nullptr) {
                    const auto& candidate = candidates_[packetIndex];
                    const auto& primitive = primitiveRecords[candidate.primitiveIndex];
                    const auto& geometry = geometryRecords[primitive.binding.y];
                    const auto& instance = instanceRecords[primitive.binding.x];
                    const glm::mat4 world = unpackGpuSceneAffine(
                        transformRecords[instance.references.x]);
                    const glm::vec3 localMinimum{ geometry.localBoundsMin.x,
                        geometry.localBoundsMin.y, geometry.localBoundsMin.z };
                    const glm::vec3 localMaximum{ geometry.localBoundsMax.x,
                        geometry.localBoundsMax.y, geometry.localBoundsMax.z };
                    glm::vec3 worldMinimum{ (std::numeric_limits<float>::max)() };
                    glm::vec3 worldMaximum{ (std::numeric_limits<float>::lowest)() };
                    for (uint32_t corner = 0; corner < 8u; ++corner) {
                        const glm::vec3 local{
                            (corner & 1u) != 0u ? localMaximum.x : localMinimum.x,
                            (corner & 2u) != 0u ? localMaximum.y : localMinimum.y,
                            (corner & 4u) != 0u ? localMaximum.z : localMinimum.z };
                        const glm::vec3 position = glm::vec3(
                            world * glm::vec4(local, 1.0f));
                        worldMinimum = glm::min(worldMinimum, position);
                        worldMaximum = glm::max(worldMaximum, position);
                    }
                    const auto projection = projectDepthPyramidBounds({
                        .extent = queryExtent,
                        .convention = DeviceDepthConvention::ForwardZeroToOne,
                        .worldToClip = worldToClip,
                        .minimumWorld = { worldMinimum.x, worldMinimum.y,
                            worldMinimum.z },
                        .maximumWorld = { worldMaximum.x, worldMaximum.y,
                            worldMaximum.z },
                        .guardPixels = 1.0f,
                        .minimumFootprintPixels = 1.0f,
                        .depthBias = 0.00001f,
                    });
                    if (projection.eligible) {
                        occlusionOracle->expectOpaqueOcclusionQuery(frame,
                            packetIndex);
                        occlusionQueries_.push_back(
                            packDepthPyramidDeviceQuery(projection.query));
                    }
                    else
                        occlusionOracle->expectOpaqueProjectionRejected(frame);
                }
                if (lodOracle != nullptr) {
                    const auto& candidate = candidates_[packetIndex];
                    const auto& primitive = primitiveRecords[candidate.primitiveIndex];
                    const auto& instance = instanceRecords[primitive.binding.x];
                    const glm::mat4 clipFromLocal = viewUniform.projection *
                        viewUniform.view *
                        unpackGpuSceneAffine(transformRecords[instance.references.x]);
                    const GpuSceneLodHistoryBinding history{ candidate.historySlot,
                        candidate.historyTokenLow, candidate.historyTokenHigh };
                    const uint32_t previous = lodHistory_.previous(history);
                    const uint32_t selected = selectGpuSceneLodGeometry(
                        geometryRecords, primitive.binding.y, clipFromLocal,
                        glm::vec2(viewUniform.renderInfo), config_.lodErrorPixels,
                        candidate.maximumLod, previous,
                        config_.lodHysteresisFraction);
                    const auto& geometry = geometryRecords[selected];
                    const uint32_t selectedLod = selected == primitive.binding.y ? 0u :
                        static_cast<uint32_t>(geometry.localBoundsMax.w);
                    lodHistory_.record(history, selectedLod);
                    lodOracle->expectOpaqueLod(frame, candidate.primitiveIndex, {
                        .command = { geometry.draw.y, 1u, geometry.draw.x,
                            std::bit_cast<int32_t>(geometry.draw.z),
                            candidate.primitiveIndex },
                        .baseTriangles =
                            geometryRecords[primitive.binding.y].draw.y / 3u,
                        .oracleTriangles = geometry.draw.y / 3u,
                        .reduced = selected != primitive.binding.y,
                        .historyValid = previous != InvalidGpuSceneIndex,
                        .historyChanged = previous != InvalidGpuSceneIndex &&
                            previous != selectedLod,
                    });
                }
            }
        }
        validation.pending = true;
        const VulkanIndirectStreamTap tap = stream(frame);
        tap.begin(buffers_.commands[frame].buffer, buffers_.counts[frame].buffer);
        std::memcpy(buffers_.candidates[frame].mapped, candidates_.data(),
            candidates_.size() * sizeof(GpuSceneIndirectCandidate));
        std::memset(buffers_.counts[frame].mapped, 0,
            bins_.size() * sizeof(uint32_t));
        tap.hostWrite(VulkanIndirectStreamHostTarget::Candidates,
            buffers_.candidates[frame].mapped,
            candidates_.size() * sizeof(GpuSceneIndirectCandidate));
        tap.hostWrite(VulkanIndirectStreamHostTarget::Counts,
            buffers_.counts[frame].mapped, bins_.size() * sizeof(uint32_t));
        if (!occlusionQueries_.empty()) {
            std::memcpy(occlusionQueryBuffers_[frame].mapped,
                occlusionQueries_.data(),
                occlusionQueries_.size() * sizeof(DepthPyramidDeviceQuery));
            tap.hostWrite(VulkanIndirectStreamHostTarget::OcclusionQueries,
                occlusionQueryBuffers_[frame].mapped,
                occlusionQueries_.size() * sizeof(DepthPyramidDeviceQuery));
        }
        if (queryOcclusion && !candidates_.empty()) {
            validation.gpuSceneOcclusionCandidateCount =
                static_cast<uint32_t>(candidates_.size());
            validation.occlusionRejectionApplied = config_.depthOcclusionRejection;
            // Query-only runs and explicit qualification consume the result
            // stream on the CPU. The deployable rejection route deliberately
            // avoids reading every candidate result back from host-visible GPU
            // memory; its compacted indirect counts are the consumed output.
            validation.gpuSceneOcclusionPending =
                !validation.occlusionRejectionApplied ||
                validation.occlusionQualificationOracle;
        }
        published_ = published;
        return true;
    }

    uint32_t VulkanOpaqueIndirectCuller::recordCompaction(VkCommandBuffer cmd,
        uint32_t frame, const OpaqueCompactionContext& context) {
        const VulkanCullerCommands& commands = services_.commands;
        const VulkanIndirectStreamTap tap = stream(frame);
        PendingValidation& validation = pending_[frame];
        uint32_t dispatches = 0u;

        // Same queue, across submissions: the preceding view's history writes
        // must be visible before this dispatch reads/updates the shared table.
        const VkAccessFlags hostSrc = VK_ACCESS_HOST_WRITE_BIT |
            (lodEnabled() ? VK_ACCESS_SHADER_WRITE_BIT : 0u);
        const VkAccessFlags hostDst =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        const VkPipelineStageFlags hostStages = VK_PIPELINE_STAGE_HOST_BIT |
            (lodEnabled() ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0u);
        commands.memoryBarrier(commands.user, cmd, hostStages,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, hostSrc, hostDst);
        tap.barrier(hostStages, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, hostSrc,
            hostDst);

        VulkanGpuRangeToken range{};
        const auto recordGpuSceneOcclusion = [&]() {
            if (!validation.gpuSceneOcclusionPending) return;
            range = commands.beginGpuRange(commands.user,
                "gpu.depth.occlusion-gpu-scene-query");
            tap.gpuRange("gpu.depth.occlusion-gpu-scene-query");
            const uint32_t queryDispatches =
                config_.depthPyramid->recordGpuSceneQueries(cmd, frame,
                    context.retainedView, context.globalSet, context.gpuSceneSet,
                    buffers_.candidates[frame].buffer,
                    buffers_.candidates[frame].size,
                    gpuSceneOcclusionResultBuffers_[frame].buffer,
                    gpuSceneOcclusionResultBuffers_[frame].size,
                    validation.gpuSceneOcclusionCandidateCount,
                    published_.transforms, published_.instances,
                    published_.primitives, published_.geometries);
            dispatches += queryDispatches;
            tap.note(1u, { context.retainedView,
                validation.gpuSceneOcclusionCandidateCount,
                published_.transforms, published_.instances,
                published_.primitives, published_.geometries, queryDispatches });
            commands.endGpuRange(commands.user, range);
        };
        if (validation.occlusionRejectionApplied) {
            const VkImageView historyView =
                config_.depthPyramid->historyImageView(context.retainedView);
            const VkSampler historySampler = config_.depthPyramid->historySampler();
            if (historyView == VK_NULL_HANDLE || historySampler == VK_NULL_HANDLE) {
                throw std::logic_error(
                    "fused depth-occlusion history descriptor is unavailable");
            }
            services_.resources.writeCombinedImageSampler(services_.resources.user,
                sets_[frame], 5u, historySampler, historyView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            tap.note(3u, { context.retainedView });
        }

        range = commands.beginGpuRange(commands.user, "gpu.gpu_scene.frustum_compact");
        tap.gpuRange("gpu.gpu_scene.frustum_compact");
        const VkPipeline compactPipeline =
            config_.depthOcclusionRejection && !validation.occlusionRejectionApplied
            ? pipelines_.fallback : pipelines_.cull;
        if (compactPipeline == VK_NULL_HANDLE)
            throw std::logic_error("GPU-scene compact pipeline is unavailable");
        commands.bindPipeline(commands.user, cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            compactPipeline);
        tap.bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, compactPipeline);
        const std::array<VkDescriptorSet, 3> sets{
            context.globalSet, context.gpuSceneSet, sets_[frame] };
        commands.bindDescriptorSets(commands.user, cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.layout, 0u,
            static_cast<uint32_t>(sets.size()), sets.data(), 0u, nullptr);
        tap.bindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.layout,
            0u, sets);
        const std::array<uint32_t, 11> parameters{
            static_cast<uint32_t>(candidates_.size()),
            published_.transforms,
            published_.instances,
            published_.primitives,
            published_.geometries,
            std::bit_cast<uint32_t>(config_.lodErrorPixels),
            lodHistory_.capacity(), lodHistory_.frameSerial(),
            std::bit_cast<uint32_t>(config_.lodHysteresisFraction),
            validation.occlusionRejectionApplied ? 1u : 0u,
            validation.gpuSceneOcclusionPending ? 1u : 0u };
        commands.pushConstants(commands.user, cmd, pipelines_.layout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(parameters), parameters.data());
        tap.pushConstants(pipelines_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
            parameters.data(), sizeof(parameters));
        const uint32_t groups = (parameters[0] + 63u) / 64u;
        commands.dispatch(commands.user, cmd, groups, 1u, 1u);
        tap.dispatch(groups, 1u, 1u);
        commands.endGpuRange(commands.user, range);
        ++dispatches;

        const uint32_t occlusionQueryCount =
            static_cast<uint32_t>(occlusionQueries_.size());
        if (occlusionQueryCount != 0u) {
            range = commands.beginGpuRange(commands.user, "gpu.depth.occlusion-query");
            tap.gpuRange("gpu.depth.occlusion-query");
            const uint32_t queryDispatches = config_.depthPyramid->recordQueries(
                cmd, frame, context.retainedView,
                occlusionQueryBuffers_[frame].buffer,
                occlusionQueryBuffers_[frame].size,
                occlusionResultBuffers_[frame].buffer,
                occlusionResultBuffers_[frame].size, occlusionQueryCount);
            dispatches += queryDispatches;
            tap.note(2u, { context.retainedView, occlusionQueryCount,
                queryDispatches });
            commands.endGpuRange(commands.user, range);
        }
        if (!validation.occlusionRejectionApplied)
            recordGpuSceneOcclusion();

        // M7R R3b.7: the compute -> indirect half of this dependency is the
        // executor's buffer barrier at gbuffer (IndirectRead of the command
        // and count buffers); the compute -> host half (validation readback
        // of counts, commands and occlusion results) stays here. The digest
        // records the combined dependency, so stream identity is unchanged.
        constexpr VkAccessFlags drawSrc = VK_ACCESS_SHADER_WRITE_BIT;
        constexpr VkAccessFlags drawDst =
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
        constexpr VkPipelineStageFlags drawStages =
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        commands.memoryBarrier(commands.user, cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            drawSrc, VK_ACCESS_HOST_READ_BIT);
        tap.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, drawStages, drawSrc,
            drawDst);
        return dispatches;
    }

    void VulkanOpaqueIndirectCuller::collect(uint32_t frameIndex) {
        PendingValidation& validation = pending_[frameIndex];
        if (!validation.pending) return;

        const auto* counts = static_cast<const uint32_t*>(
            buffers_.counts[frameIndex].mapped);
        const auto* commands = static_cast<const GpuSceneIndexedIndirectCommand*>(
            buffers_.commands[frameIndex].mapped);
        const VulkanIndirectScene scene =
            services_.scene(services_.sceneOwner, frameIndex);
        stream(frameIndex).retire({ .counts = counts, .commands = commands,
            .countCapacities = validation.binCapacities,
            .primitives = scene.primitives,
            .instances = scene.instances,
            .transforms = scene.transforms });
        // Oracle state exists only for slots that emitted expectations.
        IVulkanIndirectOracle* const lodOracle =
            validation.lodQualificationOracle ? services_.oracle : nullptr;
        IVulkanIndirectOracle* const occlusionOracle =
            validation.occlusionQualificationOracle ? services_.oracle : nullptr;
        uint64_t deviceCommands = 0;
        uint64_t oracleCommands = 0;
        uint64_t mismatchedBins = 0;
        uint64_t overflowCommands = 0;
        uint64_t gpuSceneOcclusionTested = 0;
        uint64_t gpuSceneOcclusionWouldReject = 0;
        uint64_t gpuSceneOcclusionFailVisible = 0;
        std::array<uint64_t, 8> gpuSceneOcclusionFailVisibleReasons{};
        uint64_t gpuSceneOcclusionAppliedRejects = 0;
        uint64_t invalidGpuSceneOcclusionResults = 0;
        uint64_t unsafeGpuSceneOcclusionMismatches = 0;
        for (size_t bin = 0; bin < validation.expectedBinCounts.size(); ++bin) {
            const uint32_t capacity = validation.binCapacities[bin];
            const uint32_t deviceCount = counts != nullptr ? counts[bin] : 0u;
            const uint32_t submittedCount = (std::min)(deviceCount, capacity);
            deviceCommands += submittedCount;
            oracleCommands += validation.expectedBinCounts[bin];
            overflowCommands += deviceCount > capacity
                ? static_cast<uint64_t>(deviceCount - capacity) : 0u;
            if (!validation.occlusionRejectionApplied ||
                validation.occlusionQualificationOracle) {
                mismatchedBins += submittedCount !=
                    validation.expectedBinCounts[bin] ? 1u : 0u;
            }
        }
        if (lodOracle != nullptr) {
            lodOracle->verifyOpaqueLodCommands(frameIndex, counts,
                validation.binCapacities, commands);
        }
        const VulkanOcclusionQueryVerdict occlusionQueries =
            occlusionOracle != nullptr
            ? occlusionOracle->verifyOcclusionQueries(frameIndex,
                static_cast<const DepthPyramidDeviceResult*>(
                    occlusionResultBuffers_[frameIndex].mapped))
            : VulkanOcclusionQueryVerdict{};
        if (validation.gpuSceneOcclusionPending) {
            const auto* results = static_cast<const DepthPyramidDeviceResult*>(
                gpuSceneOcclusionResultBuffers_[frameIndex].mapped);
            const uint32_t candidateCount =
                validation.gpuSceneOcclusionCandidateCount;
            if (results == nullptr ||
                validation.occlusionCandidatePrimitiveIndices.size() !=
                    candidateCount ||
                validation.occlusionCandidateBinIndices.size() !=
                    candidateCount) {
                invalidGpuSceneOcclusionResults = candidateCount;
            }
            else {
                for (uint32_t index = 0; index < candidateCount; ++index) {
                    const auto& result = results[index];
                    const bool tested = result.tested == 1u;
                    const bool validRejection = result.reserved0 <=
                        static_cast<uint32_t>(
                            DepthPyramidProjectionRejection::SmallBounds);
                    const bool validCommon =
                        result.abiVersion == DepthPyramidAbiVersion &&
                        result.mipLevel < 32u && result.tested <= 1u &&
                        result.occluded <= 1u &&
                        validRejection &&
                        result.reserved1 ==
                            validation.occlusionCandidatePrimitiveIndices[index] &&
                        std::isfinite(result.farthestOccluderDepth) &&
                        result.farthestOccluderDepth >= 0.0f &&
                        result.farthestOccluderDepth <= 1.0f;
                    const bool validTested = tested &&
                        result.sampledTexels >= 1u &&
                        result.sampledTexels <= 4u &&
                        result.reserved0 == static_cast<uint32_t>(
                            DepthPyramidProjectionRejection::None);
                    const bool validFailVisible = !tested &&
                        result.mipLevel == 0u &&
                        result.sampledTexels == 0u && result.occluded == 0u &&
                        result.reserved0 != static_cast<uint32_t>(
                            DepthPyramidProjectionRejection::None);
                    if (!validCommon || (!validTested && !validFailVisible)) {
                        ++invalidGpuSceneOcclusionResults;
                        continue;
                    }
                    if (!tested) {
                        ++gpuSceneOcclusionFailVisible;
                        ++gpuSceneOcclusionFailVisibleReasons[result.reserved0];
                        continue;
                    }
                    ++gpuSceneOcclusionTested;
                    gpuSceneOcclusionWouldReject += result.occluded;
                    if (result.occluded != 0u && occlusionOracle != nullptr &&
                        occlusionOracle->unsafeGpuSceneOcclusion(frameIndex,
                            index)) {
                        ++unsafeGpuSceneOcclusionMismatches;
                    }
                    if (validation.occlusionRejectionApplied &&
                        result.occluded != 0u &&
                        (occlusionOracle == nullptr ||
                            occlusionOracle->opaqueCandidateCpuVisible(
                                frameIndex, index))) {
                        const uint32_t bin =
                            validation.occlusionCandidateBinIndices[index];
                        if (bin >= validation.expectedBinCounts.size() ||
                            validation.expectedBinCounts[bin] == 0u) {
                            ++invalidGpuSceneOcclusionResults;
                            continue;
                        }
                        --validation.expectedBinCounts[bin];
                        ++gpuSceneOcclusionAppliedRejects;
                        if (lodOracle != nullptr &&
                            !lodOracle->rejectOpaqueLodPrimitive(frameIndex,
                                validation.occlusionCandidatePrimitiveIndices[
                                    index])) {
                            ++invalidGpuSceneOcclusionResults;
                        }
                    }
                }
            }
        }
        if (validation.occlusionRejectionApplied &&
            validation.occlusionQualificationOracle) {
            oracleCommands = 0u;
            mismatchedBins = 0u;
            for (size_t bin = 0; bin < validation.expectedBinCounts.size(); ++bin) {
                const uint32_t expected = validation.expectedBinCounts[bin];
                const uint32_t deviceCount = counts != nullptr ? counts[bin] : 0u;
                oracleCommands += expected;
                mismatchedBins += (std::min)(deviceCount,
                    validation.binCapacities[bin]) != expected ? 1u : 0u;
            }
        }
        const VulkanOpaqueLodVerdict lod = lodOracle != nullptr
            ? lodOracle->finishOpaqueLod(frameIndex) : VulkanOpaqueLodVerdict{};
        CpuProfiler* const profiler = services_.profiler;
        if (validation.profileFrameId != 0u && profiler != nullptr) {
            const ProfileCounterStatus fusedStatus =
                validation.occlusionRejectionApplied &&
                    !validation.occlusionQualificationOracle
                ? ProfileCounterStatus::Unavailable
                : ProfileCounterStatus::Exact;
            (void)profiler->attachCounter(validation.profileFrameId,
                "gpu_scene.visibility.device_commands", deviceCommands);
            (void)profiler->attachCounter(validation.profileFrameId,
                "gpu_scene.visibility.oracle_commands", oracleCommands,
                fusedStatus);
            (void)profiler->attachCounter(validation.profileFrameId,
                "gpu_scene.visibility.device_mismatched_bins", mismatchedBins,
                fusedStatus);
            (void)profiler->attachCounter(validation.profileFrameId,
                "gpu_scene.visibility.device_overflow_commands", overflowCommands);
            if (lod.active) {
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.base_triangles", lod.baseTriangles);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.oracle_triangles", lod.oracleTriangles);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.device_triangles", lod.deviceTriangles);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.oracle_reduced_commands", lod.oracleReducedCommands);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.device_mismatched_commands", lod.mismatchedCommands);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.history_valid", lod.historyValid);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.history_reset", lod.historyReset);
                (void)profiler->attachCounter(validation.profileFrameId,
                    "gpu_scene.lod.history_changed", lod.historyChanged);
            }
        }
        if (validation.occlusionProfileFrameId != 0u &&
            profiler != nullptr && occlusionQueries.active) {
            const uint64_t requested = occlusionQueries.queryCount +
                occlusionQueries.projectionRejected;
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.query.requested", requested);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.query.projected", occlusionQueries.queryCount);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.query.tested", occlusionQueries.tested);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.query.would_reject", occlusionQueries.wouldReject);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.query.invalid_results",
                occlusionQueries.invalidResults);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.query.projection_fail_visible",
                occlusionQueries.projectionRejected);
        }
        if (validation.occlusionProfileFrameId != 0u &&
            profiler != nullptr && validation.gpuSceneOcclusionPending) {
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.requested",
                validation.gpuSceneOcclusionCandidateCount);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.tested", gpuSceneOcclusionTested);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.would_reject",
                gpuSceneOcclusionWouldReject);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.fail_visible",
                gpuSceneOcclusionFailVisible);
            constexpr std::array<const char*, 8> failVisibleReasonNames{
                "", "depth.occlusion.gpu_scene.fail_visible.invalid_extent",
                "depth.occlusion.gpu_scene.fail_visible.invalid_bounds",
                "depth.occlusion.gpu_scene.fail_visible.invalid_projection",
                "depth.occlusion.gpu_scene.fail_visible.invalid_settings",
                "depth.occlusion.gpu_scene.fail_visible.clip_plane",
                "depth.occlusion.gpu_scene.fail_visible.outside_view",
                "depth.occlusion.gpu_scene.fail_visible.small_bounds" };
            for (uint32_t reason = 1u; reason < failVisibleReasonNames.size();
                    ++reason) {
                (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                    failVisibleReasonNames[reason],
                    gpuSceneOcclusionFailVisibleReasons[reason]);
            }
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.invalid_results",
                invalidGpuSceneOcclusionResults);
            if (validation.occlusionQualificationOracle)
                (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                    "depth.occlusion.gpu_scene.unsafe_mismatch",
                    unsafeGpuSceneOcclusionMismatches);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.applied_rejects",
                gpuSceneOcclusionAppliedRejects);
        }
        else if (validation.occlusionProfileFrameId != 0u &&
            profiler != nullptr && validation.occlusionRejectionApplied) {
            const uint64_t appliedRejects = oracleCommands >= deviceCommands
                ? oracleCommands - deviceCommands : 0u;
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.requested",
                validation.gpuSceneOcclusionCandidateCount);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.tested", 0u,
                ProfileCounterStatus::Unavailable);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.would_reject", appliedRejects);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.fail_visible", 0u,
                ProfileCounterStatus::Unavailable);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.invalid_results", 0u,
                ProfileCounterStatus::Unavailable);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.unsafe_mismatch", 0u,
                ProfileCounterStatus::Unavailable);
            (void)profiler->attachCounter(validation.occlusionProfileFrameId,
                "depth.occlusion.gpu_scene.applied_rejects", appliedRejects);
        }
        if (lod.mismatchedCommands != 0)
            throw std::runtime_error("experimental GPU LOD command readback disagrees with the CPU oracle");
        if (validation.occlusionRejectionApplied &&
            (overflowCommands != 0u ||
                (validation.occlusionQualificationOracle && mismatchedBins != 0u)))
            throw std::runtime_error(
                "experimental GPU-scene depth-occlusion rejection disagrees with the indirect-command oracle");
        if (occlusionQueries.invalidResults != 0)
            throw std::runtime_error(
                "experimental depth-occlusion query returned invalid device results");
        if (invalidGpuSceneOcclusionResults != 0)
            throw std::runtime_error(
                "experimental GPU-scene depth-occlusion query returned invalid device results");
        if (unsafeGpuSceneOcclusionMismatches != 0)
            throw std::runtime_error(
                "experimental GPU-scene depth-occlusion query rejected CPU-visible work");
        validation.gpuSceneOcclusionPending = false;
        validation.pending = false;
    }

} // namespace Iridium
