// M7R R5c.6: the change-driven LightExtractor, the allocation-free
// extractReflectionProbes and the change-driven ReflectionProbePublisher must
// publish exactly what the full-scan implementations they replaced published:
// packed records, record revisions, active lists and their revision, changed
// ranges, selection metadata, statistics, diagnostics and slot lookups, for
// every input sequence. The full-scan implementations are kept test-only in
// support/ReferenceLightExtractor and support/ReferenceReflectionProbe.
//
// Randomized sequences mutate every input the extractors read (component
// fields, transforms and rotation chains, parenting including cycles, identity,
// component and entity lifetime, world clears and world switches, capacity
// growth and exhaustion), with values drawn from small sets so A->B->A changes
// occur. Scripted sequences follow the H-stress add_lights and H-probe events.
// Steady frames of the production path must not allocate, including the
// per-frame shadow selection, local-shadow requests, atlas/pool reconciliation
// and cache scheduling that consume the light packet.

#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/lighting/LocalShadow.h"
#include "renderer/lighting/ReflectionProbe.h"
#include "support/ReferenceLightExtractor.h"
#include "support/ReferenceReflectionProbe.h"

#include "profiling/CpuAllocationProfile.h"
#include "scene/components/LightComponent.h"
#include "scene/components/ReflectionProbeComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "scene/systems/TransformSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace Iridium;

    // ---- comparison --------------------------------------------------------

    template<typename T>
    bool bytesEqual(std::span<const T> lhs, std::span<const T> rhs) {
        static_assert(std::is_trivially_copyable_v<T>);
        return lhs.size() == rhs.size() && (lhs.empty() ||
            std::memcmp(lhs.data(), rhs.data(), lhs.size_bytes()) == 0);
    }

    bool bitsEqual(float lhs, float rhs) {
        return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
    }

    bool bitsEqual(glm::vec3 lhs, glm::vec3 rhs) {
        return bitsEqual(lhs.x, rhs.x) && bitsEqual(lhs.y, rhs.y) &&
            bitsEqual(lhs.z, rhs.z);
    }

    bool bitsEqual(const glm::mat4& lhs, const glm::mat4& rhs) {
        return std::memcmp(&lhs, &rhs, sizeof(glm::mat4)) == 0;
    }

