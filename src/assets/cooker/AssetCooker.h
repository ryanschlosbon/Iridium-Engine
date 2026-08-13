#pragma once

#include "assets/AssetMetadata.h"
#include "assets/cooker/CookKey.h"
#include "assets/cooker/ImporterRegistry.h"
#include "assets/cooker/LocalDerivedDataCache.h"

#include <filesystem>
#include <memory>
#include <stop_token>

namespace Iridium {

    struct PreparedAssetCook {
        AssetGuid assetGuid;
        AssetCookContext context;
        std::shared_ptr<const AssetImporter> importer;
        NormalizedImportSettings settings;
        ParsedSourceAsset source;
        std::vector<AssetDependency> resolvedDependencies;
        CookTarget target;
        std::string sourceContentHash;
        std::string settingsHash;
        std::string cookKey;
        std::string cookerFeatureVersion;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return importer != nullptr && !hasCookErrors(diagnostics) &&
                settings.valid() && !hasCookErrors(source.diagnostics);
        }
    };

    [[nodiscard]] PreparedAssetCook prepareAssetCook(
        const ImporterRegistry& registry,
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& sourceRelativePath,
        const AssetMetadata& metadata,
        const CookTarget& target,
        std::string cookerFeatureVersion,
        std::stop_token stopToken = {});

    [[nodiscard]] CookedArtifactBlob buildPreparedArtifact(
        const PreparedAssetCook& prepared,
        std::stop_token stopToken = {});

    [[nodiscard]] std::shared_future<DdcRequestResult> requestPreparedCook(
        DerivedDataCache& cache,
        const PreparedAssetCook& prepared,
        std::stop_token stopToken = {});
    [[nodiscard]] std::shared_future<DdcRequestResult> requestPreparedCook(
        DerivedDataCache& cache,
        std::shared_ptr<const PreparedAssetCook> prepared,
        std::stop_token stopToken = {});

} // namespace Iridium
