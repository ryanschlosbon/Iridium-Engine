#pragma once
#include "../EditorPanel.h"
#include "ecs/Entity.h"
#include "editor/EditorUIState.h" // Include the new state struct
#include <filesystem>

class MenuBarPanel : public EditorPanel {
public:
    // Add the new pointer to the constructor
    MenuBarPanel(Entity* selectedEntityPtr, EditorUIState* uiStatePtr);

    void OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) override;

private:
    Entity* selectedEntity;
    EditorUIState* uiState; // Store the pointer
    std::filesystem::path currentScenePath;
};
