#include "editor/panels/windows/AssetBrowserPanel.h"

#include "assets/AssetCatalogService.h"
#include "assets/AssetManager.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "assets/environment/EnvironmentProduct.h"
#include "assets/model/AssetModelPreparationService.h"
#include "assets/runtime/AssetRuntimeService.h"
#include "assets/thumbnail/AssetThumbnailService.h"
#include "editor/EditorSceneActions.h"
#include "editor/EditorAssetDocumentService.h"
#include "platform/FileDialog.h"
#include "scene/Components.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cfloat>
#include <filesystem>
#include <cstring>
#include <map>
#include <span>

#include <imgui.h>

namespace {

    constexpr std::array<float, 5>
        kAssetThumbnailSizes{
            64.0f, 84.0f, 112.0f,
            148.0f, 192.0f,
        };

    const Iridium::AssetBrowserFolder*
        findFolder(
            std::span<const
                Iridium::AssetBrowserFolder> folders,
            std::string_view path) {
        for (const Iridium::AssetBrowserFolder&
                folder : folders) {
            if (folder.path == path) {
                return &folder;
            }
            if (const auto* nested =
                    findFolder(
                        folder.children, path)) {
                return nested;
            }
        }
        return nullptr;
    }

    const char* runtimeStateName(
        Iridium::RuntimeAssetState state) noexcept {
        using Iridium::RuntimeAssetState;
        switch (state) {
        case RuntimeAssetState::Missing: return "Missing";
        case RuntimeAssetState::Queued: return "Queued";
        case RuntimeAssetState::Ready: return "Ready";
        case RuntimeAssetState::ReadyWithError: return "Ready with error";
        case RuntimeAssetState::Failed: return "Failed";
        case RuntimeAssetState::Evicted: return "Evicted";
        }
        return "Unknown";
    }

    const char* dependencyTypeName(
        Iridium::AssetDependencyType type) noexcept {
        using Iridium::AssetDependencyType;
        switch (type) {
        case AssetDependencyType::SourceFile:
            return "Source";
        case AssetDependencyType::Asset:
            return "Asset";
        case AssetDependencyType::Tool:
            return "Tool";
        case AssetDependencyType::OptionalAsset:
            return "Optional asset";
        }
        return "Unknown";
    }

    bool jsonBoolControl(
        const char* label,
        nlohmann::json& settings,
        const char* key,
        bool fallback) {
        bool value =
            settings.value(key, fallback);
        if (!ImGui::Checkbox(label, &value)) {
            return false;
        }
        settings[key] = value;
        return true;
    }

    bool jsonStringControl(
        const char* label,
        nlohmann::json& settings,
        const char* key,
        std::span<const char* const> labels,
        std::span<const char* const> values,
        size_t fallbackIndex = 0) {
        const std::string current =
            settings.value(
                key,
                std::string(
                    values[fallbackIndex]));
        int selected =
            static_cast<int>(fallbackIndex);
        for (size_t index = 0;
            index < values.size(); ++index) {
            if (current == values[index]) {
                selected =
                    static_cast<int>(index);
                break;
            }
        }
        if (!ImGui::Combo(label, &selected,
                labels.data(),
                static_cast<int>(
                    labels.size()))) {
            return false;
        }
        settings[key] = values[
            static_cast<size_t>(selected)];
        return true;
    }

    uint32_t fullMipCount(uint32_t size) noexcept {
        uint32_t result = 0;
        do {
            ++result;
            size = (std::max)(size >> 1u, 1u);
        } while (size != 1u);
        return result;
    }

    uint64_t estimatedEnvironmentBytes(const nlohmann::json& settings) {
        const uint32_t radiance = settings.value("radiance_size", 1024u);
        const uint32_t irradiance = settings.value("irradiance_size", 32u);
        const uint32_t prefiltered = settings.value("prefiltered_size", 1024u);
        const uint32_t brdf = settings.value("brdf_lut_size", 256u);
        using Iridium::EnvironmentImageProductDesc;
        using Iridium::TextureFormat;
        return Iridium::environmentProductByteSize({ radiance, radiance,
                   fullMipCount(radiance), 6, TextureFormat::RGBA16_SFloat }) +
            Iridium::environmentProductByteSize({ irradiance, irradiance,
                1, 6, TextureFormat::RGBA16_SFloat }) +
            Iridium::environmentProductByteSize({ prefiltered, prefiltered,
                fullMipCount(prefiltered), 6, TextureFormat::RGBA16_SFloat }) +
            Iridium::environmentProductByteSize({ brdf, brdf,
                1, 1, TextureFormat::RG16_SFloat });
    }

    struct EnvironmentQualityRecipe {
        uint32_t radiance = 1024;
        uint32_t prefiltered = 1024;
        uint32_t samples = 1024;
    };

    constexpr std::array kEnvironmentQualityRecipes{
        EnvironmentQualityRecipe{ 512, 256, 128 },
        EnvironmentQualityRecipe{ 1024, 512, 512 },
        EnvironmentQualityRecipe{ 1024, 1024, 1024 },
        EnvironmentQualityRecipe{ 2048, 2048, 4096 },
    };

    int environmentQualityIndex(const nlohmann::json& settings) {
        for (size_t index = 0; index < kEnvironmentQualityRecipes.size(); ++index) {
            const EnvironmentQualityRecipe recipe =
                kEnvironmentQualityRecipes[index];
            if (settings.value("radiance_size", 1024u) == recipe.radiance &&
                settings.value("prefiltered_size", 1024u) == recipe.prefiltered &&
                settings.value("prefiltered_samples", 1024u) == recipe.samples &&
                settings.value("brdf_samples", 1024u) == recipe.samples)
                return static_cast<int>(index);
        }
        return static_cast<int>(kEnvironmentQualityRecipes.size());
    }

    void applyEnvironmentQuality(nlohmann::json& settings, size_t index) {
        const EnvironmentQualityRecipe recipe =
            kEnvironmentQualityRecipes.at(index);
        settings["radiance_size"] = recipe.radiance;
        settings["irradiance_size"] = 32u;
        settings["prefiltered_size"] = recipe.prefiltered;
        settings["brdf_lut_size"] = 256u;
        settings["prefiltered_samples"] = recipe.samples;
        settings["brdf_samples"] = recipe.samples;
    }

    bool powerOfTwoSetting(const char* label, nlohmann::json& settings,
        const char* key, std::span<const int> choices, int fallback) {
        const int value = settings.value(key, fallback);
        int selected = 0;
        for (size_t index = 0; index < choices.size(); ++index)
            if (choices[index] == value) selected = static_cast<int>(index);
        std::array<const char*, 8> labels{};
        std::array<std::string, 8> storage{};
        if (choices.size() > labels.size()) return false;
        for (size_t index = 0; index < choices.size(); ++index) {
            storage[index] = std::to_string(choices[index]);
            labels[index] = storage[index].c_str();
        }
        if (!ImGui::Combo(label, &selected, labels.data(),
                static_cast<int>(choices.size()))) return false;
        settings[key] = choices[static_cast<size_t>(selected)];
        return true;
    }

} // namespace

AssetBrowserPanel::AssetBrowserPanel(bool* open, Entity* selectedEntity,
    EditorUIState* uiState,
    const Iridium::AssetCatalog* catalog,
    Iridium::AssetCatalogService* catalogService,
    Iridium::AssetModelPreparationService*
        modelPreparationService,
    Iridium::AssetThumbnailService*
        thumbnailService,
    Iridium::AssetRuntimeService* runtimeService,
    Iridium::EditorAssetDocumentService* assetDocuments)
    : open_(open), selectedEntity_(selectedEntity),
      uiState_(uiState), model_(catalog),
      catalog_(catalog),
      catalogService_(catalogService),
      modelPreparationService_(modelPreparationService),
      thumbnailService_(thumbnailService),
      runtimeService_(runtimeService),
      assetDocuments_(assetDocuments) {}

void AssetBrowserPanel::openInAssetViewer(
    const Iridium::AssetBrowserItem& item) {
    if (!assetDocuments_) {
        actionDiagnostic_ = "Asset Viewer is unavailable.";
        return;
    }
    const Iridium::EditorAssetOpenResult result = assetDocuments_->open({
        .assetGuid = item.record.guid,
        .parentAssetGuid = item.record.parentGuid,
        .assetType = item.record.assetType,
        .displayName = item.record.displayName,
    });
    actionDiagnostic_ = result
        ? (result.reused ? "Asset Viewer tab activated." :
            "Asset opened in an isolated viewer.")
        : result.diagnostic;
}

