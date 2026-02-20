#pragma once
#include "../EditorPanel.h"

class ToolbarPanel : public EditorPanel {
public:
    ToolbarPanel(int* toolModePtr);

    void OnImGuiRender(Registry& registry, AssetManager* assetManager) override;

private:
    int* currentToolMode;
};