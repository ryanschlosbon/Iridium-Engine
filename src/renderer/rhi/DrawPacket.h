#pragma once
#include "assets/AssetGuid.h"
#include "material/TransparencyPolicy.h"
#include "RenderHandles.h"
#include "scene/SceneEntityUuid.h"
#include <algorithm>
#include <compare>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <glm/glm.hpp>

namespace Iridium {

    struct TransparentWorkIdentity {
        SceneEntityUuid owner;
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;
        AssetGuid materialGuid;

        auto operator<=>(const TransparentWorkIdentity&) const = default;
    };

    static_assert(sizeof(TransparentWorkIdentity) == 64);
    static_assert(std::is_trivially_copyable_v<TransparentWorkIdentity>);

    enum TransparentWorkFlag : uint32_t {
        TransparentWorkIntervalValid = 1u << 0,
        TransparentWorkNearClipped = 1u << 1,
        TransparentWorkFarClipped = 1u << 2,
        TransparentWorkCameraIntersecting = 1u << 3,
        TransparentWorkMirrored = 1u << 4,
        TransparentWorkInvalidBoundsFallback = 1u << 5,
        TransparentWorkCulled = 1u << 6,
    };

    // Exactly 240 bytes. Stable ownership lets renderer work such as probe
    // capture exclude one entity without depending on transient ECS indices.
    // World-space bounds support backend-neutral visibility decisions without
    // requiring scene access from a renderer backend.
    struct alignas(16) DrawPacket {

        // --- CHUNK 1: The Transform (64 Bytes) ---
        glm::mat4 worldTransform;     // Offsets 0 - 64

        // --- SORT KEY & TICKETS (28 Bytes) ---
        uint64_t opaqueSortKey;       // Offsets 64 - 72
        GeometryHandle geometry;      // Offsets 72 - 76
        MaterialHandle material;      // Offsets 76 - 80
        PipelineHandle pipeline;      // Offsets 80 - 84
        uint32_t indexCount;          // Offsets 84 - 88
        uint32_t firstIndex;          // Offsets 88 - 92

        // --- STATE (36 Bytes) ---
        float distanceToCamera;       // Offsets 92 - 96
        uint32_t isSelected = 0;      // Offsets 96 - 100 (Use uint32 instead of bool!)
        glm::vec3 boundsSphereCenterWorld{}; // Offsets 100 - 112
        float boundsSphereRadiusWorld = -1.0f; // Offsets 112 - 116; negative means unknown
        float transparentNearDepth = 0.0f; // Offsets 116 - 120
        float transparentFarDepth = 0.0f;  // Offsets 120 - 124
        uint32_t transparentWorkFlags = 0;  // Offsets 124 - 128
        SceneEntityUuid owner;        // Offsets 128 - 144

        // Stable transparent-work identity. The source primitive survives
        // connected-component splitting; primitive identifies the exact
        // cooked work item and material identifies its effective binding.
        AssetGuid sourcePrimitiveGuid; // Offsets 144 - 160
        AssetGuid primitiveGuid;       // Offsets 160 - 176
        AssetGuid materialGuid;        // Offsets 176 - 192

        glm::vec3 boundsMinWorld{};    // Offsets 192 - 204
        glm::vec3 boundsMaxWorld{};    // Offsets 204 - 216
        CompiledTransparencyPolicy transparency; // Offsets 216 - 228
        TransparencyExecutionMode transparencyExecutionMode =
            TransparencyExecutionMode::LegacyTwoBucket; // Offset 228
        uint8_t coverage = 0;          // Offset 229
        uint8_t _padding[2]{};         // Offsets 230 - 232
    };

    static_assert(sizeof(DrawPacket) == 240);
    static_assert(alignof(DrawPacket) == 16);
    static_assert(std::is_trivially_copyable_v<DrawPacket>);

    [[nodiscard]] constexpr TransparentWorkIdentity transparentWorkIdentity(
        const DrawPacket& packet) noexcept {
        return {
            .owner = packet.owner,
            .sourcePrimitiveGuid = packet.sourcePrimitiveGuid,
            .primitiveGuid = packet.primitiveGuid,
            .materialGuid = packet.materialGuid,
        };
    }

