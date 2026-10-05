#pragma once

// M9.2: the vendor-neutral input contract of a temporal resolve (native TAA
// now; DLSS / FSR / XeSS super-resolution providers in M9b). Backend-neutral
// data only: a provider receives these per frame from the renderer and maps
// them onto its own SDK. M9 implements provider zero (native TAA at 1:1);
// M9b adds providers and render-extent scaling without changing these fields.
//
// Conventions (all providers):
// - Colour: scene-linear ACEScg/AP1, before exposure and the output transform.
// - Motion: per pixel, current minus previous *unjittered* UV (screen space,
//   [0,1] per axis, +y down), so previousUv = uv - motion. Values with
//   |component| >= TemporalMotionNoHistory mean "no previous position".
//   Background pixels (no geometry) carry zero; providers that need camera
//   motion there reconstruct it from depth and the view matrices.
// - Depth: device depth, [0,1], standard (near 0, far 1, LESS test).
// - Jitter: this frame's sub-pixel raster offset in pixels of the render
//   extent (+x right, +y down), from a deterministic per-view sequence.
// - Exposure: the multiplier the output applies (2^EV; auto-exposure's
//   adapted value from M9.5). Providers that pre-expose use it.
// - Reactive: [0,1] per pixel, how much the current sample should replace
//   history (transparency, particles, animated emissive; M9.3). It is
//   1 - the colour input's alpha: opaque surfaces write alpha 1 and every
//   blended layer (sorted, glass, layered, WeightedOIT) multiplies it by
//   1 - its coverage, so alpha is the revealage of the opaque scene.

#include "renderer/rhi/RenderBackendConfig.h"

#include <cstdint>

#include <glm/glm.hpp>

namespace Iridium {

    inline constexpr float TemporalMotionNoHistory = 3.5f;

    enum class TemporalResolveProvider : uint8_t {
        NativeTaa = 0,   // M9: native resolution, DLAA-class
        // M9b: Dlss, Fsr, Xess (render extent may be below output extent).
    };

    struct TemporalUpscaleExtents {
        glm::uvec2 render{ 0u };   // the jittered inputs' extent
        glm::uvec2 output{ 0u };   // the resolved image's extent (== render in M9)
    };

    struct TemporalUpscaleCamera {
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        float verticalFovRadians = 1.0f;
        float frameTimeSeconds = 0.0f;   // since this view's previous turn
    };

    // One frame's resolve request. Image inputs are named by role; the
    // backend binds its own resources for them.
    struct TemporalUpscaleInputs {
        TemporalResolveProvider provider = TemporalResolveProvider::NativeTaa;
        TemporalUpscaleExtents extents{};
        glm::vec2 jitterPixels{ 0.0f };
        uint32_t jitterSequenceLength = 8;
        float exposure = 1.0f;
        float previousExposure = 1.0f;
        // A cut, teleport, resize or rebuild: discard history.
        bool resetHistory = false;
        bool reactiveMaskAvailable = false;            // M9.3
        bool transparencyCompositionMaskAvailable = false;   // M9.3
        TemporalUpscaleCamera camera{};
        // Provider-specific sharpening amount in [0,1]; 0 disables.
        float sharpness = 0.0f;
        TemporalAntiAliasingTuning nativeTaa{};
    };

} // namespace Iridium
