#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetMetadata.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    struct AssetRoot {
        std::string id;
        std::filesystem::path path;
    };

    struct AssetDiscoveryDiagnostic {
        AssetMetadataSeverity severity = AssetMetadataSeverity::Error;
        std::string code;
        std::string path;
        std::string message;
    };

    struct AssetDiscoveryResult {
        std::vector<AssetCatalogRecord> records;
        std::vector<AssetSourceDirectory> sourceDirectories;
        std::vector<AssetDiscoveryDiagnostic> diagnostics;

        [[nodiscard]] bool hasErrors() const noexcept;
    };

    [[nodiscard]] AssetDiscoveryResult discoverAssetRoots(
        std::span<const AssetRoot> roots);

    // The root with this id, or nullptr.
    [[nodiscard]] const AssetRoot* findAssetRoot(
        std::span<const AssetRoot> roots, std::string_view id) noexcept;

    // The directory of record.assetRoot among roots. A single-root list also
    // serves records with no root id. Throws std::runtime_error for an unknown root.
    [[nodiscard]] const std::filesystem::path& assetRootPathFor(
        std::span<const AssetRoot> roots, const AssetCatalogRecord& record);

    // True for the roots that hold project content records: "project" and
    // "local" (core/ProjectAssetRoots.h).
    [[nodiscard]] bool isProjectContentRoot(std::string_view id) noexcept;

    // The engine's registered roots: "project" (<repo>/assets) plus "local" when a
    // local asset library is configured (core/ProjectAssetRoots.h).
    [[nodiscard]] std::vector<AssetRoot> configuredProjectAssetRoots();

} // namespace Iridium
