#include "scene/SceneWorld.h"

#include <stdexcept>

namespace Iridium {

    SceneWorld::SceneWorld(std::unique_ptr<SceneUuidGenerator> generator)
        : generator_(generator ? std::move(generator) :
            std::make_unique<SystemSceneUuidGenerator>()),
          registry_(this) {}

    Entity SceneWorld::createEntity() {
        return registry_.createEntity();
    }

    Entity SceneWorld::createEntity(SceneEntityUuid uuid) {
        if (requestedUuid_) {
            throw std::logic_error("Nested SceneWorld entity creation is not supported");
        }
        requestedUuid_ = uuid;
        try {
            const Entity entity = registry_.createEntity();
            requestedUuid_.reset();
            return entity;
        }
        catch (...) {
            requestedUuid_.reset();
            throw;
        }
    }

    SceneEntityUuid SceneWorld::allocateEntityUuid() {
        constexpr size_t kMaximumAttempts = 16;
        for (size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            const SceneEntityUuid uuid = generator_->next();
            if (!uuid.isSupported()) {
                throw std::runtime_error(
                    "Scene UUID generator returned an unsupported identity");
            }
            if (!identities_.resolve(uuid)) return uuid;
        }
        throw std::runtime_error(
            "Scene UUID generator repeatedly returned live identities");
    }

    bool SceneWorld::destroyEntity(Entity entity) {
        return registry_.destroyEntity(entity);
    }

    void SceneWorld::clear() {
        registry_.clear();
        identities_.clear();
        references_.clear();
        if (++stateEpoch_ == 0) stateEpoch_ = 1;
    }

    void SceneWorld::swapState(SceneWorld& staging) {
        if (this == &staging) return;
        if (requestedUuid_ || staging.requestedUuid_) {
            throw std::logic_error(
                "Cannot commit a scene during entity identity allocation");
        }
        registry_.swapStorage(staging.registry_);
        identities_.swap(staging.identities_);
        references_.swap(staging.references_);
        if (++stateEpoch_ == 0) stateEpoch_ = 1;
        if (++staging.stateEpoch_ == 0) staging.stateEpoch_ = 1;
    }

    void SceneWorld::onEntityCreated(Entity entity) {
        const SceneEntityUuid uuid = requestedUuid_
            ? *requestedUuid_
            : generator_->next();
        const SceneIdentityResult result = identities_.bind(uuid, entity, registry_);
        if (!result) throw std::runtime_error(result.message);
    }

    void SceneWorld::onEntityDestroying(Entity entity) noexcept {
        identities_.unbind(entity);
    }

} // namespace Iridium
