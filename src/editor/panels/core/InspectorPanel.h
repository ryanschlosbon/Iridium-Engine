#pragma once
#include "../EditorPanel.h"
#include "ecs/Entity.h"

class InspectorPanel : public EditorPanel {
public:
    InspectorPanel(Entity* selectedEntityPtr);

    void OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) override;

private:
    Entity* selectedEntity;
};