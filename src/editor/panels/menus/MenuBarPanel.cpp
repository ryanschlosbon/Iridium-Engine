#include "MenuBarPanel.h"
#include "scene/SceneSerializer.h"
#include "utils/FileDialogs.h"
#include <imgui.h>

MenuBarPanel::MenuBarPanel(Entity* selectedEntityPtr, EditorUIState* uiStatePtr)
    : selectedEntity(selectedEntityPtr), uiState(uiStatePtr) {}

void MenuBarPanel::OnImGuiRender(Registry& registry, AssetManager* assetManager) {
    if (ImGui::BeginMainMenuBar()) {

        // --- FILE MENU ---
        if (ImGui::BeginMenu("File")) {

            // --- SAVE SCENE ---
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                if (currentScenePath.empty()) {
                    // THE FIX: Pass an empty string, PFD handles the filter now!
                    std::string filepath = FileDialogs::SaveFile("Save Scene", { "Iridium Scene", "*.iridium" });
                    if (!filepath.empty()) {
                        currentScenePath = filepath;
                        SceneSerializer serializer(&registry);
                        serializer.serialize(currentScenePath);
                    }
                }
                else {
                    SceneSerializer serializer(&registry);
                    serializer.serialize(currentScenePath);
                }
            }

            // --- SAVE AS ---
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                std::string filepath = FileDialogs::SaveFile("Save Scene As", { "Iridium Scene", "*.iridium" });
                if (!filepath.empty()) {
                    currentScenePath = filepath;
                    SceneSerializer serializer(&registry);
                    serializer.serialize(currentScenePath);
                }
            }

            ImGui::Separator();

            // --- LOAD SCENE ---
            if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
                // THE FIX: Pass an empty string here too!
                std::string filepath = FileDialogs::OpenFile("Load Scene", { "Iridium Scene", "*.iridium" });
                if (!filepath.empty()) {
                    currentScenePath = filepath;
                    registry.clear(); // Destroy the old scene before loading the new one!
                    SceneSerializer serializer(&registry);
                    serializer.deserialize(currentScenePath);
                }
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