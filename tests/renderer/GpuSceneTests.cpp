#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneUploadPlanner.h"
#include "renderer/scene/GpuScenePublisher.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n'; \
        return false; } } while (false)

    AssetGuid guid(const char* text) { return *AssetGuid::parse(text); }
    SceneEntityUuid uuid(const char* text) { return *SceneEntityUuid::parse(text); }

    GpuSceneGeometrySource geometry() {
        return {
            .handle = { 7, 3 },
            .localBoundsSphere = { 0.0f, 0.0f, 0.0f, 2.0f },
            .localBoundsMin = { -1.0f, -2.0f, -1.0f, 0.0f },
            .localBoundsMax = { 1.0f, 2.0f, 1.0f, 0.0f },
            .firstIndex = 12, .indexCount = 36, .vertexOffset = -4,
            .indexType = 1, .vertexArena = 2, .indexArena = 5,
            .vertexLayout = 9, .productRevision = 11,
            .recordRevision = 101,
            .identity = {
                guid("019fb73d-5a60-7000-8000-000000000010"),
                guid("019fb73d-5a60-7000-8000-000000000011"),
                guid("019fb73d-5a60-7000-8000-000000000012") },
        };
    }

    GpuSceneInstanceSource instance(uint32_t slot, uint32_t primitiveSlot,
        const char* ownerText, const char* materialText, uint32_t materialIndex) {
        GpuSceneInstanceSource source;
        source.handle = { slot, 2 };
        source.mobility = GpuSceneMobility::Movable;
        source.worldBoundsSphere = { float(slot), 0.0f, 0.0f, 3.0f };
        source.worldBoundsMin = { float(slot) - 2.0f, -1.0f, -1.0f, 0.0f };
        source.worldBoundsMax = { float(slot) + 2.0f, 1.0f, 1.0f, 0.0f };
        source.identity = { uuid(ownerText), geometry().identity.modelAssetGuid,
            44, "cook-key" };
        source.instanceRevision = 70 + slot;
        source.currentTransformRevision = 80 + slot;
        source.previousTransformRevision = 60 + slot;
        source.primitives.push_back({
            .handle = { primitiveSlot, 4 },
            .geometry = geometry().handle,
            .material = MaterialHandle::fromParts(materialIndex, 1),
            .pipeline = PipelineHandle::fromParts(2, 1),
            .flags = GpuScenePrimitiveOpaque,
            .primitiveLayoutRevision = 3,
            .geometryProductRevision = 11,
            .instanceBindingRevision = materialIndex,
            .materialRevision = 8,
            .recordRevision = 90 + primitiveSlot,
            .identity = { uuid(ownerText), geometry().identity.sourcePrimitiveGuid,
                geometry().identity.primitiveGuid, guid(materialText) },
        });
        return source;
    }

    bool abiAndAffineContract() {
        CHECK(offsetof(GpuSceneInstanceRecord, references) == 48);
        CHECK(offsetof(GpuSceneInstanceRecord, state) == 64);
        CHECK(offsetof(GpuScenePrimitiveRecord, revisions) == 32);
        CHECK(offsetof(GpuSceneGeometryRecord, draw) == 48);
        const uint32_t heroFlags = packGpuSceneInstanceFlags(
            GpuSceneInstanceEnabled | GpuSceneInstanceSelected, 0u);
        CHECK((heroFlags & GpuSceneInstanceEnabled) != 0u);
        CHECK((heroFlags & GpuSceneInstanceSelected) != 0u);
        CHECK(gpuSceneInstanceMaximumLod(heroFlags) == 0u);
        CHECK(gpuSceneInstanceMaximumLod(packGpuSceneInstanceFlags(0u, 99u)) ==
            MaximumGpuSceneLodLevels - 1u);
        glm::mat4 matrix(1.0f);
        matrix = glm::translate(matrix, { 100000.0f, -3.0f, 9.0f });
        matrix = glm::scale(matrix, { -2.0f, 3.0f, 0.5f });
        const glm::vec3 point{ 2.0f, 4.0f, -6.0f };
        const glm::vec3 expected = glm::vec3(matrix * glm::vec4(point, 1.0f));
        CHECK(glm::length(transformGpuScenePoint(packGpuSceneAffine(matrix), point) -
            expected) < 1.0e-4f);
        CHECK(unpackGpuSceneAffine(packGpuSceneAffine(matrix)) == matrix);
        matrix[3][3] = 2.0f;
        try { (void)packGpuSceneAffine(matrix); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    }

    bool lifetimeRejectsStaleHandles() {
        GpuSceneSlotAllocator<GpuSceneInstanceHandle> slots(1);
        const auto first = slots.allocate();
        CHECK(first && first->slot == 0 && first->generation == 1);
        CHECK(slots.retire(*first, 9));
        CHECK(!slots.isLive(*first));
        CHECK(!slots.allocate());
        CHECK(slots.collect(8) == 0);
        CHECK(slots.collect(9) == 1);
        const auto second = slots.allocate();
        CHECK(second && second->slot == 0 && second->generation == 2);
        CHECK(!slots.isLive(*first));
        CHECK(slots.isLive(*second));

        GpuSceneSlotAllocator<GpuSceneInstanceHandle> sparse(3);
        const auto a = sparse.allocate();
        const auto b = sparse.allocate();
        const auto c = sparse.allocate();
        CHECK(a && b && c);
        CHECK(sparse.retire(*a, 9));
        CHECK(sparse.retire(*b, 11));
        CHECK(sparse.retire(*c, 10));
        CHECK(sparse.collect(9) == 1);
        CHECK(sparse.retiredCount() == 2);
        CHECK(sparse.collect(10) == 1);
        CHECK(sparse.collect(11) == 1);
        CHECK(sparse.retiredCount() == 0);
        return true;
    }

    bool deterministicPackingPreservesColdIdentity() {
        const GpuSceneGeometrySource sharedGeometry = geometry();
        std::vector instances{
            instance(9, 19, "019fb73d-5a60-7000-8000-000000000002",
                "019fb73d-5a60-7000-8000-000000000022", 6),
            instance(2, 12, "019fb73d-5a60-7000-8000-000000000001",
                "019fb73d-5a60-7000-8000-000000000021", 5),
        };
        for (auto& source : instances)
            source.primitives.front().consumerMask =
                GpuSceneConsumerMainOpaque | GpuSceneConsumerShadow |
                GpuSceneConsumerProbe;
        instances[0].maximumLod = 0u;
        const GpuScenePackedTables packed = packGpuSceneReference({
            .sceneEpoch = 17, .publicationRevision = 23,
            .geometries = std::span(&sharedGeometry, 1), .instances = instances }, {});
        CHECK(packed.sceneEpoch == 17 && packed.publicationRevision == 23);
        CHECK(packed.geometries.size() == 1);
        CHECK(packed.instances.size() == 2 && packed.primitives.size() == 2);
        CHECK(packed.denseInstanceHandles[0].slot == 2);
        CHECK(packed.denseInstanceHandles[1].slot == 9);
        CHECK(gpuSceneInstanceMaximumLod(packed.instances[0].state.z) ==
            MaximumGpuSceneLodLevels - 1u);
        CHECK(gpuSceneInstanceMaximumLod(packed.instances[1].state.z) == 0u);
        CHECK(packed.primitives[0].binding.y == 0);
        CHECK(packed.primitives[1].binding.y == 0);
        CHECK(packed.primitives[0].binding.z != packed.primitives[1].binding.z);
        CHECK(packed.primitiveIdentities[0].sourcePrimitiveGuid !=
            packed.primitiveIdentities[0].primitiveGuid);
        CHECK(packed.primitiveIdentities[0].effectiveMaterialGuid !=
            packed.primitiveIdentities[1].effectiveMaterialGuid);
        CHECK(packed.directFallbackInstances.empty());
        CHECK(packed.geometryRevisions == std::vector<uint64_t>{ 101 });
        CHECK(packed.instanceRevisions == std::vector<uint64_t>({ 72, 79 }));
        CHECK(packed.transformRevisions ==
            std::vector<uint64_t>({ 82, 62, 89, 69 }));
        CHECK(packed.shadowConsumerPrimitiveIndices ==
            std::vector<uint32_t>({ 0u, 1u }));
        CHECK(packed.probeConsumerPrimitiveIndices ==
            std::vector<uint32_t>({ 0u, 1u }));
        CHECK(packed.shadowConsumerMembershipRevision != 0u);
        CHECK(packed.probeConsumerMembershipRevision != 0u);
        std::ranges::reverse(instances);
        const GpuScenePackedTables repacked = packGpuSceneReference({
            .sceneEpoch = 17, .publicationRevision = 23,
            .geometries = std::span(&sharedGeometry, 1), .instances = instances }, {});
        CHECK(repacked.instances.size() == packed.instances.size());
        CHECK(repacked.primitives.size() == packed.primitives.size());
        CHECK(std::memcmp(repacked.instances.data(), packed.instances.data(),
            packed.instances.size() * sizeof(GpuSceneInstanceRecord)) == 0);
        CHECK(std::memcmp(repacked.primitives.data(), packed.primitives.data(),
            packed.primitives.size() * sizeof(GpuScenePrimitiveRecord)) == 0);
        CHECK(repacked.instanceIdentities == packed.instanceIdentities);
        CHECK(repacked.primitiveIdentities == packed.primitiveIdentities);
        CHECK(repacked.shadowConsumerPrimitiveIndices ==
            packed.shadowConsumerPrimitiveIndices);
        CHECK(repacked.shadowConsumerMembershipRevision ==
            packed.shadowConsumerMembershipRevision);
        CHECK(repacked.probeConsumerMembershipRevision ==
            packed.probeConsumerMembershipRevision);
        return true;
    }

    bool referenceLodLinksUseOnlyAcceptedSources() {
        auto base = geometry();
        auto child = geometry();
        child.handle = { 8, 3 };
        child.identity.primitiveGuid = guid("019fb73d-5a60-7000-8000-000000000013");
        child.lodLevel = 1;
        child.geometricError = 0.02f;
        child.indexCount = 18;
        base.coarserLod = child.handle;
        std::vector sources{ base, child };
        auto packed = packGpuSceneReference({ .geometries = sources }, {});
        CHECK(packed.geometries.size() == 2 && packed.invalidSourceCount == 0);
        CHECK(packed.geometries[0].state.z == 1);
        CHECK(packed.geometries[1].localBoundsMin.w == 0.02f);
        CHECK(packed.geometries[1].localBoundsMax.w == 1.0f);
        // A rejected duplicate must not overwrite the accepted record's link.
        auto duplicate = base;
        duplicate.coarserLod = base.handle;
        sources.push_back(duplicate);
        packed = packGpuSceneReference({ .geometries = sources }, {});
        CHECK(packed.invalidSourceCount == 1);
        CHECK(packed.geometries[0].state.z == 1);
        sources = { base, child };
        sources[1].geometricError = std::numeric_limits<float>::quiet_NaN();
        packed = packGpuSceneReference({ .geometries = sources }, {});
        CHECK(packed.geometries.size() == 1);
        CHECK(packed.geometries[0].state.z == InvalidGpuSceneIndex);
        CHECK(packed.invalidSourceCount == 2);
        return true;
    }

    bool frameContextUploadPlanningIsExact() {
        const std::vector<uint64_t> source{ 4, 5, 8, 8, 10 };
        std::vector<uint64_t> frameA{ 4, 0, 0, 8, 0 };
        std::vector<uint64_t> frameB{ 0, 5, 8, 0, 10 };
        std::vector<GpuSceneRecordRange> ranges;
        buildGpuSceneUploadRanges(source, frameA, ranges);
        CHECK(ranges == std::vector<GpuSceneRecordRange>({ { 1, 2 }, { 4, 1 } }));
        buildGpuSceneUploadRanges(source, frameB, ranges);
        CHECK(ranges == std::vector<GpuSceneRecordRange>({ { 0, 1 }, { 3, 1 } }));
        return true;
    }

    bool uncertaintyAndOverflowFailToDirectPath() {
        const GpuSceneGeometrySource sharedGeometry = geometry();
        auto failVisible = instance(1, 1,
            "019fb73d-5a60-7000-8000-000000000031",
            "019fb73d-5a60-7000-8000-000000000032", 1);
        failVisible.worldBoundsSphere.x = std::numeric_limits<float>::quiet_NaN();
        auto overflow = instance(2, 2,
            "019fb73d-5a60-7000-8000-000000000033",
            "019fb73d-5a60-7000-8000-000000000034", 2);
        std::vector instances{ failVisible, overflow };
        GpuSceneCapacity capacity;
        capacity.maximumInstances = 1;
        capacity.maximumTransforms = 2;
        const auto packed = packGpuSceneReference({ .geometries =
            std::span(&sharedGeometry, 1), .instances = instances }, capacity);
        CHECK(packed.instances.size() == 1);
        CHECK((packed.instances[0].state.z &
            GpuSceneInstanceInvalidBoundsFailVisible) != 0);
        CHECK(packed.instances[0].worldBoundsSphere.w < 0.0f);
        CHECK(packed.capacityOmittedInstanceCount == 1);
        CHECK(packed.directFallbackInstances.size() == 1);
        CHECK(packed.directFallbackInstances[0] == overflow.handle);
        return true;
    }

    GpuSceneObservedInstance observed(const char* ownerText,
        const char* materialText) {
        GpuSceneObservedInstance result;
        result.identity = { uuid(ownerText),
            guid("019fb73d-5a60-7000-8000-000000000040"), 7, "cook-seven" };
        result.observationRevision = 1;
        result.worldBoundsSphere = { 0.0f, 0.0f, 0.0f, 2.0f };
        result.worldBoundsMin = { -1.0f, -1.0f, -1.0f, 0.0f };
        result.worldBoundsMax = { 1.0f, 1.0f, 1.0f, 0.0f };
        result.primitives.push_back({
            .identity = { result.identity.owner,
                guid("019fb73d-5a60-7000-8000-000000000041"),
                guid("019fb73d-5a60-7000-8000-000000000042"),
                guid(materialText) },
            .geometryIdentity = { result.identity.modelAssetGuid,
                guid("019fb73d-5a60-7000-8000-000000000041"),
                guid("019fb73d-5a60-7000-8000-000000000042") },
            .legacyGeometry = GeometryHandle::fromParts(4, 1),
            .material = MaterialHandle::fromParts(5, 1),
            .pipeline = PipelineHandle::fromParts(6, 1),
            .firstIndex = 3, .indexCount = 12,
            .localBoundsSphere = { 0.0f, 0.0f, 0.0f, 2.0f },
            .localBoundsMin = { -1.0f, -1.0f, -1.0f, 0.0f },
            .localBoundsMax = { 1.0f, 1.0f, 1.0f, 0.0f },
            .geometryProductRevision = 9,
            .materialRevision = 3,
        });
        return result;
    }

    bool publisherIsSparseAndRevisionIsolated() {
        GpuSceneCapacity capacity{ 8, 16, 8, 16 };
        GpuScenePublisher publisher(capacity);
        auto input = observed("019fb73d-5a60-7000-8000-000000000050",
            "019fb73d-5a60-7000-8000-000000000051");
        const auto& first = publisher.synchronize(1, std::span(&input, 1), 3, 0);
        CHECK(first.instances.size() == 1 && first.primitives.size() == 1);
        CHECK(publisher.stats().changedInstances == 1);
        CHECK(publisher.stats().changedTransforms == 2);
        CHECK(publisher.stats().changedPrimitives == 1);
        CHECK(publisher.stats().changedGeometries == 1);
        const auto geometryRevisions = first.geometryRevisions;
        const auto transformRevisions = first.transformRevisions;
        const uint64_t publicationRevision = first.publicationRevision;

        const auto& unchanged = publisher.synchronize(
            1, std::span(&input, 1), 4, 0);
        CHECK(publisher.stats().changedInstances == 0);
        CHECK(publisher.stats().changedTransforms == 0);
        CHECK(publisher.stats().changedPrimitives == 0);
        CHECK(publisher.stats().changedGeometries == 0);
        CHECK(publisher.stats().unchangedFastPath == 1);
        CHECK(unchanged.publicationRevision == publicationRevision);
        CHECK(unchanged.geometryRevisions == geometryRevisions);
        CHECK(unchanged.transformRevisions == transformRevisions);

        input.worldTransform = glm::translate(glm::mat4(1.0f),
            { 3.0f, 0.0f, 0.0f });
        ++input.observationRevision;
        const auto& moved = publisher.synchronize(
            1, std::span(&input, 1), 5, 0);
        CHECK(publisher.stats().changedTransforms == 2);
        CHECK(transformGpuScenePoint(moved.transforms[0], {}).x == 3.0f);
        CHECK(transformGpuScenePoint(moved.transforms[1], {}).x == 0.0f);
        CHECK(moved.geometryRevisions == geometryRevisions);
        CHECK(moved.transformRevisions != transformRevisions);

        input.primitives[0].identity.effectiveMaterialGuid =
            guid("019fb73d-5a60-7000-8000-000000000052");
        input.primitives[0].material = MaterialHandle::fromParts(7, 1);
        ++input.primitives[0].materialRevision;
        ++input.observationRevision;
        const auto& materialEdited = publisher.synchronize(
            1, std::span(&input, 1), 6, 0);
        CHECK(publisher.stats().changedTransforms == 0);
        CHECK(publisher.stats().changedGeometries == 0);
        CHECK(publisher.stats().changedPrimitives == 1);
        CHECK(materialEdited.geometryRevisions == geometryRevisions);
        input.maximumLod = 0u;
        ++input.observationRevision;
        const auto& heroEdited = publisher.synchronize(
            1, std::span(&input, 1), 7, 0);
        CHECK(publisher.stats().changedInstances == 1);
        CHECK(publisher.stats().changedTransforms == 0);
        CHECK(publisher.stats().changedPrimitives == 0);
        CHECK(gpuSceneInstanceMaximumLod(heroEdited.instances[0].state.z) == 0u);
        return true;
    }

    bool publisherRevisesRelocatedDenseReferences() {
        GpuScenePublisher publisher({ 8, 16, 8, 16 });
        std::vector input{
            observed("019fb73d-5a60-7000-8000-000000000050",
                "019fb73d-5a60-7000-8000-000000000051"),
            observed("019fb73d-5a60-7000-8000-000000000060",
                "019fb73d-5a60-7000-8000-000000000061"),
        };
        const auto first = publisher.synchronize(1, input, 1, 0);
        auto added = input[0].primitives.front();
        added.identity.primitiveGuid =
            guid("019fb73d-5a60-7000-8000-000000000043");
        // This new geometry sorts before the old geometry, while its primitive
        // sorts after the first binding. Both unchanged bindings are relocated.
        added.geometryIdentity.sourcePrimitiveGuid =
            guid("019fb73d-5a60-7000-8000-000000000010");
        added.geometryIdentity.primitiveGuid = added.identity.primitiveGuid;
        input[0].primitives.push_back(added);
        ++input[0].observationRevision;
        const auto expanded = publisher.synchronize(1, input, 2, 0);
        CHECK(expanded.instances[0].references.w == 2);
        CHECK(expanded.instances[1].references.z == 2);
        CHECK(expanded.primitives[0].binding.y == 1);
        CHECK(expanded.instanceRevisions[0] != first.instanceRevisions[0]);
        CHECK(expanded.instanceRevisions[1] != first.instanceRevisions[1]);
        CHECK(expanded.primitiveRevisions[0] != first.primitiveRevisions[0]);
        CHECK(expanded.transformRevisions == first.transformRevisions);

        std::vector<uint64_t> frameA = first.instanceRevisions;
        std::vector<uint64_t> frameB = expanded.instanceRevisions;
        std::vector<GpuSceneRecordRange> ranges;
        buildGpuSceneUploadRanges(expanded.instanceRevisions, frameA, ranges);
        CHECK(ranges == std::vector<GpuSceneRecordRange>({ { 0, 2 } }));
        buildGpuSceneUploadRanges(expanded.instanceRevisions, frameB, ranges);
        CHECK(ranges.empty());

        input[0].primitives.pop_back();
        ++input[0].observationRevision;
        const auto shrunk = publisher.synchronize(1, input, 3, 0);
        CHECK(shrunk.instances[1].references.z == 1);
        CHECK(shrunk.primitives[0].binding.y == 0);
        CHECK(shrunk.instanceRevisions[0] != expanded.instanceRevisions[0]);
        CHECK(shrunk.instanceRevisions[1] != expanded.instanceRevisions[1]);
        CHECK(shrunk.primitiveRevisions[0] != expanded.primitiveRevisions[0]);
        const auto unchanged = publisher.synchronize(1, input, 4, 0);
        CHECK(publisher.stats().unchangedFastPath == 1);
        CHECK(unchanged.instanceRevisions == shrunk.instanceRevisions);
        CHECK(unchanged.primitiveRevisions == shrunk.primitiveRevisions);
        return true;
    }

    bool publisherLodGeometryIsSharedAndRevisionSafe() {
        auto input = observed("019fb73d-5a60-7000-8000-000000000050",
            "019fb73d-5a60-7000-8000-000000000051");
        const auto& base = input.primitives.front();
        GpuSceneObservedLodGeometry child{
            .identity = base.geometryIdentity,
            .geometry = GeometryHandle::fromParts(9, 1),
            .firstIndex = 90, .indexCount = 6, .vertexOffset = 120,
            .indexType = base.indexType, .vertexLayout = base.vertexLayout,
            .localBoundsSphere = base.localBoundsSphere,
            .localBoundsMin = base.localBoundsMin, .localBoundsMax = base.localBoundsMax,
            .geometricError = 0.125f,
        };
        // Sort the child before its parent to exercise dense-link resolution.
        child.identity.primitiveGuid = guid("019fb73d-5a60-7000-8000-000000000039");
        input.primitives.front().lodChildren.push_back(child);
        auto second = input;
        second.identity.owner = uuid("019fb73d-5a60-7000-8000-000000000060");
        second.primitives[0].identity.owner = second.identity.owner;
        std::vector observations{ input, second };
        GpuScenePublisher publisher({ 4, 4, 8, 8 });
        const auto first = publisher.synchronize(1, observations, 1, 0);
        CHECK(first.instances.size() == 2 && first.primitives.size() == 2);
        CHECK(first.geometries.size() == 2); // Not duplicated per instance.
        CHECK(first.primitives[0].binding.y == 1 && first.primitives[1].binding.y == 1);
        CHECK(first.geometries[1].state.z == 0);
        CHECK(first.geometries[0].state.z == InvalidGpuSceneIndex);
        CHECK(first.geometries[0].localBoundsMin.w == 0.125f);
        CHECK(first.geometries[0].localBoundsMax.w == 1.0f);
        CHECK(first.geometries[0].draw.y == 6 && first.geometries[0].draw.z == 120);
        const auto childHandle = first.denseGeometryHandles[0];

        observations[0].primitives[0].material = MaterialHandle::fromParts(10, 1);
        ++observations[0].observationRevision;
        const auto materialEdit = publisher.synchronize(1, observations, 2, 0);
        CHECK(materialEdit.geometryRevisions == first.geometryRevisions);
        CHECK(materialEdit.primitiveRevisions != first.primitiveRevisions);
        for (auto& observation : observations) {
            observation.primitives[0].lodChildren[0].geometricError = 0.25f;
            ++observation.observationRevision;
        }
        const auto errorEdit = publisher.synchronize(1, observations, 3, 0);
        CHECK(errorEdit.geometryRevisions[0] != first.geometryRevisions[0]);
        CHECK(errorEdit.geometryRevisions[1] == first.geometryRevisions[1]);
        CHECK(errorEdit.primitiveRevisions == materialEdit.primitiveRevisions);
        for (auto& observation : observations) {
            observation.primitives[0].lodChildren.clear();
            ++observation.observationRevision;
        }
        const auto removed = publisher.synchronize(1, observations, 4, 0);
        CHECK(removed.geometries.size() == 1);
        CHECK(removed.geometries[0].state.z == InvalidGpuSceneIndex);
        CHECK(removed.primitives[0].binding.y == 0);
        CHECK(removed.primitiveRevisions != errorEdit.primitiveRevisions);
        CHECK(publisher.stats().retiredGeometries == 1);
        for (auto& observation : observations) {
            observation.primitives[0].lodChildren.push_back(child);
            ++observation.observationRevision;
        }
        const auto restored = publisher.synchronize(1, observations, 5, 4);
        CHECK(restored.geometries.size() == 2);
        CHECK(restored.denseGeometryHandles[0] != childHandle);
        const auto& unchanged = publisher.synchronize(1, observations, 6, 4);
        CHECK(publisher.stats().unchangedFastPath == 1);
        CHECK(unchanged.geometryRevisions == restored.geometryRevisions);

        GpuScenePublisher bounded({ 4, 4, 1, 8 });
        CHECK(bounded.synchronize(1, std::span(&input, 1), 1, 0).instances.empty());
        CHECK(bounded.directFallbackOwners().size() == 1);
        CHECK(bounded.stats().changedGeometries == 0);
        GpuScenePublisher malformed({ 4, 4, 8, 8 });
        input.primitives[0].lodChildren[0].identity.sourcePrimitiveGuid = {};
        CHECK(malformed.synchronize(1, std::span(&input, 1), 1, 0).instances.empty());
        CHECK(malformed.directFallbackOwners().size() == 1);
        return true;
    }

    bool publisherRetirementPreventsAba() {
        GpuSceneCapacity capacity{ 1, 1, 1, 2 };
        GpuScenePublisher publisher(capacity);
        auto input = observed("019fb73d-5a60-7000-8000-000000000060",
            "019fb73d-5a60-7000-8000-000000000061");
        const auto& first = publisher.synchronize(2, std::span(&input, 1), 10, 0);
        const GpuSceneInstanceHandle oldHandle = first.denseInstanceHandles[0];
        (void)publisher.synchronize(2, {}, 10, 0);
        CHECK(publisher.stats().retiredInstances == 1);
        (void)publisher.synchronize(2, std::span(&input, 1), 11, 9);
        CHECK(publisher.directFallbackOwners().size() == 1);
        const auto& replacement = publisher.synchronize(
            2, std::span(&input, 1), 12, 10);
        CHECK(replacement.instances.size() == 1);
        CHECK(replacement.denseInstanceHandles[0].slot == oldHandle.slot);
        CHECK(replacement.denseInstanceHandles[0].generation ==
            oldHandle.generation + 1);
        return true;
    }

    bool publisherCapacityFallbackIsAtomic() {
        GpuSceneCapacity capacity{ 2, 1, 2, 4 };
        GpuScenePublisher publisher(capacity);
        auto input = observed("019fb73d-5a60-7000-8000-000000000070",
            "019fb73d-5a60-7000-8000-000000000071");
        auto second = input.primitives.front();
        second.identity.sourcePrimitiveGuid =
            guid("019fb73d-5a60-7000-8000-000000000072");
        second.identity.primitiveGuid =
            guid("019fb73d-5a60-7000-8000-000000000073");
        second.geometryIdentity.sourcePrimitiveGuid =
            second.identity.sourcePrimitiveGuid;
        second.geometryIdentity.primitiveGuid = second.identity.primitiveGuid;
        input.primitives.push_back(second);

        const auto& packed = publisher.synchronize(
            3, std::span(&input, 1), 4, 0);
        CHECK(packed.instances.empty());
        CHECK(packed.primitives.empty());
        CHECK(packed.geometries.empty());
        CHECK(publisher.directFallbackOwners().size() == 1);
        CHECK(publisher.stats().capacityFallbackInstances == 1);
        CHECK(publisher.stats().changedInstances == 0);
        CHECK(publisher.stats().changedPrimitives == 0);
        CHECK(publisher.stats().changedGeometries == 0);
        return true;
    }

    bool consumerPrimitiveCollectionIsCameraIndependent() {
        GpuScenePackedTables scene;
        scene.instances.resize(2);
        scene.instances[0].state.z = GpuSceneInstanceEnabled;
        scene.instances[0].state.w = GpuSceneConsumerMainOpaque |
            GpuSceneConsumerShadow | GpuSceneConsumerProbe;
        scene.instances[1].state.z = 0u;
        scene.instances[1].state.w = GpuSceneConsumerShadow;
        scene.primitives.resize(5);
        scene.primitives[0].binding.x = 0u;
        scene.primitives[0].state.w = GpuSceneConsumerShadow;
        scene.primitives[1].binding.x = 0u;
        scene.primitives[1].state.w = GpuSceneConsumerMainOpaque;
        scene.primitives[2].binding.x = 1u;
        scene.primitives[2].state.w = GpuSceneConsumerShadow;
        scene.primitives[3].binding.x = 99u;
        scene.primitives[3].state.w = GpuSceneConsumerShadow;
        scene.primitives[4].binding.x = 0u;
        scene.primitives[4].state.w = GpuSceneConsumerProbe;

        std::vector<uint32_t> indices{ 99u };
        collectGpuSceneConsumerPrimitiveIndices(scene,
            GpuSceneConsumerShadow, indices);
        CHECK(indices == std::vector<uint32_t>{ 0u });

        scene.instances[1].state.z = GpuSceneInstanceEnabled;
        collectGpuSceneConsumerPrimitiveIndices(scene,
            GpuSceneConsumerShadow, indices);
        CHECK((indices == std::vector<uint32_t>{ 0u, 2u }));
        collectGpuSceneConsumerPrimitiveIndices(scene,
            GpuSceneConsumerProbe, indices);
        CHECK(indices == std::vector<uint32_t>{ 4u });
        collectGpuSceneConsumerPrimitiveIndices(scene, 0u, indices);
        CHECK(indices.empty());
        return true;
    }

    bool publishedConsumerMembershipRevisionsAreTransformIndependent() {
        GpuScenePackedTables scene;
        scene.sceneEpoch = 41u;
        scene.instances.resize(1);
        scene.instances[0].state.x = 3u;
        scene.instances[0].state.z = packGpuSceneInstanceFlags(
            GpuSceneInstanceEnabled, 4u);
        scene.instances[0].state.w = GpuSceneConsumerShadow |
            GpuSceneConsumerProbe;
        scene.primitives.resize(1);
        scene.primitives[0].binding = { 0u, 0u, 17u, 19u };
        scene.primitives[0].state = { 5u, GpuScenePrimitiveAlphaMask, 0u,
            GpuSceneConsumerShadow | GpuSceneConsumerProbe };
        scene.primitives[0].revisions = { 7u, 11u, 13u, 23u };
        scene.geometries.resize(1);
        scene.geometries[0].draw = { 2u, 36u, 0u, 1u };
        scene.geometries[0].storage = { 3u, 4u, 5u, 6u };
        scene.geometries[0].state = { 2u, 0u, InvalidGpuSceneIndex, 29u };
        scene.geometryRevisions = { 31u };

        publishGpuSceneConsumerMembership(scene);
        CHECK(scene.shadowConsumerPrimitiveIndices ==
            std::vector<uint32_t>{ 0u });
        CHECK(scene.probeConsumerPrimitiveIndices ==
            std::vector<uint32_t>{ 0u });
        const uint64_t shadowRevision =
            scene.shadowConsumerMembershipRevision;
        const uint64_t probeRevision = scene.probeConsumerMembershipRevision;

        scene.instances[0].worldBoundsSphere.x = 500.0f;
        scene.instances[0].worldBoundsMin.y = -300.0f;
        scene.transforms.resize(1);
        scene.transforms[0].row0.w = 900.0f;
        scene.transformRevisions = { 999u };
        publishGpuSceneConsumerMembership(scene);
        CHECK(scene.shadowConsumerMembershipRevision == shadowRevision);
        CHECK(scene.probeConsumerMembershipRevision == probeRevision);

        scene.instances[0].state.z = packGpuSceneInstanceFlags(
            GpuSceneInstanceEnabled, 2u);
        publishGpuSceneConsumerMembership(scene);
        CHECK(scene.shadowConsumerMembershipRevision != shadowRevision);
        const uint64_t lodRevision = scene.shadowConsumerMembershipRevision;

        scene.primitives[0].revisions.w += 1u;
        publishGpuSceneConsumerMembership(scene);
        CHECK(scene.shadowConsumerMembershipRevision != lodRevision);

        scene.instances[0].state.z &= ~GpuSceneInstanceEnabled;
        publishGpuSceneConsumerMembership(scene);
        CHECK(scene.shadowConsumerPrimitiveIndices.empty());
        CHECK(scene.probeConsumerPrimitiveIndices.empty());
        return true;
    }
}

