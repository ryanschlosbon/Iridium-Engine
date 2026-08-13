#pragma once

#include "assets/AssetCatalog.h"
#include "assets/runtime/AssetRuntimePublisher.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr std::string_view kAssetBrowserDragPayloadType =
        "IRIDIUM_ASSET_GUID_V1";

    enum class AssetBrowserLayout : uint8_t {
        Grid,
        List,
    };

    enum class AssetThumbnailState : uint8_t {
        Unavailable,
        Queued,
        Ready,
        Failed,
    };

    enum class AssetDragKind : uint16_t {
        Unknown,
        Model,
        Material,
        Texture,
        Environment,
        BakedLighting,
    };

    struct AssetBrowserDecoration {
        std::optional<RuntimeAssetState> runtimeState;
        AssetThumbnailState thumbnailState = AssetThumbnailState::Unavailable;
        std::string diagnostic;

        auto operator<=>(const AssetBrowserDecoration&) const = default;
    };

    struct AssetBrowserItem {
        AssetCatalogRecord record;
        AssetBrowserDecoration decoration;

        [[nodiscard]] bool assignable() const noexcept;
        [[nodiscard]] std::string diagnosticSummary() const;

        auto operator<=>(const AssetBrowserItem&) const = default;
    };

    struct AssetBrowserPage {
        std::vector<AssetBrowserItem> items;
        std::optional<uint64_t> totalMatches;
        uint32_t offset = 0;
        uint32_t limit = 0;
    };

    struct AssetBrowserFolder {
        std::string name;
        std::string path;
        std::vector<AssetBrowserFolder> children;

        auto operator<=>(const AssetBrowserFolder&) const = default;
    };

    [[nodiscard]] std::vector<AssetBrowserFolder>
        buildAssetBrowserFolders(
            std::span<const std::string> directories);

    struct AssetDragPayload {
        AssetGuid guid;
        AssetDragKind kind = AssetDragKind::Unknown;

        auto operator<=>(const AssetDragPayload&) const = default;
    };

    struct AssetDragPayloadBytes {
        uint32_t magic = 0;
        uint16_t schemaVersion = 0;
        uint16_t kind = 0;
        AssetGuid::Bytes guid{};

        auto operator<=>(const AssetDragPayloadBytes&) const = default;
    };

    [[nodiscard]] AssetDragKind assetDragKindForType(
        std::string_view assetType) noexcept;
    [[nodiscard]] AssetDragPayloadBytes encodeAssetDragPayload(
        const AssetDragPayload& payload) noexcept;
    [[nodiscard]] std::optional<AssetDragPayload> decodeAssetDragPayload(
        std::string_view payloadType,
        std::span<const std::byte> bytes,
        std::optional<AssetDragKind> expectedKind = std::nullopt) noexcept;

    class AssetBrowserModel {
    public:
        explicit AssetBrowserModel(const AssetCatalog* catalog = nullptr);

        void setCatalog(const AssetCatalog* catalog) noexcept;
        void setText(std::string text);
        void setDirectory(
            std::optional<std::string> sourceDirectory);
        void setAssetType(std::optional<std::string> assetType);
        void setStatus(std::optional<AssetCatalogStatus> status) noexcept;
        void setPageSize(uint32_t pageSize) noexcept;
        void setOffset(uint32_t offset) noexcept;
        void setLayout(AssetBrowserLayout layout) noexcept;

        [[nodiscard]] const std::string& text() const noexcept;
        [[nodiscard]] const std::optional<std::string>&
            directory() const noexcept;
        [[nodiscard]] const std::optional<std::string>& assetType() const noexcept;
        [[nodiscard]] std::optional<AssetCatalogStatus> status() const noexcept;
        [[nodiscard]] uint32_t pageSize() const noexcept;
        [[nodiscard]] uint32_t offset() const noexcept;
        [[nodiscard]] AssetBrowserLayout layout() const noexcept;

        void setDecoration(
            AssetGuid guid, AssetBrowserDecoration decoration);
        void clearDecoration(AssetGuid guid);
        void clearDecorations() noexcept;
        void invalidate() noexcept {
            dirty_ = true;
        }

        [[nodiscard]] AssetBrowserPage& refresh();
        [[nodiscard]] const AssetBrowserPage& page() const noexcept;

        [[nodiscard]] bool select(AssetGuid guid) noexcept;
        void clearSelection() noexcept;
        [[nodiscard]] std::optional<AssetGuid> selectedGuid() const noexcept;
        [[nodiscard]] const AssetBrowserItem* selectedItem() const noexcept;

    private:
        const AssetCatalog* catalog_ = nullptr;
        AssetCatalogQuery query_;
        AssetBrowserLayout layout_ = AssetBrowserLayout::Grid;
        std::map<AssetGuid, AssetBrowserDecoration> decorations_;
        AssetBrowserPage page_;
        std::optional<AssetGuid> selectedGuid_;
        bool dirty_ = true;
    };

} // namespace Iridium
