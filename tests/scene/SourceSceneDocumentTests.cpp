#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/CoreComponentCodecs.h"
#include "scene/runtime/CoreComponentRegistry.h"

#include <array>
#include <iostream>
#include <string>
#include <utility>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    bool resolveReferences(
        Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }
    bool validateRuntime(const Registry&, Entity) { return true; }
    bool encodeRuntime(const Registry&, Entity,
        Iridium::CookedComponentWriter&) { return true; }
    bool decodeRuntime(Registry&, Entity,
        Iridium::CookedComponentReader&) { return true; }
    bool serializeSource(const Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SourceJson&, std::string&) { return true; }
    bool deserializeSource(Registry&, Entity,
        const Iridium::SourceJson&, std::string&) { return true; }
    bool validateSource(const Iridium::SourceJson& value, std::string& error) {
        if (value.is_object()) return true;
        error = "test component data is not an object";
        return false;
    }
    bool migrateName(const Iridium::SourceJson& input,
        Iridium::SourceJson& output,
        std::vector<Iridium::SourceMigrationNotice>& notices, std::string&) {
        output = input;
        output["migrated"] = true;
        notices.push_back({ "test.name_migrated", "/migrated",
            "name fixture migrated" });
        return true;
    }

    Iridium::PropertyDescriptor property(
        std::string_view id,
        Iridium::PropertyValueType type,
        uint32_t order,
        bool nullable = false) {
        return {
            .id = *Iridium::PropertyId::parse(id),
            .valueType = type,
            .referenceKind = type == Iridium::PropertyValueType::EntityReference
                ? Iridium::PropertyReferenceKind::Entity
                : Iridium::PropertyReferenceKind::None,
            .serializationOrder = order,
            .nullable = nullable,
        };
    }

    Iridium::RuntimeComponentDescriptor runtimeDescriptor(
        std::string_view id,
        std::string_view section,
        std::vector<Iridium::PropertyDescriptor> properties) {
        return {
            .id = *Iridium::ComponentTypeId::parse(id),
            .cookedSectionId = *Iridium::CookedSectionId::parse(section),
            .currentCookedVersion = 1,
            .properties = std::move(properties),
            .resolveReferences = resolveReferences,
            .postLoadValidate = validateRuntime,
            .encodeCooked = encodeRuntime,
            .decodeCooked = decodeRuntime,
        };
    }

    Iridium::SourceComponentCodec sourceCodec(
        std::string_view id,
        uint32_t version,
        uint32_t order,
        std::vector<Iridium::SourcePropertyBinding> properties) {
        return {
            .componentId = *Iridium::ComponentTypeId::parse(id),
            .currentSourceVersion = version,
            .sourceOrder = order,
            .properties = std::move(properties),
            .serializeSource = serializeSource,
            .deserializeLocal = deserializeSource,
            .validateLocal = validateSource,
        };
    }

    struct Registries {
        Iridium::RuntimeComponentRegistry runtime;
        Iridium::ComponentSerializerRegistry source;
    };

    Registries registries(bool nameMigration = false) {
        Registries result;
        (void)result.runtime.add(runtimeDescriptor(
            "iridium.component.name", "NAM1",
            { property("value", Iridium::PropertyValueType::String, 0) }));
        (void)result.runtime.add(runtimeDescriptor(
            "iridium.component.relationship", "REL1",
            {
                property("parent", Iridium::PropertyValueType::EntityReference,
                    0, true),
                property("sibling_order", Iridium::PropertyValueType::Int32, 1),
            }));
        (void)result.runtime.freezeAndValidate();

        auto name = sourceCodec("iridium.component.name",
            nameMigration ? 2u : 1u, 0,
            { { *Iridium::PropertyId::parse("value"), "value" } });
        if (nameMigration) name.migrations = { { 1, 2, migrateName } };
        (void)result.source.add(std::move(name));
        (void)result.source.add(sourceCodec(
            "iridium.component.relationship", 1, 2,
            {
                { *Iridium::PropertyId::parse("parent"), "parent" },
                { *Iridium::PropertyId::parse("sibling_order"), "siblingOrder" },
            }));
        (void)result.source.freezeAndValidate(result.runtime);
        return result;
    }

    Registries coreRegistries() {
        const Iridium::RuntimeComponentCallbacks runtimeValue{
            resolveReferences, validateRuntime, encodeRuntime, decodeRuntime,
        };
        const auto runtime = Iridium::createRuntimeSceneComponentRegistry(
            { runtimeValue, runtimeValue, runtimeValue, runtimeValue, runtimeValue,
                runtimeValue, runtimeValue, runtimeValue });
        const Iridium::SourceComponentCallbacks sourceValue{
            serializeSource, deserializeSource, validateSource,
        };
        auto source = Iridium::createSourceComponentSerializerRegistry(
            runtime.registry,
            { sourceValue, sourceValue, sourceValue, sourceValue, sourceValue,
                sourceValue, sourceValue, sourceValue });
        return { runtime.registry, std::move(source.registry) };
    }

    constexpr std::string_view rootUuid =
        "019fb7d3-0100-7000-8000-000000000001";
    constexpr std::string_view childUuid =
        "019fb7d3-0100-7000-8000-000000000002";

    bool canonicalRoundTripPreservesUnknownData() {
        auto registry = registries();
        const std::string input = R"json({
  "zTop": {"z": 1, "a": 2},
  "entities": [
    {
      "components": [
        {"data": {"siblingOrder": 0, "parent": ")json" +
            std::string(rootUuid) + R"json("}, "version": 1, "id": "iridium.component.relationship"},
        {"data": {"value": "Child"}, "version": 1, "id": "iridium.component.name"}
      ],
      "uuid": ")json" + std::string(childUuid) + R"json("
    },
    {
      "zEntity": [3, 2, 1],
      "components": [
        {"vendorEnvelope": {"z": 1, "a": 2}, "version": 7, "data": {"strength": 2.5, "futureProperty": {"z": 1, "a": 2}}, "id": "studio.vendor.wind"},
        {"version": 1, "id": "iridium.component.relationship", "data": {"siblingOrder": 0, "parent": null}},
        {"version": 1, "data": {"zFuture": 3, "value": "Root", "aFuture": 1}, "id": "iridium.component.name"}
      ],
      "extensions": {"z": 1, "a": 2},
      "uuid": ")json" + std::string(rootUuid) + R"json("
    }
  ],
  "schemaVersion": 1,
  "name": "Canonical Test",
  "aTop": true,
  "extensions": {"z": 1, "a": 2},
  "format": "iridium.scene"
})json";

        const auto read = Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source);
        CHECK(read);
        CHECK(read.document->entities.size() == 2);
        const auto written = Iridium::writeSourceSceneCanonical(
            *read.document, registry.runtime, registry.source);
        CHECK(written);
        CHECK(written.bytes->back() == '\n');

        const auto json = Iridium::SourceJson::parse(*written.bytes);
        CHECK(json.at("entities").at(0).at("uuid").get<std::string>() == rootUuid);
        CHECK(json.at("entities").at(1).at("uuid").get<std::string>() == childUuid);
        CHECK(json.at("entities").at(0).at("components").at(0).at("id") ==
            "iridium.component.name");
        CHECK(json.at("entities").at(0).at("components").at(1).at("id") ==
            "iridium.component.relationship");
        CHECK(json.at("entities").at(0).at("components").at(2).at("id") ==
            "studio.vendor.wind");
        CHECK(json.at("entities").at(0).contains("zEntity"));
        CHECK(json.at("entities").at(0).at("components").at(2)
            .contains("vendorEnvelope"));
        CHECK(json.at("entities").at(0).at("components").at(0)
            .at("data").contains("aFuture"));

        const auto reread = Iridium::readSourceSceneSchema1(
            *written.bytes, registry.runtime, registry.source);
        CHECK(reread);
        const auto rewritten = Iridium::writeSourceSceneCanonical(
            *reread.document, registry.runtime, registry.source);
        CHECK(rewritten);
        CHECK(*rewritten.bytes == *written.bytes);
        return true;
    }

    bool knownComponentsMigrateBeforeValidation() {
        auto registry = registries(true);
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Migration",
          "entities":[{"uuid":")json" + std::string(rootUuid) + R"json(",
            "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Old"}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
            ]}]
        })json";
        const auto read = Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source);
        CHECK(read);
        CHECK(read.document->entities[0].components[0].version == 2);
        CHECK(read.document->entities[0].components[0].data.at("migrated") == true);
        return true;
    }

    bool futureKnownVersionIsRejectedWithContext() {
        auto registry = registries(true);
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Future",
          "entities":[{"uuid":")json" + std::string(rootUuid) + R"json(",
            "components":[
              {"id":"iridium.component.name","version":3,"data":{"value":"Future"}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
            ]}]
        })json";
        const auto read = Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source);
        CHECK(!read);
        CHECK(!read.diagnostics.empty());
        const auto found = std::ranges::find_if(read.diagnostics,
            [](const Iridium::SceneDiagnostic& diagnostic) {
                return diagnostic.code == "scene.component.unsupported_version";
            });
        CHECK(found != read.diagnostics.end());
        CHECK(found->entity.has_value());
        CHECK(found->component->value() == "iridium.component.name");
        CHECK(found->componentVersion == 3);
        CHECK(found->migrationFrom == 3);
        CHECK(found->migrationTo == 2);
        return true;
    }

    bool unknownFutureComponentIsPreserved() {
        auto registry = registries();
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Unknown",
          "entities":[{"uuid":")json" + std::string(rootUuid) + R"json(",
            "components":[
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
              {"id":"studio.vendor.future","version":4294967295,"data":{"payload":[1,true,null]}}
            ]}]
        })json";
        const auto read = Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source);
        CHECK(read);
        CHECK(!read.document->entities[0].components[1].known);
        CHECK(read.document->entities[0].components[1].version == 4294967295u);
        return true;
    }

    bool duplicateAndNoncanonicalIdentityAreRejected() {
        auto registry = registries();
        const std::string duplicate = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Duplicate",
          "entities":[
            {"uuid":")json" + std::string(rootUuid) + R"json(","components":[]},
            {"uuid":")json" + std::string(rootUuid) + R"json(","components":[]}
          ]
        })json";
        const auto duplicateRead = Iridium::readSourceSceneSchema1(
            duplicate, registry.runtime, registry.source);
        CHECK(!duplicateRead);
        CHECK(std::ranges::any_of(duplicateRead.diagnostics,
            [](const auto& value) {
                return value.code == "scene.entity.duplicate_uuid";
            }));

        const std::string uppercase = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Uppercase",
          "entities":[{"uuid":"019FB7D3-0100-7000-8000-000000000001",
            "components":[]}]
        })json";
        const auto uppercaseRead = Iridium::readSourceSceneSchema1(
            uppercase, registry.runtime, registry.source);
        CHECK(!uppercaseRead);
        CHECK(std::ranges::any_of(uppercaseRead.diagnostics,
            [](const auto& value) { return value.code == "scene.entity.uuid"; }));
        return true;
    }

    bool invalidHierarchyIsRejected() {
        auto registry = registries();
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Cycle",
          "entities":[
            {"uuid":")json" + std::string(rootUuid) + R"json(","components":[
              {"id":"iridium.component.relationship","version":1,"data":{"parent":")json" +
                std::string(childUuid) + R"json(","siblingOrder":0}}]},
            {"uuid":")json" + std::string(childUuid) + R"json(","components":[
              {"id":"iridium.component.relationship","version":1,"data":{"parent":")json" +
                std::string(rootUuid) + R"json(","siblingOrder":0}}]}
          ]
        })json";
        const auto read = Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source);
        CHECK(!read);
        CHECK(std::ranges::any_of(read.diagnostics,
            [](const auto& value) { return value.code == "scene.hierarchy.cycle"; }));
        return true;
    }

    bool entityWithoutTransformIsValid() {
        auto registry = registries();
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"No Transform",
          "entities":[{"uuid":")json" + std::string(rootUuid) + R"json(",
            "components":[{"id":"iridium.component.relationship","version":1,
              "data":{"parent":null,"siblingOrder":0}}]}]
        })json";
        CHECK(Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source));
        return true;
    }

    bool allCoreFieldsAndDefaultsValidateAndCanonicalize() {
        auto registry = coreRegistries();
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Core",
          "entities":[{"uuid":")json" + std::string(rootUuid) + R"json(",
            "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Core Entity"}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
              {"id":"iridium.component.mesh","version":1,"data":{"enabled":true,
                "model":{"assetGuid":"01890f4c-0000-7000-8000-000000000001"},
                "materialOverrides":[
                  {"source":{"subassetGuid":"01890f4c-0000-7000-8000-000000000003"},"replacement":{"subassetGuid":"01890f4c-0000-7000-8000-000000000005"}},
                  {"source":{"subassetGuid":"01890f4c-0000-7000-8000-000000000002"},"replacement":{"subassetGuid":"01890f4c-0000-7000-8000-000000000004"}}
                ]}},
              {"id":"iridium.component.light","version":1,"data":{"type":2,"color":[0.25,0.5,1.0],"intensity":12.0,"range":30.0,"radius":0.75,"innerCone":15.0,"outerCone":40.0,"castsShadows":false}}
            ]}]
        })json";
        const auto read = Iridium::readSourceSceneSchema1(
            input, registry.runtime, registry.source);
        CHECK(read);
        const auto written = Iridium::writeSourceSceneCanonical(
            *read.document, registry.runtime, registry.source);
        CHECK(written);
        const auto json = Iridium::SourceJson::parse(*written.bytes);
        const auto& overrides = json.at("entities").at(0).at("components")
            .at(3).at("data").at("materialOverrides");
        CHECK(overrides.at(0).at("source").at("subassetGuid") ==
            "01890f4c-0000-7000-8000-000000000002");
        return true;
    }

    bool futureEnvelopeAndHierarchyConflictsAreRejected() {
        auto registry = registries();
        const std::string future = R"json({"format":"iridium.scene",
          "schemaVersion":2,"name":"Future","entities":[]})json";
        const auto futureRead = Iridium::readSourceSceneSchema1(
            future, registry.runtime, registry.source);
        CHECK(!futureRead);
        CHECK(std::ranges::any_of(futureRead.diagnostics, [](const auto& value) {
            return value.code == "scene.envelope.future_version";
        }));

        const std::string broken = R"json({"format":"iridium.scene",
          "schemaVersion":1,"name":"Broken","entities":[
          {"uuid":")json" + std::string(rootUuid) + R"json(","components":[
            {"id":"iridium.component.relationship","version":1,"data":{"parent":"019fb7d3-0100-7000-8000-000000000099","siblingOrder":0}},
            {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
          ]}]})json";
        const auto brokenRead = Iridium::readSourceSceneSchema1(
            broken, registry.runtime, registry.source);
        CHECK(!brokenRead);
        CHECK(std::ranges::any_of(brokenRead.diagnostics, [](const auto& value) {
            return value.code == "scene.component.duplicate_id";
        }));

        const std::string missingParent = R"json({"format":"iridium.scene",
          "schemaVersion":1,"name":"Missing Parent","entities":[
          {"uuid":")json" + std::string(rootUuid) + R"json(","components":[
            {"id":"iridium.component.relationship","version":1,"data":{"parent":"019fb7d3-0100-7000-8000-000000000099","siblingOrder":0}}
          ]}]})json";
        const auto missingRead = Iridium::readSourceSceneSchema1(
            missingParent, registry.runtime, registry.source);
        CHECK(!missingRead);
        CHECK(std::ranges::any_of(missingRead.diagnostics, [](const auto& value) {
            return value.code == "scene.hierarchy.missing_parent";
        }));
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    const std::array tests{
        TestCase{ "canonical unknown-data round trip", canonicalRoundTripPreservesUnknownData },
        TestCase{ "known component migration", knownComponentsMigrateBeforeValidation },
        TestCase{ "known future-version rejection", futureKnownVersionIsRejectedWithContext },
        TestCase{ "unknown future component preservation", unknownFutureComponentIsPreserved },
        TestCase{ "identity validation", duplicateAndNoncanonicalIdentityAreRejected },
        TestCase{ "hierarchy cycle rejection", invalidHierarchyIsRejected },
        TestCase{ "entity without Transform", entityWithoutTransformIsValid },
        TestCase{ "all core fields and defaults", allCoreFieldsAndDefaultsValidateAndCanonicalize },
        TestCase{ "future envelope and hierarchy conflicts", futureEnvelopeAndHierarchyConflictsAreRejected },
    };
    size_t passed = 0;
    for (const TestCase& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        ++passed;
        std::cout << "[PASS] " << test.name << '\n';
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return 0;
}
