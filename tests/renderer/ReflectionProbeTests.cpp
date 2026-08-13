#include "renderer/lighting/ReflectionProbe.h"
#include "renderer/lighting/ReflectionProbeCapture.h"
#include "renderer/lighting/ClusteredReflectionProbes.h"
#include "scene/components/TransformComponent.h"

#include <cmath>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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
            "019fb73d-5a80-7000-8000-%012llu",
            static_cast<unsigned long long>(suffix));
        return *SceneEntityUuid::parse(text);
    }

    ReflectionProbeCandidate probe(uint64_t owner,
        ReflectionProbeShape shape = ReflectionProbeShape::Box) {
        ReflectionProbeCandidate result;
        result.owner = uuid(owner);
        result.probe.shape = shape;
        result.probe.environmentAssetGuid = *AssetGuid::parse(
            "019fb73d-5a80-7000-8000-000000000100");
        result.resident = true;
        return result;
    }

    ReflectionProbeCaptureRequest captureRequest(uint64_t owner,
        ReflectionProbeUpdateMode mode =
            ReflectionProbeUpdateMode::OnDemand) {
        return {
            .owner = uuid(owner),
            .updateMode = mode,
            .position = { 2.0f, 3.0f, 4.0f },
            .resolution = 512,
            .nearPlane = 0.1f,
            .farPlane = 100.0f,
            .priority = static_cast<int32_t>(owner),
        };
    }

    glm::vec3 expectedCubeDirection(uint32_t face, float u, float v) {
        const glm::vec3 directions[6]{
            { 1.0f, -v, -u }, { -1.0f, -v, u },
            { u, 1.0f, v }, { u, -1.0f, -v },
            { u, -v, 1.0f }, { -u, -v, -1.0f },
        };
        return glm::normalize(directions[face]);
    }

    bool influenceUsesShapeAndInteriorBlend() {
        auto sphere = probe(1, ReflectionProbeShape::Sphere);
        sphere.probe.sphereRadiusMeters = 5.0f;
        sphere.probe.blendDistanceMeters = 2.0f;
        CHECK(reflectionProbeInfluence(sphere, glm::vec3(0.0f)) == 1.0f);
        CHECK(std::abs(reflectionProbeInfluence(sphere,
            glm::vec3(4.0f, 0.0f, 0.0f)) - 0.5f) < 1.0e-6f);
        CHECK(reflectionProbeInfluence(sphere,
            glm::vec3(5.1f, 0.0f, 0.0f)) == 0.0f);
        const ReflectionProbeSelection blended = selectReflectionProbes(
            std::span(&sphere, 1), glm::vec3(4.0f, 0.0f, 0.0f));
        CHECK(blended.count == 1);
        CHECK(std::abs(blended.probes[0].weight - 0.5f) < 1.0e-6f);
        CHECK(std::abs(blended.globalEnvironmentWeight - 0.5f) < 1.0e-6f);
        CHECK(blended.useGlobalEnvironment);

        auto box = probe(2);
        box.probe.boxExtentsMeters = { 4.0f, 3.0f, 2.0f };
        box.probe.blendDistanceMeters = 1.0f;
        CHECK(reflectionProbeInfluence(box,
            glm::vec3(0.0f, 0.0f, 1.5f)) == 0.5f);
        box.resident = false;
        CHECK(reflectionProbeInfluence(box, glm::vec3(0.0f)) == 0.0f);
        return true;
    }

    bool gpuRecordAbiAndShaderMirrorAreFrozen() {
        CHECK(sizeof(PackedGpuReflectionProbe) == 112);
        CHECK(alignof(PackedGpuReflectionProbe) == 16);
        CHECK(offsetof(PackedGpuReflectionProbe, worldToProbe) == 0);
        CHECK(offsetof(PackedGpuReflectionProbe, influence) == 64);
        CHECK(offsetof(PackedGpuReflectionProbe, positionIntensity) == 80);
        CHECK(offsetof(PackedGpuReflectionProbe, metadata) == 96);
        CHECK(sizeof(PackedGpuReflectionProbeClusterParameters) == 256);
        std::ifstream shader(std::string(PROJECT_ROOT_DIR) +
            "/assets/shaders/include/reflection_probe_records.glsl",
            std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(shader)),
            std::istreambuf_iterator<char>());
        CHECK(source.find("mat4 worldToProbe;") <
            source.find("vec4 influence;"));
        CHECK(source.find("vec4 influence;") <
            source.find("vec4 positionIntensity;"));
        CHECK(source.find("vec4 positionIntensity;") <
            source.find("uvec4 metadata;"));
        std::ifstream clusterShader(std::string(PROJECT_ROOT_DIR) +
            "/assets/shaders/reflection_probe_cluster.comp",
            std::ios::binary);
        const std::string clusterSource((std::istreambuf_iterator<char>(
            clusterShader)), std::istreambuf_iterator<char>());
        CHECK(clusterSource.find("local_size_x = 64") != std::string::npos);
        CHECK(clusterSource.find("probeClusterHeaders") != std::string::npos);
        CHECK(clusterSource.find("maximumPerCluster") != std::string::npos);
        return true;
    }

    bool selectionIsPriorityBoundedAndDeterministic() {
        std::vector<ReflectionProbeCandidate> candidates;
        for (uint64_t index = 5; index > 0; --index) {
            candidates.push_back(probe(index));
            candidates.back().probe.boxExtentsMeters = glm::vec3(
                static_cast<float>(index + 1));
            candidates.back().probe.blendDistanceMeters = 0.0f;
        }
        candidates[2].probe.priority = 10;
        const ReflectionProbeSelection selected = selectReflectionProbes(
            candidates, glm::vec3(0.0f));
        CHECK(selected.influencingCandidateCount == 5);
        CHECK(selected.count == 2);
        CHECK(!selected.useGlobalEnvironment);
        CHECK(selected.probes[0].owner == candidates[2].owner);
        CHECK(std::abs(selected.probes[0].weight +
            selected.probes[1].weight - 1.0f) < 1.0e-6f);

        candidates[2].probe.priority = 0;
        const ReflectionProbeSelection byVolume = selectReflectionProbes(
            candidates, glm::vec3(0.0f));
        CHECK(byVolume.probes[0].owner == uuid(1));
        CHECK(byVolume.probes[1].owner == uuid(2));
        return true;
    }

    bool boxProjectionCorrectsOffCenterReflection() {
        auto candidate = probe(1);
        candidate.probe.boxExtentsMeters = { 5.0f, 5.0f, 5.0f };
        const glm::vec3 corrected = boxProjectedReflectionDirection(candidate,
            glm::vec3(4.0f, 0.0f, 0.0f),
            glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f)));
        const glm::vec3 expected = glm::normalize(glm::vec3(5.0f, 0.0f, 1.0f));
        CHECK(glm::length(corrected - expected) < 1.0e-5f);

        candidate.probe.parallaxMode = ReflectionProbeParallaxMode::None;
        const glm::vec3 original = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
        CHECK(glm::length(boxProjectedReflectionDirection(candidate,
            glm::vec3(4.0f, 0.0f, 0.0f), original) - original) < 1.0e-6f);
        return true;
    }

    bool missingProductsFallBackToGlobal() {
        auto candidate = probe(1);
        candidate.resident = false;
        const ReflectionProbeSelection selected = selectReflectionProbes(
            std::span(&candidate, 1), glm::vec3(0.0f));
        CHECK(selected.count == 0);
        CHECK(selected.useGlobalEnvironment);
        return true;
    }

    bool extractionIsDeterministicRigidAndResidencyAware() {
        SceneWorld world;
        const AssetGuid environment = *AssetGuid::parse(
            "019fb73d-5a80-7000-8000-000000000100");
        const Entity second = world.createEntity(uuid(2));
        auto& secondTransform = world.registry().addComponent<
            TransformComponent>(second);
        secondTransform.position = { 4.0f, 5.0f, 6.0f };
        secondTransform.rotation = { 0.0f, 90.0f, 0.0f };
        secondTransform.scale = { 2.0f, 3.0f, 4.0f };
        auto& secondProbe = world.registry().addComponent<
            ReflectionProbeComponent>(second);
        secondProbe.environmentAssetGuid = environment;

        const Entity first = world.createEntity(uuid(1));
        world.registry().addComponent<TransformComponent>(first);
        auto& firstProbe = world.registry().addComponent<
            ReflectionProbeComponent>(first);
        firstProbe.environmentAssetGuid = environment;

        const Entity invalid = world.createEntity(uuid(3));
        auto& invalidProbe = world.registry().addComponent<
            ReflectionProbeComponent>(invalid);
        invalidProbe.environmentAssetGuid = environment;

        for (Entity entity : { first, second }) {
            auto& transform = world.registry().getComponent<TransformComponent>(
                entity);
            transform.updateLocalMatrix();
            transform.worldMatrix = transform.localMatrix;
            transform.isDirty = false;
        }
        const ReflectionProbeFramePacket packet = extractReflectionProbes(
            world, [environment](AssetGuid candidate) {
                return candidate == environment;
            });
        CHECK(packet.stats.sceneProbeCount == 3);
        CHECK(packet.stats.candidateCount == 2);
        CHECK(packet.stats.residentCount == 2);
        CHECK(packet.stats.omittedCount == 1);
        CHECK(packet.candidates[0].owner == uuid(1));
        CHECK(packet.candidates[1].owner == uuid(2));
        CHECK(packet.diagnostics.size() == 1);
        CHECK(packet.diagnostics[0].code ==
            ReflectionProbeExtractionDiagnosticCode::MissingTransform);
        const glm::mat3 basis(packet.candidates[1].probeToWorld);
        CHECK(std::abs(glm::length(basis[0]) - 1.0f) < 1.0e-5f);
        CHECK(std::abs(glm::length(basis[1]) - 1.0f) < 1.0e-5f);
        CHECK(std::abs(glm::length(basis[2]) - 1.0f) < 1.0e-5f);
        const glm::vec3 roundTrip = glm::vec3(
            packet.candidates[1].worldToProbe *
            packet.candidates[1].probeToWorld *
            glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
        CHECK(glm::length(roundTrip - glm::vec3(1.0f, 2.0f, 3.0f)) <
            1.0e-4f);

        world.registry().getComponent<ReflectionProbeComponent>(first)
            .resolvedEnvironmentAssetGuid = environment;
        const ReflectionProbeFramePacket implicit = extractReflectionProbes(world);
        CHECK(implicit.stats.residentCount == 1);
        return true;
    }

    bool publicationKeepsStableSlotsAndIncrementalRanges() {
        std::vector<ReflectionProbeCandidate> candidates{
            probe(2), probe(1) };
        candidates[0].probe.priority = 2;
        candidates[0].probe.intensity = 1.5f;
        candidates[0].probeToWorld[3] = glm::vec4(2.0f, 0.0f, 0.0f, 1.0f);
        candidates[0].worldToProbe = glm::inverse(
            candidates[0].probeToWorld);
        ReflectionProbePublisher publisher({
            .initialCapacity = 2, .maximumCapacity = 4 });
        const auto resolve = [](AssetGuid) -> std::optional<uint32_t> {
            return 7u;
        };
        auto first = publisher.publish(candidates, resolve);
        CHECK(first.stats.activeProbeCount == 2);
        CHECK(first.stats.changedRecordCount == 2);
        CHECK(first.stats.changedRangeCount == 1);
        CHECK(first.stats.changedRecordBytes ==
            2 * sizeof(PackedGpuReflectionProbe));
        const uint32_t firstSlot = *publisher.slotFor(uuid(1));
        const uint32_t secondSlot = *publisher.slotFor(uuid(2));
        CHECK(firstSlot != secondSlot);
        CHECK(first.records[secondSlot].metadata.y == 7u);
        CHECK(std::bit_cast<int32_t>(first.records[secondSlot].metadata.z) == 2);
        CHECK(first.records[secondSlot].metadata.w == 0u);
        CHECK(first.records[secondSlot].positionIntensity.x == 2.0f);
        CHECK(first.records[secondSlot].positionIntensity.w == 1.5f);
        const uint64_t activeRevision = first.activeListRevision;

        auto unchanged = publisher.publish(candidates, resolve);
        CHECK(unchanged.stats.changedRecordCount == 0);
        CHECK(unchanged.changedRanges.empty());
        CHECK(unchanged.activeListRevision == activeRevision);

        candidates[0].probe.intensity = 2.0f;
        auto changed = publisher.publish(candidates, resolve);
        CHECK(changed.stats.changedRecordCount == 1);
        CHECK(changed.changedRanges.size() == 1);
        CHECK(changed.changedRanges[0].firstRecord == secondSlot);

        candidates.erase(candidates.begin() + 1);
        candidates.push_back(probe(3));
        auto replaced = publisher.publish(candidates, resolve);
        CHECK(publisher.slotFor(uuid(2)) == secondSlot);
        CHECK(publisher.slotFor(uuid(3)) == firstSlot);
        CHECK(!publisher.slotFor(uuid(1)));
        CHECK(replaced.stats.activeProbeCount == 2);
        return true;
    }

    bool publicationBoundsCapacityAndDiagnosesResidency() {
        std::vector<ReflectionProbeCandidate> candidates{
            probe(1), probe(2), probe(3), probe(4) };
        candidates[0].probe.priority = 1;
        candidates[1].probe.priority = 8;
        candidates[2].probe.priority = 4;
        candidates[3].probe.priority = 2;
        candidates[3].resident = false;
        ReflectionProbePublisher publisher({
            .initialCapacity = 2, .maximumCapacity = 2 });
        const AssetGuid unresolved = candidates[2].probe.environmentAssetGuid;
        candidates[2].probe.environmentAssetGuid = *AssetGuid::parse(
            "019fb73d-5a80-7000-8000-000000000200");
        const auto packet = publisher.publish(candidates,
            [unresolved](AssetGuid environment) -> std::optional<uint32_t> {
                return environment == unresolved
                    ? std::optional<uint32_t>(3u) : std::nullopt;
            });
        CHECK(packet.stats.extractedCandidateCount == 4);
        CHECK(packet.stats.activeProbeCount == 2);
        CHECK(packet.stats.nonresidentProbeCount == 1);
        CHECK(packet.stats.unresolvedEnvironmentCount == 1);
        CHECK(packet.stats.capacityOmittedCount == 0);
        CHECK(publisher.slotFor(uuid(1)));
        CHECK(publisher.slotFor(uuid(2)));

        candidates[2].probe.environmentAssetGuid = unresolved;
        candidates[3].resident = true;
        const auto overflow = publisher.publish(candidates,
            [](AssetGuid) -> std::optional<uint32_t> { return 3u; });
        CHECK(overflow.stats.capacityOmittedCount == 2);
        CHECK(overflow.stats.activeProbeCount == 2);
        CHECK(publisher.slotFor(uuid(2)));
        CHECK(publisher.slotFor(uuid(3)));
        CHECK(!publisher.slotFor(uuid(1)));
        CHECK(!publisher.slotFor(uuid(4)));
        return true;
    }

    bool publicationAcceptsRuntimeCaptureSlotsWithoutAssetIdentity() {
        ReflectionProbeCandidate captured = probe(1);
        captured.probe.environmentAssetGuid = {};
        captured.runtimeEnvironmentSlot = 63u;
        captured.resident = true;
        ReflectionProbePublisher publisher({
            .initialCapacity = 1, .maximumCapacity = 1 });
        const auto packet = publisher.publish(std::span(&captured, 1),
            [](AssetGuid) -> std::optional<uint32_t> {
                return std::nullopt;
            });
        CHECK(packet.stats.activeProbeCount == 1);
        CHECK(packet.stats.nonresidentProbeCount == 0);
        CHECK(packet.stats.unresolvedEnvironmentCount == 0);
        const uint32_t slot = *publisher.slotFor(captured.owner);
        CHECK(packet.records[slot].metadata.y == 63u);
        return true;
    }

    bool clusteredAssignmentBoundsAndRanksCandidates() {
        std::vector<ReflectionProbeCandidate> candidates;
        for (uint64_t index = 1; index <= 5; ++index) {
            candidates.push_back(probe(index));
            auto& candidate = candidates.back();
            candidate.probe.priority = static_cast<int32_t>(index);
            candidate.probe.boxExtentsMeters = glm::vec3(20.0f);
            candidate.probe.blendDistanceMeters = 0.0f;
            candidate.probeToWorld = glm::translate(glm::mat4(1.0f),
                glm::vec3(0.0f, 0.0f, -5.0f));
            candidate.worldToProbe = glm::inverse(candidate.probeToWorld);
        }
        ReflectionProbePublisher publisher({
            .initialCapacity = 8, .maximumCapacity = 8 });
        const auto packet = publisher.publish(candidates,
            [](AssetGuid) -> std::optional<uint32_t> { return 1u; });
        const ClusterFrameParameters frame{
            .renderWidth = 64,
            .renderHeight = 64,
            .nearPlane = 0.1f,
            .farPlane = 100.0f,
            .view = glm::mat4(1.0f),
            .projection = glm::perspective(glm::radians(60.0f),
                1.0f, 0.1f, 100.0f),
        };
        ClusteredReflectionProbeAssigner assigner({
            .tileWidth = 32,
            .tileHeight = 32,
            .depthSlices = 2,
            .maximumProbesPerCluster = 4,
            .maximumProbeReferences = 32,
        });
        const ClusteredReflectionProbeProduct product = assigner.build(
            packet, frame);
        CHECK(product.dimensions == (ClusterGridDimensions{ 2, 2, 2 }));
        CHECK(product.stats.activeProbeCount == 5);
        CHECK(product.stats.maximumRequestedOccupancy == 5);
        CHECK(product.stats.truncatedProbeReferences > 0);
        const auto header = std::ranges::find_if(product.headers,
            [](const ClusterLightHeader& candidate) {
                return candidate.count == 4;
            });
        CHECK(header != product.headers.end());
        CHECK(product.probeSlots[header->offset] ==
            *publisher.slotFor(uuid(5)));
        CHECK(product.probeSlots[header->offset + 1] ==
            *publisher.slotFor(uuid(4)));
        CHECK(product.probeSlots[header->offset + 2] ==
            *publisher.slotFor(uuid(3)));
        CHECK(product.probeSlots[header->offset + 3] ==
            *publisher.slotFor(uuid(2)));

        ClusteredReflectionProbeAssigner constrained({
            .tileWidth = 32,
            .tileHeight = 32,
            .depthSlices = 2,
            .maximumProbesPerCluster = 4,
            .maximumProbeReferences = 1,
        });
        const auto overflow = constrained.build(packet, frame);
        CHECK(overflow.stats.publishedProbeReferences == 1);
        CHECK(overflow.stats.overflow ==
            ClusterProbeOverflowCode::GlobalReferenceCapacity);
        for (const ClusterLightHeader& candidate : overflow.headers) {
            CHECK(candidate.offset <= overflow.probeSlots.size());
            CHECK(candidate.count <= overflow.probeSlots.size() -
                candidate.offset);
        }
        return true;
    }

    bool captureFacesMatchCookedCubeOrientation() {
        const glm::vec3 position(2.0f, 3.0f, 4.0f);
        const auto faces = buildReflectionProbeCaptureFaces(
            position, 0.1f, 100.0f);
        for (uint32_t face = 0; face < faces.size(); ++face) {
            CHECK(faces[face].faceIndex == face);
            for (const glm::vec2 uv : {
                    glm::vec2(0.0f), glm::vec2(-0.75f, -0.5f),
                    glm::vec2(0.5f, 0.75f) }) {
                const glm::mat4 clipToWorld = glm::inverse(
                    faces[face].worldToClip);
                glm::vec4 point = clipToWorld *
                    glm::vec4(uv.x, uv.y, 1.0f, 1.0f);
                point /= point.w;
                const glm::vec3 actual = glm::normalize(
                    glm::vec3(point) - position);
                CHECK(glm::length(actual - expectedCubeDirection(
                    face, uv.x, uv.y)) < 1.0e-4f);
            }
        }
        return true;
    }

    bool captureStorageFootprintsAreExplicitAndBounded() {
        CHECK(reflectionProbeCaptureMipCount(128) == 8);
        CHECK(reflectionProbeCaptureMipCount(512) == 10);
        CHECK(reflectionProbeCaptureMipCount(4096) == 13);
        const auto medium = reflectionProbeCaptureStorageFootprint(512);
        CHECK(medium.rawRadianceBytes == 12'582'912);
        CHECK(medium.depthBytes == 6'291'456);
        CHECK(medium.prefilteredRadianceBytes == 16'777'200);
        CHECK(medium.totalStagingBytes == 35'651'568);
        const auto cinematic = reflectionProbeCaptureStorageFootprint(4096);
        CHECK(cinematic.totalStagingBytes == 2'281'701'360ull);
        CHECK(cinematic.prefilteredRadianceBytes == 1'073'741'808ull);
        bool rejected = false;
        try {
            (void)reflectionProbeCaptureStorageFootprint(300);
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        return true;
    }

    bool captureSchedulerPublishesOnlyCompleteCubes() {
        ReflectionProbeCaptureScheduler scheduler({
            .maximumRenderedTexels = 2ull * 512ull * 512ull,
            .maximumFacesPerProbePerFrame = 2,
            .maximumCapturesInFlight = 2,
        });
        const ReflectionProbeCaptureRequest request = captureRequest(1);
        uint64_t ticket = 0;
        for (uint32_t pass = 0; pass < 3; ++pass) {
            const auto& schedule = scheduler.schedule(
                std::span(&request, 1));
            CHECK(schedule.stats.dirty == 1);
            CHECK(schedule.stats.facesScheduled == 2);
            CHECK(schedule.entries.size() == 1);
            CHECK(!schedule.entries[0].sampleable);
            if (pass == 0) {
                CHECK(schedule.stats.capturesStarted == 1);
                ticket = schedule.entries[0].captureTicket;
            }
            else {
                CHECK(schedule.stats.capturesStarted == 0);
                CHECK(schedule.entries[0].captureTicket == ticket);
            }
            scheduler.markScheduledFacesRendered();
            CHECK(scheduler.publicationsReady().size() ==
                (pass == 2 ? 1u : 0u));
        }
        const auto ready = scheduler.publicationsReady();
        CHECK(ready.size() == 1);
        CHECK(ready[0].owner == request.owner);
        CHECK(ready[0].captureTicket == ticket);
        CHECK(!scheduler.hasPublishedCapture(request.owner));
        scheduler.markPublished(request.owner, ticket);
        CHECK(scheduler.hasPublishedCapture(request.owner));
        CHECK(scheduler.publishedTicket(request.owner) == ticket);

        const auto& cacheHit = scheduler.schedule(std::span(&request, 1));
        CHECK(cacheHit.stats.cacheHits == 1);
        CHECK(cacheHit.stats.facesScheduled == 0);
        CHECK(cacheHit.entries[0].sampleable);
        scheduler.markScheduledFacesRendered();
        return true;
    }

    bool captureSchedulerKeepsLastGoodDuringRefresh() {
        ReflectionProbeCaptureScheduler scheduler({
            .maximumRenderedTexels = 512ull * 512ull,
            .maximumFacesPerProbePerFrame = 1,
            .maximumCapturesInFlight = 1,
        });
        auto request = captureRequest(1);
        for (uint32_t face = 0; face < 6; ++face) {
            (void)scheduler.schedule(std::span(&request, 1));
            scheduler.markScheduledFacesRendered();
        }
        const uint64_t firstTicket =
            scheduler.publicationsReady()[0].captureTicket;
        scheduler.markPublished(request.owner, firstTicket);

        request.explicitRequestRevision = 1;
        const auto& refresh = scheduler.schedule(std::span(&request, 1));
        CHECK(refresh.entries[0].sampleable);
        CHECK(refresh.entries[0].captureInFlight);
        CHECK(refresh.entries[0].dirtyReason ==
            ReflectionProbeCaptureDirtyReason::ExplicitRequest);
        CHECK(refresh.entries[0].captureTicket != firstTicket);
        CHECK(refresh.stats.facesScheduled == 1);
        scheduler.markScheduledFacesRendered();
        CHECK(scheduler.hasPublishedCapture(request.owner));
        CHECK(scheduler.publishedTicket(request.owner) == firstTicket);
        CHECK(scheduler.publicationsReady().empty());
        return true;
    }

    bool captureSchedulerHonorsModesInvalidationAndBudgets() {
        ReflectionProbeCaptureScheduler scheduler({
            .maximumRenderedTexels = 128ull * 128ull,
            .maximumFacesPerProbePerFrame = 1,
            .maximumCapturesInFlight = 1,
        });
        auto baked = captureRequest(1, ReflectionProbeUpdateMode::Baked);
        baked.resolution = 128;
        auto realtime = captureRequest(2, ReflectionProbeUpdateMode::Realtime);
        realtime.resolution = 128;
        const std::array requests{ baked, realtime };
        const auto& first = scheduler.schedule(requests);
        CHECK(first.stats.dirty == 1);
        CHECK(first.stats.facesScheduled == 1);
        CHECK(first.entries[0].owner == realtime.owner);
        CHECK(first.entries[0].scheduledFaceMask != 0);
        CHECK(first.entries[1].owner == baked.owner);
        CHECK(first.entries[1].scheduledFaceMask == 0);
        scheduler.markScheduledFacesRendered();

        realtime.sceneRevision = 1;
        const std::array changed{ baked, realtime };
        const auto& invalidated = scheduler.schedule(changed);
        CHECK(invalidated.stats.capturesInvalidated == 1);
        CHECK(invalidated.stats.capturesStarted == 1);
        CHECK(invalidated.entries[0].dirtyReason ==
            ReflectionProbeCaptureDirtyReason::NewProbe);
        scheduler.markScheduledFacesRendered();

        scheduler.reset();
        baked.explicitRequestRevision = 1;
        const auto& explicitBaked = scheduler.schedule(
            std::span(&baked, 1));
        CHECK(explicitBaked.stats.dirty == 1);
        CHECK(explicitBaked.entries[0].dirtyReason ==
            ReflectionProbeCaptureDirtyReason::ExplicitRequest);
        CHECK(explicitBaked.entries[0].updateMode ==
            ReflectionProbeUpdateMode::Baked);
        CHECK(explicitBaked.stats.facesScheduled == 1);
        scheduler.markScheduledFacesRendered();
        return true;
    }

    bool realtimeCaptureCadenceBoundsAutomaticRefresh() {
        ReflectionProbeCaptureScheduler scheduler({
            .maximumRenderedTexels = 6ull * 128ull * 128ull,
            .maximumFacesPerProbePerFrame = 6,
            .maximumCapturesInFlight = 1,
            .minimumRealtimeFramesBetweenCaptures = 6,
        });
        auto request = captureRequest(1, ReflectionProbeUpdateMode::Realtime);
        request.resolution = 128;
        request.frameIndex = 0;
        const auto& first = scheduler.schedule(std::span(&request, 1));
        CHECK(first.stats.facesScheduled == 6);
        const uint64_t ticket = first.entries[0].captureTicket;
        scheduler.markScheduledFacesRendered();
        CHECK(scheduler.publicationsReady().size() == 1);
        scheduler.markPublished(request.owner, ticket);

        request.sceneRevision = 2;
        request.frameIndex = 1;
        const auto& deferred = scheduler.schedule(std::span(&request, 1));
        CHECK(deferred.stats.cadenceDeferred == 1);
        CHECK(deferred.stats.facesScheduled == 0);
        CHECK(deferred.entries[0].sampleable);
        scheduler.markScheduledFacesRendered();

        request.frameIndex = 6;
        const auto& refresh = scheduler.schedule(std::span(&request, 1));
        CHECK(refresh.stats.cadenceDeferred == 0);
        CHECK(refresh.stats.facesScheduled == 6);
        CHECK(refresh.entries[0].dirtyReason ==
            ReflectionProbeCaptureDirtyReason::SceneChanged);
        scheduler.markScheduledFacesRendered();
        return true;
    }
}

int main() {
    const struct { const char* name; bool (*run)(); } tests[]{
        { "influence uses shape and interior blend",
            influenceUsesShapeAndInteriorBlend },
        { "GPU record ABI and shader mirror are frozen",
            gpuRecordAbiAndShaderMirrorAreFrozen },
        { "selection is priority bounded and deterministic",
            selectionIsPriorityBoundedAndDeterministic },
        { "box projection corrects off-center reflection",
            boxProjectionCorrectsOffCenterReflection },
        { "missing products fall back to global",
            missingProductsFallBackToGlobal },
        { "extraction is deterministic rigid and residency aware",
            extractionIsDeterministicRigidAndResidencyAware },
        { "publication keeps stable slots and incremental ranges",
            publicationKeepsStableSlotsAndIncrementalRanges },
        { "publication bounds capacity and diagnoses residency",
            publicationBoundsCapacityAndDiagnosesResidency },
        { "publication accepts runtime capture slots without asset identity",
            publicationAcceptsRuntimeCaptureSlotsWithoutAssetIdentity },
        { "clustered assignment bounds and ranks candidates",
            clusteredAssignmentBoundsAndRanksCandidates },
        { "capture faces match cooked cube orientation",
            captureFacesMatchCookedCubeOrientation },
        { "capture storage footprints are explicit and bounded",
            captureStorageFootprintsAreExplicitAndBounded },
        { "capture scheduler publishes only complete cubes",
            captureSchedulerPublishesOnlyCompleteCubes },
        { "capture scheduler keeps last good during refresh",
            captureSchedulerKeepsLastGoodDuringRefresh },
        { "capture scheduler honors modes invalidation and budgets",
            captureSchedulerHonorsModesInvalidationAndBudgets },
        { "realtime capture cadence bounds automatic refresh",
            realtimeCaptureCadenceBoundsAutomaticRefresh },
    };
    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.name << '\n';
        if (!test.run()) return 1;
        std::cout << "[       OK ] " << test.name << '\n';
    }
    return 0;
}
