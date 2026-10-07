// M7.10.1: conservative transparent frustum culling. The whole-owner test
// rejects only work the per-packet test also rejects, instance batches use a
// bound over every instance, culled keys stay tracked by the previous-
// transform cache, and the culled work's residency demand is reported.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "extraction/TransparentFrustumCulling.h"

#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderFrame.h"
#include "scene/components/RenderInstanceBatchComponent.h"

#include <glm/gtc/matrix_transform.hpp>

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

    CompiledTransparencyPolicy policy(TransparencyClass resolvedClass,
        TransparencyQuality quality = TransparencyQuality::Ordinary2) {
        CompiledTransparencyPolicy result;
        result.resolvedClass = resolvedClass;
        result.quality = quality;
        return result;
    }

    SubMesh subMesh(const glm::vec3& minimum, const glm::vec3& maximum,
        const char* primitive, TransparencyClass resolvedClass =
            TransparencyClass::ThinGlass) {
        SubMesh result;
        result.boundsMin = minimum;
        result.boundsMax = maximum;
        result.primitiveGuid = *AssetGuid::parse(primitive);
        result.transparency = policy(resolvedClass);
        return result;
    }

    void residencyDemand() {
        using Mode = TransparencyExecutionMode;
        CHECK(transparentResidencyDemand(Mode::Classified,
            policy(TransparencyClass::WeightedOit)) == TransparentDemandWeightedOit);
        CHECK(transparentResidencyDemand(Mode::Classified,
            policy(TransparencyClass::SortedSurface)) == 0u);
        CHECK(transparentResidencyDemand(Mode::Classified,
            policy(TransparencyClass::ThinGlass)) ==
            TransparentDemandCompatibilityQueue);
        CHECK(transparentResidencyDemand(Mode::Classified,
            policy(TransparencyClass::LayeredGlass, TransparencyQuality::Hero4)) ==
            (TransparentDemandCompatibilityQueue | TransparentDemandHero4));
        // Legacy work always goes to the compatibility queue.
        CHECK(transparentResidencyDemand(Mode::LegacyTwoBucket,
            policy(TransparencyClass::WeightedOit)) ==
            TransparentDemandCompatibilityQueue);
        CHECK(transparentResidencyDemand(Mode::LegacyTwoBucket,
            policy(TransparencyClass::LayeredGlass)) ==
            TransparentDemandCompatibilityQueue);
    }

    void localBounds() {
        ModelAsset model;
        model.subMeshes = {
            subMesh({ -1, -1, -1 }, { 0, 0, 0 }, "019fb73d-5a60-7000-8000-000000000001"),
            subMesh({ 5, 5, 5 }, { 6, 6, 6 }, "019fb73d-5a60-7000-8000-000000000002"),
            subMesh({ 1, -2, 0 }, { 2, 1, 3 }, "019fb73d-5a60-7000-8000-000000000003"),
        };
        const std::vector<uint32_t> listed{ 0u, 2u };
        const TransparentLocalBounds bounds = transparentSubmeshBounds(model, listed);
        CHECK(bounds.valid);
        CHECK(bounds.minimum == glm::vec3(-1.0f, -2.0f, -1.0f));
        CHECK(bounds.maximum == glm::vec3(2.0f, 1.0f, 3.0f));
        CHECK(!transparentSubmeshBounds(model, {}).valid);
        model.subMeshes[2].boundsMax.x = std::numeric_limits<float>::quiet_NaN();
        CHECK(!transparentSubmeshBounds(model, listed).valid);
        model.subMeshes[2].boundsMax.x = 0.5f;   // inverted on x
        CHECK(!transparentSubmeshBounds(model, listed).valid);

        // An off-screen invalid bound never rejects (fail visible).
        const GpuSceneFrustum identity = makeGpuSceneFrustum(glm::mat4(1.0f));
        const glm::mat4 farAway = glm::translate(glm::mat4(1.0f),
            glm::vec3(100.0f, 0.0f, 0.0f));
        CHECK(!transparentLocalBoundsFrustumRejected(identity,
            transparentSubmeshBounds(model, listed), farAway));
        model.subMeshes[2].boundsMax.x = 2.0f;
        CHECK(transparentLocalBoundsFrustumRejected(identity,
            transparentSubmeshBounds(model, listed), farAway));
        CHECK(!transparentLocalBoundsFrustumRejected(identity,
            transparentSubmeshBounds(model, listed), glm::mat4(1.0f)));
        // A degenerate transform never rejects.
        CHECK(!transparentLocalBoundsFrustumRejected(identity,
            transparentSubmeshBounds(model, listed), glm::mat4(0.0f)));
    }

    // Whole-owner rejection implies every listed packet is rejected by the
    // per-packet path (depth interval or frustum), over random transforms.
    void wholeModelImpliesPacketRejection() {
        std::mt19937 random(0x7101u);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::uniform_real_distribution<float> place(-60.0f, 60.0f);
        std::uniform_real_distribution<float> size(0.05f, 3.0f);
        uint32_t rejectedModels = 0;
        uint32_t keptModels = 0;
        for (uint32_t trial = 0; trial < 2000; ++trial) {
            ModelAsset model;
            const uint32_t count = 1u + random() % 12u;
            std::vector<uint32_t> listed;
            for (uint32_t index = 0; index < count; ++index) {
                const glm::vec3 minimum{ unit(random) * 4.0f, unit(random) * 4.0f,
                    unit(random) * 4.0f };
                model.subMeshes.push_back(subMesh(minimum, minimum +
                    glm::vec3(size(random), size(random), size(random)),
                    "019fb73d-5a60-7000-8000-000000000001"));
                if (random() % 3u != 0u) listed.push_back(index);
            }
            if (listed.empty()) listed.push_back(0u);
            glm::mat4 world = glm::translate(glm::mat4(1.0f),
                glm::vec3(place(random), place(random) * 0.2f, place(random)));
            world = glm::rotate(world, unit(random) * 3.14159f, glm::normalize(
                glm::vec3(unit(random), unit(random), unit(random)) + 0.01f));
            world = glm::scale(world, glm::vec3(0.5f + size(random),
                (random() % 5u == 0u ? -1.0f : 1.0f) * (0.5f + size(random)),
                0.5f + size(random)));
            const float nearPlane = 0.1f;
            const float farPlane = 80.0f;
            const glm::mat4 view = glm::lookAt(
                glm::vec3(unit(random) * 5.0f, 2.0f, unit(random) * 5.0f),
                glm::vec3(place(random), 0.0f, place(random)),
                glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 projection = glm::perspective(glm::radians(60.0f),
                16.0f / 9.0f, nearPlane, farPlane);
            projection[1][1] *= -1.0f;
            const GpuSceneFrustum frustum = makeGpuSceneFrustum(projection * view);
            CHECK(frustum.valid);
            const TransparentLocalBounds bounds =
                transparentSubmeshBounds(model, listed);
            if (!transparentModelFrustumRejected(frustum, model, nullptr,
                    listed, bounds, world)) {
                ++keptModels;
                continue;
            }
            ++rejectedModels;
            for (const uint32_t index : listed) {
                DrawPacket packet{};
                packet.worldTransform = world;
                const bool depthVisible = prepareTransparentWorkInterval(packet,
                    model.subMeshes[index].boundsMin,
                    model.subMeshes[index].boundsMax, view, nearPlane, farPlane);
                CHECK(!depthVisible ||
                    transparentPacketFrustumRejected(frustum, packet));
            }
        }
        CHECK(rejectedModels > 100u);
        CHECK(keptModels > 100u);
    }

    void instanceBatchBounds() {
        ModelAsset model;
        model.subMeshes = {
            subMesh({ -1, -1, -1 }, { 1, 1, 1 }, "019fb73d-5a60-7000-8000-000000000001",
                TransparencyClass::WeightedOit),
            subMesh({ -1, -1, -1 }, { 1, 1, 1 }, "019fb73d-5a60-7000-8000-000000000002",
                TransparencyClass::WeightedOit),
        };
        RenderInstanceBatchComponent batch;
        // The batch bound covers instances spread far beyond one submesh.
        batch.subMeshBounds = {
            { { -1, -1, -1 }, { 1, 1, 1 }, true },
            { { -1, -1, -1 }, { 40, 1, 1 }, true },
        };
        const std::vector<uint32_t> listed{ 0u, 1u };
        const TransparentLocalBounds bounds =
            instanceBatchTransparentBounds(batch, model, listed);
        CHECK(bounds.valid);
        CHECK(bounds.maximum.x == 40.0f);
        const GpuSceneFrustum identity = makeGpuSceneFrustum(glm::mat4(1.0f));
        // Translated so the model's own bounds are off-screen but the batch's
        // instances still reach the clip volume: not rejected.
        const glm::mat4 world = glm::translate(glm::mat4(1.0f),
            glm::vec3(-20.0f, 0.0f, 0.5f));
        const TransparentLocalBounds modelBounds =
            transparentSubmeshBounds(model, listed);
        CHECK(transparentModelFrustumRejected(identity, model, nullptr, listed,
            modelBounds, world));
        CHECK(!transparentModelFrustumRejected(identity, model, &batch, listed,
            modelBounds, world));
        // Wholly off-screen batch: rejected.
        const glm::mat4 farAway = glm::translate(glm::mat4(1.0f),
            glm::vec3(-100.0f, 0.0f, 0.5f));
        CHECK(transparentModelFrustumRejected(identity, model, &batch, listed,
            modelBounds, farAway));
        // Missing bounds or a non-WeightedOIT submesh: the per-submesh path
        // reports the batch error, so the owner is never rejected whole.
        batch.subMeshBounds[1].valid = false;
        CHECK(!instanceBatchTransparentBounds(batch, model, listed).valid);
        CHECK(!transparentModelFrustumRejected(identity, model, &batch, listed,
            modelBounds, farAway));
        batch.subMeshBounds[1].valid = true;
        model.subMeshes[1].transparency = policy(TransparencyClass::SortedSurface);
        CHECK(!transparentModelFrustumRejected(identity, model, &batch, listed,
            modelBounds, farAway));
        batch.subMeshBounds.pop_back();
        model.subMeshes[1].transparency = policy(TransparencyClass::WeightedOit);
        CHECK(!transparentModelFrustumRejected(identity, model, &batch, listed,
            modelBounds, farAway));
    }

    void replayTouchesAndReportsDemand() {
        const SceneEntityUuid owner =
            *SceneEntityUuid::parse("019fb73d-5a60-7000-8000-0000000000a0");
        const SceneEntityUuid other =
            *SceneEntityUuid::parse("019fb73d-5a60-7000-8000-0000000000b0");
        ModelAsset model;
        model.subMeshes = {
            subMesh({ -1, -1, -1 }, { 1, 1, 1 }, "019fb73d-5a60-7000-8000-000000000001",
                TransparencyClass::LayeredGlass),
            subMesh({ -1, -1, -1 }, { 1, 1, 1 }, "019fb73d-5a60-7000-8000-000000000002"),
            subMesh({ -1, -1, -1 }, { 1, 1, 1 }, "019fb73d-5a60-7000-8000-000000000003",
                TransparencyClass::WeightedOit),
        };
        const std::vector<uint32_t> listed{ 0u, 2u };
        const glm::mat4 modelWorld = glm::translate(glm::mat4(1.0f),
            glm::vec3(3.0f, 0.0f, 0.0f));
        TransparentCullRecord record;
        DrawPacket packet{};
        packet.owner = other;
        packet.primitiveGuid = model.subMeshes[1].primitiveGuid;
        packet.worldTransform = glm::translate(glm::mat4(1.0f),
            glm::vec3(0.0f, 7.0f, 0.0f));
        packet.transparency = policy(TransparencyClass::SortedSurface);
        record.cull(packet);
        CHECK(record.packetDemand == 0u);
        record.models.push_back({ owner, &model, listed, modelWorld });

        PreviousTransformCache cache;
        cache.beginFrame();
        const uint32_t authored = replayCulledTransparentWork(record, cache,
            true, 8u);
        cache.endFrame();
        CHECK(authored == (TransparentDemandCompatibilityQueue |
            TransparentDemandOrdinary2 | TransparentDemandWeightedOit));
        CHECK(cache.trackedCount() == 3u);
        cache.beginFrame();
        // Every culled key re-enters with this frame's transform as previous.
        CHECK(cache.resolve({ other, model.subMeshes[1].primitiveGuid },
            glm::mat4(1.0f)) == packet.worldTransform);
        CHECK(cache.resolve({ owner, model.subMeshes[0].primitiveGuid },
            glm::mat4(1.0f)) == modelWorld);
        CHECK(cache.resolve({ owner, model.subMeshes[2].primitiveGuid },
            glm::mat4(1.0f)) == modelWorld);
        // The unlisted submesh was never part of the owner's work.
        CHECK(cache.resolve({ owner, model.subMeshes[1].primitiveGuid },
            glm::mat4(2.0f)) == glm::mat4(2.0f));
        // The editor's layered-interface override applies unless deterministic.
        const uint32_t overridden = replayCulledTransparentWork(record, cache,
            false, 8u);
        cache.endFrame();
        CHECK(overridden == (TransparentDemandCompatibilityQueue |
            TransparentDemandCinematic8 | TransparentDemandWeightedOit));
        record.clear();
        CHECK(record.packets.empty() && record.models.empty() &&
            record.packetDemand == 0u);
    }
}

int main() {
    residencyDemand();
    localBounds();
    wholeModelImpliesPacketRejection();
    instanceBatchBounds();
    replayTouchesAndReportsDemand();
    if (failures == 0) std::cout << "TransparentFrustumCullingTests passed\n";
    return failures == 0 ? 0 : 1;
}