void AssetBrowserPanel::selectItem(
    const Iridium::AssetBrowserItem& item) {
    if (!inspectedItem_ ||
        inspectedItem_->record.guid !=
            item.record.guid) {
        detailCacheRoot_.reset();
        detailCache_ = {};
    }
    inspectedItem_ = item;
    (void)model_.select(item.record.guid);
    if (uiState_) {
        uiState_->selectedAsset =
            Iridium::AssetDragPayload{
                .guid = item.record.guid,
                .kind =
                    Iridium::assetDragKindForType(
                        item.record.assetType),
            };
    }
}

void AssetBrowserPanel::requestAssetMove(
    Iridium::AssetGuid assetGuid,
    const std::filesystem::path&
        destinationDirectory) {
    if (!catalog_ || !catalogService_) {
        actionDiagnostic_ =
            "Project content operations are unavailable.";
        return;
    }
    const auto records =
        catalog_->recordsForGuid(assetGuid);
    const auto root =
        std::ranges::find_if(
            records,
            [](const Iridium::
                    AssetCatalogRecord& record) {
                return !record.parentGuid;
            });
    if (root == records.end()) {
        actionDiagnostic_ =
            "Imported materials and textures stay nested under their source model.";
        return;
    }
    (void)catalogService_->requestMoveAsset(
        root->guid, destinationDirectory);
    actionDiagnostic_ =
        "Asset move queued.";
}

void AssetBrowserPanel::refreshDecorations() {
    std::map<Iridium::AssetGuid,
        Iridium::AssetBrowserDecoration>
        next;
    if (!runtimeService_) {
        if (!runtimeDecorations_.empty()) {
            runtimeDecorations_.clear();
            model_.clearDecorations();
        }
        return;
    }
    for (const Iridium::RuntimeAssetSnapshot& snapshot :
        runtimeService_->snapshots()) {
        next.insert_or_assign(
            snapshot.assetGuid,
            Iridium::AssetBrowserDecoration{
            .runtimeState = snapshot.state,
            .thumbnailState = Iridium::AssetThumbnailState::Unavailable,
            .diagnostic = snapshot.diagnostic,
        });
    }
    if (next == runtimeDecorations_) {
        return;
    }
    runtimeDecorations_ =
        std::move(next);
    model_.clearDecorations();
    for (const auto& [guid, decoration] :
        runtimeDecorations_) {
        model_.setDecoration(
            guid, decoration);
    }
}

void AssetBrowserPanel::drawItem(Registry& registry,
    Iridium::AssetManager* assetManager,
    const Iridium::AssetBrowserItem& item, bool grid) {
    using namespace Iridium;
    ImGui::PushID(item.record.guid.toString().c_str());
    const AssetDragKind kind = assetDragKindForType(item.record.assetType);
    const ImVec4 color = kind == AssetDragKind::Model
        ? ImVec4(0.18f, 0.34f, 0.52f, 1.0f)
        : kind == AssetDragKind::Material
            ? ImVec4(0.45f, 0.28f, 0.16f, 1.0f)
            : kind == AssetDragKind::Texture
                ? ImVec4(0.20f, 0.42f, 0.30f, 1.0f)
                : ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    void* thumbnail = assetManager
        ? assetManager->getEditorThumbnail(
            item.record.guid)
        : nullptr;
    const float thumbnailSize =
        kAssetThumbnailSizes[
            static_cast<size_t>(
                thumbnailSizeIndex_)];
    bool clicked = false;
    if (thumbnail) {
        clicked = ImGui::ImageButton(
            "##asset-thumbnail",
            ImTextureRef(static_cast<ImTextureID>(
                reinterpret_cast<uintptr_t>(
                    thumbnail))),
            grid
                ? ImVec2(thumbnailSize,
                    thumbnailSize)
                : ImVec2(44.0f, 44.0f));
    }
    else {
        ImGui::PushStyleColor(
            ImGuiCol_Button, color);
        clicked = ImGui::Button(
            grid ? item.record.assetType.c_str()
                 : item.record.displayName.c_str(),
            grid ? ImVec2(thumbnailSize,
                    thumbnailSize)
                 : ImVec2(180.0f, 0.0f));
        ImGui::PopStyleColor();
    }
    if (clicked) {
        selectItem(item);
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        (kind == AssetDragKind::Model ||
            kind == AssetDragKind::Material)) {
        selectItem(item);
        openInAssetViewer(item);
    }
    if (ImGui::BeginPopupContextItem(
            "asset-context")) {
        if ((kind == AssetDragKind::Model ||
                kind == AssetDragKind::Material) &&
            ImGui::MenuItem("Open in Asset Viewer")) {
            openInAssetViewer(item);
        }
        ImGui::Separator();
        if (!item.record.parentGuid && item.record.status ==
                AssetCatalogStatus::Ready &&
            ImGui::MenuItem("Reimport")) {
            try {
                if (requestCurrentReimport(
                        item)) {
                    actionDiagnostic_ =
                        "Reimport queued.";
                    if (thumbnailService_) {
                        thumbnailService_
                            ->invalidate(
                                item.record.guid);
                    }
                }
                else {
                    actionDiagnostic_ =
                        "Reimport is already pending or unavailable.";
                }
            }
            catch (const std::exception&
                exception) {
                actionDiagnostic_ =
                    "Reimport failed: " +
                    std::string(
                        exception.what());
            }
        }
        if (!item.record.parentGuid && ImGui::MenuItem("Rename")) {
            openContentDialog(
                ContentDialogMode::RenameAsset,
                item.record.sourcePath,
                item.record.guid,
                std::filesystem::path(
                    item.record.sourcePath)
                    .stem().string());
        }
        if (!item.record.parentGuid && ImGui::MenuItem("Delete")) {
            openContentDialog(
                ContentDialogMode::DeleteAsset,
                item.record.sourcePath,
                item.record.guid);
        }
        ImGui::EndPopup();
    }

    if (item.assignable() && ImGui::BeginDragDropSource()) {
        const AssetDragPayloadBytes payload = encodeAssetDragPayload({
            .guid = item.record.guid,
            .kind = kind,
        });
        ImGui::SetDragDropPayload(kAssetBrowserDragPayloadType.data(),
            &payload, sizeof(payload));
        ImGui::TextUnformatted(item.record.displayName.c_str());
        ImGui::TextDisabled("%s", item.record.assetType.c_str());
        ImGui::EndDragDropSource();
    }
    if (kind == AssetDragKind::Model &&
        !item.record.parentGuid) {
        ImGui::SameLine();
        if (ImGui::ArrowButton(
                "asset-drawer-toggle",
                ImGuiDir_Right)) {
            drawerItem_ = item;
            drawerPending_ = true;
        }
    }
    if (grid) {
        ImGui::TextWrapped(
            "%s",
            item.record.displayName.c_str());
    }
    else if (thumbnail) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(
            item.record.displayName.c_str());
        ImGui::TextDisabled(
            "%s",
            item.record.assetType.c_str());
        ImGui::EndGroup();
    }
    if (item.decoration.thumbnailState ==
        Iridium::AssetThumbnailState::Queued) {
        ImGui::TextDisabled("Thumbnail...");
    }
    else if (item.decoration.thumbnailState ==
        Iridium::AssetThumbnailState::Failed) {
        ImGui::TextColored(
            ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
            "Thumbnail failed");
    }
    const std::string diagnostic = item.diagnosticSummary();
    if (!diagnostic.empty() &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", diagnostic.c_str());
    }
    ImGui::PopID();
}

void AssetBrowserPanel::rebuildFolders() {
    foldersInitialized_ = true;
    folders_ = catalog_
        ? Iridium::buildAssetBrowserFolders(
            catalog_->sourceDirectories())
        : std::vector<
            Iridium::AssetBrowserFolder>{};
}

