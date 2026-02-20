#pragma once
#include "../EditorPanel.h"

class ProjectSettingsPanel : public EditorPanel {
public:
    // We pass the pointer to the specific boolean that controls this window
    ProjectSettingsPanel(bool* isOpenPtr);

    void OnImGuiRender(Registry& registry, AssetManager* assetManager) override;

private:
    bool* isOpen;
};