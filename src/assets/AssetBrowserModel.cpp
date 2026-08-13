#include "assets/AssetBrowserModel.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>

namespace Iridium {

    namespace {

        constexpr uint32_t kAssetDragPayloadMagic = 0x50415249u;
        constexpr uint16_t kAssetDragPayloadSchemaVersion = 1;
        constexpr uint32_t kMaximumPageSize = 1000;

        void sortFolders(
            std::vector<AssetBrowserFolder>& folders) {
            std::ranges::sort(
                folders,
                [](const AssetBrowserFolder& lhs,
                    const AssetBrowserFolder& rhs) {
                    return lhs.name < rhs.name;
                });
            for (AssetBrowserFolder& folder :
                folders) {
                sortFolders(folder.children);
            }
        }

    } // namespace

    bool AssetBrowserItem::assignable() const noexcept {
        const AssetDragKind kind =
            assetDragKindForType(
                record.assetType);
        return record.status ==
                AssetCatalogStatus::Ready &&
            kind != AssetDragKind::Unknown &&
            (kind != AssetDragKind::Model ||
                !record.parentGuid);
    }

    std::string AssetBrowserItem::diagnosticSummary() const {
        if (record.diagnosticSummary.empty()) return decoration.diagnostic;
        if (decoration.diagnostic.empty()) return record.diagnosticSummary;
        return record.diagnosticSummary + "\n" + decoration.diagnostic;
    }

    AssetDragKind assetDragKindForType(std::string_view assetType) noexcept {
        if (assetType == "iridium.model") return AssetDragKind::Model;
        if (assetType == "iridium.material") return AssetDragKind::Material;
        if (assetType == "iridium.texture") return AssetDragKind::Texture;
        if (assetType == "iridium.environment") return AssetDragKind::Environment;
        if (assetType == "iridium.baked-lighting")
            return AssetDragKind::BakedLighting;
        return AssetDragKind::Unknown;
    }

    std::vector<AssetBrowserFolder>
        buildAssetBrowserFolders(
            std::span<const std::string> directories) {
        std::vector<AssetBrowserFolder> roots;
        for (const std::string& directory :
            directories) {
            std::vector<AssetBrowserFolder>*
                level = &roots;
            std::filesystem::path accumulated;
            for (const auto& component :
                std::filesystem::path(directory)) {
                if (component == "." ||
                    component == "/" ||
                    component.empty()) {
                    continue;
                }
                accumulated /= component;
                const std::string name =
                    component.generic_string();
                auto found = std::ranges::find_if(
                    *level,
                    [&name](
                        const AssetBrowserFolder&
                            folder) {
                        return folder.name == name;
                    });
                if (found == level->end()) {
                    level->push_back({
                        .name = name,
                        .path =
                            accumulated
                                .generic_string(),
                    });
                    found = std::prev(
                        level->end());
                }
                level = &found->children;
            }
        }
        sortFolders(roots);
        return roots;
    }

    AssetDragPayloadBytes encodeAssetDragPayload(
        const AssetDragPayload& payload) noexcept {
        return {
            .magic = kAssetDragPayloadMagic,
            .schemaVersion = kAssetDragPayloadSchemaVersion,
            .kind = static_cast<uint16_t>(payload.kind),
            .guid = payload.guid.bytes(),
        };
    }

    std::optional<AssetDragPayload> decodeAssetDragPayload(
        std::string_view payloadType,
        std::span<const std::byte> bytes,
        std::optional<AssetDragKind> expectedKind) noexcept {
        if (payloadType != kAssetBrowserDragPayloadType ||
            bytes.size() != sizeof(AssetDragPayloadBytes)) {
            return std::nullopt;
        }
        AssetDragPayloadBytes encoded;
        std::memcpy(&encoded, bytes.data(), sizeof(encoded));
        if (encoded.magic != kAssetDragPayloadMagic ||
            encoded.schemaVersion != kAssetDragPayloadSchemaVersion) {
            return std::nullopt;
        }
        const AssetDragKind kind =
            static_cast<AssetDragKind>(encoded.kind);
        if (kind < AssetDragKind::Model ||
            kind > AssetDragKind::BakedLighting ||
            (expectedKind && kind != *expectedKind)) {
            return std::nullopt;
        }
        const AssetGuid guid(encoded.guid);
        if (guid.isNil() || guid.version() != 7 ||
            !guid.hasRfc4122Variant()) {
            return std::nullopt;
        }
        return AssetDragPayload{
            .guid = guid,
            .kind = kind,
        };
    }

    AssetBrowserModel::AssetBrowserModel(const AssetCatalog* catalog)
        : catalog_(catalog) {
        query_.limit = 100;
        query_.includeSubassets = false;
    }

    void AssetBrowserModel::setCatalog(const AssetCatalog* catalog) noexcept {
        catalog_ = catalog;
        page_ = {};
        selectedGuid_.reset();
        dirty_ = true;
    }

