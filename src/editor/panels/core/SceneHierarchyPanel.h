#pragma once

#include "../EditorPanel.h"
#include "ecs/Entity.h"
#include "editor/EditorSelectionState.h"

#include <array>

namespace Iridium {
    class EditorSceneCommandService;
    class EditorTransactionService;
}

class SceneHierarchyPanel : public EditorPanel {
public:
    explicit SceneHierarchyPanel(
        Iridium::EditorSelectionState* selection,
        Iridium::EditorTransactionService* transactions,
        Iridium::EditorSceneCommandService* sceneCommands);

    void OnImGuiRender(
        Registry& registry,
        Iridium::AssetManager* assetManager) override;

private:
    void beginRename(
        Registry& registry,
        Entity entity);

    Iridium::EditorSelectionState* selection_ = nullptr;
    Iridium::EditorTransactionService* transactions_ = nullptr;
    Iridium::EditorSceneCommandService* sceneCommands_ = nullptr;
    Entity renamingEntity_ = NULL_ENTITY;
    std::array<char, 256> renameBuffer_{};
};
