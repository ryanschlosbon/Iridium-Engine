#include "SceneHierarchyPanel.h"
#include "scene/Components.h" 
#include <imgui.h>
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
    ImGui::Text("Import New Model");
    ImGui::InputText("Path", importPathBuffer, sizeof(importPathBuffer));

    if (ImGui::Button("Import")) {
        try {
            std::string fullPath = std::string(PROJECT_ROOT_DIR) + importPathBuffer;
            auto newModel = assetManager->getModel(fullPath);
            Entity newEntity = registry.createEntity();

            registry.addComponent<MeshComponent>(newEntity, newModel);
            registry.addComponent<TransformComponent>(newEntity);

            // Automatically select the newly imported entity
            *selectedEntity = newEntity;
        }
        catch (const std::exception& e) {
            std::cerr << "IMPORT ERROR: " << e.what() << std::endl;
        }
    }
    ImGui::End();
}