    void AssetBrowserModel::setText(std::string text) {
        if (query_.text == text) return;
        query_.text = std::move(text);
        query_.offset = 0;
        dirty_ = true;
    }

    void AssetBrowserModel::setDirectory(
        std::optional<std::string> sourceDirectory) {
        if (query_.sourceDirectory == sourceDirectory) return;
        query_.sourceDirectory = std::move(sourceDirectory);
        query_.offset = 0;
        dirty_ = true;
    }

    void AssetBrowserModel::setAssetType(
        std::optional<std::string> assetType) {
        if (query_.assetType == assetType) return;
        query_.assetType = std::move(assetType);
        query_.offset = 0;
        dirty_ = true;
    }

    void AssetBrowserModel::setStatus(
        std::optional<AssetCatalogStatus> status) noexcept {
        if (query_.status == status) return;
        query_.status = status;
        query_.offset = 0;
        dirty_ = true;
    }

    void AssetBrowserModel::setPageSize(uint32_t pageSize) noexcept {
        const uint32_t clamped =
            std::clamp(
                pageSize, 1u,
                kMaximumPageSize);
        if (query_.limit == clamped) return;
        query_.limit = clamped;
        query_.offset -= query_.offset % query_.limit;
        dirty_ = true;
    }

    void AssetBrowserModel::setOffset(uint32_t offset) noexcept {
        if (query_.offset == offset) return;
        query_.offset = offset;
        dirty_ = true;
    }

    void AssetBrowserModel::setLayout(AssetBrowserLayout layout) noexcept {
        layout_ = layout;
    }

    const std::string& AssetBrowserModel::text() const noexcept {
        return query_.text;
    }

    const std::optional<std::string>&
        AssetBrowserModel::directory() const noexcept {
        return query_.sourceDirectory;
    }

    const std::optional<std::string>&
        AssetBrowserModel::assetType() const noexcept {
        return query_.assetType;
    }

    std::optional<AssetCatalogStatus>
        AssetBrowserModel::status() const noexcept {
        return query_.status;
    }

    uint32_t AssetBrowserModel::pageSize() const noexcept {
        return query_.limit;
    }

    uint32_t AssetBrowserModel::offset() const noexcept {
        return query_.offset;
    }

    AssetBrowserLayout AssetBrowserModel::layout() const noexcept {
        return layout_;
    }

    void AssetBrowserModel::setDecoration(
        AssetGuid guid, AssetBrowserDecoration decoration) {
        if (const auto found =
                decorations_.find(guid);
            found != decorations_.end() &&
            found->second == decoration) {
            return;
        }
        decorations_.insert_or_assign(guid, std::move(decoration));
        dirty_ = true;
    }

    void AssetBrowserModel::clearDecoration(AssetGuid guid) {
        if (decorations_.erase(guid) != 0) {
            dirty_ = true;
        }
    }

    void AssetBrowserModel::clearDecorations() noexcept {
        if (decorations_.empty()) return;
        decorations_.clear();
        dirty_ = true;
    }

    AssetBrowserPage&
        AssetBrowserModel::refresh() {
        if (!dirty_) return page_;
        dirty_ = false;
        page_ = {
            .offset = query_.offset,
            .limit = query_.limit,
        };
        if (!catalog_) {
            selectedGuid_.reset();
            return page_;
        }
        const AssetCatalogQueryPage records = catalog_->query(query_);
        page_.totalMatches = records.totalMatches;
        page_.items.reserve(records.records.size());
        for (const AssetCatalogRecord& record : records.records) {
            AssetBrowserItem item{
                .record = record,
            };
            if (const auto found = decorations_.find(record.guid);
                found != decorations_.end()) {
                item.decoration = found->second;
            }
            page_.items.push_back(std::move(item));
        }
        if (selectedGuid_ && !selectedItem()) selectedGuid_.reset();
        return page_;
    }

    const AssetBrowserPage& AssetBrowserModel::page() const noexcept {
        return page_;
    }

    bool AssetBrowserModel::select(AssetGuid guid) noexcept {
        const bool present = std::ranges::any_of(
            page_.items,
            [guid](const AssetBrowserItem& item) {
                return item.record.guid == guid;
            });
        if (!present) return false;
        selectedGuid_ = guid;
        return true;
    }

    void AssetBrowserModel::clearSelection() noexcept {
        selectedGuid_.reset();
    }

    std::optional<AssetGuid>
        AssetBrowserModel::selectedGuid() const noexcept {
        return selectedGuid_;
    }

    const AssetBrowserItem*
        AssetBrowserModel::selectedItem() const noexcept {
        if (!selectedGuid_) return nullptr;
        const auto found = std::ranges::find_if(
            page_.items,
            [guid = *selectedGuid_](const AssetBrowserItem& item) {
                return item.record.guid == guid;
            });
        return found == page_.items.end() ? nullptr : &*found;
    }

} // namespace Iridium
