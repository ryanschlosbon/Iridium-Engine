#include "renderer/rhi/GpuSceneVisibility.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

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

    bool sameStats(const GpuSceneVisibilityStats& a,
        const GpuSceneVisibilityStats& b) {
        return a.requestedInstances == b.requestedInstances &&
            a.visibleInstances == b.visibleInstances &&
            a.frustumRejectedInstances == b.frustumRejectedInstances &&
            a.requestedPrimitives == b.requestedPrimitives &&
            a.visiblePrimitives == b.visiblePrimitives &&
            a.frustumRejectedPrimitives == b.frustumRejectedPrimitives &&
            a.failVisibleInstances == b.failVisibleInstances &&
            a.failVisiblePrimitives == b.failVisiblePrimitives &&
            a.requestedTriangles == b.requestedTriangles &&
            a.visibleTriangles == b.visibleTriangles;
    }

    // M7R R5c.7: classifying consecutive instance ranges and merging the
    // parts in range order equals the whole-scene classification.
    void rangeClassificationMatchesWhole() {
        std::mt19937 random(7);
        std::uniform_real_distribution<float> position(-30.0f, 30.0f);
        std::uniform_real_distribution<float> extent(0.05f, 2.0f);
        for (uint32_t trial = 0; trial < 40; ++trial) {
            GpuScenePackedTables scene;
            const uint32_t instanceCount = 1u + random() % 300u;
            for (uint32_t instance = 0; instance < instanceCount; ++instance) {
                const glm::vec3 center(position(random), position(random),
                    position(random));
                const float radius = extent(random);
                scene.transforms.push_back(packGpuSceneAffine(
                    glm::translate(glm::mat4(1.0f), center)));
                const uint32_t primitives = random() % 6u;
                const uint32_t firstPrimitive =
                    static_cast<uint32_t>(scene.primitives.size());
                uint32_t state = (random() % 8u != 0u) ? GpuSceneInstanceEnabled : 0u;
                if (random() % 25u == 0u)
                    state |= GpuSceneInstanceInvalidBoundsFailVisible;
                GpuSceneInstanceRecord record{
                    .worldBoundsSphere = { center.x, center.y, center.z, radius },
                    .worldBoundsMin = { center.x - radius, center.y - radius,
                        center.z - radius, 0.0f },
                    .worldBoundsMax = { center.x + radius, center.y + radius,
                        center.z + radius, 0.0f },
                    .references = { instance, 0, firstPrimitive, primitives },
                    .state = { 1, 0, state, (random() % 5u == 0u)
                        ? GpuSceneConsumerShadow
                        : GpuSceneConsumerMainOpaque | GpuSceneConsumerForwardOpaque },
                };
                if (random() % 40u == 0u) record.references.x = 1'000'000u;
                scene.instances.push_back(record);
                for (uint32_t primitive = 0; primitive < primitives; ++primitive) {
                    const uint32_t geometry =
                        static_cast<uint32_t>(scene.geometries.size());
                    scene.primitives.push_back({
                        .binding = { instance, random() % 30u == 0u
                            ? 1'000'000u : geometry, 1, 1 },
                        .state = { 1, GpuScenePrimitiveOpaque, 0,
                            random() % 2u == 0u ? GpuSceneConsumerMainOpaque
                                : GpuSceneConsumerForwardOpaque },
                    });
                    const float offset = position(random) * 0.05f;
                    const float half = extent(random) * 0.5f;
                    scene.geometries.push_back({
                        .localBoundsSphere = { offset, 0, 0, half },
                        .localBoundsMin = { offset - half, -half, -half, 0 },
                        .localBoundsMax = { offset + half, half, half, 0 },
                        .draw = { 0, 3u * (1u + random() % 500u), 0, 0 },
                    });
                }
            }
            const glm::mat4 clip = glm::perspective(glm::radians(60.0f),
                16.0f / 9.0f, 0.1f, 40.0f) * glm::lookAt(
                    glm::vec3(position(random), 2.0f, position(random)),
                    glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            const GpuSceneFrustum frustum = makeGpuSceneFrustum(clip);
            const uint32_t mask = GpuSceneConsumerMainOpaque |
                GpuSceneConsumerForwardOpaque;
            GpuSceneVisibilityResult whole;
            classifyGpuSceneFrustum(scene, frustum, mask, whole);
            for (const uint32_t ranges : { 1u, 2u, 3u, 7u, 32u, instanceCount }) {
                GpuSceneVisibilityResult merged;
                beginGpuSceneVisibility(scene, merged);
                std::vector<GpuSceneVisibilityPart> parts(ranges);
                for (uint32_t range = 0; range < ranges; ++range) {
                    classifyGpuSceneFrustumRange(scene, frustum, mask,
                        instanceCount * range / ranges,
                        instanceCount * (range + 1u) / ranges,
                        merged.primitiveVisibility, parts[range]);
                }
                for (const GpuSceneVisibilityPart& part : parts)
                    mergeGpuSceneVisibility(part, merged);
                CHECK(merged.visibleInstanceIndices == whole.visibleInstanceIndices);
                CHECK(merged.visiblePrimitiveIndices == whole.visiblePrimitiveIndices);
                CHECK(merged.primitiveVisibility == whole.primitiveVisibility);
                CHECK(sameStats(merged.stats, whole.stats));
            }
        }
    }
}

