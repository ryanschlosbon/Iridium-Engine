#pragma once

#include <cstdint>

namespace Iridium {

    // Authoring enums shared by the reflection-probe component and the backend
    // capture contract. Values are serialized; do not renumber.
    enum class ReflectionProbeShape : int32_t {
        Sphere = 0,
        Box = 1,
    };

    enum class ReflectionProbeUpdateMode : int32_t {
        Baked = 0,
        OnDemand = 1,
        Realtime = 2,
    };

    enum class ReflectionProbeParallaxMode : int32_t {
        None = 0,
        BoxProjection = 1,
    };

} // namespace Iridium
