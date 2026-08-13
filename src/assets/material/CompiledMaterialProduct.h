#pragma once

#include "assets/cooker/CookTypes.h"
#include "material/MaterialCompiler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kCompiledMaterialProductSchemaVersion = 1;

    struct CompiledMaterialReadResult {
        std::optional<CompiledMaterial> material;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return material.has_value() && !hasCookErrors(diagnostics);
        }
    };

    // This product is the lossless CPU-side M2 closure contract. It intentionally
    // contains no GPU descriptor indices or renderer-owned residency state.
    [[nodiscard]] std::vector<std::byte> serializeCompiledMaterial(
        const CompiledMaterial& material);
    [[nodiscard]] CompiledMaterialReadResult readCompiledMaterial(
        std::span<const std::byte> bytes);

} // namespace Iridium
