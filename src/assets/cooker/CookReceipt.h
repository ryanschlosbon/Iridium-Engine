#pragma once

#include "assets/cooker/AssetCooker.h"

namespace Iridium {

    class LocalDerivedDataCache;

    [[nodiscard]] std::optional<PreparedAssetCook>
        tryPrepareAssetCookFromReceipt(
            const ImporterRegistry& registry,
            LocalDerivedDataCache& cache,
            const std::filesystem::path& assetRoot,
            const std::filesystem::path& sourceRelativePath,
            const AssetMetadata& metadata,
            const CookTarget& target,
            std::string cookerFeatureVersion,
            std::vector<CookDiagnostic>& diagnostics);

    [[nodiscard]] std::vector<CookDiagnostic> storePreparedCookReceipt(
        const LocalDerivedDataCache& cache,
        const std::filesystem::path& sourceRelativePath,
        const PreparedAssetCook& prepared);

} // namespace Iridium
