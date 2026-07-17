#include "SceneHierarchyPanel.h"
#include "scene/Components.h" 
#include "platform/FileDialog.h"
#include <imgui.h>
#include <array>
#include <filesystem>
#include <iostream>

SceneHierarchyPanel::SceneHierarchyPanel(Entity* selectedEntityPtr)
    : selectedEntity(selectedEntityPtr) {
}

void SceneHierarchyPanel::OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) {
    ImGui::Begin("Scene Hierarchy");
    auto* transformPool = registry.getPool<TransformComponent>();

    // Iterate over TransformComponent, since every entity needs a transform component
    if (transformPool) {
        for (size_t i = 0; i < transformPool->entities.size(); i++) {
            Entity e = transformPool->entities[i];

            std::string label = "Entity " + std::to_string(e);

            // Append " (Light)" if it has a light
            if (registry.getPool<LightComponent>()->has(e)) label += " (Light)";

            // Check if THIS entity matches the dereferenced global selectedEntity
            if (ImGui::Selectable(label.c_str(), *selectedEntity == e)) {
                *selectedEntity = e; // Update the global selection!
            }
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Import Model...", ImVec2(-1.0f, 0.0f))) {
        constexpr std::array filters = {
            Iridium::FileDialogFilter{ "glTF models", "*.gltf;*.glb" },
            Iridium::FileDialogFilter{ "All files", "*.*" },
        };
        const auto selectedPath = Iridium::openFileDialog(filters,
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "models");

        if (selectedPath) {
            try {
                auto newModel = assetManager->getModel(selectedPath->string());
                Entity newEntity = registry.createEntity();

                registry.addComponent<MeshComponent>(newEntity, newModel);
                registry.addComponent<TransformComponent>(newEntity);

                *selectedEntity = newEntity;
            }
            catch (const std::exception& error) {
                std::cerr << "IMPORT ERROR: " << error.what() << std::endl;
            }
        }
    }
    ImGui::End();
}
