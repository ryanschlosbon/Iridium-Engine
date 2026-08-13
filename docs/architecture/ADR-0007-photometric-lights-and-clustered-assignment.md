# ADR-0007: Photometric Lights and Shared Clustered Assignment

- Status: Accepted; staged implementation belongs to M5.1 through M5.4
- Date: 2026-08-08
- Owners: Renderer, scene system, editor, asset cooker, and RHI

## Context

The M4 Light component persisted one ambiguous `intensity` value and an RGB triple
whose transfer function and primaries were unstated. It round-tripped through source
and cooked scenes but was not consumed by the renderer. M5 needs physically named
light inputs, stable migration, one extraction representation, and one clustered
assignment product shared by deferred/material-resolve and complex-forward paths.

ADR-0002 requires scene-linear ACEScg/AP1 lighting until the final output transform.
ADR-0006 already rejects independent deferred and forward light schedulers. This ADR
freezes the authoring, migration, color, intensity, and assignment conventions needed
to implement those decisions without embedding Vulkan details in scene data.

## Decision

1. `iridium.component.light` retains its stable ID and `LGT1` section. Source and
   cooked versions advance to 2.
2. Persistent fields are type, `colorLinearRec709`, `illuminanceLux`,
   `luminousIntensityCandela`, `rangeMeters`, `sourceRadiusMeters`,
   `innerConeDegrees`, `outerConeDegrees`, `castsShadows`, `shadowQuality`, and
   signed `priority`.
3. Directional lights shade from illuminance in lux. Point and spot lights shade
   from luminous intensity in candela. Both intensity fields persist so a type
   change never silently converts or destroys the inactive value.
4. The editor presents color through an sRGB picker but persists nonnegative linear
   Rec.709/D65. Extraction converts to AP1, normalizes valid chromaticity to unit
   AP1 Y, and applies scalar physical intensity separately. Zero luminance produces
   no light and a diagnostic.
5. Point flux display uses `4*pi*cd`. Smooth-cone spot flux uses effective solid
   angle `2*pi*(1-(cos(inner)+cos(outer))/2)`. Cone values are half-angles in
   degrees and must satisfy `0 <= inner <= outer <= 90`.
6. One world unit is one metre. Local direct light uses inverse-square attenuation,
   a finite-source distance floor, and a separate smooth range window. Range is a
   culling/fade boundary, not a replacement for distance attenuation.
7. The frame-global default `photometricToSceneScale` is `1e-4`. It is applied in
   scene-linear lighting before the existing output-boundary exposure, which remains
   exactly once.
8. Version-1 migration preserves numeric RGB, adopts it as linear Rec.709/D65 with
   `light.v1_color_assumed_linear_rec709`, adopts intensity as lux or candela with
   `light.v1_intensity_unit_adopted`, preserves shape/shadow values, and initializes
   new inactive/quality/priority fields deterministically. Migration warnings are
   structured and ordered. Loading or runtime publication never implicitly saves.
9. Legacy Area is readable and emits `light.area_unsupported`; strict M5 cooking
   rejects it. M5 does not approximate Area as a point or spot light.
10. M5.2 extracts backend-neutral, UUID-owned light records. M5.3 builds one bounded
    clustered assignment representation consumed by deferred/material-resolve and
    complex/transparent forward paths. Vulkan owns storage layouts, descriptors,
    barriers, and device-limit specialization; persistent scene data owns none of
    those details.
11. Overflow and update selection are deterministic, priority-aware, bounded, and
    diagnosed. No consumer may build an independent forward-only light list.

## Consequences

- Existing v1 and legacy-v0 source scenes remain readable, while derived v1 cooked
  artifacts rebuild against the v2 runtime manifest.
- Source files become larger and migration produces visible one-time editor/cook
  cost, but runtime loading remains source-free and unambiguous.
- Light type changes preserve values in both physical unit domains and are exactly
  undoable.
- Direct-light comparisons can share CPU/GLSL reference math and AP1 conventions
  across deferred, forward, and future ray-tracing paths.
- Production Area emitters require a later ADR or a superseding decision with an
  explicit shape, photometric, BSDF, shadow, and performance contract.

## Rejected alternatives

- Persist lumens for every local light: point conversion is simple but spot flux
  depends on the cone profile, making on-axis shading indirect and type changes
  lossy.
- Persist only one scalar with a type-dependent meaning: repeats the v1 ambiguity
  and destroys inactive-unit intent during editor type changes.
- Treat authored RGB as AP1 or display sRGB: silently changes existing numbers and
  conflicts with glTF/editor color conventions.
- Approximate legacy Area lights: creates an unapproved and visually unstable
  production behavior.
- Build separate deferred and forward light assignments: duplicates scheduling,
  memory, overflow policy, diagnostics, and tuning, contradicting ADR-0006.

## Evidence

- `docs/milestones/M5-raster-lighting-shadows-probes-baking.md`
- `docs/performance/M5.1-light-component-v2-2026-08-08.md`
- `tests/scene/LightComponentV2Tests.cpp`
- `tests/renderer/LightingReferenceTests.cpp`