    [[nodiscard]] inline bool prepareTransparentWorkInterval(
        DrawPacket& packet, const glm::vec3& localBoundsMin,
        const glm::vec3& localBoundsMax, const glm::mat4& view,
        float nearPlane, float farPlane) noexcept {
        const auto finite3 = [](const glm::vec3& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        };
        const auto invalid = [&]() {
            packet.transparentNearDepth = 0.0f;
            packet.transparentFarDepth = 0.0f;
            packet.boundsMinWorld = {};
            packet.boundsMaxWorld = {};
            packet.transparentWorkFlags =
                TransparentWorkInvalidBoundsFallback;
            return true;
        };
        if (!finite3(localBoundsMin) || !finite3(localBoundsMax) ||
            glm::any(glm::greaterThan(localBoundsMin, localBoundsMax)) ||
            !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
            nearPlane <= 0.0f || farPlane <= nearPlane) {
            return invalid();
        }

        glm::vec3 worldMin(std::numeric_limits<float>::max());
        glm::vec3 worldMax(std::numeric_limits<float>::lowest());
        float depthMin = std::numeric_limits<float>::max();
        float depthMax = std::numeric_limits<float>::lowest();
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{
                (corner & 1u) != 0 ? localBoundsMax.x : localBoundsMin.x,
                (corner & 2u) != 0 ? localBoundsMax.y : localBoundsMin.y,
                (corner & 4u) != 0 ? localBoundsMax.z : localBoundsMin.z,
            };
            const glm::vec4 world = packet.worldTransform * glm::vec4(local, 1.0f);
            const glm::vec4 viewPosition = view * world;
            if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
                !std::isfinite(world.z) || !std::isfinite(world.w) ||
                !std::isfinite(viewPosition.z) || std::abs(world.w) <= 1.0e-8f) {
                return invalid();
            }
            const glm::vec3 point = glm::vec3(world) / world.w;
            worldMin = glm::min(worldMin, point);
            worldMax = glm::max(worldMax, point);
            const float depth = -viewPosition.z / world.w;
            depthMin = (std::min)(depthMin, depth);
            depthMax = (std::max)(depthMax, depth);
        }
        if (!finite3(worldMin) || !finite3(worldMax) ||
            !std::isfinite(depthMin) || !std::isfinite(depthMax)) {
            return invalid();
        }
        packet.boundsMinWorld = worldMin;
        packet.boundsMaxWorld = worldMax;
        packet.transparentWorkFlags = TransparentWorkIntervalValid;
        const float determinant = glm::determinant(glm::mat3(packet.worldTransform));
        if (std::isfinite(determinant) && determinant < 0.0f)
            packet.transparentWorkFlags |= TransparentWorkMirrored;
        if (depthMax < nearPlane || depthMin > farPlane) {
            packet.transparentWorkFlags |= TransparentWorkCulled;
            return false;
        }
        if (depthMin < nearPlane) {
            packet.transparentWorkFlags |= TransparentWorkNearClipped |
                TransparentWorkCameraIntersecting;
        }
        if (depthMax > farPlane)
            packet.transparentWorkFlags |= TransparentWorkFarClipped;
        packet.transparentNearDepth = (std::max)(depthMin, nearPlane);
        packet.transparentFarDepth = (std::min)(depthMax, farPlane);
        return true;
    }

    [[nodiscard]] inline bool transparentWorkLess(
        const DrawPacket& lhs, const DrawPacket& rhs) noexcept {
        if (lhs.transparency.priority != rhs.transparency.priority)
            return lhs.transparency.priority < rhs.transparency.priority;
        const bool lhsValid = (lhs.transparentWorkFlags &
            TransparentWorkIntervalValid) != 0;
        const bool rhsValid = (rhs.transparentWorkFlags &
            TransparentWorkIntervalValid) != 0;
        if (lhsValid != rhsValid) return lhsValid;
        if (lhsValid) {
            const bool lhsIntersects = (lhs.transparentWorkFlags &
                TransparentWorkCameraIntersecting) != 0;
            const bool rhsIntersects = (rhs.transparentWorkFlags &
                TransparentWorkCameraIntersecting) != 0;
            if (lhsIntersects != rhsIntersects) return !lhsIntersects;
            if (lhs.transparentFarDepth != rhs.transparentFarDepth)
                return lhs.transparentFarDepth > rhs.transparentFarDepth;
            if (lhs.transparentNearDepth != rhs.transparentNearDepth)
                return lhs.transparentNearDepth > rhs.transparentNearDepth;
        }
        return transparentWorkIdentity(lhs) < transparentWorkIdentity(rhs);
    }

    // The retained compatibility renderer may receive both legacy work and
    // classified classes whose dedicated execution has not landed yet. Preserve
    // the frozen legacy ordering inside LegacyTwoBucket, but never throw away the
    // stable interval/identity order already computed for classified work.
    [[nodiscard]] inline bool transparentCompatibilityLess(
        const DrawPacket& lhs, const DrawPacket& rhs) noexcept {
        if (lhs.transparencyExecutionMode != rhs.transparencyExecutionMode) {
            return lhs.transparencyExecutionMode < rhs.transparencyExecutionMode;
        }
        if (lhs.transparencyExecutionMode ==
                TransparencyExecutionMode::Classified) {
            return transparentWorkLess(lhs, rhs);
        }
        if (lhs.distanceToCamera != rhs.distanceToCamera)
            return lhs.distanceToCamera > rhs.distanceToCamera;
        if (lhs.pipeline != rhs.pipeline) return lhs.pipeline < rhs.pipeline;
        if (lhs.material != rhs.material) return lhs.material < rhs.material;
        return lhs.geometry < rhs.geometry;
    }

    [[nodiscard]] inline uint64_t countAmbiguousTransparentIntervals(
        std::span<const DrawPacket> sortedWork) noexcept {
        uint64_t count = 0;
        for (size_t outer = 0; outer < sortedWork.size(); ++outer) {
            const DrawPacket& lhs = sortedWork[outer];
            if ((lhs.transparentWorkFlags & TransparentWorkIntervalValid) == 0)
                continue;
            for (size_t inner = outer + 1; inner < sortedWork.size(); ++inner) {
                const DrawPacket& rhs = sortedWork[inner];
                if (rhs.transparency.priority != lhs.transparency.priority)
                    continue;
                if ((rhs.transparentWorkFlags &
                        TransparentWorkIntervalValid) == 0)
                    continue;
                if (lhs.transparentNearDepth <= rhs.transparentFarDepth &&
                    rhs.transparentNearDepth <= lhs.transparentFarDepth)
                    ++count;
            }
        }
        return count;
    }

    struct TransparentIntervalEndpoint {
        float nearDepth = 0.0f;
        float farDepth = 0.0f;
    };

    // Exact O(n log n) interval sweep using caller-owned scratch. Work may be
    // sorted by the render order; the sweep independently orders endpoints by
    // far depth inside each author-priority group.
    [[nodiscard]] inline uint64_t sweepAmbiguousTransparentIntervals(
        std::span<const DrawPacket> sortedWork,
        std::span<TransparentIntervalEndpoint> endpointScratch,
        std::span<float> nearScratch,
        std::span<uint32_t> fenwickScratch) {
        if (endpointScratch.size() < sortedWork.size() ||
            nearScratch.size() < sortedWork.size() ||
            fenwickScratch.size() < sortedWork.size() + 1u) {
            return countAmbiguousTransparentIntervals(sortedWork);
        }

        uint64_t count = 0;
        size_t groupBegin = 0;
        while (groupBegin < sortedWork.size()) {
            const int32_t priority =
                sortedWork[groupBegin].transparency.priority;
            size_t groupEnd = groupBegin + 1;
            while (groupEnd < sortedWork.size() &&
                sortedWork[groupEnd].transparency.priority == priority) {
                ++groupEnd;
            }

            size_t endpointCount = 0;
            for (size_t index = groupBegin; index < groupEnd; ++index) {
                const DrawPacket& packet = sortedWork[index];
                if ((packet.transparentWorkFlags &
                        TransparentWorkIntervalValid) == 0) {
                    continue;
                }
                endpointScratch[endpointCount++] = {
                    .nearDepth = packet.transparentNearDepth,
                    .farDepth = packet.transparentFarDepth,
                };
            }
            auto endpoints = endpointScratch.first(endpointCount);
            std::ranges::sort(endpoints,
                [](const TransparentIntervalEndpoint& lhs,
                    const TransparentIntervalEndpoint& rhs) {
                    if (lhs.farDepth != rhs.farDepth)
                        return lhs.farDepth > rhs.farDepth;
                    return lhs.nearDepth > rhs.nearDepth;
                });
            for (size_t index = 0; index < endpointCount; ++index)
                nearScratch[index] = endpoints[index].nearDepth;
            auto nearValues = nearScratch.first(endpointCount);
            std::ranges::sort(nearValues);
            const auto uniqueEnd = std::ranges::unique(nearValues).begin();
            const size_t uniqueCount = static_cast<size_t>(
                uniqueEnd - nearValues.begin());
            std::ranges::fill(fenwickScratch.first(uniqueCount + 1u), 0u);

            for (const TransparentIntervalEndpoint endpoint : endpoints) {
                size_t prefix = static_cast<size_t>(std::upper_bound(
                    nearValues.begin(), nearValues.begin() + uniqueCount,
                    endpoint.farDepth) - nearValues.begin());
                uint64_t overlapping = 0;
                while (prefix != 0) {
                    overlapping += fenwickScratch[prefix];
                    prefix &= prefix - 1u;
                }
                count += overlapping;

                size_t rank = static_cast<size_t>(std::lower_bound(
                    nearValues.begin(), nearValues.begin() + uniqueCount,
                    endpoint.nearDepth) - nearValues.begin()) + 1u;
                while (rank <= uniqueCount) {
                    ++fenwickScratch[rank];
                    rank += rank & (~rank + 1u);
                }
            }
            groupBegin = groupEnd;
        }
        return count;
    }

} // namespace Iridium
