#include "ecs/Registry.h"
#include "editor/CoreEditorComponentRegistry.h"
#include "editor/EditorComponentDrawerRegistry.h"
#include "profiling/CpuAllocationProfile.h"
#include "scene/Components.h"
#include "scene/SceneWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#endif

#ifndef IRIDIUM_BENCHMARK_CONFIGURATION
#define IRIDIUM_BENCHMARK_CONFIGURATION "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_COMPILER
#define IRIDIUM_BENCHMARK_COMPILER "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_SOURCE_COMMIT
#define IRIDIUM_BENCHMARK_SOURCE_COMMIT "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_SOURCE_DIRTY
#define IRIDIUM_BENCHMARK_SOURCE_DIRTY 1
#endif

namespace {

    using Clock = std::chrono::steady_clock;
    using json = nlohmann::json;

    constexpr uint32_t kWarmupSamples = 1;
    constexpr uint32_t kMeasuredSamples = 30;
    constexpr uint64_t kFixedSeed = 5570198094586900512ull;

    class DeterministicSceneUuidGenerator final :
        public Iridium::SceneUuidGenerator {
    public:
        [[nodiscard]] Iridium::SceneEntityUuid next() override {
            std::array<uint8_t, 10> randomBytes{};
            uint64_t value = ++sequence_;
            for (size_t index = 0; index < 8; ++index) {
                randomBytes[index] = static_cast<uint8_t>(value >> (index * 8u));
            }
            randomBytes[8] = 0x49;
            randomBytes[9] = 0x52;
            return Iridium::SceneEntityUuid::fromUuidV7Fields(
                1'775'000'000'000ull + sequence_, randomBytes);
        }

    private:
        uint64_t sequence_ = 0;
    };

    struct ProcessMemory {
        uint64_t workingSetBytes = 0;
        uint64_t peakWorkingSetBytes = 0;
        uint64_t privateBytes = 0;
    };

    ProcessMemory processMemory() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(counters))) {
            return {
                static_cast<uint64_t>(counters.WorkingSetSize),
                static_cast<uint64_t>(counters.PeakWorkingSetSize),
                static_cast<uint64_t>(counters.PrivateUsage),
            };
        }
#endif
        return {};
    }

    json systemDescription() {
        json description{
            { "logicalProcessorCount", std::thread::hardware_concurrency() },
        };
        if (const char* processor = std::getenv("PROCESSOR_IDENTIFIER")) {
            description["processor"] = processor;
        }
#if defined(_WIN32)
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            description["totalPhysicalMemoryBytes"] =
                static_cast<uint64_t>(memory.ullTotalPhys);
        }
