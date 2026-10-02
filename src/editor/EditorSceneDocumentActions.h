#pragma once

#include "ecs/Entity.h"

#include <filesystem>

namespace Iridium {

    class EditorSceneDocumentService;

    [[nodiscard]] std::filesystem::path normalizedEditorSceneSavePath(
        std::filesystem::path requestedPath,
        const std::filesystem::path& relativeRoot = {});
    [[nodiscard]] bool openEditorScene(
        EditorSceneDocumentService& service,
        Entity& selectedEntity,
        const std::filesystem::path& path);
    [[nodiscard]] bool recoverEditorSceneBackup(
        EditorSceneDocumentService& service,
        Entity& selectedEntity,
        const std::filesystem::path& primaryPath);
    [[nodiscard]] bool recoverEditorSceneTemporary(
        EditorSceneDocumentService& service,
        Entity& selectedEntity,
        const std::filesystem::path& primaryPath,
        const std::filesystem::path& temporaryPath);

} // namespace Iridium
