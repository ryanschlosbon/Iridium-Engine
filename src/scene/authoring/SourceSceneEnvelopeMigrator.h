#pragma once

#include "scene/authoring/SourceJsonParser.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Iridium {

    struct SourceSceneEnvelopeMigrationResult {
        std::optional<SourceJson> value;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return value.has_value() && !hasSceneErrors(diagnostics);
        }
    };

    [[nodiscard]] SourceSceneEnvelopeMigrationResult migrateSourceSceneV0(
        std::string_view bytes,
        std::span<const uint8_t, 16> sceneAssetGuid,
        SourceJsonParseOptions options = {});

} // namespace Iridium
