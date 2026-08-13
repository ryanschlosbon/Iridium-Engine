#pragma once

#include "assets/cooker/CookTypes.h"

#include <cstddef>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace Iridium {

    struct CanonicalSettingsResult {
        std::vector<std::byte> bytes;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return !hasCookErrors(diagnostics);
        }
    };

    [[nodiscard]] CanonicalSettingsResult canonicalizeSettings(
        const nlohmann::json& settings);

} // namespace Iridium
