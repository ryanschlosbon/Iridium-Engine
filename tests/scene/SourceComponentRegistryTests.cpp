#include "scene/authoring/SourceComponentRegistry.h"

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
    bool validateSource(const Iridium::SourceJson&, std::string&) { return true; }

    bool addVersion(const Iridium::SourceJson& input,
        Iridium::SourceJson& output,
        std::vector<Iridium::SourceMigrationNotice>& notices, std::string&) {
        output = input;
        output["migrationCount"] = input.value("migrationCount", 0) + 1;
        notices.push_back({ "test.migrated", "/migrationCount",
            "migration step applied" });
        return true;
    }

    bool failMigration(const Iridium::SourceJson&,
        Iridium::SourceJson&, std::vector<Iridium::SourceMigrationNotice>&,
        std::string& error) {
        error = "injected migration failure";
        return false;
    }

    Iridium::RuntimeComponentDescriptor runtimeDescriptor(
        std::string_view id, std::string_view section) {
        return {
            .id = *Iridium::ComponentTypeId::parse(id),
            .cookedSectionId = *Iridium::CookedSectionId::parse(section),
            .currentCookedVersion = 1,
            .resolveReferences = resolveReferences,
            .postLoadValidate = validateRuntime,
            .encodeCooked = encodeRuntime,
            .decodeCooked = decodeRuntime,
        };
    }

    Iridium::SourceComponentCodec sourceCodec(
        std::string_view id, uint32_t version, uint32_t order) {
        return {
            .componentId = *Iridium::ComponentTypeId::parse(id),
            .currentSourceVersion = version,
            .sourceOrder = order,
            .serializeSource = serializeSource,
            .deserializeLocal = deserializeSource,
            .validateLocal = validateSource,
        };
    }

    Iridium::RuntimeComponentRegistry runtimeRegistry(bool includeMesh = false) {
        Iridium::RuntimeComponentRegistry registry;
        (void)registry.add(runtimeDescriptor("iridium.component.name", "NAM1"));
        if (includeMesh) {
            (void)registry.add(runtimeDescriptor("iridium.component.mesh", "MSH1"));
        }
        (void)registry.freezeAndValidate();
        return registry;
    }

    bool runtimeRegistryMustBeFrozenAndComplete() {
        Iridium::RuntimeComponentRegistry runtime;
        CHECK(runtime.add(runtimeDescriptor("iridium.component.name", "NAM1")));
        Iridium::ComponentSerializerRegistry source;
        CHECK(source.add(sourceCodec("iridium.component.name", 1, 0)));
        CHECK(source.freezeAndValidate(runtime).error ==
            Iridium::SourceRegistryError::RuntimeRegistryNotFrozen);
        CHECK(runtime.freezeAndValidate());
        CHECK(source.freezeAndValidate(runtime));

        auto completeRuntime = runtimeRegistry(true);
        Iridium::ComponentSerializerRegistry incomplete;
        CHECK(incomplete.add(sourceCodec("iridium.component.name", 1, 0)));
        CHECK(incomplete.freezeAndValidate(completeRuntime).error ==
            Iridium::SourceRegistryError::MissingSourceCodec);

        Iridium::ComponentSerializerRegistry unknown;
        CHECK(unknown.add(sourceCodec("studio.vendor.wind", 1, 0)));
        CHECK(unknown.freezeAndValidate(runtime).error ==
            Iridium::SourceRegistryError::UnknownRuntimeComponent);
        return true;
    }

    bool codecsFreezeInSourceOrder() {
        auto runtime = runtimeRegistry(true);
        Iridium::ComponentSerializerRegistry source;
        CHECK(source.add(sourceCodec("iridium.component.mesh", 1, 3)));
        CHECK(source.add(sourceCodec("iridium.component.name", 1, 0)));
        CHECK(source.codecs().empty());
        CHECK(source.freezeAndValidate(runtime));
        CHECK(source.codecs().size() == 2);
        CHECK(source.codecs()[0].componentId.value() == "iridium.component.name");
        CHECK(source.codecs()[1].componentId.value() == "iridium.component.mesh");
        CHECK(source.find(*Iridium::ComponentTypeId::parse(
            "iridium.component.mesh")) == &source.codecs()[1]);
        CHECK(source.add(sourceCodec("studio.vendor.wind", 1, 7)).error ==
            Iridium::SourceRegistryError::Frozen);
        return true;
    }

    bool codecIdentityAndOrderAreUnique() {
        Iridium::ComponentSerializerRegistry source;
        CHECK(source.add(sourceCodec("iridium.component.name", 1, 0)));
        CHECK(source.add(sourceCodec("iridium.component.name", 1, 1)).error ==
            Iridium::SourceRegistryError::DuplicateComponentId);
        CHECK(source.add(sourceCodec("iridium.component.mesh", 1, 0)).error ==
            Iridium::SourceRegistryError::DuplicateSourceOrder);
        return true;
    }

    bool migrationContinuityIsStrict() {
        {
            Iridium::ComponentSerializerRegistry source;
            auto codec = sourceCodec("iridium.component.name", 3, 0);
            codec.migrations = {
                { 1, 3, addVersion },
            };
            CHECK(source.add(std::move(codec)).error ==
                Iridium::SourceRegistryError::InvalidMigrationStep);
        }
        {
            Iridium::ComponentSerializerRegistry source;
            auto codec = sourceCodec("iridium.component.name", 4, 0);
            codec.migrations = {
                { 1, 2, addVersion },
                { 3, 4, addVersion },
            };
            CHECK(source.add(std::move(codec)).error ==
                Iridium::SourceRegistryError::MigrationGap);
        }
        {
            Iridium::ComponentSerializerRegistry source;
            auto codec = sourceCodec("iridium.component.name", 4, 0);
            codec.migrations = {
                { 1, 2, addVersion },
                { 2, 3, addVersion },
            };
            CHECK(source.add(std::move(codec)).error ==
                Iridium::SourceRegistryError::MigrationDoesNotReachCurrent);
        }
        return true;
    }

    bool migrationsArePureOrderedSteps() {
        auto runtime = runtimeRegistry();
        Iridium::ComponentSerializerRegistry source;
        auto codec = sourceCodec("iridium.component.name", 3, 0);
        codec.migrations = {
            { 2, 3, addVersion },
            { 1, 2, addVersion },
        };
        CHECK(source.add(std::move(codec)));
        CHECK(source.freezeAndValidate(runtime));

        const Iridium::SourceJson original{
            { "value", "Entity" },
            { "migrationCount", 0 },
        };
        const auto migrated = source.migrateToCurrent(
            *Iridium::ComponentTypeId::parse("iridium.component.name"),
            1, original);
        CHECK(migrated);
        CHECK(migrated.sourceVersion == 1);
        CHECK(migrated.targetVersion == 3);
        CHECK(migrated.data.at("migrationCount") == 2);
        CHECK(migrated.notices.size() == 2);
        CHECK(migrated.notices[0].code == "test.migrated");
        CHECK(original.at("migrationCount") == 0);

        const auto current = source.migrateToCurrent(
            *Iridium::ComponentTypeId::parse("iridium.component.name"),
            3, original);
        CHECK(current);
        CHECK(current.data == original);

        CHECK(source.migrateToCurrent(
            *Iridium::ComponentTypeId::parse("iridium.component.name"),
            4, original).status.error ==
            Iridium::SourceRegistryError::UnsupportedSourceVersion);
        CHECK(source.migrateToCurrent(
            *Iridium::ComponentTypeId::parse("iridium.component.name"),
            0, original).status.error ==
            Iridium::SourceRegistryError::UnsupportedSourceVersion);
        return true;
    }

    bool migrationFailurePreservesDiagnostic() {
        auto runtime = runtimeRegistry();
        Iridium::ComponentSerializerRegistry source;
        auto codec = sourceCodec("iridium.component.name", 2, 0);
        codec.migrations = { { 1, 2, failMigration } };
        CHECK(source.add(std::move(codec)));
        CHECK(source.freezeAndValidate(runtime));
        const auto result = source.migrateToCurrent(
            *Iridium::ComponentTypeId::parse("iridium.component.name"),
            1, Iridium::SourceJson::object());
        CHECK(result.status.error == Iridium::SourceRegistryError::MigrationFailed);
        CHECK(result.status.message == "injected migration failure");
        return true;
    }

    bool sourcePropertyBindingsAreValidated() {
        {
            Iridium::ComponentSerializerRegistry source;
            auto codec = sourceCodec("iridium.component.name", 1, 0);
            codec.properties = {
                { *Iridium::PropertyId::parse("value"), "value" },
                { *Iridium::PropertyId::parse("value"), "displayValue" },
            };
            CHECK(source.add(std::move(codec)).error ==
                Iridium::SourceRegistryError::DuplicateSourceProperty);
        }
        {
            auto runtime = runtimeRegistry();
            Iridium::ComponentSerializerRegistry source;
            auto codec = sourceCodec("iridium.component.name", 1, 0);
            codec.properties = {
                { *Iridium::PropertyId::parse("value"), "value" },
            };
            CHECK(source.add(std::move(codec)));
            CHECK(source.freezeAndValidate(runtime).error ==
                Iridium::SourceRegistryError::UnknownRuntimeProperty);
        }
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    const std::array tests{
        TestCase{ "runtime registry dependency", runtimeRegistryMustBeFrozenAndComplete },
        TestCase{ "canonical source order", codecsFreezeInSourceOrder },
        TestCase{ "unique codec identity", codecIdentityAndOrderAreUnique },
        TestCase{ "migration continuity", migrationContinuityIsStrict },
        TestCase{ "ordered pure migrations", migrationsArePureOrderedSteps },
        TestCase{ "migration failure diagnostic", migrationFailurePreservesDiagnostic },
        TestCase{ "source property binding validation", sourcePropertyBindingsAreValidated },
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
