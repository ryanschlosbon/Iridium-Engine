#pragma once

#include <cstdint>

namespace Iridium {

    // Backend-neutral project policy. Capture resolution and clip bounds remain
    // per-probe authoring settings; these values bound shared update work and
    // select the GGX convolution quality used for runtime captures.
    struct ProjectReflectionProbeSettings {
        uint64_t maximumRenderedTexelsPerFrame =
            6ull * 512ull * 512ull;
        uint32_t maximumFacesPerProbePerFrame = 6;
        uint32_t maximumCapturesInFlight = 4;
        uint32_t minimumRealtimeFramesBetweenCaptures = 30;
        uint32_t prefilterSampleCount = 256;
    };

} // namespace Iridium
