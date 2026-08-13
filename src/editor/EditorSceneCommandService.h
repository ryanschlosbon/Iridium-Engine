#pragma once

#include "assets/AssetGuid.h"
#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace Iridium {

    class EditorSceneDocumentService;
    class EditorSelectionState;
    class EditorTransactionService;

    // The single mutation boundary for editor-owned scene structure. Commands
    // retain stable UUID/component snapshots; transient Entity handles never
    // become undo-history identity.
    class EditorSceneCommandService {
    public:
        EditorSceneCommandService(
            EditorSceneDocumentService& document,
            EditorTransactionService& transactions,
            EditorSelectionState& selection);

        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] const std::string& diagnostic() const noexcept;

        [[nodiscard]] Entity createEmpty(
            std::string_view preferredName = "Entity",
            glm::vec3 position = {});
        [[nodiscard]] Entity createModel(
            AssetGuid modelGuid,
            std::string_view preferredName = "Model",
            glm::vec3 position = {});
        [[nodiscard]] bool deleteEntity(Entity root);
        [[nodiscard]] bool reorder(
            Entity source, Entity target, bool insertAfter);
        [[nodiscard]] bool reparent(Entity source, Entity newParent);
        [[nodiscard]] Entity duplicateEntity(Entity root);
        [[nodiscard]] bool copyEntity(Entity root);
        [[nodiscard]] Entity paste();
        [[nodiscard]] bool canPaste() const noexcept;

    private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };

} // namespace Iridium
