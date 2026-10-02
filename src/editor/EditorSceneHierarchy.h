#pragma once

#include "ecs/Entity.h"

#include <cstddef>
#include <string>
#include <vector>

class Registry;

namespace Iridium {

    struct EditorHierarchyResult {
        bool succeeded = true;
        std::string diagnostic;
        std::vector<Entity> roots;

        [[nodiscard]] explicit operator bool() const noexcept {
            return succeeded;
        }
    };

    // Relationship.parent and siblingOrder are authoritative. children and
    // depth are transient derived data and are rebuilt atomically here.
    [[nodiscard]] EditorHierarchyResult rebuildEditorSceneHierarchy(
        Registry& registry);

    // Rebuilds first, then returns parent-before-child preorder for the subtree.
    [[nodiscard]] EditorHierarchyResult collectEditorSceneSubtree(
        Registry& registry, Entity root, std::vector<Entity>& entities);

    [[nodiscard]] bool editorSceneEntityIsDescendant(
        const Registry& registry, Entity possibleDescendant, Entity ancestor);

} // namespace Iridium
