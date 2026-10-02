#include "scene/authoring/SourceSceneLoadTransaction.h"

#include <array>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition) do { if (!(condition)) { std::cerr << \
    "check failed at " << __LINE__ << ": " #condition "\n"; return false; } } while (false)

    struct ValueComponent { std::string value; };
    struct ReferenceComponent { Entity target = NULL_ENTITY; };
    struct AssetIntentComponent {};

    constexpr std::string_view valueId = "test.scene.value";
    constexpr std::string_view referenceId = "test.scene.reference";
    constexpr std::string_view assetId = "test.scene.asset";
    constexpr std::string_view rootUuid =
        "019fb7d3-0100-7000-8000-000000000001";
    constexpr std::string_view childUuid =
        "019fb7d3-0100-7000-8000-000000000002";

    bool encode(const Registry&, Entity, Iridium::CookedComponentWriter&) { return true; }
    bool decode(Registry&, Entity, Iridium::CookedComponentReader&) { return true; }
    bool serialize(const Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SourceJson&, std::string&) { return true; }
    bool validateSource(const Iridium::SourceJson&, std::string&) { return true; }
    bool validateRuntime(const Registry&, Entity) { return true; }

    bool deserializeValue(Registry& registry, Entity entity,
        const Iridium::SourceJson& data, std::string&) {
        registry.addComponent<ValueComponent>(entity,
            data.at("value").get<std::string>());
        return true;
    }
    bool deserializeReference(Registry& registry, Entity entity,
        const Iridium::SourceJson&, std::string&) {
        registry.addComponent<ReferenceComponent>(entity);
        return true;
    }
    bool deserializeAsset(Registry& registry, Entity entity,
        const Iridium::SourceJson&, std::string&) {
        registry.addComponent<AssetIntentComponent>(entity);
        return true;
    }
    bool noReferences(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }
    bool resolveEntityReference(Registry& registry, Entity entity,
        const Iridium::SceneIdentityMap& identities,
        Iridium::SceneReferenceState& references) {
        const auto owner = identities.persistentId(entity);
        if (!owner) return false;
        Iridium::SceneReferenceKey key{
            *owner, *Iridium::ComponentTypeId::parse(referenceId), "target" };
        const Iridium::SceneReferenceRecord* record = references.find(key);
        if (!record || record->resolution !=
            Iridium::StableReferenceResolution::Resolved) return false;
        const auto target = identities.resolve(Iridium::SceneEntityUuid(record->target));
        if (!target) return false;
        registry.getComponent<ReferenceComponent>(entity).target = *target;
        return true;
    }
    bool validateReference(const Registry& registry, Entity entity) {
        const auto& value = registry.getComponent<ReferenceComponent>(entity);
        return registry.isAlive(value.target);
    }

    Iridium::RuntimeComponentDescriptor runtimeDescriptor(
        std::string_view id, std::string_view section,
        std::vector<Iridium::PropertyDescriptor> properties,
        Iridium::ResolveComponentReferencesFn resolve,
        Iridium::ValidateRuntimeComponentFn validate = validateRuntime) {
        return {
            .id = *Iridium::ComponentTypeId::parse(id),
            .cookedSectionId = *Iridium::CookedSectionId::parse(section),
            .currentCookedVersion = 1,
            .properties = std::move(properties),
            .resolveReferences = resolve,
            .postLoadValidate = validate,
            .encodeCooked = encode,
            .decodeCooked = decode,
        };
    }

    Iridium::SourceComponentCodec sourceCodec(
        std::string_view id, uint32_t order,
        Iridium::DeserializeSourceComponentFn deserialize) {
        return {
            .componentId = *Iridium::ComponentTypeId::parse(id),
            .currentSourceVersion = 1,
            .sourceOrder = order,
            .serializeSource = serialize,
            .deserializeLocal = deserialize,
            .validateLocal = validateSource,
        };
    }

    struct Registries {
        Iridium::RuntimeComponentRegistry runtime;
        Iridium::ComponentSerializerRegistry source;
    };

    Registries registries(bool failReferencePostValidation = false) {
        Registries result;
        Iridium::PropertyDescriptor valueProperty{
            .id = *Iridium::PropertyId::parse("value"),
            .valueType = Iridium::PropertyValueType::String,
            .serializationOrder = 0,
            .required = true,
        };
        Iridium::PropertyDescriptor targetProperty{
            .id = *Iridium::PropertyId::parse("target"),
            .valueType = Iridium::PropertyValueType::EntityReference,
            .referenceKind = Iridium::PropertyReferenceKind::Entity,
            .serializationOrder = 0,
            .required = true,
        };
        Iridium::PropertyDescriptor assetProperty{
            .id = *Iridium::PropertyId::parse("model"),
            .valueType = Iridium::PropertyValueType::AssetReference,
            .referenceKind = Iridium::PropertyReferenceKind::Asset,
            .serializationOrder = 0,
            .nullable = true,
            .defaultValue = nullptr,
        };
        (void)result.runtime.add(runtimeDescriptor(
            valueId, "TST1", { valueProperty }, noReferences));
        (void)result.runtime.add(runtimeDescriptor(
            referenceId, "TST2", { targetProperty }, resolveEntityReference,
            failReferencePostValidation
                ? +[](const Registry&, Entity) { return false; }
                : validateReference));
        (void)result.runtime.add(runtimeDescriptor(
            assetId, "TST3", { assetProperty }, noReferences));
        (void)result.runtime.freezeAndValidate();
        (void)result.source.add(sourceCodec(valueId, 0, deserializeValue));
        (void)result.source.add(sourceCodec(referenceId, 1, deserializeReference));
        (void)result.source.add(sourceCodec(assetId, 2, deserializeAsset));
        (void)result.source.freezeAndValidate(result.runtime);
        return result;
    }

    std::string sourceScene(bool missingTarget = false) {
        return R"json({"format":"iridium.scene","schemaVersion":1,"name":"Stage",
          "entities":[
            {"uuid":")json" + std::string(childUuid) + R"json(","components":[
              {"id":"test.scene.reference","version":1,"data":{"target":")json" +
                std::string(missingTarget
                    ? "019fb7d3-0100-7000-8000-000000000099"
                    : rootUuid) + R"json("}},
              {"id":"test.scene.asset","version":1,"data":{"model":{"assetGuid":"01890f4c-0000-7000-8000-000000000001"}}},
              {"id":"studio.unknown.payload","version":9,"data":{"future":true}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":")json" +
                std::string(rootUuid) + R"json(","siblingOrder":0}}
            ]},
            {"uuid":")json" + std::string(rootUuid) + R"json(","components":[
              {"id":"test.scene.value","version":1,"data":{"value":"Root"}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
            ]}
          ]})json";
    }

    bool stagingBuildsCompleteWorldAndPreservesOpaqueData() {
        auto registry = registries();
        const auto read = Iridium::readSourceSceneSchema1(
            sourceScene(), registry.runtime, registry.source);
        CHECK(read);
        const auto staged = Iridium::stageSourceScene(
            *read.document, registry.runtime, registry.source);
        CHECK(staged);
        CHECK(staged.staging->world->registry().aliveCount() == 2);
        const Entity root = *staged.staging->world->identities().resolve(
            *Iridium::SceneEntityUuid::parse(rootUuid));
        const Entity child = *staged.staging->world->identities().resolve(
            *Iridium::SceneEntityUuid::parse(childUuid));
        CHECK(staged.staging->world->registry()
            .getComponent<ValueComponent>(root).value == "Root");
        CHECK(staged.staging->world->registry()
            .getComponent<ReferenceComponent>(child).target == root);
        CHECK(std::ranges::any_of(staged.staging->document.entities,
            [](const auto& entity) {
                return std::ranges::any_of(entity.components, [](const auto& component) {
                    return component.id.value() == "studio.unknown.payload" &&
                        !component.known;
                });
            }));
        CHECK(staged.staging->world->references().records().size() == 2);
        CHECK(std::ranges::any_of(staged.staging->world->references().records(),
            [](const auto& record) {
                return record.kind == Iridium::StableReferenceKind::Asset &&
                    record.resolution == Iridium::StableReferenceResolution::Pending;
            }));
        return true;
    }

    bool failedStagingCannotMutateActiveWorld() {
        auto registry = registries(true);
        Iridium::SceneWorld active;
        const auto activeUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0100-7000-8000-000000000010");
        const Entity activeEntity = active.createEntity(activeUuid);
        active.registry().addComponent<ValueComponent>(activeEntity, "Active");
        Registry* address = &active.registry();

        const auto read = Iridium::readSourceSceneSchema1(
            sourceScene(), registry.runtime, registry.source);
        CHECK(read);
        const auto staged = Iridium::stageSourceScene(
            *read.document, registry.runtime, registry.source);
        CHECK(!staged);
        CHECK(&active.registry() == address);
        CHECK(active.identities().resolve(activeUuid) == activeEntity);
        CHECK(active.registry().getComponent<ValueComponent>(activeEntity).value ==
            "Active");
        CHECK(std::ranges::any_of(staged.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "scene.load.post_validation_failed";
        }));
        return true;
    }

    bool successfulCommitIsAtomicAndAddressStable() {
        auto registry = registries();
        Iridium::SceneWorld active;
        const auto oldUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0100-7000-8000-000000000010");
        (void)active.createEntity(oldUuid);
        Registry* address = &active.registry();
        const auto read = Iridium::readSourceSceneSchema1(
            sourceScene(), registry.runtime, registry.source);
        auto staged = Iridium::stageSourceScene(
            *read.document, registry.runtime, registry.source);
        CHECK(staged);
        Iridium::commitStagedSourceScene(active, *staged.staging);
        CHECK(&active.registry() == address);
        CHECK(active.registry().aliveCount() == 2);
        CHECK(!active.identities().resolve(oldUuid));
        CHECK(staged.staging->world->identities().resolve(oldUuid).has_value());
        CHECK(active.identities().validate(active.registry()));
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "complete staged world", stagingBuildsCompleteWorldAndPreservesOpaqueData },
        std::pair{ "failed staging retains active world", failedStagingCannotMutateActiveWorld },
        std::pair{ "atomic in-place commit", successfulCommitIsAtomicAndAddressStable },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
