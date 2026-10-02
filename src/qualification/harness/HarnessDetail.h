#pragma once

// Internal helpers shared by the qualification harness translation units.

#include <cstdint>
#include <string_view>

#include "renderer/color/OutputTransformConfig.h"

namespace Iridium {

    struct ModelAsset;

    // Transparent-submesh classification used by --validate-ordinary2-fallback.
    struct Ordinary2FallbackModelStats {
        uint32_t transparentSubmeshes = 0;
        uint32_t requestedLayeredCandidates = 0;
        uint32_t fallbackThinGlassSubmeshes = 0;
        uint32_t fallbackFlaggedSubmeshes = 0;
        uint32_t topologyRequiredSubmeshes = 0;
        uint32_t layeredGlassSubmeshes = 0;
    };

    [[nodiscard]] Ordinary2FallbackModelStats ordinary2FallbackModelStats(
        const ModelAsset& model) noexcept;

    // Output-transform names recorded in capture and run metadata.
    [[nodiscard]] std::string_view outputOperatorName(
        OutputTransformOperator value);
    [[nodiscard]] std::string_view gamutMappingName(
        OutputTransformOperator value);
    [[nodiscard]] std::string_view transformId(OutputTransformOperator value,
        Color::OutputTransport transport);

} // namespace Iridium
