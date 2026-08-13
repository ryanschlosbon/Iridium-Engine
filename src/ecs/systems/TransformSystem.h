#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <vector>

class Registry;

class TransformSystem {
public:
    TransformSystem();
    ~TransformSystem();

    [[nodiscard]] uint64_t update(Registry& registry);

private:
    void sortEntitiesByDepth(
        Registry& registry, std::vector<Entity>& outEntities);
};
