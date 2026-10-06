#pragma once

#include "assets/AssetBrowserModel.h"
#include "assets/thumbnail/AssetThumbnailService.h"
#include "editor/panels/EditorPanel.h"
#include "editor/EditorUIState.h"
#include "editor/EditorAssetSettingsTransactionService.h"
#include "editor/EditorMaterialDraftHistory.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>

#include <nlohmann/json.hpp>

namespace Iridium {
    class AssetCatalogService;
    class AssetModelPreparationService;
    class AssetThumbnailService;
    class AssetRuntimeService;
    class EditorAssetDocumentService;
}

class AssetBrowserPanel final : public EditorPanel {
public:
    AssetBrowserPanel(bool* open, Entity* selectedEntity,
        EditorUIState* uiState,
        const Iridium::AssetCatalog* catalog,
        Iridium::AssetCatalogService* catalogService,
        Iridium::AssetModelPreparationService*
            modelPreparationService,
        Iridium::AssetThumbnailService*
            thumbnailService,
        Iridium::AssetRuntimeService* runtimeService,
        Iridium::EditorAssetDocumentService* assetDocuments);

    void OnImGuiRender(Registry& registry,
        Iridium::AssetManager* assetManager) override;
    void renderAssetParameters(Iridium::AssetGuid guid, Iridium::AssetManager* assetManager);

private:
    enum class ContentDialogMode {
        None,
        CreateFolder,
        RenameFolder,
        RenameAsset,
        DeleteFolder,
        DeleteAsset,
    };

    void refreshDecorations();
    void drawItem(Registry& registry,
        Iridium::AssetManager* assetManager,
        const Iridium::AssetBrowserItem& item,
        bool grid);
    // One registered asset root ("project", "local") and its folder tree.
    struct RootSection {
        std::string id;
        std::string label;
        std::filesystem::path path;
        std::vector<Iridium::AssetBrowserFolder> folders;
        std::vector<Iridium::AssetBrowserFolder> orderedFolders;
    };

    void rebuildFolders();
    [[nodiscard]] const RootSection* rootSection(
        std::string_view id) const noexcept;
    [[nodiscard]] std::string rootLabel(std::string_view id) const;
    // The root being browsed: the selected root section, or the only root.
    [[nodiscard]] std::optional<std::string> browseRoot() const;
    // Where root-level content actions (import, new folder) go: the browsed
    // root and folder; with nothing selected, the local asset library when one
    // is configured (third-party content belongs there), else the project.
    [[nodiscard]] std::string contentRoot() const;
    [[nodiscard]] std::string contentDirectory() const;
    void browse(std::optional<std::string> root,
        std::optional<std::string> directory);
    void drawRootActions(const std::string& root);
    void drawFolders(
        const std::string& root,
        std::span<const
            Iridium::AssetBrowserFolder> folders);
    void drawFolderItem(
        const Iridium::AssetBrowserFolder& folder,
        bool grid);
    [[nodiscard]] std::span<const
        Iridium::AssetBrowserFolder>
        currentFolders() const;
    void queueImportFromDialog(
        std::optional<std::string> root = std::nullopt,
        std::string directory = {});
    void drawAssetViewContextMenu();
    void drawAssetDrawer(
        const Iridium::AssetBrowserItem& root,
        Iridium::AssetManager* assetManager);
    void ensureDrawerCache(
        Iridium::AssetGuid rootGuid,
        Iridium::AssetManager* assetManager);
    void invalidateDrawerCache();
    void drawDrawerRecord(
        const Iridium::AssetCatalogRecord& record,
        Iridium::AssetManager* assetManager,
        Iridium::AssetBrowserDrawerSection section);
    void openContentDialog(
        ContentDialogMode mode,
        std::filesystem::path path = {},
        Iridium::AssetGuid assetGuid = {},
        std::string_view initialName = {},
        std::string root = {});
    void drawContentDialog();
    void requestAssetMove(
        Iridium::AssetGuid assetGuid,
        std::string_view destinationRoot,
        const std::filesystem::path&
            destinationDirectory);
    // Folders and assets move within their own root only.
    void requestFolderMove(
        std::string_view sourceRoot,
        std::string_view sourceDirectory,
        std::string_view destinationRoot,
        const std::filesystem::path& destinationDirectory);
    void rebuildOrderedFolders();
    void loadFolderOrder();
    void saveFolderOrder();
    void reorderFolderBefore(
        std::string_view root,
        std::string_view sourcePath,
        std::string_view targetPath);
    void drawResults(Registry& registry,
        Iridium::AssetManager* assetManager,
        Iridium::AssetBrowserPage& page,
        bool grid);
    void drawDetails(
        const Iridium::AssetBrowserItem*
            selected,
        Iridium::AssetManager* assetManager);
    void syncSettingsDraft(
        Iridium::AssetGuid rootGuid,
        std::string_view settingsJson);
    void drawSettingsEditor(
        const Iridium::AssetBrowserItem&
            selected,
        Iridium::AssetGuid rootGuid,
        const Iridium::CompiledTransparencyPolicy*
            cookedTransparencyPolicy);
    [[nodiscard]] bool requestCurrentReimport(
        const Iridium::AssetBrowserItem&
            selected);
    void clearThumbnailDemand();
    void selectItem(
        const Iridium::AssetBrowserItem& item);
    void openInAssetViewer(
        const Iridium::AssetBrowserItem& item);

