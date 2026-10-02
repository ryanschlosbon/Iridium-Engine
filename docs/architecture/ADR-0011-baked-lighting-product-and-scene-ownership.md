# ADR-0011: Baked-Lighting Product and Scene Ownership

- Status: Accepted
- Date: 2026-08-13
- Extends: ADR-0004 and ADR-0010
- Owners: Asset pipeline, scene system, renderer, editor, and profiling

## Context

M10 will add production non-RT global-illumination solvers and streaming, but those
systems need stable ownership before a solver-specific representation reaches scene
files or renderer code. Lightmaps, irradiance probe volumes, and baked visibility
have different payloads and residency behavior while sharing scene provenance,
invalidation, identity, publication, and fallback requirements.

Paths, transient ECS indices, primitive draw indices, Vulkan handles, and C++ object
layouts cannot provide durable association. A failed or partially resident revision
must not erase authored intent or replace a complete published product.

## Decision

1. `iridium.component.baked_lighting_set` is the stable scene assignment. It stores
   a cooked asset GUID, bounded diffuse/specular multipliers, and independent
   lightmap, probe-volume, and visibility contribution switches. Resolution and
   publication state remain transient.
2. `iridium.baked-lighting` schema 1 is the cooked product. Its required manifest
   freezes the scene asset GUID, scene-linear ACEScg/AP1/D60 color, metre units,
   baker identity/version, quality profile, typed-section counts, and SHA-256 input
   fingerprints for scene, geometry, materials, lights, settings, and tool.
3. Optional typed sections contain lightmap atlas descriptors plus stable
   entity/mesh-primitive GUID bindings, irradiance probe-volume descriptors, and
   visibility-volume descriptors. Descriptors use explicit encodings and tightly
   packed bounded payload ranges. Section IDs, versions, and unknown sections fail
   closed.
4. Association uses scene entity UUID and mesh primitive GUID. Renaming or moving
   source assets does not change association. Dependency content/artifact hashes and
   explicit fingerprints drive CookKey and invalidation.
5. Publication validates a complete product before an atomic generation change.
   Invalid, corrupt, missing, or future-schema products preserve authored GUID intent
   and retain the last-known-good revision. With no valid revision, every optional
   contribution is neutral.
6. The contract is backend neutral. M10 may add solver-specific encodings through a
   new schema/section version, streaming chunks, or GPU realization without changing
   scene ownership. Runtime components contain no ImGui, source paths, or Vulkan
   state.

## Consequences

- M5.10 makes no GI quality or frame-time claim; it supplies deterministic data and
  publication boundaries for M10.
- Typed payloads may be large. M10 must add asynchronous I/O, independent section/
  chunk residency, upload budgets, and backend memory accounting rather than parsing
  or copying full products on the render thread.
- A baker or content change can report precise invalidation domains instead of
  invalidating by path or timestamp.
- New encodings require deliberate versioning and validation. Unknown data is never
  guessed into a current runtime representation.

## Verification

- Deterministic bytes under reordered input bindings.
- Stable GUID association across moved/renamed dependency locations.
- Exact source/cooked component round trips and source-free cooked scene loading.
- Missing, duplicate, malformed, unknown, corrupt, and future sections fail closed.
- Dependency hashes and every provenance fingerprint have independent invalidation
  coverage.
- Failed publication retains the last complete revision; no-valid-product fallback
  is neutral.
