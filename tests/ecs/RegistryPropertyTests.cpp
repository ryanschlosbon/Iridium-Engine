#include "ecs/Registry.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

    struct ValueComponent {
        int value = 0;
    };

    struct MarkerComponent {
        uint32_t tag = 0;
    };

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "check failed: " #condition \
                    << " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    bool pagedSparseAllocationIsDemandDriven() {
        PagedSparseIndex index;
        CHECK(index.get(0) == PagedSparseIndex::Empty);
        index.set(0, 7);
        index.set(1'000'000, 9);
        CHECK(index.get(0) == 7);
        CHECK(index.get(1'000'000) == 9);
        CHECK(index.get(999'999) == PagedSparseIndex::Empty);
        CHECK(index.allocatedPageCount() == 2);
        index.erase(0);
        CHECK(index.allocatedPageCount() == 1);
        index.erase(1'000'000);
        CHECK(index.allocatedPageCount() == 0);
        index.clear();
        CHECK(index.get(1'000'000) == PagedSparseIndex::Empty);

        Registry registry;
        Entity last = NULL_ENTITY;
        for (size_t count = 0; count < 100'000; ++count) {
            last = registry.createEntity();
        }
        auto* values = registry.getPool<ValueComponent>();
        registry.addComponent<ValueComponent>(last, 42);
        CHECK(values->sparseIndex.allocatedPageCount() == 1);
        CHECK(values->get(last).value == 42);
        return true;
    }

    bool randomizedRegistryOperationsMatchReference() {
        constexpr uint32_t operationCount = 100'000;
        std::mt19937_64 random(0x4d382d7061676564ull);
        Registry registry;
        auto* values = registry.getPool<ValueComponent>();
        auto* markers = registry.getPool<MarkerComponent>();
        std::vector<Entity> live;
        std::vector<Entity> historical;
        std::unordered_map<uint64_t, int> referenceValues;
        std::unordered_set<uint64_t> referenceMarkers;

        const auto create = [&] {
            const Entity entity = registry.createEntity();
            live.push_back(entity);
            historical.push_back(entity);
        };
        for (size_t index = 0; index < 128; ++index) create();

        for (uint32_t operation = 0; operation < operationCount; ++operation) {
            const uint32_t choice = static_cast<uint32_t>(random() % 8);
            if (choice == 0 || live.empty()) {
                create();
            }
            else {
                const size_t liveIndex = static_cast<size_t>(random() % live.size());
                const Entity entity = live[liveIndex];
                if (choice <= 2) {
                    const int value = static_cast<int>(random());
                    registry.addComponent<ValueComponent>(entity, value);
                    referenceValues[entity.packed()] = value;
                }
                else if (choice == 3) {
                    values->remove(entity);
                    referenceValues.erase(entity.packed());
                }
                else if (choice == 4) {
                    registry.addComponent<MarkerComponent>(entity,
                        static_cast<uint32_t>(random()));
                    referenceMarkers.insert(entity.packed());
                }
                else if (choice == 5) {
                    markers->remove(entity);
                    referenceMarkers.erase(entity.packed());
                }
                else {
                    CHECK(registry.destroyEntity(entity));
                    referenceValues.erase(entity.packed());
                    referenceMarkers.erase(entity.packed());
                    live[liveIndex] = live.back();
                    live.pop_back();
                }
            }

            for (uint32_t probe = 0; probe < 4; ++probe) {
                const Entity entity = historical[static_cast<size_t>(
                    random() % historical.size())];
                const bool alive = registry.isAlive(entity);
                const auto expectedValue = referenceValues.find(entity.packed());
                CHECK(values->has(entity) ==
                    (alive && expectedValue != referenceValues.end()));
                CHECK((values->getVoid(entity) != nullptr) == values->has(entity));
                if (values->has(entity)) {
                    CHECK(values->get(entity).value == expectedValue->second);
                    CHECK(static_cast<const ComponentPool<ValueComponent>&>(
                        *values).get(entity).value == expectedValue->second);
                }
                CHECK(markers->has(entity) ==
                    (alive && referenceMarkers.contains(entity.packed())));
            }

            if ((operation % 997) == 0) {
                CHECK(values->entities.size() == referenceValues.size());
                CHECK(values->components.size() == referenceValues.size());
                for (size_t dense = 0; dense < values->entities.size(); ++dense) {
                    const Entity entity = values->entities[dense];
                    CHECK(registry.isAlive(entity));
                    CHECK(referenceValues.at(entity.packed()) ==
                        values->components[dense].value);
                    CHECK(values->sparseIndex.get(entity.index()) == dense);
                }

                size_t viewCount = 0;
                for (Entity entity : values->entities) {
                    viewCount += markers->has(entity);
                }
                size_t expectedViewCount = 0;
                for (const auto& [packed, value] : referenceValues) {
                    (void)value;
                    expectedViewCount += referenceMarkers.contains(packed);
                }
                CHECK(viewCount == expectedViewCount);
            }
        }

        if (!historical.empty()) {
            const Entity stale = historical.front();
            if (!registry.isAlive(stale)) {
                bool rejected = false;
                try {
                    registry.addComponent<ValueComponent>(stale, 1);
                }
                catch (const std::out_of_range&) {
                    rejected = true;
                }
                CHECK(rejected);
            }
        }
        return true;
    }

} // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        { "demand-driven paged sparse allocation",
            pagedSparseAllocationIsDemandDriven },
        { "randomized registry operations",
            randomizedRegistryOperationsMatchReference },
    };
    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            if (test.run()) {
                ++passed;
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                std::cout << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            std::cout << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
        }
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
