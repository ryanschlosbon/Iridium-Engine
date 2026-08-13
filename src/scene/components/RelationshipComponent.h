#pragma once
#include <vector>
#include "ecs/Entity.h"

struct RelationshipComponent {
    Entity parent = NULL_ENTITY;
    std::vector<Entity> children;
    int depth = 0;
    // Stable editor-facing order among entities with the same parent.
    // This is serialized independently of transient ECS dense-array order.
    int siblingOrder = 0;

};