void AssetBrowserPanel::drawDrawerRecord(
    const Iridium::AssetCatalogRecord& record,
    Iridium::AssetManager* assetManager) {
    using namespace Iridium;
    ImGui::PushID(
        record.guid.toString().c_str());
    void* thumbnail = assetManager
        ? assetManager->getEditorThumbnail(
            record.guid)
        : nullptr;
    if (thumbnail) {
        ImGui::Image(
            ImTextureRef(
                reinterpret_cast<ImTextureID>(
                    thumbnail)),
            ImVec2(44.0f, 44.0f));
        ImGui::SameLine();
    }
    const std::string label =
        record.sourceKey.empty()
        ? record.displayName
        : std::filesystem::path(
            record.sourceKey)
            .filename().string();
    if (ImGui::Selectable(
            label.c_str(), false,
            ImGuiSelectableFlags_None,
            ImVec2(220.0f, 44.0f))) {
        selectItem({
            .record = record,
        });
    }
    const AssetDragKind kind =
        assetDragKindForType(
            record.assetType);
    const AssetBrowserItem item{
        .record = record,
    };
    if (item.assignable() &&
        ImGui::BeginDragDropSource()) {
        const AssetDragPayloadBytes payload =
            encodeAssetDragPayload({
                .guid = record.guid,
                .kind = kind,
            });
        ImGui::SetDragDropPayload(
            kAssetBrowserDragPayloadType.data(),
            &payload, sizeof(payload));
        ImGui::TextUnformatted(
            label.c_str());
        ImGui::TextDisabled(
            "%s",
            record.assetType.c_str());
        ImGui::EndDragDropSource();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::drawAssetDrawer(
    const Iridium::AssetBrowserItem& root,
    Iridium::AssetManager* assetManager) {
    if (!catalog_) {
        ImGui::TextDisabled(
            "Catalog unavailable.");
        return;
    }
    const std::vector<Iridium::AssetCatalogRecord>
        records =
            catalog_->recordsForSourceRoot(
                root.record.guid);
    std::map<Iridium::AssetGuid,
        Iridium::AssetCatalogRecord>
        byGuid;
    for (const auto& record : records) {
        byGuid.emplace(record.guid, record);
    }
    const Iridium::AssetThumbnailSourceDetail
        detail = thumbnailService_
        ? thumbnailService_->sourceDetail(
            root.record.guid)
        : Iridium::
            AssetThumbnailSourceDetail{};
    ImGui::TextUnformatted(
        root.record.displayName.c_str());
    ImGui::TextDisabled(
        "Imported materials and textures");
    ImGui::Separator();
    if (!detail.available) {
        ImGui::TextDisabled(
            "Preparing associations...");
        return;
    }
    size_t materialCount = 0;
    for (const Iridium::
            AssetThumbnailAssociation&
            association :
        detail.associations) {
        if (association.parentGuid !=
            root.record.guid) {
            continue;
        }
        const auto material =
            byGuid.find(
                association.childGuid);
        if (material == byGuid.end() ||
            material->second.assetType !=
                "iridium.material") {
            continue;
        }
        ++materialCount;
        ImGui::PushID(
            material->first
                .toString().c_str());
        const std::string label =
            std::filesystem::path(
                material->second.sourceKey)
                .filename().string();
        const bool open =
            ImGui::TreeNodeEx(
                label.c_str(),
                ImGuiTreeNodeFlags_OpenOnArrow |
                ImGuiTreeNodeFlags_SpanAvailWidth);
        if (ImGui::IsItemClicked() &&
            !ImGui::IsItemToggledOpen()) {
            selectItem({
                .record =
                    material->second,
            });
        }
        const Iridium::AssetBrowserItem
            materialItem{
                .record =
                    material->second,
            };
        if (materialItem.assignable() &&
            ImGui::BeginDragDropSource()) {
            const auto payload =
                Iridium::encodeAssetDragPayload({
                    .guid = material->first,
                    .kind = Iridium::
                        AssetDragKind::Material,
                });
            ImGui::SetDragDropPayload(
                Iridium::
                    kAssetBrowserDragPayloadType
                        .data(),
                &payload, sizeof(payload));
            ImGui::TextUnformatted(
                label.c_str());
            ImGui::EndDragDropSource();
        }
        if (open) {
            size_t textureCount = 0;
            for (const Iridium::
                    AssetThumbnailAssociation&
                    textureAssociation :
                detail.associations) {
                if (textureAssociation
                        .parentGuid !=
                    material->first) {
                    continue;
                }
                const auto texture =
                    byGuid.find(
                        textureAssociation
                            .childGuid);
                if (texture == byGuid.end() ||
                    texture->second.assetType !=
                        "iridium.texture") {
                    continue;
                }
                ++textureCount;
                drawDrawerRecord(
                    texture->second,
                    assetManager);
            }
            if (textureCount == 0) {
                ImGui::TextDisabled(
                    "No textures");
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (materialCount == 0) {
        ImGui::TextDisabled(
            "No imported materials.");
    }
}

void AssetBrowserPanel::drawFolders(
    std::span<const
        Iridium::AssetBrowserFolder> folders) {
    for (const Iridium::AssetBrowserFolder&
        folder : folders) {
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanAvailWidth;
        if (model_.directory() ==
            std::optional(folder.path)) {
            flags |=
                ImGuiTreeNodeFlags_Selected;
        }
        if (folder.children.empty()) {
            flags |=
                ImGuiTreeNodeFlags_Leaf |
                ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        const bool open =
            ImGui::TreeNodeEx(
                folder.path.c_str(),
                flags, "%s",
                folder.name.c_str());
        if (ImGui::IsItemClicked() &&
            !ImGui::IsItemToggledOpen()) {
            model_.setDirectory(
                folder.path);
        }
        if (ImGui::BeginPopupContextItem(
                ("folder-context##" +
                    folder.path).c_str())) {
            if (ImGui::MenuItem(
                    "Import...")) {
                queueImportFromDialog();
            }
            if (ImGui::MenuItem(
                    "New subfolder")) {
                openContentDialog(
                    ContentDialogMode::
                        CreateFolder,
                    folder.path);
            }
            if (ImGui::MenuItem("Rename")) {
                openContentDialog(
                    ContentDialogMode::
                        RenameFolder,
                    folder.path, {},
                    folder.name);
            }
            if (ImGui::MenuItem("Delete")) {
                openContentDialog(
                    ContentDialogMode::
                        DeleteFolder,
                    folder.path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh") &&
                catalogService_) {
                (void)catalogService_
                    ->requestRefresh();
                actionDiagnostic_ =
                    "Catalog refresh queued.";
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(
                        Iridium::
                            kAssetBrowserDragPayloadType
                                .data())) {
                const auto decoded =
                    Iridium::
                        decodeAssetDragPayload(
                            payload->DataType,
                            std::span(
                                static_cast<
                                    const std::byte*>(
                                    payload->Data),
                                static_cast<size_t>(
                                    payload->DataSize)));
                if (decoded &&
                    catalogService_) {
                    requestAssetMove(
                        decoded->guid,
                        folder.path);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (open &&
            !folder.children.empty()) {
            drawFolders(folder.children);
            ImGui::TreePop();
        }
    }
}

std::span<const Iridium::AssetBrowserFolder>
    AssetBrowserPanel::currentFolders() const {
    if (!model_.directory()) {
        return folders_;
    }
    const auto* folder =
        findFolder(
            folders_,
            *model_.directory());
    return folder
        ? std::span<const
            Iridium::AssetBrowserFolder>(
                folder->children)
        : std::span<const
            Iridium::AssetBrowserFolder>{};
}

void AssetBrowserPanel::drawFolderItem(
    const Iridium::AssetBrowserFolder& folder,
    bool grid) {
    ImGui::PushID(
        folder.path.c_str());
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        ImVec4(0.56f, 0.40f,
            0.12f, 1.0f));
    const float thumbnailSize =
        kAssetThumbnailSizes[
            static_cast<size_t>(
                thumbnailSizeIndex_)];
    const bool open =
        ImGui::Button(
            grid ? "Folder"
                 : folder.name.c_str(),
            grid
                ? ImVec2(thumbnailSize,
                    thumbnailSize)
                : ImVec2(180.0f, 0.0f));
    ImGui::PopStyleColor();
    if (open) {
        model_.setDirectory(
            folder.path);
    }
    if (ImGui::BeginPopupContextItem(
            "folder-item-context")) {
        if (ImGui::MenuItem("Open")) {
            model_.setDirectory(
                folder.path);
        }
        if (ImGui::MenuItem(
                "New subfolder")) {
            openContentDialog(
                ContentDialogMode::
                    CreateFolder,
                folder.path);
        }
        if (ImGui::MenuItem("Rename")) {
            openContentDialog(
                ContentDialogMode::
                    RenameFolder,
                folder.path, {},
                folder.name);
        }
        if (ImGui::MenuItem("Delete")) {
            openContentDialog(
                ContentDialogMode::
                    DeleteFolder,
                folder.path);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    Iridium::
                        kAssetBrowserDragPayloadType
                            .data())) {
            const auto decoded =
                Iridium::
                    decodeAssetDragPayload(
                        payload->DataType,
                        std::span(
                            static_cast<
                                const std::byte*>(
                                payload->Data),
                            static_cast<size_t>(
                                payload->DataSize)));
            if (decoded) {
                requestAssetMove(
                    decoded->guid,
                    folder.path);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (grid) {
        ImGui::TextWrapped(
            "%s", folder.name.c_str());
    }
    ImGui::PopID();
}

void AssetBrowserPanel::queueImportFromDialog() {
    if (!catalogService_ ||
        catalogService_->busy()) {
        return;
    }
    constexpr std::array filters = {
        Iridium::FileDialogFilter{
            "Supported assets",
            "*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr;*.irtest"
        },
        Iridium::FileDialogFilter{
            "All files", "*.*" },
    };
    if (const auto path =
            Iridium::openFileDialog(
                filters,
                std::filesystem::path(
                    PROJECT_ROOT_DIR) /
                    "assets")) {
        (void)catalogService_
            ->requestImport(
                *path,
                "project",
                model_.directory()
                    .value_or(""));
        actionDiagnostic_ =
            "Import queued. Open Window > Console for progress and errors.";
    }
}

void AssetBrowserPanel::
    drawAssetViewContextMenu() {
    if (!ImGui::BeginPopupContextWindow(
            "asset-view-context",
            ImGuiPopupFlags_MouseButtonRight |
            ImGuiPopupFlags_NoOpenOverItems)) {
        return;
    }
    ImGui::BeginDisabled(
        !catalogService_ ||
        catalogService_->busy());
    if (ImGui::MenuItem("Import...")) {
        queueImportFromDialog();
    }
    if (ImGui::MenuItem("New Folder")) {
        openContentDialog(
            ContentDialogMode::CreateFolder,
            model_.directory().value_or(""));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Refresh")) {
        (void)catalogService_
            ->requestRefresh();
        actionDiagnostic_ =
            "Catalog refresh queued.";
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void AssetBrowserPanel::openContentDialog(
    ContentDialogMode mode,
    std::filesystem::path path,
    Iridium::AssetGuid assetGuid,
    std::string_view initialName) {
    contentDialogMode_ = mode;
    contentDialogPending_ = true;
    contentDialogPath_ =
        std::move(path);
    contentDialogAssetGuid_ =
        assetGuid;
    contentDialogName_.fill('\0');
    std::memcpy(
        contentDialogName_.data(),
        initialName.data(),
        (std::min)(
            initialName.size(),
            contentDialogName_.size() - 1));
}

void AssetBrowserPanel::drawContentDialog() {
    if (contentDialogMode_ ==
        ContentDialogMode::None) {
        return;
    }
    const char* title = "";
    switch (contentDialogMode_) {
    case ContentDialogMode::CreateFolder:
        title = "Create Asset Folder";
        break;
    case ContentDialogMode::RenameFolder:
        title = "Rename Asset Folder";
        break;
    case ContentDialogMode::RenameAsset:
        title = "Rename Asset";
        break;
    case ContentDialogMode::DeleteFolder:
        title = "Delete Asset Folder";
        break;
    case ContentDialogMode::DeleteAsset:
        title = "Delete Asset";
        break;
    case ContentDialogMode::None:
        return;
    }
    if (contentDialogPending_) {
        ImGui::OpenPopup(title);
        contentDialogPending_ = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(470.0f, 0.0f),
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            title, nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const bool deleting =
        contentDialogMode_ ==
            ContentDialogMode::DeleteFolder ||
        contentDialogMode_ ==
            ContentDialogMode::DeleteAsset;
    if (deleting) {
        ImGui::TextWrapped(
            "Delete '%s' from the physical project assets folder?",
            contentDialogPath_
                .generic_string().c_str());
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f,
                0.25f, 1.0f),
            "This removes project files and cannot be undone by the editor.");
        if (contentDialogMode_ ==
            ContentDialogMode::DeleteAsset) {
            ImGui::TextWrapped(
                "External imports are project-owned copies. "
                "The original file outside this project is never deleted.");
        }
    }
    else {
        ImGui::TextWrapped(
            "%s",
            contentDialogPath_.empty()
                ? "Assets"
                : contentDialogPath_
                    .generic_string()
                    .c_str());
        ImGui::InputText(
            contentDialogMode_ ==
                ContentDialogMode::CreateFolder
                ? "Folder name"
                : "New name",
            contentDialogName_.data(),
            contentDialogName_.size());
    }
    ImGui::Separator();
    ImGui::BeginDisabled(
        !catalogService_ ||
        catalogService_->busy() ||
        (!deleting &&
            contentDialogName_[0] == '\0'));
    if (ImGui::Button(
            deleting ? "Delete" : "Apply",
            ImVec2(110.0f, 0.0f))) {
        try {
            switch (contentDialogMode_) {
            case ContentDialogMode::CreateFolder:
                (void)catalogService_
                    ->requestCreateFolder(
                        "project",
                        contentDialogPath_,
                        contentDialogName_.data());
                break;
            case ContentDialogMode::RenameFolder:
                (void)catalogService_
                    ->requestRenameFolder(
                        "project",
                        contentDialogPath_,
                        contentDialogName_.data());
                break;
            case ContentDialogMode::RenameAsset:
                (void)catalogService_
                    ->requestRenameAsset(
                        contentDialogAssetGuid_,
                        contentDialogName_.data());
                break;
            case ContentDialogMode::DeleteFolder:
                (void)catalogService_
                    ->requestDeleteFolder(
                        "project",
                        contentDialogPath_);
                break;
            case ContentDialogMode::DeleteAsset:
                (void)catalogService_
                    ->requestDeleteAsset(
                        contentDialogAssetGuid_);
                break;
            case ContentDialogMode::None:
                break;
            }
            actionDiagnostic_ =
                "Project content operation queued.";
            contentDialogMode_ =
                ContentDialogMode::None;
            ImGui::CloseCurrentPopup();
        }
        catch (const std::exception&
            exception) {
            actionDiagnostic_ =
                "Project content operation failed: " +
                std::string(
                    exception.what());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(
            "Cancel",
            ImVec2(110.0f, 0.0f))) {
        contentDialogMode_ =
            ContentDialogMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::drawResults(
    Registry& registry,
    Iridium::AssetManager* assetManager,
    Iridium::AssetBrowserPage& page,
    bool grid) {
    const ImGuiWindowFlags childFlags =
        ImGuiWindowFlags_HorizontalScrollbar;
    if (!ImGui::BeginChild(
            "asset-results",
            ImVec2(0.0f, -34.0f),
            ImGuiChildFlags_Borders,
            childFlags)) {
        ImGui::EndChild();
        return;
    }
    const auto visibleFolders =
        currentFolders();
    if (page.items.empty() &&
        visibleFolders.empty()) {
        ImGui::TextDisabled(
            "No assets match this folder and filter.");
        drawAssetViewContextMenu();
        ImGui::EndChild();
        return;
    }

    if (grid) {
        const float thumbnailSize =
            kAssetThumbnailSizes[
                static_cast<size_t>(
                    thumbnailSizeIndex_)];
        const float cardWidth =
            thumbnailSize + 52.0f;
        const float cardHeight =
            thumbnailSize + 70.0f;
        const int columns = std::max(
            1, static_cast<int>(
                ImGui::GetContentRegionAvail().x /
                cardWidth));
        if (ImGui::BeginTable(
                "asset-grid", columns,
                ImGuiTableFlags_SizingFixedFit)) {
            const int folderCount =
                static_cast<int>(
                    visibleFolders.size());
            const int totalItems =
                folderCount +
                static_cast<int>(
                    page.items.size());
            const int rowCount =
                (totalItems + columns - 1) /
                columns;
            ImGuiListClipper clipper;
            clipper.Begin(
                rowCount, cardHeight);
            while (clipper.Step()) {
                for (int row =
                        clipper.DisplayStart;
                    row <
                        clipper.DisplayEnd;
                    ++row) {
                    ImGui::TableNextRow(
                        ImGuiTableRowFlags_None,
                        cardHeight);
                    for (int column = 0;
                        column < columns;
                        ++column) {
                        const int index =
                            row * columns +
                            column;
                        if (index >=
                                totalItems) {
                            break;
                        }
                        ImGui::TableSetColumnIndex(
                            column);
                        if (index <
                                folderCount) {
                            const auto& folder =
                                visibleFolders[
                                    static_cast<
                                        size_t>(
                                        index)];
                            ImGui::PushID(
                                folder.path.c_str());
                            ImGui::BeginChild(
                                "folder-card",
                                ImVec2(
                                    cardWidth - 8.0f,
                                    cardHeight - 6.0f),
                                ImGuiChildFlags_Borders,
                                ImGuiWindowFlags_NoScrollbar);
                            drawFolderItem(
                                folder, true);
                            ImGui::EndChild();
                            ImGui::PopID();
                        }
                        else {
                            const auto& item =
                                page.items[
                                    static_cast<
                                        size_t>(
                                        index -
                                        folderCount)];
                            ImGui::PushID(
                                item.record.guid
                                    .toString()
                                    .c_str());
                            ImGui::BeginChild(
                                "asset-card",
                                ImVec2(
                                    cardWidth - 8.0f,
                                    cardHeight - 6.0f),
                                ImGuiChildFlags_Borders,
                                ImGuiWindowFlags_NoScrollbar);
                            drawItem(
                                registry,
                                assetManager,
                                item, true);
                            ImGui::EndChild();
                            ImGui::PopID();
                        }
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    else {
        for (const auto& folder :
            visibleFolders) {
            drawFolderItem(
                folder, false);
            ImGui::Separator();
        }
        ImGuiListClipper clipper;
        clipper.Begin(
            static_cast<int>(
                page.items.size()),
            56.0f);
        while (clipper.Step()) {
            for (int index =
                    clipper.DisplayStart;
                index <
                    clipper.DisplayEnd;
                ++index) {
                drawItem(
                    registry,
                    assetManager,
                    page.items[
                        static_cast<
                            size_t>(index)],
                    false);
                ImGui::Separator();
            }
        }
    }
    drawAssetViewContextMenu();
    ImGui::EndChild();

    const uint64_t total =
        page.totalMatches.value_or(
            page.items.size());
    const uint64_t first = page.items.empty()
        ? 0 : page.offset + 1;
    const uint64_t last = std::min<uint64_t>(
        total,
        static_cast<uint64_t>(
            page.offset) +
            page.items.size());
    ImGui::BeginDisabled(
        page.offset == 0);
    if (ImGui::Button("Previous")) {
        model_.setOffset(
            page.offset >= page.limit
                ? page.offset - page.limit
                : 0);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(
        last >= total);
    if (ImGui::Button("Next")) {
        model_.setOffset(
            page.offset + page.limit);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(
        "%llu-%llu of %llu",
        static_cast<unsigned long long>(
            first),
        static_cast<unsigned long long>(
            last),
        static_cast<unsigned long long>(
            total));
}

void AssetBrowserPanel::syncSettingsDraft(
    Iridium::AssetGuid rootGuid,
    std::string_view settingsJson) {
    if (settingsGuid_ ==
            std::optional(rootGuid) &&
        settingsSource_ == settingsJson) {
        return;
    }
    const nlohmann::json parsed =
        nlohmann::json::parse(
            settingsJson.begin(),
            settingsJson.end(),
            nullptr, false);
    settingsGuid_ = rootGuid;
    settingsSource_ = settingsJson;
    settingsDraft_ =
        parsed.is_object()
        ? parsed
        : nlohmann::json::object();
    settingsDirty_ = false;
}

void AssetBrowserPanel::drawSettingsEditor(
    const Iridium::AssetBrowserItem& selected,
    Iridium::AssetGuid rootGuid) {
    bool changed = false;
    if (selected.record.parentGuid) {
        ImGui::TextDisabled(
            "Inherited from the source asset.");
    }
    if (selected.record.importerId ==
        "iridium.gltf-model") {
        changed |= jsonBoolControl(
            "Generate missing tangents",
            settingsDraft_,
            "generate_missing_tangents",
            true);
        changed |= jsonBoolControl(
            "Recalculate normals",
            settingsDraft_,
            "recalculate_normals",
            false);
        changed |= jsonBoolControl(
            "Recalculate tangents",
            settingsDraft_,
            "recalculate_tangents",
            false);
        changed |= jsonBoolControl(
            "Reverse winding",
            settingsDraft_,
            "reverse_winding",
            false);
        ImGui::TextDisabled(
            "glTF winding is converted automatically. Reverse winding is a repair override for malformed exports.");
        float importScale =
            settingsDraft_.value(
                "import_scale", 1.0f);
        if (ImGui::DragFloat(
                "Import scale",
                &importScale,
                0.01f,
                1.0e-6f,
                1.0e6f,
                "%.6g",
                ImGuiSliderFlags_Logarithmic |
                ImGuiSliderFlags_AlwaysClamp)) {
            settingsDraft_[
                "import_scale"] =
                    importScale;
            changed = true;
        }
        ImGui::TextDisabled(
            "Uniformly bakes source units into render, bounds, and RT geometry.");
        bool required = true;
        ImGui::BeginDisabled();
        ImGui::Checkbox(
            "Bake node transforms",
            &required);
        ImGui::Checkbox(
            "Preserve ray-tracing geometry",
            &required);
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "The disabled settings are required by the M3 geometry contract.");
    }
    else if (selected.record.importerId ==
        "iridium.texture.directxtex") {
        static constexpr const char*
            semanticLabels[] = {
                "Color", "Normal", "Scalar",
                "HDR color", "Data",
            };
        static constexpr const char*
            semanticValues[] = {
                "color", "normal", "scalar",
                "hdr_color", "data",
            };
        static constexpr const char*
            qualityLabels[] = {
                "Iteration", "Production",
            };
        static constexpr const char*
            qualityValues[] = {
                "iteration", "production",
            };
        static constexpr const char*
            mipLabels[] = {
                "Full chain",
                "Preserve source", "None",
            };
        static constexpr const char*
            mipValues[] = {
                "full_chain",
                "preserve_source", "none",
            };
        static constexpr const char*
            alphaLabels[] = {
                "Opaque", "Straight",
                "Coverage preserving",
            };
        static constexpr const char*
            alphaValues[] = {
                "opaque", "straight",
                "coverage",
            };
        static constexpr const char*
            colorLabels[] = {
                "sRGB", "Linear",
            };
        static constexpr const char*
            colorValues[] = {
                "srgb", "linear",
            };
        changed |= jsonStringControl(
            "Semantic", settingsDraft_,
            "semantic", semanticLabels,
            semanticValues);
        changed |= jsonStringControl(
            "Compression", settingsDraft_,
            "quality", qualityLabels,
            qualityValues);
        changed |= jsonStringControl(
            "Mip policy", settingsDraft_,
            "mip_policy", mipLabels,
            mipValues);
        changed |= jsonStringControl(
            "Alpha", settingsDraft_,
            "alpha_mode", alphaLabels,
            alphaValues);
        changed |= jsonStringControl(
            "Texture view", settingsDraft_,
            "view_color_space",
            colorLabels, colorValues);
        changed |= jsonBoolControl(
            "Flip normal green",
            settingsDraft_,
            "flip_green", false);
        changed |= jsonBoolControl(
            "Reconstruct normal Z",
            settingsDraft_,
            "reconstruct_normal_z",
            true);
        float threshold =
            settingsDraft_.value(
                "alpha_coverage_threshold",
                0.5f);
        if (ImGui::SliderFloat(
                "Coverage threshold",
                &threshold, 0.01f, 0.99f,
                "%.2f")) {
            settingsDraft_[
                "alpha_coverage_threshold"] =
                    threshold;
            changed = true;
        }
    }
    else if (selected.record.importerId ==
        "iridium.environment.hdri") {
        static constexpr const char* qualityLabels[]{
            "Iteration", "High", "Ultra", "Cinematic", "Custom",
        };
        int quality = environmentQualityIndex(settingsDraft_);
        if (ImGui::Combo("Reflection quality", &quality, qualityLabels,
                static_cast<int>(std::size(qualityLabels)))) {
            if (quality >= 0 && quality <
                static_cast<int>(kEnvironmentQualityRecipes.size()))
                applyEnvironmentQuality(settingsDraft_,
                    static_cast<size_t>(quality));
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controls the cooked sky and GGX reflection product. "
                "Ultra is the high-end project default; Cinematic is intended "
                "for a small number of hero environments.");
        }

        static constexpr std::array skySizes{ 256, 512, 1024, 2048, 4096 };
        static constexpr std::array reflectionSizes{ 128, 256, 512, 1024, 2048 };
        static constexpr std::array sampleCounts{
            64, 128, 256, 512, 1024, 2048, 4096, 8192,
        };
        if (ImGui::CollapsingHeader("Custom quality settings")) {
            changed |= powerOfTwoSetting("Sky resolution", settingsDraft_,
                "radiance_size", skySizes, 1024);
            changed |= powerOfTwoSetting("Reflection resolution", settingsDraft_,
                "prefiltered_size", reflectionSizes, 1024);
            changed |= powerOfTwoSetting("GGX filter samples", settingsDraft_,
                "prefiltered_samples", sampleCounts, 1024);
            changed |= powerOfTwoSetting("BRDF integration samples", settingsDraft_,
                "brdf_samples", sampleCounts, 1024);
            ImGui::TextDisabled("Changing any value makes this a Custom recipe.");
        }
        if (settingsDraft_.value("prefiltered_size", 1024u) < 1024u) {
            if (ImGui::Button("Upgrade to Ultra")) {
                applyEnvironmentQuality(settingsDraft_, 2u);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stages the 1024-face high-end recipe. Apply and "
                    "reimport below to replace the current cooked product.");
        }
        const double mebibytes = static_cast<double>(
            estimatedEnvironmentBytes(settingsDraft_)) / (1024.0 * 1024.0);
        ImGui::Text("Estimated resident memory: %.1f MiB", mebibytes);
        ImGui::TextDisabled("Smooth materials use the full reflection resolution; "
            "rough materials automatically use filtered lower mips.");
        if (mebibytes > 640.0)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "Exceeds the editor's 640 MiB per-environment publication limit.");
        else if (mebibytes > 256.0)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Hero setting: hot replacement temporarily needs both products.");
    }
    else if (selected.record.importerId ==
        "iridium.fixture-text") {
        static constexpr const char*
            transformLabels[] = {
                "Identity", "Uppercase",
            };
        static constexpr const char*
            transformValues[] = {
                "identity", "uppercase",
            };
        changed |= jsonStringControl(
            "Transform", settingsDraft_,
            "transform",
            transformLabels,
            transformValues);
        int repeat = settingsDraft_.value(
            "repeat", 1);
        if (ImGui::SliderInt(
                "Repeat", &repeat, 1, 16)) {
            settingsDraft_["repeat"] =
                repeat;
            changed = true;
        }
    }
    else {
        ImGui::TextDisabled(
            "This importer has no editable settings UI.");
    }
    settingsDirty_ =
        settingsDirty_ || changed;

    ImGui::BeginDisabled(
        !settingsDirty_ ||
        !catalogService_ ||
        catalogService_->busy());
    if (ImGui::Button(
            "Apply and reimport")) {
        try {
            (void)catalogService_
                ->requestUpdateSettings(
                    rootGuid,
                    settingsDraft_);
            actionDiagnostic_ =
                "Settings update queued.";
            settingsDirty_ = false;
        }
        catch (const std::exception&
            exception) {
            actionDiagnostic_ =
                "Settings update failed: " +
                std::string(
                    exception.what());
        }
    }
    ImGui::EndDisabled();
}

bool AssetBrowserPanel::requestCurrentReimport(
    const Iridium::AssetBrowserItem& selected) {
    const Iridium::AssetGuid rootGuid =
        selected.record.parentGuid.value_or(
            selected.record.guid);
    if (modelPreparationService_ &&
        catalog_) {
        const std::vector<
            Iridium::AssetCatalogRecord>
            records =
                catalog_->recordsForGuid(
                    rootGuid);
        const auto root =
            std::ranges::find_if(
                records,
                [](const Iridium::
                    AssetCatalogRecord&
                    record) {
                    return !record.parentGuid &&
                        record.assetType ==
                            "iridium.model" &&
                        record.status ==
                            Iridium::
                            AssetCatalogStatus::
                                Ready;
                });
        if (root != records.end()) {
            return modelPreparationService_
                ->request(*root);
        }
    }
    return runtimeService_ &&
        runtimeService_->requestReimport(
            rootGuid);
}

void AssetBrowserPanel::drawDetails(
    const Iridium::AssetBrowserItem*
        selected,
    Iridium::AssetManager* assetManager) {
    if (!selected) {
        ImGui::TextDisabled(
            "Select an asset to inspect it.");
        return;
    }
    void* thumbnail = assetManager
        ? assetManager->
            getEditorDetailThumbnail(
                selected->record.guid)
        : nullptr;
    if (!thumbnail && assetManager) {
        thumbnail =
            assetManager->getEditorThumbnail(
                selected->record.guid);
    }
    const float previewSize =
        std::clamp(
            ImGui::GetContentRegionAvail().x,
            96.0f, 240.0f);
    if (thumbnail) {
        const float offset =
            (ImGui::GetContentRegionAvail().x -
                previewSize) * 0.5f;
        if (offset > 0.0f) {
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() +
                offset);
        }
        ImGui::Image(
            ImTextureRef(
                reinterpret_cast<
                    ImTextureID>(thumbnail)),
            ImVec2(previewSize,
                previewSize));
    }
    else {
        ImGui::TextDisabled(
            "Thumbnail is being prepared...");
    }
    ImGui::Separator();
    ImGui::TextWrapped(
        "%s",
        selected->record.displayName.c_str());
    ImGui::TextDisabled(
        "%s",
        selected->record.guid
            .toString().c_str());
    ImGui::TextWrapped(
        "%s",
        selected->record.sourcePath.c_str());
    ImGui::Separator();
    ImGui::Text(
        "Importer: %s @ %u",
        selected->record.importerId.c_str(),
        selected->record.importerVersion);
    if (!selected->record.sourceKey.empty()) {
        ImGui::TextWrapped(
            "Source key: %s",
            selected->record.sourceKey.c_str());
    }
    if (!selected->record.tags.empty()) {
        std::string tags;
        for (const std::string& tag :
            selected->record.tags) {
            if (!tags.empty()) tags += ", ";
            tags += tag;
        }
        ImGui::TextWrapped(
            "Tags: %s", tags.c_str());
    }
    if (selected->decoration.runtimeState) {
        ImGui::Text(
            "Runtime: %s",
            runtimeStateName(
                *selected->decoration
                    .runtimeState));
    }
    const std::string diagnostic =
        selected->diagnosticSummary();
    if (!diagnostic.empty()) {
        ImGui::TextWrapped(
            "%s", diagnostic.c_str());
    }

    const Iridium::AssetGuid rootGuid =
        selected->record.parentGuid
            .value_or(
                selected->record.guid);
    if (thumbnailService_) {
        if (detailCacheRoot_ !=
                std::optional(rootGuid)) {
            detailCacheRoot_ =
                rootGuid;
            detailCache_ = {};
        }
        if (!detailCache_.available &&
            detailCache_.diagnostic.empty()) {
            detailCache_ =
                thumbnailService_
                    ->sourceDetail(rootGuid);
        }
        if (detailCache_.available) {
            syncSettingsDraft(
                rootGuid,
                detailCache_.settingsJson);
            if (ImGui::CollapsingHeader(
                    "Import settings",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                drawSettingsEditor(
                    *selected, rootGuid);
            }
            if (ImGui::CollapsingHeader(
                    "Dependencies")) {
                if (detailCache_
                        .dependencies.empty()) {
                    ImGui::TextDisabled(
                        "%s",
                        detailCache_.diagnostic.empty()
                        ? "No cooked dependencies."
                        : detailCache_.diagnostic.c_str());
                }
                for (const Iridium::
                        AssetDependency& dependency :
                    detailCache_
                        .dependencies) {
                    std::string identity =
                        dependency.location;
                    if (identity.empty() &&
                        dependency.assetGuid) {
                        identity =
                            dependency.assetGuid
                                ->toString();
                    }
                    ImGui::BulletText(
                        "%s: %s",
                        dependencyTypeName(
                            dependency.type),
                        identity.c_str());
                }
            }
        }
        else {
            ImGui::TextDisabled(
                "Loading import settings and dependencies...");
        }
    }

    const bool canReimport =
        (runtimeService_ ||
            modelPreparationService_) &&
        selected->record.status ==
            Iridium::AssetCatalogStatus::Ready;
    ImGui::BeginDisabled(!canReimport);
    if (ImGui::Button("Reimport")) {
        try {
            const bool queued =
                requestCurrentReimport(
                    *selected);
            if (queued) {
                actionDiagnostic_ =
                    "Reimport queued.";
                if (thumbnailService_) {
                    thumbnailService_
                        ->invalidate(
                            rootGuid);
                }
                detailCacheRoot_.reset();
                detailCache_ = {};
            }
            else {
                actionDiagnostic_ =
                    "Reimport is already pending or unavailable.";
            }
        }
        catch (const std::exception&
            exception) {
            actionDiagnostic_ =
                "Reimport failed: " +
                std::string(exception.what());
        }
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped(
        "Drag models into the Scene Viewport, Scene Hierarchy, or Mesh component. "
        "The Create menu adds the currently selected model.");
}

void AssetBrowserPanel::clearThumbnailDemand() {
    if (!thumbnailService_ ||
        thumbnailDemandCleared_) {
        return;
    }
    thumbnailService_->setDemand(
        std::span<const
            Iridium::AssetCatalogRecord>{});
    thumbnailService_->setDetailDemand(
        std::span<const
            Iridium::AssetCatalogRecord>{},
        std::nullopt);
    detailDemandAsset_.reset();
    detailCacheRoot_.reset();
    detailCache_ = {};
    thumbnailDemandAssets_.clear();
    thumbnailDemandCleared_ = true;
}

void AssetBrowserPanel::OnImGuiRender(Registry& registry,
    Iridium::AssetManager* assetManager) {
    if (!open_ || !*open_) {
        clearThumbnailDemand();
        return;
    }
    if (!ImGui::Begin(
            "Asset Browser", open_,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        clearThumbnailDemand();
        return;
    }
    thumbnailDemandCleared_ = false;

    if (catalogService_) {
        for (const Iridium::AssetCatalogJobResult& result :
            catalogService_->takeResults()) {
            if (result.succeeded) {
                foldersInitialized_ = false;
                thumbnailDemandAssets_.clear();
                inspectedItem_.reset();
                if (thumbnailService_) {
                    thumbnailService_->
                        setDetailDemand(
                            std::span<const
                                Iridium::
                                    AssetCatalogRecord>{},
                            std::nullopt);
                }
                detailDemandAsset_.reset();
                detailCacheRoot_.reset();
                detailCache_ = {};
                model_.invalidate();
                if (result.kind ==
                    Iridium::AssetCatalogJobKind::Import) {
                    actionDiagnostic_ =
                        "Imported " +
                        result.sourcePath
                            .filename().string() +
                        (result.assetGuid
                            ? " as " +
                                result.assetGuid
                                    ->toString() +
                                "."
                            : ".");
                }
                else if (result.kind ==
                    Iridium::AssetCatalogJobKind::
                        UpdateSettings) {
                    actionDiagnostic_ =
                        "Import settings applied and reimported.";
                    if (result.assetGuid &&
                        thumbnailService_) {
                        thumbnailService_->invalidate(
                            *result.assetGuid);
                    }
                    if (result.assetGuid &&
                        modelPreparationService_ &&
                        catalog_) {
                        try {
                            const auto records =
                                catalog_->recordsForGuid(
                                    *result.assetGuid);
                            const auto model =
                                std::ranges::find_if(
                                    records,
                                    [](const Iridium::
                                        AssetCatalogRecord&
                                        record) {
                                        return !record.parentGuid &&
                                            record.assetType ==
                                                "iridium.model" &&
                                            record.status ==
                                                Iridium::
                                                AssetCatalogStatus::
                                                    Ready;
                                    });
                            if (model != records.end() &&
                                !modelPreparationService_
                                    ->pending(
                                        model->guid)) {
                                (void)modelPreparationService_
                                    ->request(*model);
                            }
                        }
                        catch (const std::exception&
                            exception) {
                            actionDiagnostic_ +=
                                " Runtime preparation could not be queued: " +
                                std::string(
                                    exception.what());
                        }
                    }
                    settingsSource_.clear();
                }
                else {
                    actionDiagnostic_ =
                        result.kind ==
                            Iridium::AssetCatalogJobKind::
                                Refresh
                        ? "Catalog refreshed."
                        : "Project content updated.";
                    model_.setDirectory(
                        std::nullopt);
                }
            }
            else {
                actionDiagnostic_ =
                    result.cancelled
                    ? "Asset catalog operation cancelled."
                    : "Asset catalog operation failed: " +
                        result.diagnostic +
                        " See Window > Console for details.";
            }
        }
    }
    const bool catalogBusy =
        catalogService_ && catalogService_->busy();
    ImGui::BeginDisabled(!catalogService_ || catalogBusy);
    if (ImGui::Button("Import...")) {
        queueImportFromDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Folder")) {
        openContentDialog(
            ContentDialogMode::CreateFolder,
            model_.directory().value_or(""));
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        (void)catalogService_->requestRefresh();
        actionDiagnostic_ = "Catalog refresh queued.";
    }
    ImGui::EndDisabled();
    if (catalogBusy) {
        ImGui::SameLine();
        ImGui::TextDisabled("Working...");
    }
    ImGui::SameLine();
    if (ImGui::Button("Folders")) {
        if (ImGui::GetWindowWidth() <
            760.0f) {
            ImGui::OpenPopup(
                "asset-folders-popup");
        }
        else {
            showFolderPanel_ =
                !showFolderPanel_;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Details")) {
        if (ImGui::GetWindowWidth() <
            1120.0f) {
            ImGui::OpenPopup(
                "asset-details-popup");
        }
        else {
            showDetailsPanel_ =
                !showDetailsPanel_;
        }
    }

    ImGui::Separator();
    bool grid =
        model_.layout() ==
        Iridium::AssetBrowserLayout::Grid;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint(
            "##asset-search",
            "Search names, paths, tags, and GUIDs",
            search_.data(),
            search_.size())) {
        model_.setText(
            search_.data());
    }
    if (ImGui::BeginTable(
            "asset-toolbar", 4,
            ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn(
            "Type");
        ImGui::TableSetupColumn(
            "Status");
        ImGui::TableSetupColumn(
            "Zoom");
        ImGui::TableSetupColumn(
            "Layout");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-FLT_MIN);
    constexpr const char* typeNames[] = {
        "All types", "Meshes / Models", "Materials", "Textures", "HDRI environments",
    };
    if (ImGui::Combo("##asset-type", &typeFilter_, typeNames,
        static_cast<int>(std::size(typeNames)))) {
        constexpr const char* types[] = {
            "", "iridium.model", "iridium.material", "iridium.texture",
            "iridium.environment",
        };
        model_.setAssetType(typeFilter_ == 0 ? std::nullopt
            : std::optional<std::string>(types[typeFilter_]));
    }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    constexpr const char* statusNames[] = {
        "All status", "Ready", "Missing source", "Duplicate GUID",
    };
    if (ImGui::Combo("##asset-status", &statusFilter_, statusNames,
        static_cast<int>(std::size(statusNames)))) {
        model_.setStatus(statusFilter_ == 0 ? std::nullopt
            : std::optional(static_cast<Iridium::AssetCatalogStatus>(
                statusFilter_ - 1)));
    }
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginDisabled(
            !grid ||
            thumbnailSizeIndex_ == 0);
        if (ImGui::Button("-")) {
            --thumbnailSizeIndex_;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Zoom %d",
            thumbnailSizeIndex_ + 1);
        ImGui::SameLine();
        ImGui::BeginDisabled(
            !grid ||
            thumbnailSizeIndex_ ==
                static_cast<int>(
                    kAssetThumbnailSizes.size()) -
                    1);
        if (ImGui::Button("+")) {
            ++thumbnailSizeIndex_;
        }
        ImGui::EndDisabled();
        ImGui::TableSetColumnIndex(3);
    if (ImGui::Button(grid ? "List" : "Grid")) {
        model_.setLayout(grid ? Iridium::AssetBrowserLayout::List
                              : Iridium::AssetBrowserLayout::Grid);
        grid = !grid;
    }
        ImGui::EndTable();
    }

    if (!foldersInitialized_) {
        rebuildFolders();
    }
    refreshDecorations();
    Iridium::AssetBrowserPage& page =
        model_.refresh();
    if (thumbnailService_) {
        std::map<Iridium::AssetGuid,
            Iridium::AssetCatalogRecord>
            demandByGuid;
        for (const Iridium::AssetBrowserItem&
            item : page.items) {
            demandByGuid.insert_or_assign(
                item.record.guid,
                item.record);
        }
        if (inspectedItem_) {
            demandByGuid.insert_or_assign(
                inspectedItem_->record.guid,
                inspectedItem_->record);
        }
        const bool drawerVisible =
            drawerItem_ &&
            (drawerPending_ ||
                ImGui::IsPopupOpen(
                    "asset-contents-drawer"));
        if (drawerVisible && catalog_) {
            const auto drawerRecords =
                catalog_->recordsForSourceRoot(
                    drawerItem_->record.guid);
            for (const auto& record :
                drawerRecords) {
                if (record.guid ==
                        drawerItem_->record.guid ||
                    record.assetType ==
                        "iridium.material" ||
                    record.assetType ==
                        "iridium.texture") {
                    demandByGuid.insert_or_assign(
                        record.guid, record);
                }
            }
        }
        std::vector<Iridium::AssetGuid>
            demandedAssets;
        std::vector<
            Iridium::AssetCatalogRecord>
            visibleRecords;
        demandedAssets.reserve(
            demandByGuid.size());
        visibleRecords.reserve(
            demandByGuid.size());
        for (const auto& [guid, record] :
            demandByGuid) {
            demandedAssets.push_back(guid);
            visibleRecords.push_back(record);
        }
        if (demandedAssets !=
            thumbnailDemandAssets_) {
            thumbnailService_->setDemand(
                visibleRecords);
            thumbnailDemandAssets_ =
                std::move(demandedAssets);
        }
        std::vector<Iridium::AssetGuid>
            visibleGuids;
        visibleGuids.reserve(
            page.items.size());
        for (const auto& item :
            page.items) {
            visibleGuids.push_back(
                item.record.guid);
        }
        const auto thumbnailInfo =
            thumbnailService_->info(
                visibleGuids);
        for (size_t itemIndex = 0;
            itemIndex < page.items.size();
            ++itemIndex) {
            Iridium::AssetBrowserItem& item =
                page.items[itemIndex];
            const Iridium::AssetThumbnailInfo&
                info =
                    thumbnailInfo[itemIndex];
            switch (info.status) {
            case Iridium::AssetThumbnailStatus::
                    Pending:
            case Iridium::AssetThumbnailStatus::
                    Prepared:
                item.decoration.thumbnailState =
                    Iridium::AssetThumbnailState::
                        Queued;
                break;
            case Iridium::AssetThumbnailStatus::
                    Ready:
                item.decoration.thumbnailState =
                    Iridium::AssetThumbnailState::
                        Ready;
                break;
            case Iridium::AssetThumbnailStatus::
                    Failed:
                item.decoration.thumbnailState =
                    Iridium::AssetThumbnailState::
                        Failed;
                if (!info.diagnostic.empty() &&
                    item.decoration.diagnostic.find(
                        info.diagnostic) ==
                        std::string::npos) {
                    if (!item.decoration
                            .diagnostic.empty()) {
                        item.decoration
                            .diagnostic += '\n';
                    }
                    item.decoration.diagnostic +=
                        info.diagnostic;
                }
                break;
            case Iridium::AssetThumbnailStatus::
                    Unavailable:
                break;
            }
            model_.setDecoration(
                item.record.guid,
                item.decoration);
        }
    }
    const Iridium::AssetBrowserItem*
        selected = inspectedItem_
        ? &*inspectedItem_
        : nullptr;
    if (thumbnailService_) {
        if (selected && catalog_) {
            if (detailDemandAsset_ !=
                    std::optional(
                        selected->record.guid)) {
                const Iridium::AssetGuid root =
                    selected->record.parentGuid
                        .value_or(
                            selected->record.guid);
                const auto records =
                    catalog_
                        ->recordsForSourceRoot(
                            root);
                thumbnailService_->
                    setDetailDemand(
                        records,
                        selected->record.guid);
                detailDemandAsset_ =
                    selected->record.guid;
            }
        }
        else if (detailDemandAsset_) {
            thumbnailService_->
                setDetailDemand(
                    std::span<const
                        Iridium::AssetCatalogRecord>{},
                    std::nullopt);
            detailDemandAsset_.reset();
        }
    }

    const auto drawFolderPane = [this]() {
            ImGui::TextUnformatted("Folders");
            ImGui::Separator();
            const float statusReserve =
                actionDiagnostic_.empty()
                ? 0.0f : 58.0f;
            if (ImGui::BeginChild(
                    "folder-tree",
                    ImVec2(0.0f,
                        statusReserve > 0.0f
                        ? -statusReserve
                        : 0.0f))) {
            const bool allSelected =
                !model_.directory();
            if (ImGui::Selectable(
                    "All Assets",
                    allSelected)) {
                model_.setDirectory(
                    std::nullopt);
            }
            if (ImGui::BeginPopupContextItem(
                    "asset-root-context")) {
                if (ImGui::MenuItem(
                        "Import...")) {
                    queueImportFromDialog();
                }
                if (ImGui::MenuItem(
                        "New folder")) {
                    openContentDialog(
                        ContentDialogMode::
                            CreateFolder);
                }
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(
                            Iridium::
                                kAssetBrowserDragPayloadType
                                    .data())) {
                    const auto decoded =
                        Iridium::
                            decodeAssetDragPayload(
                                payload->DataType,
                                std::span(
                                    static_cast<
                                        const std::byte*>(
                                        payload->Data),
                                    static_cast<size_t>(
                                        payload->DataSize)));
                    if (decoded &&
                        catalogService_) {
                        requestAssetMove(
                            decoded->guid, {});
                    }
                }
                ImGui::EndDragDropTarget();
            }
            drawFolders(folders_);
            }
            ImGui::EndChild();
            if (!actionDiagnostic_.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped(
                    "%s",
                    actionDiagnostic_.c_str());
            }
    };

    ImGui::Separator();
    const float availableWidth =
        ImGui::GetContentRegionAvail().x;
    const bool inlineFolders =
        showFolderPanel_ &&
        availableWidth >= 760.0f;
    const bool inlineDetails =
        showDetailsPanel_ &&
        availableWidth >=
            (inlineFolders
                ? 1120.0f : 840.0f);
    const int layoutColumns =
        1 + (inlineFolders ? 1 : 0) +
        (inlineDetails ? 1 : 0);
    constexpr ImGuiTableFlags layoutFlags =
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable(
            "asset-browser-layout",
            layoutColumns,
            layoutFlags,
            ImVec2(0.0f, -1.0f))) {
        if (inlineFolders) {
            ImGui::TableSetupColumn(
                "Folders",
                ImGuiTableColumnFlags_WidthFixed,
                std::min(210.0f,
                    availableWidth * 0.22f));
        }
        ImGui::TableSetupColumn(
            "Assets",
            ImGuiTableColumnFlags_WidthStretch,
            1.0f);
        if (inlineDetails) {
            ImGui::TableSetupColumn(
                "Details",
                ImGuiTableColumnFlags_WidthFixed,
                std::min(330.0f,
                    availableWidth * 0.30f));
        }
        ImGui::TableNextRow();

        int layoutColumn = 0;
        if (inlineFolders) {
            ImGui::TableSetColumnIndex(
                layoutColumn++);
            if (ImGui::BeginChild(
                    "asset-folders",
                    ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_None)) {
                drawFolderPane();
            }
            ImGui::EndChild();
        }

        ImGui::TableSetColumnIndex(
            layoutColumn++);
        if (ImGui::BeginChild(
                "asset-content",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_None)) {
            const std::string location =
                model_.directory()
                ? "Assets / " +
                    *model_.directory()
                : "All Assets";
            if (model_.directory()) {
                if (ImGui::Button("Up")) {
                    const std::string parent =
                        std::filesystem::path(
                            *model_.directory())
                            .parent_path()
                            .generic_string();
                    model_.setDirectory(
                        parent.empty()
                            ? std::nullopt
                            : std::optional(
                                parent));
                }
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(
                location.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(
                "(%llu records)",
                static_cast<
                    unsigned long long>(
                    page.totalMatches.value_or(
                        page.items.size())));
            if (!inlineFolders &&
                !actionDiagnostic_.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "  |  %s",
                    actionDiagnostic_.c_str());
            }
            drawResults(
                registry, assetManager,
                page, grid);
        }
        ImGui::EndChild();

        if (inlineDetails) {
            ImGui::TableSetColumnIndex(
                layoutColumn);
            if (ImGui::BeginChild(
                    "asset-details",
                    ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_None)) {
                ImGui::TextUnformatted(
                    "Asset Details");
                ImGui::Separator();
                drawDetails(
                    selected,
                    assetManager);
            }
            ImGui::EndChild();
        }
        ImGui::EndTable();
    }
    ImGui::SetNextWindowSize(
        ImVec2(360.0f, 520.0f),
        ImGuiCond_Appearing);
    if (ImGui::BeginPopup(
            "asset-folders-popup")) {
        drawFolderPane();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(
        ImVec2(390.0f, 620.0f),
        ImGuiCond_Appearing);
    if (ImGui::BeginPopup(
            "asset-details-popup")) {
        ImGui::TextUnformatted(
            "Asset Details");
        ImGui::Separator();
        drawDetails(
            selected, assetManager);
        ImGui::EndPopup();
    }
    if (drawerPending_) {
        ImGui::OpenPopup(
            "asset-contents-drawer");
        drawerPending_ = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(360.0f, 520.0f),
        ImGuiCond_Appearing);
    if (ImGui::BeginPopup(
            "asset-contents-drawer")) {
        if (drawerItem_) {
            drawAssetDrawer(
                *drawerItem_, assetManager);
        }
        ImGui::EndPopup();
    }
    drawContentDialog();
    ImGui::End();
}
