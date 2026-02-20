#pragma once
#include "../EditorPanel.h"
#include "ecs/Entity.h"

class SceneHierarchyPanel : public EditorPanel {
public:
    // We pass a pointer to the EditorSystem's selected entity so the panel can update it!
    SceneHierarchyPanel(Entity* selectedEntityPtr);

    void OnImGuiRender(Registry& registry, AssetManager* assetManager) override;

private:
    Entity* selectedEntity;

    // Moved directly from EditorSystem.h
    char importPathBuffer[256] = "assets/models/alfa_romeo/scene.gltf";
};