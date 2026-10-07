# M7 Completion (M7.9–M7.12) Execution Plan

## Header

- **Milestone:** M7 — GPU Scene and Indirect Visibility, completion slices M7.9–M7.12
- **Status:** Approved by the owner 2026-10-07. Active slice: M7.10.0 (backend headroom).
- **Lead:** one Claude Code lead session for all four slices
  (`docs/milestones/M7-completion-task-lead-prompt.md`)
- **Branch / PR:** `m7-completion` from `main` (`8a9a601`); one PR into `main`
- **Last updated:** 2026-10-07
- **Parent plan:** `docs/milestones/M7-gpu-scene-indirect-visibility.md`. This document
  supersedes that plan's M7.9–M7.12 slice text, which is linked from its header. The
  parent keeps the candidate register, invariants, M7.0–M7.8 history and decision log.
- **Entry hand-off:** `docs/milestones/M9-to-M7.9-handoff.md`
- **Exit hand-off (to write):** `docs/milestones/M7-to-M9c-handoff.md`
- **Governing ADRs:** ADR-0003, ADR-0005/0012 (transparency), ADR-0006, ADR-0007
  (photometric lights), ADR-0014, ADR-0015, ADR-0016, ADR-0017. ADR-0008/0009 apply
  where local shadows change.
- **Budgets and evidence tiers:** `docs/performance/FRAME_BUDGET.md`,
  `docs/performance/M7-performance-fidelity-contract.md`
- **Reference system:** RTX 4090, i9-14900K, 64 GB DDR5-6000, native 3840x2160. The
  raster target is 144 FPS (6.94 ms).

## Objective and user-visible outcome

When M7 is accepted:

- adding model instances costs CPU and GPU time in proportion to *visible* work. An
  off-screen car records no transparent work, sorts nothing and shades nothing;
- moving the camera close to glass costs only the shading the glass actually needs,
  and the cost is attributed, bounded and reported;
- a very bright point light costs no more than its authored or perceptually bounded
  influence requires, and editing it does not re-render work that its edit cannot
  change;
- a geometry-only, material-only, one-primitive or one-texture edit recooks only its
  dependent products. Oversized models appear progressively without an upload hitch;
- standard opaque surfaces have a measured indexed visibility/material-resolve
  experiment with a recorded production decision against packed deferred;
- the M7 acceptance gate passes, M7.8 is explicitly carried to its post-M8
  resumption, and M9c can start from a written hand-off.

## Current context (audit 2026-10-07, source `8a9a601`)

Four read-only audits re-inspected the source. Findings that correct or sharpen the
lead prompt and earlier plans are marked **[correction]**.

### Transparency (owner case 1)

- Transparent packets get **no side-plane frustum test**. The only cull is a
  view-space near/far interval test in `prepareTransparentWorkInterval`
  (`src/renderer/rhi/DrawPacket.h:129-200`), applied only to Classified packets at
  `src/extraction/RenderExtractor.cpp:1190-1198`. An off-screen car inside the depth
  range is fully requested, sorted, previous-transform-resolved and recorded.
- **[correction]** The 26,910 "ambiguous sort intervals" is a diagnostic *pair*
  count from an O(n log n) Fenwick sweep (`DrawPacket.h:303-375`) on a worker task.
  It is a symptom of work volume, not itself the cost. The costs are the sort
  (`ParallelDrawSort.cpp`), per-packet previous-transform binary searches,
  `submitForwardQueues` observation scans (`VulkanVertexBackend.cpp:2286-2403`),
  per-packet recording (`VulkanForwardFeature.cpp:134-260`), and the refraction
  pyramid, which is built whenever the compatibility queue is non-empty
  (`VulkanVertexBackend.cpp:2338`).
- Transparent packets are always direct; GPU-scene observation skips transparent
  submeshes (`GpuSceneObservation.cpp:61-62`), so instance bounds cannot reject them.
- The opaque frustum helper (`GpuSceneFrustum`, `GpuSceneVisibility.cpp:28-88`) is
  reusable but internal. Main-view classification uses the unjittered `projMatrix_`
  (`RenderExtractor.cpp:914-938, 1034`); the jittered matrix lives only in
  `viewTransport.jitteredProjection`.
- Culling is order-safe. Sorted and Classified compatibility orders are total
  pairwise orders with an identity tie-break, so removing packets does not reorder
  survivors. The atlas planner breaks ties by work identity. Side effects to handle:
  the previous-transform cache keeps only keys resolved this frame, so a re-entering
  packet would get zero motion for one frame. Layered/OIT residency hysteresis
  observes the queue. Layered `isPacketResolved` indexes the queue, so culling must
  happen in extraction.

### Glass close to the camera (owner case 2)

- The complex-forward light loop evaluates directional/spot/point shadow visibility
  for every listed light **before** any `noL` test
  (`assets/shaders/include/complex_material_body.glsl:904-986`; deferred tests `noL`
  first). Glass with a PCSS point light pays up to 96 taps per light per pixel.
- Every coat/sheen/iridescence lobe repeats probe-cluster selection
  (`environment_ibl.glsl:125-201`); transmission repeats it again.
- The compatibility forward pass draws back to front and writes depth, so nested
  glass is fully shaded with no early-Z benefit (`VulkanProductionRenderGraph.cpp:886-901`).
