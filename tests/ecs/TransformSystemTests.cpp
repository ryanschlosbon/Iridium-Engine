#include "ecs/Registry.h"
#include "ecs/systems/TransformSystem.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <exception>
#include <iostream>

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

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[] = {
        { "Orphan changed count", testOrphanChangedCount },
        { "Hierarchy propagation count", testHierarchyPropagationCount },
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
