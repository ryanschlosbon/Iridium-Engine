#include "renderer/lighting/ClusteredLighting.h"

#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
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

    struct Lights {
        std::vector<Iridium::PackedGpuLight> records;
        std::vector<uint64_t> revisions;
        std::vector<uint32_t> active;
        std::vector<Iridium::LightSelectionMetadata> metadata;

        uint32_t add(Iridium::PackedGpuLightType type, glm::vec3 position,
            float range, int32_t priority = 0, bool shadows = false,
            float intensity = 1'000.0f, float outerCos = 0.8f) {
            const uint32_t slot = static_cast<uint32_t>(records.size());
            Iridium::PackedGpuLight record{};
            record.positionRange = { position, range };
            record.directionOuterCos = { 0.0f, 0.0f, 1.0f, outerCos };
            record.colorIntensity = { 1.0f, 1.0f, 1.0f, intensity };
            uint32_t flags = static_cast<uint32_t>(type);
            if (shadows) flags |= Iridium::PackedGpuLightCastsShadows;
            record.shapeMetadata.z = std::bit_cast<float>(flags);
            record.shapeMetadata.w = std::bit_cast<float>(
                Iridium::kInvalidShadowDataSlot);
            records.push_back(record);
            revisions.push_back(slot + 1);
            active.push_back(slot);
            metadata.push_back({ uuid(slot + 1), priority, shadows });
            return slot;
        }

        [[nodiscard]] Iridium::LightingFramePacket packet() const {
            return {
                .records = records,
                .recordRevisions = revisions,
                .activeSlots = active,
                .selectionMetadata = metadata,
                .requiredCapacity = static_cast<uint32_t>(records.size()),
                .stats = {
                    .activeLightCount = static_cast<uint32_t>(active.size()),
                    .capacity = static_cast<uint32_t>(records.size()),
                },
            };
        }
    };

    [[nodiscard]] Iridium::ClusterFrameParameters frame(
        uint32_t width = 64, uint32_t height = 64) {
        Iridium::ClusterFrameParameters result{};
        result.renderWidth = width;
        result.renderHeight = height;
        result.nearPlane = 1.0f;
        result.farPlane = 100.0f;
        result.projection = glm::perspectiveRH_ZO(
            glm::radians(90.0f), static_cast<float>(width) / height,
            result.nearPlane, result.farPlane);
        result.projection[1][1] *= -1.0f;
        return result;
    }

    [[nodiscard]] Iridium::ClusterGridConfig smallConfig() {
        return {
            .tileWidth = 16,
            .tileHeight = 16,
            .depthSlices = 4,
            .maximumLightsPerCluster = 8,
            .maximumLightReferences = 1'024,
            .maximumDirectionalLights = 2,
            .maximumFallbackLights = 2,
        };
    }

    bool logarithmicDepthAndResizeAreFrozen() {
        CHECK(Iridium::clusterDepthSlice(1.0f, 1.0f, 100.0f, 4) == 0);
        CHECK(Iridium::clusterDepthSlice(10.0f, 1.0f, 100.0f, 4) == 2);
        CHECK(Iridium::clusterDepthSlice(100.0f, 1.0f, 100.0f, 4) == 3);
        const auto dimensions = Iridium::clusterGridDimensions(
            smallConfig(), frame(65, 33));
        CHECK(dimensions.tilesX == 5);
        CHECK(dimensions.tilesY == 3);
        CHECK(dimensions.depthSlices == 4);
        CHECK(dimensions.clusterCount() == 60);
        return true;
    }

    bool emptyDirectionalAndLocalProductsAreValid() {
        Iridium::ClusteredLightAssigner assigner(smallConfig());
        Lights empty;
        const auto zero = assigner.build(empty.packet(), frame());
        CHECK(zero.headers.size() == 64);
        CHECK(zero.localLightSlots.empty());
        CHECK(zero.globalDirectionalSlots.empty());
        CHECK(!zero.usesFallback());

        Lights lights;
        const uint32_t directional = lights.add(
            Iridium::PackedGpuLightType::Directional, {}, 0.0f);
        const uint32_t point = lights.add(
            Iridium::PackedGpuLightType::Point, { 0.0f, 0.0f, -10.0f }, 2.0f);
        const auto product = assigner.build(lights.packet(), frame());
        CHECK(product.globalDirectionalSlots ==
            std::vector<uint32_t>{ directional });
        CHECK(!product.localLightSlots.empty());
        CHECK(product.stats.localLightCount == 1);
        CHECK(product.stats.publishedLightReferences ==
            product.localLightSlots.size());
        CHECK(std::ranges::all_of(product.localLightSlots,
            [point](uint32_t slot) { return slot == point; }));
        for (const auto header : product.headers) {
            CHECK(static_cast<uint64_t>(header.offset) + header.count <=
                product.localLightSlots.size());
        }
        CHECK(product == assigner.build(lights.packet(), frame()));
        return true;
    }

    bool behindCameraAndNearPlaneBehaviorIsConservative() {
        Iridium::ClusteredLightAssigner assigner(smallConfig());
        Lights behind;
        behind.add(Iridium::PackedGpuLightType::Point,
            { 0.0f, 0.0f, 10.0f }, 1.0f);
        const auto rejected = assigner.build(behind.packet(), frame());
        CHECK(rejected.localLightSlots.empty());

        Lights crossing;
        crossing.add(Iridium::PackedGpuLightType::Point,
            { 0.0f, 0.0f, -1.25f }, 1.0f);
        const auto conservative = assigner.build(crossing.packet(), frame());
        CHECK(conservative.stats.clustersUsed >= 16);
        CHECK(conservative.stats.maximumClusterOccupancy == 1);
        return true;
    }

    bool overflowUsesWholeDeterministicFallback() {
        auto config = smallConfig();
        config.maximumLightsPerCluster = 1;
        Iridium::ClusteredLightAssigner assigner(config);
        Lights lights;
        lights.add(Iridium::PackedGpuLightType::Point,
            { 0.0f, 0.0f, -8.0f }, 3.0f, 0, true, 5'000.0f);
        lights.add(Iridium::PackedGpuLightType::Point,
            { 0.0f, 0.0f, -8.0f }, 3.0f, 10, false, 1'000.0f);
        const auto product = assigner.build(lights.packet(), frame());
        CHECK(product.usesFallback());
        CHECK(product.stats.overflow ==
            Iridium::ClusterOverflowCode::PerClusterCapacity);
        CHECK(product.localLightSlots.empty());
        CHECK(product.globalDirectionalSlots.empty());
        CHECK(std::ranges::all_of(product.headers,
            [](auto header) { return header.count == 0; }));
        CHECK(product.fallbackLightSlots == std::vector<uint32_t>({ 1, 0 }));
        CHECK(product == assigner.build(lights.packet(), frame()));
        return true;
    }

    bool directionalAndGlobalReferenceOverflowAreDistinct() {
        auto directionalConfig = smallConfig();
        directionalConfig.maximumDirectionalLights = 1;
        Iridium::ClusteredLightAssigner directionalAssigner(directionalConfig);
        Lights directionals;
        directionals.add(Iridium::PackedGpuLightType::Directional, {}, 0.0f);
        directionals.add(Iridium::PackedGpuLightType::Directional, {}, 0.0f);
        CHECK(directionalAssigner.build(directionals.packet(), frame())
            .stats.overflow ==
            Iridium::ClusterOverflowCode::DirectionalCapacity);

        auto referenceConfig = smallConfig();
        referenceConfig.maximumLightReferences = 1;
        Iridium::ClusteredLightAssigner referenceAssigner(referenceConfig);
        Lights local;
        local.add(Iridium::PackedGpuLightType::Point,
            { 0.0f, 0.0f, -1.25f }, 1.0f);
        CHECK(referenceAssigner.build(local.packet(), frame()).stats.overflow ==
            Iridium::ClusterOverflowCode::GlobalReferenceCapacity);
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "log depth and resize", logarithmicDepthAndResizeAreFrozen },
        std::pair{ "empty/global/local", emptyDirectionalAndLocalProductsAreValid },
        std::pair{ "camera boundaries", behindCameraAndNearPlaneBehaviorIsConservative },
        std::pair{ "whole fallback", overflowUsesWholeDeterministicFallback },
        std::pair{ "overflow codes", directionalAndGlobalReferenceOverflowAreDistinct },
    };
    for (const auto& [name, test] : tests) {
        if (!test()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
