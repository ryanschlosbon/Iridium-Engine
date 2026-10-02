#pragma once

#include "material/TransparencyPolicy.h"

namespace Iridium {

    struct TransparencyAuthoringPresentation {
        bool cookedOpaquePrimitive = false;
        bool controlsCollapsed = false;
    };

    [[nodiscard]] constexpr TransparencyAuthoringPresentation
        describeTransparencyAuthoring(
            bool isModelPrimitive,
            const CompiledTransparencyPolicy* cookedPolicy,
            bool hasAuthoredOverride) noexcept {
        const bool cookedOpaquePrimitive =
            isModelPrimitive && cookedPolicy != nullptr &&
            cookedPolicy->resolvedClass == TransparencyClass::None;
        return {
            .cookedOpaquePrimitive = cookedOpaquePrimitive,
            .controlsCollapsed =
                cookedOpaquePrimitive && !hasAuthoredOverride,
        };
    }

} // namespace Iridium
