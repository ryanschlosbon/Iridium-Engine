#pragma once

#include "GpuScene.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t GpuSceneVisibilityAbiVersion = 1;

    struct GpuSceneFrustum {
        std::array<glm::vec4, 6> planes{};
        bool valid = false;
    };

    struct GpuSceneVisibilityStats {
        uint32_t requestedInstances = 0;
        uint32_t visibleInstances = 0;
        uint32_t frustumRejectedInstances = 0;
        uint32_t requestedPrimitives = 0;
        uint32_t visiblePrimitives = 0;
        uint32_t frustumRejectedPrimitives = 0;
        uint32_t failVisibleInstances = 0;
        uint32_t failVisiblePrimitives = 0;
        uint64_t requestedTriangles = 0;
        uint64_t visibleTriangles = 0;
    };

    struct GpuSceneVisibilityResult {
        uint32_t abiVersion = GpuSceneVisibilityAbiVersion;
        std::vector<uint32_t> visibleInstanceIndices;
        std::vector<uint32_t> visiblePrimitiveIndices;
        std::vector<uint8_t> primitiveVisibility;
        GpuSceneVisibilityStats stats;
    };

    // Extracts Vulkan's x/y [-w,+w], z [0,+w] clip volume. A malformed matrix
    // produces an invalid frustum and the classifier fails visible.
    [[nodiscard]] GpuSceneFrustum makeGpuSceneFrustum(
        const glm::mat4& clipFromWorld) noexcept;

    // Conservative CPU oracle for the GPU implementation. Instance AABBs reject
    // coarse work first; primitive local AABBs are then transformed exactly to a
    // conservative world AABB. Invalid references or bounds fail visible.
    void classifyGpuSceneFrustum(const GpuScenePackedTables& scene,
        const GpuSceneFrustum& frustum, uint32_t consumerMask,
        GpuSceneVisibilityResult& result);

} // namespace Iridium
