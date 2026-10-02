#include "ecs/Registry.h"
#include "ecs/systems/TransformSystem.h"
#include "scene/SceneEntityUuid.h"
#include "scene/SceneIdentityMap.h"
#include "scene/SceneWorld.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    struct ValueComponent {
        int value = 0;
        void OnInspector() {}
    };

    Iridium::SceneEntityUuid uuid(uint8_t seed, uint64_t timestamp = 1000) {
        std::array<uint8_t, 10> random{};
        random.fill(seed);
        return Iridium::SceneEntityUuid::fromUuidV7Fields(timestamp, random);
    }

    class SequenceGenerator final : public Iridium::SceneUuidGenerator {
    public:
        explicit SequenceGenerator(std::vector<Iridium::SceneEntityUuid> values)
            : values_(std::move(values)) {}

        Iridium::SceneEntityUuid next() override {
            if (next_ == values_.size()) {
                throw std::runtime_error("deterministic UUID sequence exhausted");
            }
            return values_[next_++];
        }

    private:
        std::vector<Iridium::SceneEntityUuid> values_;
        size_t next_ = 0;
    };

    bool handlePackingAndNullAreExplicit() {
        const Entity entity = Entity::fromParts(42, 9);
        CHECK(entity.index() == 42);
        CHECK(entity.generation() == 9);
        CHECK(entity.packed() == (9ull << 32u | 42ull));
        CHECK(!entity.isNull());
        CHECK(NULL_ENTITY.isNull());
        CHECK(Entity::fromLegacyIndex(7) == Entity::fromParts(7, 1));
        return true;
    }

    bool destroyedHandleCannotAliasReusedIndex() {
        Registry registry;
        const Entity first = registry.createEntity();
        registry.addComponent<ValueComponent>(first, 17);
        CHECK(registry.destroyEntity(first));
        CHECK(!registry.isAlive(first));
        CHECK(!registry.destroyEntity(first));

        const Entity replacement = registry.createEntity();
        CHECK(replacement.index() == first.index());
        CHECK(replacement.generation() == first.generation() + 1);
        registry.addComponent<ValueComponent>(replacement, 29);
        CHECK(!registry.getPool<ValueComponent>()->has(first));
        CHECK(registry.getPool<ValueComponent>()->get(replacement).value == 29);
        bool rejected = false;
        try {
            (void)registry.getComponent<ValueComponent>(first);
        }
        catch (const std::out_of_range&) {
            rejected = true;
        }
        CHECK(rejected);
        return true;
    }

    bool clearInvalidatesEveryLiveHandle() {
        Registry registry;
        const Entity first = registry.createEntity();
        const Entity second = registry.createEntity();
        registry.addComponent<ValueComponent>(first, 1);
        registry.addComponent<ValueComponent>(second, 2);
        registry.clear();
        CHECK(registry.aliveCount() == 0);
        CHECK(!registry.isAlive(first));
        CHECK(!registry.isAlive(second));
        CHECK(registry.getPool<ValueComponent>()->entities.empty());

        const Entity recreated = registry.createEntity();
        CHECK(recreated.index() == 0);
        CHECK(recreated.generation() == first.generation() + 1);
        return true;
    }

    bool generationExhaustionIsHardAndNonDestructive() {
        Registry registry(nullptr, 2);
        const Entity first = registry.createEntity();
        CHECK(registry.destroyEntity(first));
        const Entity lastGeneration = registry.createEntity();
        CHECK(lastGeneration.generation() == 2);
        bool rejected = false;
        try {
            (void)registry.destroyEntity(lastGeneration);
        }
        catch (const std::overflow_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(registry.isAlive(lastGeneration));
        CHECK(registry.aliveCount() == 1);
        return true;
    }

    bool nonMutatingPoolLookupDoesNotCreateStorage() {
        Registry registry;
        CHECK(registry.getPools().empty());
        CHECK(registry.findPool<ValueComponent>() == nullptr);
        CHECK(registry.getPools().empty());
        (void)registry.getPool<ValueComponent>();
        CHECK(registry.getPools().size() == 1);
        return true;
    }

    struct ThrowingObserver final : RegistryEntityObserver {
        bool reject = true;
        void onEntityCreated(Entity) override {
            if (reject) {
                reject = false;
                throw std::runtime_error("injected identity bind failure");
            }
        }
        void onEntityDestroying(Entity) noexcept override {}
    };

    bool creationFailureRollsBackEntitySlot() {
        ThrowingObserver observer;
        Registry registry(&observer);
        bool rejected = false;
        try {
            (void)registry.createEntity();
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(registry.aliveCount() == 0);
        const Entity entity = registry.createEntity();
        CHECK(entity == Entity::fromParts(0, 1));
        return true;
    }

    bool uuidParsingAndV5DerivationAreStrict() {
        const auto parsed = Iridium::SceneEntityUuid::parse(
            "01890f4c-0000-7000-8000-000000000001");
        CHECK(parsed.has_value());
        CHECK(parsed->version() == 7);
        CHECK(parsed->toString() ==
            "01890f4c-0000-7000-8000-000000000001");
        CHECK(!Iridium::SceneEntityUuid::parse(
            "00000000-0000-0000-0000-000000000000"));
        CHECK(!Iridium::SceneEntityUuid::parse(
            "01890f4c-0000-4000-8000-000000000001"));
        CHECK(!Iridium::SceneEntityUuid::parse(
            "01890f4c-0000-7000-c000-000000000001"));

        const std::array<uint8_t, 16> dnsNamespace{
            0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
            0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8,
        };
        const auto derived = Iridium::deriveSceneEntityUuidV5(
            dnsNamespace, "www.widgets.com");
        CHECK(derived.toString() ==
            "21f7f8de-8051-5b89-8680-0195ef798b6a");
        CHECK(derived.version() == 5);
        CHECK(derived.hasRfc4122Variant());
        return true;
    }

    bool sceneWorldMaintainsOneToOneIdentity() {
        const auto firstUuid = uuid(1);
        const auto secondUuid = uuid(2);
        auto generator = std::make_unique<SequenceGenerator>(
            std::vector{ firstUuid, secondUuid });
        Iridium::SceneWorld world(std::move(generator));
        const Entity first = world.createEntity();
        const Entity second = world.createEntity();
        CHECK(world.identities().resolve(firstUuid) == first);
        CHECK(world.identities().persistentId(second) == secondUuid);
        CHECK(world.identities().containsAlive(firstUuid, world.registry()));
        CHECK(world.identities().size() == 2);
        CHECK(world.identities().validate(world.registry()));

        CHECK(world.destroyEntity(first));
        CHECK(!world.identities().resolve(firstUuid));
        const Entity replacement = world.createEntity(firstUuid);
        CHECK(replacement.index() == first.index());
        CHECK(replacement.generation() == first.generation() + 1);
        CHECK(world.identities().resolve(firstUuid) == replacement);
        CHECK(world.identities().validate(world.registry()));
        return true;
    }

    bool duplicateUuidCreationRollsBack() {
        Iridium::SceneWorld world;
        const auto id = uuid(7);
        const Entity first = world.createEntity(id);
        bool rejected = false;
        try {
            (void)world.createEntity(id);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(world.registry().aliveCount() == 1);
        CHECK(world.identities().size() == 1);
        CHECK(world.identities().resolve(id) == first);
        return true;
    }

    bool stagingCommitPreservesRegistryAddressAndObservers() {
        const auto activeId = uuid(21);
        const auto stagedId = uuid(22);
        const auto postCommitId = uuid(23);
        Iridium::SceneWorld active;
        Registry* activeRegistryAddress = &active.registry();
        const Entity oldEntity = active.createEntity(activeId);
        active.registry().addComponent<ValueComponent>(oldEntity, 21);

        auto generator = std::make_unique<SequenceGenerator>(
            std::vector{ postCommitId });
        Iridium::SceneWorld staging(std::move(generator));
        Registry* stagingRegistryAddress = &staging.registry();
        const Entity stagedEntity = staging.createEntity(stagedId);
        staging.registry().addComponent<ValueComponent>(stagedEntity, 22);

        active.swapState(staging);
        CHECK(&active.registry() == activeRegistryAddress);
        CHECK(&staging.registry() == stagingRegistryAddress);
        CHECK(active.identities().resolve(stagedId) == stagedEntity);
        CHECK(!active.identities().resolve(activeId));
        CHECK(active.registry().getComponent<ValueComponent>(stagedEntity).value == 22);
        CHECK(staging.identities().resolve(activeId) == oldEntity);
        CHECK(staging.registry().getComponent<ValueComponent>(oldEntity).value == 21);
        CHECK(active.identities().validate(active.registry()));
        CHECK(staging.identities().validate(staging.registry()));

        const Entity createdAfterCommit = staging.createEntity();
        CHECK(staging.identities().resolve(postCommitId) == createdAfterCommit);
        CHECK(staging.destroyEntity(createdAfterCommit));
        CHECK(!staging.identities().resolve(postCommitId));
        CHECK(staging.identities().validate(staging.registry()));
        return true;
    }

    bool identityValidationFindsMissingAndDeadBindings() {
        Registry registry;
        Iridium::SceneIdentityMap identities;
        const Entity entity = registry.createEntity();
        CHECK(identities.validate(registry).error ==
            Iridium::SceneIdentityError::MissingIdentity);
        const auto id = uuid(9);
        CHECK(identities.bind(id, entity, registry));
        CHECK(identities.validate(registry));
        CHECK(registry.destroyEntity(entity));
        CHECK(identities.validate(registry).error ==
            Iridium::SceneIdentityError::DeadHandle);
        identities.unbind(entity);
        CHECK(identities.validate(registry));
        CHECK(!identities.bind({}, NULL_ENTITY, registry));
        return true;
    }

    bool recycledParentHandleDoesNotTargetReplacement() {
        Registry registry;
        TransformSystem transforms;
        const Entity parent = registry.createEntity();
        auto& parentTransform =
            registry.addComponent<TransformComponent>(parent);
        parentTransform.position.x = 10.0f;
        registry.addComponent<RelationshipComponent>(parent);
        const Entity child = registry.createEntity();
        auto& childTransform =
            registry.addComponent<TransformComponent>(child);
        childTransform.position.x = 1.0f;
        auto& childRelationship =
            registry.addComponent<RelationshipComponent>(child);
        childRelationship.parent = parent;
        childRelationship.depth = 1;
        registry.getPool<RelationshipComponent>()->get(parent)
            .children.push_back(child);
        (void)transforms.update(registry);

        CHECK(registry.destroyEntity(parent));
        const Entity replacement = registry.createEntity();
        CHECK(replacement.index() == parent.index());
        auto& replacementTransform =
            registry.addComponent<TransformComponent>(replacement);
        replacementTransform.position.x = 100.0f;
        auto& liveChildTransform =
            registry.getPool<TransformComponent>()->get(child);
        liveChildTransform.isDirty = true;
        (void)transforms.update(registry);
        CHECK(std::abs(liveChildTransform.worldMatrix[3].x - 1.0f) < 0.0001f);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    const std::vector<TestCase> tests{
        { "handle packing and null", handlePackingAndNullAreExplicit },
        { "destroyed handle cannot alias reused index", destroyedHandleCannotAliasReusedIndex },
        { "clear invalidates live handles", clearInvalidatesEveryLiveHandle },
        { "generation exhaustion is hard", generationExhaustionIsHardAndNonDestructive },
        { "non-mutating pool lookup", nonMutatingPoolLookupDoesNotCreateStorage },
        { "creation failure rollback", creationFailureRollsBackEntitySlot },
        { "UUID parsing and UUIDv5", uuidParsingAndV5DerivationAreStrict },
        { "scene world identity consistency", sceneWorldMaintainsOneToOneIdentity },
        { "duplicate UUID rollback", duplicateUuidCreationRollsBack },
        { "staging commit preserves registry address", stagingCommitPreservesRegistryAddressAndObservers },
        { "identity validation", identityValidationFindsMissingAndDeadBindings },
        { "recycled parent handle isolation", recycledParentHandleDoesNotTargetReplacement },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            const bool passed = test.run();
            std::cout << (passed ? "[PASS] " : "[FAIL] ")
                << test.name << '\n';
            if (!passed) ++failures;
        }
        catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