- Layered tiers return `NearPlaneFallback` for near-clipped or camera-intersecting
  packets (`Ordinary2Atlas.h:133-136`), and `AtlasCapacityExceeded` beyond their area
  caps (Ordinary2 1/4 screen, Hero4 1/2, Cinematic8 full). A close camera therefore
  likely pushes layered glass back into compatibility forward.
- Deep tiers re-rasterize every draw N+1 times over the island rect
  (`VulkanLayeredTransparencyFeature.cpp:837-961`).
- The refraction pyramid is full-resolution RGBA16F + R32F with a full mip chain, one
  dispatch per mip, and its cost does not depend on glass coverage.
- M9.8e reactive coverage adds no passes: a dual-source output, a velocity fetch and
  a push-constant previous matrix.

### Bright point light (owner case 3)

- **[correction]** No code derives a light's influence radius from intensity. Range is
  the authored `rangeMeters` (default 10 m), copied unchanged by `LightExtractor.cpp:315`.
  Shading applies `W(d,r) = clamp(1-(d/r)^4,0,1)^2` (`direct_lighting.glsl:18-23,
  56-64`), consistent with ADR-0007 item 6. Steady-state GPU cost does not depend on
  candela.
- Intensity does reach cost through **invalidation**:
  - Any light-record change (including intensity or colour) dirties the local shadow
    as `LightChanged` (`LocalShadow.cpp:475`). A point light re-renders all six faces,
    even though shadow depth does not depend on radiometry.
  - `reflectionProbeLightingRevision` hashes every light's record revision
    (`RenderExtractor.cpp:75-82, 740-759`). Any light edit anywhere can therefore
    request every realtime probe capture.
  - So dragging the intensity slider re-renders the cube and may recapture probes
    every frame.
- Range drives steady cost: cluster count/fill loop over each light's cluster AABB
  volume (O(r^3)); a light whose sphere crosses the near plane covers every tile of
  its slices (`clustered_lighting.glsl:97-154`). The point-cube far plane is the
  range, so caster sets grow with it. A user who raises intensity and sees the light
  clip at its range will raise range too.
- All five cluster passes share one timestamp (`gpu.lighting.cluster`); there are no
  per-face point-shadow scopes.
- Exposure: manual EV is known on the CPU per view; auto-exposure is GPU-resident with
  no readback (`VulkanExposureFeature.h`).

### Backend, CPU frame, cooking and visibility (M7.9–M7.11)

- `VulkanVertexBackend.cpp` is **2,496** lines. Cheap code-motion candidates (about
  600 lines in total): runtime-info and string helpers (34-124, 1392-1539),
  `applyTransparencyPyramidTopologyChange` and `prepareFrameTopology` (976-1091,
  1540-1644), TAA/exposure staging (1967-2003), cluster setup in `submitLightingPass`
  (2243-2262, which computes `clusterGridDimensions` twice), transparency staging in
  `submitForwardQueues`, qualification observer blocks, and probe rebind lambdas.
- Command recording is serial: one primary buffer per frame slot and no secondary
  buffers. The heaviest loop is `VulkanOpaqueFeature::recordGBuffer`. Parallel
  recording was deferred from M7R (about 3.1 ms of GBuffer recording on H-stress).
- Per-view derived math is recomputed in at least six places: inverses in
  `VulkanDeferredLightingFeature.cpp:284-285`, `ClusteredReflectionProbes.cpp:203-204`
  and `VulkanReflectionProbeFeature.cpp:559`, and `proj*view` three times.
- R5c.4e main-opaque CPU classification uses unjittered matrices and is kept by
  decision for production counters (about 0.23 ms on T-F7).
- **[correction]** In current source, embedded texture CookKeys
  (`GltfModelImporter.cpp:1002-1025`) do **not** hash model geometry/LOD settings.
  The parent model CookKey hashes all settings bytes (`CookKey.cpp:38-67`), and
  texture payloads are embedded in the one parent artifact (MTX1), so any setting
  edit re-serializes and re-hashes every texture. The 2026-09-01 "76 texture misses"
  report may reflect a cold or different DDC root or a different target; it must be
  re-measured before designing the fix.
- Schema-7 "LOD children" are primitives inside the parent manifest, not independent
  artifacts. Geometry and LOD cooking are serial; only embedded textures use a
  `Background` `parallelFor`. Reimport and preparation run on `Background` strands.
  There is no editor priority.
- Runtime publication is whole-model and atomic (`AssetIntegration.cpp:727-760`). An
  oversized model publishes as the first upload of a tick (1 GiB cap) on top of a
  128 MiB per-tick budget and a 64 MiB staging ring. Last-known-good is whole-asset.
- Debug/Release texture codec payloads differ under one CookKey (41 of 76); build
  configuration is not part of the key.
- No visibility-buffer code exists. The GBuffer (Reference layout default) is
  4x RGBA16F + R32_UINT + RG16F velocity; deferred lighting writes alpha 1.

### Tooling constraints

- Runners take fixtures only from the tables in `tools/m7r/M7RFixtures.ps1`
  (`$M7RModels`, `$M7RFrozenSet`, `$M7RTimingRoutes`); admission rejects unknown routes.
- A benchmark fixture loaded exactly **one** cooked model artifact
  (`BenchmarkScene.cpp:64`). M7.P1 resolved this: see the decision log, 2026-10-07.
- Fixture lights are static; there is no scripted light animation.
- The Porsche Carrera (`models/porsche_911_carrera4s`) is not in `$M7RModels`.

