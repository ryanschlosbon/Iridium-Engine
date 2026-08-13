#include "scene/SceneIdentityMap.h"

#include "ecs/Registry.h"

namespace Iridium {

    SceneIdentityResult SceneIdentityMap::bind(
        SceneEntityUuid uuid, Entity entity, const Registry& registry) {
        if (!uuid.isSupported()) {
            return { SceneIdentityError::InvalidUuid,
                "Scene entity UUID must be non-nil RFC UUIDv5 or UUIDv7" };
        }
        if (!registry.isAlive(entity)) {
            return { SceneIdentityError::DeadHandle,
                "Cannot bind scene identity to a null, dead, or stale handle" };
        }
        if (byUuid_.contains(uuid)) {
            return { SceneIdentityError::DuplicateUuid,
                "Scene entity UUID is already bound" };
        }
        if (byEntity_.contains(entity)) {
            return { SceneIdentityError::DuplicateHandle,
                "Runtime entity handle already has a scene identity" };
        }
        byUuid_.emplace(uuid, entity);
        byEntity_.emplace(entity, uuid);
        return {};
    }

    std::optional<Entity> SceneIdentityMap::resolve(SceneEntityUuid uuid) const {
        const auto found = byUuid_.find(uuid);
        return found == byUuid_.end()
            ? std::nullopt
            : std::optional<Entity>(found->second);
    }

    std::optional<SceneEntityUuid> SceneIdentityMap::persistentId(
        Entity entity) const {
        const auto found = byEntity_.find(entity);
        return found == byEntity_.end()
            ? std::nullopt
            : std::optional<SceneEntityUuid>(found->second);
    }

    bool SceneIdentityMap::containsAlive(
        SceneEntityUuid uuid, const Registry& registry) const {
        const std::optional<Entity> entity = resolve(uuid);
        return entity && registry.isAlive(*entity);
    }

    void SceneIdentityMap::unbind(Entity entity) noexcept {
        const auto reverse = byEntity_.find(entity);
        if (reverse == byEntity_.end()) return;
        byUuid_.erase(reverse->second);
        byEntity_.erase(reverse);
    }

    void SceneIdentityMap::clear() noexcept {
        byUuid_.clear();
        byEntity_.clear();
    }

    void SceneIdentityMap::swap(SceneIdentityMap& other) noexcept {
        byUuid_.swap(other.byUuid_);
        byEntity_.swap(other.byEntity_);
    }

    SceneIdentityResult SceneIdentityMap::validate(const Registry& registry) const {
        if (byUuid_.size() != byEntity_.size()) {
            return { SceneIdentityError::InconsistentReverseMap,
                "Scene identity forward/reverse map sizes differ" };
        }
        for (const auto& [uuid, entity] : byUuid_) {
            if (!registry.isAlive(entity)) {
                return { SceneIdentityError::DeadHandle,
                    "Scene identity references a dead or stale handle" };
            }
            const auto reverse = byEntity_.find(entity);
            if (reverse == byEntity_.end() || reverse->second != uuid) {
                return { SceneIdentityError::InconsistentReverseMap,
                    "Scene identity reverse entry is missing or inconsistent" };
            }
        }
        for (Entity entity : registry.aliveEntities()) {
            if (!byEntity_.contains(entity)) {
                return { SceneIdentityError::MissingIdentity,
                    "Live scene entity has no persistent UUID" };
            }
        }
        return {};
    }

} // namespace Iridium