#endif
        return description;
    }

    double percentile(const std::vector<double>& sorted, double fraction) {
        const size_t index = static_cast<size_t>(std::ceil(
            fraction * static_cast<double>(sorted.size() - 1)));
        return sorted[index];
    }

    json statistics(std::vector<double> values) {
        std::ranges::sort(values);
        return {
            { "min", values.front() },
            { "median", percentile(values, 0.50) },
            { "p95", percentile(values, 0.95) },
            { "p99", percentile(values, 0.99) },
            { "max", values.back() },
        };
    }

    uint64_t positiveDelta(uint64_t after, uint64_t before) {
        return after > before ? after - before : 0;
    }

    template<typename Setup, typename Work>
    json measureRepeated(const std::string& name,
        size_t entityCount,
        uint64_t operationCount,
        Setup&& setup,
        Work&& work,
        json dimensions = json::object()) {
        for (uint32_t sample = 0; sample < kWarmupSamples; ++sample) {
            auto state = setup();
            (void)work(state);
        }

        std::vector<double> durationMs;
        std::vector<double> nsPerOperation;
        std::vector<double> allocationCount;
        std::vector<double> requestedAllocationBytes;
        std::vector<double> workingSetDeltaBytes;
        std::vector<double> privateDeltaBytes;
        std::vector<double> requestedBytesPerEntity;
        durationMs.reserve(kMeasuredSamples);
        nsPerOperation.reserve(kMeasuredSamples);
        allocationCount.reserve(kMeasuredSamples);
        requestedAllocationBytes.reserve(kMeasuredSamples);
        workingSetDeltaBytes.reserve(kMeasuredSamples);
        privateDeltaBytes.reserve(kMeasuredSamples);
        requestedBytesPerEntity.reserve(kMeasuredSamples);

        uint64_t checksum = 0;
        uint64_t maximumPeakWorkingSet = 0;
        for (uint32_t sample = 0; sample < kMeasuredSamples; ++sample) {
            auto state = setup();
            const ProcessMemory beforeMemory = processMemory();
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            const uint64_t currentChecksum = work(state);
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            const ProcessMemory afterMemory = processMemory();

            if (sample == 0) {
                checksum = currentChecksum;
            }
            else if (checksum != currentChecksum) {
                throw std::runtime_error(name + " produced a non-deterministic checksum");
            }

            const double milliseconds =
                std::chrono::duration<double, std::milli>(end - begin).count();
            durationMs.push_back(milliseconds);
            nsPerOperation.push_back(operationCount == 0 ? 0.0 :
                milliseconds * 1'000'000.0 /
                    static_cast<double>(operationCount));
            allocationCount.push_back(
                static_cast<double>(allocations.allocationCount));
            requestedAllocationBytes.push_back(
                static_cast<double>(allocations.requestedBytes));
            workingSetDeltaBytes.push_back(static_cast<double>(positiveDelta(
                afterMemory.workingSetBytes, beforeMemory.workingSetBytes)));
            privateDeltaBytes.push_back(static_cast<double>(positiveDelta(
                afterMemory.privateBytes, beforeMemory.privateBytes)));
            requestedBytesPerEntity.push_back(entityCount == 0 ? 0.0 :
                static_cast<double>(allocations.requestedBytes) /
                    static_cast<double>(entityCount));
            maximumPeakWorkingSet = (std::max)(maximumPeakWorkingSet,
                afterMemory.peakWorkingSetBytes);
        }

        json result{
            { "name", name },
            { "entityCount", entityCount },
            { "operationCountPerSample", operationCount },
            { "warmupSampleCount", kWarmupSamples },
            { "sampleCount", kMeasuredSamples },
            { "durationMs", statistics(std::move(durationMs)) },
            { "nsPerOperation", statistics(std::move(nsPerOperation)) },
            { "allocationCount", statistics(std::move(allocationCount)) },
            { "requestedAllocationBytes", statistics(
                std::move(requestedAllocationBytes)) },
            { "requestedBytesPerEntity", statistics(
                std::move(requestedBytesPerEntity)) },
            { "workingSetDeltaBytes", statistics(
                std::move(workingSetDeltaBytes)) },
            { "privateDeltaBytes", statistics(std::move(privateDeltaBytes)) },
            { "peakWorkingSetBytesMax", maximumPeakWorkingSet },
            { "checksum", checksum },
        };
        for (auto& [key, value] : dimensions.items()) {
            result[key] = std::move(value);
        }
        return result;
    }

    std::unique_ptr<Registry> transformRegistry(size_t count) {
        auto registry = std::make_unique<Registry>();
        for (size_t index = 0; index < count; ++index) {
            const Entity entity = registry->createEntity();
            auto& transform = registry->addComponent<TransformComponent>(entity);
            transform.position.x = static_cast<float>(index);
            transform.isDirty = false;
        }
        return registry;
    }

    std::unique_ptr<Registry> mixedRegistry(size_t count) {
        auto registry = std::make_unique<Registry>();
        for (size_t index = 0; index < count; ++index) {
            const Entity entity = registry->createEntity();
            registry->addComponent<TransformComponent>(entity);
            registry->addComponent<NameComponent>(
                entity, "Benchmark Entity " + std::to_string(index));
            registry->addComponent<RelationshipComponent>(entity).siblingOrder =
                static_cast<int>(count - index);
            if ((index & 1u) == 0u) {
                registry->addComponent<MeshComponent>(entity);
            }
        }
        return registry;
    }

    void appendCreateCases(json& cases) {
        for (const size_t count : { 10'000u, 100'000u, 1'000'000u }) {
            cases.push_back(measureRepeated("ecs_create_transform_only", count,
                count,
                [] { return std::make_unique<Registry>(); },
                [count](auto& registry) {
                    uint64_t checksum = 0;
                    for (size_t index = 0; index < count; ++index) {
                        const Entity entity = registry->createEntity();
                        registry->addComponent<TransformComponent>(entity);
                        checksum += entity.packed();
                    }
                    return checksum;
                },
                { { "occupancy", "Transform-only" } }));

            cases.push_back(measureRepeated("ecs_create_mixed", count, count,
                [] { return std::make_unique<Registry>(); },
                [count](auto& registry) {
                    uint64_t checksum = 0;
                    for (size_t index = 0; index < count; ++index) {
                        const Entity entity = registry->createEntity();
                        registry->addComponent<TransformComponent>(entity);
                        registry->addComponent<NameComponent>(
                            entity, "Benchmark Entity " + std::to_string(index));
                        registry->addComponent<RelationshipComponent>(entity);
                        if ((index & 1u) == 0u) {
                            registry->addComponent<MeshComponent>(entity);
                        }
                        checksum += entity.packed();
                    }
                    return checksum;
                },
                { { "occupancy", "Transform+Name+Relationship+50%-Mesh" } }));
        }
    }

    std::vector<Entity> randomizedEntities(size_t count, bool hits) {
        std::mt19937_64 random(kFixedSeed + (hits ? 1 : 2));
        std::uniform_int_distribution<uint32_t> distribution(
            0, static_cast<uint32_t>(count - 1));
        std::vector<Entity> entities(1'000'000);
        for (Entity& entity : entities) {
            const uint32_t offset = distribution(random);
            entity = Entity::fromLegacyIndex(hits
                ? offset
                : static_cast<uint32_t>(count + offset));
        }
        return entities;
    }

    void appendLookupCases(json& cases) {
        constexpr size_t count = 100'000;
        const std::vector<Entity> hits = randomizedEntities(count, true);
        const std::vector<Entity> misses = randomizedEntities(count, false);

        cases.push_back(measureRepeated("ecs_random_lookup_hits", count,
            hits.size(), [=] { return transformRegistry(count); },
            [&hits](auto& registry) {
                auto* transforms = registry->getPool<TransformComponent>();
                uint64_t checksum = 0;
                for (Entity entity : hits) {
                    checksum += static_cast<uint64_t>(
                        transforms->get(entity).position.x);
                }
                return checksum;
            }, { { "lookupResult", "hit" } }));

        cases.push_back(measureRepeated("ecs_random_lookup_misses", count,
            misses.size(), [=] { return transformRegistry(count); },
            [&misses](auto& registry) {
                auto* transforms = registry->getPool<TransformComponent>();
                uint64_t checksum = 0;
                for (Entity entity : misses) {
                    checksum += transforms->has(entity) ? 1ull : 0ull;
                }
                return checksum;
            }, { { "lookupResult", "miss" } }));
    }

    void appendTransformCases(json& cases) {
        constexpr size_t count = 100'000;
        constexpr uint64_t iterationCount = 10'000'000;
        cases.push_back(measureRepeated("ecs_dense_transform_iteration", count,
            iterationCount, [=] { return transformRegistry(count); },
            [](auto& registry) {
                auto* transforms = registry->getPool<TransformComponent>();
                uint64_t checksum = 0;
                for (size_t repeat = 0; repeat < 100; ++repeat) {
                    for (const TransformComponent& transform :
                            transforms->components) {
                        checksum += static_cast<uint64_t>(transform.position.x);
                    }
                }
                return checksum;
            }));

        for (const uint32_t dirtyPercent : { 0u, 1u, 10u, 100u }) {
            cases.push_back(measureRepeated("ecs_changed_transform_update", count,
                count,
                [=] {
                    auto registry = transformRegistry(count);
                    auto* transforms = registry->getPool<TransformComponent>();
                    for (size_t index = 0; index < count; ++index) {
                        transforms->components[index].isDirty =
                            (index % 100) < dirtyPercent;
                    }
                    return registry;
                },
                [](auto& registry) {
                    auto* transforms = registry->getPool<TransformComponent>();
                    uint64_t checksum = 0;
                    for (TransformComponent& transform : transforms->components) {
                        if (transform.isDirty) {
                            transform.position.x += 1.0f;
                            transform.isDirty = false;
                            ++checksum;
                        }
                    }
                    return checksum;
                }, { { "dirtyPercent", dirtyPercent } }));
        }
    }

    void appendViewCases(json& cases) {
        constexpr size_t count = 100'000;
        cases.push_back(measureRepeated("ecs_view_transform_mesh", count, count,
            [=] { return mixedRegistry(count); },
            [](auto& registry) {
                auto* transforms = registry->getPool<TransformComponent>();
                auto* meshes = registry->getPool<MeshComponent>();
                uint64_t checksum = 0;
                for (Entity entity : transforms->entities) {
                    if (meshes->has(entity)) {
                        checksum += meshes->get(entity).enabled
                            ? entity.packed() : 0;
                    }
                }
                return checksum;
            }, { { "view", "Transform+Mesh" } }));

        cases.push_back(measureRepeated("ecs_view_transform_relationship", count,
            count, [=] { return mixedRegistry(count); },
            [](auto& registry) {
                auto* transforms = registry->getPool<TransformComponent>();
                auto* relationships =
                    registry->getPool<RelationshipComponent>();
                uint64_t checksum = 0;
                for (Entity entity : transforms->entities) {
                    if (relationships->has(entity)) {
                        checksum += static_cast<uint64_t>(
                            relationships->get(entity).siblingOrder);
                    }
                }
                return checksum;
            }, { { "view", "Transform+Relationship" } }));
    }

    void appendDeletionCases(json& cases) {
        constexpr size_t count = 100'000;
        for (const uint32_t percent : { 1u, 10u, 50u }) {
            const size_t affected = count * percent / 100;
            cases.push_back(measureRepeated("ecs_batch_delete_recreate", count,
                affected * 2, [=] { return transformRegistry(count); },
                [=](auto& registry) {
                    uint64_t checksum = 0;
                    for (size_t index = 0; index < affected; ++index) {
                        (void)registry->destroyEntity(
                            Entity::fromLegacyIndex(static_cast<uint32_t>(index)));
                    }
                    for (size_t index = 0; index < affected; ++index) {
                        const Entity entity = registry->createEntity();
                        registry->addComponent<TransformComponent>(entity);
                        checksum += entity.packed();
                    }
                    return checksum;
                }, { { "affectedPercent", percent } }));
        }
    }

    struct StaleHandleState {
        std::unique_ptr<Registry> registry;
        std::vector<Entity> staleHandles;
    };

    void appendStaleHandleCases(json& cases) {
        constexpr size_t count = 100'000;
        constexpr size_t repeatCount = 10;
        cases.push_back(measureRepeated("ecs_stale_handle_probe", count,
            count * repeatCount,
            [] {
                auto state = std::make_unique<StaleHandleState>();
                state->registry = std::make_unique<Registry>();
                state->staleHandles.reserve(count);
                for (size_t index = 0; index < count; ++index) {
                    const Entity entity = state->registry->createEntity();
                    state->registry->addComponent<TransformComponent>(entity);
                    state->staleHandles.push_back(entity);
                }
                for (Entity entity : state->staleHandles) {
                    if (!state->registry->destroyEntity(entity)) {
                        throw std::runtime_error("stale handle setup destroy failed");
                    }
                }
                for (size_t index = 0; index < count; ++index) {
                    const Entity replacement = state->registry->createEntity();
                    state->registry->addComponent<TransformComponent>(replacement);
                }
                return state;
            },
            [](auto& state) {
                uint64_t checksum = 0;
                for (size_t repeat = 0; repeat < repeatCount; ++repeat) {
                    for (Entity entity : state->staleHandles) {
                        checksum += state->registry->isAlive(entity) ? 1ull : 0ull;
                    }
                }
                return checksum;
            }, { { "probeResult", "stale-rejected-after-index-reuse" } }));
    }

    std::unique_ptr<Iridium::SceneWorld> identityWorld(size_t count) {
        auto world = std::make_unique<Iridium::SceneWorld>(
            std::make_unique<DeterministicSceneUuidGenerator>());
        for (size_t index = 0; index < count; ++index) {
            (void)world->createEntity();
        }
        return world;
    }

    struct IdentityLookupState {
        std::unique_ptr<Iridium::SceneWorld> world;
        std::vector<Entity> entities;
        std::vector<Iridium::SceneEntityUuid> ids;
    };

    std::unique_ptr<IdentityLookupState> identityLookupState(size_t count) {
        auto state = std::make_unique<IdentityLookupState>();
        state->world = identityWorld(count);
        state->entities = state->world->registry().aliveEntities();
        state->ids.reserve(state->entities.size());
        for (Entity entity : state->entities) {
            state->ids.push_back(
                *state->world->identities().persistentId(entity));
        }
        return state;
    }

    void appendIdentityCases(json& cases) {
        for (const size_t count : { 10'000u, 100'000u, 1'000'000u }) {
            cases.push_back(measureRepeated("scene_identity_create_bind", count,
                count,
                [] {
                    return std::make_unique<Iridium::SceneWorld>(
                        std::make_unique<DeterministicSceneUuidGenerator>());
                },
                [count](auto& world) {
                    uint64_t checksum = 0;
                    for (size_t index = 0; index < count; ++index) {
                        checksum += world->createEntity().packed();
                    }
                    if (world->identities().size() != count) {
                        throw std::runtime_error("scene identity bind count mismatch");
                    }
                    return checksum;
                }));
        }

        constexpr size_t count = 100'000;
        constexpr size_t repeatCount = 10;
        cases.push_back(measureRepeated("scene_identity_uuid_resolve", count,
            count * repeatCount, [=] { return identityLookupState(count); },
            [](auto& state) {
                uint64_t checksum = 0;
                for (size_t repeat = 0; repeat < repeatCount; ++repeat) {
                    for (Iridium::SceneEntityUuid uuid : state->ids) {
                        checksum += state->world->identities().resolve(uuid)
                            ->packed();
                    }
                }
                return checksum;
            }));

        cases.push_back(measureRepeated("scene_identity_handle_reverse_lookup",
            count, count * repeatCount,
            [=] { return identityLookupState(count); },
            [](auto& state) {
                uint64_t checksum = 0;
                for (size_t repeat = 0; repeat < repeatCount; ++repeat) {
                    for (Entity entity : state->entities) {
                        checksum += state->world->identities().persistentId(entity)
                            ->bytes()[15];
                    }
                }
                return checksum;
            }));

        cases.push_back(measureRepeated("scene_identity_destroy_recreate", count,
            count * 2, [=] { return identityWorld(count); },
            [](auto& world) {
                std::vector<Entity> oldEntities =
                    world->registry().aliveEntities();
                std::vector<Iridium::SceneEntityUuid> oldIds;
                oldIds.reserve(oldEntities.size());
                for (Entity entity : oldEntities) {
                    oldIds.push_back(*world->identities().persistentId(entity));
                }
                for (Entity entity : oldEntities) {
                    if (!world->destroyEntity(entity)) {
                        throw std::runtime_error("identity destroy failed");
                    }
                }
                uint64_t checksum = 0;
                for (size_t index = 0; index < count; ++index) {
                    checksum += world->createEntity().generation();
                }
                for (Iridium::SceneEntityUuid uuid : oldIds) {
                    if (world->identities().resolve(uuid)) {
                        throw std::runtime_error("destroyed UUID remained bound");
                    }
                }
                return checksum;
            }, { { "oldUuidResult", "unbound" } }));
    }

    std::unique_ptr<Registry> hierarchyRegistry(size_t count, bool breadth) {
        auto registry = std::make_unique<Registry>();
        auto* relationships = registry->getPool<RelationshipComponent>();
        Entity previous = NULL_ENTITY;
        for (size_t index = 0; index < count; ++index) {
            const Entity entity = registry->createEntity();
            auto& relationship =
                registry->addComponent<RelationshipComponent>(entity);
            relationship.siblingOrder = static_cast<int>(count - index);
            if (index > 0) {
                const Entity parent = breadth
                    ? Entity::fromLegacyIndex(0)
                    : previous;
                relationship.parent = parent;
                relationships->get(parent).children.push_back(entity);
            }
            previous = entity;
        }
        return registry;
    }

    void appendHierarchyCases(json& cases) {
        for (const size_t count : { 10'000u, 100'000u }) {
            for (const bool breadth : { false, true }) {
                cases.push_back(measureRepeated(
                    breadth ? "ecs_hierarchy_breadth_traversal" :
                        "ecs_hierarchy_depth_traversal",
                    count, count,
                    [=] { return hierarchyRegistry(count, breadth); },
                    [breadth](auto& registry) {
                        auto* relationships =
                            registry->getPool<RelationshipComponent>();
                        uint64_t checksum = 1;
                        if (breadth) {
                            for (Entity child : relationships->get(
                                    Entity::fromLegacyIndex(0)).children) {
                                checksum += relationships->has(child) ? 1ull : 0ull;
                            }
                        }
                        else {
                            Entity entity = Entity::fromLegacyIndex(0);
                            while (!relationships->get(entity).children.empty()) {
                                entity = relationships->get(entity).children.front();
                                ++checksum;
                            }
                        }
                        return checksum;
                    }, { { "shape", breadth ? "breadth" : "depth" } }));
            }
        }
    }

    void appendEditorCases(json& cases) {
        auto editorComponents = Iridium::createCoreEditorComponentRegistry();
        if (!editorComponents) {
            throw std::runtime_error(editorComponents.status.message);
        }
        const auto noop = [](Iridium::EditorComponentDrawContext&) {};
        Iridium::CoreEditorComponentDrawerCallbacks callbacks;
        callbacks.transform = noop;
        callbacks.relationship = noop;
        callbacks.mesh = noop;
        callbacks.light = noop;
        callbacks.sky = noop;
        callbacks.reflectionProbe = noop;
        callbacks.bakedLightingSet = noop;
        auto editorDrawers =
            Iridium::createCoreEditorComponentDrawerRegistry(
                editorComponents.registry, std::move(callbacks));
        if (!editorDrawers) {
            throw std::runtime_error(editorDrawers.status.message);
        }

        cases.push_back(measureRepeated(
            "editor_inspector_registry_construction", 0, 1,
            [] { return 0; },
            [noop](auto&) {
                auto components =
                    Iridium::createCoreEditorComponentRegistry();
                if (!components) {
                    throw std::runtime_error(components.status.message);
                }
                Iridium::CoreEditorComponentDrawerCallbacks drawers;
                drawers.transform = noop;
                drawers.relationship = noop;
                drawers.mesh = noop;
                drawers.light = noop;
                drawers.sky = noop;
                drawers.reflectionProbe = noop;
                drawers.bakedLightingSet = noop;
                auto registered =
                    Iridium::createCoreEditorComponentDrawerRegistry(
                        components.registry, std::move(drawers));
                if (!registered) {
                    throw std::runtime_error(registered.status.message);
                }
                return static_cast<uint64_t>(
                    components.registry.descriptors().size() +
                    registered.registry.descriptors().size());
            }));

        for (const size_t count : { 10'000u, 100'000u }) {
            const std::vector<Entity> selections = randomizedEntities(count, true);
            cases.push_back(measureRepeated("editor_hierarchy_snapshot_sort", count,
                count, [=] { return mixedRegistry(count); },
                [](auto& registry) {
                    auto* relationships =
                        registry->getPool<RelationshipComponent>();
                    std::vector<Entity> ordered = relationships->entities;
                    std::ranges::stable_sort(ordered,
                        [relationships](Entity lhs, Entity rhs) {
                            return relationships->get(lhs).siblingOrder <
                                relationships->get(rhs).siblingOrder;
                        });
                    return ordered.empty() ? 0ull : ordered.front().packed();
                }));

            cases.push_back(measureRepeated("editor_selection_lookup", count,
                selections.size(), [=] { return mixedRegistry(count); },
                [&selections](auto& registry) {
                    auto* names = registry->getPool<NameComponent>();
                    uint64_t checksum = 0;
                    for (Entity entity : selections) {
                        checksum += names->get(entity).name.size();
                    }
                    return checksum;
                }));

            cases.push_back(measureRepeated(
                "editor_inspector_descriptor_construction", count,
                count * 4, [=] { return mixedRegistry(count); },
                [&componentRegistry = editorComponents.registry,
                 &drawerRegistry = editorDrawers.registry](auto& registry) {
                    auto* transforms = registry->getPool<TransformComponent>();
                    uint64_t checksum = 0;
                    for (Entity entity : transforms->entities) {
                        for (const auto& descriptor :
                                componentRegistry.descriptors()) {
                            if (!descriptor.visible ||
                                !descriptor.has(*registry, entity)) {
                                continue;
                            }
                            checksum += descriptor.getMutable(
                                *registry, entity) != nullptr;
                            checksum += drawerRegistry.find(descriptor.id) !=
                                nullptr;
                        }
                    }
                    return checksum;
                }));
        }
    }

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path outputPath;
        bool quiet = false;
        bool editorOnly = false;
        bool identityOnly = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--output" && index + 1 < argc) {
                outputPath = argv[++index];
            }
            else if (argument == "--quiet") {
                quiet = true;
            }
            else if (argument == "--editor-only") {
                editorOnly = true;
            }
            else if (argument == "--identity-only") {
                identityOnly = true;
            }
            else {
                std::cerr << "Usage: IridiumEcsSceneBenchmark [--output path] "
                    "[--quiet] [--editor-only | --identity-only]\n";
                return 2;
            }
        }
        if (editorOnly && identityOnly) {
            std::cerr << "--editor-only and --identity-only are mutually exclusive\n";
            return 2;
        }

        json result{
            { "schema", "iridium.m4.ecs-scene-baseline" },
            { "version", 4 },
            { "configuration", IRIDIUM_BENCHMARK_CONFIGURATION },
            { "compiler", IRIDIUM_BENCHMARK_COMPILER },
            { "sourceCommit", IRIDIUM_BENCHMARK_SOURCE_COMMIT },
            { "sourceDirtyAtConfigure", IRIDIUM_BENCHMARK_SOURCE_DIRTY != 0 },
            { "fixedSeed", kFixedSeed },
            { "registryContract", "generational-64-bit-handle+dense-vector+paged-32-bit-sparse-index+generation-validation-m4.8" },
            { "sceneIdentityContract", "uuidv7+bidirectional-map-m4.1" },
            { "sourceSceneContract", "schema1 production measured by IridiumSourceSceneBenchmark; v0 retained only as pure migration fixtures" },
            { "system", systemDescription() },
            { "clock", "steady_clock" },
            { "warmupSamplesPerCase", kWarmupSamples },
            { "measuredSamplesPerCase", kMeasuredSamples },
            { "allocationScope", "C++ global new/new[] requested bytes" },
            { "unsupportedCases", json::array({
                {
                    { "name", "cooked_scene_save_load" },
                    { "reason", "measured by the dedicated IridiumCookedSceneBenchmark" },
                    { "appendInSlice", "M4.4 accepted" },
                },
            }) },
            { "cases", json::array() },
        };

        if (identityOnly) {
            appendIdentityCases(result["cases"]);
        }
        else if (!editorOnly) {
            appendCreateCases(result["cases"]);
            appendLookupCases(result["cases"]);
            appendTransformCases(result["cases"]);
            appendViewCases(result["cases"]);
            appendDeletionCases(result["cases"]);
            appendStaleHandleCases(result["cases"]);
            appendIdentityCases(result["cases"]);
            appendHierarchyCases(result["cases"]);
        }
        if (!identityOnly) {
            appendEditorCases(result["cases"]);
        }
        const std::string serialized = result.dump(2) + '\n';
        if (!outputPath.empty()) {
            if (outputPath.has_parent_path()) {
                std::filesystem::create_directories(outputPath.parent_path());
            }
            std::ofstream output(outputPath);
            output << serialized;
            if (!output.good()) {
                throw std::runtime_error("could not write benchmark output");
            }
        }
        if (!quiet) {
            std::cout << serialized;
        }
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << "IridiumEcsSceneBenchmark failed: "
            << exception.what() << '\n';
        return 1;
    }
}
