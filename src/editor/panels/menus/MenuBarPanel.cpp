#include "MenuBarPanel.h"
#include "scene/SceneSerializer.h"
#include <imgui.h>

MenuBarPanel::MenuBarPanel(Entity* selectedEntityPtr, EditorUIState* uiStatePtr)
    : selectedEntity(selectedEntityPtr), uiState(uiStatePtr) {}

void MenuBarPanel::OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) {
    if (ImGui::BeginMainMenuBar()) {

        // --- FILE MENU ---
        if (ImGui::BeginMenu("File")) {
            // Save Scene
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                SceneSerializer serializer(&registry);
                serializer.serialize("assets/scenes/test_scene.json");
            }

            // Load Scene
            if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
                registry.clear();
                SceneSerializer serializer(&registry);
                if (serializer.deserialize("assets/scenes/test_scene.json")) {
                    // Reset selected entity so we don't crash
                    *selectedEntity = NULL_ENTITY;
                }
            }

            ImGui::Separator();

            // Exit
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                // To make this work later, you can pass a boolean pointer like `bool* isRunning` 
                // into this panel's constructor and set it to false here!
            }

            ImGui::EndMenu();
        }

        // --- EDIT MENU ---
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
            ImGui::EndMenu();
        }

        // --- VIEW MENU ---
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Hierarchy", nullptr, true);
            ImGui::MenuItem("Inspector", nullptr, true);
            ImGui::EndMenu();
        }

        // Window Menu for floating panels
        if (ImGui::BeginMenu("Window")) {

            // By passing the address of the boolean, ImGui automatically renders a checkmark 
            // next to the text when it is true, and toggles it when clicked!
            ImGui::MenuItem("Project Settings", nullptr, &uiState->showProjectSettings);

            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}