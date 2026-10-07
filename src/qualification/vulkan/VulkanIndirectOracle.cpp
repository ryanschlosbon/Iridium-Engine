#include "qualification/vulkan/VulkanIndirectOracle.h"

#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace Iridium {

    namespace {
        [[nodiscard]] size_t shadowViewIndex(VulkanIndirectOracleView view) {
            switch (view) {
            case VulkanIndirectOracleView::DirectionalShadow: return 0u;
            case VulkanIndirectOracleView::SpotShadow: return 1u;
            case VulkanIndirectOracleView::PointShadow: return 2u;
            case VulkanIndirectOracleView::ReflectionProbe: return 3u;
            default:
                throw std::invalid_argument(
                    "Indirect oracle view is not a shadow/probe consumer");
            }
        }

        [[nodiscard]] bool commandLess(const GpuSceneIndexedIndirectCommand& left,
            const GpuSceneIndexedIndirectCommand& right) noexcept {
            return std::tie(left.firstInstance, left.indexCount,
                left.firstIndex, left.vertexOffset, left.instanceCount) <
                std::tie(right.firstInstance, right.indexCount,
                    right.firstIndex, right.vertexOffset, right.instanceCount);
        }

        [[nodiscard]] bool commandEqual(const GpuSceneIndexedIndirectCommand& left,
            const GpuSceneIndexedIndirectCommand& right) noexcept {
            return left.indexCount == right.indexCount &&
                left.instanceCount == right.instanceCount &&
                left.firstIndex == right.firstIndex &&
                left.vertexOffset == right.vertexOffset &&
                left.firstInstance == right.firstInstance;
        }
    }

    bool VulkanIndirectOracle::enabled(
        VulkanIndirectOracleView view) const noexcept {
        switch (view) {
        case VulkanIndirectOracleView::DirectionalShadow:
        case VulkanIndirectOracleView::SpotShadow:
        case VulkanIndirectOracleView::PointShadow:
            return config_.shadowIndirect;
        case VulkanIndirectOracleView::ReflectionProbe:
            return config_.shadowIndirect || config_.probeLod;
        case VulkanIndirectOracleView::OpaqueLod:
            return config_.gpuLod;
        case VulkanIndirectOracleView::DepthOcclusion:
            return config_.depthOcclusion;
        case VulkanIndirectOracleView::VirtualShadowDepth:
            return config_.virtualShadowDepth;
        }
        return false;
    }

    VulkanIndirectOracle::ShadowSlot& VulkanIndirectOracle::shadowSlot(
        VulkanIndirectOracleView view, uint32_t slot) {
        if (slot >= FramesInFlight)
            throw std::out_of_range("Indirect oracle frame slot");
        return shadow_[shadowViewIndex(view)][slot];
    }

    void VulkanIndirectOracle::beginShadowWork(VulkanIndirectOracleView view,
        uint32_t slot, size_t countRegionCount) {
        ShadowSlot& work = shadowSlot(view, slot);
        work.begun = true;
        work.expectedCounts.assign(countRegionCount, 0u);
        work.expectedCommands.assign(countRegionCount, {});
    }

    void VulkanIndirectOracle::expectShadowCommand(VulkanIndirectOracleView view,
        uint32_t slot, size_t countIndex,
        const GpuSceneIndexedIndirectCommand& command) {
        ShadowSlot& work = shadowSlot(view, slot);
        if (!work.begun || countIndex >= work.expectedCounts.size()) return;
        ++work.expectedCounts[countIndex];
        work.expectedCommands[countIndex].push_back(command);
    }

    VulkanIndirectOracleResult VulkanIndirectOracle::verifyShadowWork(
        VulkanIndirectOracleView view, uint32_t slot,
        const VulkanIndirectReadback& readback) {
        ShadowSlot& work = shadowSlot(view, slot);
        VulkanIndirectOracleResult result{};
        if (!work.begun) return result;
        result.validated = true;
        const auto baseIndexCount = [&](uint32_t primitiveIndex) {
            if (primitiveIndex >= readback.primitives.size()) return 0u;
            const uint32_t geometryIndex =
                readback.primitives[primitiveIndex].binding.y;
            return geometryIndex < readback.geometries.size()
                ? readback.geometries[geometryIndex].draw.y : 0u;
        };
        std::vector<GpuSceneIndexedIndirectCommand> actualCommands;
        for (size_t index = 0; index < readback.countCapacities.size(); ++index) {
            const uint32_t capacity = readback.countCapacities[index];
            const uint32_t deviceCount = readback.counts != nullptr
                ? readback.counts[index] : 0u;
            const uint32_t submitted = (std::min)(deviceCount, capacity);
            const uint32_t commandOffset = index < readback.commandOffsets.size()
                ? readback.commandOffsets[index] : 0u;
            actualCommands.clear();
            actualCommands.reserve(submitted);
            for (uint32_t commandIndex = 0u;
                    readback.commands != nullptr && commandIndex < submitted;
                    ++commandIndex)
                actualCommands.push_back(
                    readback.commands[commandOffset + commandIndex]);
            const uint32_t expectedCount = index < work.expectedCounts.size()
                ? work.expectedCounts[index] : 0u;
            result.oracleCommands += expectedCount;
            result.mismatchedBins += submitted != expectedCount ? 1u : 0u;
            auto sortedExpected = index < work.expectedCommands.size()
                ? work.expectedCommands[index]
                : std::vector<GpuSceneIndexedIndirectCommand>{};
            for (const GpuSceneIndexedIndirectCommand& command : sortedExpected) {
                result.oracleTriangles += command.indexCount / 3u;
                result.oracleReducedCommands += command.indexCount <
                    baseIndexCount(command.firstInstance) ? 1u : 0u;
            }
            std::ranges::sort(actualCommands, commandLess);
            std::ranges::sort(sortedExpected, commandLess);
            result.mismatchedCommandRegions += !std::ranges::equal(
                actualCommands, sortedExpected, commandEqual) ? 1u : 0u;
        }
        work.begun = false;
        work.expectedCommands.clear();
        return result;
    }

    void VulkanIndirectOracle::beginOpaqueWork(uint32_t slot,
        uint32_t primitiveCount, uint32_t candidateCount, bool lod,
        bool occlusion) {
        if (slot >= FramesInFlight)
            throw std::out_of_range("Indirect oracle frame slot");
        OpaqueSlot& work = opaque_[slot];
        work.lod = lod;
        work.occlusion = occlusion;
        if (lod) {
            work.expectedCommandsByPrimitive.assign(primitiveCount, {});
            work.seenPrimitives.assign(primitiveCount, 0u);
        }
        else {
            work.expectedCommandsByPrimitive.clear();
            work.seenPrimitives.clear();
        }
        work.baseTriangles = work.oracleTriangles = work.oracleReducedCommands = 0u;
        work.historyValid = work.historyReset = work.historyChanged = 0u;
        work.deviceTriangles = work.mismatchedCommands = 0u;
        work.candidateCount = candidateCount;
        work.projectedCandidateIndices.clear();
        work.projectionRejected = 0u;
        if (occlusion)
            work.cpuVisibleCandidates.assign(candidateCount, 0u);
        else
            work.cpuVisibleCandidates.clear();
        work.cpuProjectedCandidates.clear();
        work.cpuOccludedCandidates.clear();
    }

    void VulkanIndirectOracle::expectOpaqueVisibleCandidate(uint32_t slot,
        uint32_t candidateIndex) {
        OpaqueSlot& work = opaque_.at(slot);
        if (candidateIndex < work.cpuVisibleCandidates.size())
            work.cpuVisibleCandidates[candidateIndex] = 1u;
    }

    void VulkanIndirectOracle::expectOpaqueOcclusionQuery(uint32_t slot,
        uint32_t candidateIndex) {
        opaque_.at(slot).projectedCandidateIndices.push_back(candidateIndex);
    }

    void VulkanIndirectOracle::expectOpaqueProjectionRejected(uint32_t slot) {
        ++opaque_.at(slot).projectionRejected;
    }

    void VulkanIndirectOracle::expectOpaqueLod(uint32_t slot,
        uint32_t primitiveIndex, const VulkanOpaqueLodExpectation& expectation) {
        OpaqueSlot& work = opaque_.at(slot);
        if (primitiveIndex >= work.expectedCommandsByPrimitive.size())
            throw std::out_of_range("GPU LOD oracle primitive index");
        work.expectedCommandsByPrimitive[primitiveIndex] = expectation.command;
        work.baseTriangles += expectation.baseTriangles;
        work.oracleTriangles += expectation.oracleTriangles;
        work.oracleReducedCommands += expectation.reduced ? 1u : 0u;
        work.historyValid += expectation.historyValid ? 1u : 0u;
        work.historyReset += expectation.historyValid ? 0u : 1u;
        work.historyChanged += expectation.historyChanged ? 1u : 0u;
    }

    void VulkanIndirectOracle::verifyOpaqueLodCommands(uint32_t slot,
        const uint32_t* counts, std::span<const uint32_t> binCapacities,
        const GpuSceneIndexedIndirectCommand* commands) {
        OpaqueSlot& work = opaque_.at(slot);
        if (!work.lod || work.expectedCommandsByPrimitive.empty()) return;
        const uint32_t commandCapacity = std::accumulate(
            binCapacities.begin(), binCapacities.end(), 0u);
        work.commandReadback.resize(commandCapacity);
        // One contiguous readback avoids repeatedly touching uncached mapped
        // memory during field-by-field oracle/duplicate checks.
        std::memcpy(work.commandReadback.data(), commands,
            commandCapacity * sizeof(GpuSceneIndexedIndirectCommand));
        uint32_t commandBase = 0u;
        for (size_t bin = 0; bin < binCapacities.size(); ++bin) {
            const uint32_t capacity = binCapacities[bin];
            const uint32_t deviceCount = counts != nullptr ? counts[bin] : 0u;
            const uint32_t submittedCount = (std::min)(deviceCount, capacity);
            for (uint32_t offset = 0; offset < submittedCount; ++offset) {
                const auto& command = work.commandReadback[commandBase + offset];
                work.deviceTriangles += command.indexCount / 3u;
                if (command.firstInstance >=
                        work.expectedCommandsByPrimitive.size()) {
                    ++work.mismatchedCommands;
                    continue;
                }
                const auto& expected =
                    work.expectedCommandsByPrimitive[command.firstInstance];
                work.mismatchedCommands +=
                    work.seenPrimitives[command.firstInstance] != 0 ||
                    std::memcmp(&command, &expected, sizeof(command)) != 0
                    ? 1u : 0u;
                work.seenPrimitives[command.firstInstance] = 1u;
            }
            commandBase += capacity;
        }
    }

    VulkanOcclusionQueryVerdict VulkanIndirectOracle::verifyOcclusionQueries(
        uint32_t slot, const DepthPyramidDeviceResult* results) {
        OpaqueSlot& work = opaque_.at(slot);
        VulkanOcclusionQueryVerdict verdict{};
        if (!work.occlusion) return verdict;
        verdict.queryCount = static_cast<uint32_t>(
            work.projectedCandidateIndices.size());
        verdict.projectionRejected = work.projectionRejected;
        verdict.active = verdict.queryCount != 0u ||
            verdict.projectionRejected != 0u;
        work.cpuProjectedCandidates.assign(work.candidateCount, 0u);
        work.cpuOccludedCandidates.assign(work.candidateCount, 0u);
        if (verdict.queryCount == 0u) return verdict;
        if (results == nullptr) {
            verdict.invalidResults = verdict.queryCount;
            return verdict;
        }
        for (uint32_t index = 0; index < verdict.queryCount; ++index) {
            const auto& result = results[index];
            const bool valid =
                result.abiVersion == DepthPyramidAbiVersion &&
                result.mipLevel < 32u && result.sampledTexels >= 1u &&
                result.sampledTexels <= 4u && result.tested == 1u &&
                result.occluded <= 1u &&
                std::isfinite(result.farthestOccluderDepth) &&
                result.farthestOccluderDepth >= 0.0f &&
                result.farthestOccluderDepth <= 1.0f;
            if (!valid) {
                ++verdict.invalidResults;
                continue;
            }
            ++verdict.tested;
            verdict.wouldReject += result.occluded;
            const uint32_t candidateIndex = work.projectedCandidateIndices[index];
            if (candidateIndex >= work.cpuProjectedCandidates.size()) {
                ++verdict.invalidResults;
                continue;
            }
            work.cpuProjectedCandidates[candidateIndex] = 1u;
            work.cpuOccludedCandidates[candidateIndex] =
                static_cast<uint8_t>(result.occluded);
        }
        return verdict;
    }

    bool VulkanIndirectOracle::opaqueCandidateCpuVisible(uint32_t slot,
        uint32_t candidateIndex) const noexcept {
        if (slot >= FramesInFlight) return false;
        const OpaqueSlot& work = opaque_[slot];
        return candidateIndex < work.cpuVisibleCandidates.size() &&
            work.cpuVisibleCandidates[candidateIndex] != 0u;
    }

    bool VulkanIndirectOracle::unsafeGpuSceneOcclusion(uint32_t slot,
        uint32_t candidateIndex) const noexcept {
        if (!opaqueCandidateCpuVisible(slot, candidateIndex)) return false;
        const OpaqueSlot& work = opaque_[slot];
        const bool projected = candidateIndex < work.cpuProjectedCandidates.size() &&
            work.cpuProjectedCandidates[candidateIndex] != 0u;
        const bool occluded = candidateIndex < work.cpuOccludedCandidates.size() &&
            work.cpuOccludedCandidates[candidateIndex] != 0u;
        return !projected || !occluded;
    }

    bool VulkanIndirectOracle::rejectOpaqueLodPrimitive(uint32_t slot,
        uint32_t primitiveIndex) {
        OpaqueSlot& work = opaque_.at(slot);
        if (!work.lod || work.expectedCommandsByPrimitive.empty()) return true;
        if (primitiveIndex >= work.expectedCommandsByPrimitive.size())
            return false;
        work.expectedCommandsByPrimitive[primitiveIndex].instanceCount = 0u;
        return true;
    }

    VulkanOpaqueLodVerdict VulkanIndirectOracle::finishOpaqueLod(uint32_t slot) {
        OpaqueSlot& work = opaque_.at(slot);
        VulkanOpaqueLodVerdict verdict{};
        verdict.active = work.lod && !work.expectedCommandsByPrimitive.empty();
        if (!verdict.active) return verdict;
        for (size_t primitive = 0;
                primitive < work.expectedCommandsByPrimitive.size(); ++primitive)
            if (work.expectedCommandsByPrimitive[primitive].instanceCount != 0 &&
                work.seenPrimitives[primitive] == 0)
                ++work.mismatchedCommands;
        verdict.deviceTriangles = work.deviceTriangles;
        verdict.mismatchedCommands = work.mismatchedCommands;
        verdict.baseTriangles = work.baseTriangles;
        verdict.oracleTriangles = work.oracleTriangles;
        verdict.oracleReducedCommands = work.oracleReducedCommands;
        verdict.historyValid = work.historyValid;
        verdict.historyReset = work.historyReset;
        verdict.historyChanged = work.historyChanged;
        return verdict;
    }

    void VulkanIndirectOracle::recordVirtualShadowDepthSnapshot(
        VkCommandBuffer cmd, uint32_t slot,
        const VulkanVirtualShadowDepthPayload& payload) {
        if (allocator_ == nullptr || payload.depth == nullptr)
            throw std::logic_error(
                "Virtual-shadow depth oracle is not attached to the device");
        VirtualShadowSlot& snapshot = virtualShadow_.at(slot);
        const uint64_t pixels = uint64_t{ payload.extent.width } *
            payload.extent.height;
        if (pixels > std::numeric_limits<uint32_t>::max())
            throw std::overflow_error(
                "Virtual-shadow depth oracle pixel count exceeds its ABI");
        const VkDeviceSize depthBytes = pixels * sizeof(float);
        if (snapshot.depthReadback.size != depthBytes) {
            // Current slot's fence retired its previous capture before resize.
            allocator_->destroy(snapshot.depthReadback);
            snapshot.depthReadback = allocator_->createBuffer(depthBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::CaptureReadback);
        }
        snapshot.extent = payload.extent;
        snapshot.inverseViewProjection = payload.inverseViewProjection;
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        copy.imageExtent = { payload.extent.width, payload.extent.height, 1 };
        vkCmdCopyImageToBuffer(cmd, payload.depth->image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            snapshot.depthReadback.buffer, 1, &copy);
        VkBufferMemoryBarrier depthBarrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        depthBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        depthBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        depthBarrier.srcQueueFamilyIndex = depthBarrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.buffer = snapshot.depthReadback.buffer;
        depthBarrier.size = snapshot.depthReadback.size;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &depthBarrier,
            0, nullptr);
    }

    bool VulkanIndirectOracle::beginVirtualShadowVerify(
        const VulkanVirtualShadowOracleInput& input) {
        virtualShadowPlan_.reset();
        if (!config_.virtualShadowDepth) return false;
        CpuScope scope(profiler_, "cpu.render.virtual-shadow.depth-oracle");
        const VirtualShadowSlot& snapshot = virtualShadow_.at(input.slot);
        const VkExtent2D extent = snapshot.extent;
        const uint32_t pixels = extent.width * extent.height;
        if (!snapshot.depthReadback.mapped || !pixels)
            throw std::logic_error(
                "Virtual-shadow depth oracle has no retired depth snapshot");
        const auto packedReceivers = buildVirtualShadowDepthReceivers(
            { extent.width, extent.height, 0, 0, extent.width, extent.height,
                false },
            snapshot.inverseViewProjection,
            std::span(static_cast<const float*>(snapshot.depthReadback.mapped),
                pixels), pixels);
        std::vector<DirectionalVirtualShadowReceiverSample> receivers;
        receivers.reserve(packedReceivers.size());
        for (const auto& receiver : packedReceivers)
            if (receiver.receiverSamples)
                receivers.push_back({ glm::vec3(receiver.worldPosition),
                    receiver.receiverSamples });
        DirectionalVirtualShadowMarkConfig markConfig{};
        markConfig.pageSizeTexels = input.pageSizeTexels;
        markConfig.maximumUniquePageRequests = input.requestCapacity;
        DirectionalVirtualShadowMarkPlan plan =
            buildDirectionalVirtualShadowReceiverMarks(markConfig, input.levels,
                receivers);
        if (plan.uniquePagesBeforeCapacity != input.uniquePagesBeforeCapacity ||
            plan.requests.size() != input.outputRequestCount ||
            plan.requestCapacityOverflow != input.requestCapacityOverflow ||
            plan.requestCapacityDroppedSamples !=
                input.requestCapacityDroppedSamples)
            throw std::runtime_error(
                "Live virtual-shadow depth oracle telemetry mismatch");
        if (profiler_) {
            profiler_->recordCounter("shadow.virtual.oracle.depth_pixels", pixels);
            profiler_->recordCounter("shadow.virtual.oracle.receiver_samples",
                plan.markedReceiverSamples);
            profiler_->recordCounter("shadow.virtual.oracle.depth_readback_bytes",
                snapshot.depthReadback.size);
        }
        virtualShadowPlan_ = std::move(plan);
        return true;
    }

    void VulkanIndirectOracle::verifyVirtualShadowRequest(uint32_t requestIndex,
        const VirtualShadowPageRequest& request) {
        if (!virtualShadowPlan_) return;
        const auto& expected = virtualShadowPlan_->requests.at(requestIndex);
        if (request.address != expected.address ||
            request.staticCasterRevision != expected.staticCasterRevision ||
            request.dynamicCasterRevision != expected.dynamicCasterRevision ||
            request.receiverSamples != expected.receiverSamples ||
            request.priority != expected.priority ||
            request.requiredLayers != expected.requiredLayers)
            throw std::runtime_error(
                "Live virtual-shadow depth oracle request/coverage mismatch");
    }

    uint64_t VulkanIndirectOracle::comparedVirtualShadowRequests() const noexcept {
        return virtualShadowPlan_ ? virtualShadowPlan_->requests.size() : 0u;
    }

    void VulkanIndirectOracle::destroyDeviceResources() noexcept {
        if (allocator_ != nullptr)
            for (VirtualShadowSlot& snapshot : virtualShadow_)
                allocator_->destroy(snapshot.depthReadback);
        virtualShadow_ = {};
        virtualShadowPlan_.reset();
        for (auto& view : shadow_)
            for (ShadowSlot& work : view) work = {};
        opaque_ = {};
    }

} // namespace Iridium
