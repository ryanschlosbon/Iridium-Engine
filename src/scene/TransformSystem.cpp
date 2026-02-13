#include "TransformSystem.h"
#include "scene/Registry.h" // Include your specific Registry header here
#include <algorithm>

TransformSystem::TransformSystem() {}
TransformSystem::~TransformSystem() {}

void TransformSystem::sortEntitiesByDepth(Registry& registry, std::vector<uint32_t>& outEntities) {
    // 1. Get the pool of entities that have relationships
    // Note: Adjust this syntax to match your specific ECS implementation
    auto* relationshipPool = registry.getPool<RelationshipComponent>();

    // Copy the list so we can sort it
    outEntities = relationshipPool->entities;

    // 2. Sort by Depth (0 -> 1 -> 2 -> ...)
    // This ensures we always calculate the Tank Body (0) before the Turret (1)
    std::sort(outEntities.begin(), outEntities.end(), [&](uint32_t a, uint32_t b) {
        // Direct access is faster than map lookups if possible
        return relationshipPool->get(a).depth < relationshipPool->get(b).depth;
        });
}

void TransformSystem::update(Registry& registry) {
    auto* transformPool = registry.getPool<TransformComponent>();
    auto* relationPool = registry.getPool<RelationshipComponent>();

    // Process "Orphans" (Entities with Transform but NO Relationship)
    // We iterate the raw transform pool to catch entities that aren't in the hierarchy.
    for (uint32_t entity : transformPool->entities) {
        // If it HAS a relationship, skip it (the sorted loop below will handle it correctly)
        if (relationPool->has(entity)) continue;

        auto& transform = transformPool->get(entity);
        if (transform.isDirty) {
            transform.updateLocalMatrix();
            transform.worldMatrix = transform.localMatrix; // No parent, so World = Local
        }
    }

    // Process Hierarchy (Parents & Children)
    std::vector<uint32_t> sortedEntities;
    sortEntitiesByDepth(registry, sortedEntities);

    for (uint32_t entity : sortedEntities) {
        // Skip entities that don't have transforms (safety check)
        if (!transformPool->has(entity)) continue;

        auto& transform = transformPool->get(entity);
        auto& relation = relationPool->get(entity);

        // Did the Parent change?
        // If my parent is dirty, I MUST be dirty too (because I am attached to them)
        if (relation.parent != NULL_ENTITY && transformPool->has(relation.parent)) {
            auto& parentTransform = transformPool->get(relation.parent);
            if (parentTransform.isDirty) {
                transform.isDirty = true;
            }
        }

        // Do we need to recalculate?
        if (transform.isDirty) {
            // Update my Local Matrix (Position/Rotation/Scale)
            transform.updateLocalMatrix();

            // Update my World Matrix
            if (relation.parent == NULL_ENTITY) {
                // Root Object: World is just Local
                transform.worldMatrix = transform.localMatrix;
            }
            else {
                // Child Object: Parent World * My Local
                auto& parentTransform = transformPool->get(relation.parent);
                transform.worldMatrix = parentTransform.worldMatrix * transform.localMatrix;
            }
        }
    }

    // Cleanup (Reset Dirty Flags for EVERYONE)
    for (uint32_t entity : transformPool->entities) {
        transformPool->get(entity).isDirty = false;
    }
}