    bool* open_ = nullptr;
    Entity* selectedEntity_ = nullptr;
    EditorUIState* uiState_ = nullptr;
    Iridium::AssetBrowserModel model_;
    const Iridium::AssetCatalog* catalog_ = nullptr;
    Iridium::AssetCatalogService* catalogService_ = nullptr;
    Iridium::AssetModelPreparationService*
        modelPreparationService_ = nullptr;
    Iridium::AssetThumbnailService*
        thumbnailService_ = nullptr;
    Iridium::AssetRuntimeService* runtimeService_ = nullptr;
    Iridium::EditorAssetDocumentService* assetDocuments_ = nullptr;
    Iridium::EditorAssetSettingsTransactionService
        settingsTransactions_;
    std::array<char, 256> search_{};
    int typeFilter_ = 0;
    int statusFilter_ = 0;
    int thumbnailSizeIndex_ = 2;
    bool showFolderPanel_ = true;
    bool showDetailsPanel_ = true;
    std::vector<RootSection> rootSections_;
    // Keyed by parent folder; non-project roots prefix the key with "<root>:".
    std::map<std::string, std::vector<std::string>>
        folderOrder_;
    std::filesystem::path folderOrderPath_;
    struct PendingFolderReorder {
        std::string root;
        std::string source;
        std::string target;
    };
    std::optional<PendingFolderReorder>
        pendingFolderReorder_;
    // Root of the folder being dragged (the payload carries only its path).
    std::string draggedFolderRoot_;
    bool foldersInitialized_ = false;
    std::optional<Iridium::AssetGuid>
        settingsGuid_;
    std::string settingsSource_;
    nlohmann::json settingsDraft_ =
        nlohmann::json::object();
    bool settingsDirty_ = false;
    struct ViewerDraft {
        uint64_t sessionSerial = 0;
        Iridium::EditorMaterialDraftHistory history;
        std::string historySource;
        std::optional<Iridium::AssetGuid> guid;
        std::string source;
        nlohmann::json settings = nlohmann::json::object();
        bool dirty = false;
        std::string diagnostic;
    };
    std::map<Iridium::AssetGuid, ViewerDraft> viewerDrafts_;
    std::string actionDiagnostic_;
    std::optional<Iridium::AssetBrowserItem>
        inspectedItem_;
    std::optional<Iridium::AssetBrowserItem>
        drawerItem_;
    struct DrawerCachedRecord {
        Iridium::AssetCatalogRecord record;
        Iridium::AssetBrowserDrawerSection section =
            Iridium::AssetBrowserDrawerSection::Unsupported;
    };
    std::optional<Iridium::AssetGuid> drawerCacheRoot_;
    std::vector<Iridium::AssetCatalogRecord> drawerCacheRecords_;
    std::vector<DrawerCachedRecord> drawerCacheContents_;
    Iridium::AssetThumbnailSourceDetail drawerCacheDetail_;
    std::vector<Iridium::AssetGuid> drawerVisibleDemandGuids_;
    bool drawerCacheClassified_ = false;
    int drawerCacheNextDetailProbeFrame_ = 0;
    std::map<Iridium::AssetGuid,
        Iridium::AssetBrowserDecoration>
        runtimeDecorations_;
    std::optional<Iridium::AssetGuid>
        detailDemandAsset_;
    std::optional<Iridium::AssetGuid>
        detailCacheRoot_;
    Iridium::AssetThumbnailSourceDetail
        detailCache_;
    std::vector<Iridium::AssetGuid>
        thumbnailDemandAssets_;
    bool thumbnailDemandCleared_ = false;
    ContentDialogMode contentDialogMode_ =
        ContentDialogMode::None;
    bool contentDialogPending_ = false;
    std::filesystem::path
        contentDialogPath_;
    std::string contentDialogRoot_;
    Iridium::AssetGuid
        contentDialogAssetGuid_;
    std::array<char, 256>
        contentDialogName_{};
};
