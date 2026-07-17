# ADR-0001: Compile Rich Source Materials to Canonical Runtime Closures

- Status: Accepted direction; details to validate in M2
- Date: 2026-07-17
- Owners: Renderer and asset pipeline

## Context

Iridium imports glTF metallic/roughness materials today and will encounter specular/glossiness assets, glTF material extensions, authored coatings, transmission, and future procedural materials. Storing only base color, metallic, and roughness cannot represent every source workflow. Storing every authoring parameter in every GBuffer pixel would increase bandwidth and still fail to represent arbitrary layered closures cleanly.

A metallic/roughness workflow is an authoring parameterization, not the only physically based runtime representation. For a standard microfacet surface, the lighting equations ultimately need diffuse reflectance, specular reflectance at normal incidence (`F0`), and roughness. A specular/glossiness material can often be converted to those terms without changing that underlying single-lobe model. A model with coating, sheen, anisotropy, volume transmission, or multiple independent lobes cannot always be reduced without loss.

## Decision

The asset pipeline will preserve source material data and compile it into one of a small number of explicit runtime closures.

The standard deferred closure contains:

- diffuse albedo;
- RGB `F0`;
- perceptual roughness;
- shading normal and optional normal texture;
- ambient-occlusion input;
- scene-linear emissive radiance or a documented pre-exposure representation;
- material model and feature flags.

Metallic/roughness, specular/glossiness, and supported glTF extension inputs are authoring front ends. The compiler evaluates their factors, textures, defaults, color spaces, and channel mappings into the standard closure where that conversion represents the same single surface model.

Materials that require materially distinct lobes or transport are classified into a complex forward closure. Initial examples include layered clearcoat that cannot be acceptably approximated, volume transmission/absorption, sheen, anisotropy, and explicitly layered materials. Deferred and forward paths share BSDF functions and conventions.

The engine will keep a high-precision reference GBuffer and compare proposed packed production layouts against it. The final layout is an M2 measurement decision, not fixed by this ADR.

## Consequences

- The GBuffer does not need both metallic and arbitrary specular controls. `F0` is the canonical lighting input.
- Metallic may still be retained in material/debug data when useful for authoring, diagnostics, or an optimized encoding.
- A source material inspector must expose source values, glTF defaults, textures, compiled values, selected closure, and conversion warnings.
- Complex materials pay forward-lighting cost only where used instead of increasing every deferred pixel.
- Conversion tests need reference assets for dielectric, conductor, specular/glossiness, clearcoat, transmission, normal maps, and emissive behavior.
- Ray-tracing materials should consume the same compiled closure semantics.

## Rejected alternatives

- Metallic/roughness only: simple, but cannot faithfully ingest all common PBR parameterizations and encourages hidden approximations.
- Put all possible lobes in the GBuffer: expensive for every pixel, difficult to extend, and still unsuitable for arbitrary layering.
- Convert every material unconditionally: loses source intent for closures that are not mathematically equivalent.