## Invariants

The lead prompt's invariants are authoritative. In summary:

- **Identity:** scene UUID and asset/subasset GUIDs persist; GPU slots, offsets and
  indices are transient (ADR-0014). Material-only edits never recook geometry.
- **Visibility:** main, shadow, probe and selection visibility are independent.
  Main-camera culling never removes a shadow caster or reflection participant.
  Uncertain culling or residency fails visible. Transparent culling is per view and
  conservative over each primitive's world AABB. Culling never reorders visible
  transparent work.
- **M9 contracts:** scene-colour alpha is revealage and opaque writers output 1.
  Opaque writers also write velocity. Jittered matrices never reach culling, Hi-Z,
  clustering, LOD, shadows, probes or layered atlas rects. Temporal state goes through
  `createHistory`.
- **Shading:** one clustered-light representation (ADR-0006/0007); shared BSDFs;
  scene-linear AP1 until the single output transform; ADR-0007 inverse-square
  photometrics stay, and range remains a fade/culling boundary.
- **Graph and frame:** declared passes and resources; barriers from usages; no render
  passes, `waitForAllFrames`, blocking uploads or `std::function` frame callbacks;
  zero steady-frame allocations on timing routes.
- **Fallbacks:** LOD and Hi-Z workload-selectable; conventional shadows and direct
  submission available; VSM default-off, compiling and tested.
- **Layering:** qualification code stays in `iridium_qualification`, with no test
  hooks in core interfaces. New code goes into feature owners; files stay under 2,500
  lines (`GltfModelImporter.cpp` and `AssetBrowserPanel.cpp` remain the recorded
  exceptions).
- **Kept behaviour:** ADR-0013 live transport switching, ADR-0017 history across
  rebuilds, and resize/zero-extent safety.

## Scope and non-goals

**In scope:** the three owner performance cases and the M7.10 fixes they motivate;
the rest of M7.10; M7.9; M7.11; M7.12 including the acceptance report and the
M7-to-M9c hand-off.

**Out of scope:** the lead prompt's list. That is M9c, M8, M7.8 VSM resumption,
M10, M9b, M11, the M9 TAA open items and the material editor follow-ups. The Porsche
mixed-class glass ordering defect is also out, unless a performance case proves it
is the same code path; in that case the lead raises it with the owner first.

## Measurement policy for this plan

- **Two routes per owner case.**
  - *Product route*: TAA, auto-exposure and bloom on, matching what the owner sees.
    It is used for attribution and the phase-1 before/after report, passed as
    `-ExtraArgs "--anti-aliasing taa --exposure auto --bloom on"`; a later flag wins
    over `Get-M7REngineBaseArgs`.
  - *Measurement route*: the pinned M7R route, used for refactor-tier identity and
    timing pairs.
- **Report, per fixture:**
  - GPU pass medians, p95 and p99;
  - non-wait CPU, split by stage critical path and aggregate worker time;
  - `acquire`, `present` and `frame_fence_wait`, reported separately;
  - requested, visible and recorded counts per queue;
  - committed memory by category, and steady allocations.
- **Attribution runs** are single-process diagnostics (500 + 2,000 frames). Fix
  evidence uses the tier in the slice.
- **Machine state:** ask the owner before any timing or admission run, and record
  the state with every pair.
- **Editor gap:** fixtures run headless, without the editor UI. If a case does not
  reproduce in fixtures, ask the owner for the scene document and the exact
  interaction.

## Vertical slices

Only one slice is `In Progress` at a time. Each slice states its evidence tier. A
fix that changes pixels is feature tier; a fix that must not is refactor tier, and
any byte-identity exception is explained.

### Phase 1 — owner performance cases

#### M7.10.0 — Backend headroom (refactor tier) — first, because phase-1 fixes touch forward submission

- **Move:**
  - runtime-info and string helpers into `VulkanRuntimeInfo.cpp`;
  - transparency-pyramid topology and frame-topology preparation into the forward
    or layered owners;
  - TAA/exposure staging into `VulkanTemporalAntiAliasingFeature` and
    `VulkanExposureFeature`;
  - cluster setup, computed once, into `VulkanClusterLightingFeature`;
  - the qualification observer blocks into `VulkanExtensionHooks`.
- **Target:** the backend under 2,000 lines, with no behaviour change.
- **Evidence:** Debug and Release tests; byte-identical frozen set (`m9-g8`) with
  `-SyncValidation`; one matched T-F1/T-F7 timing pair.

#### M7.P1 — Owner-case fixtures and baseline attribution (no engine behaviour change)

