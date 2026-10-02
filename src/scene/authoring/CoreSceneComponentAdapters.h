#pragma once

#include "scene/authoring/CoreComponentCodecs.h"
#include "scene/runtime/CoreComponentRegistry.h"

#include <string>

namespace Iridium {

    struct CoreSceneRegistryBundle {
        RuntimeComponentRegistry runtime;
        ComponentSerializerRegistry source;
        std::string diagnostic;

        [[nodiscard]] explicit operator bool() const noexcept {
            return diagnostic.empty() && runtime.isFrozen() && source.isFrozen();
        }
    };

    [[nodiscard]] CoreSceneRegistryBundle createCoreSceneRegistryBundle();

} // namespace Iridium