#define IRIDIUM_EXPECT(field, equal) \
    if (!(equal)) return #field;

    std::string compareLighting(const LightingFramePacket& expected,
        std::span<const LightExtractionDiagnostic> expectedDiagnostics,
        const LightingFramePacket& actual,
        std::span<const LightExtractionDiagnostic> actualDiagnostics) {
        IRIDIUM_EXPECT(records, bytesEqual(expected.records, actual.records))
        IRIDIUM_EXPECT(recordRevisions,
            bytesEqual(expected.recordRevisions, actual.recordRevisions))
        IRIDIUM_EXPECT(activeSlots,
            bytesEqual(expected.activeSlots, actual.activeSlots))
        IRIDIUM_EXPECT(changedRanges,
            bytesEqual(expected.changedRanges, actual.changedRanges))
        IRIDIUM_EXPECT(selectionMetadata.size, expected.selectionMetadata.size() ==
            actual.selectionMetadata.size())
        for (size_t index = 0; index < expected.selectionMetadata.size(); ++index) {
            const LightSelectionMetadata& lhs = expected.selectionMetadata[index];
            const LightSelectionMetadata& rhs = actual.selectionMetadata[index];
            IRIDIUM_EXPECT(selectionMetadata, lhs.owner == rhs.owner &&
                lhs.priority == rhs.priority &&
                lhs.castsShadows == rhs.castsShadows)
        }
        IRIDIUM_EXPECT(activeListRevision,
            expected.activeListRevision == actual.activeListRevision)
        IRIDIUM_EXPECT(requiredCapacity,
            expected.requiredCapacity == actual.requiredCapacity)
        const LightExtractionStats& lhs = expected.stats;
        const LightExtractionStats& rhs = actual.stats;
        IRIDIUM_EXPECT(stats, lhs.sceneLightCount == rhs.sceneLightCount &&
            lhs.activeLightCount == rhs.activeLightCount &&
            lhs.directionalLightCount == rhs.directionalLightCount &&
            lhs.localLightCount == rhs.localLightCount &&
            lhs.changedRecordCount == rhs.changedRecordCount &&
            lhs.changedRangeCount == rhs.changedRangeCount &&
            lhs.omittedLightCount == rhs.omittedLightCount &&
            lhs.capacity == rhs.capacity &&
            lhs.changedRecordBytes == rhs.changedRecordBytes)
        IRIDIUM_EXPECT(diagnostics.size,
            expectedDiagnostics.size() == actualDiagnostics.size())
        for (size_t index = 0; index < expectedDiagnostics.size(); ++index) {
            const LightExtractionDiagnostic& e = expectedDiagnostics[index];
            const LightExtractionDiagnostic& a = actualDiagnostics[index];
            IRIDIUM_EXPECT(diagnostics, e.code == a.code && e.owner == a.owner &&
                e.propertyPath == a.propertyPath && e.message == a.message)
        }
        return {};
    }

    bool sameProbe(const ReflectionProbeComponent& lhs,
        const ReflectionProbeComponent& rhs) {
        return lhs.enabled == rhs.enabled && lhs.shape == rhs.shape &&
            bitsEqual(lhs.sphereRadiusMeters, rhs.sphereRadiusMeters) &&
            bitsEqual(lhs.boxExtentsMeters, rhs.boxExtentsMeters) &&
            bitsEqual(lhs.blendDistanceMeters, rhs.blendDistanceMeters) &&
            bitsEqual(lhs.intensity, rhs.intensity) &&
            lhs.priority == rhs.priority && lhs.updateMode == rhs.updateMode &&
            lhs.parallaxMode == rhs.parallaxMode &&
            lhs.captureResolution == rhs.captureResolution &&
            bitsEqual(lhs.captureNearMeters, rhs.captureNearMeters) &&
            bitsEqual(lhs.captureFarMeters, rhs.captureFarMeters) &&
            lhs.captureSky == rhs.captureSky &&
            lhs.environmentAssetGuid == rhs.environmentAssetGuid &&
            lhs.resolvedEnvironmentAssetGuid == rhs.resolvedEnvironmentAssetGuid &&
            lhs.requestedEnvironmentAssetGuid ==
                rhs.requestedEnvironmentAssetGuid &&
            lhs.publicationDiagnostic == rhs.publicationDiagnostic &&
            lhs.explicitCaptureRevision == rhs.explicitCaptureRevision;
    }

    std::string compareExtraction(const ReflectionProbeFramePacket& expected,
        const ReflectionProbeFramePacket& actual) {
        IRIDIUM_EXPECT(candidates.size,
            expected.candidates.size() == actual.candidates.size())
        for (size_t index = 0; index < expected.candidates.size(); ++index) {
            const ReflectionProbeCandidate& e = expected.candidates[index];
            const ReflectionProbeCandidate& a = actual.candidates[index];
            IRIDIUM_EXPECT(candidate.owner, e.owner == a.owner)
            IRIDIUM_EXPECT(candidate.probe, sameProbe(e.probe, a.probe))
            IRIDIUM_EXPECT(candidate.transforms,
                bitsEqual(e.worldToProbe, a.worldToProbe) &&
                bitsEqual(e.probeToWorld, a.probeToWorld))
            IRIDIUM_EXPECT(candidate.residency, e.resident == a.resident &&
                e.runtimeEnvironmentSlot == a.runtimeEnvironmentSlot)
        }
        IRIDIUM_EXPECT(diagnostics.size,
            expected.diagnostics.size() == actual.diagnostics.size())
        for (size_t index = 0; index < expected.diagnostics.size(); ++index) {
            const auto& e = expected.diagnostics[index];
            const auto& a = actual.diagnostics[index];
            IRIDIUM_EXPECT(diagnostics, e.code == a.code && e.owner == a.owner &&
                e.propertyPath == a.propertyPath && e.message == a.message)
        }
        IRIDIUM_EXPECT(stats,
            expected.stats.sceneProbeCount == actual.stats.sceneProbeCount &&
            expected.stats.candidateCount == actual.stats.candidateCount &&
            expected.stats.residentCount == actual.stats.residentCount &&
            expected.stats.omittedCount == actual.stats.omittedCount)
        return {};
    }

    std::string comparePublication(const ReflectionProbeGpuFramePacket& expected,
        const ReflectionProbeGpuFramePacket& actual) {
        IRIDIUM_EXPECT(records, bytesEqual(expected.records, actual.records))
        IRIDIUM_EXPECT(recordRevisions,
            bytesEqual(expected.recordRevisions, actual.recordRevisions))
        IRIDIUM_EXPECT(activeSlots,
            bytesEqual(expected.activeSlots, actual.activeSlots))
        IRIDIUM_EXPECT(changedRanges,
            bytesEqual(expected.changedRanges, actual.changedRanges))
        IRIDIUM_EXPECT(selectionMetadata.size, expected.selectionMetadata.size() ==
            actual.selectionMetadata.size())
        for (size_t index = 0; index < expected.selectionMetadata.size(); ++index) {
            const auto& lhs = expected.selectionMetadata[index];
            const auto& rhs = actual.selectionMetadata[index];
            IRIDIUM_EXPECT(selectionMetadata, lhs.owner == rhs.owner &&
                lhs.priority == rhs.priority &&
                bitsEqual(lhs.influenceVolume, rhs.influenceVolume))
        }
        IRIDIUM_EXPECT(activeListRevision,
            expected.activeListRevision == actual.activeListRevision)
        IRIDIUM_EXPECT(requiredCapacity,
            expected.requiredCapacity == actual.requiredCapacity)
        const ReflectionProbePublicationStats& lhs = expected.stats;
        const ReflectionProbePublicationStats& rhs = actual.stats;
        IRIDIUM_EXPECT(stats,
            lhs.extractedCandidateCount == rhs.extractedCandidateCount &&
            lhs.activeProbeCount == rhs.activeProbeCount &&
            lhs.nonresidentProbeCount == rhs.nonresidentProbeCount &&
            lhs.unresolvedEnvironmentCount == rhs.unresolvedEnvironmentCount &&
            lhs.capacityOmittedCount == rhs.capacityOmittedCount &&
            lhs.changedRecordCount == rhs.changedRecordCount &&
            lhs.changedRangeCount == rhs.changedRangeCount &&
            lhs.capacity == rhs.capacity &&
            lhs.changedRecordBytes == rhs.changedRecordBytes)
        return {};
    }
