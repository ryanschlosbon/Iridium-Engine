#pragma once

#include "assets/AssetGuid.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    enum class AssetCatalogStatus : uint8_t {
        Ready,
        MissingSource,
        DuplicateGuid,
    };

    [[nodiscard]] const char* assetCatalogStatusName(AssetCatalogStatus status) noexcept;

    struct AssetCatalogRecord {
        AssetGuid guid;
        std::optional<AssetGuid> parentGuid;
        std::string assetType;
        std::string assetRoot;
        std::string sourcePath;
        std::string metadataPath;
        std::string sourceKey;
        std::string displayName;
        std::string importerId;
        uint32_t importerVersion = 0;
        AssetCatalogStatus status = AssetCatalogStatus::Ready;
        std::vector<std::string> tags;
        std::string diagnosticSummary;

        auto operator<=>(const AssetCatalogRecord&) const = default;
    };

    struct AssetCatalogQuery {
        std::string text;
        std::optional<std::string> sourceDirectory;
        std::optional<std::string> assetType;
        std::optional<AssetCatalogStatus> status;
        bool includeSubassets = true;
        uint32_t limit = 100;
        uint32_t offset = 0;
        bool calculateTotalMatches = true;
    };

    struct AssetCatalogQueryPage {
        std::vector<AssetCatalogRecord> records;
        std::optional<uint64_t> totalMatches;
    };

    class AssetCatalog {
    public:
        virtual ~AssetCatalog() = default;

        virtual void rebuild(
            std::span<const AssetCatalogRecord> records,
            std::span<const std::string> sourceDirectories = {}) = 0;
        [[nodiscard]] virtual std::vector<AssetCatalogRecord> recordsForGuid(
            const AssetGuid& guid) const = 0;
        [[nodiscard]] virtual std::vector<AssetCatalogRecord>
            recordsForSourceRoot(
                const AssetGuid& rootGuid) const = 0;
        [[nodiscard]] virtual AssetCatalogQueryPage query(
            const AssetCatalogQuery& query) const = 0;
        [[nodiscard]] virtual uint64_t recordCount() const = 0;
        [[nodiscard]] virtual std::vector<std::string>
            sourceDirectories() const = 0;
    };

} // namespace Iridium
