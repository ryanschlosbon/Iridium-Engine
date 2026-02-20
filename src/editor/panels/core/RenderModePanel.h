#pragma once
#include "../EditorPanel.h"

class RenderModePanel : public EditorPanel {
public:
    RenderModePanel(int* renderModePtr);

    void OnImGuiRender(Registry& registry, AssetManager* assetManager) override;

private:
    int* currentRenderMode;
};