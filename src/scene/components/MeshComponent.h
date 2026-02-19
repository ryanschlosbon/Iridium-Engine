#pragma once
#include <memory>
#include <string>
#include "assets/AssetManager.h" 

struct MeshComponent {
    std::shared_ptr<ModelAsset> model;
    bool enabled = true;

    // The Option A Flag: If this is not empty, the engine knows to load a new mesh next frame.
    std::string requestedMeshPath = "";

    void OnInspector() {
        // Draw the Enable checkbox using your Reflection macros
        PROPERTY(enabled);

        ImGui::Separator();

        // Display the current model
        if (model) {
            ImGui::TextWrapped("Model: %s", model->filePath.c_str());
        }
        else {
            ImGui::TextDisabled("Model: None (Missing)");
        }

        ImGui::Spacing();

        // The Swap Interface
        static char pathBuffer[256] = ""; // Buffer to hold the typed path
        ImGui::InputText("Path", pathBuffer, sizeof(pathBuffer));

        ImGui::SameLine();

        // When clicked, we set the flag. We DO NOT load the asset here!
        if (ImGui::Button("Swap Mesh")) {
            if (strlen(pathBuffer) > 0) {
                requestedMeshPath = std::string(pathBuffer);

                // Clear the buffer after grabbing the string
                memset(pathBuffer, 0, sizeof(pathBuffer));
            }
        }
    }
};