#undef IRIDIUM_EXPECT

    // ---- scene construction --------------------------------------------------

    SceneEntityUuid makeUuid(uint32_t ordinal, uint8_t tag) {
        std::array<uint8_t, 10> random{};
        for (size_t byte = 0; byte < 4; ++byte)
            random[byte] = static_cast<uint8_t>(ordinal >> (byte * 8u));
        random[9] = tag;
        return SceneEntityUuid::fromUuidV7Fields(
            1'790'000'000'000ull + ordinal, random);
    }

    AssetGuid makeGuid(uint32_t ordinal) {
        std::array<uint8_t, 10> random{};
        random[0] = static_cast<uint8_t>(ordinal);
        random[9] = 0xe7;
        return AssetGuid::fromUuidV7Fields(1'790'000'500'000ull + ordinal, random);
    }

    template<typename T>
    T pick(std::mt19937_64& random, std::initializer_list<T> values) {
        std::uniform_int_distribution<size_t> index(0, values.size() - 1);
        return *(values.begin() + index(random));
    }

    bool chance(std::mt19937_64& random, double probability) {
        return std::uniform_real_distribution<double>(0.0, 1.0)(random) <
            probability;
    }

    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

    void randomizeLight(std::mt19937_64& random, LightComponent& light) {
        light.type = chance(random, 0.03) ? LightType::Area : pick(random,
            { LightType::Directional, LightType::Point, LightType::Spot,
              LightType::Spot });
        light.colorLinearRec709 = chance(random, 0.03)
            ? glm::vec3(0.0f) : pick(random, { glm::vec3(1.0f, 0.5f, 0.25f),
                glm::vec3(0.2f, 0.9f, 0.4f), glm::vec3(1.0f) });
        light.illuminanceLux = pick(random, { 100'000.0f, 50.0f, 3.0f });
        light.luminousIntensityCandela = chance(random, 0.02)
            ? kNaN : pick(random, { 1'250.0f, 2'000.0f, 80.0f });
        light.rangeMeters = pick(random, { 6.0f, 20.0f, 0.0f });
        light.sourceRadiusMeters = pick(random, { 0.05f, 0.1f });
        light.innerConeDegrees = pick(random, { 10.0f, 15.0f, 0.0f });
        light.outerConeDegrees = chance(random, 0.02)
            ? 95.0f : pick(random, { 30.0f, 45.0f, 15.0f });
        light.castsShadows = chance(random, 0.5);
        light.shadowQuality = pick(random, { LightShadowQuality::Low,
            LightShadowQuality::Medium, LightShadowQuality::High,
            LightShadowQuality::Ultra });
        light.priority = pick(random, { -1, 0, 0, 1, 2 });
    }

    void mutateLightField(std::mt19937_64& random, LightComponent& light) {
        LightComponent fresh;
        randomizeLight(random, fresh);
        switch (std::uniform_int_distribution<int>(0, 10)(random)) {
        case 0: light.type = fresh.type; break;
        case 1: light.colorLinearRec709 = fresh.colorLinearRec709; break;
        case 2: light.illuminanceLux = fresh.illuminanceLux; break;
        case 3: light.luminousIntensityCandela =
            fresh.luminousIntensityCandela; break;
        case 4: light.rangeMeters = fresh.rangeMeters; break;
        case 5: light.sourceRadiusMeters = fresh.sourceRadiusMeters; break;
        case 6: light.innerConeDegrees = fresh.innerConeDegrees; break;
        case 7: light.outerConeDegrees = fresh.outerConeDegrees; break;
        case 8: light.castsShadows = !light.castsShadows; break;
        case 9: light.shadowQuality = fresh.shadowQuality; break;
        default: light.priority = fresh.priority; break;
        }
    }

    void randomizeTransform(std::mt19937_64& random,
        TransformComponent& transform) {
        transform.position = pick(random, { glm::vec3(0.0f),
            glm::vec3(3.0f, 2.0f, -1.0f), glm::vec3(-4.0f, 1.0f, 5.0f) });
        transform.rotation = pick(random, { glm::vec3(0.0f),
            glm::vec3(30.0f, 0.0f, 0.0f), glm::vec3(0.0f, 90.0f, 0.0f),
            glm::vec3(-45.0f, 10.0f, 20.0f) });
        if (chance(random, 0.01)) transform.rotation.y = kNaN;
        transform.scale = pick(random, { glm::vec3(1.0f), glm::vec3(2.0f),
            glm::vec3(1.0f, -1.0f, 3.0f) });
        transform.isDirty = true;
    }

    struct ReplayWorld {
        SceneWorld world;
        std::vector<Entity> entities;
        uint32_t nextOrdinal = 0;
        uint8_t tag = 0;

        Entity create(std::mt19937_64& random, Entity parent = NULL_ENTITY) {
            const Entity entity = world.createEntity(
                makeUuid(nextOrdinal++, tag));
            Registry& registry = world.registry();
            randomizeTransform(random,
                registry.addComponent<TransformComponent>(entity));
            auto& relationship =
                registry.addComponent<RelationshipComponent>(entity);
            relationship.parent = parent;
            relationship.depth = parent != NULL_ENTITY &&
                registry.findPool<RelationshipComponent>()->has(parent)
                ? registry.getComponent<RelationshipComponent>(parent).depth + 1
                : 0;
            entities.push_back(entity);
            return entity;
        }

        std::optional<Entity> anyAlive(std::mt19937_64& random) const {
            if (entities.empty()) return std::nullopt;
            const Entity entity = entities[std::uniform_int_distribution<size_t>(
                0, entities.size() - 1)(random)];
            return world.registry().isAlive(entity)
                ? std::optional<Entity>(entity) : std::nullopt;
        }

        void clear() {
            world.clear();
            entities.clear();
        }

        void updateTransforms() {
            TransformSystem transforms;
            (void)transforms.update(world.registry());
        }
    };

    // ---- lights ----------------------------------------------------------------

    void randomLightStep(std::mt19937_64& random, ReplayWorld& scene) {
        Registry& registry = scene.world.registry();
        const int operation = std::uniform_int_distribution<int>(0, 99)(random);
        if (operation < 30) return; // steady frame
        if (operation < 40) {
            const size_t count = pick(random, { size_t{ 1 }, size_t{ 3 },
                size_t{ 12 } });
            for (size_t added = 0; added < count; ++added) {
                const std::optional<Entity> parent = chance(random, 0.3)
                    ? scene.anyAlive(random) : std::nullopt;
                const Entity entity = scene.create(random,
                    parent.value_or(NULL_ENTITY));
                if (!chance(random, 0.1))
                    randomizeLight(random,
                        registry.addComponent<LightComponent>(entity));
            }
            return;
        }
        const std::optional<Entity> target = scene.anyAlive(random);
        if (!target) return;
        const Entity entity = *target;
        auto* lights = registry.findPool<LightComponent>();
        auto* transforms = registry.findPool<TransformComponent>();
        if (operation < 46) (void)scene.world.destroyEntity(entity);
        else if (operation < 51) {
            if (lights && lights->has(entity)) lights->remove(entity);
            else randomizeLight(random,
                registry.addComponent<LightComponent>(entity));
        }
        else if (operation < 66) {
            if (lights && lights->has(entity))
                mutateLightField(random, lights->get(entity));
        }
        else if (operation < 84) {
            if (transforms && transforms->has(entity)) {
                TransformComponent& transform = transforms->get(entity);
                TransformComponent fresh;
                randomizeTransform(random, fresh);
                if (chance(random, 0.5)) transform.position = fresh.position;
                else if (chance(random, 0.7)) transform.rotation = fresh.rotation;
                else transform.scale = fresh.scale;
                transform.isDirty = true;
            }
        }
        else if (operation < 90) {
            // Reparent, including onto itself or a descendant (a cycle).
            auto* relationships = registry.findPool<RelationshipComponent>();
            if (relationships && relationships->has(entity)) {
                const std::optional<Entity> parent = scene.anyAlive(random);
                relationships->get(entity).parent = chance(random, 0.3)
                    ? NULL_ENTITY : parent.value_or(NULL_ENTITY);
                if (transforms && transforms->has(entity))
                    transforms->get(entity).isDirty = true;
            }
        }
        else if (operation < 94) {
            if (transforms && transforms->has(entity)) transforms->remove(entity);
            else randomizeTransform(random,
                registry.addComponent<TransformComponent>(entity));
        }
        else if (operation < 96) scene.world.identities().unbind(entity);
        else if (operation < 98) {
            // Same values written again: a write that changes nothing.
            if (lights && lights->has(entity)) {
                const LightComponent copy = lights->get(entity);
                lights->get(entity) = copy;
            }
        }
        else if (chance(random, 0.25)) scene.clear();
    }

    struct LightReplay {
        ReferenceLightExtractor reference;
        LightExtractor production;
        size_t checked = 0;

        explicit LightReplay(LightExtractionConfig config)
            : reference(config), production(config) {}

        bool step(const SceneWorld& world, std::string_view context) {
            const LightingFramePacket expected = reference.extract(world);
            const LightingFramePacket actual = production.extract(world);
            const std::string difference = compareLighting(expected,
                reference.diagnostics(), actual, production.diagnostics());
            if (!difference.empty()) {
                std::cerr << context << ": light packets differ in "
                    << difference << " after " << checked << " frames\n";
                return false;
            }
            ++checked;
            return true;
        }

        bool slotsAgree(const ReplayWorld& scene) const {
            for (Entity entity : scene.entities) {
                const auto owner = scene.world.identities().persistentId(entity);
                if (owner && reference.slotFor(*owner) !=
                        production.slotFor(*owner)) return false;
            }
            return true;
        }
    };

    bool randomizedLightSequence(uint64_t seed, LightExtractionConfig config,
        size_t steps, size_t& checked) {
        std::mt19937_64 random(seed);
        ReplayWorld primary;
        ReplayWorld secondary;
        secondary.tag = 1;
        for (int index = 0; index < 6; ++index) {
            const Entity entity = secondary.create(random);
            randomizeLight(random, secondary.world.registry()
                .addComponent<LightComponent>(entity));
        }
        LightReplay replay(config);
        for (size_t step = 0; step < steps; ++step) {
            const size_t operations = chance(random, 0.2) ? 4 : 1;
            for (size_t operation = 0; operation < operations; ++operation)
                randomLightStep(random, primary);
            primary.updateTransforms();
            // An occasional frame extracts another world (the editor's
            // asset-preview lighting world), which resets both extractors.
            ReplayWorld& scene = chance(random, 0.03) ? secondary : primary;
            if (&scene == &secondary) secondary.updateTransforms();
            if (!replay.step(scene.world, "random seed " + std::to_string(seed)))
                return false;
            if (!replay.slotsAgree(scene)) {
                std::cerr << "random seed " << seed << ": slotFor differs\n";
                return false;
            }
        }
        checked += replay.checked;
        return true;
    }

    bool randomizedLightSequencesMatchReference() {
        size_t checked = 0;
        const std::array<LightExtractionConfig, 3> configs{
            LightExtractionConfig{},
            LightExtractionConfig{ .initialCapacity = 2, .maximumCapacity = 8 },
            LightExtractionConfig{ .initialCapacity = 4, .maximumCapacity = 64 },
        };
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            if (!randomizedLightSequence(seed, configs[seed % configs.size()],
                    160, checked)) return false;
        }
        std::cout << "  light: 300 randomized sequences, " << checked
                  << " frames identical\n";
        return true;
    }

    // H-stress shape: lit scene, then add_lights batches that cross the 256,
    // 512 and 1,024 record capacities, animation, removal and a world reset.
    bool scriptedLightEventsMatchReference() {
        std::mt19937_64 random(0x5157);
        ReplayWorld scene;
        Registry& registry = scene.world.registry();
        std::vector<Entity> parents;
        for (int index = 0; index < 8; ++index)
            parents.push_back(scene.create(random));
        for (int index = 0; index < 64; ++index) {
            const Entity entity = scene.create(random,
                index % 3 == 0 ? parents[index % parents.size()] : NULL_ENTITY);
            randomizeLight(random, registry.addComponent<LightComponent>(entity));
        }
        LightReplay replay({});
        size_t frame = 0;
        const auto run = [&](size_t frames, const std::function<void()>& perFrame) {
            for (size_t index = 0; index < frames; ++index, ++frame) {
                if (perFrame) perFrame();
                scene.updateTransforms();
                if (!replay.step(scene.world, "scripted light frame " +
                        std::to_string(frame))) return false;
            }
            return true;
        };
        uint32_t scriptedOrdinal = 0;
        const auto addLights = [&](uint32_t count) {
            for (uint32_t added = 0; added < count; ++added) {
                const Entity entity = scene.world.createEntity(
                    makeUuid(100'000u + scriptedOrdinal, 0x4c));
                auto& transform = registry.addComponent<TransformComponent>(entity);
                transform.position = { 1.5f * static_cast<float>(scriptedOrdinal % 32u),
                    1.5f, 40.0f + 2.5f * static_cast<float>(scriptedOrdinal / 32u) };
                transform.isDirty = true;
                registry.addComponent<RelationshipComponent>(entity);
                auto& light = registry.addComponent<LightComponent>(entity);
                light.type = LightType::Point;
                light.colorLinearRec709 = { 1.0f, 0.82f, 0.6f };
                light.luminousIntensityCandela = 2'000.0f;
                light.rangeMeters = 6.0f;
                light.castsShadows = false;
                scene.entities.push_back(entity);
                ++scriptedOrdinal;
            }
        };
        const auto animate = [&] {
            // Positions cycle A->B->A; one parent rotates its children.
            for (size_t index = 0; index < 16 && index < scene.entities.size();
                ++index) {
                auto& transform = registry.getComponent<TransformComponent>(
                    scene.entities[8 + index]);
                transform.position.x = static_cast<float>((frame + index) % 3);
                transform.isDirty = true;
            }
            auto& parent = registry.getComponent<TransformComponent>(parents[0]);
            parent.rotation.y = static_cast<float>(frame % 4) * 30.0f;
            parent.isDirty = true;
        };
        if (!run(20, {})) return false;
        addLights(300);
        if (!run(10, {})) return false;
        addLights(300);
        if (!run(10, {})) return false;
        addLights(500);
        if (!run(10, {})) return false;
        if (!run(40, animate)) return false;
        for (int removed = 0; removed < 400; ++removed) {
            (void)scene.world.destroyEntity(scene.entities.back());
            scene.entities.pop_back();
        }
        if (!run(10, {})) return false;
        scene.clear();
        parents.clear();
        for (int index = 0; index < 8; ++index)
            parents.push_back(scene.create(random));
        addLights(64);
        if (!run(10, {})) return false;
        std::cout << "  light: scripted H-stress events, " << replay.checked
                  << " frames identical\n";
        return true;
    }

    // ---- probes --------------------------------------------------------------

    void randomizeProbe(std::mt19937_64& random,
        ReflectionProbeComponent& probe) {
        probe.enabled = !chance(random, 0.1);
        probe.shape = pick(random, { ReflectionProbeShape::Sphere,
            ReflectionProbeShape::Box });
        probe.sphereRadiusMeters = pick(random, { 2.0f, 5.0f });
        probe.boxExtentsMeters = pick(random, { glm::vec3(5.0f),
            glm::vec3(2.0f, 3.0f, 4.0f) });
        probe.blendDistanceMeters = pick(random, { 0.5f, 1.0f });
        probe.intensity = pick(random, { 1.0f, 0.5f });
        probe.priority = pick(random, { 0, 0, 1, -1 });
        probe.updateMode = pick(random, { ReflectionProbeUpdateMode::Baked,
            ReflectionProbeUpdateMode::OnDemand,
            ReflectionProbeUpdateMode::Realtime });
        probe.parallaxMode = pick(random, { ReflectionProbeParallaxMode::None,
            ReflectionProbeParallaxMode::BoxProjection });
        probe.captureResolution = chance(random, 0.04)
            ? 100 : pick(random, { 128, 512 });
        probe.captureNearMeters = 0.1f;
        probe.captureFarMeters = pick(random, { 100.0f, 50.0f });
        probe.captureSky = chance(random, 0.5);
        const uint32_t environment = pick(random, { 0u, 1u, 2u, 3u });
        probe.environmentAssetGuid = environment == 0
            ? AssetGuid{} : makeGuid(environment);
        probe.resolvedEnvironmentAssetGuid = chance(random, 0.7)
            ? probe.environmentAssetGuid : AssetGuid{};
        probe.requestedEnvironmentAssetGuid = chance(random, 0.15)
            ? makeGuid(pick(random, { 1u, 2u, 3u })) : AssetGuid{};
        // Long strings exercise heap-backed component copies.
        probe.publicationDiagnostic = pick(random, { std::string{},
            std::string("environment pending publication for this probe"),
            std::string("short") });
        probe.explicitCaptureRevision = pick(random, { uint64_t{ 0 },
            uint64_t{ 3 } });
    }

    void randomProbeStep(std::mt19937_64& random, ReplayWorld& scene) {
        Registry& registry = scene.world.registry();
        const int operation = std::uniform_int_distribution<int>(0, 99)(random);
        if (operation < 35) return;
        if (operation < 45) {
            const size_t count = pick(random, { size_t{ 1 }, size_t{ 4 } });
            for (size_t added = 0; added < count; ++added) {
                const Entity entity = scene.create(random);
                randomizeProbe(random,
                    registry.addComponent<ReflectionProbeComponent>(entity));
            }
            return;
        }
        const std::optional<Entity> target = scene.anyAlive(random);
        if (!target) return;
        const Entity entity = *target;
        auto* probes = registry.findPool<ReflectionProbeComponent>();
        auto* transforms = registry.findPool<TransformComponent>();
        if (operation < 50) (void)scene.world.destroyEntity(entity);
        else if (operation < 55) {
            if (probes && probes->has(entity)) probes->remove(entity);
            else randomizeProbe(random,
                registry.addComponent<ReflectionProbeComponent>(entity));
        }
        else if (operation < 75) {
            if (probes && probes->has(entity)) {
                ReflectionProbeComponent fresh;
                randomizeProbe(random, fresh);
                ReflectionProbeComponent& probe = probes->get(entity);
                switch (std::uniform_int_distribution<int>(0, 7)(random)) {
                case 0: probe.enabled = fresh.enabled; break;
                case 1: probe.shape = fresh.shape;
                    probe.sphereRadiusMeters = fresh.sphereRadiusMeters; break;
                case 2: probe.boxExtentsMeters = fresh.boxExtentsMeters;
                    probe.blendDistanceMeters = fresh.blendDistanceMeters; break;
                case 3: probe.intensity = fresh.intensity;
                    probe.priority = fresh.priority; break;
                case 4: probe.parallaxMode = fresh.parallaxMode;
                    probe.captureResolution = fresh.captureResolution; break;
                case 5: probe.environmentAssetGuid = fresh.environmentAssetGuid;
                    probe.resolvedEnvironmentAssetGuid =
                        fresh.resolvedEnvironmentAssetGuid; break;
                case 6: probe.requestedEnvironmentAssetGuid =
                    fresh.requestedEnvironmentAssetGuid; break;
                default: probe.publicationDiagnostic =
                    fresh.publicationDiagnostic; break;
                }
            }
        }
        else if (operation < 90) {
            if (transforms && transforms->has(entity)) {
                TransformComponent& transform = transforms->get(entity);
                TransformComponent fresh;
                randomizeTransform(random, fresh);
                transform.position = fresh.position;
                transform.rotation = fresh.rotation;
                transform.scale = chance(random, 0.05)
                    ? glm::vec3(0.0f, 1.0f, 1.0f) : fresh.scale;
                transform.isDirty = true;
            }
        }
        else if (operation < 94) {
            if (transforms && transforms->has(entity)) transforms->remove(entity);
            else randomizeTransform(random,
                registry.addComponent<TransformComponent>(entity));
        }
        else if (operation < 96) scene.world.identities().unbind(entity);
        else if (operation < 99) return;
        else if (chance(random, 0.3)) scene.clear();
    }

    // The engine's post-extraction step (RenderExtractor::prepareLightsAndProbes):
    // runtime-capture probes take their captured environment slot when the
    // backend has one.
    void applyRuntimeCaptures(ReflectionProbeFramePacket& packet,
        uint64_t frame) {
        for (ReflectionProbeCandidate& candidate : packet.candidates) {
            if (!candidate.probe.environmentAssetGuid.isNil()) continue;
            uint32_t key = static_cast<uint32_t>(frame / 40u);
            for (uint8_t byte : candidate.owner.bytes()) key = key * 31u + byte;
            candidate.runtimeEnvironmentSlot = key % 3u == 0u
                ? std::nullopt : std::optional<uint32_t>(key % 5u);
            if (candidate.runtimeEnvironmentSlot) candidate.resident = true;
        }
    }

    struct ProbeReplay {
        ReferenceReflectionProbePublisher reference;
        ReflectionProbePublisher production;
        ReflectionProbeFramePacket persistent;
        size_t checked = 0;

        explicit ProbeReplay(ReflectionProbePublicationConfig config)
            : reference(config), production(config) {}

        bool step(const SceneWorld& world, uint64_t frame,
            const std::vector<AssetGuid>& loaded,
            const std::array<std::optional<uint32_t>, 4>& slots,
            bool explicitResidency, std::string_view context) {
            const ReflectionProbeResidencyFn residency = explicitResidency
                ? ReflectionProbeResidencyFn([&loaded](AssetGuid guid) {
                    return std::ranges::find(loaded, guid) != loaded.end();
                }) : ReflectionProbeResidencyFn{};
            ReflectionProbeFramePacket expected =
                referenceExtractReflectionProbes(world, residency);
            extractReflectionProbes(world, residency, persistent);
            std::string difference = compareExtraction(expected, persistent);
            if (!difference.empty()) {
                std::cerr << context << ": probe extraction differs in "
                    << difference << " after " << checked << " frames\n";
                return false;
            }
            applyRuntimeCaptures(expected, frame);
            applyRuntimeCaptures(persistent, frame);
            const ReflectionProbeEnvironmentSlotFn environmentSlot =
                [&slots](AssetGuid guid) -> std::optional<uint32_t> {
                    for (uint32_t index = 1; index <= 3; ++index)
                        if (guid == makeGuid(index)) return slots[index];
                    return std::nullopt;
                };
            const ReflectionProbeGpuFramePacket expectedPublication =
                reference.publish(expected.candidates, environmentSlot);
            const ReflectionProbeGpuFramePacket actualPublication =
                production.publish(persistent.candidates, environmentSlot);
            difference = comparePublication(expectedPublication,
                actualPublication);
            if (!difference.empty()) {
                std::cerr << context << ": probe publication differs in "
                    << difference << " after " << checked << " frames\n";
                return false;
            }
            for (const ReflectionProbeCandidate& candidate : expected.candidates) {
                if (reference.slotFor(candidate.owner) !=
                    production.slotFor(candidate.owner)) {
                    std::cerr << context << ": probe slotFor differs\n";
                    return false;
                }
            }
            ++checked;
            return true;
        }
    };

    bool randomizedProbeSequence(uint64_t seed,
        ReflectionProbePublicationConfig config, size_t steps, size_t& checked) {
        std::mt19937_64 random(seed);
        ReplayWorld scene;
        ProbeReplay replay(config);
        std::vector<AssetGuid> loaded{ makeGuid(1), makeGuid(2) };
        std::array<std::optional<uint32_t>, 4> slots{ std::nullopt, 0u, 1u,
            kInvalidEnvironmentTableSlot };
        bool explicitResidency = true;
        for (size_t step = 0; step < steps; ++step) {
            const size_t operations = chance(random, 0.2) ? 4 : 1;
            for (size_t operation = 0; operation < operations; ++operation)
                randomProbeStep(random, scene);
            if (chance(random, 0.04)) {
                // The environment table changes (a new environment publishes,
                // the order shifts) or residency changes.
                slots[pick(random, { 1, 2, 3 })] = pick(random,
                    { std::optional<uint32_t>{}, std::optional<uint32_t>{ 0u },
                      std::optional<uint32_t>{ 2u },
                      std::optional<uint32_t>{ kInvalidEnvironmentTableSlot } });
                if (chance(random, 0.5)) {
                    if (loaded.size() == 3) loaded.pop_back();
                    else loaded.push_back(makeGuid(3));
                }
                if (chance(random, 0.2)) explicitResidency = !explicitResidency;
            }
            scene.updateTransforms();
            if (!replay.step(scene.world, step, loaded, slots, explicitResidency,
                    "probe seed " + std::to_string(seed))) return false;
        }
        checked += replay.checked;
        return true;
    }

    bool randomizedProbeSequencesMatchReference() {
        size_t checked = 0;
        const std::array<ReflectionProbePublicationConfig, 2> configs{
            ReflectionProbePublicationConfig{},
            ReflectionProbePublicationConfig{ .initialCapacity = 2,
                .maximumCapacity = 6 },
        };
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            if (!randomizedProbeSequence(seed, configs[seed % configs.size()],
                    160, checked)) return false;
        }
        std::cout << "  probe: 300 randomized sequences, " << checked
                  << " frames identical\n";
        return true;
    }

    // H-probe shape: an asset-environment probe and a realtime capture probe,
    // 40 environment probes within the 64-record capacity, 40 more past it,
    // capture-probe removal and re-addition (the same entity and component),
    // environment-table growth, removal of 80, and a capture-resolution change.
    bool scriptedProbeEventsMatchReference() {
        std::mt19937_64 random(0x9e0b);
        ReplayWorld scene;
        Registry& registry = scene.world.registry();
        const AssetGuid environment = makeGuid(1);
        const Entity environmentProbe = scene.create(random);
        {
            auto& probe = registry.addComponent<ReflectionProbeComponent>(
                environmentProbe);
            probe.environmentAssetGuid = environment;
            probe.resolvedEnvironmentAssetGuid = environment;
        }
        const Entity captureProbe = scene.create(random);
        registry.addComponent<ReflectionProbeComponent>(captureProbe)
            .updateMode = ReflectionProbeUpdateMode::Realtime;
        ProbeReplay replay({});
        std::vector<AssetGuid> loaded{ environment };
        std::array<std::optional<uint32_t>, 4> slots{ std::nullopt, 0u,
            std::nullopt, std::nullopt };
        uint64_t frame = 0;
        const auto run = [&](size_t frames) {
            for (size_t index = 0; index < frames; ++index, ++frame) {
                scene.updateTransforms();
                if (!replay.step(scene.world, frame, loaded, slots, true,
                        "scripted probe frame " + std::to_string(frame)))
                    return false;
            }
            return true;
        };
        uint32_t ordinal = 0;
        std::vector<Entity> environmentProbes;
        const auto addEnvironmentProbes = [&](uint32_t count) {
            for (uint32_t added = 0; added < count; ++added, ++ordinal) {
                const Entity entity = scene.world.createEntity(
                    makeUuid(200'000u + ordinal, 0x50));
                auto& transform = registry.addComponent<TransformComponent>(entity);
                transform.position = {
                    (static_cast<float>(ordinal % 8u) - 3.5f) * 1.5f, 1.0f,
                    (static_cast<float>((ordinal / 8u) % 8u) - 3.5f) * 1.5f };
                transform.isDirty = true;
                registry.addComponent<RelationshipComponent>(entity);
                auto& probe = registry.addComponent<ReflectionProbeComponent>(entity);
                probe.shape = ReflectionProbeShape::Sphere;
                probe.sphereRadiusMeters = 2.0f;
                probe.blendDistanceMeters = 0.5f;
                probe.parallaxMode = ReflectionProbeParallaxMode::None;
                probe.environmentAssetGuid = environment;
                probe.resolvedEnvironmentAssetGuid = environment;
                environmentProbes.push_back(entity);
            }
        };
        std::optional<ReflectionProbeComponent> removedCapture;
        const auto removeCapture = [&] {
            auto* probes = registry.findPool<ReflectionProbeComponent>();
            removedCapture = probes->get(captureProbe);
            probes->remove(captureProbe);
        };
        const auto addCapture = [&] {
            registry.addComponent<ReflectionProbeComponent>(captureProbe,
                *removedCapture);
            removedCapture.reset();
        };
        if (!run(20)) return false;
        addEnvironmentProbes(40);
        if (!run(10)) return false;
        removeCapture();
        if (!run(10)) return false;
        addCapture();
        if (!run(10)) return false;
        loaded.push_back(makeGuid(2));
        slots[1] = 1u;
        slots[2] = 0u;
        if (!run(10)) return false;
        addEnvironmentProbes(40);
        if (!run(10)) return false;
        removeCapture();
        if (!run(5)) return false;
        addCapture();
        if (!run(10)) return false;
        for (int removed = 0; removed < 80; ++removed) {
            (void)scene.world.destroyEntity(environmentProbes.back());
            environmentProbes.pop_back();
        }
        if (!run(10)) return false;
        registry.getComponent<ReflectionProbeComponent>(captureProbe)
            .captureResolution = 512;
        if (!run(10)) return false;
        // The capture probe moves every frame.
        for (int index = 0; index < 20; ++index) {
            auto& transform = registry.getComponent<TransformComponent>(
                captureProbe);
            transform.position.x = static_cast<float>(index % 3);
            transform.isDirty = true;
            if (!run(1)) return false;
        }
        std::cout << "  probe: scripted H-probe events, " << replay.checked
                  << " frames identical\n";
        return true;
    }

    // ---- steady-frame allocations ------------------------------------------

    bool steadyFramesDoNotAllocate() {
        std::mt19937_64 random(0xa110c);
        ReplayWorld scene;
        Registry& registry = scene.world.registry();
        std::vector<Entity> parents;
        for (int index = 0; index < 4; ++index)
            parents.push_back(scene.create(random));
        for (int index = 0; index < 600; ++index) {
            const Entity entity = scene.create(random,
                index % 4 == 0 ? parents[index % parents.size()] : NULL_ENTITY);
            randomizeLight(random, registry.addComponent<LightComponent>(entity));
        }
        // Lights that are rejected every frame (diagnostics repeat).
        for (int index = 0; index < 3; ++index) {
            const Entity entity = scene.create(random);
            registry.addComponent<LightComponent>(entity).type = LightType::Area;
        }
        for (int index = 0; index < 24; ++index) {
            const Entity entity = scene.create(random);
            auto& probe = registry.addComponent<ReflectionProbeComponent>(entity);
            randomizeProbe(random, probe);
            probe.publicationDiagnostic =
                "environment pending publication for this probe";
        }
        // An invalid probe repeats its diagnostic every frame.
        registry.addComponent<ReflectionProbeComponent>(scene.create(random))
            .captureResolution = 100;
        scene.updateTransforms();

        LightExtractor lights;
        ReflectionProbePublisher publisher;
        ReflectionProbeFramePacket probes;
        const std::vector<AssetGuid> loaded{ makeGuid(1), makeGuid(2) };
        const ReflectionProbeResidencyFn residency = [&loaded](AssetGuid guid) {
            return std::ranges::find(loaded, guid) != loaded.end();
        };
        const ReflectionProbeEnvironmentSlotFn environmentSlot =
            [](AssetGuid guid) -> std::optional<uint32_t> {
                if (guid == makeGuid(1)) return 0u;
                if (guid == makeGuid(2)) return 1u;
                return std::nullopt;
            };
        std::vector<DirectionalShadowSelection> selections;
        std::vector<LocalShadowRequest> requests;
        StableSpotShadowAtlas atlas;
        StablePointShadowPools pools;
        LocalShadowCacheScheduler spotCache;
        LocalShadowCacheScheduler pointCache;
        std::vector<LocalShadowCacheInput> spotInputs;
        std::vector<LocalShadowCacheInput> pointInputs;
        size_t scheduledShadows = 0;
        // The light packet's consumers as RenderExtractor runs them: shadow
        // selection, requests, reconciliation and cache scheduling.
        const auto scheduleShadows = [&](const LightingFramePacket& packet) {
            selectDirectionalShadowLights(packet,
                kDirectionalShadowLightCapacity, selections);
            buildLocalShadowRequests(packet, glm::vec3(0.0f, 1.0f, 2.0f),
                requests);
            (void)atlas.reconcile(requests);
            (void)pools.reconcile(requests);
            const auto inputFor = [&](LocalShadowKind kind, SceneEntityUuid owner,
                uint32_t lightSlot, uint32_t resolution) {
                const auto request = std::ranges::find_if(requests,
                    [&](const LocalShadowRequest& candidate) {
                        return candidate.kind == kind && candidate.owner == owner;
                    });
                return LocalShadowCacheInput{ .request = *request,
                    .resolution = resolution, .allocationRevision = 1,
                    .lightRevision = packet.recordRevisions[lightSlot],
                    .casterRevision = 1, .projectionRevision = 1 };
            };
            spotInputs.clear();
            for (const SpotShadowTile& tile : atlas.allocations())
                spotInputs.push_back(inputFor(LocalShadowKind::Spot, tile.owner,
                    tile.lightSlot, tile.size));
            pointInputs.clear();
            for (const PointShadowSlot& slot : pools.allocations())
                pointInputs.push_back(inputFor(LocalShadowKind::Point,
                    slot.owner, slot.lightSlot, slot.resolution));
            scheduledShadows = spotCache.schedule(spotInputs).entries.size() +
                pointCache.schedule(pointInputs).entries.size() +
                selections.size();
            spotCache.markScheduledRendered();
            pointCache.markScheduledRendered();
        };
        // Runtime capture slots stay fixed: a slot change is a membership
        // change, not a steady frame.
        const auto frame = [&] {
            scheduleShadows(lights.extract(scene.world));
            extractReflectionProbes(scene.world, residency, probes);
            applyRuntimeCaptures(probes, 0);
            (void)publisher.publish(probes.candidates, environmentSlot);
        };
        TransformSystem transforms;
        std::vector<Entity> changedTransforms;
        changedTransforms.reserve(1024);
        // Half the frames move a light and a parent of several lights through
        // a 12-frame cycle (A->B->A), so re-packing also runs. Moving lights
        // also changes which ones hold shadow allocations, so the first three
        // cycles warm the shadow caches' capacity; later frames are measured.
        constexpr uint64_t kWarmupFrames = 36;
        uint64_t worst = 0;
        for (uint64_t index = 0; index < kWarmupFrames + 60; ++index) {
            if (index % 2 == 0) {
                auto& moved = registry.getComponent<TransformComponent>(
                    scene.entities[10]);
                moved.position.x = static_cast<float>(index % 3);
                moved.isDirty = true;
                auto& parent = registry.getComponent<TransformComponent>(
                    parents[1]);
                parent.rotation.z = static_cast<float>(index % 4) * 15.0f;
                parent.isDirty = true;
            }
            (void)transforms.update(registry, &changedTransforms);
            if (index < kWarmupFrames) {
                frame();
                continue;
            }
            beginCpuAllocationFrame();
            frame();
            worst = (std::max)(worst, endCpuAllocationFrame().allocationCount);
        }
        if (lights.diagnostics().size() < 3 || probes.diagnostics.empty() ||
            scheduledShadows == 0) {
            std::cerr << "steady-frame fixture lost its diagnostics or shadows\n";
            return false;
        }
        // The by-value reference extraction allocates every frame, which shows
        // the counter is live in this test.
        beginCpuAllocationFrame();
        const ReflectionProbeFramePacket byValue =
            referenceExtractReflectionProbes(scene.world, residency);
        const uint64_t referenceAllocations =
            endCpuAllocationFrame().allocationCount;
        std::cout << "  steady frames: " << worst
                  << " allocations (by-value reference extraction: "
                  << referenceAllocations << ")\n";
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
        // MSVC checked iterators give every newly constructed container its own
        // heap proxy, and the probe candidate sort move-constructs temporaries
        // (one proxy each), so zero is asserted only without iterator debugging
        // (Release, where the steady-frame allocation counters are measured).
        return referenceAllocations != 0 && !byValue.candidates.empty();
#else
        return worst == 0 && referenceAllocations != 0 &&
            !byValue.candidates.empty();
#endif
    }

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const std::pair<const char*, bool (*)()> tests[] = {
        { "randomized light sequences match the full scan",
            randomizedLightSequencesMatchReference },
        { "scripted H-stress light events match the full scan",
            scriptedLightEventsMatchReference },
        { "randomized probe sequences match the full scan",
            randomizedProbeSequencesMatchReference },
        { "scripted H-probe events match the full scan",
            scriptedProbeEventsMatchReference },
        { "steady light and probe frames do not allocate",
            steadyFramesDoNotAllocate },
    };
    for (const auto& [name, test] : tests) {
        if (!test()) {
            std::cerr << "[FAIL] " << name << '\n';
            return 1;
        }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