- **Fixtures** (new manifest `assets/m7c-owner-cases-manifest.v1.json`; Porsche
  Carrera added to `$M7RModels` and cooked; routes added to `$M7RTimingRoutes`):
  - **PC1 many objects:**
    - Porsche composition with N = 1, 4 and 16 instances, each in three states:
      all visible, half off-frustum, and all off-frustum (camera turned away);
    - Alfa composition with N = 16, all visible;
    - the existing 256-instance stress fixture, `m7_many_instance_stress_v1`.
  - **PC2 close to glass:** Porsche with one directional and one shadowed point
    light. A camera series puts glass at about 5%, 25%, 60% and about 100% of the
    screen (windshield close-up). Coverage comes from a separate
    `--profile-transparent-overdraw` run, because that counter costs time.
  - **PC3 point-light cost** (re-scoped from the owner's answer, 2026-10-07):
    - **Owner observation:** the camera sits between the 930 and the 911 in the
      editor. With no point light it runs at about 300 FPS (3.33 ms); adding one point
      light drops it to about 220 FPS (4.55 ms), roughly +1.2 ms wall time,
      **regardless of intensity**. Range scales the cost; source radius barely
      matters. Spot and directional lights cost less.
    - **Fixture:** the 930 and the 911 with the camera between them. This needs the
      multi-model harness (below), pulled forward from M7.12.
    - **Variants:**
      - no light;
      - one point light, with range 5, 10, 20 and 40 m;
      - unshadowed, High (hard) and Ultra (PCSS);
      - the same scene with one spot light and with one directional light, for the
        relative cost;
      - one intensity point at 1e3 and at 1e6 cd, confirming independence.
    - **Attribution targets:**
      - the point-cube shadow raster: is the cube re-rendered in steady frames, for
        example by the global caster revision?
      - shadow sampling taps in deferred and complex forward;
      - cluster assignment against range;
      - deferred and forward light-loop cost.
  - **Multi-model harness (qualification-only, pulled forward from M7.12):** a
    composition fixture may name several source assets, each with its own
    `--cooked-model-artifact`. It lives in `src/qualification/harness/BenchmarkScene`
    and the manifest parser; core interfaces are unchanged.
- **Attribution:** product and measurement routes, single process, with the full
  counter and pass breakdown. Captures (`scene`, `scene-resolved`, `final-sdr`) of
  each fixture for visual reference, inspected by eye.
- **Deliverable:** `docs/performance/M7C-owner-cases-baseline-2026-10.md`, with the
  attribution table per case and the fix candidates ranked by measured cost.

#### M7.10.1 — Conservative transparent frustum culling (refactor tier target)

- **Culling:** build one `GpuSceneFrustum` per `extract` from the unjittered
  `projMatrix * viewMatrix`, for the scene and preview views. Expose the AABB-plane
  test from `GpuSceneVisibility` as a public RHI helper. Reject a transparent packet
  whose world AABB is outside any plane. Keep invalid-bounds packets (fail visible).
  Apply the existing depth cull to every execution mode.
- **Skip whole models:** a per-model union of transparent submesh bounds, cached
  beside `TransparentSubmeshList`. Its whole-model rejection skips the per-submesh
  8-corner transform for off-screen models.
- **Keep previous transforms of culled keys alive** in `PreviousTransformCache`, so
  a re-entering packet keeps correct motion under TAA (contract 3 spirit).
- **Counters:** `transparent.work.requested`, `transparent.work.frustum_rejected`,
  `transparent.work.visible`, and `transparent.model.frustum_rejected`.
  `draw.requested.transparent` stays meaning queue size, now documented as
  post-cull.
- **Evidence:**
  - the TAA-off frozen set byte-identical. Off-screen packets contribute no pixels;
    the pyramid build is skipped only when no compatibility packet survives, and
    then nothing samples it;
  - PC1 before/after on both routes;
  - TF-pan and TF-disocclude motion evaluation, to show no re-entry regression;
  - a matched timing pair.

#### M7.10.2–M7.10.4 — Fixes chosen by attribution (tier per fix)

Candidates. The P1 baseline decides which proceed and in what order; anything
unmotivated by measurement is dropped from phase 1.

- **Bright light, invalidation (refactor tier; low priority).** The owner's
  steady-state case does not depend on intensity, so this item is not motivated by
  that case. It removes real edit-time waste and is exact, so it is kept as an
  optional cheap fix:
  - split the light record revision into a *shadow-geometry* revision (type,
    position, orientation, range, cone, source radius, shadow settings) and a
    radiometric revision. Local shadows go dirty only on the former;
  - scope `reflectionProbeLightingRevision` per probe to lights whose range sphere
    intersects the probe's capture sphere.
  - Both are exact: shadow depth does not depend on radiometry, and a light outside
    a capture sphere contributes `W = 0`.
- **Bright light, range cost (refactor tier if light lists change only by
  zero-contribution entries):**
  - a tighter sphere-vs-cluster-AABB test in count/fill;
  - per-pass cluster timestamps for attribution;
  - near-plane-crossing lights limited to their projected screen rect.
- **Bright light, perceptual bound (feature tier).** Owner decision 2026-10-07: do it
  only if measurement shows the "barely visible" part of the authored range is
  substantial *and* costly. Otherwise it is not built.
  - an optional *Auto* range mode that bounds the effective culling radius where
    exposed luminance falls below a threshold, using manual EV or EV compensation
    (auto-exposure is GPU-resident; any readback lags 2–3 frames);
  - authored range stays authoritative unless the light opts in.
  - It would be measured on PC3 as the fraction of the range sphere's shaded pixels
    whose exposed contribution is below threshold.
- **Glass:**
  - skip shadow evaluation for a light when every BSDF lobe for that light
    (reflection and transmission) is provably zero. Refactor tier only if exact,
    because transmission lights back faces;
  - hoist probe-cluster selection once per pixel across lobes (refactor tier if
    exact);
  - measure layered near-plane and capacity fallback rates; if the fallback
    dominates, a near-plane-tolerant rect is a feature-tier change;
  - measure the refraction pyramid against coverage.
- **Many objects beyond transparency:** extraction per entity, opaque CPU
  classification (R5c.4e), GBuffer recording (parallel recording is M7.10.6; it is
  pulled forward if attribution shows it dominating), shadow caster and probe
  membership.

#### Owner checkpoint (stop)

Report to the owner:
- fixtures and attribution;
- fixes, with each one's tier classification;
- before/after tables on both routes;
- images.

Wait for the owner to verify by eye in the editor, and continue to phase 2 only after
the owner replies.

### Phase 2

Order: M7.10 remainder → M7.9 → M7.11 → M7.12. M7.10 comes first because parallel
recording and per-view caching change frame structure that M7.11's resolve passes
then build on. M7.9 is mostly asset-pipeline work, so it can overlap with M7.10
kernel benchmarks run as isolated subagent lanes.

#### M7.10.5 — Per-view derived math (refactor tier)

- Add `ViewDerived` to the per-view record, computed once in `finalizeView`. It holds
  unjittered `viewProjection`, both inverses, frustum planes and cluster-grid
  dimensions; jittered fields stay raster-only.
- Replace the six recomputation sites.
- Evidence: byte-identical frozen set and one timing pair.

#### M7.10.6 — Parallel command recording (refactor tier)

- Record GBuffer bins (then shadows and forward, if measured) into secondary command
  buffers. Use per-worker, per-frame-slot pools on `FrameCritical` tasks, with no
  steady allocation. Execute them in the original order inside the dynamic-rendering
  scope (`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`).
- Draw order is unchanged, so output is byte-identical.
- Gate it on the H-stress and T-F7 CPU wins against the serial path, with a
  tiny-workload serial fallback.

#### M7.10.7 — Cluster assignment reprofile (tier per change)

- Per-pass timestamps on current workloads (PC3, T-F5, a cluster-stress route).
- Reduce only the passes evidence identifies. Keep one shared product for deferred
  and forward.

#### M7.10.8 — R5c.4e revisit

- Once GPU counters cover visibility, move the CPU classification into the
  qualification oracle; otherwise re-record the decision with current numbers.

#### M7.10.9 — Benchmark-gated candidates

Each candidate gets an experiment record using the contract template, and is
classified production, workload-selectable, experimental or rejected:
- DirectXMath, GLM SIMD and SoA kernels behind an internal boundary (frustum, AABB
  transform, affine compose, view derivation);
- LTCG and PGO (a toolchain change: owner approval);
- an AVX2 tier;
- a conditional depth prepass;
- async compute, gated on the timeline.

#### M7.9.0 — Recook re-measurement (no behaviour change)

Re-run the 2026-09-01 geometry-only edit against a warm DDC on the current importer,
and measure the material-only, one-primitive, one-texture, unchanged and superseded
edits. Record:
- per-stage time, cache hits and parent bytes;
- whether textures are re-encoded, or only re-serialized inside the parent.

This replaces the assumed defect with a measured one.

#### M7.9.1 — Child products and CookKeys

- **Children:** texture views, per-primitive geometry and LOD levels become
  independent DDC artifacts, each with its own CookKey. A key hashes only its
  inputs:
  - the texture key: image hash, semantic and colour space, codec identity, and
    build-configuration identity, which closes the Debug/Release key gap;
  - the primitive key: primitive source hash plus geometry and LOD settings.
- **Parent:** a deterministic manifest of child keys and payload hashes in fixed
  slot order. Its bytes do not depend on worker order.
- **Compatibility and future hooks:** schema advance with explicit migration; frozen
  products remain readable. Meshlet and RT attachment points are reserved.
- **Library check:** if geometry processing would benefit from meshoptimizer,
  explain it to the owner first (library policy).

#### M7.9.2 — Bounded cook job graph

- A deterministic DAG on the M7R task system at `Background` priority: per-texture,
  per-primitive and per-LOD jobs with memory and worker limits, newest-revision
  cancellation, and editor priority (the asset being viewed is admitted first).
- Telemetry: stage, worker, cache hits and cancellation latency.

#### M7.9.3 — Progressive, budgeted publication

- Publish child products by visible demand within per-frame byte and time budgets.
  Missing textures use semantic fallbacks; missing geometry uses a bounds proxy or a
  resident coarser LOD. Last-known-good is per child; a parent revision is never
  mixed.
- M9 contract 3: content swaps keep the instance's previous transforms. Check with
  TF-teleport and TF-disocclude, and confirm no streaming swap ghosts.

#### M7.9.4 — Edit-case acceptance matrix

Prove the material-only, one-primitive, one-texture, unchanged, superseded and
oversized-model cases (FRAME_BUDGET "Import, cooking, and publication").

#### M7.11 — Indexed visibility/material-resolve experiment (feature tier)

- **Payload:** depth plus an R32G32_UINT visibility payload holding the
  instantiated-primitive handle and the triangle id. Emitted by an indexed GPU-scene
  pass now, and by M8 mesh shaders later through the same encoding.
- **Resolve:**
  - a compute or full-screen resolve reconstructs barycentrics and analytic
    derivatives (ray-differential or barycentric-derivative), fetches attributes
    through ADR-0014 records, and evaluates the standard material;
  - it writes the M2 canonical cache and velocity (from previous transforms) and
    reuses deferred lighting, which writes alpha 1.
- **Scope:** standard opaque materials only. Alpha-clip uses the explicit forward
  class unless restricted opacity is sufficient.
- **Identity expectation:** analytic derivatives will not reproduce hardware quad
  derivatives bit for bit, so texture LOD may differ. The expected classification is
  feature tier with a stated reason; the TAA-off identity route stays on packed
  deferred, the default.
- **Compare the complete chain:** images, GPU time, bandwidth and attachment memory,
  material coherence and divergence. Run it on representative scenes: the three-asset
  fixture, PC1 and the 256-instance stress.
- **Decision:** production, workload-selectable, experimental or rejected. Packed
  deferred remains the fallback.

#### M7.12 — Production qualification and hand-off

- **Harness:** the multi-model composition built in M7.P1 lets the fixed native-4K
  three-high-fidelity-asset scene mix Porsche, Alfa and a third asset.
- **Acceptance gate:** run the roadmap M7 gate, covering:
  - static, moving, off-screen, occluded and LOD cases, with presentation wait
    separated;
  - the recook and residency matrix;
  - the visibility-path decision.
- **TAA-on fixtures:** add them to the qualification sweep in the `scene-resolved`
  domain, with measured envelopes.
- **Before/after:** re-run the owner cases as evidence.
- **Reports and documents:**
  - the M7 acceptance report, accepting M7 with M7.8 carried to its post-M8
    resumption;
  - `docs/milestones/M7-to-M9c-handoff.md`;
  - updates to ROADMAP (M7 `Accepted`, M9c `Ready`), FRAME_BUDGET and
    PROJECT_CONTEXT "Current direction".

## Delegation and integration

- The lead owns integration, shared headers and shaders, the graph, the executor,
  CMake and the model schemas. No two concurrent writers touch those.
- **Subagents** (each writer in its own worktree, built under the disk rules):
  - read-only audits;
  - fixture and manifest authoring;
  - metric tooling (`Summarize-*` extensions);
  - CPU kernel benchmarks (M7.10.9), as isolated targets;
  - the cooker child-product work (M7.9.1–2), after the lead fixes the schema;
  - the visibility resolve shader, once its ABI is frozen.
- C: has about 153 GB free (2026-10-07). A lane worktree costs about 25 GB, so run at
  most two lanes at once and delete build directories when a lane ends. Scan for
  junctions and symlinks before any recursive delete. Worktree removal needs the
  owner's approval.

## Verification

- Always build with `tools/m7r/Build.ps1` (`-Preset x64-debug` and `x64-release`, `-Test`).
- **Refactor tier:**
  - `tools/m7r/Run-FrozenCaptures.ps1 -SyncValidation`, then
    `Compare-FrozenCaptures.ps1` against `out/m7r/captures/m9-g8`;
  - one `Run-TimingPair.ps1` pair on a quiet machine.
- **Feature tier:**
  - `tools/m9/Run-FeatureAdmission.ps1 -RequireQuiet`, with per-fixture envelopes;
  - `Run-MotionEvaluation.ps1` where motion or streaming is involved;
  - memory and counters.
- **Every slice:** `--qualification-allocation-trace` on the timing routes; image
  inspection by eye (`--require-capture-signal`); resize and transport-switch smoke
  when the graph changes.

## Risks, fallback and rollback

| Risk | Mitigation |
|---|---|
| Transparent culling drops visible glass (bad bounds, instance batches) | Conservative world AABB; invalid bounds fail visible; instance batches use the union bound; capture comparison on PC1/PC2 and the frozen set |
| Re-entering glass loses motion for a frame | Keep previous-transform entries for culled keys; motion evaluation |
| Shadow-revision split misses a geometry-affecting field | Enumerate fields from `PackedGpuLight`; test each field's dirty reason |
| Perceptual cutoff visibly clips lights | Opt-in Auto mode only; authored range default; feature-tier image gate; owner judges by eye |
| Owner cases do not reproduce headless | Ask for the editor scene and interaction; add an editor-route measurement |
| Parallel recording costs more than it saves on small scenes | Workload threshold and serial fallback |
| Child-product schema churn invalidates caches | Explicit schema advance; frozen products readable; recook measured |
| Visibility resolve loses derivative or material fidelity | Packed deferred stays default; feature-tier gate on representative scenes |
| Disk exhaustion on C: | At most two lanes; transient references deleted; usage checked before runs |

## Open questions for the owner

None open. The two-car scene was never saved, so PC3 is the reference approximation
(owner, 2026-10-07).

## Decision log

- **2026-10-07 — Plan audit corrections.** The intensity-derived light radius does
  not exist (range is authored); intensity reaches cost through shadow and probe
  invalidation. Ambiguous intervals are a diagnostic pair count. Embedded texture
  keys do not hash geometry settings in current source, so M7.9 re-measures first.
  Fixtures support one model each, and mixed-asset scenes need a qualification-side
  harness extension (M7.12). Recorded so later slices do not design against stale
  descriptions.
- **2026-10-07 — Slice order.** M7.10.0 backend headroom precedes the phase-1 fixes,
  because they touch `submitForwardQueues`. Phase 1 fixes are chosen by measured
  attribution, not by the candidate list.
- **2026-10-07 — Owner answers (plan approved).**
  - The point-light case is the steady cost of one point light, about +1.2 ms, in
    the two-car editor scene. It is independent of intensity, scales with range and
    barely depends on source radius; spot and directional lights cost less. PC3 is
    re-scoped and the multi-model harness moves to phase 1.
  - The perceptual Auto range is built only if measurement shows substantial cost
    for barely visible contribution.
  - The owner asked for testing that does not consume unnecessary disk space; the
    retention proposal is pending approval.

- **2026-10-07 — M7.10.0 code motion landed (refactor tier; timing pair pending).**
  - `VulkanVertexBackend.cpp` went from 2,496 to 1,906 lines. What moved:
    - runtime info, capabilities, telemetry and memory snapshot went to the new
      `VulkanRuntimeInfo.cpp` (344 lines);
    - TAA staging went to `VulkanTemporalAntiAliasingFeature::stageFrame`;
    - cluster setup went to `VulkanClusterLightingFeature::recordFrame`, with the grid
      computed once (a pure function of unchanged inputs);
    - caster-revision observer forwarding went to
      `VulkanExtensionHooks::observeCasterRevision`, still guarded by
      `kQualificationBuild`;
    - transparency topology change and preparation went to the layered owner, which
      reaches the pyramid and WeightedOIT residencies and the frame-target
      callbacks through plain function pointers.
  - Profiler scope names, counters and the failure-restore path are unchanged.
  - **Evidence:**
    - Debug and Release each pass 118/118 tests;
    - the shipping `iridium_vulkan` target compiles;
    - frozen set `m7c-1000` against `m9-g8` with `-SyncValidation`: 22 of 24
      byte-identical, F4-woit within its `woit-order` envelope, zero validation and
      synchronization messages.
  - **Still owed:** the matched native-4K timing pair, which needs the owner's
    machine-state approval.
  - The Release build was produced in `out/build/x64-release-m7c`, because the owner's
    running editor locked `out/build/x64-release`.

- **2026-10-07 — M7.P1 fixtures landed (qualification-only; no runtime change).**
  - **Multi-model mechanism:**
    - composition entities may name their own `source_asset`, and `node` is optional
      (whole model);
    - each extra model's artifact goes in a repeatable, qualification-only
      `--benchmark-model-artifact`;
    - the harness matches artifacts to sources by the asset GUID recorded in the
      artifact, never by argument order. It checks dependency files for staleness and
      fails clearly on any ambiguity;
    - multi-model fixtures get a startup-only topology prewarm.
  - **New models:**
    - `porsche911` (cook key `37049427…`);
    - `porsche930` is `porsche911930t/porsche911930t.gltf` (cook key `1f6d625d…`,
      245 MB). It carries the owner's material overrides; the `free_1975` copy has no
      sidecar and cannot be cooked.
  - **Fixtures:** 25 owner-case fixtures in `assets/m7c-owner-cases-manifest.v1.json`,
    regenerated by `tools/m7r/Generate-OwnerCaseManifest.py`. They run through
    `$M7COwnerCases` / `$M7CTimingRoutes`, `Run-OwnerCaseCaptures.ps1` captures them,
    and preview PNGs were inspected.
  - **Evidence:**
    - Debug and Release each pass 119/119 tests;
    - zero validation messages on PC1-n16 and PC3-r10;
    - the frozen set and the M9 composition set are unchanged.
  - **Note:** the all-off PC1 states are constant images, verified as uniform instead
    of passing `--require-capture-signal`.

- **2026-10-07 — M7.10.1 transparent frustum culling landed (refactor tier; timing
  pair and the both-route PC1 before/after pending).**
  - **Culling:** one unjittered frustum per `extract`, shared with main-view
    classification. Per-packet AABB rejection uses the new public helper
    `gpuSceneFrustumRejectsAabb`, plus the depth-interval cull, now applied in every
    execution mode. Invalid bounds fail visible. Whole-model rejection tests a
    cached local union in `TransparentSubmeshList` (instance batches use the union of
    their per-instance bounds), in `src/extraction/TransparentFrustumCulling.*`.
  - **Previous transforms:** `PreviousTransformCache::touch` keeps culled keys alive
    with their current transform, so re-entering glass keeps exact motion. Visible
    packets keep the original path byte for byte.
  - **Lead decision, residency:** the refraction-pyramid, layered-tier and
    WeightedOIT residency observe the demand of culled work through
    `RenderFrame::culledTransparentDemand`, so turning away from glass does not
    release and rebuild targets after 120 frames. The pyramid *build* still runs only
    for surviving compatibility work. As a consequence, Classified depth-culled work
    now also keeps residency and motion history; before, it dropped out.
  - **Counters:** `transparent.work.{requested,visible,frustum_rejected}` and
    `transparent.model.frustum_rejected`.
  - **Evidence:**
    - Debug and Release pass 120/120 tests;
    - frozen set `m7c-1010` with sync validation: 22 of 24 byte-identical, F4-woit
      within its envelope, zero messages;
    - owner cases 50/50 identical;
    - TAA-on composition set 88/88 identical;
    - a scratch TF-glass that leaves and re-enters the frustum: 30/30 frames
      identical. With touches disabled it diverges, which proves the check is
      sensitive.
  - **Diagnostic counters on PC1-911-n16-half:**
    - recorded transparent draws: 4,376 → 1,967;
    - ambiguous intervals: 146,536 → 34,429;
    - transparent sort: 0.241 → 0.129 ms;
    - intervals: 0.326 → 0.167 ms;
    - forward recording: 0.571 → 0.285 ms.

    These are single short runs, diagnostic only.

- **2026-10-07 — Evidence retention policy (owner-approved).**
  - Added `tools/Prune-Evidence.ps1` and the policy in PROJECT_CONTEXT "Evidence
    retention and disk use" and AGENTS.md.
  - The first prune removed 152.7 GB of raw pre-completion evidence (summaries kept)
    and the four `out/m7r/worktrees` baselines (11 GB). C: free space went from 153 to
    304 GB.
  - This milestone's `m7c-*` labels stay until their slices are accepted. Each later
    slice prunes its own evidence on acceptance.
- **2026-10-07 — PC3 diagnostic attribution (single process; machine not verified
  quiet).** One 10 m point light with the camera inside its sphere:
  - `gpu.lighting.cluster` rises from 0.020 to 4.160 ms, with 138,720 of 195,840
    clusters used;
  - `gpu.transparency.sorted.forward` rises from 1.314 to 3.657 ms;
  - deferred lighting rises from 0.200 to 0.398 ms.

  The point cube is a cache hit, so shadow raster is not the cost. Root cause of the
  cluster cost: `cluster_count`/`cluster_fill` run one 64-lane workgroup per light
  over the light's whole cluster AABB, which serializes large lights. That is fixed
  as M7.10.2 (parallel assignment, refactor tier). The sorted-forward increase is the
  next PC3 attribution item.

- **2026-10-07 — M7.10.2 parallel clustered-light assignment landed (refactor tier;
  timing pair pending).**
  - **New pass:** `cluster_light_bounds.comp` evaluates each active light once:
    directional list, local diagnostic, cluster AABB and reference reservation. It
    writes a 32-byte record to the new graph transient `lighting.cluster.light-bounds`
    (2 MiB per slot, sized to the 65,536-light bound).
  - **Count and fill:** they dispatch (light, chunk), with
    K = clamp(16384 / lights, 1, 64), and grid-stride the light's AABB. Their
    per-cluster atomics and enable conditions are unchanged; fill's overflow gate is
    equivalent because overflow is monotonic within a frame.
  - **Placement:** the bounds dispatch runs inside the existing
    `lighting.cluster.count` pass, so pass order is unchanged. The aliasing and
    executor goldens gain exactly one transient slot.
  - **Evidence:**
    - Debug and Release pass 120/120 tests, with new CPU-reference cases (a
      camera-enclosing light; 49 lights with order independence);
    - frozen set `m7c-1020` with sync validation: 22 of 24 byte-identical, F4-woit
      within its envelope;
    - PC2 and PC3 owner cases 28/28 identical;
    - the 512-light cluster-stress capture is identical, and every cluster counter
      is identical including the dense-sort path.
  - **Diagnostic single-process timings:**

    | Case | `gpu.lighting.cluster` (ms) | `gpu.frame` (ms) |
    |---|---|---|
    | PC3 point light, 10 m | 2.009 → 0.074 | 5.637 → 3.144 |
    | PC3 point light, 40 m | 2.792 → 0.083 | 6.716 → 3.152 |

    The no-light case is unchanged. The earlier 4.16 ms reading came from a busier
    machine.
  - **Overflow note:** under per-cluster overflow, the LOCAL and REQUESTED diagnostics
    may differ from before. They were already schedule-dependent there, and the image
    fallback is unchanged.

- **2026-10-07 — M7.10.1b: culled-glass bookkeeping made O(owners) (refactor tier).**
  - **Problem:** the owner-case timing found PC1-911-n16-off regressed from 0.692 to
    0.869 ms wall time (non-wait CPU 0.26 → 0.77 ms). The cause was the M7.10.1
    keep-alive: it touched all 4,320 culled keys, then sorted and merged them every
    frame, serially and unscoped. Culled work was therefore not free.
  - **Fix:**
    - whole-model rejection records one owner entry
      (`PreviousTransformCache::touchOwner`), and a key with no entry of its own
      falls back to its owner's entry. This is exact, because every transparent
      packet of an owner carries the owner's world transform
      (`RenderExtractor.cpp:1146`);
    - per-packet culls still touch per key, bounded by partially visible models;
    - the block now has the scope `cpu.render.previous_transforms`;
    - a never-seen key of an owner rejected last frame now reports the owner's
      last-frame transform instead of its current one. That is consistent with M9
      contract 3 for content swaps; no capture exercises it.
  - **Evidence:**
    - Debug and Release pass 120/120 tests, including the new owner-level tests
      against a never-culling reference cache;
    - frozen set `m7c-1011` with sync validation: 0 failures, zero messages;
    - owner cases 50/50 identical to `m7c-p1-fixtures`;
    - a scratch TF-glass re-entry (the panel keyframed off-screen and back; the whole
      model rejected for about 57 frames; TAA on) matches the no-culling baseline
      `1758e0d` on 16/16 `scene-resolved` frames.
  - **Timing:** native 4K, A,B,B,A, product route, `m7c-owner-product-pc1b`:

    | Route | Wall time | Non-wait CPU |
    |---|---|---|
    | n16-off | 0.692 → 0.699 ms (parity) | 0.261 → 0.235 ms |
    | n16-half | 2.568 → 1.749 ms | 2.45 → 1.35 ms |
    | n16-all | unchanged | unchanged |

## Completion report

To be written at M7.12, following AGENTS.md:
- owner-case before/after;
- the classification of every candidate, with evidence;
- cook, residency and recook results;
- the visibility-buffer decision;
- memory and VRAM;
- images;
- ADR changes;
- remaining risks.
