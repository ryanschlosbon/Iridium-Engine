#pragma once

#include "assets/cooker/CookTypes.h"

#include <cstdint>
#include <span>
#include <string>

namespace Iridium {

    struct CookKeyInput {
        AssetGuid assetGuid;
        std::string importerId;
        uint32_t importerImplementationVersion = 0;
        uint32_t settingsSchemaVersion = 0;
        std::span<const std::byte> canonicalSettings;
        std::string sourceContentHash;
        std::span<const AssetDependency> dependencies;
        CookTarget target;
        std::string cookerFeatureVersion;
    };

    [[nodiscard]] std::string calculateCookKey(const CookKeyInput& input);

} // namespace Iridium
