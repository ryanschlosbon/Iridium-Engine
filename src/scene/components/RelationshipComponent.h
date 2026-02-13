#pragma once
#include <vector>
#include "../Entity.h"

struct RelationshipComponent {
    Entity parent = NULL_ENTITY;
    std::vector<Entity> children;
    int depth = 0;
};