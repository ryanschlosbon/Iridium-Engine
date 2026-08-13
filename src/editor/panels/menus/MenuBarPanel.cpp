#include "MenuBarPanel.h"
#include "platform/FileDialog.h"
#include "editor/EditorSceneActions.h"
#include "editor/EditorSceneCommandService.h"
#include "editor/EditorSceneDocumentActions.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorTransactionService.h"
#include <imgui.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <chrono>

MenuBarPanel::MenuBarPanel(Entity* selectedEntityPtr, EditorUIState* uiStatePtr,
    Iridium::EditorSceneDocumentService* sceneDocumentService,
    Iridium::EditorTransactionService* transactionService,
    Iridium::EditorSceneCommandService* sceneCommands)
    : selectedEntity(selectedEntityPtr), uiState(uiStatePtr),
      sceneDocumentService_(sceneDocumentService),
      transactionService_(transactionService),
      sceneCommands_(sceneCommands) {}

namespace {
    std::filesystem::path sceneDirectory() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "scenes";
    }
}

void MenuBarPanel::openSceneDialog(
    SceneDialogMode mode,
    const std::filesystem::path& suggestion) {
    sceneDialogMode_ = mode;
    sceneDialogPending_ = true;
    sceneDiagnostic_.clear();
    scenePath_.fill('\0');
    const std::string text =
        suggestion.lexically_normal().string();
    std::memcpy(scenePath_.data(), text.data(),
        (std::min)(text.size(),
            scenePath_.size() - 1));
    refreshSceneFiles();
    if (suggestion.has_filename()) requestOrphanScan(suggestion);
}

void MenuBarPanel::requestOrphanScan(
    const std::filesystem::path& destination) {
    if (destination.empty() || !destination.has_filename()) return;
    if (orphanScan_.valid() && orphanScan_.wait_for(
            std::chrono::seconds(0)) != std::future_status::ready) {
        orphanScanPending_ = true;
        orphanScanPath_ = destination.lexically_normal();
        return;
    }
    if (orphanScan_.valid()) orphanedTemporaries_ = orphanScan_.get();
    orphanScanPath_ = destination.lexically_normal();
    orphanScanPending_ = false;
    orphanedTemporaries_.clear();
    const std::filesystem::path requested = orphanScanPath_;
    orphanScan_ = std::async(std::launch::async, [requested] {
        return Iridium::findOrphanedSceneTemporaries(requested);
    });
}

void MenuBarPanel::pollOrphanScan() {
    if (!orphanScan_.valid() || orphanScan_.wait_for(
            std::chrono::seconds(0)) != std::future_status::ready) return;
    orphanedTemporaries_ = orphanScan_.get();
    if (orphanScanPending_) {
        const std::filesystem::path pending = orphanScanPath_;
        orphanScanPending_ = false;
        requestOrphanScan(pending);
    }
}

void MenuBarPanel::refreshSceneFiles() {
    sceneFiles_.clear();
    std::error_code error;
    std::filesystem::create_directories(
        sceneDirectory(), error);
    if (error) {
        sceneDiagnostic_ =
            "Could not open the scene folder: " +
            error.message();
        return;
    }
    for (std::filesystem::directory_iterator iterator(
            sceneDirectory(), error), end;
        !error && iterator != end;
        iterator.increment(error)) {
        if (!iterator->is_regular_file(error) ||
            !iterator->path().filename().string().ends_with(
                ".iridium.scene.json")) {
            continue;
        }
        sceneFiles_.push_back(
            iterator->path());
    }
    std::ranges::sort(sceneFiles_,
        [](const auto& lhs, const auto& rhs) {
            return lhs.filename().string() <
                rhs.filename().string();
        });
}

bool MenuBarPanel::saveScene(std::filesystem::path path) {
    if (path.empty()) {
        sceneDiagnostic_ =
            "Choose a scene path before saving.";
        return false;
    }
    path = Iridium::normalizedEditorSceneSavePath(
        std::move(path), sceneDirectory());
    if (!sceneDocumentService_ || !sceneDocumentService_->saveAs(path)) {
        sceneDiagnostic_ = sceneDocumentService_
            ? sceneDocumentService_->operationDiagnostic()
            : "Scene document service is unavailable";
        return false;
    }
    sceneDiagnostic_ = sceneDocumentService_->operationDiagnostic();
    refreshSceneFiles();
    return true;
}

