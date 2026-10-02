#include "GpuSceneVisibility.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Iridium {
namespace {

    [[nodiscard]] glm::vec4 row(const glm::mat4& matrix,
        uint32_t index) noexcept {
        return { matrix[0][index], matrix[1][index],
            matrix[2][index], matrix[3][index] };
    }

    [[nodiscard]] bool finite(const glm::vec3& value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    [[nodiscard]] bool validBounds(const glm::vec3& minimum,
        const glm::vec3& maximum) noexcept {
        return finite(minimum) && finite(maximum) &&
            minimum.x <= maximum.x && minimum.y <= maximum.y &&
            minimum.z <= maximum.z;
    }

    [[nodiscard]] bool rejected(const GpuSceneFrustum& frustum,
        const glm::vec3& minimum, const glm::vec3& maximum) noexcept {
        const glm::vec3 center = (minimum + maximum) * 0.5f;
        const glm::vec3 extent = (maximum - minimum) * 0.5f;
        for (const glm::vec4& plane : frustum.planes) {
            const glm::vec3 normal(plane);
            const float radius = glm::dot(glm::abs(normal), extent);
            if (glm::dot(normal, center) + plane.w + radius < 0.0f)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool transformedBounds(
        const GpuSceneAffineTransform& packed,
        const GpuSceneGeometryRecord& geometry,
        glm::vec3& minimum, glm::vec3& maximum) noexcept {
        const glm::vec3 localMinimum{
            geometry.localBoundsMin.x, geometry.localBoundsMin.y,
            geometry.localBoundsMin.z };
        const glm::vec3 localMaximum{
            geometry.localBoundsMax.x, geometry.localBoundsMax.y,
            geometry.localBoundsMax.z };
        if (!validBounds(localMinimum, localMaximum)) return false;
        const glm::mat4 world = unpackGpuSceneAffine(packed);
        const glm::vec3 localCenter = (localMinimum + localMaximum) * 0.5f;
        const glm::vec3 localExtent = (localMaximum - localMinimum) * 0.5f;
        const glm::vec3 worldCenter = glm::vec3(world * glm::vec4(localCenter, 1.0f));
        glm::mat3 absoluteLinear(world);
        absoluteLinear[0] = glm::abs(absoluteLinear[0]);
        absoluteLinear[1] = glm::abs(absoluteLinear[1]);
        absoluteLinear[2] = glm::abs(absoluteLinear[2]);
        const glm::vec3 worldExtent = absoluteLinear * localExtent;
        minimum = worldCenter - worldExtent;
        maximum = worldCenter + worldExtent;
        return validBounds(minimum, maximum);
    }

} // namespace

    GpuSceneFrustum makeGpuSceneFrustum(
        const glm::mat4& clipFromWorld) noexcept {
        GpuSceneFrustum result;
        const glm::vec4 x = row(clipFromWorld, 0);
        const glm::vec4 y = row(clipFromWorld, 1);
        const glm::vec4 z = row(clipFromWorld, 2);
        const glm::vec4 w = row(clipFromWorld, 3);
        result.planes = { w + x, w - x, w + y, w - y, z, w - z };
        for (glm::vec4& plane : result.planes) {
            const float length = glm::length(glm::vec3(plane));
            if (!std::isfinite(length) || length <=
                    std::numeric_limits<float>::epsilon() ||
                !std::isfinite(plane.w)) {
                result.valid = false;
                return result;
            }
            plane /= length;
        }
        result.valid = true;
        return result;
    }

    void classifyGpuSceneFrustum(const GpuScenePackedTables& scene,
        const GpuSceneFrustum& frustum, uint32_t consumerMask,
        GpuSceneVisibilityResult& result) {
        result.abiVersion = GpuSceneVisibilityAbiVersion;
        result.visibleInstanceIndices.clear();
        result.visiblePrimitiveIndices.clear();
        result.primitiveVisibility.assign(scene.primitives.size(), 0u);
        result.stats = {};

        for (uint32_t instanceIndex = 0;
            instanceIndex < scene.instances.size(); ++instanceIndex) {
            const GpuSceneInstanceRecord& instance = scene.instances[instanceIndex];
            if ((instance.state.w & consumerMask) == 0 ||
                (instance.state.z & GpuSceneInstanceEnabled) == 0)
                continue;
            ++result.stats.requestedInstances;

            const bool referencesValid = instance.references.x <
                    scene.transforms.size() &&
                instance.references.z <= scene.primitives.size() &&
                instance.references.w <= scene.primitives.size() -
                    instance.references.z;
            const glm::vec3 instanceMinimum{
                instance.worldBoundsMin.x, instance.worldBoundsMin.y,
                instance.worldBoundsMin.z };
            const glm::vec3 instanceMaximum{
                instance.worldBoundsMax.x, instance.worldBoundsMax.y,
                instance.worldBoundsMax.z };
            const bool failVisible = !frustum.valid || !referencesValid ||
                (instance.state.z & GpuSceneInstanceInvalidBoundsFailVisible) != 0 ||
                !validBounds(instanceMinimum, instanceMaximum);
            uint32_t instancePrimitiveCount = 0;
            uint64_t instanceTriangleCount = 0;
            if (referencesValid) {
                for (uint32_t offset = 0; offset < instance.references.w; ++offset) {
                    const GpuScenePrimitiveRecord& primitive =
                        scene.primitives[instance.references.z + offset];
                    if ((primitive.state.w & consumerMask) == 0) continue;
                    ++instancePrimitiveCount;
                    if (primitive.binding.y < scene.geometries.size())
                        instanceTriangleCount +=
                            scene.geometries[primitive.binding.y].draw.y / 3u;
                }
                result.stats.requestedPrimitives += instancePrimitiveCount;
                result.stats.requestedTriangles += instanceTriangleCount;
            }
            if (!failVisible && rejected(frustum, instanceMinimum,
                    instanceMaximum)) {
                ++result.stats.frustumRejectedInstances;
                result.stats.frustumRejectedPrimitives += instancePrimitiveCount;
                continue;
            }
            result.visibleInstanceIndices.push_back(instanceIndex);
            ++result.stats.visibleInstances;
            if (failVisible) ++result.stats.failVisibleInstances;
            if (!referencesValid) continue;

            for (uint32_t offset = 0; offset < instance.references.w; ++offset) {
                const uint32_t primitiveIndex = instance.references.z + offset;
                const GpuScenePrimitiveRecord& primitive =
                    scene.primitives[primitiveIndex];
                if ((primitive.state.w & consumerMask) == 0) continue;
                const bool geometryValid = primitive.binding.x == instanceIndex &&
                    primitive.binding.y < scene.geometries.size();
                const GpuSceneGeometryRecord* geometry = geometryValid
                    ? &scene.geometries[primitive.binding.y] : nullptr;
                glm::vec3 minimum, maximum;
                const bool primitiveFailVisible = failVisible || !geometry ||
                    !transformedBounds(scene.transforms[instance.references.x],
                        *geometry, minimum, maximum);
                if (!primitiveFailVisible && rejected(frustum, minimum, maximum)) {
                    ++result.stats.frustumRejectedPrimitives;
                    continue;
                }
                result.visiblePrimitiveIndices.push_back(primitiveIndex);
                result.primitiveVisibility[primitiveIndex] = 1u;
                ++result.stats.visiblePrimitives;
                if (primitiveFailVisible) ++result.stats.failVisiblePrimitives;
                if (geometry)
                    result.stats.visibleTriangles += geometry->draw.y / 3u;
            }
        }
    }

} // namespace Iridium