int main() {
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        { "ABI and affine contract", abiAndAffineContract },
        { "lifetime rejects stale handles", lifetimeRejectsStaleHandles },
        { "deterministic packing preserves cold identity", deterministicPackingPreservesColdIdentity },
        { "frame context upload planning is exact", frameContextUploadPlanningIsExact },
        { "reference LOD links use accepted sources", referenceLodLinksUseOnlyAcceptedSources },
        { "uncertainty and overflow fail to direct path", uncertaintyAndOverflowFailToDirectPath },
        { "publisher is sparse and revision isolated",
            publisherIsSparseAndRevisionIsolated },
        { "publisher retirement prevents ABA", publisherRetirementPreventsAba },
        { "publisher shares and safely revises LOD geometry", publisherLodGeometryIsSharedAndRevisionSafe },
        { "publisher revises relocated dense references",
            publisherRevisesRelocatedDenseReferences },
        { "publisher capacity fallback is atomic",
            publisherCapacityFallbackIsAtomic },
        { "consumer primitive collection is camera independent",
            consumerPrimitiveCollectionIsCameraIndependent },
        { "published consumer membership revisions are transform independent",
            publishedConsumerMembershipRevisionsAreTransformIndependent },
    };
    size_t passed = 0;
    for (const Test& test : tests) {
        if (!test.run()) { std::cerr << "[FAIL] " << test.name << '\n'; return 1; }
        std::cout << "[PASS] " << test.name << '\n'; ++passed;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return 0;
}
