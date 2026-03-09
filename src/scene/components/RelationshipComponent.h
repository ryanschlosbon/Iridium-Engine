#pragma once
#include <vector>
#include "ecs/Entity.h"
#include "editor/Reflection.h"

struct RelationshipComponent {
    Entity parent = NULL_ENTITY;
    std::vector<Entity> children;
    int depth = 0;

    REFLECT_BEGIN()
        // Simple serialization of depth. Complex vectors require a custom array overload in the archive later!
        PROPERTY(depth)
    REFLECT_END()
};

AUTO_REGISTER_COMPONENT(RelationshipComponent)