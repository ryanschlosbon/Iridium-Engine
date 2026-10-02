#pragma once

#include <cstdint>

namespace Iridium {

    // Shared visibility vocabulary. Conventional raster maps implement the
    // first representation now; virtual maps and ray tracing must preserve the
    // same owner/visibility semantics when they arrive.
    enum class ShadowRepresentation : uint32_t {
        ConventionalMap = 0,
        VirtualMap = 1,
        RayTraced = 2,
    };

    enum class ShadowVisibilityEncoding : uint32_t {
        Scalar = 0,
        RgbTransmittance = 1,
    };

    struct VirtualShadowPagePolicy {
        uint32_t pageSizeTexels = 128;
        uint32_t borderTexels = 4;
        uint32_t maximumPhysicalPages = 16'384;
        uint32_t maximumPageUpdatesPerFrame = 2'048;
        bool cacheStaticCasters = true;
        bool deterministicConventionalFallback = true;
    };

    struct ShadowTemporalHandoff {
        bool requiresMotionVectors = true;
        bool requiresDisocclusionMask = true;
        bool requiresReactiveMask = true;
        bool acceptsStochasticVisibility = true;
        uint32_t maximumHistoryFrames = 8;
    };

    struct ShadowRayTracingHandoff {
        ShadowVisibilityEncoding visibilityEncoding =
            ShadowVisibilityEncoding::RgbTransmittance;
        bool preservePhysicalEmitterExtent = true;
        bool preservePerLightOwnership = true;
        bool allowPerLightRepresentationSelection = true;
        bool deterministicRasterFallback = true;
    };

    enum class AmbientOcclusionMethod : uint32_t {
        Disabled = 0,
        Gtao = 1,
        Cacao = 2,
        RayTraced = 3,
    };

    struct AmbientOcclusionPolicy {
        AmbientOcclusionMethod method = AmbientOcclusionMethod::Gtao;
        float radiusMeters = 1.0f;
        float strength = 1.0f;
        uint32_t directionSamples = 8;
        uint32_t stepSamples = 4;
        bool outputBentNormal = true;
        bool applySpecularOcclusion = true;
        bool temporallyFilter = true;
        bool halfResolution = true;
    };

    static_assert(sizeof(VirtualShadowPagePolicy) == 20);
    static_assert(sizeof(ShadowTemporalHandoff) == 8);
    static_assert(sizeof(ShadowRayTracingHandoff) == 8);
    static_assert(sizeof(AmbientOcclusionPolicy) == 24);

} // namespace Iridium
