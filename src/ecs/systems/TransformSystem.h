#pragma once
#include "scene/Components.h"
#include <vector>

// Forward declaration
class Registry;

class TransformSystem {
public:
    TransformSystem();
    ~TransformSystem();

    // The main function you call every frame before rendering
    void update(Registry& registry);

private:
    // Helper to sort entities so parents are processed before children
    void sortEntitiesByDepth(Registry& registry, std::vector<uint32_t>& outEntities);
};