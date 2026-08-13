#pragma once

#include "scene/authoring/AtomicSourceSceneFile.h"
#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/SceneWorld.h"
#include "assets/AssetMetadata.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    using SceneDocumentStateToken = uint64_t;

    class EditorSceneHistoryObserver {
    public:
        virtual ~EditorSceneHistoryObserver() = default;
        virtual void onSceneDocumentCommitted(
            SceneDocumentStateToken state) noexcept = 0;
    };

    class EditorSceneDocumentService {
    public:
        explicit EditorSceneDocumentService(SceneWorld& world);

        [[nodiscard]] bool ready() const noexcept {
            return static_cast<bool>(registries_);
        }
        [[nodiscard]] bool open(const std::filesystem::path& path);
        [[nodiscard]] bool recoverBackup(const std::filesystem::path& primaryPath);
        [[nodiscard]] bool recoverTemporary(
            const std::filesystem::path& primaryPath,
            const std::filesystem::path& temporaryPath);
        [[nodiscard]] bool save(AtomicSceneSaveOptions options = {});
        [[nodiscard]] bool saveAs(const std::filesystem::path& path,
            AtomicSceneSaveOptions options = {});

        [[nodiscard]] SceneDocumentStateToken advanceState();
        void adoptState(SceneDocumentStateToken token) noexcept;
        [[nodiscard]] bool dirty() const noexcept {
            return currentState_ != savedState_;
        }
        [[nodiscard]] SceneDocumentStateToken currentState() const noexcept {
            return currentState_;
        }
        [[nodiscard]] SceneDocumentStateToken savedState() const noexcept {
            return savedState_;
        }
        void setHistoryObserver(EditorSceneHistoryObserver* observer) noexcept {
            historyObserver_ = observer;
        }
        void removeHistoryObserver(EditorSceneHistoryObserver* observer) noexcept {
            if (historyObserver_ == observer) historyObserver_ = nullptr;
        }

        [[nodiscard]] const std::filesystem::path& currentPath() const noexcept {
            return currentPath_;
        }
        [[nodiscard]] std::filesystem::path backupPath() const;
        [[nodiscard]] const SourceSceneDocument& document() const noexcept {
            return document_;
        }
        // Structural undo may resurrect an entity after a save committed its
        // deletion. Re-seed the source merge cache so opaque components and
        // unknown envelope fields survive the next canonical capture.
        void restorePreservedEntities(
            std::span<const SourceSceneEntity> entities);
        [[nodiscard]] AssetGuid sceneAssetGuid() const noexcept {
            return sceneAssetGuid_;
        }
        [[nodiscard]] std::optional<SceneEntityUuid> persistentId(
            Entity entity) const {
            return world_.identities().persistentId(entity);
        }
        [[nodiscard]] std::optional<Entity> resolve(
            SceneEntityUuid uuid) const {
            return world_.identities().resolve(uuid);
        }
        [[nodiscard]] SceneWorld& world() noexcept { return world_; }
        [[nodiscard]] const SceneWorld& world() const noexcept { return world_; }
        [[nodiscard]] const std::vector<SceneDiagnostic>& diagnostics() const noexcept {
            return diagnostics_;
        }
        [[nodiscard]] const std::string& operationDiagnostic() const noexcept {
            return operationDiagnostic_;
        }

    private:
        [[nodiscard]] bool openCandidate(const std::filesystem::path& loadPath,
            const std::filesystem::path& adoptedPath, bool recovered);
        [[nodiscard]] bool saveTo(const std::filesystem::path& path,
            AtomicSceneSaveOptions options);
        void setOperationFailure(ScenePhase phase, std::string code,
            std::string message);

        SceneWorld& world_;
        CoreSceneRegistryBundle registries_;
        SourceSceneDocument document_;
        AssetGuid sceneAssetGuid_;
        std::filesystem::path currentPath_;
        bool requiresSaveAs_ = true;
        SceneDocumentStateToken nextState_ = 2;
        SceneDocumentStateToken currentState_ = 1;
        SceneDocumentStateToken savedState_ = 0;
        std::vector<SceneDiagnostic> diagnostics_;
        std::string operationDiagnostic_;
        EditorSceneHistoryObserver* historyObserver_ = nullptr;
    };

} // namespace Iridium
