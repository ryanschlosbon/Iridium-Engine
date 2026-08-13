#include "scene/authoring/SourceSceneCapture.h"

#include <array>
#include <iostream>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    struct ValueComponent {
        int value = 0;
    };

    bool resolve(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }
    bool validateRuntime(const Registry&, Entity) { return true; }
    bool encode(const Registry&, Entity, Iridium::CookedComponentWriter&) { return true; }
    bool decode(Registry&, Entity, Iridium::CookedComponentReader&) { return true; }
    bool validateSource(const Iridium::SourceJson&, std::string&) { return true; }
    bool deserialize(Registry& registry, Entity entity,
        const Iridium::SourceJson& data, std::string&) {
        registry.addComponent<ValueComponent>(entity, data.at("value").get<int>());
        return true;
    }
    bool serialize(const Registry& registry, Entity entity,
        const Iridium::SceneIdentityMap&, Iridium::SourceJson& data,
        std::string&) {
        const auto* pool = registry.findPool<ValueComponent>();
        if (!pool || !pool->has(entity)) {
            data = nullptr;
            return true;
        }
        if (!data.is_object()) data = Iridium::SourceJson::object();
        data["value"] = pool->get(entity).value;
        return true;
    }

    struct Registries {
        Iridium::RuntimeComponentRegistry runtime;
        Iridium::ComponentSerializerRegistry source;
    };

    Registries registries() {
        Registries value;
        (void)value.runtime.add({
            .id = *Iridium::ComponentTypeId::parse("test.component.value"),
            .cookedSectionId = *Iridium::CookedSectionId::parse("TST1"),
            .currentCookedVersion = 1,
            .properties = {{
                .id = *Iridium::PropertyId::parse("value"),
                .valueType = Iridium::PropertyValueType::Int32,
                .serializationOrder = 0,
                .required = true,
            }},
            .resolveReferences = resolve,
            .postLoadValidate = validateRuntime,
            .encodeCooked = encode,
            .decodeCooked = decode,
        });
        (void)value.runtime.freezeAndValidate();
        (void)value.source.add({
            .componentId = *Iridium::ComponentTypeId::parse(
                "test.component.value"),
            .currentSourceVersion = 1,
            .sourceOrder = 0,
            .properties = {{
                *Iridium::PropertyId::parse("value"), "value" }},
            .serializeSource = serialize,
            .deserializeLocal = deserialize,
            .validateLocal = validateSource,
        });
        (void)value.source.freezeAndValidate(value.runtime);
        return value;
    }

    bool liveCaptureMergesUnknownDataAndDropsRemovedKnownComponents() {
        auto registry = registries();
        Iridium::SceneWorld world;
        const auto firstUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0200-7000-8000-000000000001");
        const auto secondUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0200-7000-8000-000000000002");
        const Entity first = world.createEntity(firstUuid);
        (void)world.createEntity(secondUuid);
        world.registry().addComponent<ValueComponent>(first, 42);

        Iridium::SourceSceneDocument previous;
        previous.name = "Capture";
        previous.unknownFields["vendorTop"] = 7;
        Iridium::SourceSceneEntity oldFirst;
        oldFirst.uuid = firstUuid;
        oldFirst.unknownFields["vendorEntity"] = true;
        oldFirst.components.push_back({
            .id = *Iridium::ComponentTypeId::parse("test.component.value"),
            .version = 1,
            .data = {{"value", 1}, {"vendorProperty", "kept"}},
            .unknownEnvelopeFields = {{"vendorEnvelope", 3}},
            .known = true,
        });
        oldFirst.components.push_back({
            .id = *Iridium::ComponentTypeId::parse("studio.opaque.component"),
            .version = 99,
            .data = {{"payload", 5}},
            .known = false,
        });
        previous.entities.push_back(oldFirst);
        Iridium::SourceSceneEntity oldSecond;
        oldSecond.uuid = secondUuid;
        oldSecond.components.push_back(oldFirst.components.front());
        previous.entities.push_back(oldSecond);

        const auto captured = Iridium::captureSourceScene(
            world, previous, registry.runtime, registry.source);
        CHECK(captured);
        CHECK(captured.document->unknownFields.at("vendorTop") == 7);
        CHECK(captured.document->entities.size() == 2);
        const auto& firstResult = captured.document->entities[0];
        CHECK(firstResult.unknownFields.at("vendorEntity") == true);
        CHECK(firstResult.components.size() == 2);
        CHECK(firstResult.components[0].data.at("value") == 42);
        CHECK(firstResult.components[0].data.at("vendorProperty") == "kept");
        CHECK(firstResult.components[0].unknownEnvelopeFields.at(
            "vendorEnvelope") == 3);
        CHECK(!firstResult.components[1].known);
        CHECK(captured.document->entities[1].components.empty());
        return true;
    }

} // namespace

int main() {
    if (!liveCaptureMergesUnknownDataAndDropsRemovedKnownComponents()) {
        std::cerr << "[FAIL] live capture unknown merge\n";
        return 1;
    }
    std::cout << "[PASS] live capture unknown merge\n";
    return 0;
}
