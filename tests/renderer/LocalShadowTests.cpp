#include "renderer/lighting/LocalShadow.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/rhi/ShadowSettings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {
    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n'; \
        return false; } } while (false)

    SceneEntityUuid uuid(uint64_t suffix) {
        char text[64]{};
        std::snprintf(text, sizeof(text),
            "019fb73d-5a70-7000-8000-%012llu",
            static_cast<unsigned long long>(suffix));
        return *SceneEntityUuid::parse(text);
    }

    LocalShadowRequest request(uint64_t owner, LocalShadowKind kind,
        uint32_t quality, int32_t priority = 0, float contribution = 1.0f) {
        return { uuid(owner), kind, static_cast<uint32_t>(owner), quality,
            priority, contribution };
    }

    bool qualityAndRankingAreFrozen() {
        CHECK(localShadowResolution(LocalShadowKind::Spot, 0) == 512u);
        CHECK(localShadowResolution(LocalShadowKind::Spot, 1) == 1024u);
        CHECK(localShadowResolution(LocalShadowKind::Spot, 2) == 2048u);
        CHECK(localShadowResolution(LocalShadowKind::Spot, 3) == 4096u);
        CHECK(localShadowResolution(LocalShadowKind::Point, 0) == 256u);
        CHECK(localShadowResolution(LocalShadowKind::Point, 1) == 512u);
        CHECK(localShadowResolution(LocalShadowKind::Point, 2) == 512u);
        CHECK(localShadowResolution(LocalShadowKind::Point, 3) == 1024u);
        CHECK(localShadowRequestPrecedes(request(1, LocalShadowKind::Spot, 3),
            request(2, LocalShadowKind::Spot, 2, 100)));
        CHECK(localShadowRequestPrecedes(request(1, LocalShadowKind::Spot, 2, 5),
            request(2, LocalShadowKind::Spot, 2, 4, 100.0f)));
        CHECK(localShadowRequestPrecedes(request(1, LocalShadowKind::Spot, 2, 5, 2),
            request(2, LocalShadowKind::Spot, 2, 5, 1)));
        return true;
    }

    bool requestsExtractFromSharedLightRecordsInStableOrder() {
        auto packed = [](PackedGpuLightType type, uint32_t quality,
            glm::vec3 position, float intensity) {
            PackedGpuLight value{};
            value.positionRange = glm::vec4(position, 20.0f);
            value.colorIntensity = glm::vec4(1.0f, 0.8f, 0.6f, intensity);
            value.shapeMetadata.x = 0.25f;
            const uint32_t metadata = static_cast<uint32_t>(type) |
                PackedGpuLightCastsShadows |
                (quality << PackedGpuLightShadowQualityShift);
            value.shapeMetadata.z = std::bit_cast<float>(metadata);
            return value;
        };
        const std::vector records{
            packed(PackedGpuLightType::Point, 2, { 0, 0, 2 }, 100),
            packed(PackedGpuLightType::Spot, 3, { 0, 0, 10 }, 10),
            packed(PackedGpuLightType::Directional, 3, { 0, 0, 0 }, 1000),
        };
        const std::vector<LightSelectionMetadata> metadata{
            { uuid(2), 5, true }, { uuid(1), 0, true }, { uuid(3), 100, true },
        };
        const std::vector<uint32_t> active{ 2, 0, 1 };
        LightingFramePacket packet{ .records = records,
            .activeSlots = active, .selectionMetadata = metadata };
        const auto requests = buildLocalShadowRequests(packet, { 0, 0, 0 });
        CHECK(requests.size() == 2u);
        CHECK(requests[0].owner == uuid(1));
        CHECK(requests[0].kind == LocalShadowKind::Spot);
        CHECK(requests[1].owner == uuid(2));
        CHECK(requests[1].kind == LocalShadowKind::Point);
        CHECK(requests[1].conservativeContribution >
            requests[0].conservativeContribution);
        return true;
    }

    bool spotAtlasIsStableDeterministicAndCapacityBounded() {
        StableSpotShadowAtlas atlas({ .atlasResolution = 4096,
            .minimumTileResolution = 512, .guardTexels = 4 });
        std::vector<LocalShadowRequest> requests;
        for (uint64_t index = 1; index <= 5; ++index)
            requests.push_back(request(index, LocalShadowKind::Spot, 2,
                static_cast<int32_t>(10 - index)));
        auto stats = atlas.reconcile(requests);
        CHECK(stats.requested == 5u);
        CHECK(stats.allocated == 4u);
        CHECK(stats.omitted == 1u);
        const std::vector<SpotShadowTile> first(
            atlas.allocations().begin(), atlas.allocations().end());

        std::reverse(requests.begin(), requests.end());
        stats = atlas.reconcile(requests);
        CHECK(stats.reused == 4u);
        CHECK(std::equal(first.begin(), first.end(), atlas.allocations().begin()));

        // A stronger newcomer deterministically replaces the weakest allocation.
        requests.pop_back();
        requests.push_back(request(99, LocalShadowKind::Spot, 3, 100));
        stats = atlas.reconcile(requests);
        CHECK(stats.allocated >= 1u);
        CHECK(std::any_of(atlas.allocations().begin(), atlas.allocations().end(),
            [](const SpotShadowTile& tile) { return tile.owner == uuid(99); }));
        for (const SpotShadowTile& tile : atlas.allocations()) {
            CHECK(tile.x % tile.size == 0u);
            CHECK(tile.y % tile.size == 0u);
            CHECK(tile.x + tile.size <= 4096u);
            CHECK(tile.y + tile.size <= 4096u);
            CHECK(tile.guardTexels == 4u);
        }
        return true;
    }

    bool maximumSpotAtlasMatchesGpuEntryCapacity() {
        StableSpotShadowAtlas atlas({ .atlasResolution = 8192,
            .minimumTileResolution = 512, .guardTexels = 4 });
        std::vector<LocalShadowRequest> requests;
        requests.reserve(kSpotShadowEntryCapacity + 1u);
        for (uint32_t index = 1; index <= kSpotShadowEntryCapacity + 1u;
            ++index) {
            requests.push_back(request(index, LocalShadowKind::Spot, 0,
                static_cast<int32_t>(kSpotShadowEntryCapacity + 1u - index)));
        }
        const LocalShadowAllocationStats stats = atlas.reconcile(requests);
        CHECK(kSpotShadowEntryCapacity == 256u);
        CHECK(stats.allocated == kSpotShadowEntryCapacity);
        CHECK(stats.omitted == 1u);
        CHECK(atlas.allocations().size() == kSpotShadowEntryCapacity);
        return true;
    }

    bool pointPoolsRetainSlotsAndEvictByRank() {
        StablePointShadowPools pools({ .cubeCapacity = { 2, 2, 1 } });
        std::vector requests{
            request(1, LocalShadowKind::Point, 2, 10),
            request(2, LocalShadowKind::Point, 2, 9),
            request(3, LocalShadowKind::Point, 2, 8),
            request(4, LocalShadowKind::Point, 3, 7),
        };
        auto stats = pools.reconcile(requests);
        CHECK(stats.allocated == 3u);
        CHECK(stats.omitted == 1u);
        const std::vector<PointShadowSlot> first(
            pools.allocations().begin(), pools.allocations().end());
        std::reverse(requests.begin(), requests.end());
        stats = pools.reconcile(requests);
        CHECK(stats.reused == 3u);
        CHECK(std::equal(first.begin(), first.end(), pools.allocations().begin()));

        requests.push_back(request(99, LocalShadowKind::Point, 2, 100));
        stats = pools.reconcile(requests);
        CHECK(stats.evicted == 1u);
        CHECK(std::any_of(pools.allocations().begin(), pools.allocations().end(),
            [](const PointShadowSlot& slot) { return slot.owner == uuid(99); }));

        // A quality change moves the owner to a compatible tier and is explicit.
        auto ownerOne = std::find_if(requests.begin(), requests.end(),
            [](const LocalShadowRequest& value) { return value.owner == uuid(1); });
        ownerOne->quality = 3;
        stats = pools.reconcile(requests);
        CHECK(stats.relocated >= 1u);
        CHECK(std::any_of(pools.allocations().begin(), pools.allocations().end(),
            [](const PointShadowSlot& slot) {
                return slot.owner == uuid(1) && slot.resolution == 1024u;
            }));
        return true;
    }

    bool maximumPointPoolsMatchGpuEntryCapacity() {
        StablePointShadowPools pools({ .cubeCapacity = {
            kPointShadowPool256Capacity, kPointShadowPool512Capacity,
            kPointShadowPool1024Capacity } });
        std::vector<LocalShadowRequest> requests;
        uint64_t owner = 1;
        for (uint32_t index = 0; index < kPointShadowPool256Capacity; ++index)
            requests.push_back(request(owner++, LocalShadowKind::Point, 0));
        for (uint32_t index = 0; index < kPointShadowPool512Capacity; ++index)
            requests.push_back(request(owner++, LocalShadowKind::Point, 2));
        for (uint32_t index = 0; index < kPointShadowPool1024Capacity; ++index)
            requests.push_back(request(owner++, LocalShadowKind::Point, 3));
        requests.push_back(request(owner, LocalShadowKind::Point, 0, -1));
        const LocalShadowAllocationStats stats = pools.reconcile(requests);
        CHECK(kPointShadowEntryCapacity == 56u);
        CHECK(stats.allocated == kPointShadowEntryCapacity);
        CHECK(stats.omitted == 1u);
        CHECK(pools.allocations().size() == kPointShadowEntryCapacity);
        return true;
    }

    bool pointFacesUseFrozenOrientationAndVulkanDepth() {
        const auto faces = buildPointShadowFaces({ 1.0f, 2.0f, 3.0f },
            0.1f, 20.0f);
        const std::array expected{
            glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
            glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
            glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        };
        for (size_t index = 0; index < faces.size(); ++index) {
            CHECK(faces[index].direction == expected[index]);
            const glm::vec3 world = glm::vec3(1, 2, 3) +
                faces[index].direction;
            const glm::vec4 clip = faces[index].worldToShadowClip *
                glm::vec4(world, 1.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            CHECK(std::abs(ndc.x) < 1.0e-5f);
            CHECK(std::abs(ndc.y) < 1.0e-5f);
            CHECK(ndc.z >= 0.0f && ndc.z <= 1.0f);
        }
        return true;
    }

    bool spotProjectionUsesOuterConeAndVulkanDepth() {
        const glm::vec3 position{ 2.0f, 3.0f, 4.0f };
        const float outerRadians = glm::radians(35.0f);
        const auto projection = buildSpotShadowProjection(position,
            { 0.0f, 0.0f, 1.0f }, std::cos(outerRadians), 0.1f, 25.0f);
        CHECK(glm::length(projection.lightForward - glm::vec3(0, 0, 1)) <
            1.0e-6f);
        CHECK(std::abs(projection.outerConeRadians - outerRadians) < 1.0e-6f);
        const float depth = 10.0f;
        const glm::vec3 center = position + projection.lightForward * depth;
        const glm::vec3 boundary = center + projection.lightRight *
            std::tan(outerRadians) * depth;
        const glm::vec4 centerClip = projection.worldToShadowClip *
            glm::vec4(center, 1.0f);
        const glm::vec4 boundaryClip = projection.worldToShadowClip *
            glm::vec4(boundary, 1.0f);
        const glm::vec3 centerNdc = glm::vec3(centerClip) / centerClip.w;
        const glm::vec3 boundaryNdc = glm::vec3(boundaryClip) / boundaryClip.w;
        CHECK(std::abs(centerNdc.x) < 1.0e-6f);
        CHECK(std::abs(centerNdc.y) < 1.0e-6f);
        CHECK(centerNdc.z >= 0.0f && centerNdc.z <= 1.0f);
        CHECK(std::abs(std::abs(boundaryNdc.x) - 1.0f) < 1.0e-5f);
        return true;
    }

    bool spotCasterBoundsAreConservativeAndClipCulled() {
        glm::mat4 world(1.0f);
        world = glm::translate(world, glm::vec3(2.0f, 3.0f, 4.0f));
        world = glm::scale(world, glm::vec3(2.0f, 3.0f, 4.0f));
        const ShadowCasterSphere transformed = transformShadowCasterSphere(
            { 1.0f, 0.0f, 0.0f }, 0.5f, world);
        CHECK(glm::length(transformed.center - glm::vec3(4, 3, 4)) < 1.0e-6f);
        CHECK(std::abs(transformed.radius - 2.0f) < 1.0e-6f);

        const glm::vec3 lightPosition{ 0.0f, 0.0f, 5.0f };
        const SpotShadowProjection projection = buildSpotShadowProjection(
            lightPosition, { 0.0f, 0.0f, -1.0f },
            std::cos(glm::radians(30.0f)), 0.1f, 20.0f);
        CHECK(shadowCasterSphereIntersectsClipVolume(
            projection.worldToShadowClip, { 0, 0, 0 }, 0.5f));
        CHECK(!shadowCasterSphereIntersectsClipVolume(
            projection.worldToShadowClip, { 15, 0, 0 }, 0.5f));
        CHECK(!shadowCasterSphereIntersectsClipVolume(
            projection.worldToShadowClip, { 0, 0, 8 }, 0.5f));
        CHECK(shadowCasterSphereIntersectsClipVolume(
            projection.worldToShadowClip, { 15, 0, 0 }, -1.0f));
        CHECK(transformShadowCasterSphere({ 0, 0, 0 }, 0.0f, world).
            radius < 0.0f);
        return true;
    }

    LocalShadowCacheInput cacheInput(uint64_t owner, LocalShadowKind kind,
        uint32_t quality, int32_t priority = 0) {
        return {
            .request = request(owner, kind, quality, priority),
            .resolution = localShadowResolution(kind, quality),
            .allocationRevision = 1,
            .lightRevision = 1,
            .casterRevision = 1,
            .projectionRevision = 1,
            .pipelineRevision = 1,
        };
    }

    bool schedulerBudgetsUpdatesAndBoundsCompatibleStaleMaps() {
        // The budget can refresh either one 512 point cube or one 2048 spot map,
        // but not both in the same frame.
        LocalShadowCacheScheduler scheduler({
            .maximumRenderedTexels = 2048ull * 2048ull,
            .maximumCompatibleStaleFrames = 2,
        });
        std::vector inputs{
            cacheInput(1, LocalShadowKind::Point, 2, 10),
            cacheInput(2, LocalShadowKind::Spot, 2, 9),
        };
        const auto& cold = scheduler.schedule(inputs);
        CHECK(cold.stats.updates == 1u);
        CHECK(cold.stats.unshadowed == 1u);
        CHECK(cold.entries[0].update);
        CHECK(!cold.entries[1].sampleable);
        scheduler.markScheduledRendered();

        // The point is now a hit, so the previously omitted spot uses the budget.
        const auto& fill = scheduler.schedule(inputs);
        CHECK(fill.stats.cacheHits == 1u);
        CHECK(fill.stats.updates == 1u);
        CHECK(fill.entries[1].update);
        scheduler.markScheduledRendered();

        ++inputs[0].casterRevision;
        ++inputs[1].casterRevision;
        const auto& casterDirty = scheduler.schedule(inputs);
        CHECK(casterDirty.entries[0].update);
        CHECK(casterDirty.entries[1].stale);
        CHECK(casterDirty.entries[1].staleAgeFrames == 1u);
        scheduler.markScheduledRendered();

        ++inputs[0].casterRevision;
        const auto& staleTwo = scheduler.schedule(inputs);
        CHECK(staleTwo.entries[1].stale);
        CHECK(staleTwo.entries[1].staleAgeFrames == 2u);
        scheduler.markScheduledRendered();
        ++inputs[0].casterRevision;
        const auto& staleExpired = scheduler.schedule(inputs);
        CHECK(!staleExpired.entries[1].sampleable);
        CHECK(staleExpired.stats.unshadowed == 1u);
        scheduler.markScheduledRendered();
        return true;
    }

    bool incompatibleLocalHistoryIsNeverSampled() {
        LocalShadowCacheScheduler scheduler({
            .maximumRenderedTexels = 2048ull * 2048ull,
            .maximumCompatibleStaleFrames = 10,
        });
        std::vector inputs{ cacheInput(1, LocalShadowKind::Spot, 2) };
        auto schedule = scheduler.schedule(inputs);
        CHECK(schedule.entries[0].update);
        scheduler.markScheduledRendered();

        // Exhaust the budget with a stronger new request; a moved light must not
        // retain its old projection even though compatible-caster stale age allows it.
        ++inputs[0].lightRevision;
        inputs.push_back(cacheInput(2, LocalShadowKind::Spot, 2, 100));
        schedule = scheduler.schedule(inputs);
        CHECK(schedule.entries[0].owner == uuid(2));
        CHECK(schedule.entries[0].update);
        CHECK(schedule.entries[1].dirtyReason ==
            LocalShadowDirtyReason::LightChanged);
        CHECK(!schedule.entries[1].sampleable);
        scheduler.markScheduledRendered();
        return true;
    }
}

