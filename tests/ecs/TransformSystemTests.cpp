#include "ecs/Registry.h"
#include "scene/systems/TransformSystem.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include "core/tasks/TaskSystem.h"

#include <exception>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

namespace {

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    bool testOrphanChangedCount() {
        Registry registry;
        TransformSystem system;
        const Entity entity = registry.createEntity();
        registry.addComponent<TransformComponent>(entity);

        CHECK(system.update(registry) == 1);
        CHECK(system.update(registry) == 0);

        registry.getComponent<TransformComponent>(entity).setPosition({ 2.0f, 0.0f, 0.0f });
        CHECK(system.update(registry) == 1);
        CHECK(system.update(registry) == 0);
        return true;
    }

    bool testHierarchyPropagationCount() {
        Registry registry;
        TransformSystem system;
        const Entity parent = registry.createEntity();
        const Entity child = registry.createEntity();

        registry.addComponent<TransformComponent>(parent);
        registry.addComponent<TransformComponent>(child);
        auto& parentRelationship = registry.addComponent<RelationshipComponent>(parent);
        parentRelationship.children.push_back(child);
        parentRelationship.depth = 0;
        auto& childRelationship = registry.addComponent<RelationshipComponent>(child);
        childRelationship.parent = parent;
        childRelationship.depth = 1;

        CHECK(system.update(registry) == 2);
        CHECK(system.update(registry) == 0);

        registry.getComponent<TransformComponent>(parent).setPosition({ 1.0f, 0.0f, 0.0f });
        CHECK(system.update(registry) == 2);
        CHECK(system.update(registry) == 0);
        return true;
    }

    bool testChangedEntityJournalIncludesPropagatedChildren() {
        Registry registry;
        TransformSystem system;
        const Entity parent = registry.createEntity();
        const Entity child = registry.createEntity();
        registry.addComponent<TransformComponent>(parent);
        registry.addComponent<TransformComponent>(child);
        auto& parentRelationship = registry.addComponent<RelationshipComponent>(parent);
        parentRelationship.children.push_back(child);
        auto& childRelationship = registry.addComponent<RelationshipComponent>(child);
        childRelationship.parent = parent;
        childRelationship.depth = 1;
        std::vector<Entity> changed;
        CHECK(system.update(registry, &changed) == 2);
        CHECK(changed.size() == 2);
        CHECK(std::ranges::find(changed, parent) != changed.end());
        CHECK(std::ranges::find(changed, child) != changed.end());
        CHECK(system.update(registry, &changed) == 0);
        CHECK(changed.empty());
        registry.getComponent<TransformComponent>(parent).setPosition(
            { 3.0f, 0.0f, 0.0f });
        CHECK(system.update(registry, &changed) == 2);
        CHECK(changed.size() == 2);
        return true;
    }

    // M7R R5c.7: a scene of `roots` root transforms and `children`
    // parent/child pairs.
    struct Scene {
        Registry registry;
        std::vector<Entity> roots;
    };

    void buildScene(Scene& scene, size_t roots, size_t children) {
        std::mt19937 random(3);
        std::uniform_real_distribution<float> value(-50.0f, 50.0f);
        for (size_t index = 0; index < roots; ++index) {
            const Entity entity = scene.registry.createEntity();
            auto& transform = scene.registry.addComponent<TransformComponent>(entity);
            transform.position = { value(random), value(random), value(random) };
            transform.rotation = { value(random), value(random), value(random) };
            transform.scale = { 1.0f + value(random) * 0.01f, 1.0f, 1.0f };
            scene.roots.push_back(entity);
        }
        for (size_t index = 0; index < children; ++index) {
            const Entity parent = scene.registry.createEntity();
            const Entity child = scene.registry.createEntity();
            scene.registry.addComponent<TransformComponent>(parent).position =
                { value(random), 0.0f, 0.0f };
            scene.registry.addComponent<TransformComponent>(child).rotation =
                { 0.0f, value(random), 0.0f };
            auto& parentRelationship =
                scene.registry.addComponent<RelationshipComponent>(parent);
            parentRelationship.children.push_back(child);
            auto& childRelationship =
                scene.registry.addComponent<RelationshipComponent>(child);
            childRelationship.parent = parent;
            childRelationship.depth = 1;
        }
    }

    // Marks every `every`-th root dirty with a new position.
    void dirtyRoots(Scene& scene, size_t every, uint32_t frame) {
        for (size_t index = 0; index < scene.roots.size(); index += every) {
            auto& transform =
                scene.registry.getComponent<TransformComponent>(scene.roots[index]);
            transform.setPosition(transform.position +
                glm::vec3(0.001f * static_cast<float>(frame), 0.0f, 0.0f));
        }
    }

