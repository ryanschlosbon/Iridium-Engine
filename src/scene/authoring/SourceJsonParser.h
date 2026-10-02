#pragma once

#include "scene/authoring/SourceComponentRegistry.h"
#include "scene/runtime/SceneDiagnostic.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace Iridium {

    struct SourceJsonParseOptions {
        size_t maximumBytes = 256ull * 1024ull * 1024ull;
    };

    struct SourceJsonParseResult {
        std::optional<SourceJson> value;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return value.has_value() && !hasSceneErrors(diagnostics);
        }
    };

    [[nodiscard]] SourceJsonParseResult parseSourceJsonStrict(
        std::string_view bytes,
        SourceJsonParseOptions options = {});

} // namespace Iridium

