#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <vector>

class Registry;

class TransformSystem {
public:
    TransformSystem();
    ~TransformSystem();

    // When supplied, changedEntities receives the exact post-hierarchy world-
    // transform journal before dirty bits are cleared. Caller-owned storage keeps
    // the steady path allocation-free.
    [[nodiscard]] uint64_t update(Registry& registry,
        std::vector<Entity>* changedEntities = nullptr);

private:
    void sortEntitiesByDepth(
        Registry& registry, std::vector<Entity>& outEntities);
    std::vector<Entity> sortedEntitiesScratch_;
};
