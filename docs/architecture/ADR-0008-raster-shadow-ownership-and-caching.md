# ADR-0008: Raster Shadow Ownership and Caching

- Status: Accepted for cache validity and fallback; the single-directional-owner
  capacity in decision 3 is superseded by ADR-0009
- Date: 2026-08-09
- Owners: Renderer, scene system, RHI, Vulkan backend, and profiling

## Context

M5 requires directional, spot, and point raster shadows without duplicating light
selection or shading policy between deferred and forward consumers. Shadow images
are persistent history: their validity depends on stable light identity, projection,
casters, and pipeline behavior rather than only the current render-graph frame.
ADR-0003 will later replace CPU draw-packet caster submission with a GPU scene, while
ADR-0006 requires deferred/material-resolve and forward paths to share lighting data.

## Decision

1. Backend-neutral RHI data owns selected light identity, cascade transforms, split
   distances, resolution, update mask, and sampleable mask. Vulkan owns image/view,
   render-pass, pipeline, descriptor, sampler, layout, and synchronization details.
2. The production graph declares persistent shadow products as imported external
   resources. Their backend owner preserves valid layers across frames and performs
   the real subresource transitions; graph consumers still declare sampled reads.
3. M5.6 supports one shadowed directional light. Selection is descending authored
   priority then ascending stable entity UUID. Other valid directional lights remain
   lit but unshadowed, and omission is counted.
4. The High directional product is one persistent four-layer 2048x2048 D32 array.
   Cascades use practical/log splits with lambda 0.7, exact frustum corners, a 5%
   guard band, 100 metre depth padding, radius quantization to 1/16 metre, light-space
   texel snapping, Vulkan depth 0..1, and a 10% smooth transition to the next
   sampleable cascade.
5. Sampling is shared by deferred and complex forward GLSL. It uses a clamp-to-white
   `LESS_OR_EQUAL` compare sampler and normalized 5x5 `[1,2,3,2,1]` tent PCF. Bias is
   a 1.25 constant/1.75 slope raster bias, a 0.001 receiver-depth bias adjusted by
   normal/light angle, and a one-texel world-normal receiver offset.
6. Opaque and forward-opaque packets form the current caster queue. Alpha-mask
   casters evaluate the indexed material opacity contract; one- and two-sided
   pipelines respect material culling. General transparent shadows are unsupported.
7. Cache validity includes selected owner, stabilized matrix, light revision, caster
   revision, and pipeline revision. Dirty layers are never sampled with a new
   projection. Updates are nearest-cascade first within the frame budget; newly
   rendered layers become sampleable in that frame. The accepted directional budget
   permits all four layers.
8. Missing, incompatible, deferred, or unpublished shadow data returns visibility
   one. No stale map is substituted. Debug views expose cascade choice and final
   visibility; captures and profiles publish owner, slot, masks, format, filter,
   update counts, pass time, and dedicated memory.
9. M7 may replace caster extraction/revision generation and raster submission with
   stable GPU-scene visibility, but it must preserve this RHI validity and fallback
   contract. M5.7 extends the same identity, budget, and stale-map rules to a spot
   atlas and tiered point cube arrays.

## Consequences

- The accepted directional product reserves exactly 64 MiB requested/committed on
  the reference driver, independent of the main 4K render extent.
- Static scenes pay sampling cost but record no directional shadow pass. Moving
  casters invalidate all affected cascades and expose their update work explicitly.
- A single selected sun is a deliberate capacity limit, not silent data loss.
- Wide kernels, colored/transmissive shadows, clipmaps, and transparent shadowing
  require later measured contracts rather than implicit changes to this path.

## Rejected alternatives

- Independent deferred and forward shadow schedulers: they can disagree on
  selection, cache validity, overflow, and visibility and conflict with ADR-0006.
- Graph-owned transient cascades: this discards cache history and forces refreshes.
- Sampling stale layers until an update budget catches up: the old projection is not
  valid for the new camera/light and produces leaks or detached shadows.
- Directional clipmaps now: they add residency and update complexity without an
  accepted dressed open-world fixture.
- VSM/EVSM as the initial filter: wide kernels are attractive but require separate
  precision, light-leak, memory, and blur policy evidence.
- One dedicated image per light: simple ownership scales descriptors and memory
  poorly and does not generalize to M5.7 local-light budgets.

## Evidence

- `docs/milestones/M5-raster-lighting-shadows-probes-baking.md`
- `docs/performance/M5.6-directional-shadows-2026-08-09.md`
- `docs/performance/data/M5.6-directional-shadows-2026-08-09.json`
- `tests/renderer/DirectionalShadowTests.cpp`
- `tests/renderer/VulkanRenderGraphExecutorTests.cpp`
