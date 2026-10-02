#include "ecs/systems/TransformSystem.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/rhi/LightUploadPlanner.h"
#include "scene/components/LightComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
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

        std::ifstream shader(std::string(PROJECT_ROOT_DIR) +
            "/assets/shaders/include/lighting_records.glsl",
            std::ios::binary);
        const std::string shaderSource((std::istreambuf_iterator<char>(shader)),
            std::istreambuf_iterator<char>());
        CHECK(!shaderSource.empty());
        const size_t position = shaderSource.find("vec4 positionRange;");
        const size_t direction = shaderSource.find("vec4 directionOuterCos;");
        const size_t color = shaderSource.find("vec4 colorIntensity;");
        const size_t shaderMetadata = shaderSource.find("vec4 shapeMetadata;");
        CHECK(position < direction && direction < color &&
            color < shaderMetadata);

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
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
