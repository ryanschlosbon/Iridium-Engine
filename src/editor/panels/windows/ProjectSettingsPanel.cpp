#include "ProjectSettingsPanel.h"
#include <imgui.h>

ProjectSettingsPanel::ProjectSettingsPanel(bool* isOpenPtr)
    : isOpen(isOpenPtr) {
}

void ProjectSettingsPanel::OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) {
    // 1. THE GATEKEEPER
    // If the boolean is false (because the Menu Bar hasn't toggled it), we exit instantly.
    // The window draws nothing and takes up zero CPU time.
    if (!*isOpen) return;

    // 2. THE WINDOW
    // By passing 'isOpen' (which is already a pointer) as the second argument, 
    // ImGui automatically gives the window an "X" close button in the top right.
    // If the user clicks that "X", ImGui automatically sets *isOpen to false!
    if (ImGui::Begin("Project Settings", isOpen)) {

        ImGui::Text("Engine Configuration");
        ImGui::Separator();

        // Just some dummy variables for demonstration
        static char projectName[128] = "Iridium Engine";
        static float physicsGravity = -9.81f;
        static bool enableVSync = true;

        ImGui::InputText("Project Name", projectName, sizeof(projectName));
        ImGui::DragFloat("Gravity", &physicsGravity, 0.1f);
        ImGui::Checkbox("Enable VSync", &enableVSync);

        ImGui::Spacing();
        if (ImGui::Button("Apply Settings", ImVec2(120, 0))) {
            // Future logic to save settings would go here
        }
    }

    // 3. CLEANUP
    ImGui::End();
}