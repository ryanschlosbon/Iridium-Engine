#pragma once

#include "assets/AssetBrowserModel.h"
#include "assets/thumbnail/AssetThumbnailService.h"
#include "editor/panels/EditorPanel.h"
#include "editor/EditorUIState.h"

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
    void rebuildFolders();
    void drawFolders(
        std::span<const
            Iridium::AssetBrowserFolder> folders);
    void drawFolderItem(
        const Iridium::AssetBrowserFolder& folder,
        bool grid);
    [[nodiscard]] std::span<const
        Iridium::AssetBrowserFolder>
        currentFolders() const;
    void queueImportFromDialog();
    void drawAssetViewContextMenu();
    void drawAssetDrawer(
        const Iridium::AssetBrowserItem& root,
        Iridium::AssetManager* assetManager);
    void drawDrawerRecord(
        const Iridium::AssetCatalogRecord& record,
        Iridium::AssetManager* assetManager);
    void openContentDialog(
        ContentDialogMode mode,
        std::filesystem::path path = {},
        Iridium::AssetGuid assetGuid = {},
        std::string_view initialName = {});
    void drawContentDialog();
    void requestAssetMove(
        Iridium::AssetGuid assetGuid,
        const std::filesystem::path&
            destinationDirectory);
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
        Iridium::AssetGuid rootGuid);
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
    std::array<char, 256> search_{};
    int typeFilter_ = 0;
    int statusFilter_ = 0;
    int thumbnailSizeIndex_ = 2;
    bool showFolderPanel_ = true;
    bool showDetailsPanel_ = true;
    std::vector<Iridium::AssetBrowserFolder>
        folders_;
    bool foldersInitialized_ = false;
    std::optional<Iridium::AssetGuid>
        settingsGuid_;
    std::string settingsSource_;
    nlohmann::json settingsDraft_ =
        nlohmann::json::object();
    bool settingsDirty_ = false;
    std::string actionDiagnostic_;
    std::optional<Iridium::AssetBrowserItem>
        inspectedItem_;
    std::optional<Iridium::AssetBrowserItem>
        drawerItem_;
    bool drawerPending_ = false;
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
    Iridium::AssetGuid
        contentDialogAssetGuid_;
    std::array<char, 256>
        contentDialogName_{};
};
