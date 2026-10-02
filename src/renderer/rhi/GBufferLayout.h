#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Iridium {
    enum class GBufferLayout : uint8_t {
        CanonicalReference,
        CanonicalQuality,
        CanonicalCompact,
    };

    [[nodiscard]] constexpr std::string_view gBufferLayoutName(
        GBufferLayout layout) noexcept {
        switch (layout) {
        case GBufferLayout::CanonicalReference: return "reference";
        case GBufferLayout::CanonicalQuality: return "quality";
        case GBufferLayout::CanonicalCompact: return "compact";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::optional<GBufferLayout> parseGBufferLayout(
        std::string_view value) noexcept {
        if (value == "reference" || value == "r") return GBufferLayout::CanonicalReference;
        if (value == "quality" || value == "q") return GBufferLayout::CanonicalQuality;
        if (value == "compact" || value == "c") return GBufferLayout::CanonicalCompact;
        return std::nullopt;
    }
}
