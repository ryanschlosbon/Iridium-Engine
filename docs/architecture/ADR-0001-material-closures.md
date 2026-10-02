# ADR-0001: Compile Rich Source Materials to Canonical Runtime Closures

- Status: Accepted and implemented by M2
- Date: 2026-07-17
- Last updated: 2026-07-25 (M2 acceptance)
- Owners: Renderer and asset pipeline

## Context

Iridium imports glTF metallic/roughness materials today and will encounter specular/glossiness assets, glTF material extensions, authored coatings, transmission, and future procedural materials. Storing only base color, metallic, and roughness cannot represent every source workflow. Storing every authoring parameter in every GBuffer pixel would increase bandwidth and still fail to represent arbitrary layered closures cleanly.

A metallic/roughness workflow is an authoring parameterization, not the only physically based runtime representation. For a standard microfacet surface, the lighting equations ultimately need diffuse reflectance, specular reflectance at normal incidence (`F0`), and roughness. A specular/glossiness material can often be converted to those terms without changing that underlying single-lobe model. A model with coating, sheen, anisotropy, volume transmission, or multiple independent lobes cannot always be reduced without loss.

## Decision

The asset pipeline will preserve source material data and compile it into one of a small number of explicit runtime closures.

The standard closure contains:

- diffuse albedo;
- RGB `F0`;
- scalar `F90`/specular weight for exact `KHR_materials_specular` grazing behavior;
- perceptual roughness;
- shading normal and optional normal texture;
- ambient-occlusion input;
- scene-linear ACEScg/AP1 emissive radiance;
- material model and feature flags.

Metallic/roughness, specular/glossiness, and supported glTF extension inputs are authoring front ends. The compiler evaluates their factors, textures, defaults, color spaces, and channel mappings into the standard closure where that conversion represents the same single surface model.

Materials that require materially distinct lobes or transport are classified into a complex forward closure. Initial examples include layered clearcoat that cannot be acceptably approximated, volume transmission/absorption, sheen, anisotropy, and explicitly layered materials. Deferred/material-resolve and forward paths share BSDF functions and conventions. M5 supplies one clustered-light representation to both lighting paths.

The engine will keep a high-precision reference GBuffer and compare proposed packed production layouts against it. M2 selects a measured near-term production layout, but that layout is a canonical surface cache rather than a permanent requirement that geometry emit every surface field. Under ADR-0006, a later visibility-buffer material resolve may produce the same cache, fuse material and lighting evaluation when evidence supports it, or retain conventional GBuffer generation as a fallback.

## Consequences

- The GBuffer does not need both metallic and arbitrary specular controls. `F0` is the canonical lighting input.
- Metallic may still be retained in material/debug data when useful for authoring, diagnostics, or an optimized encoding.
- A source material inspector must expose source values, glTF defaults, textures, compiled values, selected closure, and conversion warnings.
- Complex materials pay forward-lighting cost only where used instead of increasing every deferred pixel.
- Compiled closure semantics remain independent of whether values come from a
  geometry-generated GBuffer, a visibility-buffer material resolve, forward raster,
  or ray tracing.
- Conversion tests need reference assets for dielectric, conductor, specular/glossiness, clearcoat, transmission, normal maps, and emissive behavior.
- Ray-tracing materials should consume the same compiled closure semantics.

## M2.5 implementation note

The shared standard reflection and normal conventions are now implemented as
descriptor-free GLSL includes with matching CPU reference functions. Deferred and
non-transmissive standard-forward raster evaluation share F0/F90 Fresnel, GGX/Smith,
diffuse weighting, tangent handedness, two-sided orientation, and numerical guards.
Missing glTF tangents use the MikkTSpace handedness convention and mirrored baked
transforms preserve orientation.

## M2.6 implementation note

Active coat, sheen, anisotropy, iridescence, thin/volume/diffuse transmission, and
dispersion compile to versioned tagged records consumed by an explicit canonical
forward shader; unlit is an explicit feature path. Dormant zero-effect extensions
remain standard with diagnostics. Surface reflection lobes use shared normal and
standard-base conventions, while transmission feeds the retained bounded M1
transport with source IOR, normal, roughness, thickness, and attenuation. Named
diagnostics retain M5 clustered-light and M6 ordering/refraction/dispersion limits.
The universal GBuffer did not grow; measured cost is restricted to selected forward
draws and their existing scene-copy passes.

## M2 acceptance note

M2 made source, compiled, instance, and schema-2 GPU material records the
unconditional production path. Standard closures use the complete 36-byte/pixel R
surface cache. Active complex closures are split into depth-writing opaque forward
and classified transparent forward queues. The faster Q/C cache experiments remain
non-production because they omit scalar F90 and reduce metadata to 16 bits. Shared
GGX/Smith/Fresnel and normal conventions are used by deferred and forward shaders;
future RT must reuse these semantics.

## Rejected alternatives

- Metallic/roughness only: simple, but cannot faithfully ingest all common PBR parameterizations and encourages hidden approximations.
- Put all possible lobes in the GBuffer: expensive for every pixel, difficult to extend, and still unsuitable for arbitrary layering.
- Convert every material unconditionally: loses source intent for closures that are not mathematically equivalent.
- Make a permanent geometry-path choice in M2: the material contract must survive the
  later GPU-scene and visibility-buffer work without pulling M7/M8 into M2.
