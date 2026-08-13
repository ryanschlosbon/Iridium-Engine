#include "editor/EditorSceneDocumentActions.h"

#include "editor/EditorSceneDocumentService.h"

#include <optional>

namespace Iridium {
    namespace {

        template<typename Action>
        bool swapScenePreservingSelection(
            EditorSceneDocumentService& service,
            Entity& selectedEntity,
            Action&& action) {
            const std::optional<SceneEntityUuid> persistentSelection =
                selectedEntity != NULL_ENTITY
                ? service.persistentId(selectedEntity)
                : std::nullopt;
            if (!action()) return false;
            selectedEntity = persistentSelection
                ? service.resolve(*persistentSelection).value_or(NULL_ENTITY)
                : NULL_ENTITY;
            return true;
        }

    } // namespace

    std::filesystem::path normalizedEditorSceneSavePath(
        std::filesystem::path requestedPath,
        const std::filesystem::path& relativeRoot) {
        if (requestedPath.empty()) return {};
        if (requestedPath.is_relative() && !relativeRoot.empty()) {
            requestedPath = relativeRoot / requestedPath;
        }
        if (!requestedPath.filename().string().ends_with(
                ".iridium.scene.json")) {
            if (requestedPath.extension() == ".json") {
                requestedPath.replace_extension();
            }
            requestedPath += ".iridium.scene.json";
        }
        return requestedPath.lexically_normal();
    }

    bool openEditorScene(
        EditorSceneDocumentService& service,
        Entity& selectedEntity,
        const std::filesystem::path& path) {
        return swapScenePreservingSelection(service, selectedEntity,
            [&] { return service.open(path); });
    }

    bool recoverEditorSceneBackup(
        EditorSceneDocumentService& service,
        Entity& selectedEntity,
        const std::filesystem::path& primaryPath) {
        return swapScenePreservingSelection(service, selectedEntity,
            [&] { return service.recoverBackup(primaryPath); });
    }

    bool recoverEditorSceneTemporary(
        EditorSceneDocumentService& service,
        Entity& selectedEntity,
        const std::filesystem::path& primaryPath,
        const std::filesystem::path& temporaryPath) {
        return swapScenePreservingSelection(service, selectedEntity,
            [&] { return service.recoverTemporary(
                primaryPath, temporaryPath); });
    }

} // namespace Iridium
