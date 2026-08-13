#include "editor/EditorTransactionService.h"
#include "editor/EditorSceneCommandService.h"
#include "editor/EditorSceneHierarchy.h"
#include "editor/EditorMeshTransaction.h"
#include "editor/EditorPropertyTransaction.h"
#include "editor/EditorSelectionState.h"
#include "editor/EditorSceneActions.h"
#include "assets/AssetMetadata.h"
#include "renderer/rhi/Mesh.h"
#include "scene/SceneWorld.h"
#include "scene/Components.h"
#include "scene/components/LightComponent.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    struct TemporaryDirectory {
        std::filesystem::path path;
        TemporaryDirectory() {
            path = std::filesystem::temp_directory_path() /
                ("iridium-transaction-service-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    struct PropertyComponent {
        int32_t count = 3;
        std::string name = "Before";
    };

    class SequenceUuidGenerator final : public Iridium::SceneUuidGenerator {
    public:
        explicit SequenceUuidGenerator(
            std::vector<Iridium::SceneEntityUuid> values)
            : values_(std::move(values)) {}

        Iridium::SceneEntityUuid next() override {
            if (next_ == values_.size()) {
                throw std::runtime_error("UUID sequence exhausted");
            }
            return values_[next_++];
        }

    private:
        std::vector<Iridium::SceneEntityUuid> values_;
        size_t next_ = 0;
    };

    Iridium::SceneEntityUuid entityUuid(std::string_view text) {
        const auto parsed = Iridium::SceneEntityUuid::parse(text);
        if (!parsed) throw std::runtime_error("Invalid test entity UUID");
        return *parsed;
    }

    void write(const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), {} };
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

    std::string source(std::string_view name, std::string_view uuid) {
        return std::string(R"json({"format":"iridium.scene","schemaVersion":1,"name":"Doc","entities":[{"uuid":")json") +
            std::string(uuid) + R"json(","components":[
              {"id":"iridium.component.name","version":1,"data":{"value":")json" +
            std::string(name) + R"json("}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
            ]}]})json";
    }

    Iridium::EditorTransactionOperation assignOperation(
        std::string target, int& value, int before, int after,
        bool* rejectApply = nullptr, bool* rejectRevert = nullptr) {
        return {
            .target = std::move(target),
            .apply = [&value, before, after, rejectApply] {
                if (rejectApply && *rejectApply) {
                    return Iridium::EditorMutationResult::failure(
                        "apply rejected");
                }
                if (value == after) {
                    return Iridium::EditorMutationResult::noChange();
                }
                if (value != before) {
                    return Iridium::EditorMutationResult::failure(
                        "unexpected value before apply");
                }
                value = after;
                return Iridium::EditorMutationResult::applied();
            },
            .revert = [&value, before, after, rejectRevert] {
                if (rejectRevert && *rejectRevert) {
                    return Iridium::EditorMutationResult::failure(
                        "revert rejected");
                }
                if (value == before) {
                    return Iridium::EditorMutationResult::noChange();
                }
                if (value != after) {
                    return Iridium::EditorMutationResult::failure(
                        "unexpected value before revert");
                }
                value = before;
                return Iridium::EditorMutationResult::applied();
            },
            .estimatedPayloadBytes = sizeof(int) * 2,
        };
    }

    Iridium::EditorTransaction transaction(std::string label,
        Iridium::EditorTransactionOperation operation) {
        Iridium::EditorTransaction result;
        result.label = std::move(label);
        result.operations.push_back(std::move(operation));
        return result;
    }

    Iridium::AssetGuid guid(std::string_view text) {
        return *Iridium::AssetGuid::parse(text);
    }

    bool exactApplyUndoRedoAndLabels() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        int value = 4;

        CHECK(history.execute(transaction("Set value",
            assignOperation("entity/value", value, 4, 9))));
        CHECK(value == 9);
        CHECK(history.canUndo());
        CHECK(!history.canRedo());
        CHECK(history.undoLabel() == "Set value");
        CHECK(history.historyEntryCount() == 1);
        CHECK(history.estimatedHistoryBytes() != 0);

        CHECK(history.undo());
        CHECK(value == 4);
        CHECK(!history.canUndo());
        CHECK(history.canRedo());
        CHECK(history.redoLabel() == "Set value");
        CHECK(history.redo());
        CHECK(value == 9);
        return true;
    }

    bool noOpValidationAndAtomicRollback() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        int first = 1;
        int second = 2;

        const auto initialState = document.currentState();
        const auto noChange = history.execute(transaction("No-op",
            assignOperation("first", first, 1, 1)));
        CHECK(noChange.outcome == Iridium::EditorTransactionOutcome::NoChange);
        CHECK(history.historyEntryCount() == 0);
        CHECK(document.currentState() == initialState);

        bool reject = true;
        Iridium::EditorTransaction atomic;
        atomic.label = "Atomic multi-edit";
        atomic.operations.push_back(assignOperation("first", first, 1, 10));
        atomic.operations.push_back(assignOperation(
            "second", second, 2, 20, &reject));
        const auto failed = history.execute(std::move(atomic));
        CHECK(failed.outcome == Iridium::EditorTransactionOutcome::Failed);
        CHECK(first == 1);
        CHECK(second == 2);
        CHECK(history.historyEntryCount() == 0);
        CHECK(document.currentState() == initialState);
        return true;
    }

    bool failedUndoRestoresCurrentState() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        int first = 1;
        int second = 2;
        bool rejectFirstRevert = false;

        Iridium::EditorTransaction atomic;
        atomic.label = "Two values";
        atomic.operations.push_back(assignOperation(
            "first", first, 1, 10, nullptr, &rejectFirstRevert));
        atomic.operations.push_back(assignOperation("second", second, 2, 20));
        CHECK(history.execute(std::move(atomic)));
        const auto appliedState = document.currentState();

        rejectFirstRevert = true;
        const auto failed = history.undo();
        CHECK(failed.outcome == Iridium::EditorTransactionOutcome::Failed);
        CHECK(first == 10);
        CHECK(second == 20);
        CHECK(document.currentState() == appliedState);
        CHECK(history.canUndo());
        return true;
    }

    bool coalescingAndBranchingUseDistinctStates() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        int value = 0;

        auto first = transaction("Move", assignOperation("x", value, 0, 1));
        first.coalescingKey = "transform/x";
        first.coalescingSession = 72;
        CHECK(history.execute(std::move(first)));
        const auto firstState = document.currentState();

        auto second = transaction("Move", assignOperation("x", value, 1, 2));
        second.coalescingKey = "transform/x";
        second.coalescingSession = 72;
        CHECK(history.execute(std::move(second)));
        const auto coalescedState = document.currentState();
        CHECK(coalescedState != firstState);
        CHECK(history.historyEntryCount() == 1);
        CHECK(history.undo());
        CHECK(value == 0);
        CHECK(history.redo());
        CHECK(value == 2);

        CHECK(history.execute(transaction("Separate edit",
            assignOperation("x", value, 2, 3))));
        const auto discardedState = document.currentState();
        CHECK(history.undo());
        CHECK(value == 2);
        CHECK(history.execute(transaction("Branched edit",
            assignOperation("x", value, 2, 4))));
        CHECK(value == 4);
        CHECK(document.currentState() != discardedState);
        CHECK(!history.canRedo());
        return true;
    }

    bool savepointDirtyTracksHistoryState() {
        TemporaryDirectory temporary;
        const auto path = temporary.path / "state.iridium.scene.json";
        write(path, source("State",
            "019fb7d3-0400-7000-8000-000000000021"));
        writeSidecar(path);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        CHECK(document.open(path));
        Iridium::EditorTransactionService history(document);
        int value = 0;
        CHECK(!document.dirty());
        CHECK(history.execute(transaction("Edit",
            assignOperation("value", value, 0, 1))));
        CHECK(document.dirty());
        CHECK(history.undo());
        CHECK(!document.dirty());
        CHECK(history.redo());
        CHECK(document.dirty());
        CHECK(document.save());
        CHECK(!document.dirty());
        CHECK(history.undo());
        CHECK(document.dirty());
        CHECK(history.redo());
        CHECK(!document.dirty());
        return true;
    }

    bool successfulOpenClearsAndFailedOpenRetainsHistory() {
        TemporaryDirectory temporary;
        const auto firstPath = temporary.path / "first.iridium.scene.json";
        const auto secondPath = temporary.path / "second.iridium.scene.json";
        const auto invalidPath = temporary.path / "invalid.iridium.scene.json";
        write(firstPath, source("First",
            "019fb7d3-0400-7000-8000-000000000031"));
        write(secondPath, source("Second",
            "019fb7d3-0400-7000-8000-000000000032"));
        writeSidecar(firstPath);
        writeSidecar(secondPath);
        write(invalidPath, "{broken");
        writeSidecar(invalidPath);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        CHECK(document.open(firstPath));
        Iridium::EditorTransactionService history(document);
        int value = 0;
        CHECK(history.execute(transaction("Edit",
            assignOperation("value", value, 0, 1))));
        CHECK(!document.open(invalidPath));
        CHECK(history.canUndo());
        CHECK(history.historyEntryCount() == 1);
        CHECK(document.open(secondPath));
        CHECK(!history.canUndo());
        CHECK(!history.canRedo());
        CHECK(history.historyEntryCount() == 0);
        return true;
    }

    bool externalMutationInvalidatesUnsafeHistory() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        int value = 0;
        CHECK(history.execute(transaction("Edit",
            assignOperation("value", value, 0, 1))));
        (void)document.advanceState();
        CHECK(!history.canUndo());
        const auto result = history.undo();
        CHECK(result.outcome ==
            Iridium::EditorTransactionOutcome::HistoryDiverged);
        CHECK(history.historyEntryCount() == 0);
        CHECK(value == 1);
        return true;
    }

    bool meshGuidHistoryIgnoresRuntimeResidency() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Registry& registry = world.registry();
        const Entity entity = registry.createEntity();
        auto& mesh = registry.addComponent<MeshComponent>(entity);
        const auto oldModel = guid(
            "01890f4c-0000-7000-8000-000000000101");
        const auto newModel = guid(
            "01890f4c-0000-7000-8000-000000000102");
        const auto sourceMaterial = guid(
            "01890f4c-0000-7000-8000-000000000103");
        const auto replacementMaterial = guid(
            "01890f4c-0000-7000-8000-000000000104");
        mesh.assetGuid = oldModel;

        const auto before = Iridium::captureEditorMeshAuthoringState(mesh);
        auto after = before;
        after.modelGuid = newModel;
        after.materialOverrides = {{ sourceMaterial, replacementMaterial }};
        Iridium::EditorTransaction assign;
        assign.label = "Assign Model and Material";
        assign.operations.push_back(
            Iridium::makeEditorMeshAuthoringOperation(
                registry, entity, before, after));
        CHECK(history.execute(std::move(assign)));
        const auto assignedState = document.currentState();
        CHECK(mesh.requestedAssetGuid == newModel);
        CHECK(mesh.materialOverrides == after.materialOverrides);

        // Pending and failed runtime states are diagnostics, not authoring edits.
        mesh.assetResolutionDiagnostic = "publication pending";
        mesh.requestedMaterialAssetRoots.push_back(newModel);
        CHECK(document.currentState() == assignedState);
        CHECK(history.canUndo());

        // Publication consumes the request and changes only residency fields.
        auto resident = std::make_shared<Iridium::ModelAsset>();
        resident->assetGuid = newModel;
        mesh.model = resident;
        mesh.assetGuid = newModel;
        mesh.requestedAssetGuid = {};
        mesh.assetResolutionDiagnostic.clear();
        mesh.requestedMaterialAssetRoots.clear();
        CHECK(document.currentState() == assignedState);
        CHECK(history.undo());
        CHECK(mesh.requestedAssetGuid == oldModel);
        CHECK(mesh.materialOverrides == before.materialOverrides);

        // Redo is valid while the old asset request is pending.
        mesh.assetResolutionDiagnostic = "old model pending";
        CHECK(history.redo());
        CHECK(mesh.requestedAssetGuid == newModel);
        CHECK(mesh.materialOverrides == after.materialOverrides);

        // Eviction after publication likewise cannot alter history or dirtiness.
        mesh.assetGuid = newModel;
        mesh.requestedAssetGuid = {};
        mesh.model.reset();
        mesh.assetResolutionDiagnostic = "evicted";
        CHECK(history.canUndo());
        CHECK(document.currentState() == assignedState);
        CHECK(history.undo());
        CHECK(mesh.requestedAssetGuid == oldModel);
        return true;
    }

    bool meshAssignmentFromNoneUndoClearsPublishedAsset() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Registry& registry = world.registry();
        const Entity entity = registry.createEntity();
        auto& mesh = registry.addComponent<MeshComponent>(entity);
        const auto modelGuid = guid(
            "01890f4c-0000-7000-8000-000000000111");
        const auto before = Iridium::captureEditorMeshAuthoringState(mesh);
        auto after = before;
        after.modelGuid = modelGuid;
        Iridium::EditorTransaction assign;
        assign.label = "Assign Model";
        assign.operations.push_back(
            Iridium::makeEditorMeshAuthoringOperation(
                registry, entity, before, after));
        CHECK(history.execute(std::move(assign)));

        auto resident = std::make_shared<Iridium::ModelAsset>();
        resident->assetGuid = modelGuid;
        mesh.model = resident;
        mesh.assetGuid = modelGuid;
        mesh.requestedAssetGuid = {};
        CHECK(history.undo());
        CHECK(mesh.requestedAssetGuid.isNil());
        CHECK(mesh.assetGuid.isNil());
        CHECK(!mesh.model);
        CHECK(Iridium::captureEditorMeshAuthoringState(mesh).modelGuid.isNil());
        CHECK(history.redo());
        CHECK(mesh.requestedAssetGuid == modelGuid);
        return true;
    }

    bool pendingMeshAssignmentSavesBeforePublication() {
        TemporaryDirectory temporary;
        const auto path = temporary.path / "pending.iridium.scene.json";
        write(path, source("Pending",
            "019fb7d3-0400-7000-8000-000000000121"));
        writeSidecar(path);
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        CHECK(document.open(path));
        Iridium::EditorTransactionService history(document);
        Registry& registry = world.registry();
        const Entity entity = registry.aliveEntities().front();
        auto& mesh = registry.addComponent<MeshComponent>(entity);
        const auto modelGuid = guid(
            "01890f4c-0000-7000-8000-000000000122");
        const auto before = Iridium::captureEditorMeshAuthoringState(mesh);
        auto after = before;
        after.modelGuid = modelGuid;
        Iridium::EditorTransaction assign;
        assign.label = "Assign Pending Model";
        assign.operations.push_back(
            Iridium::makeEditorMeshAuthoringOperation(
                registry, entity, before, after));
        CHECK(history.execute(std::move(assign)));
        CHECK(document.dirty());
        CHECK(mesh.requestedAssetGuid == modelGuid);
        CHECK(document.save());
        CHECK(!document.dirty());
        CHECK(read(path).find(modelGuid.toString()) != std::string::npos);

        mesh.assetResolutionDiagnostic = "cook failed; retry retained";
        CHECK(!document.dirty());
        auto resident = std::make_shared<Iridium::ModelAsset>();
        resident->assetGuid = modelGuid;
        mesh.model = resident;
        mesh.assetGuid = modelGuid;
        mesh.requestedAssetGuid = {};
        mesh.assetResolutionDiagnostic.clear();
        CHECK(!document.dirty());
        CHECK(history.undo());
        CHECK(document.dirty());
        return true;
    }

    bool multiTargetPropertiesAreAtomicAndValidated() {
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Registry& registry = world.registry();
        auto descriptor =
            Iridium::editorComponentDescriptor<PropertyComponent>(
                "test.property.component", "Property", 0, 100.0f,
                true, false, true);
        descriptor.properties.push_back(Iridium::editorPropertyDescriptor(
            "count", "Count", 0, Iridium::PropertyValueType::Int32,
            &PropertyComponent::count, false, false, int32_t{3},
            0.0f, 10.0f));
        const auto& property = descriptor.properties.front();
        std::vector<Entity> entities;
        for (int index = 0; index < 3; ++index) {
            const Entity entity = registry.createEntity();
            registry.addComponent<PropertyComponent>(entity).count = index + 1;
            entities.push_back(entity);
        }

        Iridium::EditorTransaction multi;
        multi.label = "Set Counts";
        for (Entity entity : entities) {
            auto& component = registry.getComponent<PropertyComponent>(entity);
            const auto before = Iridium::captureEditorPropertyValue(
                property, &component);
            CHECK(before.has_value());
            multi.operations.push_back(Iridium::makeEditorPropertyOperation(
                registry, entity, descriptor, property, *before,
                Iridium::EditorPropertyValue{int32_t{9}}));
        }
        CHECK(history.execute(std::move(multi)));
        for (Entity entity : entities) {
            CHECK(registry.getComponent<PropertyComponent>(entity).count == 9);
        }
        CHECK(history.undo());
        for (size_t index = 0; index < entities.size(); ++index) {
            CHECK(registry.getComponent<PropertyComponent>(
                entities[index]).count == static_cast<int32_t>(index + 1));
        }
        CHECK(history.redo());

        CHECK(history.undo());
        const auto stateBeforeFailure = document.currentState();
        Iridium::EditorTransaction invalid;
        invalid.label = "Invalid Counts";
        for (size_t index = 0; index < entities.size(); ++index) {
            auto& component = registry.getComponent<PropertyComponent>(
                entities[index]);
            const auto before = Iridium::captureEditorPropertyValue(
                property, &component);
            invalid.operations.push_back(Iridium::makeEditorPropertyOperation(
                registry, entities[index], descriptor, property, *before,
                Iridium::EditorPropertyValue{index == 1
                    ? int32_t{99} : int32_t{8}}));
        }
        const auto rejected = history.execute(std::move(invalid));
        CHECK(rejected.outcome == Iridium::EditorTransactionOutcome::Failed);
        CHECK(document.currentState() == stateBeforeFailure);
        for (size_t index = 0; index < entities.size(); ++index) {
            CHECK(registry.getComponent<PropertyComponent>(
                entities[index]).count == static_cast<int32_t>(index + 1));
        }
        return true;
    }

    bool componentRemoveUndoPreservesUnknownProperties() {
        TemporaryDirectory temporary;
        const auto path = temporary.path / "component-unknown.iridium.scene.json";
        write(path, R"json({
          "format":"iridium.scene",
          "schemaVersion":1,
          "name":"Component unknown preservation",
          "entities":[{
            "uuid":"019fb7d3-0400-7000-8000-000000000141",
            "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Light"}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
              {"id":"iridium.component.light","version":1,"data":{
                "type":1,
                "intensity":1.0,
                "studioHint":{"group":"hero","revision":7}
              }}
            ]
          }]
        })json");
        writeSidecar(path);
        const std::string originalSource = read(path);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        CHECK(document.open(path));
        CHECK(!document.dirty());
        CHECK(read(path) == originalSource);
        Iridium::EditorTransactionService history(document);
        Registry& registry = world.registry();
        const Entity entity = registry.aliveEntities().front();
        auto* lights = registry.findPool<LightComponent>();
        CHECK(lights && lights->has(entity));
        const LightComponent snapshot = lights->get(entity);

        Iridium::EditorTransaction remove;
        remove.label = "Remove Light";
        remove.operations.push_back({
            .target = "iridium.component.light",
            .apply = [&registry, entity] {
                auto* pool = registry.findPool<LightComponent>();
                if (!pool || !pool->has(entity)) {
                    return Iridium::EditorMutationResult::noChange();
                }
                pool->remove(entity);
                return Iridium::EditorMutationResult::applied();
            },
            .revert = [&registry, entity, snapshot] {
                auto* pool = registry.findPool<LightComponent>();
                if (pool && pool->has(entity)) {
                    return Iridium::EditorMutationResult::noChange();
                }
                registry.addComponent<LightComponent>(entity, snapshot);
                return Iridium::EditorMutationResult::applied();
            },
            .estimatedPayloadBytes = sizeof(LightComponent),
        });
        CHECK(history.execute(std::move(remove)));
        CHECK(!lights->has(entity));
        CHECK(history.undo());
        CHECK(lights->has(entity));

        auto& restored = lights->get(entity);
        Iridium::EditorTransaction edit;
        edit.label = "Edit Restored Light";
        edit.operations.push_back(
            Iridium::makeEditorValueOperation<float>(
                "entity/light/luminous-intensity-candela",
                [&registry, entity]() -> float* {
                    auto* pool = registry.findPool<LightComponent>();
                    return pool && pool->has(entity)
                        ? &pool->get(entity).luminousIntensityCandela : nullptr;
                }, restored.luminousIntensityCandela, 2.0f));
        edit.operations.push_back(
            Iridium::makeEditorValueOperation<LightType>(
                "entity/light/type",
                [&registry, entity]() -> LightType* {
                    auto* pool = registry.findPool<LightComponent>();
                    return pool && pool->has(entity)
                        ? &pool->get(entity).type : nullptr;
                }, restored.type, LightType::Spot));
        CHECK(history.execute(std::move(edit)));
        CHECK(lights->get(entity).type == LightType::Spot);
        CHECK(lights->get(entity).luminousIntensityCandela == 2.0f);
        CHECK(lights->get(entity).illuminanceLux == restored.illuminanceLux);
        CHECK(history.undo());
        CHECK(lights->get(entity).type == LightType::Point);
        CHECK(lights->get(entity).luminousIntensityCandela ==
            restored.luminousIntensityCandela);
        CHECK(history.redo());
        CHECK(lights->get(entity).type == LightType::Spot);
        CHECK(lights->get(entity).luminousIntensityCandela == 2.0f);
        CHECK(document.save());
        const std::string saved = read(path);
        CHECK(saved.find("\"luminousIntensityCandela\": 2") !=
            std::string::npos);
        CHECK(saved.find("\"version\": 2") != std::string::npos);
        CHECK(saved.find("\"studioHint\"") != std::string::npos);
        CHECK(saved.find("\"group\": \"hero\"") != std::string::npos);
        CHECK(saved.find("\"revision\": 7") != std::string::npos);
        return true;
    }

    bool selectionStateSupportsToggleAndExternalCollapse() {
        Registry registry;
        const Entity first = registry.createEntity();
        const Entity second = registry.createEntity();
        const Entity third = registry.createEntity();
        Iridium::EditorSelectionState selection;
        selection.selectExclusive(first);
        CHECK(selection.primary == first);
        CHECK(selection.entities.size() == 1);
        selection.toggle(second);
        CHECK(selection.primary == second);
        CHECK(selection.entities.size() == 2);
        CHECK(selection.contains(first));
        CHECK(selection.contains(second));
        selection.toggle(second);
        CHECK(selection.primary == first);
        CHECK(selection.entities.size() == 1);

        // Existing integrations write the primary pointer directly. Reconcile
        // intentionally converts that into an exclusive selection.
        selection.primary = third;
        selection.reconcile(registry);
        CHECK(selection.primary == third);
        CHECK(selection.entities.size() == 1);
        CHECK(selection.entities.front() == third);
        CHECK(registry.destroyEntity(third));
        selection.reconcile(registry);
        CHECK(selection.primary == NULL_ENTITY);
        CHECK(selection.entities.empty());
        return true;
    }

    bool structuralCreateDeleteSubtreeUsesStableIdentity() {
        const auto rootUuid = entityUuid(
            "018f4b5a-1000-7000-8000-000000000001");
        const auto childUuid = entityUuid(
            "018f4b5a-1000-7000-8000-000000000002");
        auto generator = std::make_unique<SequenceUuidGenerator>(
            std::vector<Iridium::SceneEntityUuid>{ rootUuid, childUuid });
        Iridium::SceneWorld world(std::move(generator));
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Iridium::EditorSelectionState selection;
        Iridium::EditorSceneCommandService commands(
            document, history, selection);
        CHECK(commands.ready());

        const Entity originalRoot = commands.createEmpty("Root");
        CHECK(originalRoot != NULL_ENTITY);
        CHECK(world.identities().persistentId(originalRoot) == rootUuid);
        CHECK(selection.primary == originalRoot);
        CHECK(history.undo());
        CHECK(!world.identities().resolve(rootUuid));
        CHECK(selection.primary == NULL_ENTITY);
        CHECK(history.redo());
        const Entity root = *world.identities().resolve(rootUuid);
        CHECK(root != originalRoot);
        CHECK(selection.primary == root);

        const Entity child = commands.createEmpty("Child", { 1.0f, 2.0f, 3.0f });
        CHECK(world.identities().persistentId(child) == childUuid);
        auto* relationships = world.registry().getPool<RelationshipComponent>();
        relationships->get(child).parent = root;
        relationships->get(child).siblingOrder = 0;
        CHECK(Iridium::rebuildEditorSceneHierarchy(world.registry()));
        CHECK(relationships->get(root).children.size() == 1);
        CHECK(relationships->get(root).children.front() == child);
        CHECK(relationships->get(child).depth == 1);
        auto& light = world.registry().addComponent<LightComponent>(child);
        light.luminousIntensityCandela = 17.0f;
        selection.selectExclusive(root);
        selection.toggle(child);

        CHECK(commands.deleteEntity(root));
        CHECK(!world.identities().resolve(rootUuid));
        CHECK(!world.identities().resolve(childUuid));
        CHECK(selection.primary == NULL_ENTITY);
        CHECK(selection.entities.empty());
        CHECK(history.undo());
        const Entity restoredRoot = *world.identities().resolve(rootUuid);
        const Entity restoredChild = *world.identities().resolve(childUuid);
        CHECK(restoredRoot != root);
        CHECK(restoredChild != child);
        relationships = world.registry().getPool<RelationshipComponent>();
        CHECK(relationships->get(restoredChild).parent == restoredRoot);
        CHECK(relationships->get(restoredChild).depth == 1);
        CHECK(relationships->get(restoredRoot).children.size() == 1);
        CHECK(relationships->get(restoredRoot).children.front() == restoredChild);
        CHECK(world.registry().getPool<LightComponent>()->get(
            restoredChild).luminousIntensityCandela == 17.0f);
        CHECK(selection.primary == restoredChild);
        CHECK(selection.contains(restoredRoot));
        CHECK(selection.contains(restoredChild));
        CHECK(history.redo());
        CHECK(!world.identities().resolve(rootUuid));
        CHECK(!world.identities().resolve(childUuid));
        CHECK(history.undo());
        CHECK(world.identities().resolve(rootUuid));
        CHECK(world.identities().resolve(childUuid));
        return true;
    }

    bool startupModelSupportsCreateDragAndDeleteCommands() {
        const auto startupUuid = entityUuid(
            "018f4b5a-1100-7000-8000-000000000001");
        const auto createdUuid = entityUuid(
            "018f4b5a-1100-7000-8000-000000000002");
        auto generator = std::make_unique<SequenceUuidGenerator>(
            std::vector<Iridium::SceneEntityUuid>{ startupUuid, createdUuid });
        Iridium::SceneWorld world(std::move(generator));
        const Iridium::AssetGuid startupModel =
            Iridium::createAssetGuidV7();
        const Entity startup = Iridium::createModelEditorEntity(
            world.registry(), startupModel, "Startup Model");
        CHECK(startup.index() == 0);
        CHECK(world.identities().persistentId(startup) == startupUuid);
        CHECK(world.registry().getPool<NameComponent>()->has(startup));
        CHECK(world.registry().getPool<TransformComponent>()->has(startup));
        CHECK(world.registry().getPool<RelationshipComponent>()->has(startup));
        CHECK(world.registry().getPool<MeshComponent>()->has(startup));
        CHECK(Iridium::rebuildEditorSceneHierarchy(world.registry()));

        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Iridium::EditorSelectionState selection;
        selection.selectExclusive(startup);
        Iridium::EditorSceneCommandService commands(
            document, history, selection);
        CHECK(commands.ready());

        const Iridium::AssetGuid droppedModel =
            Iridium::createAssetGuidV7();
        const Entity created = commands.createModel(
            droppedModel, "Dropped Model", { 1.0f, 2.0f, 3.0f });
        CHECK(created != NULL_ENTITY);
        CHECK(world.identities().persistentId(created) == createdUuid);
        CHECK(world.registry().getComponent<MeshComponent>(created)
            .requestedAssetGuid == droppedModel);
        CHECK(world.registry().getComponent<RelationshipComponent>(created)
            .parent == NULL_ENTITY);

        CHECK(commands.deleteEntity(startup));
        CHECK(!world.registry().isAlive(startup));
        CHECK(history.undo());
        const auto restored = world.identities().resolve(startupUuid);
        CHECK(restored);
        CHECK(world.registry().getPool<RelationshipComponent>()->has(*restored));
        return true;
    }

    bool structuralSiblingOrderAndCyclesAreValidated() {
        auto generator = std::make_unique<SequenceUuidGenerator>(
            std::vector<Iridium::SceneEntityUuid>{
                entityUuid("018f4b5a-2000-7000-8000-000000000001"),
                entityUuid("018f4b5a-2000-7000-8000-000000000002"),
                entityUuid("018f4b5a-2000-7000-8000-000000000003"),
            });
        Iridium::SceneWorld world(std::move(generator));
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Iridium::EditorSelectionState selection;
        Iridium::EditorSceneCommandService commands(
            document, history, selection);
        const Entity first = commands.createEmpty("First");
        const Entity second = commands.createEmpty("Second");
        const Entity third = commands.createEmpty("Third");
        CHECK(commands.reorder(third, first, false));
        auto hierarchy = Iridium::rebuildEditorSceneHierarchy(world.registry());
        CHECK(hierarchy);
        CHECK(hierarchy.roots == std::vector<Entity>({ third, first, second }));
        CHECK(history.undo());
        hierarchy = Iridium::rebuildEditorSceneHierarchy(world.registry());
        CHECK(hierarchy);
        CHECK(hierarchy.roots == std::vector<Entity>({ first, second, third }));
        CHECK(history.redo());
        hierarchy = Iridium::rebuildEditorSceneHierarchy(world.registry());
        CHECK(hierarchy.roots == std::vector<Entity>({ third, first, second }));

        CHECK(commands.reparent(first, third));
        hierarchy = Iridium::rebuildEditorSceneHierarchy(world.registry());
        CHECK(hierarchy.roots == std::vector<Entity>({ third, second }));
        auto* relationships = world.registry().getPool<RelationshipComponent>();
        CHECK(relationships->get(first).parent == third);
        CHECK(relationships->get(first).depth == 1);
        const size_t historyBeforeCycle = history.historyEntryCount();
        CHECK(!commands.reparent(third, first));
        CHECK(history.historyEntryCount() == historyBeforeCycle);
        CHECK(relationships->get(third).parent == NULL_ENTITY);
        CHECK(relationships->get(first).parent == third);
        CHECK(history.undo());
        hierarchy = Iridium::rebuildEditorSceneHierarchy(world.registry());
        CHECK(hierarchy.roots == std::vector<Entity>({ third, first, second }));

        const std::vector<Entity> previousFirstChildren =
            relationships->get(first).children;
        const std::vector<Entity> previousSecondChildren =
            relationships->get(second).children;
        const int previousFirstDepth = relationships->get(first).depth;
        const int previousSecondDepth = relationships->get(second).depth;
        relationships->get(first).parent = second;
        relationships->get(second).parent = first;
        const auto cycle = Iridium::rebuildEditorSceneHierarchy(world.registry());
        CHECK(!cycle);
        CHECK(cycle.diagnostic.find("cycle") != std::string::npos);
        CHECK(relationships->get(first).children == previousFirstChildren);
        CHECK(relationships->get(second).children == previousSecondChildren);
        CHECK(relationships->get(first).depth == previousFirstDepth);
        CHECK(relationships->get(second).depth == previousSecondDepth);
        return true;
    }

    bool structuralDeepHierarchyUsesIterativeTraversal() {
        constexpr size_t entityCount = 20'000;
        Registry registry;
        std::vector<Entity> entities;
        entities.reserve(entityCount);
        Entity parent = NULL_ENTITY;
        for (size_t index = 0; index < entityCount; ++index) {
            const Entity entity = registry.createEntity();
            RelationshipComponent& relationship =
                registry.addComponent<RelationshipComponent>(entity);
            relationship.parent = parent;
            relationship.siblingOrder = 0;
            entities.push_back(entity);
            parent = entity;
        }
        const auto rebuilt = Iridium::rebuildEditorSceneHierarchy(registry);
        CHECK(rebuilt);
        CHECK(rebuilt.roots == std::vector<Entity>{ entities.front() });
        CHECK(registry.getPool<RelationshipComponent>()->get(
            entities.back()).depth == static_cast<int>(entityCount - 1));
        std::vector<Entity> subtree;
        const auto collected = Iridium::collectEditorSceneSubtree(
            registry, entities.front(), subtree);
        CHECK(collected);
        CHECK(subtree == entities);
        return true;
    }

    bool structuralUndoAfterSavePreservesOpaqueSourcePayload() {
        TemporaryDirectory temporary;
        const auto path = temporary.path / "opaque.iridium.scene.json";
        constexpr std::string_view rootText =
            "018f4b5a-3000-7000-8000-000000000001";
        constexpr std::string_view childText =
            "018f4b5a-3000-7000-8000-000000000002";
        const std::string bytes = std::string(R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Opaque",
          "entities":[
            {"uuid":")json") + std::string(rootText) + R"json(",
             "studioEntity":{"group":"hero"},"components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Root"}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}}
             ]},
            {"uuid":")json" + std::string(childText) + R"json(","components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Child"}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":")json" +
            std::string(rootText) + R"json(","siblingOrder":0}},
              {"id":"studio.component.note","version":4,
               "studioEnvelope":{"revision":7},
               "data":{"text":"preserve me","target":")json" +
            std::string(rootText) + R"json("}}
             ]}
          ]})json";
        write(path, bytes);
        writeSidecar(path);

        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        CHECK(document.open(path));
        Iridium::EditorSelectionState selection;
        Iridium::EditorSceneCommandService commands(
            document, history, selection);
        const auto rootUuid = entityUuid(rootText);
        const auto childUuid = entityUuid(childText);
        const Entity root = *world.identities().resolve(rootUuid);
        selection.selectExclusive(root);
        CHECK(commands.deleteEntity(root));
        CHECK(document.save());
        CHECK(read(path).find("preserve me") == std::string::npos);
        CHECK(history.undo());
        CHECK(world.identities().resolve(rootUuid));
        CHECK(world.identities().resolve(childUuid));
        CHECK(document.save());
        const std::string restored = read(path);
        CHECK(restored.find("studio.component.note") != std::string::npos);
        CHECK(restored.find("preserve me") != std::string::npos);
        CHECK(restored.find("studioEnvelope") != std::string::npos);
        CHECK(restored.find("studioEntity") != std::string::npos);
        return true;
    }

    bool structuralDuplicateRemapsInternalAndKeepsExternalReferences() {
        std::vector<Iridium::SceneEntityUuid> uuids;
        for (uint32_t index = 1; index <= 7; ++index) {
            uuids.push_back(entityUuid(
                "018f4b5a-4000-7000-8000-00000000000" +
                std::to_string(index)));
        }
        auto generator = std::make_unique<SequenceUuidGenerator>(uuids);
        Iridium::SceneWorld world(std::move(generator));
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Iridium::EditorSelectionState selection;
        Iridium::EditorSceneCommandService commands(
            document, history, selection);
        const Entity external = commands.createEmpty("External");
        const Entity root = commands.createEmpty("Root");
        const Entity child = commands.createEmpty("Child");
        auto* relationships = world.registry().getPool<RelationshipComponent>();
        relationships->get(root).parent = external;
        relationships->get(child).parent = root;
        CHECK(Iridium::rebuildEditorSceneHierarchy(world.registry()));

        const Entity duplicatedRoot = commands.duplicateEntity(root);
        CHECK(duplicatedRoot != NULL_ENTITY);
        CHECK(world.identities().persistentId(duplicatedRoot) == uuids[3]);
        relationships = world.registry().getPool<RelationshipComponent>();
        CHECK(relationships->get(duplicatedRoot).parent == external);
        CHECK(relationships->get(duplicatedRoot).children.size() == 1);
        const Entity duplicatedChild =
            relationships->get(duplicatedRoot).children.front();
        CHECK(world.identities().persistentId(duplicatedChild) == uuids[4]);
        CHECK(relationships->get(duplicatedChild).parent == duplicatedRoot);
        const auto* names = world.registry().getPool<NameComponent>();
        CHECK(names->get(duplicatedRoot).name == "Root Copy");
        CHECK(names->get(duplicatedChild).name == "Child Copy");
        CHECK(selection.primary == duplicatedRoot);

        CHECK(history.undo());
        CHECK(!world.identities().resolve(uuids[3]));
        CHECK(!world.identities().resolve(uuids[4]));
        CHECK(history.redo());
        const Entity redoneRoot = *world.identities().resolve(uuids[3]);
        const Entity redoneChild = *world.identities().resolve(uuids[4]);
        relationships = world.registry().getPool<RelationshipComponent>();
        CHECK(relationships->get(redoneRoot).parent == external);
        CHECK(relationships->get(redoneChild).parent == redoneRoot);
        CHECK(commands.copyEntity(redoneRoot));
        CHECK(commands.deleteEntity(redoneRoot));
        const Entity pastedRoot = commands.paste();
        CHECK(world.identities().persistentId(pastedRoot) == uuids[5]);
        relationships = world.registry().getPool<RelationshipComponent>();
        CHECK(relationships->get(pastedRoot).parent == external);
        CHECK(relationships->get(pastedRoot).children.size() == 1);
        const Entity pastedChild = relationships->get(pastedRoot).children.front();
        CHECK(world.identities().persistentId(pastedChild) == uuids[6]);
        CHECK(relationships->get(pastedChild).parent == pastedRoot);
        return true;
    }

    bool structuralDuplicateBulkNamesRemainUnique() {
        constexpr size_t entityCount = 256;
        Iridium::SceneWorld world;
        Iridium::EditorSceneDocumentService document(world);
        Iridium::EditorTransactionService history(document);
        Iridium::EditorSelectionState selection;
        Iridium::EditorSceneCommandService commands(
            document, history, selection);
        std::vector<Entity> originals;
        originals.reserve(entityCount);
        for (size_t index = 0; index < entityCount; ++index) {
            const Entity entity = world.createEntity();
            world.registry().addComponent<NameComponent>(entity).name = "Same";
            world.registry().addComponent<TransformComponent>(entity);
            RelationshipComponent& relationship =
                world.registry().addComponent<RelationshipComponent>(entity);
            relationship.parent = index == 0
                ? NULL_ENTITY : originals.front();
            relationship.siblingOrder = static_cast<int>(index);
            originals.push_back(entity);
        }
        CHECK(Iridium::rebuildEditorSceneHierarchy(world.registry()));
        const Entity duplicated = commands.duplicateEntity(originals.front());
        CHECK(duplicated != NULL_ENTITY);
        std::vector<Entity> duplicates;
        CHECK(Iridium::collectEditorSceneSubtree(
            world.registry(), duplicated, duplicates));
        CHECK(duplicates.size() == entityCount);
        const auto* names = world.registry().getPool<NameComponent>();
        std::unordered_set<std::string> uniqueNames;
        uniqueNames.reserve(entityCount);
        for (Entity entity : duplicates) {
            CHECK(uniqueNames.insert(names->get(entity).name).second);
        }
        CHECK(names->get(duplicates.front()).name == "Same Copy");
        CHECK(names->get(duplicates.back()).name == "Same Copy (256)");
        return true;
    }

} // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        { "exact apply/undo/redo", exactApplyUndoRedoAndLabels },
        { "no-op and atomic rollback", noOpValidationAndAtomicRollback },
        { "failed undo rollback", failedUndoRestoresCurrentState },
        { "coalescing and branching", coalescingAndBranchingUseDistinctStates },
        { "savepoint dirty", savepointDirtyTracksHistoryState },
        { "open history lifecycle", successfulOpenClearsAndFailedOpenRetainsHistory },
        { "external mutation invalidation", externalMutationInvalidatesUnsafeHistory },
        { "mesh GUID residency independence",
            meshGuidHistoryIgnoresRuntimeResidency },
        { "mesh assignment from none",
            meshAssignmentFromNoneUndoClearsPublishedAsset },
        { "pending mesh assignment save",
            pendingMeshAssignmentSavesBeforePublication },
        { "atomic multi-target properties",
            multiTargetPropertiesAreAtomicAndValidated },
        { "component remove unknown preservation",
            componentRemoveUndoPreservesUnknownProperties },
        { "selection toggle and reconciliation",
            selectionStateSupportsToggleAndExternalCollapse },
        { "stable structural create/delete subtree",
            structuralCreateDeleteSubtreeUsesStableIdentity },
        { "startup model create drag and delete commands",
            startupModelSupportsCreateDragAndDeleteCommands },
        { "structural sibling order and cycle validation",
            structuralSiblingOrderAndCyclesAreValidated },
        { "structural deep hierarchy iterative traversal",
            structuralDeepHierarchyUsesIterativeTraversal },
        { "structural opaque payload after saved delete",
            structuralUndoAfterSavePreservesOpaqueSourcePayload },
        { "structural duplicate reference remap",
            structuralDuplicateRemapsInternalAndKeepsExternalReferences },
        { "structural duplicate bulk names",
            structuralDuplicateBulkNamesRemainUnique },
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            if (!test.run()) {
                std::cerr << "FAILED: " << test.name << '\n';
                ++failures;
            }
        }
        catch (const std::exception& exception) {
            std::cerr << "FAILED: " << test.name << ": "
                << exception.what() << '\n';
            ++failures;
        }
    }
    if (failures == 0) {
        std::cout << "Editor transaction service tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
