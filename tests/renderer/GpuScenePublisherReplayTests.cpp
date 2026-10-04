// M7R R5c.5: the incremental GpuScenePublisher must publish exactly what the
// full-scan publisher it replaced published (packed bytes, revisions, slot
// handles, membership, statistics and fallback owners) for every input
// sequence that honours the observation-revision contract. The full-scan
// publisher is kept test-only in support/ReferenceGpuScenePublisher.
//
// The caller model mirrors Application::prepareGpuScenePublication: one
// observation slot per accepted mesh entity in pool order, and a per-slot
// revision that advances whenever the slot's content changes (slot metadata is
// dropped when the observation count shrinks).
//
// Run with --scale for an H-stress sized replay (1,024 instances, 113
// primitives each) that also prints synchronize durations of both publishers.

#include "renderer/scene/GpuScenePublisher.h"
#include "support/ReferenceGpuScenePublisher.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Iridium;

    template<typename T>
    bool bytesEqual(const std::vector<T>& lhs, const std::vector<T>& rhs) {
        static_assert(std::is_trivially_copyable_v<T>);
        return lhs.size() == rhs.size() && (lhs.empty() ||
            std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(T)) == 0);
    }

    template<typename T>
    bool podEqual(const T& lhs, const T& rhs) {
        return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
    }

    std::string compareTables(const GpuScenePackedTables& expected,
        const GpuScenePackedTables& actual) {
#define IRIDIUM_COMPARE(field, equal) \
        if (!(equal)) return #field;
        IRIDIUM_COMPARE(abiVersion, expected.abiVersion == actual.abiVersion)
        IRIDIUM_COMPARE(sceneEpoch, expected.sceneEpoch == actual.sceneEpoch)
        IRIDIUM_COMPARE(publicationRevision,
            expected.publicationRevision == actual.publicationRevision)
        IRIDIUM_COMPARE(transforms, bytesEqual(expected.transforms, actual.transforms))
        IRIDIUM_COMPARE(instances, bytesEqual(expected.instances, actual.instances))
        IRIDIUM_COMPARE(primitives, bytesEqual(expected.primitives, actual.primitives))
        IRIDIUM_COMPARE(geometries, bytesEqual(expected.geometries, actual.geometries))
        IRIDIUM_COMPARE(transformRevisions,
            bytesEqual(expected.transformRevisions, actual.transformRevisions))
        IRIDIUM_COMPARE(instanceRevisions,
            bytesEqual(expected.instanceRevisions, actual.instanceRevisions))
        IRIDIUM_COMPARE(primitiveRevisions,
            bytesEqual(expected.primitiveRevisions, actual.primitiveRevisions))
        IRIDIUM_COMPARE(geometryRevisions,
            bytesEqual(expected.geometryRevisions, actual.geometryRevisions))
        IRIDIUM_COMPARE(instanceIdentities,
            expected.instanceIdentities == actual.instanceIdentities)
        IRIDIUM_COMPARE(primitiveIdentities,
            expected.primitiveIdentities == actual.primitiveIdentities)
        IRIDIUM_COMPARE(geometryIdentities,
            expected.geometryIdentities == actual.geometryIdentities)
        IRIDIUM_COMPARE(denseInstanceHandles,
            bytesEqual(expected.denseInstanceHandles, actual.denseInstanceHandles))
        IRIDIUM_COMPARE(densePrimitiveHandles,
            bytesEqual(expected.densePrimitiveHandles, actual.densePrimitiveHandles))
        IRIDIUM_COMPARE(denseGeometryHandles,
            bytesEqual(expected.denseGeometryHandles, actual.denseGeometryHandles))
        IRIDIUM_COMPARE(directFallbackInstances, bytesEqual(
            expected.directFallbackInstances, actual.directFallbackInstances))
        IRIDIUM_COMPARE(shadowConsumerPrimitiveIndices, bytesEqual(
            expected.shadowConsumerPrimitiveIndices,
            actual.shadowConsumerPrimitiveIndices))
        IRIDIUM_COMPARE(probeConsumerPrimitiveIndices, bytesEqual(
            expected.probeConsumerPrimitiveIndices,
            actual.probeConsumerPrimitiveIndices))
        IRIDIUM_COMPARE(shadowConsumerMembershipRevision,
            expected.shadowConsumerMembershipRevision ==
                actual.shadowConsumerMembershipRevision)
        IRIDIUM_COMPARE(probeConsumerMembershipRevision,
            expected.probeConsumerMembershipRevision ==
                actual.probeConsumerMembershipRevision)
        IRIDIUM_COMPARE(shadowConsumerContentWatermark,
            expected.shadowConsumerContentWatermark ==
                actual.shadowConsumerContentWatermark)
        IRIDIUM_COMPARE(probeConsumerContentWatermark,
            expected.probeConsumerContentWatermark ==
                actual.probeConsumerContentWatermark)
        IRIDIUM_COMPARE(invalidSourceCount,
            expected.invalidSourceCount == actual.invalidSourceCount)
        IRIDIUM_COMPARE(capacityOmittedInstanceCount,
            expected.capacityOmittedInstanceCount ==
                actual.capacityOmittedInstanceCount)
#undef IRIDIUM_COMPARE
        return {};
    }

    std::string compareStats(const GpuScenePublisherStats& expected,
        const GpuScenePublisherStats& actual) {
#define IRIDIUM_COMPARE(field) \
        if (expected.field != actual.field) return "stats." #field;
        IRIDIUM_COMPARE(activeInstances)
        IRIDIUM_COMPARE(activePrimitives)
        IRIDIUM_COMPARE(activeGeometries)
        IRIDIUM_COMPARE(changedInstances)
        IRIDIUM_COMPARE(changedTransforms)
        IRIDIUM_COMPARE(changedPrimitives)
        IRIDIUM_COMPARE(changedGeometries)
        IRIDIUM_COMPARE(retiredInstances)
        IRIDIUM_COMPARE(retiredPrimitives)
        IRIDIUM_COMPARE(retiredGeometries)
        IRIDIUM_COMPARE(capacityFallbackInstances)
        IRIDIUM_COMPARE(unchangedFastPath)
        IRIDIUM_COMPARE(changedInstanceBytes)
        IRIDIUM_COMPARE(changedTransformBytes)
        IRIDIUM_COMPARE(changedPrimitiveBytes)
        IRIDIUM_COMPARE(changedGeometryBytes)
#undef IRIDIUM_COMPARE
        return {};
    }

    // --- caller model -------------------------------------------------------

    template<typename Bytes>
    Bytes randomBytes(std::mt19937_64& random) {
        Bytes bytes{};
        for (auto& byte : bytes) byte = static_cast<uint8_t>(random() & 0xffu);
        bytes[0] = static_cast<uint8_t>(bytes[0] | 1u); // never nil
        return bytes;
    }

    AssetGuid randomGuid(std::mt19937_64& random) {
        return AssetGuid(randomBytes<AssetGuid::Bytes>(random));
    }

    SceneEntityUuid randomOwner(std::mt19937_64& random) {
        return SceneEntityUuid(randomBytes<SceneEntityUuid::Bytes>(random));
    }

    struct ModelPrimitive {
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;
        AssetGuid materialGuid;
        GeometryHandle geometry;
        MaterialHandle material;
        PipelineHandle pipeline;
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t indexType = 1, vertexLayout = 0;
        uint32_t flags = GpuScenePrimitiveOpaque;
        uint32_t consumerMask = GpuSceneConsumerMainOpaque |
            GpuSceneConsumerShadow | GpuSceneConsumerProbe;
        glm::vec4 sphere{ 0.0f, 0.0f, 0.0f, 2.0f };
        glm::vec4 minimum{ -1.0f, -1.0f, -1.0f, 0.0f };
        glm::vec4 maximum{ 1.0f, 1.0f, 1.0f, 0.0f };
        uint32_t materialRevision = 1;
        std::vector<GpuSceneObservedLodGeometry> lods;
    };

    struct Model {
        AssetGuid guid;
        std::string cookKey;
        uint32_t productRevision = 1;
        std::vector<ModelPrimitive> primitives;
    };

    struct SceneEntity {
        SceneEntityUuid owner;
        uint32_t model = 0;
        glm::mat4 world{ 1.0f };
        uint32_t flags = GpuSceneInstanceEnabled;
        uint32_t maximumLod = MaximumGpuSceneLodLevels - 1u;
        uint32_t consumerMask = GpuSceneConsumerMainOpaque |
            GpuSceneConsumerShadow | GpuSceneConsumerProbe;
        uint32_t materialVariant = 0;
        uint64_t primitiveMask = ~0ull;  // which model primitives it observes
        int64_t productOverride = -1;    // incoherent shared geometry
        bool empty = false;
        bool duplicatePrimitive = false;
        bool invalidLod = false;
        bool zeroRevision = false;
    };

    GpuSceneObservedLodGeometry makeLod(std::mt19937_64& random,
        const Model& model, const ModelPrimitive& base, size_t level,
        uint32_t previousCount, float previousError) {
        GpuSceneObservedLodGeometry lod;
        lod.identity = { model.guid, base.sourcePrimitiveGuid, randomGuid(random) };
        lod.geometry = GeometryHandle::fromParts(
            static_cast<uint32_t>(10 + random() % 50), 1);
        lod.indexCount = (std::max)(3u, (previousCount / 3u / 2u) * 3u);
        if (lod.indexCount >= previousCount) lod.indexCount = 0; // caller trims
        lod.firstIndex = static_cast<uint32_t>(random() % 1000);
        lod.vertexOffset = static_cast<int32_t>(random() % 100) - 50;
        lod.indexType = base.indexType;
        lod.vertexLayout = base.vertexLayout;
        lod.localBoundsSphere = base.sphere;
        lod.localBoundsMin = base.minimum;
        lod.localBoundsMax = base.maximum;
        lod.geometricError = previousError + 0.125f *
            static_cast<float>(1 + level + random() % 3);
        return lod;
    }

    void rebuildLods(std::mt19937_64& random, const Model& model,
        ModelPrimitive& primitive, size_t levels) {
        primitive.lods.clear();
        uint32_t previousCount = primitive.indexCount;
        float previousError = 0.0f;
        for (size_t level = 0; level < levels; ++level) {
            auto lod = makeLod(random, model, primitive, level, previousCount,
                previousError);
            if (lod.indexCount == 0) break;
            previousCount = lod.indexCount;
            previousError = lod.geometricError;
            primitive.lods.push_back(lod);
        }
    }

    Model makeModel(std::mt19937_64& random, size_t primitiveCount,
        size_t lodLevels, bool shareSourcePrimitives) {
        Model model;
        model.guid = randomGuid(random);
        model.cookKey = "cook-" + std::to_string(random() % 100000);
        const AssetGuid sharedSource = randomGuid(random);
        for (size_t index = 0; index < primitiveCount; ++index) {
            ModelPrimitive primitive;
            primitive.sourcePrimitiveGuid = shareSourcePrimitives && index % 2 == 0
                ? sharedSource : randomGuid(random);
            primitive.primitiveGuid = randomGuid(random);
            primitive.materialGuid = randomGuid(random);
            primitive.geometry = GeometryHandle::fromParts(
                static_cast<uint32_t>(1 + random() % 8), 1);
            primitive.material = MaterialHandle::fromParts(
                static_cast<uint32_t>(1 + random() % 6), 1);
            primitive.pipeline = PipelineHandle::fromParts(
                static_cast<uint32_t>(1 + random() % 3), 1);
            primitive.firstIndex = static_cast<uint32_t>(random() % 4096);
            primitive.indexCount = 3u * static_cast<uint32_t>(64 + random() % 512);
            primitive.vertexOffset = static_cast<int32_t>(random() % 256) - 128;
            primitive.indexType = static_cast<uint32_t>(random() % 2);
            primitive.vertexLayout = static_cast<uint32_t>(random() % 4);
            primitive.flags = (random() % 4 == 0)
                ? (GpuScenePrimitiveOpaque | GpuScenePrimitiveAlphaMask)
                : GpuScenePrimitiveOpaque;
            primitive.consumerMask = (random() % 5 == 0)
                ? (GpuSceneConsumerForwardOpaque | GpuSceneConsumerShadow |
                    GpuSceneConsumerProbe)
                : (GpuSceneConsumerMainOpaque | GpuSceneConsumerShadow |
                    GpuSceneConsumerProbe);
            const float size = 0.5f + static_cast<float>(random() % 8);
            primitive.sphere = { 0.0f, 0.0f, 0.0f, size * 1.8f };
            primitive.minimum = { -size, -size, -size, 0.0f };
            primitive.maximum = { size, size, size, 0.0f };
            primitive.materialRevision = primitive.material.id;
            model.primitives.push_back(primitive);
            rebuildLods(random, model, model.primitives.back(),
                lodLevels == 0 ? 0 : random() % (lodLevels + 1));
        }
        return model;
    }

    GpuSceneObservedInstance observe(const std::vector<Model>& models,
        const SceneEntity& entity) {
        const Model& model = models[entity.model];
        GpuSceneObservedInstance result;
        result.identity = { entity.owner, model.guid, 1, model.cookKey };
        result.flags = entity.flags;
        result.maximumLod = entity.maximumLod;
        result.consumerMask = entity.consumerMask;
        result.worldTransform = entity.world;
        const glm::vec3 center = glm::vec3(entity.world[3]);
        result.worldBoundsSphere = glm::vec4(center, 9.0f);
        result.worldBoundsMin = glm::vec4(center - glm::vec3(5.0f), 0.0f);
        result.worldBoundsMax = glm::vec4(center + glm::vec3(5.0f), 0.0f);
        if (entity.empty) return result;
        for (size_t index = 0; index < model.primitives.size(); ++index) {
            if (index < 64 && ((entity.primitiveMask >> index) & 1u) == 0u)
                continue;
            const ModelPrimitive& source = model.primitives[index];
            GpuSceneObservedPrimitive primitive{
                .identity = { entity.owner, source.sourcePrimitiveGuid,
                    source.primitiveGuid, source.materialGuid },
                .geometryIdentity = { model.guid, source.sourcePrimitiveGuid,
                    source.primitiveGuid },
                .legacyGeometry = source.geometry,
                .material = source.material,
                .pipeline = source.pipeline,
                .firstIndex = source.firstIndex,
                .indexCount = source.indexCount,
                .vertexOffset = source.vertexOffset,
                .indexType = source.indexType,
                .vertexLayout = source.vertexLayout,
                .primitiveFlags = source.flags,
                .consumerMask = source.consumerMask,
                .localBoundsSphere = source.sphere,
                .localBoundsMin = source.minimum,
                .localBoundsMax = source.maximum,
                .geometryProductRevision = entity.productOverride >= 0
                    ? static_cast<uint32_t>(entity.productOverride)
                    : model.productRevision,
                .materialRevision = source.materialRevision,
                .lodChildren = source.lods,
            };
            if (index == 0 && entity.materialVariant != 0) {
                primitive.material = MaterialHandle::fromParts(
                    20 + entity.materialVariant % 50, 1);
                primitive.materialRevision = primitive.material.id;
                primitive.identity.effectiveMaterialGuid = AssetGuid(
                    AssetGuid::Bytes{ 7, static_cast<uint8_t>(entity.materialVariant) });
            }
            if (index == 0 && entity.invalidLod && !primitive.lodChildren.empty())
                primitive.lodChildren.front().identity.primitiveGuid = {};
            result.primitives.push_back(std::move(primitive));
        }
        if (entity.duplicatePrimitive && !result.primitives.empty())
            result.primitives.push_back(result.primitives.front());
        return result;
    }

    bool sameObservation(const GpuSceneObservedInstance& lhs,
        const GpuSceneObservedInstance& rhs) {
        if (lhs.identity != rhs.identity || lhs.mobility != rhs.mobility ||
            lhs.flags != rhs.flags || lhs.maximumLod != rhs.maximumLod ||
            lhs.consumerMask != rhs.consumerMask ||
            !podEqual(lhs.worldTransform, rhs.worldTransform) ||
            !podEqual(lhs.worldBoundsSphere, rhs.worldBoundsSphere) ||
            !podEqual(lhs.worldBoundsMin, rhs.worldBoundsMin) ||
            !podEqual(lhs.worldBoundsMax, rhs.worldBoundsMax) ||
            lhs.primitives.size() != rhs.primitives.size())
            return false;
        for (size_t index = 0; index < lhs.primitives.size(); ++index) {
            const auto& a = lhs.primitives[index];
            const auto& b = rhs.primitives[index];
            if (a.identity != b.identity || a.geometryIdentity != b.geometryIdentity ||
                a.legacyGeometry != b.legacyGeometry || a.material != b.material ||
                a.pipeline != b.pipeline || a.firstIndex != b.firstIndex ||
                a.indexCount != b.indexCount || a.vertexOffset != b.vertexOffset ||
                a.indexType != b.indexType || a.vertexLayout != b.vertexLayout ||
                a.primitiveFlags != b.primitiveFlags ||
                a.consumerMask != b.consumerMask ||
                !podEqual(a.localBoundsSphere, b.localBoundsSphere) ||
                !podEqual(a.localBoundsMin, b.localBoundsMin) ||
                !podEqual(a.localBoundsMax, b.localBoundsMax) ||
                a.geometryProductRevision != b.geometryProductRevision ||
                a.materialRevision != b.materialRevision ||
                a.lodChildren.size() != b.lodChildren.size())
                return false;
            for (size_t child = 0; child < a.lodChildren.size(); ++child) {
                const auto& c = a.lodChildren[child];
                const auto& d = b.lodChildren[child];
                if (c.identity != d.identity || c.geometry != d.geometry ||
                    c.firstIndex != d.firstIndex || c.indexCount != d.indexCount ||
                    c.vertexOffset != d.vertexOffset || c.indexType != d.indexType ||
                    c.vertexLayout != d.vertexLayout ||
                    !podEqual(c.localBoundsSphere, d.localBoundsSphere) ||
                    !podEqual(c.localBoundsMin, d.localBoundsMin) ||
                    !podEqual(c.localBoundsMax, d.localBoundsMax) ||
                    !podEqual(c.geometricError, d.geometricError))
                    return false;
            }
        }
        return true;
    }

    // Application-like observation slots: one per observed entity in pool
    // order; the slot revision advances when the slot's content changes.
    struct ObservationSlots {
        struct Slot {
            GpuSceneObservedInstance previous;
            uint64_t revision = 0;
            bool valid = false;
        };
        std::vector<Slot> slots;
        std::vector<GpuSceneObservedInstance> observations;

        void build(const std::vector<Model>& models,
            const std::vector<SceneEntity>& entities) {
            observations.resize(entities.size());
            if (slots.size() < entities.size()) slots.resize(entities.size());
            for (size_t index = 0; index < entities.size(); ++index) {
                GpuSceneObservedInstance observation = observe(models, entities[index]);
                Slot& slot = slots[index];
                if (!slot.valid || !sameObservation(slot.previous, observation)) {
                    ++slot.revision;
                    slot.previous = observation;
                    slot.valid = true;
                }
                observation.observationRevision =
                    entities[index].zeroRevision ? 0u : slot.revision;
                observations[index] = std::move(observation);
            }
            slots.resize(entities.size());
        }
    };

    struct Replay {
        GpuScenePublisher incremental;
        ReferenceGpuScenePublisher reference;
        ObservationSlots slots;
        uint64_t frame = 0;
        uint64_t completed = 0;
        uint64_t epoch = 1;
        size_t steps = 0;
        double incrementalSeconds = 0.0, referenceSeconds = 0.0;
        double maximumIncrementalSeconds = 0.0, maximumReferenceSeconds = 0.0;

        explicit Replay(GpuSceneCapacity capacity)
            : incremental(capacity), reference(capacity) {}

        bool step(const std::vector<Model>& models,
            const std::vector<SceneEntity>& entities, const char* label,
            bool stallCompletion = false) {
            slots.build(models, entities);
            ++frame;
            if (!stallCompletion) completed = frame > 2 ? frame - 2 : 0;
            using Clock = std::chrono::steady_clock;
            const auto referenceStart = Clock::now();
            const GpuScenePackedTables& expected = reference.synchronize(
                epoch, slots.observations, frame, completed);
            const auto incrementalStart = Clock::now();
            const GpuScenePackedTables& actual = incremental.synchronize(
                epoch, slots.observations, frame, completed);
            const auto end = Clock::now();
            const double referenceTime = std::chrono::duration<double>(
                incrementalStart - referenceStart).count();
            const double incrementalTime = std::chrono::duration<double>(
                end - incrementalStart).count();
            referenceSeconds += referenceTime;
            incrementalSeconds += incrementalTime;
            maximumReferenceSeconds = (std::max)(maximumReferenceSeconds, referenceTime);
            maximumIncrementalSeconds = (std::max)(maximumIncrementalSeconds,
                incrementalTime);
            ++steps;
            std::string difference = compareTables(expected, actual);
            if (difference.empty())
                difference = compareStats(reference.stats(), incremental.stats());
            if (difference.empty()) {
                const auto a = reference.directFallbackOwners();
                const auto b = incremental.directFallbackOwners();
                if (!std::equal(a.begin(), a.end(), b.begin(), b.end()))
                    difference = "directFallbackOwners";
            }
            if (!difference.empty()) {
                std::cerr << "replay diverged at frame " << frame << " (" << label
                    << "): " << difference << '\n';
                return false;
            }
            return true;
        }
    };

    glm::mat4 placed(std::mt19937_64& random) {
        return glm::translate(glm::mat4(1.0f), {
            static_cast<float>(random() % 200) - 100.0f,
            static_cast<float>(random() % 20),
            static_cast<float>(random() % 200) - 100.0f });
    }

    SceneEntity newEntity(std::mt19937_64& random, uint32_t modelCount) {
        SceneEntity entity;
        entity.owner = randomOwner(random);
        entity.model = static_cast<uint32_t>(random() % modelCount);
        entity.world = placed(random);
        return entity;
    }

    // --- randomized sequences ---------------------------------------------

    bool randomizedSequence(uint64_t seed, size_t stepCount, size_t& checkedSteps) {
        std::mt19937_64 random(seed);
        const bool tight = seed % 3 == 0;
        const GpuSceneCapacity capacity = tight
            ? GpuSceneCapacity{ static_cast<uint32_t>(6 + random() % 20),
                static_cast<uint32_t>(16 + random() % 120),
                static_cast<uint32_t>(8 + random() % 40), 1u << 10u }
            : GpuSceneCapacity{ 4096, 1u << 14u, 4096, 1u << 13u };
        std::vector<Model> models;
        const size_t modelCount = 1 + random() % 4;
        for (size_t index = 0; index < modelCount; ++index)
            models.push_back(makeModel(random, 1 + random() % 9,
                random() % 3 == 0 ? 0 : 4, random() % 4 == 0));
        std::vector<SceneEntity> entities;
        const size_t initial = random() % 12;
        for (size_t index = 0; index < initial; ++index)
            entities.push_back(newEntity(random, static_cast<uint32_t>(models.size())));

        Replay replay(capacity);
        replay.epoch = seed % 7 == 0 ? 0 : 1;
        if (!replay.step(models, entities, "initial")) return false;
        for (size_t stepIndex = 0; stepIndex < stepCount; ++stepIndex) {
            const size_t mutations = random() % 4;
            for (size_t mutation = 0; mutation < mutations; ++mutation) {
                const uint64_t kind = random() % 100;
                SceneEntity* target = entities.empty() ? nullptr
                    : &entities[random() % entities.size()];
                if (kind < 18 && target) {
                    target->world = glm::translate(target->world, {
                        0.25f * static_cast<float>(random() % 5), 0.0f, 0.5f });
                } else if (kind < 28) {
                    entities.push_back(newEntity(random,
                        static_cast<uint32_t>(models.size())));
                } else if (kind < 36 && !entities.empty()) {
                    // Component-pool removal: swap with the last, then pop.
                    const size_t removed = random() % entities.size();
                    entities[removed] = entities.back();
                    entities.pop_back();
                } else if (kind < 40 && !entities.empty()) {
                    entities.pop_back();
                } else if (kind < 45 && target) {
                    target->flags ^= GpuSceneInstanceSelected;
                    target->consumerMask ^= GpuSceneConsumerSelection;
                } else if (kind < 49 && target) {
                    target->maximumLod = static_cast<uint32_t>(random() % 20);
                } else if (kind < 54 && target) {
                    target->materialVariant = static_cast<uint32_t>(random() % 4);
                } else if (kind < 58 && target) {
                    target->model = static_cast<uint32_t>(random() % models.size());
                } else if (kind < 62 && target) {
                    target->primitiveMask = random() | random();
                } else if (kind < 65) {
                    // Reimport: every instance of the model changes together.
                    ++models[random() % models.size()].productRevision;
                } else if (kind < 68) {
                    Model& model = models[random() % models.size()];
                    ModelPrimitive& primitive =
                        model.primitives[random() % model.primitives.size()];
                    if (!primitive.lods.empty() && random() % 2 == 0)
                        primitive.lods.front().geometricError += 0.0625f;
                    else
                        rebuildLods(random, model, primitive, random() % 4);
                } else if (kind < 71 && target) {
                    // Incoherent shared geometry: one observer disagrees.
                    target->productOverride = target->productOverride >= 0
                        ? -1 : static_cast<int64_t>(random() % 3);
                } else if (kind < 73 && target) {
                    target->empty = !target->empty;
                } else if (kind < 75 && target) {
                    target->duplicatePrimitive = !target->duplicatePrimitive;
                } else if (kind < 77 && target) {
                    target->invalidLod = !target->invalidLod;
                } else if (kind < 79 && target) {
                    target->zeroRevision = !target->zeroRevision;
                } else if (kind < 81 && target) {
                    target->flags ^= GpuSceneInstanceHistoryReset;
                } else if (kind < 83 && target && entities.size() > 1) {
                    target->owner = entities[random() % entities.size()].owner;
                } else if (kind < 84 && target) {
                    target->owner = {};
                } else if (kind < 88 && entities.size() > 1) {
                    std::swap(entities[random() % entities.size()],
                        entities[random() % entities.size()]);
                } else if (kind < 89) {
                    ++replay.epoch;
                } else if (kind < 92 && target) {
                    // A child LOD reuses a sibling's base identity.
                    Model& model = models[target->model];
                    if (model.primitives.size() > 1 &&
                        !model.primitives[0].lods.empty() &&
                        model.primitives[0].sourcePrimitiveGuid ==
                            model.primitives[1].sourcePrimitiveGuid)
                        model.primitives[0].lods.front().identity.primitiveGuid =
                            model.primitives[1].primitiveGuid;
                } else if (kind < 94) {
                    models.push_back(makeModel(random, 1 + random() % 6, 3,
                        random() % 2 == 0));
                }
            }
            const bool stall = random() % 6 == 0;
            if (!replay.step(models, entities, "random", stall)) {
                std::cerr << "seed " << seed << " step " << stepIndex << '\n';
                return false;
            }
        }
        checkedSteps += replay.steps;
        return true;
    }

    bool randomizedSequencesMatchReference() {
        size_t checked = 0;
        for (uint64_t seed = 1; seed <= 400; ++seed)
            if (!randomizedSequence(seed, 80, checked)) return false;
        std::cout << "  randomized: 400 sequences, " << checked
            << " publications byte-identical\n";
        return true;
    }

    // --- scripted patterns --------------------------------------------------

    // m7r_hitch_stress_v1 instance events on a fixture of `fixture` instances
    // of one model: +a, +b, -b (LIFO), +b, with unchanged frames between, plus
    // the editor selecting and deselecting one instance.
    bool hitchStressPattern(size_t fixture, size_t primitives, size_t first,
        size_t second, size_t quietFrames, bool printTiming) {
        std::mt19937_64 random(0x5eed + fixture);
        std::vector<Model> models{ makeModel(random, primitives, 3, false) };
        std::vector<SceneEntity> entities;
        for (size_t index = 0; index < fixture; ++index)
            entities.push_back(newEntity(random, 1));
        Replay replay({ 1u << 16u, 1u << 20u, 1u << 16u, 1u << 17u });
        std::vector<std::pair<std::string, double>> eventTimes;
        const auto quiet = [&](const char* label) {
            for (size_t frame = 0; frame < quietFrames; ++frame)
                if (!replay.step(models, entities, label)) return false;
            return true;
        };
        const auto event = [&](const std::string& label) {
            const double referenceBefore = replay.referenceSeconds;
            const double incrementalBefore = replay.incrementalSeconds;
            if (!replay.step(models, entities, label.c_str())) return false;
            if (printTiming) {
                std::cout << "  " << label << ": reference "
                    << 1000.0 * (replay.referenceSeconds - referenceBefore)
                    << " ms, incremental "
                    << 1000.0 * (replay.incrementalSeconds - incrementalBefore)
                    << " ms (" << replay.incremental.stats().activePrimitives
                    << " primitives)\n";
            }
            return true;
        };
        const auto add = [&](size_t count) {
            for (size_t index = 0; index < count; ++index)
                entities.push_back(newEntity(random, 1));
        };
        if (!event("startup") || !quiet("warmup")) return false;
        add(first);
        if (!event("add_instances " + std::to_string(first)) || !quiet("steady"))
            return false;
        add(second);
        if (!event("add_instances " + std::to_string(second)) || !quiet("steady"))
            return false;
        entities[3].flags |= GpuSceneInstanceSelected;
        entities[3].consumerMask |= GpuSceneConsumerSelection;
        if (!event("select") || !quiet("steady")) return false;
        entities[3].flags &= ~GpuSceneInstanceSelected;
        entities[3].consumerMask &= ~GpuSceneConsumerSelection;
        if (!event("deselect")) return false;
        entities.resize(entities.size() - second);
        if (!event("remove_instances " + std::to_string(second)) || !quiet("steady"))
            return false;
        add(second);
        if (!event("add_instances " + std::to_string(second)) || !quiet("steady"))
            return false;
        // One moving instance (the F-fixture animated pattern).
        for (size_t frame = 0; frame < quietFrames; ++frame) {
            entities[1].world = glm::translate(entities[1].world, { 0.1f, 0.0f, 0.0f });
            if (!event("move one")) return false;
        }
        if (printTiming) {
            std::cout << "  maxima: reference " << 1000.0 * replay.maximumReferenceSeconds
                << " ms, incremental " << 1000.0 * replay.maximumIncrementalSeconds
                << " ms over " << replay.steps << " publications\n";
        }
        return true;
    }

    bool scriptedHitchPatternsMatchReference() {
        if (!hitchStressPattern(16, 13, 16, 32, 3, false)) return false;
        if (!hitchStressPattern(64, 31, 64, 128, 4, false)) return false;
        std::cout << "  scripted: H-stress instance events at two scales identical\n";
        return true;
    }

    // Every instance moving each frame, plus material edits and reimports
    // (the animated and editor fixtures' change patterns).
    bool animatedPatternsMatchReference() {
        std::mt19937_64 random(77);
        std::vector<Model> models{ makeModel(random, 9, 3, true),
            makeModel(random, 4, 0, false) };
        std::vector<SceneEntity> entities;
        for (size_t index = 0; index < 48; ++index)
            entities.push_back(newEntity(random, 2));
        Replay replay({ 1024, 4096, 1024, 2048 });
        for (size_t frame = 0; frame < 120; ++frame) {
            for (size_t index = 0; index < entities.size(); index += 1 + frame % 3)
                entities[index].world = glm::rotate(entities[index].world, 0.01f,
                    { 0.0f, 1.0f, 0.0f });
            if (frame % 17 == 5)
                entities[frame % entities.size()].materialVariant =
                    static_cast<uint32_t>(frame % 3);
            if (frame % 29 == 11) ++models[frame % 2].productRevision;
            if (!replay.step(models, entities, "animated")) return false;
        }
        std::cout << "  animated: 120 frames identical\n";
        return true;
    }

    bool scaleReplay() {
        std::cout << "H-stress scale (1,024 instances x 113 primitives):\n";
        return hitchStressPattern(256, 113, 256, 512, 2, true);
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--scale")
        return scaleReplay() ? 0 : 1;
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        { "randomized sequences match the full-scan reference",
            randomizedSequencesMatchReference },
        { "scripted hitch patterns match the full-scan reference",
            scriptedHitchPatternsMatchReference },
        { "animated patterns match the full-scan reference",
            animatedPatternsMatchReference },
    };
    size_t passed = 0;
    for (const Test& test : tests) {
        if (!test.run()) { std::cerr << "[FAIL] " << test.name << '\n'; return 1; }
        std::cout << "[PASS] " << test.name << '\n'; ++passed;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return 0;
}
