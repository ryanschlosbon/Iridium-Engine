#include "renderer/rhi/GpuSceneLod.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace Iridium;

namespace {
    int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::cerr << "check failed: " #expression " (line " << __LINE__ << ")\n"; \
    ++failures; } } while (false)

    constexpr float Pi = 3.14159265358979323846f;
    const std::array<GpuSceneLodLevel, 3> Levels{{
        { 10u, 0.0f, 10'000u, true },
        { 11u, 0.01f, 4'000u, true },
        { 12u, 0.04f, 1'000u, true },
    }};

    GpuSceneLodSelection atDistance(float distance,
        uint32_t previous = InvalidGpuSceneIndex,
        uint32_t maximum = UINT32_MAX) {
        return selectGpuSceneLod({
            .levels = Levels,
            .distanceToBounds = distance,
            .verticalFieldOfViewRadians = Pi / 3.0f,
            .viewportHeightPixels = 1080.0f,
            .targetErrorPixels = 2.0f,
            .hysteresisFraction = 0.15f,
            .previousLod = previous,
            .maximumLod = maximum,
        });
    }

    void projectedSelectionAndHeroLimit() {
        CHECK(atDistance(2.0f).selectedLod == 0u);
        CHECK(atDistance(10.0f).selectedLod == 1u);
        CHECK(atDistance(40.0f).selectedLod == 2u);
        const GpuSceneLodSelection hero = atDistance(40.0f,
            InvalidGpuSceneIndex, 0u);
        CHECK(hero.selectedLod == 0u);
        CHECK(hero.geometryIndex == 10u);
        CHECK(hero.triangleCount == 10'000u);
    }

    void hysteresisIsDirectionalAndStable() {
        const GpuSceneLodSelection retainFine = atDistance(20.0f, 1u);
        CHECK(retainFine.selectedLod == 1u);
        CHECK(retainFine.reason == GpuSceneLodReason::HysteresisRetained);
        CHECK(!retainFine.changed);
        CHECK(atDistance(24.0f, 1u).selectedLod == 2u);

        const GpuSceneLodSelection retainCoarse = atDistance(17.0f, 2u);
        CHECK(retainCoarse.selectedLod == 2u);
        CHECK(retainCoarse.reason == GpuSceneLodReason::HysteresisRetained);
        CHECK(atDistance(15.0f, 2u).selectedLod == 1u);
    }

    void residencyFallbackIsExplicit() {
        auto levels = Levels;
        levels[1].resident = false;
        GpuSceneLodSelection selection = selectGpuSceneLod({
            .levels = levels, .distanceToBounds = 10.0f,
            .verticalFieldOfViewRadians = Pi / 3.0f,
            .viewportHeightPixels = 1080.0f,
        });
        CHECK(selection.selectedLod == 2u);
        CHECK(selection.reason ==
            GpuSceneLodReason::CoarserResidentFallback);

        levels[2].resident = false;
        selection = selectGpuSceneLod({
            .levels = levels, .distanceToBounds = 10.0f,
            .verticalFieldOfViewRadians = Pi / 3.0f,
            .viewportHeightPixels = 1080.0f,
        });
        CHECK(selection.selectedLod == 0u);
        CHECK(selection.reason == GpuSceneLodReason::FinerResidentFallback);
    }

    void malformedInputFailsVisibleDeterministically() {
        auto malformed = Levels;
        malformed[1].geometricError =
            std::numeric_limits<float>::quiet_NaN();
        const GpuSceneLodSelection selection = selectGpuSceneLod({
            .levels = malformed, .distanceToBounds = 10.0f,
            .verticalFieldOfViewRadians = Pi / 3.0f,
            .viewportHeightPixels = 1080.0f,
        });
        CHECK(selection.drawable);
        CHECK(selection.selectedLod == 0u);
        CHECK(selection.reason ==
            GpuSceneLodReason::InvalidInputFailVisible);

        for (GpuSceneLodLevel& level : malformed) level.resident = false;
        CHECK(!selectGpuSceneLod({ .levels = malformed }).drawable);
    }

    GpuSceneGeometryRecord box() {
        GpuSceneGeometryRecord geometry;
        geometry.localBoundsMin = { -1, -1, -1, 0 };
        geometry.localBoundsMax = { 1, 1, 1, 0 };
        geometry.draw.y = 3000;
        return geometry;
    }

    void projectionBoundContainsPerturbations() {
        const auto base = box();
        const glm::vec2 viewport(3840, 2160);
        constexpr float error = 0.03f;
        for (uint32_t mode = 0; mode < 5; ++mode) {
            glm::mat4 projection = mode == 4 ? glm::orthoRH_ZO(-12.0f, 12.0f, -8.0f, 8.0f, 0.1f, 100.0f) :
                glm::perspectiveRH_ZO(Pi / 3, viewport.x / viewport.y, 0.1f, 100.0f);
            if (mode == 3) { projection[2][0] = 0.4f; projection[2][1] = -0.3f; }
            glm::mat4 world = glm::translate(glm::mat4(1), glm::vec3(3, 2, -20));
            world = glm::scale(world, glm::vec3(mode == 1 ? -2.0f : 1.0f, 0.5f, 3.0f));
            if (mode == 2) world[1][0] = 4.0f;
            const glm::mat4 clip = projection * world;
            const float bound = boundGpuSceneLodPixelError(base, clip, viewport, error);
            CHECK(std::isfinite(bound) && bound > 0);
            const auto pixels = [&](glm::vec3 point) {
                const glm::vec4 projected = clip * glm::vec4(point, 1);
                return glm::vec2(projected) / projected.w * viewport * 0.5f;
            };
            for (uint32_t corner = 0; corner < 8; ++corner) {
                const glm::vec3 point((corner & 1u) ? 1 : -1,
                    (corner & 2u) ? 1 : -1, (corner & 4u) ? 1 : -1);
                for (int x = -2; x <= 2; ++x)
                    for (int y = -2; y <= 2; ++y)
                        for (int z = -2; z <= 2; ++z) {
                            if (x == 0 && y == 0 && z == 0) continue;
                            const glm::vec3 delta = glm::normalize(glm::vec3(x, y, z)) * error;
                            CHECK(glm::length(pixels(point + delta) - pixels(point)) <= bound);
                        }
            }
            CHECK(boundGpuSceneLodPixelError(base, clip, viewport * 2.0f, error) >= bound * 1.999f);
        }
        const glm::mat4 eyeCrossing = glm::perspectiveRH_ZO(Pi / 3, 1.0f, 0.1f, 100.0f);
        CHECK(std::isinf(boundGpuSceneLodPixelError(base, eyeCrossing, viewport, error)));
        auto small = box();
        small.localBoundsMin = { -0.1f, -0.1f, -0.1f, 0 };
        small.localBoundsMax = { 0.1f, 0.1f, 0.1f, 0 };
        const glm::mat4 grazingNear = eyeCrossing *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -0.15f));
        CHECK(std::isinf(boundGpuSceneLodPixelError(small, grazingNear, viewport, 0.0001f)));
        const glm::mat4 approachingNear = eyeCrossing *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -0.205f));
        CHECK(std::isfinite(boundGpuSceneLodPixelError(small, approachingNear, viewport, 0.001f)));
        CHECK(std::isinf(boundGpuSceneLodPixelError(small, approachingNear, viewport, 0.01f)));
        const glm::mat4 grazingFar = eyeCrossing *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -99.99f));
        CHECK(std::isinf(boundGpuSceneLodPixelError(small, grazingFar, viewport, 0.0001f)));
        glm::mat4 reverseZ = eyeCrossing;
        for (uint32_t column = 0; column < 4; ++column)
            reverseZ[column][2] = eyeCrossing[column][3] - eyeCrossing[column][2];
        CHECK(std::isinf(boundGpuSceneLodPixelError(small, reverseZ *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -0.15f)), viewport, 0.0001f)));
        CHECK(std::isinf(boundGpuSceneLodPixelError(small, reverseZ *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -99.99f)), viewport, 0.0001f)));
        CHECK(std::isinf(boundGpuSceneLodPixelError(base, glm::mat4(1), glm::vec2(0), error)));
        auto malformed = base;
        malformed.localBoundsMin.x = 2;
        CHECK(std::isinf(boundGpuSceneLodPixelError(malformed, glm::mat4(1), viewport, error)));
        glm::mat4 invalid(1);
        invalid[2][1] = std::numeric_limits<float>::quiet_NaN();
        CHECK(std::isinf(boundGpuSceneLodPixelError(base, invalid, viewport, error)));
    }

    void strictHysteresisDoesNotRelaxTheCeiling() {
        std::array<GpuSceneGeometryRecord, 2> geometries{ box(), box() };
        geometries[0].state.z = 1;
        geometries[1].localBoundsMin.w = 0.01f;
        geometries[1].localBoundsMax.w = 1;
        geometries[1].draw.y = 1500;
        const glm::mat4 clip = glm::perspectiveRH_ZO(Pi / 3, 16.0f / 9.0f, 0.1f, 100.0f) *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -40));
        const float error = boundGpuSceneLodPixelError(geometries[0], clip, { 3840, 2160 }, 0.01f);
        const auto select = [&](float ratio, uint32_t previous, float margin = 0.15f) {
            return selectGpuSceneLodGeometry(geometries, 0, clip, { 3840, 2160 },
                error / ratio, 1, previous, margin);
        };
        CHECK(select(0.99f, InvalidGpuSceneIndex) == 1); // Cold start is stateless.
        CHECK(select(0.99f, 1) == 1);
        CHECK(select(1.01f, 1) == 0); // Refinement never waits until 1.15 * target.
        uint32_t previous = 0;
        for (uint32_t frame = 0; frame < 120; ++frame) {
            previous = select(frame % 2 ? 0.99f : 1.01f, previous);
            CHECK(previous == 0);
        }
        CHECK(select(0.84f, previous) == 1);
        CHECK(select(0.86f, previous) == 0);
        CHECK(select(0.99f, previous, 0) == 1); // Exact stateless control.
        CHECK(select(0.1f, 0, std::numeric_limits<float>::quiet_NaN()) == 0);
    }

    void historyOwnershipGenerationAndInvalidation() {
        GpuScenePackedTables scene;
        scene.sceneEpoch = 1;
        scene.publicationRevision = 1;
        scene.primitives.resize(2);
        for (auto& primitive : scene.primitives) primitive.binding.y = 0;
        scene.geometries.push_back(box());
        scene.geometryRevisions = { 1 };
        // Large sparse logical slots must not force a giant GPU allocation.
        scene.densePrimitiveHandles = { { 999999, 7 }, { 123456, 3 } };
        scene.denseGeometryHandles = { { 99, 4 } };
        GpuSceneLodHistory history;
        history.resize(2);
        ViewTransportRecord view;
        ViewHistoryContext context;
        const auto frame = [&] {
            history.updateView(view, context);
            history.publish(scene);
        };
        frame();
        const auto first = history.binding(0);
        CHECK(first.slot < 2);
        CHECK(history.previous(first) == InvalidGpuSceneIndex);
        history.record(first, 1);
        frame();
        CHECK(history.previous(history.binding(0)) == 1); // Immediate preceding frame, not ring slot.
        history.record(history.binding(0), 2);
        frame();
        CHECK(history.previous(history.binding(0)) == 2);
        history.record(history.binding(0), 1);
        // Dense primitive relocation preserves its stable slot/token/history.
        std::swap(scene.densePrimitiveHandles[0], scene.densePrimitiveHandles[1]);
        ++scene.publicationRevision;
        frame();
        CHECK(history.binding(1).slot == first.slot);
        CHECK(history.previous(history.binding(1)) == 1);
        history.record(history.binding(1), 1);
        // Material edits and ordinary camera motion preserve history.
        ++scene.primitives[1].revisions.w;
        ++scene.publicationRevision;
        view.view[3][0] += 0.01f;
        frame();
        CHECK(history.previous(history.binding(1)) == 1);
        history.record(history.binding(1), 1);
        // Stable slot reuse with a new generation cannot inherit selection.
        ++scene.densePrimitiveHandles[1].generation;
        ++scene.publicationRevision;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        CHECK(history.binding(1).tokenLow != first.tokenLow);
        history.record(history.binding(1), 1);
        ++scene.geometryRevisions[0]; // Includes child-only and chain relocation changes.
        ++scene.publicationRevision;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        ++context.resetRevision; // Explicit camera cut.
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        ++context.identity; // Distinct consumer/view must not borrow history.
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        view.renderInfo.x = 0;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        view.renderInfo.x = 3840;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        view.projection[0][0] += 0.1f;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        frame(); // No record: culled or direct fallback frame.
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        ++scene.sceneEpoch;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        GpuSceneLodHistory otherView;
        otherView.resize(2);
        otherView.updateView(view, context);
        otherView.publish(scene);
        history.record(history.binding(1), 1);
        CHECK(otherView.previous(otherView.binding(1)) == InvalidGpuSceneIndex);
        otherView.record(otherView.binding(1), 2);
        otherView.updateView(view, context);
        frame();
        CHECK(otherView.previous(otherView.binding(1)) == 2);
        CHECK(history.previous(history.binding(1)) == 1);
        history.record(history.binding(1), 1);
        history.resetView(); // Suspension/rebuild even when the restored extent is identical.
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        ++scene.primitives[1].revisions.x;
        ++scene.publicationRevision;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        ++scene.primitives[1].revisions.y;
        ++scene.publicationRevision;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        history.record(history.binding(1), 1);
        ++scene.denseGeometryHandles[0].generation;
        ++scene.publicationRevision;
        frame();
        CHECK(history.previous(history.binding(1)) == InvalidGpuSceneIndex);
        // Buffer replacement, bounded exhaustion, removal, and duplicate writers.
        history.resize(1);
        frame();
        CHECK(history.binding(0).slot == InvalidGpuSceneIndex);
        scene.primitives.resize(1);
        scene.densePrimitiveHandles.resize(1);
        ++scene.publicationRevision;
        frame();
        CHECK(history.binding(0).slot == 0);
        history.resize(2);
        scene.primitives.resize(2);
        scene.primitives[1].binding.y = 0;
        scene.densePrimitiveHandles.push_back(scene.densePrimitiveHandles[0]);
        ++scene.publicationRevision;
        bool rejected = false;
        try { frame(); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }

    void packedSelectionFailsToBaseAndHonorsCap() {
        std::array<GpuSceneGeometryRecord, 3> geometries{ box(), box(), box() };
        geometries[0].state.z = 1;
        geometries[1].localBoundsMin.w = 0.01f;
        geometries[1].localBoundsMax.w = 1;
        geometries[1].draw.y = 1500;
        geometries[1].state.z = 2;
        geometries[2].localBoundsMin.w = 0.04f;
        geometries[2].localBoundsMax.w = 2;
        geometries[2].draw.y = 600;
        const glm::mat4 clip = glm::perspectiveRH_ZO(Pi / 3, 16.0f / 9.0f, 0.1f, 100.0f) *
            glm::translate(glm::mat4(1), glm::vec3(0, 0, -40));
        const auto select = [&](uint32_t cap = 15, float threshold = 4.0f) {
            return selectGpuSceneLodGeometry(geometries, 0, clip, { 3840, 2160 }, threshold, cap);
        };
        CHECK(select() == 2u);
        CHECK(select(1) == 1u);
        CHECK(select(0) == 0u);
        CHECK(select(15, 0.001f) == 0u);
        CHECK(select(15, std::numeric_limits<float>::quiet_NaN()) == 0u);
        geometries[2].state.z = 1; // Cycle must not select an arbitrary child.
        CHECK(select() == 0u);
        geometries[2].state.z = InvalidGpuSceneIndex;
        geometries[2].draw.w = 1; // Mixed index types cannot share a bound bin.
        CHECK(select() == 0u);
        geometries[2].draw.w = 0;
        geometries[2].localBoundsMin.w = 0.005f;
        CHECK(select() == 0u);
        geometries[1].state.z = 42;
        CHECK(select() == 0u);
        CHECK(selectGpuSceneLodGeometry(geometries, 42, clip, { 3840, 2160 }, 2, 15) == InvalidGpuSceneIndex);
    }

    void radialProbeSelectionIsFaceInvariantAndFailVisible() {
        std::array<GpuSceneGeometryRecord, 3> geometries{ box(), box(), box() };
        geometries[0].localBoundsSphere = { 0, 0, 0, 1 };
        geometries[0].state.z = 1;
        geometries[1].localBoundsMin.w = 0.01f;
        geometries[1].localBoundsMax.w = 1;
        geometries[1].draw.y = 1500;
        geometries[1].state.z = 2;
        geometries[2].localBoundsMin.w = 0.04f;
        geometries[2].localBoundsMax.w = 2;
        geometries[2].draw.y = 600;
        GpuSceneInstanceRecord instance;
        instance.worldBoundsSphere = { 0, 0, 21, 1 };
        const auto select = [&](glm::vec3 probe, float threshold = 1.0f,
                uint32_t cap = 15u) {
            return selectGpuSceneRadialLodGeometry(geometries, 0, instance,
                probe, 1024.0f, threshold, cap);
        };
        CHECK(select({ 0, 0, 0 }) == 1u);
        CHECK(select({ 0, 0, -100 }) == 2u);
        CHECK(select({ 0, 0, -100 }, 1.0f, 1u) == 1u);
        CHECK(select({ 0, 0, -100 }, 1.0f, 0u) == 0u);
        // Selection depends only on radial distance, never cube-face index.
        CHECK(select({ 20, 0, 21 }) == select({ 0, 20, 21 }));
        CHECK(select({ -20, 0, 21 }) == select({ 0, -20, 21 }));
        instance.worldBoundsSphere.w = -1.0f;
        CHECK(select({ 0, 0, -100 }) == 0u);
        instance.worldBoundsSphere.w = 1.0f;
        geometries[2].draw.w = 1u;
        CHECK(select({ 0, 0, -100 }) == 0u);
        CHECK(selectGpuSceneRadialLodGeometry(geometries, 99u, instance,
            {}, 1024.0f, 1.0f, 15u) == InvalidGpuSceneIndex);
    }

    void directionalDensitySelectionTracksShadowTexels() {
        std::array<GpuSceneGeometryRecord, 3> geometries{ box(), box(), box() };
        geometries[0].localBoundsSphere = { 0, 0, 0, 2 };
        geometries[0].state.z = 1;
        geometries[1].localBoundsMin.w = 0.01f;
        geometries[1].localBoundsMax.w = 1;
        geometries[1].draw.y = 1500;
        geometries[1].state.z = 2;
        geometries[2].localBoundsMin.w = 0.04f;
        geometries[2].localBoundsMax.w = 2;
        geometries[2].draw.y = 600;
        GpuSceneInstanceRecord instance;
        instance.worldBoundsSphere = { 0, 0, 0, 4 };
        CHECK(selectGpuSceneDensityLodGeometry(geometries, 0, instance,
            0.01f, 2.0f, 15u) == 1u);
        CHECK(selectGpuSceneDensityLodGeometry(geometries, 0, instance,
            0.05f, 2.0f, 15u) == 2u);
        CHECK(selectGpuSceneDensityLodGeometry(geometries, 0, instance,
            0.05f, 2.0f, 0u) == 0u);
        CHECK(selectGpuSceneDensityLodGeometry(geometries, 0, instance,
            0.0f, 2.0f, 15u) == 0u);
        geometries[2].localBoundsMax.w = 4.0f;
        CHECK(selectGpuSceneDensityLodGeometry(geometries, 0, instance,
            0.05f, 2.0f, 15u) == 0u);
    }
}

int main() {
    projectedSelectionAndHeroLimit();
    hysteresisIsDirectionalAndStable();
    residencyFallbackIsExplicit();
    malformedInputFailsVisibleDeterministically();
    projectionBoundContainsPerturbations();
    packedSelectionFailsToBaseAndHonorsCap();
    radialProbeSelectionIsFaceInvariantAndFailVisible();
    directionalDensitySelectionTracksShadowTexels();
    strictHysteresisDoesNotRelaxTheCeiling();
    historyOwnershipGenerationAndInvalidation();
    if (failures == 0) std::cout << "GpuSceneLodTests passed\n";
    return failures == 0 ? 0 : 1;
}
