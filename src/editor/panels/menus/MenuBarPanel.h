#pragma once
#include "../EditorPanel.h"
#include "ecs/Entity.h"
#include "editor/EditorUIState.h" // Include the new state struct
#include "scene/authoring/AtomicSourceSceneFile.h"
#include <array>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace Iridium {
    class EditorSceneCommandService;
    class EditorSceneDocumentService;
    class EditorTransactionService;
}

class MenuBarPanel : public EditorPanel {
public:
    // Add the new pointer to the constructor
    MenuBarPanel(Entity* selectedEntityPtr, EditorUIState* uiStatePtr,
        Iridium::EditorSceneDocumentService* sceneDocumentService,
        Iridium::EditorTransactionService* transactionService,
        Iridium::EditorSceneCommandService* sceneCommands);

    void OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) override;

private:
    enum class SceneDialogMode {
        None,
        Save,
        Load,
    };

    void openSceneDialog(SceneDialogMode mode,
        const std::filesystem::path& suggestion);
    void drawSceneDialog(Registry& registry);
    void refreshSceneFiles();
    void requestOrphanScan(const std::filesystem::path& destination);
    void pollOrphanScan();
    [[nodiscard]] bool saveScene(std::filesystem::path path);
    [[nodiscard]] bool loadScene(const std::filesystem::path& path);

    Entity* selectedEntity;
    EditorUIState* uiState; // Store the pointer
    Iridium::EditorSceneDocumentService* sceneDocumentService_ = nullptr;
    Iridium::EditorTransactionService* transactionService_ = nullptr;
    Iridium::EditorSceneCommandService* sceneCommands_ = nullptr;
    std::filesystem::path failedLoadPath_;
    SceneDialogMode sceneDialogMode_ =
        SceneDialogMode::None;
    bool sceneDialogPending_ = false;
    std::array<char, 1024> scenePath_{};
    std::vector<std::filesystem::path> sceneFiles_;
    std::future<std::vector<Iridium::OrphanedSceneTemporary>> orphanScan_;
    std::filesystem::path orphanScanPath_;
    std::vector<Iridium::OrphanedSceneTemporary> orphanedTemporaries_;
    bool orphanScanPending_ = false;
    std::string sceneDiagnostic_;
    std::string transactionDiagnostic_;
    bool transactionDiagnosticPending_ = false;
};
