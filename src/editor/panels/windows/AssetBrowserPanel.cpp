#include "editor/panels/windows/AssetBrowserPanel.h"
#include "editor/TransparencyAuthoringPresentation.h"
#include "editor/MaterialParameterEditor.h"
#include "material/TransparencyDiagnostics.h"

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
#include <fstream>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

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

    void applyFolderOrder(
        std::vector<Iridium::AssetBrowserFolder>& folders,
        const std::map<std::string, std::vector<std::string>>& order,
        std::string_view parent = {}) {
        const auto found = order.find(std::string(parent));
        if (found != order.end()) {
            const auto rank = [&found](std::string_view path) {
                const auto position = std::ranges::find(found->second, path);
                return position == found->second.end()
                    ? found->second.size()
                    : static_cast<size_t>(std::distance(
                        found->second.begin(), position));
            };
            std::ranges::stable_sort(folders,
                [&rank](const Iridium::AssetBrowserFolder& lhs,
                    const Iridium::AssetBrowserFolder& rhs) {
                    const size_t lhsRank = rank(lhs.path);
                    const size_t rhsRank = rank(rhs.path);
                    return lhsRank != rhsRank ? lhsRank < rhsRank
                        : lhs.name < rhs.name;
                });
        }
        for (auto& folder : folders) {
            applyFolderOrder(folder.children, order, folder.path);
        }
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

    void itemTooltip(const char* text) {
        if (!ImGui::IsItemHovered()) return;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    bool transparencyPolicyTarget(std::string_view assetType) noexcept {
        return assetType == "iridium.material" ||
            assetType == "iridium.model-primitive";
    }

    struct TransparencyLayerBudgetPresentation {
        const char* label;
        const char* value;
        const char* guidance;
        const char* tooltip;
        uint32_t interfaceCount;
        bool runtimeAvailable;
    };

    constexpr std::array kTransparencyLayerBudgets{
        TransparencyLayerBudgetPresentation{
            "Ordinary - 2 interfaces (one shell)",
            "ordinary2",
            "Use for normal closed glass: windows, lenses, bottles, and body panels with one entry and one exit.",
            "Default production tier. Stores one closed shell (entry + exit), has the lowest layered-glass cost, and is the right choice unless diagnostics show more interfaces are needed.",
            2u,
            true,
        },
        TransparencyLayerBudgetPresentation{
            "Hero - 4 interfaces (nested glass)",
            "hero4",
            "Use for important assets with two nested shells, such as a thick windshield assembly or glass inside glass.",
            "Hero tier. Stores up to two closed shells and has roughly twice Ordinary2 peel storage. Its nested capture, local composition, and scene resolve are active; use it only where Ordinary2 visibly loses an interface.",
            4u,
            true,
        },
        TransparencyLayerBudgetPresentation{
            "Cinematic - 8 interfaces (explicit)",
            "cinematic8",
            "Reserve for complex close-up glass where four interfaces demonstrably overflow and the shot justifies the cost.",
            "Cinematic tier. Stores up to four closed shells and can cost roughly four times Ordinary2 peel storage. Its bounded capture, local composition, and scene resolve are active, but it is never selected automatically; use it only after a Hero4 overflow is measured.",
            8u,
            true,
        },
    };

    size_t transparencyLayerBudgetIndex(
        const nlohmann::json& policy) noexcept {
        const std::string quality = policy.value(
            "quality", std::string("ordinary2"));
        for (size_t index = 0;
            index < kTransparencyLayerBudgets.size(); ++index) {
            if (quality == kTransparencyLayerBudgets[index].value)
                return index;
        }
        return 0u;
    }

    bool drawTransparencyPolicyEditor(
        nlohmann::json& settings,
        const Iridium::AssetCatalogRecord& target,
        const Iridium::CompiledTransparencyPolicy*
            cookedPolicy) {
        if (!settings.contains("transparency_policies") ||
            !settings["transparency_policies"].is_object()) {
            settings["transparency_policies"] = nlohmann::json::object();
        }
        nlohmann::json& policies = settings["transparency_policies"];
        const std::string targetGuid = target.guid.toString();
        bool overrideEnabled = policies.contains(targetGuid) &&
            policies[targetGuid].is_object();
        const Iridium::TransparencyAuthoringPresentation presentation =
            Iridium::describeTransparencyAuthoring(
                target.assetType == "iridium.model-primitive",
                cookedPolicy, overrideEnabled);
        bool changed = false;

        ImGui::SeparatorText("Transparency policy");
        ImGui::TextWrapped("Target: %s", target.displayName.c_str());
        ImGui::TextWrapped("Source locator: %s", target.sourceKey.c_str());
        ImGui::TextDisabled("%s policy | stable GUID %s",
            target.assetType == "iridium.model-primitive"
                ? "Primitive override" : "Material",
            targetGuid.c_str());
        if (target.assetType == "iridium.model-primitive") {
            ImGui::TextDisabled(
                "Primitive policy takes precedence over its material policy.");
            ImGui::TextWrapped(
                "Primitive locators identify geometry, not optical material. "
                "A Thin Glass override changes routing but cannot create missing "
                "transmission or repair metallic source values.");
        }
        else {
            ImGui::TextDisabled(
                "Material policy applies to its primitives unless a primitive override exists.");
        }

        if (presentation.cookedOpaquePrimitive) {
            ImGui::TextDisabled(
                "Cooked result: Opaque (transparency options collapsed).");
            ImGui::TextDisabled(
                "This primitive has no detected transparent material behavior.");
        }

        const char* overrideLabel = presentation.cookedOpaquePrimitive
            ? "Override opaque primitive transparency"
            : "Override inherited transparency policy";
        if (ImGui::Checkbox(overrideLabel,
                &overrideEnabled)) {
            if (overrideEnabled) {
                policies[targetGuid] = {
                    { "class", "auto" },
                    { "priority", 0 },
                    { "quality", "ordinary2" },
                    { "schema_version", 1 },
                    { "thin_sheet_thickness_m", 0.0 },
                };
            }
            else {
                policies.erase(targetGuid);
            }
            changed = true;
        }
        if (presentation.cookedOpaquePrimitive) {
            itemTooltip(
                "Advanced escape hatch for intentional art direction. Enabling it reveals the full policy editor, but routing alone cannot add alpha, transmission, or repair the source material.");
            if (!overrideEnabled) return changed;
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Opaque override active: choose an explicit class below to force transparent routing.");
        }
        else {
            itemTooltip(
                "Leave this off for the safe inherited defaults: Auto classification, Ordinary2 layered quality, priority zero, and zero fabricated thin-sheet thickness.");
        }

        if (target.assetType == "iridium.model-primitive" && overrideEnabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
            ImGui::TextWrapped("This primitive's transparency policy overrides its material's policy. "
                "Disable the override above to inherit the material policy.");
            ImGui::PopStyleColor();
        }

        nlohmann::json inheritedPolicy{
            { "class", "auto" },
            { "priority", 0 },
            { "quality", "ordinary2" },
            { "schema_version", 1 },
            { "thin_sheet_thickness_m", 0.0 },
        };
        nlohmann::json& policy = overrideEnabled
            ? policies[targetGuid] : inheritedPolicy;
        policy["schema_version"] = 1;

        static constexpr const char* classLabels[]{
            "Auto (recommended)",
            "Alpha Clip - binary cutout",
            "Sorted Surface - simple blend",
            "Thin Glass - sheet/window",
            "Layered Glass - closed volume",
            "Weighted OIT - dense approximate effect",
        };
        static constexpr const char* classValues[]{
            "auto", "alpha_clip", "sorted_surface", "thin_glass",
            "layered_glass", "weighted_oit",
        };
        ImGui::BeginDisabled(!overrideEnabled);
        changed |= jsonStringControl("Transparency class", policy, "class",
            classLabels, classValues);
        itemTooltip(
            "Auto inspects coverage, transmission, volume features, and validated mesh topology. Use explicit classes only to express intentional art direction; incompatible choices diagnose and fall back safely.");

        const std::string requestedClass = policy.value(
            "class", std::string("auto"));
        if (requestedClass == "auto") {
            ImGui::TextDisabled(
                "Auto: closed volume -> Layered Glass; transmission -> Thin Glass; ordinary blend -> Sorted Surface.");
        }
        else if (requestedClass == "layered_glass") {
            ImGui::TextDisabled(
                "Requires a consistently oriented closed manifold; invalid topology falls back to Thin Glass.");
        }
        else if (requestedClass == "thin_glass") {
            ImGui::TextDisabled(
                "Thin Glass requires the linked source material to carry "
                "transmission; this class does not synthesize an optical closure.");
        }
        else if (requestedClass == "weighted_oit") {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Weighted OIT is approximate, non-refractive, and arrives in M6.7.");
        }

        size_t budgetIndex = transparencyLayerBudgetIndex(policy);
        int selectedBudget = static_cast<int>(budgetIndex);
        std::array<const char*, kTransparencyLayerBudgets.size()> budgetLabels{};
        for (size_t index = 0; index < budgetLabels.size(); ++index)
            budgetLabels[index] = kTransparencyLayerBudgets[index].label;
        if (ImGui::Combo("Layer budget", &selectedBudget,
                budgetLabels.data(), static_cast<int>(budgetLabels.size()))) {
            budgetIndex = static_cast<size_t>(selectedBudget);
            policy["quality"] = kTransparencyLayerBudgets[budgetIndex].value;
            changed = true;
        }
        itemTooltip(kTransparencyLayerBudgets[budgetIndex].tooltip);
        const TransparencyLayerBudgetPresentation& budget =
            kTransparencyLayerBudgets[budgetIndex];
        ImGui::TextWrapped("%s", budget.guidance);
        ImGui::TextDisabled("Maximum stored interfaces: %u",
            budget.interfaceCount);
        if (!budget.runtimeAvailable) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "This quality tier is authorable but not available in the active runtime.");
        }

        int priority = policy.value("priority", 0);
        if (ImGui::InputInt("Layer priority", &priority)) {
            policy["priority"] = priority;
            changed = true;
        }
        itemTooltip(
            "Higher signed priority wins when multiple layered-glass islands compete for the bounded atlas. Keep zero unless an important asset needs deterministic preference.");

        float thickness = policy.value("thin_sheet_thickness_m", 0.0f);
        if (ImGui::DragFloat("Thin-sheet thickness (m)", &thickness,
                0.001f, 0.0f, 1.0e6f, "%.4g m",
                ImGuiSliderFlags_Logarithmic |
                    ImGuiSliderFlags_AlwaysClamp)) {
            policy["thin_sheet_thickness_m"] = thickness;
            changed = true;
        }
        itemTooltip(
            "Only affects Thin Glass. Zero means an infinitesimal sheet with no fabricated absorption distance or lateral screen offset. Layered Glass measures thickness from paired geometry instead.");
        ImGui::EndDisabled();

        if (overrideEnabled) {
            if (ImGui::Button("Reset to inherited Auto")) {
                policies.erase(targetGuid);
                changed = true;
            }
            itemTooltip(
                "Removes this GUID override. The asset returns to Auto classification and Ordinary2 when layered glass is selected.");
        }
        return changed;
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
      assetDocuments_(assetDocuments),
      settingsTransactions_(
          [catalogService](Iridium::AssetGuid rootGuid,
              nlohmann::json settings) {
              if (!catalogService) {
                  throw std::runtime_error(
                      "Asset catalog service is unavailable");
              }
              return catalogService->requestUpdateSettings(
                  rootGuid, std::move(settings));
          }) {
    folderOrderPath_ = std::filesystem::path(PROJECT_ROOT_DIR) /
        "out" / "editor" / "asset-browser-folder-order.json";
    loadFolderOrder();
}

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

void AssetBrowserPanel::requestFolderMove(
    std::string_view sourceDirectory,
    const std::filesystem::path& destinationDirectory) {
    if (!catalogService_ || sourceDirectory.empty()) {
        actionDiagnostic_ = "Folder move is unavailable.";
        return;
    }
    (void)catalogService_->requestMoveFolder("project",
        std::filesystem::path(sourceDirectory), destinationDirectory);
    actionDiagnostic_ = "Folder move queued.";
}

void AssetBrowserPanel::loadFolderOrder() {
    folderOrder_.clear();
    std::ifstream input(folderOrderPath_, std::ios::binary);
    if (!input) return;
    const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
    if (!document.is_object() || document.value("schemaVersion", 0) != 1 ||
        !document.contains("parents") || !document["parents"].is_object()) {
        return;
    }
    for (const auto& [parent, children] : document["parents"].items()) {
        if (!children.is_array()) continue;
        std::vector<std::string> paths;
        for (const auto& child : children) {
            if (child.is_string()) paths.push_back(child.get<std::string>());
        }
        folderOrder_.insert_or_assign(parent, std::move(paths));
    }
}

void AssetBrowserPanel::saveFolderOrder() {
    try {
        std::filesystem::create_directories(folderOrderPath_.parent_path());
        nlohmann::json document{
            { "schemaVersion", 1 },
            { "parents", folderOrder_ },
        };
        std::ofstream output(folderOrderPath_,
            std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not open folder order file");
        output << document.dump(2) << '\n';
        if (!output) throw std::runtime_error("Could not write folder order file");
    }
    catch (const std::exception& exception) {
        actionDiagnostic_ = "Folder order could not be saved: " +
            std::string(exception.what());
    }
}

void AssetBrowserPanel::rebuildOrderedFolders() {
    orderedFolders_ = folders_;
    applyFolderOrder(orderedFolders_, folderOrder_);
}

void AssetBrowserPanel::reorderFolderBefore(
    std::string_view sourcePath, std::string_view targetPath) {
    if (sourcePath.empty() || targetPath.empty() || sourcePath == targetPath) {
        return;
    }
    const std::string sourceParent = std::filesystem::path(sourcePath)
        .parent_path().generic_string();
    const std::string targetParent = std::filesystem::path(targetPath)
        .parent_path().generic_string();
    if (sourceParent != targetParent) {
        actionDiagnostic_ =
            "Drop onto the folder to move it; reorder slots only accept siblings.";
        return;
    }
    const auto* parentFolder = sourceParent.empty()
        ? nullptr : findFolder(folders_, sourceParent);
    const auto siblings = parentFolder
        ? std::span<const Iridium::AssetBrowserFolder>(parentFolder->children)
        : std::span<const Iridium::AssetBrowserFolder>(folders_);
    std::vector<std::string> order;
    order.reserve(siblings.size());
    for (const auto& sibling : siblings) order.push_back(sibling.path);
    const auto source = std::ranges::find(order, sourcePath);
    const auto target = std::ranges::find(order, targetPath);
    if (source == order.end() || target == order.end()) return;
    const std::string moved = *source;
    order.erase(source);
    const auto targetAfterErase = std::ranges::find(order, targetPath);
    order.insert(targetAfterErase, moved);
    folderOrder_.insert_or_assign(sourceParent, std::move(order));
    rebuildOrderedFolders();
    saveFolderOrder();
    actionDiagnostic_ = "Custom folder order saved.";
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
        const bool expanded = drawerItem_ &&
            drawerItem_->record.guid == item.record.guid;
        if (ImGui::ArrowButton(
            "asset-drawer-toggle",
                expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
            if (expanded) {
                drawerItem_.reset();
                invalidateDrawerCache();
            }
            else drawerItem_ = item;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Expand model materials and primitives in the horizontal strip.");
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
    rebuildOrderedFolders();
}

void AssetBrowserPanel::drawDrawerRecord(
    const Iridium::AssetCatalogRecord& record,
    Iridium::AssetManager* assetManager,
    Iridium::AssetBrowserDrawerSection section) {
    using namespace Iridium;
    ImGui::PushID(
        record.guid.toString().c_str());
    void* thumbnail = assetManager
        ? assetManager->getEditorThumbnail(
            record.guid)
        : nullptr;
    ImGui::BeginChild("model-content-card", ImVec2(132.0f, 154.0f),
        ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    bool selected = false;
    if (thumbnail) {
        selected = ImGui::ImageButton("##content-thumbnail",
            ImTextureRef(
                reinterpret_cast<ImTextureID>(
                    thumbnail)),
            ImVec2(104.0f, 96.0f));
    }
    else {
        selected = ImGui::Button(record.assetType.c_str(),
            ImVec2(104.0f, 96.0f));
    }
    const std::string label =
        record.sourceKey.empty()
        ? record.displayName
        : std::filesystem::path(
            record.sourceKey)
            .filename().string();
    if (selected) {
        selectItem({
            .record = record,
        });
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        openInAssetViewer({.record = record});
    ImGui::TextWrapped("%s", label.c_str());
    ImGui::TextDisabled("%s", section ==
            AssetBrowserDrawerSection::TransparentPrimitive
        ? "Transparent Primitive"
        : record.assetType == "iridium.model-primitive"
            ? "Primitive" : "Material");
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
    ImGui::EndChild();
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
    ensureDrawerCache(root.record.guid, assetManager);
    ImGui::TextUnformatted(
        root.record.displayName.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Close subcar")) {
        drawerItem_.reset();
        invalidateDrawerCache();
        return;
    }
    ImGui::TextDisabled(
        "Selectable materials and model primitives - Shift+wheel or drag the scrollbar to browse");
    ImGui::Separator();
    drawerVisibleDemandGuids_.clear();
    if (drawerCacheContents_.empty()) {
        ImGui::TextDisabled("No imported materials or model primitives.");
        return;
    }
    if (ImGui::BeginChild("model-contents-horizontal",
            ImVec2(0.0f, 174.0f), ImGuiChildFlags_None,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        static constexpr std::array sections{
            Iridium::AssetBrowserDrawerSection::Material,
            Iridium::AssetBrowserDrawerSection::ModelPrimitive,
            Iridium::AssetBrowserDrawerSection::TransparentPrimitive,
        };
        bool drewSection = false;
        const float visibleMinX = ImGui::GetWindowPos().x;
        const float visibleMaxX = visibleMinX + ImGui::GetWindowSize().x;
        for (const Iridium::AssetBrowserDrawerSection section : sections) {
            const bool sectionPresent = std::ranges::any_of(drawerCacheContents_,
                [section](const DrawerCachedRecord& content) {
                    return content.section == section;
                });
            if (!sectionPresent) continue;
            if (drewSection) {
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(8.0f, 0.0f));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s",
                Iridium::assetBrowserDrawerSectionLabel(section).data());
            bool drewRecord = false;
            for (const DrawerCachedRecord& content : drawerCacheContents_) {
                if (content.section != section) continue;
                if (drewRecord) ImGui::SameLine();
                const float cardMinX = ImGui::GetCursorScreenPos().x;
                const float cardMaxX = cardMinX + 132.0f;
                if (cardMaxX >= visibleMinX && cardMinX <= visibleMaxX) {
                    drawerVisibleDemandGuids_.push_back(content.record.guid);
                    drawDrawerRecord(content.record, assetManager, section);
                }
                else {
                    // Retain horizontal layout while avoiding child windows,
                    // image commands and thumbnail lookups for clipped cards.
                    ImGui::Dummy(ImVec2(132.0f, 154.0f));
                }
                drewRecord = true;
            }
            ImGui::EndGroup();
            drewSection = true;
        }
    }
    ImGui::EndChild();
}

void AssetBrowserPanel::invalidateDrawerCache() {
    drawerCacheRoot_.reset();
    drawerCacheRecords_.clear();
    drawerCacheContents_.clear();
    drawerCacheDetail_ = {};
    drawerVisibleDemandGuids_.clear();
    drawerCacheClassified_ = false;
    drawerCacheNextDetailProbeFrame_ = 0;
}

void AssetBrowserPanel::ensureDrawerCache(
    Iridium::AssetGuid rootGuid,
    Iridium::AssetManager* assetManager) {
    using namespace Iridium;
    bool sortNeeded = false;
    if (drawerCacheRoot_ != std::optional(rootGuid)) {
        invalidateDrawerCache();
        drawerCacheRoot_ = rootGuid;
        drawerCacheRecords_ = catalog_
            ? catalog_->recordsForSourceRoot(rootGuid)
            : std::vector<AssetCatalogRecord>{};
        for (const AssetCatalogRecord& record : drawerCacheRecords_) {
            if (record.assetType == "iridium.material" ||
                record.assetType == "iridium.model-primitive") {
                drawerCacheContents_.push_back({
                    .record = record,
                    .section = assetBrowserDrawerSection(record, false),
                });
            }
        }
        sortNeeded = true;
    }

    if (thumbnailService_ &&
        ImGui::GetFrameCount() >= drawerCacheNextDetailProbeFrame_) {
        drawerCacheNextDetailProbeFrame_ = ImGui::GetFrameCount() + 30;
        AssetThumbnailSourceDetail detail =
            thumbnailService_->sourceDetail(rootGuid);
        if ((detail.available || !detail.diagnostic.empty()) &&
            (detail.sourceCookKey != drawerCacheDetail_.sourceCookKey ||
                detail.transparencyDetails !=
                    drawerCacheDetail_.transparencyDetails ||
                detail.diagnostic != drawerCacheDetail_.diagnostic)) {
            drawerCacheDetail_ = std::move(detail);
            drawerCacheClassified_ = false;
        }
    }
    if (!drawerCacheClassified_) {
        const std::shared_ptr<ModelAsset> runtimeModel = assetManager
            ? assetManager->findCookedModel(rootGuid) : nullptr;
        if (drawerCacheDetail_.available ||
            !drawerCacheDetail_.diagnostic.empty() || runtimeModel) {
            const auto transparentPolicy = [](const CompiledTransparencyPolicy& policy) {
                return policy.resolvedClass != TransparencyClass::None &&
                    policy.resolvedClass != TransparencyClass::AlphaClip;
            };
            for (DrawerCachedRecord& content : drawerCacheContents_) {
                bool transparent = false;
                if (content.record.assetType == "iridium.model-primitive") {
                    const auto detail = std::ranges::find_if(
                        drawerCacheDetail_.transparencyDetails,
                        [&](const AssetThumbnailTransparencyDetail& value) {
                            return value.assetGuid == content.record.guid;
                        });
                    if (detail != drawerCacheDetail_.transparencyDetails.end()) {
                        transparent = transparentPolicy(detail->policy);
                    }
                    else if (runtimeModel) {
                        transparent = std::ranges::any_of(runtimeModel->subMeshes,
                            [&](const SubMesh& primitive) {
                                return primitive.sourcePrimitiveGuid == content.record.guid &&
                                    transparentPolicy(primitive.transparency);
                            });
                    }
                }
                content.section = assetBrowserDrawerSection(content.record, transparent);
            }
            drawerCacheClassified_ = true;
            sortNeeded = true;
        }
    }
    if (sortNeeded) {
        std::ranges::sort(drawerCacheContents_,
            [](const DrawerCachedRecord& lhs, const DrawerCachedRecord& rhs) {
                if (lhs.section != rhs.section) return lhs.section < rhs.section;
                return lhs.record.sourceKey < rhs.record.sourceKey;
            });
    }
}

void AssetBrowserPanel::drawFolders(
    std::span<const
        Iridium::AssetBrowserFolder> folders) {
    for (const Iridium::AssetBrowserFolder&
        folder : folders) {
        ImGui::PushID(folder.path.c_str());
        ImGui::InvisibleButton("folder-order-before",
            ImVec2(-FLT_MIN, 4.0f));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    Iridium::kAssetBrowserFolderDragPayloadType.data())) {
                const auto source = Iridium::decodeAssetFolderDragPayload(
                    payload->DataType,
                    std::span(static_cast<const std::byte*>(payload->Data),
                        static_cast<size_t>(payload->DataSize)));
                if (source) pendingFolderReorder_ =
                    std::pair<std::string, std::string>{ *source, folder.path };
            }
            ImGui::EndDragDropTarget();
        }
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
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(
                Iridium::kAssetBrowserFolderDragPayloadType.data(),
                folder.path.c_str(), folder.path.size() + 1);
            ImGui::Text("Move folder: %s", folder.name.c_str());
            ImGui::TextDisabled("Drop onto a folder to nest it, or between siblings to reorder the tree.");
            ImGui::EndDragDropSource();
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
                        Iridium::kAssetBrowserFolderDragPayloadType.data())) {
                const auto source = Iridium::decodeAssetFolderDragPayload(
                    payload->DataType,
                    std::span(static_cast<const std::byte*>(payload->Data),
                        static_cast<size_t>(payload->DataSize)));
                if (source) requestFolderMove(*source, folder.path);
            }
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
        ImGui::PopID();
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
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(
            Iridium::kAssetBrowserFolderDragPayloadType.data(),
            folder.path.c_str(), folder.path.size() + 1);
        ImGui::Text("Move folder: %s", folder.name.c_str());
        ImGui::EndDragDropSource();
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
                    Iridium::kAssetBrowserFolderDragPayloadType.data())) {
            const auto source = Iridium::decodeAssetFolderDragPayload(
                payload->DataType,
                std::span(static_cast<const std::byte*>(payload->Data),
                    static_cast<size_t>(payload->DataSize)));
            if (source) requestFolderMove(*source, folder.path);
        }
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

void AssetBrowserPanel::renderAssetParameters(Iridium::AssetGuid guid,
    Iridium::AssetManager* assetManager) {
    if (!catalog_) return;
    const auto records = catalog_->recordsForGuid(guid);
    if (records.empty()) return;
    const Iridium::AssetBrowserItem item{.record = records.front()};
    const auto* document = assetDocuments_ ? assetDocuments_->active() : nullptr;
    if (!document) return;
    if (thumbnailService_ && detailDemandAsset_ != std::optional(guid)) {
        thumbnailService_->setDetailDemand(catalog_->recordsForSourceRoot(document->presentationAssetGuid), guid);
        detailDemandAsset_ = guid;
    }
    std::erase_if(viewerDrafts_, [&](const auto& entry) { return !assetDocuments_->find(entry.first); });
    auto& draft = viewerDrafts_[document->assetGuid];
    if (draft.sessionSerial != document->sessionSerial) {
        draft = {};
        draft.sessionSerial = document->sessionSerial;
    }
    const auto swapDraft = [&] {
        std::swap(settingsGuid_, draft.guid);
        std::swap(settingsSource_, draft.source);
        std::swap(settingsDraft_, draft.settings);
        std::swap(settingsDirty_, draft.dirty);
        std::swap(actionDiagnostic_, draft.diagnostic);
    };
    swapDraft();
    drawDetails(&item, assetManager);
    if (settingsGuid_ && assetManager && !detailCache_.materialSources.empty()) {
        if (draft.historySource != settingsSource_) {
            const auto published = nlohmann::json::parse(settingsSource_, nullptr, false);
            if (published.is_object()) draft.history.reset(published);
            draft.historySource = settingsSource_;
        }
        draft.history.observe(settingsDraft_, ImGui::GetActiveID(),
            ImGui::GetCurrentContext()->ActiveIdIsJustActivated);
        ImGui::SeparatorText("Unapplied edit history");
        ImGui::BeginDisabled(!draft.history.canUndo() || settingsTransactions_.pending());
        if (ImGui::Button("Undo draft edit")) {
            if (auto value = draft.history.undo()) {
                settingsDraft_ = std::move(*value);
                settingsDirty_ = settingsDraft_ != nlohmann::json::parse(settingsSource_);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!draft.history.canRedo() || settingsTransactions_.pending());
        if (ImGui::Button("Redo draft edit")) {
            if (auto value = draft.history.redo()) {
                settingsDraft_ = std::move(*value);
                settingsDirty_ = settingsDraft_ != nlohmann::json::parse(settingsSource_);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Revert unapplied viewer edits")) {
            settingsSource_ = detailCache_.settingsJson;
            settingsDraft_ = nlohmann::json::parse(settingsSource_, nullptr, false);
            settingsDirty_ = false;
            draft.history.reset(settingsDraft_);
            draft.historySource = settingsSource_;
        }
        ImGui::TextWrapped("Parameters and route-preserving policy values preview privately. Changing coverage or transparency class requires Apply. Closing this document discards unapplied edits.");
        if (settingsDraft_.is_object())
            assetManager->requestMaterialPreview(document->assetGuid, *settingsGuid_,
                detailCache_.materialSources, settingsDraft_, detailCache_.sourceCookKey,
                nlohmann::json::parse(detailCache_.settingsJson));
        const auto diagnostic = assetManager->materialPreviewDiagnostic(document->assetGuid);
        if (!diagnostic.empty()) ImGui::TextWrapped("%s", diagnostic.c_str());
        if (const auto preview = assetManager->findMaterialPreview(document->assetGuid)) {
            std::optional<Iridium::CompiledTransparencyPolicy> displayed;
            bool mixed = false;
            for (const auto& part : preview->subMeshes) {
                const auto partGuid = item.record.assetType == "iridium.material"
                    ? part.materialGuid : part.sourcePrimitiveGuid;
                if (partGuid != guid) continue;
                if (!displayed) displayed = part.transparency;
                else mixed |= *displayed != part.transparency;
            }
            if (displayed) {
                ImGui::SeparatorText("Live preview policy");
                if (mixed) ImGui::TextWrapped("Rendered pieces use different policies; showing the first piece below.");
                ImGui::Text("Class: %s | quality: %s",
                    Iridium::transparencyClassName(displayed->resolvedClass).data(),
                    Iridium::transparencyQualityName(displayed->quality).data());
                ImGui::Text("Priority: %d | thin-sheet thickness: %.4g m",
                    displayed->priority, displayed->thinSheetThicknessMeters);
            }
        }
    }
    swapDraft();
}

void AssetBrowserPanel::syncSettingsDraft(
    Iridium::AssetGuid rootGuid,
    std::string_view settingsJson) {
    if (settingsGuid_ == std::optional(rootGuid) && settingsDirty_ && settingsSource_ != settingsJson) {
        actionDiagnostic_ = "Published settings changed while this draft was edited. Draft retained; Revert to reload published settings before applying.";
        return;
    }
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
    Iridium::AssetGuid rootGuid,
    const Iridium::CompiledTransparencyPolicy*
        cookedTransparencyPolicy) {
    bool changed = false;
    if (selected.record.parentGuid) {
        ImGui::TextDisabled(
            "Edits are stored on the source asset under this stable subasset GUID.");
    }
    if (selected.record.importerId ==
        "iridium.gltf-model") {
        if (selected.record.assetType == "iridium.material") {
            ImGui::BeginDisabled(selected.record.importerVersion < 8);
            if (selected.record.importerVersion < 8)
                ImGui::TextWrapped("Material authoring requires importer 8. This asset uses the frozen legacy importer.");
            const auto source = detailCache_.materialSourceValues.find(selected.record.guid);
            if (source != detailCache_.materialSourceValues.end())
                changed |= Iridium::drawMaterialParameterEditor(settingsDraft_,
                    selected.record.guid.toString(), source->second);
            else ImGui::TextDisabled("Preparing source material parameters...");
            ImGui::EndDisabled();
        }
        if (!selected.record.parentGuid) {
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
        ImGui::TextUnformatted("Transparency renderer");
        ImGui::SameLine();
        ImGui::TextDisabled("Classified hybrid (production)");
        itemTooltip(
            "Classified hybrid uses Alpha Clip, Sorted Surface, Thin Glass, Layered Glass, and Weighted OIT according to each stable material/primitive policy. Runtime architecture is no longer an artist import setting.");
        if (settingsDraft_.value("transparency_execution_mode",
                std::string("classified")) == "legacy_two_bucket") {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Historical legacy metadata is ignored by production runtime.");
        }
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
        if (transparencyPolicyTarget(selected.record.assetType)) {
            ImGui::TextWrapped(selected.record.assetType == "iridium.material"
                ? "Material transport defaults are inherited by primitives unless a primitive has its own policy override."
                : "Primitive transport override: topology and layer budget apply to this geometry. Edit its material for color, metallic, roughness or transmission.");
            changed |= drawTransparencyPolicyEditor(
                settingsDraft_, selected.record,
                cookedTransparencyPolicy);
        }
        else {
            ImGui::SeparatorText("Transparency policy");
            ImGui::TextWrapped(
                "Select a material or model-primitive subasset to edit its stable transparency class and Layered Glass budget.");
            ImGui::TextDisabled(
                "Material policies are inherited by primitives; primitive policies are precise overrides.");
        }
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

    const nlohmann::json publishedSettings =
        nlohmann::json::parse(settingsSource_, nullptr, false);
    const bool publishedSettingsAvailable = publishedSettings.is_object();
    const bool settingsJobBlocked = !catalogService_ ||
        catalogService_->busy() || settingsTransactions_.pending();

    ImGui::SeparatorText("Asset settings history");
    const bool canUndoSettings = !settingsDirty_ &&
        publishedSettingsAvailable && !settingsJobBlocked &&
        settingsTransactions_.canUndo(rootGuid);
    const bool canRedoSettings = !settingsDirty_ &&
        publishedSettingsAvailable && !settingsJobBlocked &&
        settingsTransactions_.canRedo(rootGuid);
    ImGui::BeginDisabled(!canUndoSettings);
    if (ImGui::Button("Undo settings")) {
        const auto result = settingsTransactions_.requestUndo(
            rootGuid, publishedSettings);
        actionDiagnostic_ = result
            ? "Settings undo queued; the previous cook will be restored."
            : "Settings undo failed: " + result.diagnostic;
    }
    ImGui::EndDisabled();
    itemTooltip(
        "Restores the previous successfully cooked settings document and reimports the asset. History advances only after that cook succeeds.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedoSettings);
    if (ImGui::Button("Redo settings")) {
        const auto result = settingsTransactions_.requestRedo(
            rootGuid, publishedSettings);
        actionDiagnostic_ = result
            ? "Settings redo queued; the later cook will be restored."
            : "Settings redo failed: " + result.diagnostic;
    }
    ImGui::EndDisabled();
    itemTooltip(
        "Reapplies the next successfully cooked settings document. External settings changes invalidate divergent history instead of being overwritten.");
    ImGui::TextDisabled(
        "Undo/redo is scoped to this source asset and never dirties scene history.");

    ImGui::BeginDisabled(
        !settingsDirty_ || !publishedSettingsAvailable ||
        settingsJobBlocked);
    if (ImGui::Button(
            "Apply and reimport")) {
        const auto result = settingsTransactions_.requestApply(
            rootGuid,
            selected.record.assetType == "iridium.material"
                ? "Edit material parameters and policy"
                : transparencyPolicyTarget(selected.record.assetType)
                ? "Edit transparency policy"
                : "Edit import settings",
            publishedSettings,
            settingsDraft_);
        if (result) {
            actionDiagnostic_ = result.outcome == Iridium::
                    EditorAssetSettingsTransactionOutcome::NoChange
                ? "Settings already match the published cook."
                : "Settings update queued.";
            settingsDirty_ = false;
        }
        else {
            actionDiagnostic_ =
                "Settings update failed: " +
                result.diagnostic;
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
    if (runtimeService_) {
        const std::optional<Iridium::RuntimeAssetSnapshot>
            runtime = runtimeService_->snapshot(rootGuid);
        if (runtime) {
            ImGui::SeparatorText("Live imported model");
            ImGui::Text("State: %s | revision %llu",
                runtimeStateName(runtime->state),
                static_cast<unsigned long long>(runtime->revision));
            if (!runtime->cookKey.empty()) {
                ImGui::TextWrapped("Active cook: %s",
                    runtime->cookKey.c_str());
            }
            if (!runtime->pendingCookKey.empty()) {
                ImGui::TextWrapped("Pending cook: %s",
                    runtime->pendingCookKey.c_str());
                ImGui::TextColored(
                    ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                    "The viewport is still showing the previous revision.");
            }
            else if (runtime->state ==
                    Iridium::RuntimeAssetState::Ready) {
                ImGui::TextDisabled(
                    "This cook is published in the viewport.");
            }
        }
    }
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
            const Iridium::CompiledTransparencyPolicy*
                cookedTransparencyPolicy = nullptr;
            const auto transparency =
                std::ranges::find_if(
                    detailCache_.transparencyDetails,
                    [&selected](const Iridium::
                            AssetThumbnailTransparencyDetail& detail) {
                        return detail.assetGuid ==
                            selected->record.guid;
                    });
            if (transparency !=
                    detailCache_.transparencyDetails.end()) {
                cookedTransparencyPolicy = &transparency->policy;
                const Iridium::TransparencyDiagnosticSummary
                    transparencyDiagnostics =
                        Iridium::describeTransparencyPolicy(
                            transparency->policy);
                ImGui::SeparatorText("Cooked transparency result");
                ImGui::Text("Requested: %s",
                    Iridium::transparencyClassName(
                        transparency->policy.requestedClass).data());
                ImGui::Text("Resolved: %s | quality: %s",
                    Iridium::transparencyClassName(
                        transparency->policy.resolvedClass).data(),
                    Iridium::transparencyQualityName(
                        transparency->policy.quality).data());
                ImGui::Text("Runtime pieces: %u",
                    transparency->runtimePrimitiveCount);
                ImGui::Text("Route: %s",
                    Iridium::transparencyExecutionRouteName(
                        transparency->policy.resolvedClass).data());
                ImGui::Text("Topology: %s",
                    Iridium::transparencyTopologyDiagnosticName(
                        transparencyDiagnostics.topology).data());
                itemTooltip(
                    Iridium::transparencyTopologyDiagnosticDescription(
                        transparencyDiagnostics.topology).data());
                if (!transparency->uniformPolicy) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                        "Connected runtime pieces resolved differently.");
                }
                if ((transparency->policy.flags &
                        Iridium::CompiledTransparencyFallbackApplied) != 0u) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                        "Requested class was incompatible; the safe resolved class is active.");
                    ImGui::TextWrapped("Reason: %s",
                        Iridium::transparencyFallbackReasonDescription(
                            transparencyDiagnostics.fallback).data());
                }
                else {
                    ImGui::TextDisabled(
                        "No compatibility fallback is active.");
                }
                if (transparencyDiagnostics.policySanitized) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                        "Authored policy values were sanitized during cook.");
                }
            }
            syncSettingsDraft(
                rootGuid,
                detailCache_.settingsJson);
            if (ImGui::CollapsingHeader(
                    "Import settings",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                drawSettingsEditor(
                    *selected, rootGuid,
                    cookedTransparencyPolicy);
            }
            if (!selected->record.parentGuid && ImGui::CollapsingHeader(
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
            const Iridium::EditorAssetSettingsTransactionResult
                settingsTransaction = settingsTransactions_.complete(
                    result.serial, result.succeeded,
                    result.diagnostic);
            const bool matchedSettingsTransaction =
                settingsTransaction.outcome != Iridium::
                    EditorAssetSettingsTransactionOutcome::
                        IgnoredCompletion;
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
                invalidateDrawerCache();
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
                    if (matchedSettingsTransaction &&
                        settingsTransaction.action == Iridium::
                            EditorAssetSettingsTransactionAction::Undo) {
                        actionDiagnostic_ =
                            "Asset settings undo cooked successfully; live publication queued.";
                    }
                    else if (matchedSettingsTransaction &&
                        settingsTransaction.action == Iridium::
                            EditorAssetSettingsTransactionAction::Redo) {
                        actionDiagnostic_ =
                            "Asset settings redo cooked successfully; live publication queued.";
                    }
                    else {
                        actionDiagnostic_ =
                            "Import settings applied; model recook and live publication queued.";
                    }
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
                if (matchedSettingsTransaction &&
                    settingsTransaction.action == Iridium::
                        EditorAssetSettingsTransactionAction::Apply &&
                    settingsGuid_ == std::optional(
                        settingsTransaction.rootGuid)) {
                    settingsDirty_ = true;
                }
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
        "All types", "Meshes / Models", "Materials", "Model primitives",
        "Textures", "HDRI environments",
    };
    if (ImGui::Combo("##asset-type", &typeFilter_, typeNames,
        static_cast<int>(std::size(typeNames)))) {
        constexpr const char* types[] = {
            "", "iridium.model", "iridium.material",
            "iridium.model-primitive", "iridium.texture",
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
        std::vector<Iridium::AssetGuid> demandedAssets;
        demandedAssets.reserve(page.items.size() +
            drawerVisibleDemandGuids_.size() + 1u);
        for (const Iridium::AssetBrowserItem&
            item : page.items) {
            demandedAssets.push_back(item.record.guid);
        }
        if (inspectedItem_) {
            demandedAssets.push_back(inspectedItem_->record.guid);
        }
        const bool drawerVisible = drawerItem_.has_value();
        if (drawerVisible && catalog_) {
            ensureDrawerCache(drawerItem_->record.guid, assetManager);
            demandedAssets.insert(demandedAssets.end(),
                drawerVisibleDemandGuids_.begin(),
                drawerVisibleDemandGuids_.end());
        }
        std::ranges::sort(demandedAssets);
        const auto uniqueEnd = std::ranges::unique(demandedAssets).begin();
        demandedAssets.erase(uniqueEnd, demandedAssets.end());
        if (demandedAssets !=
            thumbnailDemandAssets_) {
            std::map<Iridium::AssetGuid, Iridium::AssetCatalogRecord> demandByGuid;
            for (const Iridium::AssetBrowserItem& item : page.items)
                demandByGuid.insert_or_assign(item.record.guid, item.record);
            if (inspectedItem_)
                demandByGuid.insert_or_assign(inspectedItem_->record.guid,
                    inspectedItem_->record);
            if (drawerVisible) {
                for (const auto& record : drawerCacheRecords_) {
                    if (std::ranges::binary_search(demandedAssets, record.guid))
                        demandByGuid.insert_or_assign(record.guid, record);
                }
            }
            std::vector<Iridium::AssetCatalogRecord> visibleRecords;
            visibleRecords.reserve(demandByGuid.size());
            for (const auto& [guid, record] : demandByGuid) {
                (void)guid;
                visibleRecords.push_back(record);
            }
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
        if (assetDocuments_ && assetDocuments_->active()) {
            // The floating viewer owns the detail worker's demand while open;
            // keep browser selection independent without cancelling that work.
        }
        else if (selected && catalog_) {
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
                            Iridium::kAssetBrowserFolderDragPayloadType.data())) {
                    const auto source = Iridium::decodeAssetFolderDragPayload(
                        payload->DataType,
                        std::span(static_cast<const std::byte*>(payload->Data),
                            static_cast<size_t>(payload->DataSize)));
                    if (source) requestFolderMove(*source, {});
                }
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
            drawFolders(orderedFolders_);
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
                if (ImGui::Button("Up", ImVec2(72.0f, 0.0f))) {
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
            if (drawerItem_) {
                ImGui::Separator();
                drawAssetDrawer(*drawerItem_, assetManager);
                ImGui::Separator();
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
    if (pendingFolderReorder_) {
        const auto [source, target] = std::move(*pendingFolderReorder_);
        pendingFolderReorder_.reset();
        reorderFolderBefore(source, target);
    }
    drawContentDialog();
    ImGui::End();
}
