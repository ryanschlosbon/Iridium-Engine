#pragma once

#include "ecs/Registry.h"
#include "scene/SceneIdentityMap.h"
#include "scene/runtime/SceneReferenceState.h"

#include <memory>
#include <optional>
#include <cstdint>

namespace Iridium {

    class SceneWorld final : private RegistryEntityObserver {
    public:
        explicit SceneWorld(
            std::unique_ptr<SceneUuidGenerator> generator = {});

        [[nodiscard]] Registry& registry() noexcept { return registry_; }
        [[nodiscard]] const Registry& registry() const noexcept { return registry_; }
        [[nodiscard]] SceneIdentityMap& identities() noexcept { return identities_; }
        [[nodiscard]] const SceneIdentityMap& identities() const noexcept {
            return identities_;
        }
        [[nodiscard]] SceneReferenceState& references() noexcept {
            return references_;
        }
        [[nodiscard]] const SceneReferenceState& references() const noexcept {
            return references_;
        }
        // Changes only when the complete world state is cleared or swapped. It is
        // deliberately unaffected by ordinary entity/component edits so renderer
        // extraction can distinguish a world replacement from incremental change.
        [[nodiscard]] uint64_t stateEpoch() const noexcept { return stateEpoch_; }

        [[nodiscard]] Entity createEntity();
        [[nodiscard]] Entity createEntity(SceneEntityUuid uuid);
        // Editor structural commands allocate an identity before the transient
        // ECS handle exists so create/undo/redo can address the entity by its
        // persistent identity throughout the transaction.
        [[nodiscard]] SceneEntityUuid allocateEntityUuid();
        [[nodiscard]] bool destroyEntity(Entity entity);
        void clear();
        // Commits a fully validated staging world without changing the address of
        // either world's Registry. The displaced active state moves into staging
        // and retires normally when that world is destroyed.
        void swapState(SceneWorld& staging);

    private:
        void onEntityCreated(Entity entity) override;
        void onEntityDestroying(Entity entity) noexcept override;

        std::unique_ptr<SceneUuidGenerator> generator_;
        SceneIdentityMap identities_;
        SceneReferenceState references_;
        Registry registry_;
        std::optional<SceneEntityUuid> requestedUuid_;
        uint64_t stateEpoch_ = 1;
    };

} // namespace Iridium
