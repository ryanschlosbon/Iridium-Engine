# ADR-0009: Multi-Light Shadow Visibility and Project Policy

- Status: Accepted; directional, spot-atlas, and tiered point-cube implementations
  landed during M5.7
- Date: 2026-08-11
- Supersedes: ADR-0008 decision 3 and the single-owner capacity/memory
  consequences; all ADR-0008 cache-validity and fallback rules remain accepted
- Owners: Renderer, scene system, RHI, Vulkan backend, editor, and profiling

## Context

ADR-0008 deliberately accepted one shadowed directional owner for the M5.6
foundation. Production lighting must also support overlapping shadows from distinct
lights: two opposing lights illuminate a receiver independently, so each light must
sample its own visibility before its radiance is accumulated. A single global
visibility term or a single shadow owner makes the other light incorrectly
unshadowed and cannot represent the scene.

Shadow storage and tuning also require project ownership. Resolution, owner budget,
cascade update budget, stabilization policy, and per-light quality must not be
embedded in Vulkan code or ImGui widgets.

## Decision

1. Shadow visibility is evaluated per light record. Deferred and complex-forward
   consumers resolve a shadow owner from the current light slot, sample that owner's
   layers, multiply only that light's radiance by the result, and then accumulate.
   Overlapping cast regions therefore compose through the independent light sums.
2. The M5 directional storage ceiling is two simultaneous owners, each with four
   cascades in one persistent D32 array. The eight layers use deterministic
   priority/UUID selection and independent cache state. Lower-ranked valid lights
   remain lit but unshadowed and are counted explicitly.
3. `ProjectShadowSettings` is a backend-neutral, editor-independent policy record.
   It owns directional resolution, active owner count, cascade update budget, split
   lambda, guard band, and depth padding. The active resolution is selected at
   startup and drives both Vulkan allocation and the external render-graph extent.
   Other fields can change between frames from Project Settings.
4. Per-light `castsShadows`, `shadowQuality`, and `priority` remain persisted Light
   component data and are editable through the Inspector. They do not depend on the
   project settings UI.
5. The existing ADR-0008 rules remain mandatory per owner: stable identity,
   projection/caster/pipeline revisions, no stale incompatible projection,
   deterministic update budgeting, visibility-one fallback, and shared sampling
   between deferred and forward paths.
6. M5.7 spot and point shadows use the same per-light visibility rule and the same
   project/per-light ownership split. Spots use a stable guarded D32 atlas and
   publish a constant-time shadow-data slot through the packed GPU light record.
   Points use stable 256/512/1024 cube-array pools, publish only complete six-face
   cubes, and share one seam-safe sampler. Neither may reintroduce a global shadow
   term.

## Consequences

- The default 2048 product reserves 128 MiB for eight D32 layers. A 1024 project
  setting reserves exactly 32 MiB; 512 and 4096 scale quadratically.
- Two selected owners can update eight cascades on a cold frame. The project update
  budget can trade refresh latency against spike cost without publishing invalid
  layers.
- The current fixed storage ceiling deliberately bounds descriptor, UBO, and VRAM
  cost. Raising it requires new performance and memory evidence, not a shader-only
  constant change.
- Resolution changes currently require renderer restart because they change
  persistent image allocation and graph topology. The active value is visible in
  Project Settings and configurable through the application/project startup policy.
- The default 4096 D32 spot atlas reserves 64 MiB. Its 2048/4096/8192 startup
  choices reserve 16/64/256 MiB. A 256-entry frame table covers the maximum atlas;
  cache-compatible static tiles require no raster work.
- Default point-pool capacities are 32 cubes at 256, 16 at 512, and 8 at 1024:
  56 stable slots and exactly 336 MiB of D32 storage. Capacity is startup project
  policy; per-light quality selects a tier. Updates are atomic six-face units under
  a project-wide rendered-texel budget, so an incomplete cube is never sampled.

## Rejected alternatives

- Multiplying all direct lighting by one combined visibility term: lights have
  different directions, projections, blockers, colors, and intensities.
- Keeping one directional owner and treating additional lights as a local-shadow
  concern: opposing directional sources still require independent maps.
- One descriptor/image per light: it scales resource ownership poorly and is not
  needed for the bounded M5 capacity.
- Editor-owned renderer constants: UI lifetime and persistence must not define RHI
  or Vulkan behavior.

## Evidence

- `tests/renderer/DirectionalShadowTests.cpp`
- `tests/renderer/VulkanRenderGraphExecutorTests.cpp`
- `tests/renderer/LocalShadowTests.cpp`
- `tests/renderer/LightingReferenceTests.cpp`
- `tests/core/ApplicationConfigTests.cpp`
- `docs/performance/M5.7-multi-directional-resize-2026-08-11.md`
- `docs/performance/M5.7-spot-shadow-atlas-2026-08-11.md`
- `docs/performance/M5.7-point-shadow-pools-2026-08-11.md`
- `out/benchmarks/m5.7-multi-directional-debug.jsonl`
- `out/benchmarks/m5.7-multi-directional-1024-debug.jsonl`
