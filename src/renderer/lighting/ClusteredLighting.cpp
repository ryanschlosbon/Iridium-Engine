#include "renderer/lighting/ClusteredLighting.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace Iridium {
    namespace {

        struct ClusterBounds {
            uint32_t minX = 0;
            uint32_t maxX = 0;
            uint32_t minY = 0;
            uint32_t maxY = 0;
            uint32_t minZ = 0;
            uint32_t maxZ = 0;
            bool valid = false;
        };

        [[nodiscard]] bool finite(const glm::mat4& value) noexcept {
            for (uint32_t column = 0; column < 4; ++column) {
                for (uint32_t row = 0; row < 4; ++row) {
                    if (!std::isfinite(value[column][row])) return false;
                }
            }
            return true;
        }

        [[nodiscard]] PackedGpuLightType lightType(
            const PackedGpuLight& light) noexcept {
            return static_cast<PackedGpuLightType>(
                std::bit_cast<uint32_t>(light.shapeMetadata.z) & 3u);
        }

        [[nodiscard]] uint32_t clusterIndex(ClusterGridDimensions dimensions,
            uint32_t x, uint32_t y, uint32_t z) noexcept {
            return (z * dimensions.tilesY + y) * dimensions.tilesX + x;
        }

        [[nodiscard]] ClusterBounds boundsForLocalLight(
            const PackedGpuLight& light, const ClusterFrameParameters& frame,
            const ClusterGridConfig& config,
            ClusterGridDimensions dimensions) {
            const PackedGpuLightType type = lightType(light);
            glm::vec3 center = glm::vec3(light.positionRange);
            float radius = light.positionRange.w;
            if (!std::isfinite(radius) || radius <= 0.0f) return {};
            if (type == PackedGpuLightType::Spot) {
                const float halfRange = radius * 0.5f;
                const float outerCos = std::clamp(
                    light.directionOuterCos.w, 0.0f, 1.0f);
                const float coneRadius = outerCos > 1.0e-4f
                    ? radius * std::sqrt((std::max)(0.0f,
                        1.0f - outerCos * outerCos)) / outerCos
                    : std::numeric_limits<float>::max() * 0.25f;
                center += glm::vec3(light.directionOuterCos) * halfRange;
                radius = std::hypot(halfRange, coneRadius) +
                    (std::max)(0.0f, light.shapeMetadata.x);
            }
            const glm::vec3 viewCenter = glm::vec3(frame.view *
                glm::vec4(center, 1.0f));
            const float positiveDepth = -viewCenter.z;
            if (!std::isfinite(positiveDepth) || !std::isfinite(radius)) {
                // A 90-degree cone conservatively covers the complete frustum.
                if (type != PackedGpuLightType::Spot) return {};
                return { 0, dimensions.tilesX - 1, 0, dimensions.tilesY - 1,
                    0, dimensions.depthSlices - 1, true };
            }
            const float minDepth = (std::max)(frame.nearPlane,
                positiveDepth - radius);
            const float maxDepth = (std::min)(frame.farPlane,
                positiveDepth + radius);
            if (maxDepth < frame.nearPlane || minDepth > frame.farPlane ||
                minDepth > maxDepth) return {};

            ClusterBounds result{};
            result.minZ = clusterDepthSlice(minDepth, frame.nearPlane,
                frame.farPlane, dimensions.depthSlices);
            result.maxZ = clusterDepthSlice(maxDepth, frame.nearPlane,
                frame.farPlane, dimensions.depthSlices);

            if (positiveDepth - radius <= frame.nearPlane) {
                result.minX = result.minY = 0;
                result.maxX = dimensions.tilesX - 1;
                result.maxY = dimensions.tilesY - 1;
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
                return (std::min)(count - 1,
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

        [[nodiscard]] float conservativeContribution(
            uint32_t slot, const LightingFramePacket& lights,
            const ClusterFrameParameters& frame) noexcept {
            const PackedGpuLight& light = lights.records[slot];
            if (lightType(light) == PackedGpuLightType::Directional) {
                return light.colorIntensity.w;
            }
            const glm::vec3 viewPosition = glm::vec3(frame.view *
                glm::vec4(glm::vec3(light.positionRange), 1.0f));
            const float distanceSquared = glm::dot(viewPosition, viewPosition);
            const float radiusSquared = light.shapeMetadata.x *
                light.shapeMetadata.x;
            return light.colorIntensity.w /
                (std::max)({ distanceSquared, radiusSquared, 1.0e-6f });
        }

        void buildFallback(ClusteredLightingProduct& result,
            const LightingFramePacket& lights,
            const ClusterFrameParameters& frame,
            const ClusterGridConfig& config) {
            selectClusterFallbackLights(lights, frame.view,
                config.maximumFallbackLights, result.fallbackLightSlots);
            result.stats.fallbackLightCount = static_cast<uint32_t>(
                result.fallbackLightSlots.size());
            result.stats.droppedLightCount = result.stats.activeLightCount -
                result.stats.fallbackLightCount;
            std::ranges::fill(result.headers, ClusterLightHeader{});
            result.localLightSlots.clear();
            result.globalDirectionalSlots.clear();
        }

    } // namespace

    ClusterGridDimensions clusterGridDimensions(
        const ClusterGridConfig& config,
        const ClusterFrameParameters& frame) {
        if (config.tileWidth == 0 || config.tileHeight == 0 ||
            config.depthSlices == 0 || frame.renderWidth == 0 ||
            frame.renderHeight == 0) {
            throw std::invalid_argument("Cluster grid dimensions must be nonzero");
        }
        return {
            (frame.renderWidth + config.tileWidth - 1) / config.tileWidth,
            (frame.renderHeight + config.tileHeight - 1) / config.tileHeight,
            config.depthSlices,
        };
    }

    uint32_t clusterDepthSlice(float positiveViewDepth, float nearPlane,
        float farPlane, uint32_t sliceCount) {
        if (!(nearPlane > 0.0f) || !(farPlane > nearPlane) ||
            sliceCount == 0 || !std::isfinite(positiveViewDepth)) {
            throw std::invalid_argument("Invalid logarithmic cluster depth input");
        }
        const float depth = std::clamp(positiveViewDepth, nearPlane, farPlane);
        const float normalized = std::log(depth / nearPlane) /
            std::log(farPlane / nearPlane);
        return (std::min)(sliceCount - 1, static_cast<uint32_t>(
            normalized * static_cast<float>(sliceCount)));
    }

    void selectClusterFallbackLights(const LightingFramePacket& lights,
        const glm::mat4& view, uint32_t maximumLights,
        std::vector<uint32_t>& output) {
        if (maximumLights == 0 || !finite(view) ||
            lights.selectionMetadata.size() < lights.records.size()) {
            throw std::invalid_argument("Cluster fallback policy is invalid");
        }
        output.assign(lights.activeSlots.begin(), lights.activeSlots.end());
        for (uint32_t slot : output) {
            if (slot >= lights.records.size()) {
                throw std::out_of_range(
                    "Cluster fallback slot is outside the light table");
            }
        }
        const ClusterFrameParameters frame{ .view = view };
        const auto preferred = [&](uint32_t lhs, uint32_t rhs) {
            const LightSelectionMetadata& left = lights.selectionMetadata[lhs];
            const LightSelectionMetadata& right = lights.selectionMetadata[rhs];
            if (left.priority != right.priority)
                return left.priority > right.priority;
            if (left.castsShadows != right.castsShadows)
                return left.castsShadows > right.castsShadows;
            const float leftContribution = conservativeContribution(
                lhs, lights, frame);
            const float rightContribution = conservativeContribution(
                rhs, lights, frame);
            if (leftContribution != rightContribution)
                return leftContribution > rightContribution;
            return left.owner < right.owner;
        };
        const size_t selectedCount = (std::min)(output.size(),
            static_cast<size_t>(maximumLights));
        std::partial_sort(output.begin(), output.begin() + selectedCount,
            output.end(), preferred);
        output.resize(selectedCount);
    }

    ClusteredLightAssigner::ClusteredLightAssigner(ClusterGridConfig config)
        : config_(config) {
        if (config_.tileWidth == 0 || config_.tileHeight == 0 ||
            config_.depthSlices == 0 ||
            config_.maximumLightsPerCluster == 0 ||
            config_.maximumLightReferences == 0 ||
            config_.maximumDirectionalLights == 0 ||
            config_.maximumFallbackLights == 0) {
            throw std::invalid_argument("Cluster assignment policy is invalid");
        }
    }

    ClusteredLightingProduct ClusteredLightAssigner::build(
        const LightingFramePacket& lights,
        const ClusterFrameParameters& frame) const {
        if (!(frame.nearPlane > 0.0f) ||
            !(frame.farPlane > frame.nearPlane) ||
            !finite(frame.view) || !finite(frame.projection) ||
            lights.selectionMetadata.size() < lights.records.size()) {
            throw std::invalid_argument("Cluster frame or light metadata is invalid");
        }
        ClusteredLightingProduct result{};
        result.dimensions = clusterGridDimensions(config_, frame);
        const uint64_t clusterCount64 = result.dimensions.clusterCount();
        if (clusterCount64 > std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("Cluster grid exceeds 32-bit addressing");
        }
        const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
        result.headers.resize(clusterCount);
        result.stats.activeLightCount = static_cast<uint32_t>(
            lights.activeSlots.size());

        std::vector<uint32_t> localSlots;
        localSlots.reserve(lights.activeSlots.size());
        for (uint32_t slot : lights.activeSlots) {
            if (slot >= lights.records.size()) {
                throw std::out_of_range("Active light slot is outside record table");
            }
            if (lightType(lights.records[slot]) ==
                PackedGpuLightType::Directional) {
                result.globalDirectionalSlots.push_back(slot);
            }
            else localSlots.push_back(slot);
        }
        std::ranges::sort(result.globalDirectionalSlots);
        std::ranges::sort(localSlots);
        result.stats.directionalLightCount = static_cast<uint32_t>(
            result.globalDirectionalSlots.size());
        result.stats.localLightCount = static_cast<uint32_t>(localSlots.size());
        if (result.globalDirectionalSlots.size() >
            config_.maximumDirectionalLights) {
            result.stats.overflow = ClusterOverflowCode::DirectionalCapacity;
        }

        std::vector<uint32_t> counts(clusterCount, 0);
        const auto visit = [&](uint32_t slot, auto&& operation) {
            const ClusterBounds bounds = boundsForLocalLight(
                lights.records[slot], frame, config_, result.dimensions);
            if (!bounds.valid) return;
            for (uint32_t z = bounds.minZ; z <= bounds.maxZ; ++z) {
                for (uint32_t y = bounds.minY; y <= bounds.maxY; ++y) {
                    for (uint32_t x = bounds.minX; x <= bounds.maxX; ++x) {
                        operation(clusterIndex(result.dimensions, x, y, z));
                    }
                }
            }
        };
        for (uint32_t slot : localSlots) {
            visit(slot, [&](uint32_t cluster) {
                if (counts[cluster] != std::numeric_limits<uint32_t>::max())
                    ++counts[cluster];
            });
        }
        for (uint32_t count : counts) {
            result.stats.requestedLightReferences += count;
            result.stats.maximumClusterOccupancy = (std::max)(
                result.stats.maximumClusterOccupancy, count);
            if (count != 0) ++result.stats.clustersUsed;
            if (count > config_.maximumLightsPerCluster &&
                result.stats.overflow == ClusterOverflowCode::None) {
                result.stats.overflow = ClusterOverflowCode::PerClusterCapacity;
            }
        }
        if (result.stats.requestedLightReferences >
                config_.maximumLightReferences &&
            result.stats.overflow == ClusterOverflowCode::None) {
            result.stats.overflow = ClusterOverflowCode::GlobalReferenceCapacity;
        }
        if (result.stats.overflow != ClusterOverflowCode::None) {
            buildFallback(result, lights, frame, config_);
            return result;
        }

        uint32_t offset = 0;
        for (uint32_t cluster = 0; cluster < clusterCount; ++cluster) {
            result.headers[cluster] = { offset, counts[cluster] };
            offset += counts[cluster];
        }
        result.localLightSlots.resize(offset);
        std::vector<uint32_t> cursors(clusterCount, 0);
        for (uint32_t slot : localSlots) {
            visit(slot, [&](uint32_t cluster) {
                result.localLightSlots[result.headers[cluster].offset +
                    cursors[cluster]++] = slot;
            });
        }
        result.stats.publishedLightReferences = offset;
        return result;
    }

} // namespace Iridium
