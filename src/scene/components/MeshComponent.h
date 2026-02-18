#pragma once
#include <memory>
#include "assets/AssetManager.h" // Fix path relative to this folder

struct MeshComponent {
    std::shared_ptr<ModelAsset> model;
    bool enabled = true;

    void OnInspector() {
        PROPERTY(enabled);
        if (model) {
            ImGui::Text("Model: %s", model->filePath.c_str());
        }
        else {
            ImGui::Text("Model: None (Missing)");
        }
    }
};