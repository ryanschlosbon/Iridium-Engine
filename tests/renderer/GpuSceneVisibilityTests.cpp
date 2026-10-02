#include "renderer/rhi/GpuSceneVisibility.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <iostream>
#include <limits>

using namespace Iridium;

namespace {
    int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::cerr << "check failed: " #expression " (line " << __LINE__ << ")\n"; \
    ++failures; } } while (false)

    GpuScenePackedTables sceneAt(float x, float primitiveOffset = 0.0f) {
        GpuScenePackedTables scene;
        scene.transforms.push_back(packGpuSceneAffine(
            glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.5f))));
        scene.instances.push_back({
            .worldBoundsSphere = { x, 0.0f, 0.5f, 0.25f },
            .worldBoundsMin = { x - 0.25f, -0.25f, 0.25f, 0.0f },
            .worldBoundsMax = { x + 0.25f, 0.25f, 0.75f, 0.0f },
            .references = { 0, 0, 0, 1 },
            .state = { 1, 0, GpuSceneInstanceEnabled,
                GpuSceneConsumerMainOpaque },
        });
        scene.primitives.push_back({
            .binding = { 0, 0, 1, 1 },
            .state = { 1, GpuScenePrimitiveOpaque, 0,
                GpuSceneConsumerMainOpaque },
        });
        scene.geometries.push_back({
            .localBoundsSphere = { primitiveOffset, 0, 0, 0.1f },
            .localBoundsMin = { primitiveOffset - 0.1f, -0.1f, -0.1f, 0 },
            .localBoundsMax = { primitiveOffset + 0.1f, 0.1f, 0.1f, 0 },
            .draw = { 0, 300, 0, 0 },
        });
        return scene;
    }

    void identityVulkanClipVolume() {
        const GpuSceneFrustum frustum = makeGpuSceneFrustum(glm::mat4(1.0f));
        CHECK(frustum.valid);
        GpuSceneVisibilityResult result;
        const GpuScenePackedTables inside = sceneAt(0.0f);
        classifyGpuSceneFrustum(inside, frustum,
            GpuSceneConsumerMainOpaque, result);
        CHECK(result.stats.visibleInstances == 1);
        CHECK(result.stats.visiblePrimitives == 1);
        CHECK(result.stats.visibleTriangles == 100);
        CHECK(result.primitiveVisibility.size() == 1u);
        CHECK(result.primitiveVisibility[0] == 1u);

        const GpuScenePackedTables outside = sceneAt(2.0f);
        classifyGpuSceneFrustum(outside, frustum,
            GpuSceneConsumerMainOpaque, result);
        CHECK(result.stats.frustumRejectedInstances == 1);
        CHECK(result.stats.requestedPrimitives == 1);
        CHECK(result.stats.frustumRejectedPrimitives == 1);
        CHECK(result.visiblePrimitiveIndices.empty());
        CHECK(result.primitiveVisibility[0] == 0u);
    }

    void primitiveSecondStage() {
        GpuScenePackedTables scene = sceneAt(0.0f, 3.0f);
        const GpuSceneFrustum frustum = makeGpuSceneFrustum(glm::mat4(1.0f));
        GpuSceneVisibilityResult result;
        classifyGpuSceneFrustum(scene, frustum,
            GpuSceneConsumerMainOpaque, result);
        CHECK(result.stats.visibleInstances == 1);
        CHECK(result.stats.frustumRejectedPrimitives == 1);
        CHECK(result.stats.requestedTriangles == 100);
        CHECK(result.stats.visibleTriangles == 0);
    }

    void invalidDataFailsVisible() {
        GpuScenePackedTables scene = sceneAt(4.0f);
        scene.instances[0].state.z |= GpuSceneInstanceInvalidBoundsFailVisible;
        scene.instances[0].worldBoundsMin.x =
            std::numeric_limits<float>::quiet_NaN();
        GpuSceneVisibilityResult result;
        classifyGpuSceneFrustum(scene, makeGpuSceneFrustum(glm::mat4(1.0f)),
            GpuSceneConsumerMainOpaque, result);
        CHECK(result.stats.failVisibleInstances == 1);
        CHECK(result.stats.visiblePrimitives == 1);

        glm::mat4 malformed(0.0f);
        CHECK(!makeGpuSceneFrustum(malformed).valid);
        classifyGpuSceneFrustum(scene, makeGpuSceneFrustum(malformed),
            GpuSceneConsumerMainOpaque, result);
        CHECK(result.stats.visibleInstances == 1);
    }

    void consumerMaskAndDeterminism() {
        const GpuScenePackedTables scene = sceneAt(0.0f);
        const GpuSceneFrustum frustum = makeGpuSceneFrustum(glm::mat4(1.0f));
        GpuSceneVisibilityResult first, second;
        classifyGpuSceneFrustum(scene, frustum, GpuSceneConsumerShadow, first);
        CHECK(first.stats.requestedInstances == 0);
        classifyGpuSceneFrustum(scene, frustum,
            GpuSceneConsumerMainOpaque, first);
        classifyGpuSceneFrustum(scene, frustum,
            GpuSceneConsumerMainOpaque, second);
        CHECK(first.visibleInstanceIndices == second.visibleInstanceIndices);
        CHECK(first.visiblePrimitiveIndices == second.visiblePrimitiveIndices);
        CHECK(first.abiVersion == GpuSceneVisibilityAbiVersion);
    }
}

int main() {
    identityVulkanClipVolume();
    primitiveSecondStage();
    invalidDataFailsVisible();
    consumerMaskAndDeterminism();
    if (failures == 0) std::cout << "GpuSceneVisibilityTests passed\n";
    return failures == 0 ? 0 : 1;
}
