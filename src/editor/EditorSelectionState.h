#pragma once

#include "ecs/Registry.h"

#include <algorithm>
#include <span>
#include <vector>

namespace Iridium {

    struct EditorSelectionState {
        Entity primary = NULL_ENTITY;
        std::vector<Entity> entities;

        [[nodiscard]] bool contains(Entity entity) const noexcept {
            return std::ranges::find(entities, entity) != entities.end();
        }

        void selectExclusive(Entity entity) {
            primary = entity;
            entities.clear();
            if (entity != NULL_ENTITY) entities.push_back(entity);
        }

        void toggle(Entity entity) {
            const auto found = std::ranges::find(entities, entity);
            if (found == entities.end()) {
                entities.push_back(entity);
                primary = entity;
                return;
            }
            entities.erase(found);
            if (primary == entity) {
                primary = entities.empty() ? NULL_ENTITY : entities.back();
            }
        }

        void reconcile(const Registry& registry) {
            std::erase_if(entities, [&](Entity entity) {
                return !registry.isAlive(entity);
            });
            if (primary != NULL_ENTITY && !registry.isAlive(primary)) {
                primary = NULL_ENTITY;
            }
            if (primary == NULL_ENTITY) {
                entities.clear();
                return;
            }
            if (!contains(primary)) {
                entities.assign(1, primary);
            }
        }

        [[nodiscard]] std::span<const Entity> selected() const noexcept {
            return entities;
        }
    };

} // namespace Iridium
