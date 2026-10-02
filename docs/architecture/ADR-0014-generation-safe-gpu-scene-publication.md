# ADR-0014: Generation-Safe GPU-Scene Publication

- Status: Accepted; M7.1 reference ABI implemented
- Date: 2026-08-29
- Last updated: 2026-08-31 (ABI 2 per-content LOD policy)
- Refines: ADR-0003 persistent GPU scene and ADR-0006 hybrid visibility

## Context

ADR-0003 requires one persistent GPU scene to serve indexed raster, future mesh
shaders, visibility resolve, shadows, probes, selection, and ray tracing. The current
renderer instead uses transient 240-byte per-submesh `DrawPacket` records and 32-bit
render-resource handles with a 20-bit index and 12-bit wrapping generation. Those
handles are suitable for short-lived RHI resources but can alias stale temporal
visibility, selection, readback, or indirect data after 4,095 slot reuses.

Material overrides are per scene entity while geometry identity is shared. Main,
shadow, probe, and selection visibility are independent consumers. Treating a
per-instance material binding as shared geometry state, or treating main-camera
visibility as persistent instance state, would violate the accepted asset and
visibility contracts.

## Decision

The GPU scene uses transient typed 64-bit handles represented by a 32-bit slot and a
32-bit nonzero generation. Retired slots remain unavailable until the caller's GPU
completion serial has passed. Generation exhaustion permanently quarantines a slot;
it never wraps. UUID/GUID identity remains in lossless CPU side tables and is never
inferred from a slot, pointer, ECS index, compacted offset, or indirect command.

The shader-hot ABI is split into:

- explicit 48-byte row-major affine transforms, with separately indexed current and
  previous records;
- 80-byte instance records containing conservative sphere/AABB bounds, transform
  and primitive ranges, generation, explicit mobility, history/state flags, and an
  independent consumer mask. ABI 2 reserves state-flag bits 8–11 for the authored
  maximum LOD ordinal: zero pins hero content to LOD0 and fifteen permits the full
  ABI-supported chain. The effective limit is the minimum of this content ceiling,
  project/device policy, and the validated compatible resident prefix;
- 48-byte instantiated-primitive records containing instance, shared geometry,
  effective material, pipeline/bin, generation, identity indirection, LOD
  indirection, and isolated layout/geometry/binding/material revisions;
- 96-byte shared geometry records containing conservative local bounds, indexed
  draw addressing, logical arena/layout/index-format data, generation, identity,
  LOD indirection, and product revision.

The ABI uses explicit scalar four-tuples rather than relying on GLM binary layout.
The affine convention preserves shear, non-uniform and negative scale, and large
translations. Invalid or unknown bounds carry a negative radius and a fail-visible
flag. Malformed, missing-residency, stale, or capacity-omitted instances remain on
the existing direct path for that frame; capacity overflow never silently publishes
a partial scene that makes content disappear.

Publication is atomic by `sceneEpoch` and monotonic `publicationRevision`. Every
record table has a parallel 64-bit revision table. Each fence-owned frame context
compares its uploaded revisions independently and coalesces exact changed ranges;
CPU-global changed lists cannot substitute for this comparison. Physical compaction
is not allowed to mutate logical handles. A future compaction changes dense mappings
inside one publication and invalidates affected temporal products.

Missing authored mobility maps to `Movable` until a versioned source/cooked schema
adds an explicit property. `Static` is never inferred from inactivity; `Animated`
is an explicit reserved update class. Selection and each main/shadow/probe view use
their own visibility product. The classified M6 transparency packet path remains
unchanged until a later measured migration.

## Consequences

- Stale indirect, history, selection, and readback IDs cannot alias a new occupant
  through ordinary generation wrap.
- Two entities can share one geometry record while retaining distinct effective
  material bindings and exact owner/primitive identity.
- Later classic indexed, visibility-buffer, mesh-shader, and ray-tracing paths can
  consume one identity and geometry database.
- Two transforms per instance are an intentionally conservative M7.1 reference;
  M7.2 may share equal current/previous slots for synchronized static records if
  measured and proven correct.
- M7.2 must add the publisher/change journals and the two-stage RHI prepare/publish
  integration before these records become a production Vulkan input.

## Rejected alternatives

- Reuse wrapping 20/12 RHI handles for scene lifetime: permits ABA after bounded
  reuse and is unsafe for temporal GPU products.
- Put UUIDs/GUIDs in every hot GPU record: preserves identity but wastes bandwidth
  and duplicates cold diagnostic/provenance state.
- Bake effective material into shared geometry: breaks per-entity overrides and
  causes geometry churn for material-only edits.
- Store one global visibility bit on the instance: couples independent camera,
  light, probe, and selection consumers.
