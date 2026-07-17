#include "MenuBarPanel.h"
#include "platform/FileDialog.h"
#include "scene/SceneSerializer.h"
#include <imgui.h>
#include <array>
#include <filesystem>
#include <iostream>

MenuBarPanel::MenuBarPanel(Entity* selectedEntityPtr, EditorUIState* uiStatePtr)
    : selectedEntity(selectedEntityPtr), uiState(uiStatePtr) {}

void MenuBarPanel::OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) {
    constexpr std::array sceneFilters = {
        Iridium::FileDialogFilter{ "Iridium scenes", "*.json" },
        Iridium::FileDialogFilter{ "All files", "*.*" },
    };

    const auto saveScene = [&](const std::filesystem::path& path) {
        SceneSerializer serializer(&registry);
        if (serializer.serialize(path.string())) {
            currentScenePath = path;
        }
        else {
            std::cerr << "Failed to save scene to '" << path.string() << "'.\n";
        }
    };

    bool requestSave = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S);
    bool requestSaveAs = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);
    bool requestLoad = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O);

    if (ImGui::BeginMainMenuBar()) {

        // --- FILE MENU ---
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                requestSave = true;
            }

            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                requestSaveAs = true;
            }

            if (ImGui::MenuItem("Load...", "Ctrl+O")) {
                requestLoad = true;
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

    if (requestSaveAs) {
        const std::filesystem::path suggestion = currentScenePath.empty()
            ? std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "scenes" / "untitled.json"
            : currentScenePath;
        const auto path = Iridium::saveFileDialog(sceneFilters, suggestion, "json");
        if (path) saveScene(*path);
    }
    else if (requestSave) {
        if (currentScenePath.empty()) {
            const auto path = Iridium::saveFileDialog(sceneFilters,
                std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "scenes" / "untitled.json",
                "json");
            if (path) saveScene(*path);
        }
        else {
            saveScene(currentScenePath);
        }
    }

    if (requestLoad) {
        const auto path = Iridium::openFileDialog(sceneFilters,
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "scenes");
        if (path) {
            SceneSerializer serializer(&registry);
            if (serializer.deserialize(path->string())) {
                currentScenePath = *path;
                *selectedEntity = NULL_ENTITY;
            }
            else {
                std::cerr << "Failed to load scene from '" << path->string() << "'.\n";
            }
        }
    }
}
