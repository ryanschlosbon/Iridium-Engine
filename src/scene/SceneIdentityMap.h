#pragma once

#include "ecs/Entity.h"
#include "scene/SceneEntityUuid.h"

#include <optional>
#include <string>
#include <unordered_map>

class Registry;

namespace Iridium {

    enum class SceneIdentityError {
        None,
        InvalidUuid,
        DeadHandle,
        DuplicateUuid,
        DuplicateHandle,
        InconsistentReverseMap,
        MissingIdentity,
    };

    struct SceneIdentityResult {
        SceneIdentityError error = SceneIdentityError::None;
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == SceneIdentityError::None;
        }
    };

    class SceneIdentityMap {
    public:
        [[nodiscard]] SceneIdentityResult bind(
            SceneEntityUuid uuid, Entity entity, const Registry& registry);
        [[nodiscard]] std::optional<Entity> resolve(SceneEntityUuid uuid) const;
        [[nodiscard]] std::optional<SceneEntityUuid> persistentId(
            Entity entity) const;
        [[nodiscard]] bool containsAlive(
            SceneEntityUuid uuid, const Registry& registry) const;
        void unbind(Entity entity) noexcept;
        void clear() noexcept;
        void swap(SceneIdentityMap& other) noexcept;
        [[nodiscard]] SceneIdentityResult validate(const Registry& registry) const;
        [[nodiscard]] size_t size() const noexcept { return byUuid_.size(); }

    private:
        std::unordered_map<SceneEntityUuid, Entity, SceneEntityUuidHash> byUuid_;
        std::unordered_map<Entity, SceneEntityUuid> byEntity_;
    };

} // namespace Iridium
