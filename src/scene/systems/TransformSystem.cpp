#include "scene/systems/TransformSystem.h"

#include "core/tasks/TaskSystem.h"
#include "ecs/Registry.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <algorithm>
#include <stdexcept>

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
    std::vector<Entity>* changedEntities, Iridium::Tasks::TaskSystem* tasks) {
    auto* transforms = registry.getPool<TransformComponent>();
    auto* relationships = registry.getPool<RelationshipComponent>();
    uint64_t changedTransformCount = 0;
    if (changedEntities) changedEntities->clear();

    // Dirty roots, in pool order. The dense component of entities[i] is
    // components[i] (the pool's invariant, what get() resolves).
    if (transforms->entities.size() > UINT32_MAX)
        throw std::length_error("Transform pool exceeds 32-bit indices");
    dirtyRootsScratch_.clear();
    for (size_t dense = 0; dense < transforms->entities.size(); ++dense) {
        if (relationships->has(transforms->entities[dense])) continue;
        if (transforms->components[dense].isDirty)
            dirtyRootsScratch_.push_back(static_cast<uint32_t>(dense));
    }
    const auto updateRoot = [transforms, this](size_t root) {
        TransformComponent& transform =
            transforms->components[dirtyRootsScratch_[root]];
        transform.updateLocalMatrix();
        transform.worldMatrix = transform.localMatrix;
    };
    const size_t dirtyRoots = dirtyRootsScratch_.size();
    if (tasks && dirtyRoots >= ParallelDirtyRootThreshold) {
        // M7R R5c.7: each task writes only its own roots' components.
        tasks->parallelFor(Iridium::Tasks::TaskPriority::FrameCritical,
            static_cast<uint32_t>(dirtyRoots), ParallelDirtyRootGrain,
            [&updateRoot](Iridium::Tasks::TaskRange range, uint32_t) {
                for (uint32_t root = range.begin; root < range.end; ++root)
                    updateRoot(root);
            }, "cpu.scene.transforms.roots");
    }
    else {
        for (size_t root = 0; root < dirtyRoots; ++root) updateRoot(root);
    }
    changedTransformCount += dirtyRoots;
    if (changedEntities) {
        for (const uint32_t dense : dirtyRootsScratch_)
            changedEntities->push_back(transforms->entities[dense]);
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
