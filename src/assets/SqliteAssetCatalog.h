#pragma once

#include "assets/AssetCatalog.h"

#include <filesystem>
#include <memory>

namespace Iridium {

    [[nodiscard]] std::unique_ptr<AssetCatalog> createSqliteAssetCatalog(
        const std::filesystem::path& databasePath);

} // namespace Iridium
