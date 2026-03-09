#include "InspectorPanel.h"
#include "scene/ComponentRegistry.h"
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
        for (auto& [name, funcs] : ComponentRegistry::RegistryMap) {

            // Only draw it if the entity actually owns this component
            if (funcs.hasComponent(registry, *selectedEntity)) {

                // Magic: Draws the Header and all the variables using ImGuiArchive!
                funcs.drawInspector(registry, *selectedEntity);

                // Add the remove button right below the component properties
                if (name != "TransformComponent") {
                    if (ImGui::Button(("Remove " + name).c_str())) {
                        funcs.removeComponent(registry, *selectedEntity);
                    }
                }
                else {
                    ImGui::TextDisabled("(Required Component)");
                }
                ImGui::Separator();
            }
        }

        // --- ADD COMPONENT POPUP ---
        ImGui::Spacing();
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("AddComponentPopup");
        }

        if (ImGui::BeginPopup("AddComponentPopup")) {
            bool allAdded = true;

            // Loop through the dictionary to populate the menu
            for (auto& [name, funcs] : ComponentRegistry::RegistryMap) {

                // Only show the menu item if the entity DOES NOT have it yet
                if (!funcs.hasComponent(registry, *selectedEntity)) {
                    allAdded = false;

                    if (ImGui::MenuItem(name.c_str())) {
                        funcs.addComponent(registry, *selectedEntity);
                    }
                }
            }

            // If the user added everything possible
            if (allAdded) {
                ImGui::TextDisabled("All components added");
            }

            ImGui::EndPopup();
        }
    }

    ImGui::End();
}