namespace {
    // M7.10.1: the public box-plane test (transparent culling) matches the
    // classifier's instance test, rejects only boxes wholly outside one plane
    // and fails visible on an invalid frustum or invalid bounds.
    void publicAabbRejection() {
        const GpuSceneFrustum identity = makeGpuSceneFrustum(glm::mat4(1.0f));
        CHECK(!gpuSceneFrustumRejectsAabb(identity,
            { -0.5f, -0.5f, 0.25f }, { 0.5f, 0.5f, 0.75f }));
        CHECK(gpuSceneFrustumRejectsAabb(identity,
            { 1.5f, -0.5f, 0.25f }, { 2.0f, 0.5f, 0.75f }));       // x > w
        CHECK(gpuSceneFrustumRejectsAabb(identity,
            { -0.5f, -2.0f, 0.25f }, { 0.5f, -1.5f, 0.75f }));     // y < -w
        CHECK(gpuSceneFrustumRejectsAabb(identity,
            { -0.5f, -0.5f, -1.0f }, { 0.5f, 0.5f, -0.5f }));      // z < 0
        CHECK(gpuSceneFrustumRejectsAabb(identity,
            { -0.5f, -0.5f, 1.5f }, { 0.5f, 0.5f, 2.0f }));        // z > w
        CHECK(!gpuSceneFrustumRejectsAabb(identity,
            { 0.9f, -0.5f, 0.25f }, { 1.5f, 0.5f, 0.75f }));       // straddles
        // Outside two planes' corner region but no single plane: kept.
        CHECK(!gpuSceneFrustumRejectsAabb(identity,
            { 0.5f, 0.5f, 0.25f }, { 1.5f, 1.5f, 0.75f }));

        // Fail visible.
        const GpuSceneFrustum invalid = makeGpuSceneFrustum(glm::mat4(0.0f));
        CHECK(!invalid.valid);
        CHECK(!gpuSceneFrustumRejectsAabb(invalid,
            { 1.5f, -0.5f, 0.25f }, { 2.0f, 0.5f, 0.75f }));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        CHECK(!gpuSceneFrustumRejectsAabb(identity,
            { nan, -0.5f, 0.25f }, { 2.0f, 0.5f, 0.75f }));
        CHECK(!gpuSceneFrustumRejectsAabb(identity,
            { 2.0f, -0.5f, 0.25f }, { 1.5f, 0.5f, 0.75f }));       // inverted

        // Same decision as the classifier's instance stage.
        for (const float x : { -2.0f, -1.1f, -0.9f, 0.0f, 0.9f, 1.1f, 2.0f }) {
            const GpuScenePackedTables scene = sceneAt(x);
            GpuSceneVisibilityResult result;
            classifyGpuSceneFrustum(scene, identity,
                GpuSceneConsumerMainOpaque, result);
            const GpuSceneInstanceRecord& instance = scene.instances[0];
            const glm::vec3 minimum{ instance.worldBoundsMin.x,
                instance.worldBoundsMin.y, instance.worldBoundsMin.z };
            const glm::vec3 maximum{ instance.worldBoundsMax.x,
                instance.worldBoundsMax.y, instance.worldBoundsMax.z };
            CHECK(gpuSceneFrustumRejectsAabb(identity, minimum, maximum) ==
                (result.stats.frustumRejectedInstances == 1u));
        }

        // Conservative under a perspective view: a rejected box has no
        // corner inside the clip volume.
        std::mt19937 random(0x7f10u);
        std::uniform_real_distribution<float> position(-30.0f, 30.0f);
        std::uniform_real_distribution<float> extent(0.01f, 6.0f);
        const glm::mat4 clip = glm::perspective(glm::radians(60.0f),
            16.0f / 9.0f, 0.1f, 40.0f) * glm::lookAt(glm::vec3(1.0f, 2.0f, 3.0f),
                glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const GpuSceneFrustum frustum = makeGpuSceneFrustum(clip);
        CHECK(frustum.valid);
        uint32_t rejectedCount = 0;
        for (uint32_t sample = 0; sample < 4096; ++sample) {
            const glm::vec3 minimum{ position(random), position(random),
                position(random) };
            const glm::vec3 maximum = minimum + glm::vec3(extent(random),
                extent(random), extent(random));
            if (!gpuSceneFrustumRejectsAabb(frustum, minimum, maximum)) continue;
            ++rejectedCount;
            for (uint32_t corner = 0; corner < 8; ++corner) {
                const glm::vec4 point = clip * glm::vec4(
                    (corner & 1u) ? maximum.x : minimum.x,
                    (corner & 2u) ? maximum.y : minimum.y,
                    (corner & 4u) ? maximum.z : minimum.z, 1.0f);
                const bool inside = point.w > 0.0f &&
                    std::abs(point.x) <= point.w && std::abs(point.y) <= point.w &&
                    point.z >= 0.0f && point.z <= point.w;
                CHECK(!inside);
            }
        }
        CHECK(rejectedCount > 0u);
    }
}

int main() {
    identityVulkanClipVolume();
    primitiveSecondStage();
    invalidDataFailsVisible();
    consumerMaskAndDeterminism();
    rangeClassificationMatchesWhole();
    publicAabbRejection();
    if (failures == 0) std::cout << "GpuSceneVisibilityTests passed\n";
    return failures == 0 ? 0 : 1;
}
