#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetMetadata.h"

#include <filesystem>
#include <span>
#include <string>
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
        std::vector<std::string> sourceDirectories;
        std::vector<AssetDiscoveryDiagnostic> diagnostics;

        [[nodiscard]] bool hasErrors() const noexcept;
    };

    [[nodiscard]] AssetDiscoveryResult discoverAssetRoots(
        std::span<const AssetRoot> roots);

} // namespace Iridium
