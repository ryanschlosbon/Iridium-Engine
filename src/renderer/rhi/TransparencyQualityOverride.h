#pragma once
#include "material/TransparencyPolicy.h"

namespace Iridium {
    // Execution-only quality override: preserve source/cooked author intent,
    // resolved routing, validated topology, priority and optical properties.
    [[nodiscard]] constexpr CompiledTransparencyPolicy withLayeredInterfaceBudget(
        CompiledTransparencyPolicy policy, unsigned interfaces) noexcept {
        if (policy.resolvedClass != TransparencyClass::LayeredGlass) return policy;
        switch (interfaces) {
        case 2: policy.quality = TransparencyQuality::Ordinary2; break;
        case 4: policy.quality = TransparencyQuality::Hero4; break;
        case 8: policy.quality = TransparencyQuality::Cinematic8; break;
        default: break; // Zero means authored; unsupported budgets cannot escape the bound.
        }
        return policy;
    }
}
