#pragma once

#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class Registry;

namespace Iridium::Tasks {
    class TaskSystem;
}

class TransformSystem {
public:
    // M7R R5c.7: from this many dirty root transforms (entities without a
    // RelationshipComponent) on, their local and world matrices are computed
    // as frame-critical parallel work when a task system is given, in ranges
    // of ParallelDirtyRootGrain. Below it the serial loop is as fast or faster
    // (TransformSystemTests --timing, 31 threads: serial about 53 ns per dirty
    // root; the parallel pass costs 35-50 us of wake-up and join, so it wins
    // from about 1,024 roots and halves the update at 2,048).
    static constexpr size_t ParallelDirtyRootThreshold = 2048;
    static constexpr uint32_t ParallelDirtyRootGrain = 256;

    TransformSystem();
    ~TransformSystem();

    // When supplied, changedEntities receives the exact post-hierarchy world-
    // transform journal before dirty bits are cleared. Caller-owned storage keeps
    // the steady path allocation-free. Each dirty root's matrices depend only on
    // its own component, so the parallel path writes the same values; the
    // journal and the hierarchy pass stay in pool order on the calling thread.
    [[nodiscard]] uint64_t update(Registry& registry,
        std::vector<Entity>* changedEntities = nullptr,
        Iridium::Tasks::TaskSystem* tasks = nullptr);

private:
    void sortEntitiesByDepth(
        Registry& registry, std::vector<Entity>& outEntities);
    std::vector<Entity> sortedEntitiesScratch_;
    // Dense indices of this frame's dirty roots, in pool order.
    std::vector<uint32_t> dirtyRootsScratch_;
};