    bool sameTransforms(Registry& lhs, Registry& rhs) {
        auto* a = lhs.getPool<TransformComponent>();
        auto* b = rhs.getPool<TransformComponent>();
        if (a->entities != b->entities) return false;
        for (size_t index = 0; index < a->components.size(); ++index) {
            if (std::memcmp(&a->components[index].worldMatrix,
                    &b->components[index].worldMatrix, sizeof(glm::mat4)) != 0 ||
                std::memcmp(&a->components[index].localMatrix,
                    &b->components[index].localMatrix, sizeof(glm::mat4)) != 0)
                return false;
        }
        return true;
    }

    // The parallel dirty-root path gives the serial matrices, count and
    // journal (in pool order), below and above the threshold.
    bool testParallelDirtyRootsMatchSerial() {
        Iridium::Tasks::TaskSystem tasks(Iridium::Tasks::TaskSystemConfig{
            .workerThreadCount = 7, .reservedFrameWorkers = 2,
            .pinnedIoThread = false });
        const size_t rootCounts[] = { 0, 1, 100,
            TransformSystem::ParallelDirtyRootThreshold - 1,
            TransformSystem::ParallelDirtyRootThreshold,
            3 * TransformSystem::ParallelDirtyRootThreshold + 17 };
        bool passed = true;
        for (const size_t roots : rootCounts) {
            Scene serial;
            Scene parallel;
            buildScene(serial, roots, 64);
            buildScene(parallel, roots, 64);
            TransformSystem serialSystem;
            TransformSystem parallelSystem;
            std::vector<Entity> serialJournal;
            std::vector<Entity> parallelJournal;
            for (uint32_t frame = 0; frame < 6 && passed; ++frame) {
                if (frame != 0) {
                    dirtyRoots(serial, frame % 3 + 1, frame);
                    dirtyRoots(parallel, frame % 3 + 1, frame);
                }
                const uint64_t serialCount = serialSystem.update(
                    serial.registry, &serialJournal);
                const uint64_t parallelCount = parallelSystem.update(
                    parallel.registry, &parallelJournal, &tasks);
                passed = serialCount == parallelCount &&
                    serialJournal == parallelJournal &&
                    sameTransforms(serial.registry, parallel.registry);
                if (!passed)
                    std::cerr << "  mismatch: " << roots << " roots, frame " << frame << '\n';
            }
        }
        tasks.shutdown();
        return passed;
    }

    // Indicative only: the serial update of N dirty roots against the
    // parallel root-matrix pass alone, median of 51, default task system.
    void printTiming() {
        Iridium::Tasks::TaskSystem tasks;
        for (const size_t roots : { 256u, 1024u, 2048u, 4096u, 8192u, 16384u,
                65536u }) {
            Scene scene;
            buildScene(scene, roots, 0);
            TransformSystem system;
            std::vector<Entity> journal;
            std::vector<double> serialSamples;
            std::vector<double> parallelSamples;
            auto* pool = scene.registry.getPool<TransformComponent>();
            for (uint32_t frame = 0; frame < 51; ++frame) {
                dirtyRoots(scene, 1, frame);
                auto start = std::chrono::steady_clock::now();
                (void)system.update(scene.registry, &journal, nullptr);
                serialSamples.push_back(std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - start).count());
                dirtyRoots(scene, 1, frame);
                start = std::chrono::steady_clock::now();
                tasks.parallelFor(Iridium::Tasks::TaskPriority::FrameCritical,
                    static_cast<uint32_t>(pool->components.size()),
                    TransformSystem::ParallelDirtyRootGrain,
                    [pool](Iridium::Tasks::TaskRange range, uint32_t) {
                        for (uint32_t index = range.begin; index < range.end; ++index) {
                            auto& transform = pool->components[index];
                            transform.updateLocalMatrix();
                            transform.worldMatrix = transform.localMatrix;
                        }
                    });
                parallelSamples.push_back(std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - start).count());
                (void)system.update(scene.registry, &journal, nullptr);
            }
            std::ranges::sort(serialSamples);
            std::ranges::sort(parallelSamples);
            std::cout << roots << " dirty roots: serial update "
                << serialSamples[25] << " us, parallel root pass "
                << parallelSamples[25] << " us\n";
        }
        tasks.shutdown();
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--timing") {
        printTiming();
        return 0;
    }
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[] = {
        { "Orphan changed count", testOrphanChangedCount },
        { "Hierarchy propagation count", testHierarchyPropagationCount },
        { "Changed entity journal includes propagated children",
            testChangedEntityJournalIncludesPropagatedChildren },
        { "Parallel dirty roots match the serial update",
            testParallelDirtyRootsMatchSerial },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
