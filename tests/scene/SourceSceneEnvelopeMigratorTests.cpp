#include "scene/authoring/SourceSceneEnvelopeMigrator.h"

#include "scene/SceneEntityUuid.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    constexpr std::array<uint8_t, 16> sceneAssetGuid{
        0x01, 0x9f, 0xb7, 0xd3, 0x01, 0x00, 0x70, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa,
    };

    std::string fixture(std::string_view name) {
        const std::filesystem::path path =
            std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "scene" /
            "fixtures" / name;
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        return bytes.str();
    }

    bool guidFixtureMigratesDeterministically() {
        const std::string source = fixture("m3_guid_scene_v0.json");
        const auto first = Iridium::migrateSourceSceneV0(source, sceneAssetGuid);
        const auto second = Iridium::migrateSourceSceneV0(source, sceneAssetGuid);
        CHECK(first);
        CHECK(second);
        CHECK(*first.value == *second.value);
        CHECK(first.value->at("format") == "iridium.scene");
        CHECK(first.value->at("schemaVersion") == 1);
        CHECK(first.value->at("entities").size() == 2);

        const auto expectedRoot = Iridium::deriveSceneEntityUuidV5(
            sceneAssetGuid, "legacy-entity/7").toString();
        const auto expectedChild = Iridium::deriveSceneEntityUuidV5(
            sceneAssetGuid, "legacy-entity/11").toString();
        const auto& root = first.value->at("entities").at(0);
        const auto& child = first.value->at("entities").at(1);
        CHECK(root.at("uuid") == expectedRoot);
        CHECK(child.at("uuid") == expectedChild);
        CHECK(root.at("components").at(0).at("data").at("value") ==
            "Root Vehicle");
        CHECK(child.at("components").at(0).at("data").at("value") == "Child");
        CHECK(child.at("components").at(2).at("data").at("parent") ==
            expectedRoot);
        const auto& mesh = root.at("components").at(3).at("data");
        CHECK(mesh.at("model").at("assetGuid") ==
            "01890f4c-0000-7000-8000-000000000001");
        CHECK(mesh.at("materialOverrides").at(0).at("source")
            .at("subassetGuid") ==
            "01890f4c-0000-7000-8000-000000000002");
        return true;
    }

    bool removedPathShapesAreHardErrors() {
        for (std::string_view name : {
            "pre_m3_mesh_path.json",
            "pre_m3_runtime_mesh_paths.json",
            "pre_m3_nested_mesh_file_path.json",
        }) {
            const auto migrated = Iridium::migrateSourceSceneV0(
                fixture(name), sceneAssetGuid);
            CHECK(!migrated);
            CHECK(std::ranges::any_of(migrated.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code == "scene.v0.path_identity_removed";
                }));
        }
        return true;
    }

    bool missingNameAndUnknownFieldsArePreserved() {
        const std::string source = R"json({
          "Entities":[{"EntityID":5,"Custom":{"future":true}}]
        })json";
        const auto migrated = Iridium::migrateSourceSceneV0(
            source, sceneAssetGuid);
        CHECK(migrated);
        CHECK(migrated.value->at("name") == "Untitled Scene");
        const auto& entity = migrated.value->at("entities").at(0);
        CHECK(entity.at("components").at(0).at("data").at("value") ==
            "Entity 5");
        CHECK(entity.at("extensions").at("iridium.legacy.v0")
            .at("Custom").at("future") == true);
        CHECK(std::ranges::any_of(migrated.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.severity ==
                    Iridium::SceneDiagnosticSeverity::Warning;
            }));
        return true;
    }

    bool duplicateIdsAndCyclesAreRejected() {
        const std::string duplicate = R"json({"Entities":[
          {"EntityID":1},{"EntityID":1}
        ]})json";
        CHECK(!Iridium::migrateSourceSceneV0(duplicate, sceneAssetGuid));

        const std::string cycle = R"json({"Entities":[
          {"EntityID":1,"RelationshipComponent":{"parent":2,"siblingOrder":0}},
          {"EntityID":2,"RelationshipComponent":{"parent":1,"siblingOrder":0}}
        ]})json";
        const auto migrated = Iridium::migrateSourceSceneV0(cycle, sceneAssetGuid);
        CHECK(!migrated);
        CHECK(std::ranges::any_of(migrated.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code == "scene.v0.hierarchy_cycle";
            }));

        const std::string contradiction = R"json({"Entities":[
          {"EntityID":1,"RelationshipComponent":{"parent":null,"children":[],"depth":4,"siblingOrder":0}},
          {"EntityID":2,"RelationshipComponent":{"parent":1,"children":[],"depth":1,"siblingOrder":0}}
        ]})json";
        const auto contradicted = Iridium::migrateSourceSceneV0(
            contradiction, sceneAssetGuid);
        CHECK(!contradicted);
        CHECK(std::ranges::any_of(contradicted.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code == "scene.v0.children_contradiction" ||
                    diagnostic.code == "scene.v0.depth_contradiction";
            }));
        return true;
    }

    struct TestCase { const char* name; bool (*run)(); };

} // namespace

int main() {
    const std::array tests{
        TestCase{ "GUID fixture deterministic migration", guidFixtureMigratesDeterministically },
        TestCase{ "removed path rejection", removedPathShapesAreHardErrors },
        TestCase{ "legacy unknown preservation", missingNameAndUnknownFieldsArePreserved },
        TestCase{ "legacy identity and hierarchy validation", duplicateIdsAndCyclesAreRejected },
    };
    for (const TestCase& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        std::cout << "[PASS] " << test.name << '\n';
    }
    std::cout << tests.size() << '/' << tests.size() << " tests passed\n";
    return 0;
}
