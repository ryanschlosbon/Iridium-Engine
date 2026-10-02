#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorSceneDocumentActions.h"
#include "scene/Components.h"
#include "renderer/rhi/Mesh.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    struct TemporaryDirectory {
        std::filesystem::path path;
        TemporaryDirectory() {
            path = std::filesystem::temp_directory_path() /
                ("iridium-document-service-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    void write(const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void writeSidecar(const std::filesystem::path& sourcePath) {
        Iridium::AssetMetadata metadata;
        metadata.assetGuid = Iridium::createAssetGuidV7();
        metadata.assetType = "iridium.scene";
        metadata.importerId = "iridium.scene";
        metadata.importerVersion = 1;
        std::string error;
        if (!Iridium::writeAssetMetadataAtomic(
                Iridium::assetMetadataSidecarPath(sourcePath), metadata, error)) {
            throw std::runtime_error(error);
        }
    }

    std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), {} };
    }

    std::string source(std::string_view name, std::string_view uuid) {
        return std::string(R"json({"format":"iridium.scene","schemaVersion":1,"name":"Doc","entities":[{"uuid":")json") +
            std::string(uuid) + R"json(","components":[
              {"id":"iridium.component.name","version":1,"data":{"value":")json" +
            std::string(name) + R"json("}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
            ]}]})json";
    }

    bool failedOpenRetainsWorldPathAndState() {
        TemporaryDirectory temporary;
        const auto valid = temporary.path / "valid.iridium.scene.json";
        const auto invalid = temporary.path / "invalid.json";
        write(valid, source("Loaded",
            "019fb7d3-0400-7000-8000-000000000001"));
        writeSidecar(valid);
        write(invalid, "{broken");

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        CHECK(service.ready());
        CHECK(service.open(valid));
        const Entity loaded = world.registry().aliveEntities().front();
        CHECK(world.registry().getComponent<NameComponent>(loaded).name == "Loaded");
        const auto path = service.currentPath();
        const auto token = service.currentState();
        CHECK(!service.dirty());

        CHECK(!service.open(invalid));
        CHECK(service.currentPath() == path);
        CHECK(service.currentState() == token);
        CHECK(!service.dirty());
        CHECK(world.registry().aliveCount() == 1);
        CHECK(world.registry().getComponent<NameComponent>(loaded).name == "Loaded");
        return true;
    }

    bool saveAsTokensFailuresAndBackupAreTransactional() {
        TemporaryDirectory temporary;
        const auto input = temporary.path / "input.iridium.scene.json";
        const auto output = temporary.path / "output.iridium.scene.json";
        write(input, source("Before",
            "019fb7d3-0400-7000-8000-000000000002"));
        writeSidecar(input);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        CHECK(service.open(input));
        Entity entity = world.registry().aliveEntities().front();
        world.registry().getComponent<NameComponent>(entity).name = "First Save";
        const auto firstEdit = service.advanceState();
        CHECK(service.dirty());
        CHECK(service.saveAs(output));
        CHECK(service.currentPath() == output);
        CHECK(service.savedState() == firstEdit);
        CHECK(!service.dirty());

        const std::string firstBytes = read(output);
        world.registry().getComponent<NameComponent>(entity).name = "Failed Save";
        const auto failedEdit = service.advanceState();
        CHECK(!service.save({ Iridium::AtomicSceneSavePhase::VerifyTemporary }));
        CHECK(service.currentState() == failedEdit);
        CHECK(service.savedState() == firstEdit);
        CHECK(service.dirty());
        CHECK(read(output) == firstBytes);

        world.registry().getComponent<NameComponent>(entity).name = "Second Save";
        CHECK(service.save());
        CHECK(!service.dirty());
        CHECK(read(service.backupPath()) == firstBytes);
        CHECK(read(output).find("Second Save") != std::string::npos);

        const auto saveToken = service.savedState();
        const auto branchToken = service.advanceState();
        CHECK(branchToken != saveToken);
        service.adoptState(saveToken);
        CHECK(!service.dirty());
        service.adoptState(branchToken);
        CHECK(service.dirty());
        CHECK(service.advanceState() != branchToken);
        return true;
    }

    bool explicitBackupRecoveryIsDirtyAndFailureSafe() {
        TemporaryDirectory temporary;
        const auto primary = temporary.path / "scene.iridium.scene.json";
        const auto backup = std::filesystem::path(primary.string() + ".bak");
        write(primary, "{corrupt");
        write(backup, source("Recovered",
            "019fb7d3-0400-7000-8000-000000000003"));
        writeSidecar(primary);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        Entity selection = NULL_ENTITY;
        CHECK(!Iridium::openEditorScene(service, selection, primary));
        CHECK(world.registry().aliveCount() == 0);
        CHECK(Iridium::recoverEditorSceneBackup(
            service, selection, primary));
        CHECK(service.currentPath() == primary);
        CHECK(service.dirty());
        const Entity recovered = world.registry().aliveEntities().front();
        CHECK(world.registry().getComponent<NameComponent>(recovered).name ==
            "Recovered");
        selection = recovered;

        write(backup, "{also corrupt");
        const auto token = service.currentState();
        CHECK(!Iridium::recoverEditorSceneBackup(
            service, selection, primary));
        CHECK(selection == recovered);
        CHECK(service.currentState() == token);
        CHECK(world.registry().getComponent<NameComponent>(recovered).name ==
            "Recovered");
        return true;
    }

    bool legacyMigrationRequiresSaveAsAndPreservesEntityIdentity() {
        TemporaryDirectory temporary;
        const auto legacy = temporary.path / "legacy.json";
        write(legacy, R"json({"Scene":"Legacy","Entities":[{
          "EntityID":7,"Name":"Legacy Entity",
          "TransformComponent":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},
          "RelationshipComponent":{"parent":null,"children":[],"depth":0,"siblingOrder":0}
        }]})json");
        writeSidecar(legacy);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        CHECK(service.open(legacy));
        CHECK(service.dirty());
        CHECK(!service.save());
        CHECK(service.operationDiagnostic().find("Save As") != std::string::npos);
        const Entity entity = world.registry().aliveEntities().front();
        const auto migratedUuid = world.identities().persistentId(entity);
        CHECK(migratedUuid && migratedUuid->version() == 5);

        const auto canonical = temporary.path / "legacy.iridium.scene.json";
        CHECK(service.saveAs(canonical));
        CHECK(!service.dirty());
        CHECK(std::filesystem::is_regular_file(canonical));
        CHECK(std::filesystem::is_regular_file(
            Iridium::assetMetadataSidecarPath(canonical)));
        CHECK(world.identities().persistentId(entity) == migratedUuid);
        return true;
    }

    bool failedFirstSaveAsRemovesNewSidecar() {
        TemporaryDirectory temporary;
        Iridium::SceneWorld world;
        const Entity entity = world.createEntity();
        world.registry().addComponent<NameComponent>(entity, "Unsaved");
        world.registry().addComponent<TransformComponent>(entity);
        world.registry().addComponent<RelationshipComponent>(entity);
        Iridium::EditorSceneDocumentService service(world);
        const auto destination = temporary.path / "failed.iridium.scene.json";
        CHECK(!service.saveAs(destination,
            { Iridium::AtomicSceneSavePhase::ReplaceDestination }));
        CHECK(service.currentPath().empty());
        CHECK(service.dirty());
        CHECK(!std::filesystem::exists(destination));
        CHECK(!std::filesystem::exists(
            Iridium::assetMetadataSidecarPath(destination)));
        return true;
    }

    bool orphanTemporaryRequiresExplicitListedRecovery() {
        TemporaryDirectory temporary;
        const auto primary = temporary.path / "scene.iridium.scene.json";
        const auto candidate = temporary.path /
            ".scene.iridium.scene.json.iridium-save-crash-1";
        const auto unlisted = temporary.path / "unlisted.json";
        write(primary, "{corrupt");
        write(candidate, source("Temporary Recovery",
            "019fb7d3-0400-7000-8000-000000000004"));
        write(unlisted, source("Not Listed",
            "019fb7d3-0400-7000-8000-000000000005"));
        writeSidecar(primary);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        Entity selection = NULL_ENTITY;
        CHECK(!Iridium::recoverEditorSceneTemporary(
            service, selection, primary, unlisted));
        CHECK(world.registry().aliveCount() == 0);
        CHECK(Iridium::recoverEditorSceneTemporary(
            service, selection, primary, candidate));
        CHECK(service.currentPath() == primary);
        CHECK(service.dirty());
        CHECK(read(primary) == "{corrupt");
        const Entity recovered = world.registry().aliveEntities().front();
        CHECK(world.registry().getComponent<NameComponent>(recovered).name ==
            "Temporary Recovery");
        CHECK(std::filesystem::exists(candidate));
        return true;
    }

    bool assetResidenceNeverChangesAuthoringState() {
        TemporaryDirectory temporary;
        const auto path = temporary.path / "asset.iridium.scene.json";
        write(path, R"json({"format":"iridium.scene","schemaVersion":1,
          "name":"Asset","entities":[{
          "uuid":"019fb7d3-0400-7000-8000-000000000006","components":[
          {"id":"iridium.component.name","version":1,"data":{"value":"Asset Entity"}},
          {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
          {"id":"iridium.component.mesh","version":1,"data":{"enabled":true,
            "model":{"assetGuid":"01890f4c-0000-7000-8000-000000000010"},
            "materialOverrides":[{
              "source":{"subassetGuid":"01890f4c-0000-7000-8000-000000000011"},
              "replacement":{"subassetGuid":"01890f4c-0000-7000-8000-000000000012"}
            }]}}
          ]}]})json");
        writeSidecar(path);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        CHECK(service.open(path));
        const auto cleanToken = service.currentState();
        const Entity entity = world.registry().aliveEntities().front();
        auto& mesh = world.registry().getComponent<MeshComponent>(entity);
        CHECK(mesh.requestedAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000010");
        CHECK(mesh.materialOverrides.size() == 1);
        CHECK(world.references().records().size() == 3);
        CHECK(std::ranges::all_of(world.references().records(),
            [](const Iridium::SceneReferenceRecord& reference) {
                return reference.resolution ==
                    Iridium::StableReferenceResolution::Pending;
            }));

        mesh.model.reset();
        mesh.assetGuid = {};
        mesh.assetResolutionDiagnostic = "Cooked product is temporarily missing";
        CHECK(service.currentState() == cleanToken);
        CHECK(!service.dirty());

        auto resident = std::make_shared<Iridium::ModelAsset>();
        resident->assetGuid = mesh.requestedAssetGuid;
        mesh.model = resident;
        mesh.assetGuid = resident->assetGuid;
        mesh.assetResolutionDiagnostic.clear();
        CHECK(service.currentState() == cleanToken);
        CHECK(!service.dirty());
        CHECK(service.save());
        CHECK(service.currentState() == cleanToken);
        CHECK(!service.dirty());
        const std::string saved = read(path);
        CHECK(saved.find("01890f4c-0000-7000-8000-000000000010") !=
            std::string::npos);
        CHECK(saved.find("01890f4c-0000-7000-8000-000000000011") !=
            std::string::npos);
        CHECK(saved.find("01890f4c-0000-7000-8000-000000000012") !=
            std::string::npos);
        CHECK(saved.find("temporarily missing") == std::string::npos);
        return true;
    }

    bool persistentSelectionResolvesAfterWorldSwap() {
        TemporaryDirectory temporary;
        const auto first = temporary.path / "first.iridium.scene.json";
        const auto second = temporary.path / "second.iridium.scene.json";
        constexpr std::string_view uuid =
            "019fb7d3-0400-7000-8000-000000000007";
        write(first, source("Before Swap", uuid));
        write(second, source("After Swap", uuid));
        writeSidecar(first);
        writeSidecar(second);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        Entity selection = NULL_ENTITY;
        CHECK(Iridium::openEditorScene(service, selection, first));
        selection = world.registry().aliveEntities().front();
        const auto persistentSelection = service.persistentId(selection);
        CHECK(persistentSelection.has_value());

        CHECK(Iridium::openEditorScene(service, selection, second));
        const auto restored = service.resolve(*persistentSelection);
        CHECK(restored.has_value() && selection == *restored);
        const auto* names = world.registry().findPool<NameComponent>();
        CHECK(names && names->has(*restored));
        CHECK(world.registry().getComponent<NameComponent>(*restored).name ==
            "After Swap");
        return true;
    }

    bool menuCommandPathAndFailureBehavior() {
        TemporaryDirectory temporary;
        const auto valid = temporary.path / "valid.iridium.scene.json";
        const auto invalid = temporary.path / "invalid.iridium.scene.json";
        write(valid, source("Visible Workflow",
            "019fb7d3-0400-7000-8000-000000000008"));
        writeSidecar(valid);
        write(invalid, "{broken");
        writeSidecar(invalid);

        CHECK(Iridium::normalizedEditorSceneSavePath(
            "named", temporary.path) ==
            temporary.path / "named.iridium.scene.json");
        CHECK(Iridium::normalizedEditorSceneSavePath(
            "named.json", temporary.path) ==
            temporary.path / "named.iridium.scene.json");
        CHECK(Iridium::normalizedEditorSceneSavePath(
            valid, temporary.path) == valid);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService service(world);
        Entity selection = NULL_ENTITY;
        CHECK(Iridium::openEditorScene(service, selection, valid));
        selection = world.registry().aliveEntities().front();
        const Entity retainedSelection = selection;
        CHECK(!Iridium::openEditorScene(service, selection, invalid));
        CHECK(selection == retainedSelection);
        CHECK(world.registry().isAlive(selection));
        CHECK(service.currentPath() == valid);
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "failed open retention", failedOpenRetainsWorldPathAndState },
        std::pair{ "transactional save and tokens",
            saveAsTokensFailuresAndBackupAreTransactional },
        std::pair{ "explicit backup recovery",
            explicitBackupRecoveryIsDirtyAndFailureSafe },
        std::pair{ "legacy migration Save As",
            legacyMigrationRequiresSaveAsAndPreservesEntityIdentity },
        std::pair{ "failed first Save As sidecar rollback",
            failedFirstSaveAsRemovesNewSidecar },
        std::pair{ "explicit orphan temporary recovery",
            orphanTemporaryRequiresExplicitListedRecovery },
        std::pair{ "asset residence is not authoring state",
            assetResidenceNeverChangesAuthoringState },
        std::pair{ "persistent selection after world swap",
            persistentSelectionResolvesAfterWorldSwap },
        std::pair{ "menu command path and failure behavior",
            menuCommandPathAndFailureBehavior },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
