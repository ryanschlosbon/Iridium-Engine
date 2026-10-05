#pragma once
#include "../EditorPanel.h"
#include "ecs/Entity.h"
#include "editor/EditorUIState.h" // Include the new state struct
#include "core/tasks/TaskSystem.h"
#include "scene/authoring/AtomicSourceSceneFile.h"
#include <array>
#include <filesystem>
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
        Iridium::EditorSceneCommandService* sceneCommands,
        Iridium::Tasks::TaskSystem* tasks = nullptr);
    // Waits for an orphan scan in flight (one directory listing).
    ~MenuBarPanel() override;

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
    // The orphaned scene-temporary scan (M7R R5b.2): a Normal task on the
    // engine task system, polled through its completion state every frame.
    class OrphanScan final : public Iridium::Tasks::TaskSet {
    public:
        OrphanScan();
        void runInline() { execute({ 0, 1 }, 0); }
        std::filesystem::path requested;
        std::vector<Iridium::OrphanedSceneTemporary> found;

    private:
        void execute(Iridium::Tasks::TaskRange range, uint32_t threadIndex) override;
    };
    Iridium::Tasks::TaskSystem* tasks_ = nullptr;
    OrphanScan orphanScan_;
    bool orphanScanInFlight_ = false;
    std::filesystem::path orphanScanPath_;
    std::vector<Iridium::OrphanedSceneTemporary> orphanedTemporaries_;
    bool orphanScanPending_ = false;
    std::string sceneDiagnostic_;
    std::string transactionDiagnostic_;
    bool transactionDiagnosticPending_ = false;
};
