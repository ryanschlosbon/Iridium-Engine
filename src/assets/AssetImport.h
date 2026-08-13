#pragma once

#include "assets/AssetMetadata.h"
#include "assets/cooker/ImporterRegistry.h"

#include <filesystem>
#include <stop_token>
#include <vector>

namespace Iridium {

    struct AssetImportResult {
        std::filesystem::path sourcePath;
        std::filesystem::path metadataPath;
        AssetMetadata metadata;
        size_t preservedSubassets = 0;
        size_t createdSubassets = 0;
        size_t dependencyCount = 0;
        std::vector<CookDiagnostic> diagnostics;
    };

    // Parses source and updates its sidecar without cooking or publishing runtime
    // resources. Existing GUIDs survive deterministic reimport and subasset matching.
    [[nodiscard]] AssetImportResult importAssetSource(
        const std::filesystem::path& sourcePath,
        const ImporterRegistry& importers,
        std::stop_token stopToken = {});
    [[nodiscard]] AssetImportResult updateAssetImportSettings(
        const std::filesystem::path& sourcePath,
        const ImporterRegistry& importers,
        nlohmann::json settings,
        std::stop_token stopToken = {});

    [[nodiscard]] ImporterRegistry createStandardAssetImporterRegistry();

} // namespace Iridium
