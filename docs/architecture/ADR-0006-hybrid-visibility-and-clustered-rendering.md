# ADR-0006: Hybrid Visibility, Clustered Deferred, and Clustered Forward Rendering

- Status: Accepted direction; staged implementation belongs to M2, M5, M7, and M8
- Date: 2026-07-22
- Last updated: 2026-07-25 (M2 cache decision)
- Owners: Renderer, material system, geometry/GPU-scene pipeline, and RHI

## Context

Iridium currently renders ordinary opaque surfaces with conventional deferred
rendering: geometry writes the M2 canonical R cache (four `RGBA16F` targets plus
`R32_UINT`) and a full-screen pass evaluates lighting. Opaque complex and
transparent complex materials use separate forward passes. Neither path currently
consumes clustered light lists.

M2 must establish canonical material closures and compare reference and packed
GBuffer layouts. That requirement provides an image oracle and a production bridge,
but it does not require geometry to emit a full GBuffer forever. At 4K, repeatedly
writing and reading material fields consumes substantial bandwidth. A visibility
buffer can reduce the initial opaque raster payload and delay material evaluation
until after visibility, but it requires stable GPU-scene identities, random-access
geometry/material data, correct attribute and derivative reconstruction, scalable
texture access, and carefully measured material coherence.

Clustered shading addresses a different problem: it assigns lights to bounded
screen/depth regions and can serve both deferred and forward consumers. Combining a
visibility buffer, a full conventional GBuffer, and independent light lists without
a staged contract would duplicate storage and shading work rather than form an
efficient hybrid renderer.

## Decision

Iridium will evolve toward one hybrid raster architecture:

1. M2 compiles rich source materials into backend-neutral standard, complex-forward,
   and unlit closures. Its measured decision retains the complete high-precision R
   cache for near-term conventional deferred production. Q/C remain experiments
   because their speed and memory wins do not compensate for lost F90 and metadata.
2. M5 builds one clustered-light assignment representation. Clustered deferred
   lighting and clustered forward shading consume the same light/cluster records and
   shared BSDF conventions.
3. M7 builds the persistent GPU scene and indexed indirect visibility foundation,
   then introduces an indexed visibility-buffer experiment for standard opaque
   surfaces. The visibility payload identifies reconstructable instance/primitive
   data and material records; it does not contain display-referred shading values.
4. The first production visibility path, if measurements justify it, performs a
   material/attribute resolve and writes the M2 canonical packed surface cache for
   clustered deferred lighting and downstream surface consumers.
5. A fused visibility-to-material-to-lighting compute path may later bypass that
   cache only when matched 4K evidence demonstrates a net benefit and all required
   downstream consumers, derivatives, debug views, and capture domains remain
   correct.
6. M8 adds mesh shaders as another emitter of the same GPU-scene visibility
   representation. Classic indexed raster remains a supported fallback and an A/B
   comparator.
7. Complex opaque materials use clustered forward shading. Alpha-clipped standard
   materials may use the visibility path when restricted opacity evaluation is
   sufficient; otherwise they use an explicit forward class. Transparent and
   transmissive surfaces remain in the classified M6 forward/ordered paths.

The reference GBuffer, packed surface cache, and visibility buffer may run together
for validation or controlled experiments. Production must not pay for redundant
full-screen representations without measured downstream reuse that justifies them.

RHI contracts expose capabilities, resource dependencies, and backend-neutral
identities. Vulkan owns descriptor layouts, formats, barriers, subgroup choices, and
device-specific execution.

## Required evidence

Every geometry-path decision uses matched 4K scenes and records:

- depth/visibility, material-resolve, surface-cache, deferred-lighting, and
  complex-forward GPU times separately;
- attachment and buffer bytes, requested/committed/transient memory, cache/bandwidth
  counters where available, and synchronization/queue costs;
- visible triangles/pixels, overdraw avoided, material/texture divergence, and
  reconstructed-attribute work;
- scene-linear and final-output image differences, edge/derivative behavior, normal
  and roughness error, motion vectors, and temporal stability;
- classic indexed versus mesh-shader emission using the same scene and visibility
  records.

The visibility path is accepted only on a representative workload, not merely on a
small proxy scene. Conventional packed deferred remains the fallback when it is
faster or more robust for a device or workload.

## Consequences

- M2's GBuffer work remains necessary but is no longer described as the permanent
  opaque geometry architecture.
- M2 packed material records and M3 asset/cooker products must support indexed GPU
  access without requiring a future material-semantic rewrite.
- M5 cannot build separate deferred and forward light-management systems.
- M7 owns the first visibility-buffer implementation because stable GPU scene and
  geometry identities are prerequisites; M8 does not create a mesh-shader-only
  renderer.
- Screen-space effects and diagnostics consume an explicit canonical surface
  contract rather than depending accidentally on legacy attachment packing.
- Complexity is spatially classified: common opaque surfaces use the cheapest
  measured path, while uncommon lobes and transport pay clustered-forward costs only
  where present.

## Rejected alternatives

- Permanently extend the conventional GBuffer for every future lobe: charges every
  pixel for uncommon material complexity and does not represent arbitrary layering.
- Add a visibility buffer beside a full production GBuffer unconditionally:
  duplicates bandwidth and working set without proving downstream reuse.
- Implement visibility buffering in M2: pulls stable GPU scene, geometry indirection,
  descriptor strategy, and indirect visibility from M3/M7/M8 into a material
  milestone.
- Use independent light assignment for deferred and forward rendering: duplicates
  culling, diagnostics, memory, and tuning while making BSDF comparisons harder.
- Require mesh shaders for visibility buffering: removes the portable indexed path
  and prevents an isolated measurement of mesh-shader benefit.

## References

- Ola Olsson, Markus Billeter, and Ulf Assarsson, *Clustered Deferred and Forward
  Shading*: https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf
- Christopher Burns and Warren A. Hunt, *The Visibility Buffer: A Cache-Friendly
  Approach to Deferred Shading*: https://jcgt.org/published/0002/02/04/