int main() {
    const struct { const char* name; bool (*run)(); } tests[]{
        { "quality and ranking are frozen", qualityAndRankingAreFrozen },
        { "requests extract from shared light records in stable order",
            requestsExtractFromSharedLightRecordsInStableOrder },
        { "spot atlas is stable deterministic and capacity bounded",
            spotAtlasIsStableDeterministicAndCapacityBounded },
        { "maximum spot atlas matches GPU entry capacity",
            maximumSpotAtlasMatchesGpuEntryCapacity },
        { "point pools retain slots and evict by rank",
            pointPoolsRetainSlotsAndEvictByRank },
        { "maximum point pools match GPU entry capacity",
            maximumPointPoolsMatchGpuEntryCapacity },
        { "point faces use frozen orientation and Vulkan depth",
            pointFacesUseFrozenOrientationAndVulkanDepth },
        { "spot projection uses outer cone and Vulkan depth",
            spotProjectionUsesOuterConeAndVulkanDepth },
        { "spot caster bounds are conservative and clip culled",
            spotCasterBoundsAreConservativeAndClipCulled },
        { "scheduler budgets updates and bounds compatible stale maps",
            schedulerBudgetsUpdatesAndBoundsCompatibleStaleMaps },
        { "incompatible local history is never sampled",
            incompatibleLocalHistoryIsNeverSampled },
    };
    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.name << '\n';
        if (!test.run()) return 1;
        std::cout << "[       OK ] " << test.name << '\n';
    }
    return 0;
}
