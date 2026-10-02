#include "TransformSystem.h"

#include "ecs/Registry.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <algorithm>

TransformSystem::TransformSystem() = default;
TransformSystem::~TransformSystem() = default;

void TransformSystem::sortEntitiesByDepth(
    Registry& registry, std::vector<Entity>& outEntities) {
    auto* relationships = registry.getPool<RelationshipComponent>();
    outEntities = relationships->entities;
    std::ranges::sort(outEntities,
        [relationships](Entity lhs, Entity rhs) {
            return relationships->get(lhs).depth <
                relationships->get(rhs).depth;
        });
}

uint64_t TransformSystem::update(Registry& registry,
    std::vector<Entity>* changedEntities) {
    auto* transforms = registry.getPool<TransformComponent>();
    auto* relationships = registry.getPool<RelationshipComponent>();
    uint64_t changedTransformCount = 0;
    if (changedEntities) changedEntities->clear();

    for (Entity entity : transforms->entities) {
        if (relationships->has(entity)) continue;
        auto& transform = transforms->get(entity);
        if (transform.isDirty) {
            transform.updateLocalMatrix();
            transform.worldMatrix = transform.localMatrix;
            ++changedTransformCount;
            if (changedEntities) changedEntities->push_back(entity);
        }
    }

    sortEntitiesByDepth(registry, sortedEntitiesScratch_);
    for (Entity entity : sortedEntitiesScratch_) {
        if (!transforms->has(entity)) continue;
        auto& transform = transforms->get(entity);
        auto& relationship = relationships->get(entity);
        const bool hasLiveParent = relationship.parent != NULL_ENTITY &&
            registry.isAlive(relationship.parent) &&
            transforms->has(relationship.parent);
        if (hasLiveParent &&
            transforms->get(relationship.parent).isDirty) {
            transform.isDirty = true;
        }
        if (!transform.isDirty) continue;

        transform.updateLocalMatrix();
        transform.worldMatrix = hasLiveParent
            ? transforms->get(relationship.parent).worldMatrix *
                transform.localMatrix
            : transform.localMatrix;
        ++changedTransformCount;
        if (changedEntities) changedEntities->push_back(entity);
    }

    for (Entity entity : transforms->entities) {
        transforms->get(entity).isDirty = false;
    }
    return changedTransformCount;
}
