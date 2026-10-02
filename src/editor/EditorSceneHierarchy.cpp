#include "editor/EditorSceneHierarchy.h"

#include "ecs/Registry.h"
#include "scene/components/RelationshipComponent.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace Iridium {
    namespace {

        using ChildMap = std::unordered_map<Entity, std::vector<Entity>>;

        void sortSiblings(const ComponentPool<RelationshipComponent>& relationships,
            std::vector<Entity>& siblings) {
            std::ranges::stable_sort(siblings,
                [&relationships](Entity lhs, Entity rhs) {
                    const int lhsOrder = relationships.get(lhs).siblingOrder;
                    const int rhsOrder = relationships.get(rhs).siblingOrder;
                    return lhsOrder != rhsOrder
                        ? lhsOrder < rhsOrder
                        : lhs.packed() < rhs.packed();
                });
        }

    } // namespace

    EditorHierarchyResult rebuildEditorSceneHierarchy(Registry& registry) {
        EditorHierarchyResult result;
        auto* relationships = registry.findPool<RelationshipComponent>();
        const std::vector<Entity> alive = registry.aliveEntities();
        if (alive.empty()) return result;
        if (!relationships) {
            return { .succeeded = false,
                .diagnostic = "Scene entities are missing Relationship components" };
        }

        ChildMap children;
        children.reserve(alive.size());
        result.roots.reserve(alive.size());
        for (Entity entity : alive) {
            if (!relationships->has(entity)) {
                return { .succeeded = false,
                    .diagnostic = "A scene entity is missing its required Relationship component" };
            }
            const RelationshipComponent& relationship = relationships->get(entity);
            if (relationship.siblingOrder < 0) {
                return { .succeeded = false,
                    .diagnostic = "A scene entity has a negative sibling order" };
            }
            if (relationship.parent == NULL_ENTITY) {
                result.roots.push_back(entity);
                continue;
            }
            if (relationship.parent == entity ||
                !registry.isAlive(relationship.parent) ||
                !relationships->has(relationship.parent)) {
                return { .succeeded = false,
                    .diagnostic = "A scene entity has a missing or self-referential parent" };
            }
            children[relationship.parent].push_back(entity);
        }

        sortSiblings(*relationships, result.roots);
        for (auto& [parent, siblings] : children) {
            (void)parent;
            sortSiblings(*relationships, siblings);
        }

        // Every non-root has exactly one validated parent, so an iterative walk
        // from the roots visits every entity iff the graph is acyclic. Keeping
        // this traversal separate preserves atomicity: derived children/depth
        // stay untouched when validation fails.
        std::vector<std::pair<Entity, int>> traversal;
        traversal.reserve(alive.size());
        std::vector<std::pair<Entity, int>> stack;
        stack.reserve(alive.size());
        for (auto iterator = result.roots.rbegin();
            iterator != result.roots.rend(); ++iterator) {
            stack.emplace_back(*iterator, 0);
        }
        while (!stack.empty()) {
            const auto [entity, depth] = stack.back();
            stack.pop_back();
            traversal.emplace_back(entity, depth);
            if (const auto found = children.find(entity);
                found != children.end()) {
                if (depth == std::numeric_limits<int>::max()) {
                    return { .succeeded = false,
                        .diagnostic = "Scene hierarchy exceeds the supported depth" };
                }
                for (auto iterator = found->second.rbegin();
                    iterator != found->second.rend(); ++iterator) {
                    stack.emplace_back(*iterator, depth + 1);
                }
            }
        }
        if (traversal.size() != alive.size()) {
            return { .succeeded = false,
                .diagnostic = "Scene hierarchy contains a parent cycle" };
        }

        for (Entity entity : alive) {
            RelationshipComponent& relationship = relationships->get(entity);
            relationship.children.clear();
            relationship.depth = 0;
        }
        for (const auto& [parent, siblings] : children) {
            relationships->get(parent).children = siblings;
        }
        for (const auto [entity, depth] : traversal) {
            relationships->get(entity).depth = depth;
        }
        return result;
    }

    EditorHierarchyResult collectEditorSceneSubtree(
        Registry& registry, Entity root, std::vector<Entity>& entities) {
        entities.clear();
        if (!registry.isAlive(root)) {
            return { .succeeded = false,
                .diagnostic = "The subtree root is no longer alive" };
        }
        EditorHierarchyResult result = rebuildEditorSceneHierarchy(registry);
        if (!result) return result;
        const auto* relationships = registry.findPool<RelationshipComponent>();
        std::vector<Entity> stack{ root };
        while (!stack.empty()) {
            const Entity entity = stack.back();
            stack.pop_back();
            entities.push_back(entity);
            const std::vector<Entity>& children =
                relationships->get(entity).children;
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
        return result;
    }

    bool editorSceneEntityIsDescendant(
        const Registry& registry, Entity possibleDescendant, Entity ancestor) {
        if (possibleDescendant == NULL_ENTITY || ancestor == NULL_ENTITY) return false;
        const auto* relationships = registry.findPool<RelationshipComponent>();
        if (!relationships || !relationships->has(possibleDescendant)) return false;
        Entity current = relationships->get(possibleDescendant).parent;
        size_t remaining = registry.aliveCount();
        while (current != NULL_ENTITY && remaining-- != 0) {
            if (current == ancestor) return true;
            if (!relationships->has(current)) return false;
            current = relationships->get(current).parent;
        }
        return false;
    }

} // namespace Iridium
