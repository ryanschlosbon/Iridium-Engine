#include "InspectorPanel.h"
#include "scene/Components.h"
#include <imgui.h>

InspectorPanel::InspectorPanel(Entity* selectedEntityPtr)
    : selectedEntity(selectedEntityPtr) {
}

void InspectorPanel::OnImGuiRender(Registry& registry, AssetManager* assetManager) {
    ImGui::Begin("Inspector");

    // Only draw the inspector if an entity is actually selected
    if (*selectedEntity != NULL_ENTITY) {

        ImGui::Text("Entity ID: %d", (int)*selectedEntity);
        ImGui::Separator();

        // --- DELETE ENTITY BUTTON ---
        ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
        if (ImGui::Button("DELETE ENTITY", ImVec2(-1, 0))) {
            registry.destroyEntity(*selectedEntity);
            *selectedEntity = NULL_ENTITY; // Deselect so we don't crash
            ImGui::PopStyleColor();
            ImGui::End();
            return; // Exit early to prevent drawing a dead entity's components
        }
        ImGui::PopStyleColor();
        ImGui::Separator();

        // --- AUTOMATIC COMPONENT LOOP ---
        for (auto& [typeIndex, pool] : registry.getPools()) {
            if (pool) {
                std::string name = typeIndex.name();
                if (name.find("struct ") != std::string::npos) name = name.substr(7);
                if (name.find("class ") != std::string::npos) name = name.substr(6);

                // Draw the Header IF the entity actually has the component
                if (pool->getVoid(*selectedEntity) != nullptr) {
                    if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {

                        pool->DrawInspector(*selectedEntity);

                        ImGui::Spacing();

                        if (name != "TransformComponent") {
                            if (ImGui::Button(("Remove " + name).c_str())) {
                                pool->remove(*selectedEntity);
                            }
                        }
                        else {
                            ImGui::TextDisabled("(Required Component)");
                        }
                        ImGui::Separator();
                    }
                }
            }
        }

        // --- ADD COMPONENT POPUP ---
        ImGui::Spacing();
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("AddComponentPopup");
        }

        if (ImGui::BeginPopup("AddComponentPopup")) {

            // Only show "Light" if the entity DOES NOT have a LightComponent
            if (!registry.getPool<LightComponent>()->has(*selectedEntity)) {
                if (ImGui::MenuItem("Light")) {
                    registry.addComponent<LightComponent>(*selectedEntity);
                }
            }

            // Only show "Mesh" if the entity DOES NOT have a MeshComponent
            if (!registry.getPool<MeshComponent>()->has(*selectedEntity)) {
                if (ImGui::MenuItem("Mesh")) {
                    registry.addComponent<MeshComponent>(*selectedEntity);
                }
            }

            if (!registry.getPool<TransformComponent>()->has(*selectedEntity)) {
                if (ImGui::MenuItem("Transform")) {
                    registry.addComponent<TransformComponent>(*selectedEntity);
                }
            }

            // If the user added everything possible
            if (registry.getPool<LightComponent>()->has(*selectedEntity) &&
                registry.getPool<MeshComponent>()->has(*selectedEntity)) {
                ImGui::TextDisabled("All components added");
            }

            ImGui::EndPopup();
        }
    }

    ImGui::End();
}