bool MenuBarPanel::loadScene(const std::filesystem::path& requestedPath) {
    std::filesystem::path path =
        requestedPath;
    if (path.empty()) {
        sceneDiagnostic_ =
            "Choose a scene before loading.";
        return false;
    }
    if (path.is_relative()) {
        path = sceneDirectory() / path;
    }
    if (!sceneDocumentService_ || !Iridium::openEditorScene(
            *sceneDocumentService_, *selectedEntity, path)) {
        failedLoadPath_ = path;
        sceneDiagnostic_ = sceneDocumentService_
            ? sceneDocumentService_->operationDiagnostic()
            : "Scene document service is unavailable";
        requestOrphanScan(path);
        return false;
    }
    failedLoadPath_.clear();
    sceneDiagnostic_ = sceneDocumentService_->operationDiagnostic();
    return true;
}

void MenuBarPanel::drawSceneDialog(
    Registry& registry) {
    (void)registry;
    pollOrphanScan();
    if (sceneDialogMode_ ==
        SceneDialogMode::None) {
        return;
    }
    const char* title =
        sceneDialogMode_ ==
            SceneDialogMode::Save
        ? "Save Scene"
        : "Load Scene";
    if (sceneDialogPending_) {
        ImGui::OpenPopup(title);
        sceneDialogPending_ = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(640.0f, 430.0f),
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            title, nullptr,
            ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    ImGui::TextUnformatted(
        sceneDialogMode_ ==
            SceneDialogMode::Save
        ? "Save the current scene"
        : "Open a project scene");
    ImGui::Separator();
    ImGui::InputText("Path", scenePath_.data(),
        scenePath_.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        constexpr std::array sceneFilters = {
            Iridium::FileDialogFilter{
                "Iridium scenes", "*.iridium.scene.json" },
        };
        const std::filesystem::path current(
            scenePath_.data());
        const auto selected =
            sceneDialogMode_ ==
                SceneDialogMode::Save
            ? Iridium::saveFileDialog(
                sceneFilters, current, "iridium.scene.json")
            : Iridium::openFileDialog(
                sceneFilters,
                current.has_parent_path()
                    ? current.parent_path()
                    : sceneDirectory());
        if (selected) {
            scenePath_.fill('\0');
            const std::string text =
                selected->string();
            std::memcpy(scenePath_.data(),
                text.data(),
                (std::min)(text.size(),
                    scenePath_.size() - 1));
            requestOrphanScan(*selected);
        }
    }

    ImGui::TextDisabled(
        "Project scenes");
    if (ImGui::BeginChild(
            "scene-files",
            ImVec2(0.0f, 285.0f),
            ImGuiChildFlags_Borders)) {
        if (sceneFiles_.empty()) {
            ImGui::TextDisabled(
                "No saved scenes yet.");
        }
        for (const auto& path :
            sceneFiles_) {
            const bool selected =
                std::filesystem::path(
                    scenePath_.data())
                    .lexically_normal() ==
                path.lexically_normal();
            if (ImGui::Selectable(
                    path.filename()
                        .string().c_str(),
                    selected)) {
                scenePath_.fill('\0');
                const std::string text =
                    path.string();
                std::memcpy(scenePath_.data(),
                    text.data(),
                    (std::min)(text.size(),
                        scenePath_.size() - 1));
                requestOrphanScan(path);
            }
        }
    }
    ImGui::EndChild();

    if (!sceneDiagnostic_.empty()) {
        ImGui::TextWrapped(
            "%s",
            sceneDiagnostic_.c_str());
    }
    if (!orphanedTemporaries_.empty() &&
        sceneDialogMode_ == SceneDialogMode::Load) {
        ImGui::SeparatorText("Crash recovery candidates");
        ImGui::TextDisabled(
            "Candidates are verified only when explicitly recovered.");
        for (const auto& candidate : orphanedTemporaries_) {
            ImGui::PushID(candidate.path.string().c_str());
            ImGui::TextWrapped("%s (%llu bytes, SHA-256 %.12s...)",
                candidate.path.filename().string().c_str(),
                static_cast<unsigned long long>(candidate.sizeBytes),
                candidate.contentSha256.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Recover")) {
                const std::filesystem::path primary(scenePath_.data());
                if (sceneDocumentService_ &&
                    Iridium::recoverEditorSceneTemporary(
                        *sceneDocumentService_, *selectedEntity,
                        primary, candidate.path)) {
                    sceneDiagnostic_ =
                        sceneDocumentService_->operationDiagnostic();
                    ImGui::CloseCurrentPopup();
                    sceneDialogMode_ = SceneDialogMode::None;
                }
                else if (sceneDocumentService_) {
                    sceneDiagnostic_ =
                        sceneDocumentService_->operationDiagnostic();
                }
            }
            ImGui::PopID();
        }
    }
    const char* action =
        sceneDialogMode_ ==
            SceneDialogMode::Save
        ? "Save"
        : "Load";
    if (ImGui::Button(action,
            ImVec2(100.0f, 0.0f))) {
        const bool succeeded =
            sceneDialogMode_ ==
                SceneDialogMode::Save
            ? saveScene(
                std::filesystem::path(
                    scenePath_.data()))
            : loadScene(
                std::filesystem::path(
                    scenePath_.data()));
        if (succeeded) {
            ImGui::CloseCurrentPopup();
            sceneDialogMode_ =
                SceneDialogMode::None;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel",
            ImVec2(100.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        sceneDialogMode_ =
            SceneDialogMode::None;
    }
    if (sceneDialogMode_ == SceneDialogMode::Load &&
        !failedLoadPath_.empty()) {
        std::filesystem::path backup = failedLoadPath_;
        backup += ".bak";
        std::error_code backupError;
        if (std::filesystem::is_regular_file(backup, backupError)) {
            ImGui::SameLine();
            if (ImGui::Button("Recover Backup")) {
                if (sceneDocumentService_ &&
                    Iridium::recoverEditorSceneBackup(
                        *sceneDocumentService_, *selectedEntity,
                        failedLoadPath_)) {
                    sceneDiagnostic_ =
                        sceneDocumentService_->operationDiagnostic();
                    failedLoadPath_.clear();
                    ImGui::CloseCurrentPopup();
                    sceneDialogMode_ = SceneDialogMode::None;
                }
                else if (sceneDocumentService_) {
                    sceneDiagnostic_ =
                        sceneDocumentService_->operationDiagnostic();
                }
            }
        }
    }
    ImGui::EndPopup();
}

void MenuBarPanel::OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) {
    (void)assetManager;

    constexpr ImGuiInputFlags shortcutRoute =
        ImGuiInputFlags_RouteGlobal;
    bool requestSave = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_S, shortcutRoute);
    bool requestSaveAs = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, shortcutRoute);
    bool requestLoad = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_O, shortcutRoute);
    bool requestUndo = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_Z, shortcutRoute);
    bool requestRedo = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_Y, shortcutRoute) ||
        ImGui::Shortcut(
            ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
            shortcutRoute);
    bool requestCopy = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_C, shortcutRoute);
    bool requestPaste = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_V, shortcutRoute);
    bool requestDuplicate = ImGui::Shortcut(
        ImGuiMod_Ctrl | ImGuiKey_D, shortcutRoute);
    bool requestDelete = ImGui::Shortcut(
        ImGuiKey_Delete, shortcutRoute);
    if (ImGui::GetIO().WantTextInput) {
        requestCopy = false;
        requestPaste = false;
        requestDuplicate = false;
        requestDelete = false;
    }

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
            const bool canUndo = transactionService_ &&
                transactionService_->canUndo();
            const bool canRedo = transactionService_ &&
                transactionService_->canRedo();
            std::string undoLabel = "Undo";
            std::string redoLabel = "Redo";
            if (canUndo) {
                undoLabel += " ";
                undoLabel += transactionService_->undoLabel();
            }
            if (canRedo) {
                redoLabel += " ";
                redoLabel += transactionService_->redoLabel();
            }
            if (ImGui::MenuItem(
                    undoLabel.c_str(), "Ctrl+Z", false, canUndo)) {
                requestUndo = true;
            }
            if (ImGui::MenuItem(
                    redoLabel.c_str(), "Ctrl+Y", false, canRedo)) {
                requestRedo = true;
            }
            ImGui::Separator();
            const bool hasEntity = selectedEntity &&
                registry.isAlive(*selectedEntity);
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasEntity)) {
                requestCopy = true;
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                    sceneCommands_ && sceneCommands_->canPaste())) {
                requestPaste = true;
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasEntity)) {
                requestDuplicate = true;
            }
            if (ImGui::MenuItem("Delete", "Delete", false, hasEntity)) {
                requestDelete = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Empty Entity")) {
                if (sceneCommands_) (void)sceneCommands_->createEmpty();
            }
            const bool hasSelectedModel =
                uiState->selectedAsset &&
                uiState->selectedAsset->kind ==
                    Iridium::AssetDragKind::Model;
            ImGui::BeginDisabled(
                !hasSelectedModel);
            if (ImGui::MenuItem(
                    "Selected Model")) {
                if (sceneCommands_) {
                    (void)sceneCommands_->createModel(
                        uiState->selectedAsset->guid);
                }
            }
            ImGui::EndDisabled();
            if (!hasSelectedModel) {
                ImGui::TextDisabled(
                    "Select a model in the Asset Browser first.");
            }
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
            ImGui::MenuItem("Profiler", nullptr, &uiState->showProfiler);
            ImGui::MenuItem("Material Diagnostics", nullptr,
                &uiState->showMaterialDiagnostics);
            ImGui::MenuItem("Asset Browser", nullptr,
                &uiState->showAssetBrowser);
            ImGui::MenuItem("Console", nullptr,
                &uiState->showConsole);

            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    if (requestSaveAs) {
        const std::filesystem::path current = sceneDocumentService_
            ? sceneDocumentService_->currentPath()
            : std::filesystem::path{};
        const std::filesystem::path suggestion = current.empty()
            ? sceneDirectory() / "untitled.iridium.scene.json"
            : current;
        openSceneDialog(
            SceneDialogMode::Save,
            suggestion);
    }
    else if (requestSave) {
        const std::filesystem::path current = sceneDocumentService_
            ? sceneDocumentService_->currentPath()
            : std::filesystem::path{};
        if (current.empty()) {
            openSceneDialog(
                SceneDialogMode::Save,
                sceneDirectory() /
                    "untitled.iridium.scene.json");
        }
        else {
            if (!sceneDocumentService_->save()) {
                sceneDiagnostic_ =
                    sceneDocumentService_->operationDiagnostic();
                if (sceneDiagnostic_.find("Save As") != std::string::npos) {
                    openSceneDialog(SceneDialogMode::Save, current);
                }
            }
            else {
                sceneDiagnostic_ =
                    sceneDocumentService_->operationDiagnostic();
                refreshSceneFiles();
            }
        }
    }

    if (requestLoad) {
        const std::filesystem::path current = sceneDocumentService_
            ? sceneDocumentService_->currentPath()
            : std::filesystem::path{};
        openSceneDialog(
            SceneDialogMode::Load,
            current.empty()
                ? sceneDirectory()
                : current);
    }

    if (requestUndo && transactionService_ &&
        transactionService_->canUndo()) {
        const auto result = transactionService_->undo();
        if (!result) {
            transactionDiagnostic_ = result.diagnostic;
            transactionDiagnosticPending_ = true;
        }
    }
    if (requestRedo && transactionService_ &&
        transactionService_->canRedo()) {
        const auto result = transactionService_->redo();
        if (!result) {
            transactionDiagnostic_ = result.diagnostic;
            transactionDiagnosticPending_ = true;
        }
    }
    const bool hasEntity = sceneCommands_ && selectedEntity &&
        registry.isAlive(*selectedEntity);
    if (requestCopy && hasEntity &&
        !sceneCommands_->copyEntity(*selectedEntity)) {
        transactionDiagnostic_ = sceneCommands_->diagnostic();
        transactionDiagnosticPending_ = true;
    }
    if (requestPaste && sceneCommands_ &&
        sceneCommands_->paste() == NULL_ENTITY) {
        transactionDiagnostic_ = sceneCommands_->diagnostic();
        transactionDiagnosticPending_ = true;
    }
    if (requestDuplicate && hasEntity &&
        sceneCommands_->duplicateEntity(*selectedEntity) == NULL_ENTITY) {
        transactionDiagnostic_ = sceneCommands_->diagnostic();
        transactionDiagnosticPending_ = true;
    }
    if (requestDelete && hasEntity &&
        !sceneCommands_->deleteEntity(*selectedEntity)) {
        transactionDiagnostic_ = sceneCommands_->diagnostic();
        transactionDiagnosticPending_ = true;
    }

    if (transactionDiagnosticPending_) {
        ImGui::OpenPopup("Editor Command Failed");
        transactionDiagnosticPending_ = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "Editor Command Failed", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", transactionDiagnostic_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(100.0f, 0.0f))) {
            transactionDiagnostic_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    drawSceneDialog(registry);
}
