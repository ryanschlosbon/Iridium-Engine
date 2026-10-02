#include "renderer/lighting/ClusteredReflectionProbes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Iridium {
    namespace {

        struct ProbeClusterBounds {
            uint32_t minX = 0;
            uint32_t maxX = 0;
            uint32_t minY = 0;
            uint32_t maxY = 0;
            uint32_t minZ = 0;
            uint32_t maxZ = 0;
            bool valid = false;
        };

        struct RankedClusterProbe {
            uint32_t slot = kInvalidEnvironmentTableSlot;
            float influence = 0.0f;
        };

        [[nodiscard]] bool finite(const glm::mat4& value) noexcept {
            for (uint32_t column = 0; column < 4; ++column) {
                for (uint32_t row = 0; row < 4; ++row) {
                    if (!std::isfinite(value[column][row])) return false;
                }
            }
            return true;
        }

        [[nodiscard]] uint32_t clusterIndex(ClusterGridDimensions dimensions,
            uint32_t x, uint32_t y, uint32_t z) noexcept {
            return (z * dimensions.tilesY + y) * dimensions.tilesX + x;
        }

        [[nodiscard]] ProbeClusterBounds boundsForProbe(
            const PackedGpuReflectionProbe& probe,
            const ClusterFrameParameters& frame,
            const ClusteredReflectionProbeConfig& config,
            ClusterGridDimensions dimensions) {
            const glm::vec3 center = glm::vec3(probe.positionIntensity);
            const bool box = (probe.metadata.x &
                PackedGpuReflectionProbeBoxShape) != 0u;
            const float radius = box
                ? glm::length(glm::vec3(probe.influence))
                : probe.influence.x;
            if (!std::isfinite(radius) || radius <= 0.0f) return {};
            const glm::vec3 viewCenter = glm::vec3(frame.view *
                glm::vec4(center, 1.0f));
            const float positiveDepth = -viewCenter.z;
            if (!std::isfinite(positiveDepth)) return {};
            const float minDepth = (std::max)(frame.nearPlane,
                positiveDepth - radius);
            const float maxDepth = (std::min)(frame.farPlane,
                positiveDepth + radius);
            if (maxDepth < frame.nearPlane || minDepth > frame.farPlane ||
                minDepth > maxDepth) return {};

            ProbeClusterBounds result;
            result.minZ = clusterDepthSlice(minDepth, frame.nearPlane,
                frame.farPlane, dimensions.depthSlices);
            result.maxZ = clusterDepthSlice(maxDepth, frame.nearPlane,
                frame.farPlane, dimensions.depthSlices);
            if (positiveDepth - radius <= frame.nearPlane) {
                result.maxX = dimensions.tilesX - 1u;
                result.maxY = dimensions.tilesY - 1u;
                result.valid = true;
                return result;
            }
            const float inverseDepth = 1.0f / positiveDepth;
            const float centerX = frame.projection[0][0] * viewCenter.x *
                inverseDepth;
            const float centerY = frame.projection[1][1] * viewCenter.y *
                inverseDepth;
            const float conservativeDepth = (std::max)(frame.nearPlane,
                positiveDepth - radius);
            const float radiusX = std::abs(frame.projection[0][0]) * radius /
                conservativeDepth;
            const float radiusY = std::abs(frame.projection[1][1]) * radius /
                conservativeDepth;
            const float minNdcX = centerX - radiusX;
            const float maxNdcX = centerX + radiusX;
            const float minNdcY = centerY - radiusY;
            const float maxNdcY = centerY + radiusY;
            if (maxNdcX < -1.0f || minNdcX > 1.0f ||
                maxNdcY < -1.0f || minNdcY > 1.0f) return {};
            const auto tile = [](float ndc, uint32_t pixels,
                uint32_t tileSize, uint32_t count) {
                const float pixel = std::clamp(ndc * 0.5f + 0.5f,
                    0.0f, 1.0f) * static_cast<float>(pixels);
                return (std::min)(count - 1u,
                    static_cast<uint32_t>(pixel) / tileSize);
            };
            result.minX = tile(minNdcX, frame.renderWidth,
                config.tileWidth, dimensions.tilesX);
            result.maxX = tile(maxNdcX, frame.renderWidth,
                config.tileWidth, dimensions.tilesX);
            result.minY = tile(minNdcY, frame.renderHeight,
                config.tileHeight, dimensions.tilesY);
            result.maxY = tile(maxNdcY, frame.renderHeight,
                config.tileHeight, dimensions.tilesY);
            result.valid = true;
            return result;
        }

        [[nodiscard]] glm::vec3 clusterCenterWorld(
            uint32_t x, uint32_t y, uint32_t z,
            ClusterGridDimensions dimensions,
            const ClusterFrameParameters& frame,
            const glm::mat4& inverseView,
            const glm::mat4& inverseProjection) {
            const float ndcX = (static_cast<float>(x) + 0.5f) /
                static_cast<float>(dimensions.tilesX) * 2.0f - 1.0f;
            const float ndcY = (static_cast<float>(y) + 0.5f) /
                static_cast<float>(dimensions.tilesY) * 2.0f - 1.0f;
            const float normalizedDepth =
                (static_cast<float>(z) + 0.5f) /
                static_cast<float>(dimensions.depthSlices);
            const float positiveDepth = frame.nearPlane * std::pow(
                frame.farPlane / frame.nearPlane, normalizedDepth);
            glm::vec4 viewFar = inverseProjection *
                glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
            viewFar /= viewFar.w;
            const float scale = positiveDepth /
                (std::max)(-viewFar.z, 1.0e-6f);
            return glm::vec3(inverseView *
                glm::vec4(glm::vec3(viewFar) * scale, 1.0f));
        }

        [[nodiscard]] float probeInfluenceAt(
            const PackedGpuReflectionProbe& probe,
            glm::vec3 worldPosition) noexcept {
            const glm::vec3 local = glm::vec3(probe.worldToProbe *
                glm::vec4(worldPosition, 1.0f));
            float boundary = 0.0f;
            if ((probe.metadata.x & PackedGpuReflectionProbeBoxShape) != 0u) {
                const glm::vec3 margin = glm::vec3(probe.influence) -
                    glm::abs(local);
                boundary = (std::min)(margin.x,
                    (std::min)(margin.y, margin.z));
            }
            else boundary = probe.influence.x - glm::length(local);
            if (!(boundary >= 0.0f)) return 0.0f;
            return probe.influence.w > 0.0f
                ? std::clamp(boundary / probe.influence.w, 0.0f, 1.0f)
                : 1.0f;
        }

    } // namespace

    ClusteredReflectionProbeAssigner::ClusteredReflectionProbeAssigner(
        ClusteredReflectionProbeConfig config)
        : config_(config) {
        if (config_.tileWidth == 0 || config_.tileHeight == 0 ||
            config_.depthSlices == 0 ||
            config_.maximumProbesPerCluster == 0 ||
            config_.maximumProbeReferences == 0) {
            throw std::invalid_argument(
                "Clustered reflection probe policy is invalid");
        }
    }

    ClusteredReflectionProbeProduct ClusteredReflectionProbeAssigner::build(
        const ReflectionProbeGpuFramePacket& probes,
        const ClusterFrameParameters& frame) const {
        if (!(frame.nearPlane > 0.0f) ||
            !(frame.farPlane > frame.nearPlane) ||
            !finite(frame.view) || !finite(frame.projection) ||
            probes.selectionMetadata.size() < probes.records.size()) {
            throw std::invalid_argument(
                "Clustered reflection probe frame is invalid");
        }
        const ClusterGridConfig gridConfig{
            .tileWidth = config_.tileWidth,
            .tileHeight = config_.tileHeight,
            .depthSlices = config_.depthSlices,
        };
        ClusteredReflectionProbeProduct result;
        result.dimensions = clusterGridDimensions(gridConfig, frame);
        const uint64_t clusterCount64 = result.dimensions.clusterCount();
        if (clusterCount64 > (std::numeric_limits<uint32_t>::max)()) {
            throw std::overflow_error(
                "Reflection probe cluster grid exceeds 32-bit addressing");
        }
        const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
        result.headers.resize(clusterCount);
        result.stats.activeProbeCount = static_cast<uint32_t>(
            probes.activeSlots.size());
        const uint64_t rankedCount64 = clusterCount64 *
            config_.maximumProbesPerCluster;
        if (rankedCount64 > (std::numeric_limits<size_t>::max)()) {
            throw std::overflow_error(
                "Reflection probe cluster candidate storage is too large");
        }
        std::vector<RankedClusterProbe> ranked(
            static_cast<size_t>(rankedCount64));
        std::vector<uint32_t> selectedCounts(clusterCount, 0u);
        std::vector<uint32_t> requestedCounts(clusterCount, 0u);
        const glm::mat4 inverseView = glm::inverse(frame.view);
        const glm::mat4 inverseProjection = glm::inverse(frame.projection);
        if (!finite(inverseView) || !finite(inverseProjection)) {
            throw std::invalid_argument(
                "Clustered reflection probe matrices are singular");
        }

        const auto preferred = [&](uint32_t lhsSlot, float lhsInfluence,
                                   uint32_t rhsSlot, float rhsInfluence) {
            const ReflectionProbeSelectionMetadata& lhs =
                probes.selectionMetadata[lhsSlot];
            const ReflectionProbeSelectionMetadata& rhs =
                probes.selectionMetadata[rhsSlot];
            if (lhs.priority != rhs.priority)
                return lhs.priority > rhs.priority;
            if (lhsInfluence != rhsInfluence)
                return lhsInfluence > rhsInfluence;
            if (lhs.influenceVolume != rhs.influenceVolume)
                return lhs.influenceVolume < rhs.influenceVolume;
            return lhs.owner < rhs.owner;
        };
        for (uint32_t slot : probes.activeSlots) {
            if (slot >= probes.records.size()) {
                throw std::out_of_range(
                    "Active reflection probe slot is outside the record table");
            }
            const ProbeClusterBounds bounds = boundsForProbe(
                probes.records[slot], frame, config_, result.dimensions);
            if (!bounds.valid) continue;
            for (uint32_t z = bounds.minZ; z <= bounds.maxZ; ++z) {
                for (uint32_t y = bounds.minY; y <= bounds.maxY; ++y) {
                    for (uint32_t x = bounds.minX; x <= bounds.maxX; ++x) {
                        const uint32_t cluster = clusterIndex(
                            result.dimensions, x, y, z);
                        if (requestedCounts[cluster] !=
                            (std::numeric_limits<uint32_t>::max)())
                            ++requestedCounts[cluster];
                        const glm::vec3 center = clusterCenterWorld(x, y, z,
                            result.dimensions, frame, inverseView,
                            inverseProjection);
                        const float influence = probeInfluenceAt(
                            probes.records[slot], center);
                        const size_t base = static_cast<size_t>(cluster) *
                            config_.maximumProbesPerCluster;
                        uint32_t count = selectedCounts[cluster];
                        uint32_t position = count;
                        for (uint32_t index = 0; index < count; ++index) {
                            const RankedClusterProbe& current = ranked[base + index];
                            if (preferred(slot, influence,
                                    current.slot, current.influence)) {
                                position = index;
                                break;
                            }
                        }
                        if (position >= config_.maximumProbesPerCluster)
                            continue;
                        const uint32_t newCount = (std::min)(count + 1u,
                            config_.maximumProbesPerCluster);
                        for (uint32_t index = newCount - 1u;
                            index > position; --index) {
                            ranked[base + index] = ranked[base + index - 1u];
                        }
                        ranked[base + position] = { slot, influence };
                        selectedCounts[cluster] = newCount;
                    }
                }
            }
        }

        result.probeSlots.reserve((std::min)(
            static_cast<uint64_t>(config_.maximumProbeReferences),
            clusterCount64 * config_.maximumProbesPerCluster));
        for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
            const uint32_t requested = requestedCounts[cluster];
            result.stats.requestedProbeReferences += requested;
            result.stats.maximumRequestedOccupancy = (std::max)(
                result.stats.maximumRequestedOccupancy, requested);
            if (requested != 0) ++result.stats.clustersUsed;
            const uint32_t available = config_.maximumProbeReferences -
                static_cast<uint32_t>(result.probeSlots.size());
            const uint32_t publish = (std::min)(
                selectedCounts[cluster], available);
            result.headers[cluster] = {
                static_cast<uint32_t>(result.probeSlots.size()), publish };
            const size_t base = static_cast<size_t>(cluster) *
                config_.maximumProbesPerCluster;
            for (uint32_t index = 0; index < publish; ++index)
                result.probeSlots.push_back(ranked[base + index].slot);
            if (publish < selectedCounts[cluster]) {
                result.stats.overflow =
                    ClusterProbeOverflowCode::GlobalReferenceCapacity;
            }
        }
        result.stats.publishedProbeReferences = static_cast<uint32_t>(
            result.probeSlots.size());
        result.stats.truncatedProbeReferences =
            result.stats.requestedProbeReferences -
            result.stats.publishedProbeReferences;
        return result;
    }

} // namespace Iridium
