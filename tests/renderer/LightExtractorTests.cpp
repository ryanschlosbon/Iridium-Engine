#include "scene/systems/TransformSystem.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/lighting/LocalShadow.h"
#include "renderer/rhi/LightUploadPlanner.h"
#include "scene/components/LightComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    [[nodiscard]] Iridium::SceneEntityUuid uuid(uint32_t suffix) {
        std::string value = "019fb7d3-0520-7000-8000-000000000000";
        constexpr char digits[] = "0123456789abcdef";
        value[value.size() - 1] = digits[suffix & 0xfu];
        return *Iridium::SceneEntityUuid::parse(value);
    }

    Entity addLight(Iridium::SceneWorld& world, uint32_t suffix,
        LightType type = LightType::Point, int32_t siblingOrder = 0,
        Entity parent = NULL_ENTITY) {
        const Entity entity = world.createEntity(uuid(suffix));
        world.registry().addComponent<TransformComponent>(entity);
        auto& relationship = world.registry().addComponent<RelationshipComponent>(
            entity);
        relationship.parent = parent;
        relationship.siblingOrder = siblingOrder;
        relationship.depth = parent == NULL_ENTITY ? 0 : 1;
        auto& light = world.registry().addComponent<LightComponent>(entity);
        light.type = type;
        light.colorLinearRec709 = { 1.0f, 0.5f, 0.25f };
        light.illuminanceLux = 100'000.0f;
        light.luminousIntensityCandela = 1'250.0f;
        light.rangeMeters = 20.0f;
        light.sourceRadiusMeters = 0.1f;
        light.innerConeDegrees = 15.0f;
        light.outerConeDegrees = 35.0f;
        light.shadowQuality = LightShadowQuality::Ultra;
        return entity;
    }

    void updateTransforms(Iridium::SceneWorld& world) {
        TransformSystem transforms;
        (void)transforms.update(world.registry());
    }

    [[nodiscard]] bool near(float lhs, float rhs, float epsilon = 1.0e-5f) {
        return std::abs(lhs - rhs) <= epsilon;
    }

    bool abiAndInitialUuidOrderingAreFrozen() {
        CHECK(sizeof(Iridium::PackedGpuLight) == 64);
        CHECK(alignof(Iridium::PackedGpuLight) == 16);
        CHECK(offsetof(Iridium::PackedGpuLight, positionRange) == 0);
        CHECK(offsetof(Iridium::PackedGpuLight, directionOuterCos) == 16);
        CHECK(offsetof(Iridium::PackedGpuLight, colorIntensity) == 32);
        CHECK(offsetof(Iridium::PackedGpuLight, shapeMetadata) == 48);

        // The GLSL mirror is checked against compiled SPIR-V member offsets in
        // ShaderAbiContractTests.

        Iridium::SceneWorld world;
        addLight(world, 3, LightType::Spot, 2);
        addLight(world, 1, LightType::Directional, 0);
        addLight(world, 2, LightType::Point, 1);
        updateTransforms(world);
        Iridium::LightExtractor extractor;
        const auto packet = extractor.extract(world);
        CHECK(packet.stats.sceneLightCount == 3);
        CHECK(packet.stats.activeLightCount == 3);
        CHECK(packet.stats.directionalLightCount == 1);
        CHECK(packet.stats.localLightCount == 2);
        CHECK(packet.stats.changedRecordCount == 3);
        CHECK(packet.changedRanges.size() == 1);
        CHECK(packet.changedRanges[0].firstRecord == 0);
        CHECK(packet.changedRanges[0].recordCount == 3);
        CHECK(extractor.slotFor(uuid(1)) == 0);
        CHECK(extractor.slotFor(uuid(2)) == 1);
        CHECK(extractor.slotFor(uuid(3)) == 2);

        const auto& directional = packet.records[0];
        CHECK(near(directional.directionOuterCos.x, 0.0f));
        CHECK(near(directional.directionOuterCos.y, 0.0f));
        CHECK(near(directional.directionOuterCos.z, 1.0f));
        CHECK(near(directional.colorIntensity.y, 1.0f));
        CHECK(near(directional.colorIntensity.w, 100'000.0f));
        const uint32_t metadata = std::bit_cast<uint32_t>(
            directional.shapeMetadata.z);
        CHECK((metadata & 3u) == static_cast<uint32_t>(
            Iridium::PackedGpuLightType::Directional));
        CHECK((metadata & Iridium::PackedGpuLightCastsShadows) != 0);
        CHECK(((metadata & Iridium::PackedGpuLightShadowQualityMask) >>
            Iridium::PackedGpuLightShadowQualityShift) == 3);
        CHECK(std::bit_cast<uint32_t>(directional.shapeMetadata.w) ==
            Iridium::kInvalidShadowDataSlot);
        return true;
    }

    bool changesRemovalAndReuseProduceExactRanges() {
        Iridium::SceneWorld world;
        const Entity first = addLight(world, 1, LightType::Point, 0);
        const Entity second = addLight(world, 2, LightType::Point, 1);
        updateTransforms(world);
        Iridium::LightExtractor extractor({ .initialCapacity = 2,
            .maximumCapacity = 8 });
        (void)extractor.extract(world);

        auto unchanged = extractor.extract(world);
        CHECK(unchanged.stats.changedRecordCount == 0);
        CHECK(unchanged.changedRanges.empty());
        const uint64_t firstRevision = unchanged.recordRevisions[0];
        const uint64_t secondRevision = unchanged.recordRevisions[1];

        world.registry().getComponent<LightComponent>(second)
            .luminousIntensityCandela = 2'500.0f;
        const auto changed = extractor.extract(world);
        CHECK(changed.stats.changedRecordCount == 1);
        CHECK(changed.changedRanges.size() == 1);
        CHECK(changed.changedRanges[0].firstRecord == 1);
        CHECK(changed.recordRevisions[0] == firstRevision);
        CHECK(changed.recordRevisions[1] > secondRevision);
        CHECK(near(changed.records[1].colorIntensity.w, 2'500.0f));

        CHECK(world.destroyEntity(first));
        const auto removed = extractor.extract(world);
        CHECK(removed.stats.activeLightCount == 1);
        CHECK(removed.stats.changedRecordCount == 1);
        CHECK(removed.changedRanges[0].firstRecord == 0);
        CHECK(removed.records[0].colorIntensity == glm::vec4(0.0f));

        addLight(world, 3, LightType::Spot, 0);
        updateTransforms(world);
        const auto reused = extractor.extract(world);
        CHECK(extractor.slotFor(uuid(3)) == 0);
        CHECK(reused.stats.changedRecordCount == 1);
        CHECK(reused.changedRanges[0].firstRecord == 0);
        return true;
    }

    bool hierarchyDirectionIgnoresNonuniformAndNegativeScale() {
        Iridium::SceneWorld world;
        const Entity parent = world.createEntity(uuid(1));
        auto& parentTransform = world.registry().addComponent<TransformComponent>(
            parent);
        parentTransform.position = { 10.0f, 2.0f, 3.0f };
        parentTransform.rotation = { 0.0f, 90.0f, 0.0f };
        parentTransform.scale = { -2.0f, 3.0f, 0.5f };
        world.registry().addComponent<RelationshipComponent>(parent);
        const Entity child = addLight(world, 2, LightType::Spot, 0, parent);
        auto& childTransform = world.registry().getComponent<TransformComponent>(
            child);
        childTransform.position = { 0.0f, 0.0f, -2.0f };
        childTransform.rotation = { 0.0f, 0.0f, 90.0f };
        updateTransforms(world);

        Iridium::LightExtractor extractor;
        const auto packet = extractor.extract(world);
        CHECK(packet.stats.activeLightCount == 1);
        const auto& record = packet.records[*extractor.slotFor(uuid(2))];
        const glm::vec3 expectedPosition = glm::vec3(
            childTransform.worldMatrix[3]);
        CHECK(near(record.positionRange.x, expectedPosition.x));
        CHECK(near(record.positionRange.y, expectedPosition.y));
        CHECK(near(record.positionRange.z, expectedPosition.z));
        // Local +Z is the authored emission axis used by the editor gizmo,
        // shading cone, cluster bounds, and shadow camera.
        CHECK(near(record.directionOuterCos.x, 1.0f));
        CHECK(near(record.directionOuterCos.y, 0.0f));
        CHECK(near(record.directionOuterCos.z, 0.0f));
        return true;
    }

    // Regression (M7R R4c.0): growing record capacity inside one extract must not
    // invalidate the new-candidate pointers. Debug heaps poison freed memory, so a
    // dangling pointer reads garbage owners and slots.
    bool capacityGrowthKeepsNewCandidatesValid() {
        Iridium::SceneWorld world;
        addLight(world, 1, LightType::Point, 0);
        addLight(world, 2, LightType::Point, 1);
        updateTransforms(world);
        Iridium::LightExtractor extractor({ .initialCapacity = 2,
            .maximumCapacity = 16 });
        const auto first = extractor.extract(world);
        CHECK(first.stats.activeLightCount == 2);
        for (uint32_t index = 3; index <= 14; ++index) {
            const Entity entity = addLight(world, index, LightType::Point,
                static_cast<int32_t>(index - 1));
            world.registry().getComponent<LightComponent>(entity).priority =
                static_cast<int32_t>(index);
        }
        updateTransforms(world);
        const auto grown = extractor.extract(world);
        CHECK(grown.stats.activeLightCount == 14);
        CHECK(grown.stats.omittedLightCount == 0);
        std::array<bool, 16> used{};
        for (uint32_t index = 1; index <= 14; ++index) {
            const auto slot = extractor.slotFor(uuid(index));
            CHECK(slot.has_value());
            CHECK(*slot < used.size());
            CHECK(!used[*slot]);
            used[*slot] = true;
        }
        return true;
    }

    bool invalidCapacityAndWorldSwapAreDeterministic() {
        Iridium::SceneWorld world;
        const Entity area = addLight(world, 1, LightType::Area, 0);
        const Entity zero = addLight(world, 2, LightType::Point, 1);
        world.registry().getComponent<LightComponent>(zero)
            .colorLinearRec709 = glm::vec3(0.0f);
        const Entity missing = world.createEntity(uuid(3));
        world.registry().addComponent<RelationshipComponent>(missing)
            .siblingOrder = 2;
        world.registry().addComponent<LightComponent>(missing);
        (void)area;
        updateTransforms(world);
        Iridium::LightExtractor extractor({ .initialCapacity = 2,
            .maximumCapacity = 3 });
        const auto invalid = extractor.extract(world);
        CHECK(invalid.stats.sceneLightCount == 3);
        CHECK(invalid.stats.activeLightCount == 0);
        CHECK(invalid.stats.omittedLightCount == 3);
        CHECK(extractor.diagnostics().size() == 3);

        world.clear();
        for (uint32_t index = 1; index <= 4; ++index) {
            const Entity entity = addLight(world, index, LightType::Point,
                static_cast<int32_t>(index - 1));
            auto& light = world.registry().getComponent<LightComponent>(entity);
            light.priority = static_cast<int32_t>(index);
            light.castsShadows = index != 3;
        }
        updateTransforms(world);
        const auto limited = extractor.extract(world);
        CHECK(limited.stats.activeLightCount == 3);
        CHECK(limited.stats.omittedLightCount == 1);
        CHECK(!extractor.slotFor(uuid(1)));
        CHECK(extractor.slotFor(uuid(2)) == 0);
        CHECK(extractor.slotFor(uuid(3)) == 1);
        CHECK(extractor.slotFor(uuid(4)) == 2);

        Iridium::SceneWorld staging;
        addLight(staging, 1, LightType::Directional, 0);
        addLight(staging, 4, LightType::Point, 1);
        updateTransforms(staging);
        const uint64_t epoch = world.stateEpoch();
        world.swapState(staging);
        CHECK(world.stateEpoch() != epoch);
        const auto swapped = extractor.extract(world);
        CHECK(swapped.stats.activeLightCount == 2);
        CHECK(extractor.slotFor(uuid(1)) == 0);
        CHECK(extractor.slotFor(uuid(4)) == 1);
        return true;
    }

    // M7.10.5: a radiometric edit revises the record (it still uploads) but
    // not the shadow revision; anything that changes shadow-map content
    // revises both.
    bool shadowRevisionsTrackOnlyShadowGeometry() {
        Iridium::SceneWorld world;
        const Entity spot = addLight(world, 1, LightType::Spot, 0);
        const Entity point = addLight(world, 2, LightType::Point, 1);
        const Entity sun = addLight(world, 3, LightType::Directional, 2);
        updateTransforms(world);
        Iridium::LightExtractor extractor;
        const auto first = extractor.extract(world);
        CHECK(first.shadowRevisions.size() == first.recordRevisions.size());
        for (uint32_t slot : first.activeSlots)
            CHECK(first.shadowRevisions[slot] != 0);

        const auto edit = [&](Entity entity, uint32_t suffix,
            const auto& mutate, bool recordChanges, bool shadowChanges) {
            const uint32_t slot = *extractor.slotFor(uuid(suffix));
            const auto before = extractor.extract(world);
            const uint64_t record = before.recordRevisions[slot];
            const uint64_t shadow = before.shadowRevisions[slot];
            auto& transform = world.registry().getComponent<TransformComponent>(
                entity);
            mutate(world.registry().getComponent<LightComponent>(entity),
                transform);
            transform.isDirty = true;
            updateTransforms(world);
            const auto after = extractor.extract(world);
            return (after.recordRevisions[slot] != record) == recordChanges &&
                (after.shadowRevisions[slot] != shadow) == shadowChanges;
        };
        using Light = LightComponent;
        using Transform = TransformComponent;
        // Radiometric and sampling-only inputs.
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.luminousIntensityCandela = 50'000.0f; }, true, false));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.colorLinearRec709 = { 0.25f, 0.5f, 1.0f }; }, true, false));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.sourceRadiusMeters = 0.4f; }, true, false));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.innerConeDegrees = 25.0f; }, true, false));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.priority = 7; }, false, false));
        CHECK(edit(point, 2, [](Light& l, Transform&) {
            l.luminousIntensityCandela = 1.0e6f; }, true, false));
        CHECK(edit(sun, 3, [](Light& l, Transform&) {
            l.illuminanceLux = 1'000.0f; }, true, false));
        // Shadow view, projection, far plane and shadow settings.
        CHECK(edit(spot, 1, [](Light&, Transform& t) {
            t.position.x += 1.0f; }, true, true));
        CHECK(edit(spot, 1, [](Light&, Transform& t) {
            t.rotation.y += 10.0f; }, true, true));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.rangeMeters = 30.0f; }, true, true));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.outerConeDegrees = 40.0f; }, true, true));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.shadowQuality = LightShadowQuality::Low; }, true, true));
        CHECK(edit(spot, 1, [](Light& l, Transform&) {
            l.castsShadows = false; }, true, true));
        CHECK(edit(point, 2, [](Light& l, Transform&) {
            l.rangeMeters = 5.0f; }, true, true));
        CHECK(edit(point, 2, [](Light&, Transform& t) {
            t.position.z -= 2.0f; }, true, true));
        CHECK(edit(point, 2, [](Light& l, Transform&) {
            l.type = LightType::Spot; }, true, true));
        CHECK(edit(sun, 3, [](Light&, Transform& t) {
            t.rotation.x += 15.0f; }, true, true));

        // A removed light's cleared record revises its slot's shadow too.
        const uint32_t sunSlot = *extractor.slotFor(uuid(3));
        const uint64_t sunShadow = extractor.extract(world)
            .shadowRevisions[sunSlot];
        CHECK(world.destroyEntity(sun));
        const auto removed = extractor.extract(world);
        CHECK(removed.shadowRevisions[sunSlot] != sunShadow);
        return true;
    }

    // M7.10.5: the cached local and directional shadow maps survive an
    // intensity or colour drag when keyed on the shadow revision (as
    // RenderExtractor keys them); a range or position edit re-renders.
    bool shadowCachesSurviveRadiometricEdits() {
        using namespace Iridium;
        SceneWorld world;
        const Entity point = addLight(world, 1, LightType::Point, 0);
        const Entity spot = addLight(world, 2, LightType::Spot, 1);
        const Entity sun = addLight(world, 3, LightType::Directional, 2);
        updateTransforms(world);
        LightExtractor extractor;
        StableSpotShadowAtlas atlas;
        StablePointShadowPools pools;
        // Ultra tiles exceed the default per-frame texel budget.
        constexpr LocalShadowScheduleConfig budget{
            .maximumRenderedTexels = 64ull * 1024ull * 1024ull };
        LocalShadowCacheScheduler spotCache(budget);
        LocalShadowCacheScheduler pointCache(budget);
        DirectionalShadowCache sunCache;
        std::vector<LocalShadowRequest> requests;
        std::vector<DirectionalShadowSelection> selections;
        std::vector<LocalShadowCacheInput> inputs;
        struct Reasons {
            LocalShadowDirtyReason point = LocalShadowDirtyReason::None;
            LocalShadowDirtyReason spot = LocalShadowDirtyReason::None;
            uint32_t sunDirtyMask = 0;
            uint32_t allocationChanges = 0;
        };
        const auto frame = [&] {
            Reasons result;
            const LightingFramePacket packet = extractor.extract(world);
            buildLocalShadowRequests(packet, glm::vec3(0.0f, 1.0f, 5.0f),
                requests);
            const LocalShadowAllocationStats spotStats =
                atlas.reconcile(requests);
            const LocalShadowAllocationStats pointStats =
                pools.reconcile(requests);
            result.allocationChanges = spotStats.relocated +
                spotStats.evicted + pointStats.relocated + pointStats.evicted;
            const auto input = [&](LocalShadowKind kind, SceneEntityUuid owner,
                uint32_t slot, uint32_t resolution) {
                const auto request = std::ranges::find_if(requests,
                    [&](const LocalShadowRequest& candidate) {
                        return candidate.kind == kind &&
                            candidate.owner == owner;
                    });
                return LocalShadowCacheInput{ .request = *request,
                    .resolution = resolution, .allocationRevision = 1,
                    .lightRevision = packet.shadowRevisions[slot],
                    .casterRevision = 1, .projectionRevision = 1 };
            };
            inputs.clear();
            for (const SpotShadowTile& tile : atlas.allocations())
                inputs.push_back(input(LocalShadowKind::Spot, tile.owner,
                    tile.lightSlot, tile.size));
            const LocalShadowSchedule& spotSchedule =
                spotCache.schedule(inputs);
            if (spotSchedule.entries.size() == 1)
                result.spot = spotSchedule.entries[0].dirtyReason;
            spotCache.markScheduledRendered();
            inputs.clear();
            for (const PointShadowSlot& slot : pools.allocations())
                inputs.push_back(input(LocalShadowKind::Point, slot.owner,
                    slot.lightSlot, slot.resolution));
            const LocalShadowSchedule& pointSchedule =
                pointCache.schedule(inputs);
            if (pointSchedule.entries.size() == 1)
                result.point = pointSchedule.entries[0].dirtyReason;
            pointCache.markScheduledRendered();
            selectDirectionalShadowLights(packet,
                kDirectionalShadowLightCapacity, selections);
            if (selections.size() == 1) {
                const DirectionalShadowSchedule schedule = sunCache.schedule({
                    .selection = selections[0],
                    .lightRevision =
                        packet.shadowRevisions[selections[0].lightSlot],
                    .casterRevisions = { 1, 1, 1, 1 },
                }, kDirectionalShadowCascadeCount);
                result.sunDirtyMask = schedule.dirtyMask;
                sunCache.markRendered(schedule.updateMask);
            }
            return result;
        };
        const auto lightOf = [&](Entity entity) -> LightComponent& {
            return world.registry().getComponent<LightComponent>(entity);
        };
        const auto steady = [](const Reasons& reasons) {
            return reasons.point == LocalShadowDirtyReason::None &&
                reasons.spot == LocalShadowDirtyReason::None &&
                reasons.sunDirtyMask == 0 && reasons.allocationChanges == 0;
        };

        Reasons reasons = frame();
        CHECK(reasons.point == LocalShadowDirtyReason::NewAllocation);
        CHECK(reasons.spot == LocalShadowDirtyReason::NewAllocation);
        CHECK(reasons.sunDirtyMask != 0);
        CHECK(steady(frame()));
        // The slider drag of owner case 3: intensity and colour every frame.
        for (uint32_t step = 1; step <= 8; ++step) {
            lightOf(point).luminousIntensityCandela = 1'000.0f * step;
            lightOf(spot).luminousIntensityCandela = 2'000.0f * step;
            lightOf(sun).illuminanceLux = 10'000.0f * step;
            lightOf(point).colorLinearRec709 = { 1.0f, 0.1f * step, 0.5f };
            lightOf(spot).sourceRadiusMeters = 0.05f * step;
            CHECK(steady(frame()));
        }
        lightOf(point).rangeMeters = 12.0f;
        reasons = frame();
        CHECK(reasons.point == LocalShadowDirtyReason::LightChanged);
        CHECK(reasons.spot == LocalShadowDirtyReason::None);
        auto& spotTransform = world.registry().getComponent<TransformComponent>(
            spot);
        spotTransform.position.y += 0.5f;
        spotTransform.isDirty = true;
        updateTransforms(world);
        reasons = frame();
        CHECK(reasons.spot == LocalShadowDirtyReason::LightChanged);
        CHECK(reasons.point == LocalShadowDirtyReason::None);
        CHECK(steady(frame()));
        return true;
    }

    bool perFrameUploadPlanningIsRevisionExact() {
        std::array<uint64_t, 4> source{ 1, 0, 2, 2 };
        std::array<uint64_t, 4> firstFrame{};
        std::array<uint64_t, 4> secondFrame{};
        std::vector<Iridium::LightRecordRange> ranges;
        Iridium::buildLightUploadRanges(source, firstFrame, ranges);
        CHECK(ranges.size() == 2);
        CHECK(ranges[0].firstRecord == 0 && ranges[0].recordCount == 1);
        CHECK(ranges[1].firstRecord == 2 && ranges[1].recordCount == 2);

        firstFrame = source;
        Iridium::buildLightUploadRanges(source, firstFrame, ranges);
        CHECK(ranges.empty());
        Iridium::buildLightUploadRanges(source, secondFrame, ranges);
        CHECK(ranges.size() == 2);

        source[1] = 3;
        Iridium::buildLightUploadRanges(source, firstFrame, ranges);
        CHECK(ranges.size() == 1);
        CHECK(ranges[0].firstRecord == 1 && ranges[0].recordCount == 1);
        firstFrame[1] = source[1];
        source[0] = 4; // A removed light is a revised zero record on the wire.
        Iridium::buildLightUploadRanges(source, firstFrame, ranges);
        CHECK(ranges.size() == 1 && ranges[0].firstRecord == 0);

        const std::array<uint64_t, 4> zero{};
        Iridium::buildLightUploadRanges(zero, secondFrame, ranges);
        CHECK(ranges.empty());
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "ABI and UUID ordering", abiAndInitialUuidOrderingAreFrozen },
        std::pair{ "change ranges and reuse", changesRemovalAndReuseProduceExactRanges },
        std::pair{ "scale-independent hierarchy direction", hierarchyDirectionIgnoresNonuniformAndNegativeScale },
        std::pair{ "invalid capacity and swap", invalidCapacityAndWorldSwapAreDeterministic },
        std::pair{ "per-frame upload revisions", perFrameUploadPlanningIsRevisionExact },
        std::pair{ "capacity growth keeps new candidates valid", capacityGrowthKeepsNewCandidatesValid },
        std::pair{ "shadow revisions track only shadow geometry", shadowRevisionsTrackOnlyShadowGeometry },
        std::pair{ "shadow caches survive radiometric edits", shadowCachesSurviveRadiometricEdits },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
