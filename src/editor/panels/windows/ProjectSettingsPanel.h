#pragma once
#include "../EditorPanel.h"

struct EditorOutputSettings;
namespace Iridium { struct ProjectShadowSettings; }
namespace Iridium { struct ProjectReflectionProbeSettings; }

class ProjectSettingsPanel : public EditorPanel {
public:
    // We pass the pointer to the specific boolean that controls this window
    ProjectSettingsPanel(bool* isOpenPtr, EditorOutputSettings* outputSettingsPtr,
        Iridium::ProjectShadowSettings* shadowSettingsPtr,
        bool* shadowSettingsChangedPtr,
        Iridium::ProjectReflectionProbeSettings* reflectionProbeSettingsPtr,
        bool* reflectionProbeSettingsChangedPtr);

    void OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) override;

private:
    bool* isOpen;
    EditorOutputSettings* outputSettings;
    Iridium::ProjectShadowSettings* shadowSettings;
    bool* shadowSettingsChanged;
    Iridium::ProjectReflectionProbeSettings* reflectionProbeSettings;
    bool* reflectionProbeSettingsChanged;
};
