#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/rhi/ShadowFiltering.h"
#include "renderer/rhi/VisibilityContracts.h"

#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "check failed: " #condition " at line " \
                    << __LINE__ << '\n'; \
                return false; \
            } \
        } while (false)

    SceneEntityUuid uuid(const char* text) {
        return *SceneEntityUuid::parse(text);
    }

    PackedGpuLight light(PackedGpuLightType type, bool shadows,
        uint32_t quality, glm::vec3 direction) {
        uint32_t metadata = static_cast<uint32_t>(type) |
            (quality << PackedGpuLightShadowQualityShift);
        if (shadows) metadata |= PackedGpuLightCastsShadows;
        PackedGpuLight result{};
        result.directionOuterCos = glm::vec4(direction, 0.0f);
        result.shapeMetadata.z = std::bit_cast<float>(metadata);
        return result;
    }

    bool selectionIsPriorityThenStableIdentity() {
        std::vector<PackedGpuLight> records{
            light(PackedGpuLightType::Directional, true, 2,
                { 0.0f, -1.0f, 0.0f }),
            light(PackedGpuLightType::Directional, true, 3,
                { 1.0f, -1.0f, 0.0f }),
            light(PackedGpuLightType::Point, true, 3,
                { 0.0f, 0.0f, 0.0f }),
            light(PackedGpuLightType::Directional, false, 3,
                { -1.0f, -1.0f, 0.0f }),
        };
        std::vector<LightSelectionMetadata> metadata{
            { uuid("019fb73d-5a60-7000-8000-000000000002"), 10, true },
            { uuid("019fb73d-5a60-7000-8000-000000000001"), 10, true },
            { uuid("019fb73d-5a60-7000-8000-000000000003"), 100, true },
            { uuid("019fb73d-5a60-7000-8000-000000000004"), 100, false },
        };
        const std::vector<uint32_t> active{ 0, 1, 2, 3 };
        LightingFramePacket packet{
            .records = records,
            .activeSlots = active,
            .selectionMetadata = metadata,
        };
        const auto selected = selectDirectionalShadowLight(packet);
        CHECK(selected.has_value());
        CHECK(selected->lightSlot == 1);
        CHECK(selected->owner == metadata[1].owner);
        CHECK(selected->quality == 3);
        CHECK(selected->omittedShadowDirectionalLights == 1);
        CHECK(glm::length(selected->lightForward - glm::vec3(1, -1, 0)) <
            1.0e-6f);
        const auto selectedPair = selectDirectionalShadowLights(packet, 2);
        CHECK(selectedPair.size() == 2);
        CHECK(selectedPair[0].lightSlot == 1);
        CHECK(selectedPair[1].lightSlot == 0);
        CHECK(selectedPair[0].omittedShadowDirectionalLights == 0);
        CHECK(selectedPair[1].omittedShadowDirectionalLights == 0);

        records[0] = light(PackedGpuLightType::Directional, false, 2,
            { 0.0f, -1.0f, 0.0f });
        records[1] = light(PackedGpuLightType::Directional, false, 3,
            { 0.0f, -1.0f, 0.0f });
        CHECK(!selectDirectionalShadowLight(packet).has_value());
        return true;
    }

    bool cascadesContainTheirFrustumAndUseVulkanDepth() {
        const DirectionalShadowCamera camera{
            .position = { 3.0f, 2.0f, 5.0f },
            .forward = glm::normalize(glm::vec3(-0.1f, -0.05f, -1.0f)),
            .up = { 0.0f, 1.0f, 0.0f },
            .verticalFovRadians = glm::radians(65.0f),
            .aspectRatio = 16.0f / 9.0f,
            .nearPlane = 0.2f,
            .farPlane = 500.0f,
        };
        const glm::vec3 lightForward = glm::normalize(
            glm::vec3(0.4f, -1.0f, 0.2f));
        const DirectionalShadowConfig config{};
        const DirectionalShadowCascadePlan plan =
            buildDirectionalShadowCascades(camera, lightForward, config);
        float previous = camera.nearPlane;
        const glm::vec3 forward = glm::normalize(camera.forward);
        const glm::vec3 right = glm::normalize(glm::cross(forward, camera.up));
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));
        const float tangent = std::tan(camera.verticalFovRadians * 0.5f);
        for (uint32_t index = 0; index < kDirectionalShadowCascadeCount; ++index) {
            const DirectionalShadowCascade& cascade = plan.cascades[index];
            CHECK(cascade.splitNear == previous);
            CHECK(cascade.splitFar > cascade.splitNear);
            CHECK(cascade.radiusMeters > 0.0f);
            CHECK(std::abs(cascade.worldUnitsPerTexel -
                2.0f * cascade.radiusMeters / config.resolution) < 1.0e-6f);
            for (float distance : { cascade.splitNear, cascade.splitFar }) {
                const float halfHeight = tangent * distance;
                const float halfWidth = halfHeight * camera.aspectRatio;
                const glm::vec3 center = camera.position + forward * distance;
                for (float vertical : { -1.0f, 1.0f })
                    for (float horizontal : { -1.0f, 1.0f }) {
                        const glm::vec3 world = center +
                            right * (horizontal * halfWidth) +
                            up * (vertical * halfHeight);
                        const glm::vec4 clip = cascade.worldToShadowClip *
                            glm::vec4(world, 1.0f);
                        CHECK(std::abs(clip.x) <= 1.0001f);
                        CHECK(std::abs(clip.y) <= 1.0001f);
                        CHECK(clip.z >= -1.0e-5f && clip.z <= 1.00001f);
                        CHECK(clip.w == 1.0f);
                    }
            }
            previous = cascade.splitFar;
        }
        CHECK(std::abs(plan.splitFar.back() - camera.farPlane) < 1.0e-3f);
        return true;
    }

    bool subTexelLateralMotionIsStable() {
        const DirectionalShadowCamera camera{
            .position = { 0.0f, 1.0f, 4.0f },
            .forward = { 0.0f, 0.0f, -1.0f },
            .up = { 0.0f, 1.0f, 0.0f },
            .verticalFovRadians = glm::radians(60.0f),
            .aspectRatio = 16.0f / 9.0f,
            .nearPlane = 0.1f,
            .farPlane = 300.0f,
        };
        const glm::vec3 lightForward = glm::normalize(
            glm::vec3(0.4f, -1.0f, 0.2f));
        const DirectionalShadowCascadePlan baseline =
            buildDirectionalShadowCascades(camera, lightForward);
        const glm::vec3 upReference = std::abs(lightForward.y) < 0.99f
            ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
        const glm::vec3 lateral = glm::normalize(
            glm::cross(upReference, lightForward));
        const float motion = baseline.cascades[0].worldUnitsPerTexel * 0.01f;
        DirectionalShadowCamera plus = camera;
        DirectionalShadowCamera minus = camera;
        plus.position += lateral * motion;
        minus.position -= lateral * motion;
        const auto plusPlan = buildDirectionalShadowCascades(plus, lightForward);
        const auto minusPlan = buildDirectionalShadowCascades(minus, lightForward);
        const bool plusStable = plusPlan.cascades[0].worldToShadowClip ==
            baseline.cascades[0].worldToShadowClip;
        const bool minusStable = minusPlan.cascades[0].worldToShadowClip ==
            baseline.cascades[0].worldToShadowClip;
        CHECK(plusStable || minusStable);
        return true;
    }

    bool cacheBudgetsUpdatesAndNeverSamplesChangedProjection() {
        const DirectionalShadowCamera camera{
            .position = { 0.0f, 1.0f, 4.0f },
            .forward = { 0.0f, 0.0f, -1.0f },
            .up = { 0.0f, 1.0f, 0.0f },
            .verticalFovRadians = glm::radians(60.0f),
            .aspectRatio = 16.0f / 9.0f,
            .nearPlane = 0.1f,
            .farPlane = 300.0f,
        };
        DirectionalShadowCache cache;
        DirectionalShadowCacheInput input{
            .selection = {
                .owner = uuid("019fb73d-5a60-7000-8000-000000000001"),
                .lightSlot = 7,
                .quality = 2,
                .priority = 5,
                .lightForward = glm::normalize(glm::vec3(0.4f, -1.0f, 0.2f)),
            },
            .plan = buildDirectionalShadowCascades(camera,
                glm::normalize(glm::vec3(0.4f, -1.0f, 0.2f))),
            .lightRevision = 10,
            .casterRevision = 20,
            .pipelineRevision = 1,
        };
        auto schedule = cache.schedule(input, 2);
        CHECK(schedule.dirtyMask == 0xfu);
        CHECK(schedule.updateMask == 0x3u);
        CHECK(schedule.sampleableMask == 0x3u);
        CHECK(schedule.deferredCount == 2);
        cache.markRendered(schedule.updateMask);

        schedule = cache.schedule(input, 4);
        CHECK(schedule.dirtyMask == 0xcu);
        CHECK(schedule.updateMask == 0xcu);
        CHECK(schedule.sampleableMask == 0xfu);
        CHECK(schedule.cacheHitCount == 2);
        cache.markRendered(schedule.updateMask);

        schedule = cache.schedule(input, 0);
        CHECK(schedule.dirtyMask == 0u);
        CHECK(schedule.sampleableMask == 0xfu);
        CHECK(schedule.cacheHitCount == 4);
        cache.markRendered(0);

        ++input.lightRevision;
        schedule = cache.schedule(input, 1);
        CHECK(schedule.dirtyMask == 0xfu);
        CHECK(schedule.updateMask == 0x1u);
        CHECK(schedule.sampleableMask == 0x1u);
        CHECK(schedule.deferredCount == 3);
        cache.markRendered(schedule.updateMask);
        return true;
    }

    bool filterBlendBiasAndInvalidInputsAreFrozen() {
        const DirectionalShadowCamera camera{
            .position = { 0.0f, 1.0f, 4.0f },
            .forward = { 0.0f, 0.0f, -1.0f },
            .up = { 0.0f, 1.0f, 0.0f },
            .verticalFovRadians = glm::radians(60.0f),
            .aspectRatio = 16.0f / 9.0f,
            .nearPlane = 0.1f,
            .farPlane = 300.0f,
        };
        const auto plan = buildDirectionalShadowCascades(camera,
            glm::normalize(glm::vec3(0.4f, -1.0f, 0.2f)));
        const float split = plan.splitFar[0];
        const float blendStart = split - split * 0.1f;
        auto blend = directionalShadowCascadeBlend(plan,
            blendStart - 0.01f, 0xfu);
        CHECK(blend.primaryCascade == 0u);
        CHECK(blend.secondaryCascade == 1u);
        CHECK(blend.secondaryWeight == 0.0f);
        blend = directionalShadowCascadeBlend(plan,
            blendStart + (split - blendStart) * 0.5f, 0xfu);
        CHECK(std::abs(blend.secondaryWeight - 0.5f) < 1.0e-5f);
        blend = directionalShadowCascadeBlend(plan, split, 0x1u);
        CHECK(blend.secondaryCascade == 0u);
        CHECK(blend.secondaryWeight == 0.0f);

        float filterSum = 0.0f;
        for (int32_t y = -2; y <= 2; ++y)
            for (int32_t x = -2; x <= 2; ++x)
                filterSum += directionalShadowTentWeight(x, y);
        CHECK(std::abs(filterSum - 1.0f) < 1.0e-6f);
        CHECK(std::abs(directionalShadowTentWeight(0, 0) - 9.0f / 81.0f) <
            1.0e-7f);
        CHECK(directionalShadowTentWeight(3, 0) == 0.0f);

        const glm::vec3 offset = directionalShadowNormalOffset(
            { 1.0f, 2.0f, 3.0f }, { 0.0f, 1.0f, 0.0f }, 0.25f, 1.5f);
        CHECK(offset == glm::vec3(1.0f, 2.375f, 3.0f));

        bool rejected = false;
        try {
            DirectionalShadowConfig invalid{};
            invalid.resolution = 0;
            (void)buildDirectionalShadowCascades(camera,
                { 0.0f, -1.0f, 0.0f }, invalid);
        }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool cacheKeysDistinguishCastersCameraLightAndOwnerNotReceivers() {
        const DirectionalShadowCamera camera{
            .position = { 0.0f, 1.0f, 4.0f },
            .forward = { 0.0f, 0.0f, -1.0f },
            .up = { 0.0f, 1.0f, 0.0f },
            .verticalFovRadians = glm::radians(60.0f),
            .aspectRatio = 16.0f / 9.0f,
            .nearPlane = 0.1f,
            .farPlane = 300.0f,
        };
        const glm::vec3 lightDirection = glm::normalize(
            glm::vec3(0.4f, -1.0f, 0.2f));
        DirectionalShadowCacheInput input{
            .selection = {
                .owner = uuid("019fb73d-5a60-7000-8000-000000000001"),
                .lightSlot = 0,
                .lightForward = lightDirection,
            },
            .plan = buildDirectionalShadowCascades(camera, lightDirection),
            .lightRevision = 1,
            .casterRevision = 2,
            .pipelineRevision = 3,
        };
        DirectionalShadowCache cache;
        auto schedule = cache.schedule(input, 4);
        cache.markRendered(schedule.updateMask);

        // Receiver-only changes are deliberately absent from the cache key.
        schedule = cache.schedule(input, 4);
        CHECK(schedule.dirtyMask == 0u);
        CHECK(schedule.cacheHitCount == 4u);
        cache.markRendered(0u);

        ++input.casterRevision;
        schedule = cache.schedule(input, 4);
        CHECK(schedule.dirtyMask == 0xfu);
        cache.markRendered(schedule.updateMask);

        ++input.lightRevision;
        schedule = cache.schedule(input, 4);
        CHECK(schedule.dirtyMask == 0xfu);
        cache.markRendered(schedule.updateMask);

        DirectionalShadowCamera movedCamera = camera;
        movedCamera.position.x += 2.0f;
        input.plan = buildDirectionalShadowCascades(movedCamera, lightDirection);
        schedule = cache.schedule(input, 4);
        CHECK(schedule.dirtyMask != 0u);
        cache.markRendered(schedule.updateMask);

        input.selection.owner =
            uuid("019fb73d-5a60-7000-8000-000000000002");
        schedule = cache.schedule(input, 4);
        CHECK(schedule.dirtyMask == 0xfu);
        cache.markRendered(schedule.updateMask);
        return true;
    }

    bool contactHardeningUsesPhysicalEmitterExtent() {
        const ShadowFilterProfile profile = shadowFilterProfile(
            ShadowQualityProfile::Ultra);
        const ShadowContactFilterPlan contact = directionalShadowFilterPlan(
            0.5f, 0.5f, 200.0f, 0.02f, 0.535f, profile, 48.0f);
        CHECK(!contact.contactHardening);
        CHECK(contact.penumbraRadiusTexels == 0.0f);

        const ShadowContactFilterPlan nearReceiver =
            directionalShadowFilterPlan(0.55f, 0.5f, 200.0f, 0.02f,
                0.535f, profile, 48.0f);
        const ShadowContactFilterPlan farReceiver =
            directionalShadowFilterPlan(0.75f, 0.5f, 200.0f, 0.02f,
                0.535f, profile, 48.0f);
        const ShadowContactFilterPlan largerEmitter =
            directionalShadowFilterPlan(0.75f, 0.5f, 200.0f, 0.02f,
                1.07f, profile, 48.0f);
        CHECK(nearReceiver.contactHardening);
        CHECK(farReceiver.penumbraRadiusTexels >
            nearReceiver.penumbraRadiusTexels);
        CHECK(largerEmitter.penumbraRadiusTexels >
            farReceiver.penumbraRadiusTexels);
        CHECK(largerEmitter.penumbraRadiusTexels <= 48.0f);

        const ShadowContactFilterPlan pointLimit = localShadowFilterPlan(
            10.0f, 5.0f, 0.0f, 512.0f, profile, 48.0f);
        CHECK(!pointLimit.contactHardening);
        CHECK(pointLimit.blockerSearchSamples == 0);
        CHECK(pointLimit.penumbraRadiusTexels == 0.0f);

        const ShadowContactFilterPlan localNear = localShadowFilterPlan(
            5.1f, 5.0f, 0.25f, 512.0f, profile, 48.0f);
        const ShadowContactFilterPlan localFar = localShadowFilterPlan(
            10.0f, 5.0f, 0.25f, 512.0f, profile, 48.0f);
        CHECK(localNear.contactHardening);
        CHECK(localFar.penumbraRadiusTexels >
            localNear.penumbraRadiusTexels);
        CHECK(localFar.blockerSearchSamples == profile.blockerSearchSamples);

        const ShadowFilterProfile low = shadowFilterProfile(
            ShadowQualityProfile::Low);
        CHECK(!low.contactHardening);
        CHECK(low.blockerSearchSamples == 0);
        CHECK(low.filterSamples == 9);
        return true;
    }

    bool futureVisibilityContractsKeepDeterministicFallbacks() {
        const VirtualShadowPagePolicy virtualPages{};
        CHECK(virtualPages.pageSizeTexels == 128);
        CHECK(virtualPages.borderTexels > 0);
        CHECK(virtualPages.maximumPageUpdatesPerFrame <=
            virtualPages.maximumPhysicalPages);
        CHECK(virtualPages.deterministicConventionalFallback);

        const ShadowTemporalHandoff temporal{};
        CHECK(temporal.requiresMotionVectors);
        CHECK(temporal.requiresDisocclusionMask);
        CHECK(temporal.acceptsStochasticVisibility);

        const ShadowRayTracingHandoff rayTracing{};
        CHECK(rayTracing.visibilityEncoding ==
            ShadowVisibilityEncoding::RgbTransmittance);
        CHECK(rayTracing.preservePhysicalEmitterExtent);
        CHECK(rayTracing.preservePerLightOwnership);
        CHECK(rayTracing.deterministicRasterFallback);

        const AmbientOcclusionPolicy ao{};
        CHECK(ao.method == AmbientOcclusionMethod::Gtao);
        CHECK(ao.outputBentNormal);
        CHECK(ao.applySpecularOcclusion);
        return true;
    }

} // namespace

int main() {
    const struct { const char* name; bool (*run)(); } tests[]{
        { "selection is priority then stable identity",
            selectionIsPriorityThenStableIdentity },
        { "cascades contain frustum and use Vulkan depth",
            cascadesContainTheirFrustumAndUseVulkanDepth },
        { "sub-texel lateral motion is stable", subTexelLateralMotionIsStable },
        { "cache budgets updates and rejects stale projections",
            cacheBudgetsUpdatesAndNeverSamplesChangedProjection },
        { "filter blend bias and invalid inputs are frozen",
            filterBlendBiasAndInvalidInputsAreFrozen },
        { "cache keys distinguish casters camera light and owner not receivers",
            cacheKeysDistinguishCastersCameraLightAndOwnerNotReceivers },
        { "contact hardening uses physical emitter extent",
            contactHardeningUsesPhysicalEmitterExtent },
        { "future visibility contracts keep deterministic fallbacks",
            futureVisibilityContractsKeepDeterministicFallbacks },
    };
    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.name << '\n';
        if (!test.run()) return 1;
        std::cout << "[       OK ] " << test.name << '\n';
    }
    return 0;
}
