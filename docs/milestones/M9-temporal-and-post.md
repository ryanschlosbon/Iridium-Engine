# M9 — Temporal Rendering and Core Post-Processing

## Header

| Item | Value |
|---|---|
| Milestone | M9 — native motion vectors, jitter, native TAA (DLAA-class at 1:1), reactive handling, bloom, auto-exposure, generic reprojection/history utilities, vendor-neutral super-resolution input contract |
| Status | **Accepted** 2026-10-06 (completion report below; hand-off `docs/milestones/M9-to-M7.9-handoff.md`). |
| Lead | M9 lead session (Claude Code), from `docs/milestones/M9-task-lead-prompt.md` |
| Branch / PR | `m9-temporal` from `Render-Refactor-for-Modularity` at `da8e4e8` (PR #7 merged). One PR into `Render-Refactor-for-Modularity`. |
| ADRs | ADR-0002 (scene-linear HDR, single output transform, auto-exposure deferred to M9), ADR-0006 (velocity not GBuffer-only), ADR-0012, ADR-0013 (transport switch rebuilds the graph), ADR-0014 (identity; current/previous records), ADR-0015 (task rules), ADR-0016 (graph execution; History item 5) |
| Dependencies | M1, M2, M7R (accepted 2026-10-04), the M7.1/M7.2 GPU scene |
| Next lead | M7.9 (`docs/milestones/M9-to-M7.9-handoff.md`) |
| Last updated | 2026-10-06 |

## Objective and user-visible outcome

Iridium has no anti-aliasing, motion vectors, bloom or auto-exposure today. When M9 is
accepted:

- the default raster image is **temporally anti-aliased at native 3840x2160**. It is
  stable on a static camera, sharp, free of ghosting in motion, and correct for HDR
  extremes. The quality target is a modern AAA TAA or DLAA, judged by eye and by
  metric;
- every opaque depth writer produces **correct per-pixel motion**, including camera
  motion, object motion, stopped objects, teleports and cuts;
- moving glass, emissive and particles do not smear (reactive handling);
- **bloom** is physically based and energy-conserving;
- **auto-exposure** uses a histogram with eye adaptation. It survives camera cuts and
  adapts independently in the editor's scene view and asset preview;
- TAA costs at most **0.40 ms**. Post, bloom, exposure and output together cost at
  most **0.50 ms** on the reference RTX 4090 at native 4K;
- a **TAA-off and jitter-off route** still reproduces the M7R frozen set byte for
  byte;
- M9b can plug DLSS, FSR and XeSS into the same input contract without reworking M9.

## Current context (audit 2026-10-05 against `da8e4e8`)

Four read-only audits re-checked `docs/milestones/M7R-to-M9-handoff.md` against
current source. Most cited lines still match. The corrections that change the design
are listed first.

### Corrections to the hand-off

| # | Hand-off said | Source shows | Consequence for M9 |
|---|---|---|---|
| C1 | Dual views "invalidate each other every frame" | **One view renders per frame.** `EditorViewScheduler::choose` (`EditorViewCadence.h:40-62`) gives the background view one turn at 30/60 FPS and never two turns in a row. Each switch between views invalidates every pair. The scene view's identity is 1 (dual or single mode); each preview is `sessionSerial + 2`. | Per-view history must be valid if the writer ran on **that view's previous turn**, not in the previous global frame. Hi-Z uses a global serial-diff rule (`DepthPyramid.cpp:98-100`), so the background view never has valid Hi-Z history; graph History must not copy that rule. Exposure adaptation needs a per-view time delta. |
| C2 | The view reaches only the Hi-Z owner | It also reaches `GpuSceneLodHistory` (`VulkanOpaqueFeature.cpp:171`, `GpuSceneLod.cpp:422-430`). That history is invalidated on any projection change. | Jitter must not enter `view.projection` (this confirms the hand-off's recommendation). |
| C3 | View UBO ABI is checked by `ShaderAbiContract` | The set-0 block is **copied inline in seven shaders** (`canonical_material.vert`, `gpu_scene_material.vert`, `weighted_oit_instanced.vert`, `gpu_scene_frustum_compact.comp`, `depth_pyramid_gpu_scene_query.comp`, `transparency_pyramid.comp`, `include/complex_material_body.glsl`). The test checks `canonical_material_vert.spv` only (`ShaderAbiContractTests.cpp:369-380`). `ViewTransportRecord` has only a size assert. | G5 first moves the block into one shared include and extends the ABI test to every declaration, before any field is added. |
| C4 | GBuffer target change = constant, 3-or-5 check, backend target list | The fixed wireframe, wireframe-indirect and selection-outline pipelines also hard-code five targets (`VkGraphicsPipeline.cpp:169-177`, `:205-209`), and so does a five-entry blend array (`VulkanPipelineLibrary.cpp:309-311`). | M9.1 changes all of them together. |
| C5 | "Keep transparent targets at one colour" | Compatibility transparency draws through `RenderPassClass::Forward`, not Transparent (`VulkanForwardFeature.cpp:24-26`, `ModelRuntimeProduct.cpp:69-73`). The library requires Forward and Transparent to have exactly one colour (`VulkanPipelineLibrary.cpp:136-141`). | A velocity attachment on forward-opaque needs a distinct forward-opaque target class, so compatibility transparent pipelines are not affected. |
| C6 | `transparent.compatibility.forward` writes fallback glass depth | The pass *declares* a depth write (`VulkanProductionRenderGraph.cpp:878-879`), but every transparent pipeline has `depthWrite = !transparentBlend`, which is false (`ModelRuntimeProduct.cpp:43-55`, `:88`). In practice it writes no depth. | Reprojection does not see glass depth. The opaque depth writers are `gbuffer` (G-buffer material and wireframe pipelines) and `forward-opaque`. |
| C7 | The "moved last frame" list may need extraction support | Extraction re-observing a stopped entity would produce an identical observation, which the publisher still classifies Clean. The publisher's unchanged fast path (`GpuScenePublisher.cpp:244-257`) and Clean classification (`:461-473`) are the gates. | The settle list lives in `GpuScenePublisher`, beside `touchedInstanceScratch_`. The extraction verifier compares observations only, so it is unaffected. `ReferenceGpuScenePublisher` (tests) changes in lockstep. |
| C8 | Transparent and direct draws carry no previous matrix | Primitive-indexed packets (selection, GPU-scene forward-opaque, parity packets) carry a dense primitive index. Truly direct packets (non-GPU-scene owners) carry only a matrix. All transparent packets are matrix-only. `CanonicalMeshPushConstants` is 80 B, so a second mat4 would exceed 128 B. Set 4 (GPU scene) is already in the forward layout but not bound there. | G4 uses two mechanisms. Primitive-indexed packets get the primitive index in `padding[2]` and read `references.y`. Matrix-only packets get a per-frame previous-transform stream filled from an extractor cache keyed by stable owner identity. |
| C9 | Manifests are SHA-pinned in `BenchmarkManifestTests` | Manifest files are not pinned. The test asserts parsed fields and the `content_files` SHAs inside each manifest. | Add parsed-field assertions for the new M9 manifests. |
| C10 | Timing routes T-F1/T-F7 at `M7RFixtures.ps1:52-55` | Four routes (T-F1-all, T-F7-stack, T-F5-hetero, T-F6-probecap) at `:56-61`. | All four are M9 timing routes. |
| C11 | `--validation-sync` is a qualification flag | It is a runtime option (`RuntimeOptions.cpp:41`). | — |
| C12 | Benchmark motion supports temporal scenes | `scene_factory.kind` accepts only `instanced_grid` of one glTF. `camera_motion` has velocity, oscillation and a single cut. There are no paths, keyframes, multiple cuts or per-entity motion. No procedural mesh generator exists. | G6 adds engine-authored geometry and a composition scene factory (below). |
| C13 | Captures can drive temporal evidence | Each process allows exactly one capture (`QualificationReport.cpp:43-50`, one armed request at a time). | G6 adds capture sequences and an accumulation reference. |
| C14 | Line numbers | `RenderExtractor.cpp` projection `:910-915`, `finalizeView` `:929-959`, history fill `:950-954`, EV select `:945`; test ranges `RenderGraphTests.cpp:120-265`, `VulkanRenderGraphDeviceTests.cpp:186-249`, R3b.6 golden test `ProductionGraphContractTests.cpp:384-470`. Pass pins: count 26 at `VulkanRenderGraphExecutorTests.cpp:162`, indices at `:175-193`. | — |

### What exists that M9 builds on

- **Graph History** (`RenderGraph.h:414-423`, `.cpp:211-240`, `:734-770`, `:872-954`):
  - pairs with two non-aliased slots each;
  - a validity key `{identity, resetRevision, extent, format, topologyHash}`;
  - one tracker per executor;
  - parity flips when the writer begins;
  - an invalid `previous` transitions from `UNDEFINED`.
  - Production declares none. `beginFrameExecution(frame)` reuses `lastView_ = {0,0}`. The two-argument form exists but is unused.
- **View flow:**
  - order: `beginFrame` (`FrameOrchestrator.cpp:322`), `finalizeView` (`:377-384`), `extract` (`:391-397`), `submitFrame` (`:404`);
  - `updateCamera` runs first in `submitFrame`, before any graph pass;
  - `finalizeView` does not receive `applicationFrameIndex`;
  - no per-view frame counter exists.
- **GPU scene:**
  - two affine records per instance (`2d` current, `2d+1` previous) with per-index revisions;
  - dirty-range uploads per frame slot;
  - a moving instance costs 96 B per slot per frame;
  - a settle would cost 48 B per slot, once;
  - the shadow/probe content watermark reads only the current revision, so a settle does not trigger shadow or probe caches.
- **Opaque compaction is an atomic append** (`gpu_scene_frustum_compact.comp:303`). That is the source of the F3/F7-lod depth-tie nondeterminism (G8).
- **Output pass:**
  - four bindings: scene.color, ACES2 LUT, emissive (selection mask) and depth (grid occlusion);
  - a 128 B push block with `manualExposureEv` at offset 112;
  - output settings are owned by `ApplicationConfig`, the CLI (`RendererOptions.cpp:164`) and the editor (`ProjectSettingsPanel.cpp:136`). There is no persisted project profile yet.
- **Feature owners:**
  - `IVulkanFeature` (`VulkanFeatureContext.h:87-96`); the backend's owner array has 12 entries (`VulkanVertexBackend.h:239-243`);
  - `VulkanOutputFeature` is the template: about 160 lines, including drain-point staging;
  - history descriptors must be selected per parity, not per frame slot. No existing owner does this.
- **Evidence tooling:** `tools/m7r` (frozen set of 12 fixtures, envelopes, four timing routes, four-process A,B,B,A pair, sweep, digests, hitch, starvation). Python tooling uses only the standard library; numpy is not installed.

### Measured baseline (M7R final, native 4K, quiet machine)

| Route | GPU median | CPU frame median | Steady allocations |
|---|---:|---:|---:|
| T-F1-all | 1.129 ms | 1.114 ms | 0 |
| T-F7-stack | 1.942 ms | 1.919 ms | 0 |
| T-F5-hetero | 2.997 ms | 2.974 ms | 0 |
| T-F6-probecap | 3.669 ms | 3.649 ms | 0 |

M9 base build check (2026-10-05, `da8e4e8`): Release build ok, **113/113 tests**.

## Invariants

Section 10 of the hand-off is authoritative; summarised here:

1. **Colour:**
   - one output transform;
   - scene-linear AP1 until it;
   - TAA, bloom and exposure operate on scene-linear data;
   - `output.display` keeps one writer;
   - UI is composed after output and never exposed.
2. **Shading:** one clustered-light representation; BSDFs shared by deferred, forward and RT. Velocity is derived from GPU-scene current/previous records, not a GBuffer-only contract (ADR-0006).
3. **Visibility:**
   - never jitter culling, clustering, Hi-Z (`projectionRevision_`), LOD history, shadow, probe, the layered atlas request rects, or the R5c.4e CPU frustum classification;
   - main, shadow, probe and selection visibility stay independent.
4. **Graph:**
   - all GPU work is declared passes and resources;
   - one `vkCmdPipelineBarrier2` per pass, plus the ADR-0016 item 6 cases (mip chains are one);
   - History is never aliased;
   - no transient is read before its writer.
5. **Frame path:**
   - no render passes, `std::function` frame callbacks, `waitForAllFrames` or blocking uploads;
   - resize and growth go through the deletion queue;
   - zero steady-frame C++ allocations on the timing routes (`--qualification-allocation-trace`);
   - per-frame CPU work follows ADR-0015: frame-critical or inline, per-index outputs, no order-deciding atomics.
6. **Layering:**
   - qualification code stays in `iridium_qualification`, with no test hooks in core interfaces, and the shipping preset stays clean;
   - new code goes in new feature owners. `VulkanVertexBackend.cpp` (2,334 lines) and `VulkanRenderGraphExecutor.cpp` (2,038) must stay under 2,500.
7. **Identity and robustness:** ADR-0013 live transport switching, ADR-0014 identity, and zero-extent and resize safety.
8. **Defaults:** LOD, Hi-Z and VSM stay default-off; VSM keeps compiling and passing its tests.
9. **Evidence:**
   - the TAA-off route reproduces the M7R frozen set (`captures/r0`) byte for byte, or within the recorded R0 envelopes;
   - never edit the frozen R0 entries;
   - images change only under the feature tier.
10. **Content:** no third-party content and no `imgui.ini` in commits. M9 fixtures are engine-authored.

## Scope and non-goals

**In scope:**
- the gaps G1–G8;
- M9.1–M9.7 as planned below;
- the vendor-neutral super-resolution (SR) input contract, with native TAA as its first provider;
- generic reprojection and history utilities for M10;
- the temporal evidence tooling;
- an amended ADR-0016 note, or a new ADR if the History contract changes materially.

**Out of scope** (each with an owner):
- DLSS/FSR/XeSS integration and dynamic resolution: **M9b**, defined at M9.7, before M11;
- motion blur and depth of field: a later post slice, which the ROADMAP will name if recommended;
- GTAO, stochastic-shadow and GI denoiser consumers: M10;
- RT: M11;
- VSM completion and M7.9–M7.12;
- skinned motion: M13, with the velocity contract left ready for it;
- material editor follow-ups, the Porsche mixed-class glass defect, async compute, and parallel command recording.

## Design and data flow

### Frame tail with M9 (TAA on)

```
... transparent.compatibility.forward | transparent.oit.resolve   (last scene.color writer)
scene-color-capture-hook       reads scene.color          (single-frame domain; unchanged)
temporal.taa.resolve           reads scene.color, velocity, depth.opaque, taa.history.previous,
                               exposure.previous, reactive        -> writes taa.history.current
scene-resolved-capture-hook    reads taa.history.current  (new post-TAA domain; qualification)
post.exposure.histogram        reads taa.history.current  -> exposure.histogram (transient buffer)
post.exposure.adapt            reads histogram, exposure.previous -> exposure.current (History)
post.bloom                     reads taa.history.current  -> bloom.chain (transient, mipped)
output-transform               reads resolved colour, bloom, exposure.current, selection mask,
                               depth.opaque               -> output.display
final-capture-hook             reads resolved colour, output.display
ui-* / hdr10-encode-present
```

- **TAA off:** the graph declares exactly today's passes. Output, bloom and exposure
  read `scene.color`. This is what keeps the frozen-set route byte-identical,
  including its alias plan.
- **"Resolved colour":** a small helper (`VulkanProductionGraphIds::resolvedSceneColor`)
  names the resource the post chain reads: `taa.history.current` when TAA is on,
  otherwise `scene.color`.
- **Final capture:** `final-capture-hook` keeps its pinned `scene.color` read for the
  TAA-off route. Under TAA it reads the resolved colour. The contract test is updated
  to pin both topologies.
- **Placement:** bloom and exposure are independent of each other and of TAA, so each
  works with TAA off. Their declarations are gated by their own settings.

### History (G1, G2)

**One `ViewHistoryContext` type.** The graph type in `renderer/graph` becomes the only
definition, and rhi `Mesh.h` re-exports it.
- `identity == 0` means "no view": history is never valid.
- The extractor always sets an explicit identity: scene view 1, preview
  `sessionSerial + 2`. That preserves today's Hi-Z and LOD behaviour, which relied on
  the rhi default of 1.
- It gains `retainedView` (0 = scene, 1 = asset preview) and `viewFrame` (the per-view
  frame counter, see G5).

**Supplying the view.** `submitFrame` calls a new
`executor.beginViewExecution(view)` beside `updateCamera`, before the first graph
pass. It re-keys validity for that view, so `beginFrame` keeps its current position.
`beginFrameExecution(frame)` stops touching validity.

**Per-view pairs.**
- The executor keeps up to `HistoryViewCount = 2` physical slot sets per pair. The
  index is `retainedView`, following the Hi-Z precedent.
- Each set has its own parity, access state and validity record.
- Slot set 1 is allocated only when a retained view 1 first renders (dual editor
  mode), through the normal rebuild/retire path; it is never allocated on a steady
  frame.
- The tracker holds one `PairState` per (pair, view set).
- **Validity rule:** the writer ran on that view set's previous turn, under an
  identical key. Validity is *not* "previous global frame" (C1).
- Asset-preview sessions share set 1 and are told apart by identity, so a new session
  invalidates set 1 and nothing else.

**Per-pair reset policy.** `createHistory(name, desc, HistoryPolicy{reset})`, where:
- `HistoryReset::OnCut` is the default. The key includes `resetRevision`. TAA,
  velocity history and the M10 denoisers use it.
- `HistoryReset::SurviveCut`: the key excludes `resetRevision`, but still includes
  identity, extent, format and topology. Adapted exposure uses it.

**Cut and teleport ownership** (extraction, `finalizeView`). A per-view
`ViewMotionTracker` keeps the previous unjittered view and projection for each
retained view, and bumps that view's `resetRevision` on:
- an explicit request: benchmark cut, editor "frame selection", camera bookmark or
  teleport, or preview framing (already a revision);
- a projection change: FOV, near/far, kind or extent;
- a heuristic discontinuity: per-frame camera translation above a threshold relative
  to the near plane and scene scale, or rotation above about 30° in one frame.
  Thresholds are tuned against the temporal fixtures and recorded in the decision log.

Object teleports set `GpuSceneInstanceHistoryReset` (G3 and M9.6).

**ADR-0016.** Item 5's "as implemented" note is amended with per-view sets, the
per-pair reset policy and the per-view validity rule. If review finds this changes
the contract rather than completing it, a new ADR supersedes item 5 instead. (Review
did not require one; ADR-0017 later refined item 5 for compatible rebuilds, M9.8d.)

### Previous transforms (G3, G4)

- **Settle (G3).** `GpuScenePublisher` keeps a bounded `movedLastPass_` list of the
  dense indices whose transform changed in the previous pass.
  - A listed instance that is Clean this pass gets previous = current, with a revision
    bump on slot `2d+1` only, and joins the touched set.
  - A non-empty list disables the unchanged fast path for that pass only.
  - Capacity equals the instance pool, reserved with the pool: no steady allocation.
  - Upload cost is 48 B per stopping instance per frame slot, once. It is reported in
    `GpuSceneUploadTelemetry`.
  - `ReferenceGpuScenePublisher` and the replay tests change in lockstep.
  - `packGpuSceneReference` keeps its caller-supplied previous.
- **Teleport (G3).** A transform change flagged as discontinuous sets
  `GpuSceneInstanceHistoryReset` and previous = current, so the object shows zero
  motion and TAA rejects it through its disocclusion and clip tests.
  - Discontinuous changes are: editor numeric edits, undo/redo, scene load, and the
    harness's `object_motion.step`.
  - Gizmo drags are continuous.
  - The flag is carried on the observation; it is not inferred from distance.
- **Direct packets (G4):**
  - *Primitive-indexed packets* (selection, GPU-scene forward-opaque and their
    fallbacks) put the dense primitive index in `CanonicalMeshPushConstants.padding[2]`
    with a "has GPU-scene record" bit. They bind set 4 in the forward pass and read
    `references.y`.
  - *Matrix-only packets* read a per-frame `motion.direct-previous` stream: 48 B affine
    per packet, staged into a fixed member and indexed by `padding[2]`. The extractor
    fills it from a `PreviousWorldCache` keyed by stable owner identity (ADR-0014; no
    ECS indices or pointers). It applies the same settle and teleport rules.
  - WeightedOIT instance batches get a parallel previous-instance stream.

### Jitter and view ABI (G5)

**G5a: refactor first.** Add `assets/shaders/include/view_uniforms.glsl`, the single
declaration of the set-0 block, and include it in all seven declarers.
- Add offset `static_assert`s for every `ViewTransportRecord` and
  `UniformBufferObject` field.
- Extend `ShaderAbiContractTests` to each shader that declares or uses the block.
- SPIR-V changes only in block and member names, so frozen captures must stay
  byte-identical.

**G5b: new fields**, appended so every existing offset is kept:

| Field (record / UBO) | Content |
|---|---|
| `previousViewProjection` (mat4) | the previous turn's **unjittered** view-projection for this view (equals current after a reset) |
| `jitter` (vec4) | xy = current jitter in NDC, zw = previous jitter in NDC |
| `temporalInfo` (uvec4) | x = jitter sequence index, y = per-view frame counter, z = history flags (reset this frame, TAA on), w = reserved |

- `ViewTransportRecord` grows from 320 B to 416 B, and the UBO from 384 B to 480 B.
- `proj` and `inverseProjection` stay unjittered, so the compute culling and Hi-Z
  shaders and every CPU consumer are unchanged.
- **Raster vertex stages** apply `clip.xy += jitter.xy * clip.w` through
  `iridiumApplyJitter`: G-buffer (direct and indirect), forward-opaque, transparent,
  WeightedOIT, layered capture/composition/resolve, and wireframe.
  - The selection-mask pipeline does not: a specialization constant
    `IRIDIUM_APPLY_JITTER = false` keeps the mask, which output reads after TAA,
    unjittered.
- **Depth-based reconstruction** subtracts the current jitter in NDC before the
  unjittered inverse, so it is exact against jittered depth. This covers deferred
  lighting, the sky background, the layered chord and VSM full-view marking.
  Refraction lookups add the jitter, because the pyramid holds jittered colour.
- **Grid occlusion** samples depth at the jitter-compensated texel. Its existing bias
  covers the remainder (verified visually).
- **Sequence:**
  - Halton(2,3), with the phase index = per-view frame counter mod N;
  - the counter lives in `ViewMotionTracker`, advances once per rendered turn of that
    view, and resets with the view's `resetRevision`;
  - it never uses wall time, so captures are reproducible;
  - N = 8 is the starting value; 8 and 16 are compared on the stability and sharpness
    metrics in M9.2.
- **Zero jitter** (TAA off) must produce byte-identical frozen captures. If FMA
  contraction breaks identity, the fallback is a specialization constant that removes
  the jitter term for the TAA-off route. This is measured, not assumed.

### Motion vectors (M9.1)

- **Target.** `gbuffer.velocity` is appended after `gbuffer.material-flags`, so
  attachments 0–4 stay stable. It is cleared and alias-eligible. Everything listed in
  C4 changes together: `VulkanPipelineMaxColorTargets` becomes 6, the 3-or-5 check
  becomes 3-or-6, and so do the fixed pipelines and blend arrays.
- **Format candidates.**
  - **RG16F**, holding the UV-space delta `currentUV − previousUV` from unjittered
    positions. Precision is about 1e-7 UV near zero, and about 1 px at half-screen
    motion on 4K.
  - **RG32F**, as the precision reference.
  - The choice follows a CPU-oracle comparison on analytic motion and a bandwidth
    measurement.
- **Writers.**
  - The G-buffer and forward-opaque pass write velocity.
  - Forward-opaque gets a `ForwardOpaque` target class with two colours (C5).
    Compatibility transparent stays one colour.
  - Velocity is computed in the vertex stage from current and previous GPU-scene (or
    G4 stream) world positions and the current and previous unjittered
    view-projections, then interpolated.
  - Fragments that need no history write a "no history" marker. Those are fragments
    of instances with `GpuSceneInstanceHistoryReset`, and any after a view reset.
  - Skinned motion (M13) will supply the previous position through the same vertex
    contract.
- **Background.** Pixels at the far plane have no writer. TAA reconstructs their
  motion from the camera matrices.
- **Debug view.** `RenderDebugView` value 24, `motion-vectors`: direction is hue,
  magnitude is log brightness. Sampled in the lighting debug branch.
- **Oracle.** A qualification CPU oracle projects the analytic benchmark motion
  (camera path plus object motion) at sample pixels and compares it with velocity
  readback. Tolerance: 1/64 px at RG32F; the measured envelope for RG16F.

### TAA (M9.2)

`VulkanTemporalAntiAliasingFeature`, a new `IVulkanFeature`. It declares
`temporal.taa.resolve` (compute, one dispatch) and owns `taa.history`, an RGBA16F
History pair with the `OnCut` policy. The starting point, each part admitted only on
evidence:

1. **Reprojection.** A 3x3 closest-depth dilated velocity fetch. Far-plane pixels use
   camera reprojection.
2. **History sampling.** Five-tap Catmull-Rom (bicubic as the alternative).
3. **Neighbourhood clipping.** Variance clipping in YCoCg (Salvi), with gamma about
   1.0–1.25, against an AABB clamp. The neighbourhood is computed in perceptual space
   (below).
4. **HDR handling.**
   - Inputs are AP1 scene-linear. They are pre-exposed with the **previous frame's
     adapted exposure** (`exposure.previous`, or manual EV), so weights, clipping and
     flicker tests run in a display-like range.
   - Karis inverse-luma weighting is applied to the neighbourhood and the blend.
   - Stored history stays scene-linear: the exposure is removed again.
5. **Disocclusion and rejection.**
   - Depth reprojection against previous depth. The candidates are a
     `depth.history` pair (R32F, `OnCut`) and velocity-history disagreement; choose
     by evidence.
   - Off-screen previous UVs and "no history" velocity reset that pixel.
6. **Blend.**
   - A base current-frame weight of about 0.04–0.1, raised by velocity magnitude
     (sharpness under motion), by the reactive mask (M9.3) and by disocclusion.
   - Anti-flicker: history-luma contrast tracking in history alpha.
7. **Sharpening** (optional, measured). A contrast-adaptive sharpen, or a negative-lobe
   reconstruction filter, compared at equal stability.
8. **Candidates, each needing evidence:**
   - a negative texture LOD bias (−0.5 to −1.0) applied through the material sampler
     set;
   - specular anti-aliasing (normal-variance roughness filtering) as a complement to
     TAA, not a substitute.
9. **Overlays.** The selection mask is unjittered (G5). The grid is drawn in output
   after TAA. Both are checked for shimmer by eye.
10. **SR input contract** (`renderer/rhi/TemporalUpscaleInputs.h`), backend-neutral
    data:
    - colour, depth, motion (units, scale, sign convention);
    - exposure (buffer or value), reactive and transparency-composition masks;
    - jitter (pixels, sequence length), render extent and output extent;
    - reset flag, camera near/far/vertical FOV, frame time delta, sharpness.

    Providers implement `IVulkanTemporalResolveProvider`; native TAA is provider 0.
    M9b adds providers and render-extent scaling without changing the inputs.

**Settings.**
- `--anti-aliasing none|taa`. The default stays `none` until feature-tier admission,
  then becomes `taa`.
- `--temporal-jitter auto|off|on`: `auto` follows TAA.
- Editor: Project Settings > Anti-aliasing. `ApplicationConfig` owns the setting,
  like output settings; components hold no ImGui state.

### Reactive and transparency (M9.3)

Moving glass, particles and emissive need history suppression where transparency
dominates. Candidates, by cost:
1. **WeightedOIT:** reactive = 1 − revealage, sampled from the existing revealage
   target. No new writes.
2. **Sorted and compatibility transparency:** an R8 `temporal.reactive` target with
   max-blend. It is written either by a second attachment in the sorted and
   compatibility passes (a separate `TransparentReactive` target class, so ordinary
   transparent pipelines stay single-colour), or by a coverage-only draw.
3. **Layered tiers:** reactive from the composed interface coverage.

- Opaque emissive pixels use luminance-based current-frame weighting, not the mask.
- The mask also feeds the SR contract's transparency-composition input.
- The choice is made with ghosting metrics on the moving-glass and particle fixtures.

### Auto-exposure (M9.5, before bloom)

Two passes in a `VulkanExposureFeature` owner:
- **`post.exposure.histogram`:** compute over the resolved colour. 128 log2-luminance
  bins over a configurable EV range, workgroup-shared atomics (counts only;
  order-independent), and optional centre-weighted metering.
- **`post.exposure.adapt`:** one workgroup.
  - It applies percentile clipping (defaults 10–90%, as in UE), EV100 min/max limits,
    and separate up/down adaptation speeds in EV/s using the **per-view** time delta.
  - Exposure compensation is the existing manual EV.
  - It writes `exposure` (a 16 B History buffer pair, `SurviveCut`): adapted log2
    luminance, multiplier and EV100.
  - With invalid history (first turn of a view, rebuild), it adapts instantly to the
    target; there is no fade-in.

Exposure modes:
- **`Manual`:** today's behaviour. The frozen set and benchmark fixtures pin it.
- **`Auto`.** The output pass reads exposure through a new storage-buffer binding.

Exposure never enters UI. No CPU readback is needed. A qualification readback reports
the EV100 and histogram per frame.

### Bloom (M9.4)

`VulkanBloomFeature` replaces the inactive `bloom-hook` with `post.bloom`:
- a dual-filter chain: a 13-tap downsample with Karis averaging on the first level only
  (anti-firefly), then a tent upsample;
- about six levels from half resolution;
- one transient mipped image (RGBA16F; B10G11R11 is a measured candidate), declared as
  a whole-resource write, so it aliases;
- mip barriers inside the pass, per ADR-0016 item 6.

The output composite is energy-conserving:
`colour = lerp(sceneLinear, bloom, intensity)`, with a physically small default
intensity. There is no threshold by default; a threshold or knee is an explicit
option. Output reads it through a new binding, so the output set goes from four
bindings to six: bloom and exposure. Bloom is exposure-independent, because it is
linear.

### Robustness (M9.6)

| Event | Expected behaviour |
|---|---|
| Resize, transport switch, aliasing or topology rebuild | All history invalid, one unconverged frame with no flash. Exposure adapts instantly, and TAA starts from the current frame. |
| Zero extent | History untouched; no TAA or post passes declared. |
| Cut or teleport (view) | TAA and depth/velocity history invalid; exposure survives. |
| Object teleport, new instance, LOD change | Per-pixel "no history" motion; TAA rejects that pixel locally. |
| Editor view switch | Each retained view keeps its own TAA and exposure history. |

The editor checks both views, and the asset-preview session changes. LOD stays
default-off and workload-selectable; LOD transitions are tested on F7-lod with TAA on.

## Vertical slices

Status values: `Planned`, `In Progress`, `Accepted`. One slice is `In Progress` at a
time on the integration branch; parallel lanes run in their own worktrees.

| Slice | Tier | Summary |
|---|---|---|
| M9.0 | — | This plan and the audit (`Accepted` 2026-10-05) |
| G1 | refactor | View supplied before the first pass; one `ViewHistoryContext` type. **Accepted** `725d5b4` |
| G2 | refactor | Per-view History sets, per-pair reset policy, `ViewMotionTracker` cut detection, ADR-0016 note. **Accepted** `70f7397` |
| G5a | refactor | Shared `view_uniforms.glsl`; offset asserts; ABI tests for every declarer. **Accepted** `92ded54` |
| G5b | refactor | Jitter, previous view-projection and temporal-info fields; per-view counter; Halton; zero-jitter identity. **Accepted** `56132eb` |
| G3 | refactor | Publisher settle; upload-byte report. **Accepted** (teleport flag moved to M9.6) |
| G4 | refactor | Previous transforms for opaque direct and forward-opaque packets (GPU-scene tables or identity-keyed cache). **Accepted** `1e559b5` |
| G6a | tooling | Five-process feature-admission runner (lane). **Accepted** `ee0fd4f` |
| G6b | tooling | Engine-authored temporal geometry, composition scene factory, camera paths, M9 fixture set (lane). **Accepted** `ce24794` |
| G6c | tooling | Capture sequences, post-TAA capture domain, jitter metadata, accumulation reference. **Accepted** |
| G6d | tooling | Temporal metrics tool (lane). **Accepted** `3c8d8b8` |
| G7 | refactor | Probe-promotion race: qualification finalize drain or warm-up past publication. **Accepted** (qualification finalize drain) |
| G8 | feature | Deterministic opaque compaction; new feature-set hashes. **Accepted** `9dc44fe` |
| M9.1 | refactor (images) | Velocity target, writers, debug view, oracle; GPU cost and bandwidth. **Accepted** |
| M9.2 | feature | Native TAA feature owner, SR contract, overlays; quality candidates. **Accepted** (M9.2a–d; default on) |
| M9.3 | feature | Reactive and transparency handling. **Accepted** (revealage-alpha reactive mask) |
| M9.5 | feature | Auto-exposure. **Accepted** (default auto) |
| M9.4 | feature | Bloom. **Accepted** (default on, subtle; Karis auto) |
| M9.6 | feature | History invalidation and robustness matrix. **Accepted** (M9.6a/b) |
| M9.7 | feature | Admission, F6 re-measure, completion report, M9-to-M7.9 hand-off, M9b definition. **Accepted** 2026-10-06 |

Exposure (M9.5) comes before bloom (M9.4): TAA's HDR weighting reads the previous
exposure, and the two are otherwise independent.

### G1 — View-keyed history supply
- **Files:** `RenderGraph.h`, `rhi/Mesh.h`, `VulkanRenderGraphExecutor.{h,cpp}`, `VulkanVertexBackend.cpp` (`submitFrame`), `RenderExtractor.cpp` (`finalizeView`), and tests.
- **Change:**
  - one type, re-exported;
  - an explicit identity everywhere;
  - `beginViewExecution(view)` before the first pass;
  - `beginFrameExecution(frame)` no longer re-keys.
- **Tests:**
  - History unit and device tests, plus a new "view supplied before first pass" test;
  - Hi-Z and LOD tests unchanged.
- **Evidence:** refactor tier: frozen set byte-identical with `--validation-sync`, Debug and Release tests, the shipping preset, one T-F1/T-F7 timing pair.
- **Done when:** no production frame keys history with `{0,0}`.

### G2 — Per-view History and reset policy
- **Files:** the graph tracker, executor history storage (per view set), `RenderGraphBuilder::createHistory` (policy), `ViewMotionTracker` (extraction), editor "frame selection" and teleport hooks, and ADR-0016.
- **Tests:**
  - two-view alternation keeps both sets valid;
  - a preview session change invalidates only set 1;
  - a cut invalidates `OnCut` but not `SurviveCut`;
  - rebuild invalidates all;
  - a device test under sync validation;
  - an allocation test proving steady view alternation allocates nothing;
  - a qualification-only test pair so the policy is exercised before TAA exists.
- **Evidence:** refactor tier. Production still declares no History until M9.2, so images cannot change.
- **Done when:** validity follows the C1 rule and ADR-0016 records it.

### G5a — Shared view-uniform include
- **Evidence:** refactor tier, and frozen captures must be byte-identical.
- **Done when:** one GLSL declaration exists and ABI tests cover all seven users.

### G5b — Jitter ABI
- **Files:** `Mesh.h`, `VulkanViewUniforms.cpp`, `view_uniforms.glsl`, the raster vertex shaders, the reconstruction sites (lighting body, complex body, VSM mark), output grid, `RenderExtractor` and `ViewMotionTracker`, the CLI flag, and the capture metadata (jitter index, offset, sequence; `reconstruction_mode`).
- **Tests:**
  - ABI offsets;
  - a Halton sequence unit test;
  - a counter-reset-on-cut test;
  - a test that culling and Hi-Z inputs are unchanged under jitter (`projectionRevision_` stable).
- **Evidence:** refactor tier with jitter off. One diagnostic run with `--temporal-jitter on` and TAA off checks visually that the image only shifts sub-pixel and that culling and shadow counters are unchanged.
- **Done when:** jitter is available and the zero-jitter route is byte-identical.

### G3 — Settle publication
- **Files:** `GpuScenePublisher.{h,cpp}`, the observation flag for discontinuous changes, `ReferenceGpuScenePublisher`, and the replay tests.
- **Tests:**
  - stop → settle on the next pass → steady;
  - two-frame slot coverage;
  - teleport → HistoryReset;
  - no fast-path regression on unchanged frames;
  - the extraction verifier passes.
- **Evidence:** refactor tier. Report the upload-byte delta on a moving-object fixture.
- **Done when:** previous means "last frame" for every GPU-scene instance.

### G4 — Previous matrices for direct packets
- **Files:** `CanonicalMeshPushConstants` (padding semantics), `canonical_material.vert`, forward set-4 binding, `PreviousWorldCache` (extraction), the direct-previous stream (feature owner), the OIT instance stream.
- **Tests:**
  - matrix-only owners' previous equals their last-frame matrix;
  - identity keyed, so it survives ECS index reuse.
- **Evidence:** refactor tier.
- **Done when:** every opaque depth writer can derive velocity.

### G6a — Feature-admission runner (lane, `tools/m9/Run-FeatureAdmission.ps1`)
- Five fresh native-4K Release processes per side, interleaved A,B,B,A,A,B,B,A,A,B, with 500 + 10,000 frames.
- Records machine state (GPU clocks and power, background processes, driver).
- Reports median, p95 and p99 per run and the median of run medians, pass GPU ranges per run, memory, counters and allocations.
- Extends `Summarize-Profiles.py`; `Run-TimingPair.ps1` is not changed.
- Includes a pass-range bisection report for the F6 watch item.

### G6b — Temporal fixtures (lane)
- **Geometry.** `tools/m9/Generate-TemporalAssets.py` (standard library only) writes deterministic **engine-authored** glTFs with embedded buffers and procedural textures to `assets/benchmarks/m9/`:
  - a fence or grille and wires;
  - an alpha-mask foliage card set with a procedural leaf texture;
  - a high-frequency stripe/checker plane;
  - a curved high-specular body (sphere/torus with a low-roughness conductor);
  - a moving occluder slab;
  - an emissive bar;
  - a glass panel;
  - a super-bright small-highlight array.

  Generator and outputs are committed; both are engine-authored.
- **Scene factory.** `scene_factory.kind = "composition"`: a list of `{source_asset, transform, motion}` entities, where motion is a linear path, a rotation or keyframes with a teleport step.
- **Camera.** `camera_motion.path`: keyframes plus multiple cuts.
- **Parsed-field tests** in `BenchmarkManifestTests`.
- **Fixture set** (`tools/m9/M9Fixtures.ps1`):

| Fixture | Content |
|---|---|
| TF-thin | thin geometry |
| TF-foliage | alpha-mask foliage cards |
| TF-disocclude | moving occluder |
| TF-pan | fast pan, then a cut |
| TF-emissive | moving emissive |
| TF-glass | moving glass |
| TF-specular | curved high specular |
| TF-static | stability |
| TF-hdr | super-bright highlights |
| TF-teleport | object teleport |
| TF-dual | editor-like view alternation; harness-driven |

### G6c — Capture sequences and the accumulation reference
- **Capture sequences.** `--capture-frames A:B[:step]` produces one artifact per frame, and the report accepts N captures.
- **New domain.** `--capture-point scene-resolved` captures post-TAA and pre-bloom.
- **Capture metadata** records:
  - the jitter index, offset and sequence;
  - `reconstruction_mode` (`none_native` / `native_taa`);
  - the history-valid flag;
  - the adapted EV100.
- **Accumulation reference.** `--qualification-accumulation-reference N`:
  - holds the benchmark state at a frame and renders N jittered frames with TAA off;
  - accumulates the scene-linear colour in an FP32 qualification image through a declared qualification hook pass;
  - captures the mean as a supersampled reference (box filter over the pixel; 64–256 samples).
- **Ownership.** This lives in qualification, apart from the declared hook pass and the CLI.

### G6d — Temporal metrics tool (lane)
`iridium_temporal_metrics`, a C++ tool target with no new library. It reads PFM and TGA
and reports:
- **Reference error:** log-luminance RMSE and HDR-VDP-like simple metrics in
  scene-linear; PSNR and SSIM in final SDR.
- **Static stability:** mean and p99 per-pixel frame-to-frame absolute luma delta, and
  temporal standard deviation over K frames, with a flicker map.
- **Ghosting:** error energy in a moving-object trail mask, against per-frame
  references.
- **Disocclusion recovery:** frames until the disoccluded-region error falls below
  threshold.

Python is avoided here because numpy is not installed and pure Python is too slow at
4K × many frames.

### G7 — Probe-promotion race
A qualification-only finalize drain makes the probe capture publish at a
deterministic frame on probe routes. If that needs a core hook, use a warm-up past
publication instead. The drain is never in production.
- **Evidence:** probe counters stable over five repeats on V02/O03/R00–R03; images identical.

### G8 — Deterministic opaque compaction (feature tier)
Replace the atomic append with an order-preserving compaction: a per-workgroup
ballot/prefix, a per-bin scan and a stable write. Alternatives are a fixed-slot or a
post-sort design, chosen by measured cost.
- **Evidence:**
  - F3 and F7-lod are deterministic over eight repeats;
  - a new feature set `m9-g8` records their new hashes, and R0 is untouched;
  - every other frozen fixture stays identical;
  - five-process T-F7 and T-F1 cost within noise.

### M9.1 — Motion vectors
As designed above.
- **Evidence:**
  - final images byte-identical (refactor tier for images);
  - the analytic-motion oracle passes;
  - the debug view is inspected on TF-pan, TF-disocclude and TF-teleport;
  - GPU cost and bandwidth (G-buffer pass delta, memory) on one timing pair, with T-F7 included.

### M9.2 — Native TAA
- **M9.2a:** the core resolve (reprojection, Catmull-Rom, YCoCg variance clip, HDR weighting, depth rejection) and the SR contract.
- **M9.2b:** anti-flicker, the velocity-weighted blend, sharpening, the LOD bias and specular-AA candidates, jitter length 8 vs 16.
- **M9.2c:** overlays and the editor settings.
- **Evidence** (feature tier):
  - TF set metrics against TAA-off and the accumulation reference;
  - stills and motion sequences inspected;
  - the five-process cost against 0.40 ms;
  - memory;
  - the TAA-off frozen route byte-identical.

### M9.3, M9.5, M9.4, M9.6
As designed above; each is feature tier with its own fixture metrics and five-process
cost.
- **Budget:** exposure plus bloom plus output ≤ 0.50 ms on the heaviest transport.
- **Exposure:** adaptation curves on TF-hdr and TF-pan cuts.
- **Bloom:** energy conservation (integrated luminance before and after bloom within
  1% at intensity 0) and firefly stability on TF-hdr.

### M9.7 — Qualification and hand-off
- **Admission:**
  - five-process native-4K admission for TAA, bloom and exposure (each on its own and combined);
  - the shipping preset with production defaults.
- **Identity and validation:**
  - the frozen-set TAA-off byte identity;
  - validation and synchronization validation over the M7R and M9 sets.
- **Smoke tests:**
  - the editor smoke test with dual views;
  - the hitch and starvation reruns.
- **Watch item:** the F6 re-measure and the placement-policy decision.
- **Defaults:** TAA, plus bloom and exposure per the owner decisions, become defaults.
- **Documents:** the completion report, the M9-to-M7.9 hand-off (including M9b), ROADMAP (M9 Accepted, M9b defined), FRAME_BUDGET, and the PROJECT_CONTEXT "Current direction" section.

## Delegation and integration

- **The lead owns:**
  - the graph and executor (G1, G2);
  - the view ABI and common shaders (G5);
  - the publisher (G3) and G4;
  - CMake;
  - integration and acceptance.
- **Lanes, each in its own worktree:**
  - **Lane T** (tooling): G6a, then G6d. Touches only `tools/m9/`, plus one CMake target, added by the lead at integration.
  - **Lane F** (fixtures): G6b. Touches `src/benchmarks`, `src/qualification/harness`, `assets/benchmarks/m9` and the manifest tests.
  - **Lane S** (isolated shaders): the TAA resolve, bloom and histogram compute shaders, developed against fixed interfaces after G5b. The lead integrates the feature owners.
- **Read-only audits** run in parallel freely. There are never two concurrent writers on the graph, executor, view ABI, common shaders or CMake.
- **Merging.** Each lane is merged by the lead after review against this plan and current source. Each accepted slice is committed with a message naming it, then pushed; the PR is kept updated.

## Verification

| Item | Requirement |
|---|---|
| Builds | Always via `tools/m7r/Build.ps1`: `x64-debug`, `x64-release`, `x64-release-shipping` |
| Tests | Debug and Release, currently 113; shipping, currently 106. New tests per slice. |
| Frozen set | `tools/m7r/Run-FrozenCaptures.ps1 -SyncValidation` on the TAA-off route against `captures/r0` |
| M9 set | `tools/m9` fixtures with measured envelopes; `--require-capture-signal`; sequences inspected by eye |
| Timing | Refactor: one matched native-4K pair (`Run-TimingPair.ps1`). Feature: `Run-FeatureAdmission.ps1`. Machine state recorded with every run. Bisect by pass GPU ranges before attributing a delta. |
| Allocations | 0 steady on the timing routes (`--qualification-allocation-trace` on regression) |
| Oracles | Extraction verifier, caster-revision oracle, indirect digests, sweep |
| Memory | Persistent and transient by category; history per view set; alias-plan savings |

Hardware: RTX 4090, i9-14900K, Windows 10.0.26220, MSVC 19.51, Vulkan SDK 1.4.335.
Captures and timing run at native 3840x2160, Release, borderless hidden window, SDR
unless the route names a transport.

## Risks, fallback and rollback

| Risk | Mitigation |
|---|---|
| Zero-jitter output not byte-identical (FMA contraction) | Specialization-constant fallback for the TAA-off route (G5b) |
| TAA over 0.40 ms | One compute pass, groupshared neighbourhood, measured options; quality options are toggles with their own cost |
| Ghosting on glass/particles | Reactive candidates compared on metrics (M9.3) |
| History memory in dual editor mode (≈ +133 MB at 4K for set 1) | Allocated only when the second view first renders; released at rebuild after dual mode ends |
| Alias-plan reshuffle moves GPU placement (F6 watch) | Quiet machine, pass-range bisection, F6 re-measure; placement policy behind `VulkanResourceAllocator` only if a placement-attributed regression above 1% remains |
| Velocity precision at extreme motion | RG32F reference comparison; format decision logged |
| Executor file size | History view sets live in a new `VulkanHistoryResources` helper, not inline in the executor |
| Product regression | `--anti-aliasing none`, exposure `Manual` and bloom off stay supported; defaults change only after admission |

## Owner decisions (2026-10-05)

1. The plan is approved as written.
2. **Auto-exposure default after admission: `Auto`** for the editor and runtime. Manual EV becomes exposure compensation, and `Manual` stays selectable. Benchmark and qualification fixtures pin `Manual`.
3. **Bloom default after admission: on**, energy-conserving, at a physically small intensity (about 4% scatter), with no threshold. Intensity and an optional threshold or knee are adjustable.

## Decision log

| Date | Decision | Evidence |
|---|---|---|
| 2026-10-05 | PR #7 is merged (`da8e4e8`); `m9-temporal` branched from `Render-Refactor-for-Modularity`. | `gh pr list`; `git log` |
| 2026-10-05 | Audit corrections C1–C14 recorded. They change the design of G2 (per-view-turn validity), G5 (shared include first), M9.1 (fixed pipelines, ForwardOpaque class), G3 (publisher-side settle) and G6 (composition factory, sequences). | Four read-only audits against `da8e4e8` |
| 2026-10-05 | Jitter lives in new UBO fields; `proj`/`inverseProjection` stay unjittered. Splitting the fields keeps every culling, Hi-Z, cluster and CPU consumer unchanged. | Jitter audit consumer table |
| 2026-10-05 | Owner approved the plan as written. Defaults after admission: auto-exposure `Auto` (manual EV becomes compensation; fixtures pin `Manual`), and bloom on, subtle, no threshold. | Owner, in the lead session |
| 2026-10-05 | Metrics tooling is a C++ tool, not Python with numpy (not installed; no new library). | `python -c "import numpy"` fails |
| 2026-10-05 | **G1 accepted** (`725d5b4`). One `ViewHistoryContext` type (`renderer/graph/ViewHistory.h`; identity 0 = no view). `beginViewExecution` keys History before the first pass. Frozen `m9-g1` with `--validation-sync`: 24/24 identical or within the R0 envelopes, 0 messages, 0 hazards. Debug/Release 113/113, shipping 106/106. The matched timing pair is deferred to a combined G1+G2 pair: lanes were building in parallel, so the machine was not quiet, and G1 adds one call per frame. | `out/m7r/captures/m9-g1` |
| 2026-10-05 | **G6a accepted** (`ee0fd4f`, Lane T): `tools/m9/Run-FeatureAdmission.ps1` and `Summarize-Admission.py`. The lane's worktree started from `main`; it rebased its branch onto `m9-temporal` before committing. Later lane prompts name the base explicitly. | Lane T smoke runs (50 frames) |
| 2026-10-05 | **G2 design:** <ul><li>History validity has **no staleness limit** across a view's turns. A frame-count gap would depend on editor cadence (a 30 FPS background view beside a 1,000+ FPS foreground). Wall time would make validity nondeterministic. TAA rejection handles a stale turn like any scene change, and exposure uses a per-view time delta.</li><li>A **field-of-view change is not a cut**: velocity uses previous and current view-projections, so it is reprojectable. The plan's earlier "projection change" cut is narrowed to kind and extent.</li><li>Discontinuity thresholds start at 10 m or 45° per turn, to be tuned on TF-pan/TF-teleport.</li><li>A scene load is not yet a cut; it is tracked for M9.6.</li></ul> | G2 implementation |
| 2026-10-05 | **G2 accepted** (`70f7397`). Frozen `m9-g2` (sync validation): 24/24 identical or within the R0 envelopes, 0 messages. Tests: Debug/Release 114/114 (+`ViewMotionTests`), shipping 107/107. New tests: tracker view sets and reset policy; executor view sets (set 1 created lazily, distinct images, rebuild retires it); a device test with two alternating views; steady alternation allocates nothing. | `out/m7r/captures/m9-g2` |
| 2026-10-05 | **G5a accepted** (`92ded54`). Frozen `m9-g5a`: identical or within the envelopes, 0 messages. | `captures/m9-g5a` |
| 2026-10-05 | **G5b design: jittered *matrices*, not a shader offset.** The UBO and record carry `jitteredProjection` and `jitteredInverseProjection`, which with jitter off are the unjittered matrices bit for bit (copied, not recomputed). Raster stages and reconstruction swap which field they read instead of adding arithmetic, so the TAA-off route is byte-identical by construction; no specialization-constant fallback was needed. Deferred lighting receives the raster projection (its inverse is computed from it). The selection mask uses an `IRIDIUM_UNJITTERED_RASTER` vertex variant. The sequence index is the view's turns since its last cut. `--temporal-jitter on\|off` (default off; TAA will drive it in M9.2). **Not yet handled:** the layered atlas request rects need about ±1 px of margin under jitter, which M9.2 adds when TAA turns jitter on. | G5b implementation |
| 2026-10-05 | **G5b accepted** (`56132eb`). Frozen `m9-g5b` (jitter off, sync validation): identical or within the envelopes, 0 messages. Diagnostic with jitter on (`m9-g5b-jitter-on`, F1/F7-hiz): only edge pixels change (2–3% of pixels inside object bounds, max abs 0.049 scene-linear), validation is clean, and visibility, LOD and Hi-Z counters are identical with jitter on and off (T-F1, T-F7). | `captures/m9-g5b*`; `out/m9/timing/g5b-jitter-counters` |
| 2026-10-05 | **G6b accepted** (`ce24794`, Lane F):<ul><li>`scene_factory.kind = composition`: entities are top-level nodes of one generated glTF, as non-owning per-node `ModelAsset` views with stable UUIDs.</li><li>Motion kinds none, linear, rotation and keyframes (with teleport).</li><li>`camera_motion.path` with multiple cuts.</li><li>Ten engine-authored fixtures (`assets/benchmarks/m9`, generator `tools/m9/Generate-TemporalAssets.py`, byte-deterministic).</li><li>Manifest tests 36. Frozen fixtures unchanged. Captures were inspected by the lane: content visible, motion and cuts present.</li></ul>**Known:** glass tint and absorption read only about 1–2% (as in the M6 reference fixture); composition views are built once at startup; `tests/assets/**` lacks an `eol=lf` rule (fresh worktrees with autocrlf fail two SHA tests). | Lane F report |
| 2026-10-05 | **Timing pair G1–G5b** (`m9-g1-g5b`: A = M7R final `da8e4e8`, B = `ce24794`; A,B,B,A, 500 + 10,000, native 4K).<ul><li>T-F1 GPU 1.0852 → 1.0856 ms (+0.04%); CPU and non-wait CPU within noise.</li><li>T-F7 GPU 1.8930 → 1.9037 ms (+0.56%). By pass: `forward.opaque` +6.7 µs, `lighting.deferred` +3.1 µs, `transparency.sorted.forward` +2.0 µs, `refraction-pyramids` −1.0 µs. The deferred-lighting shader and its inputs are unchanged, so at least part of the shift is placement or environment, not work. Both A runs and both B runs agree within ±0.5 µs per pass.</li><li>Accepted as a watch item, consistent with the director's F6 decision: re-measure after M9.1, which reshapes G-buffer memory anyway, and at M9.7.</li><li>**Machine state:** quiet preflight, P0, High Performance plan. `cpu.renderer.present` is about 3.4 ms on both sides (the display was probably asleep), so CPU frame times are not comparable with M7R's recorded 1.9 ms; non-wait CPU and GPU are. The same effect appears with A = M7R final, so it is environmental.</li></ul> | `out/m7r/timing/m9-g1-g5b`; per-pass via `Summarize-Admission.py` |
| 2026-10-05 | **G3 design and acceptance.** The settle touches only slot 2d+1 and its revision. `GpuSceneInstanceHistoryReset` semantics are **unchanged**: instance flags feed the shadow/probe membership revisions (state word z), so clearing a flag would re-render cached shadows and recapture probes. Velocity does not need the flag: new or teleported instances have previous = current, which gives zero object motion, and TAA's depth and neighbourhood rejection handle the pixels. The explicit **teleport flag moves to M9.6**: unflagged and flagged teleports behave the same under TAA's rejection, and motion blur (a later slice) is its main consumer. The fast path is skipped only on the pass after a transform change. Frozen `m9-g3`: identical or within the envelopes, 0 messages. On T-F5/T-F6 (continuous motion) counters are unchanged except the new `gpu_scene.transform.settled` (median 0). The upload increase is 48 B per stopping instance per frame slot, once. | `captures/m9-g3`; `out/m9/timing/g3-upload-counters` |
| 2026-10-05 | **G4 design:** <ul><li>Previous transforms are spans parallel to the opaque direct packets and the forward-opaque queue, so `DrawPacket` stays 240 B and sort cost is unchanged.</li><li>GPU-scene-backed packets read slot 2d+1 from the published tables.</li><li>Other packets use `PreviousTransformCache`, keyed by (owner UUID, primitive GUID), with sorted flat arrays: no steady allocation.</li><li>**Transparent queues carry no previous transform.** Transparency writes no velocity; it feeds the reactive mask (M9.3). Resolving about 4k transparent packets per frame on F7 through the cache would cost CPU for no consumer. OIT instance batches likewise.</li><li>The GPU stream that hands direct-packet previous transforms to the vertex stage lands with its consumer in M9.1.</li></ul> | G4 implementation |
| 2026-10-05 | **G6c accepted** (Lane C):<ul><li>`--capture-frames FIRST:LAST[:STEP]` streams images as readbacks complete. Peak working set stays about 1 GB at any length (48-frame 4K run).</li><li>`--benchmark-hold-frame F` evaluates all benchmark state at min(frame, F).</li><li>`render_configuration.temporal_jitter` in capture sidecars records the captured frame's own jitter (verified by re-shifting: the recorded offsets explain the jittered/unjittered difference about 5x better than any other sign).</li><li>`tools/m9/Run-TemporalCaptures.ps1`, including `-Reference`: held, jittered N-sample captures averaged by `IridiumTemporalMetrics accumulate`.</li><li>The single `--capture-frame` path reproduces the frozen hashes.</li></ul>**Decision:** `AppFrameContext` gains a read-only `const RenderFrame* renderFrame`, set only around `submitFrame`, so observers can read the frame they capture. This extends the qualification observer contract, not a core runtime interface. The post-TAA capture domain moves to M9.2 with its producer. Image writes take about 2.5 s per 4K PFM, so a 256-sample reference takes about 10 min. | Lane C report; `QualificationHarnessTests`, `CaptureArtifactTests` |
| 2026-10-05 | **G6d accepted** (`3c8d8b8`, Lane D): `IridiumTemporalMetrics` with `reference-error`, `stability`, `ghosting`, `recovery` and `accumulate`. It uses tone-mapped luma Y/(1+Y) and SSIM 11x11 Gaussian, is deterministic across thread counts, has 15 unit tests, and computes a 4K scene pair in 11–21 ms after a load of about 100 ms. Thresholds (log epsilon 1e-4, 1/64, 1/255, trail 0.02) are uncalibrated until real TAA sequences exist. | Lane D report; `tools/m9/Temporal-Metrics.md` |
| 2026-10-05 | **M9.1a design:**<ul><li>Velocity is RG16F in UV units, current minus previous, both unjittered (`include/motion_vectors.glsl`), with "no history" = 4.0 (off screen).</li><li>Direct draws push the previous transform in `CanonicalMotionPushConstants` (144 B) on the G-buffer and forward layouts only. Shadow, probe and layered layouts keep 80 B. The device limit is checked at init; every supported desktop GPU has 256 B. This is simpler and allocation-free compared with a per-frame storage stream, which would need mid-frame descriptor growth.</li><li>Forward pipelines that write depth are forward-opaque: two colour targets and velocity shader variants (`canonical_material_velocity_vert`, `complex_opaque_material_velocity_indexed_frag`). The asset-side `RenderPassClass` and cooked pipeline state are unchanged; the Vulkan library routes by `depthWrite`.</li><li>Forward passes skip pipelines whose colour count does not match.</li><li>Velocity is always written, including on the TAA-off route, so its cost is paid there too. A conditional target would double the G-buffer pipeline set.</li></ul> | M9.1a implementation |
| 2026-10-05 | **M9.1a accepted** (`9c35190`):<ul><li>Frozen `m9-m91a` (sync validation): identical or within the envelopes, 0 messages.</li><li>Tests 115/115 (shipping 108).</li><li>Contract tests updated deliberately: the R4b.3 aliasing golden is regenerated (+1 slot per topology); the R3b.6 golden filters resources declared since; the 4K transient model is 597.6 MB requested and 431.6 MB peak (was 564 and 398).</li></ul> | `captures/m9-m91a` |
| 2026-10-05 | **M9.1b:** `RenderDebugView::MotionVectors` in the output pass (binding 4): direction is hue and log2 pixels is brightness, black is still and magenta is no history. The output pass reads velocity, so velocity stays live until output-transform (aliasing digests regenerated). | M9.1b implementation |
| 2026-10-05 | **Flaky test:** `TaskSystemTests` failed once in Release under `ctest -j8` while other processes were loading the CPU, then passed 3 of 3 alone. It is pre-existing and load-sensitive; M9 does not touch the task system. It is tracked here; if it recurs, it gets a separate fix with its failure output. | Release run during the G6c merge |
| 2026-10-05 | **M9.1 evidence:**<ul><li>Motion debug view inspected on TF-pan (camera pan over static geometry: velocity smooth with depth, continuous across surfaces; empty background zero by design, since TAA reconstructs camera motion there) and TF-disocclude (the moving slab is one uniform motion colour, everything static is black, edges clean).</li><li>Frozen `m9-m91b` (sync validation): identical or within the envelopes, 0 messages.</li><li>**Cost** (timing pair `m9-m91`: A = `90abe10` before M9.1, B = M9.1a+b with G6c and G6d; A,B,B,A, 500 + 10,000, native 4K). GPU: T-F1 1.0915 → 1.1366 ms (+0.045 ms) and T-F7 1.9032 → 1.9599 ms (+0.057 ms). By pass: `gbuffer.opaque` +0.011/+0.034 ms (sixth target), `forward.opaque` +0.029/+0.027 ms (second target plus motion varyings), `lighting.deferred` +0.010/+0.003 ms (unchanged shader; placement or noise).</li><li>CPU scopes change by at most 0.012 ms (`record.forward`). The retained-frame non-wait CPU mean (+0.21 ms on T-F7) is not reproduced by any scope; present waits were about 3.4 ms on both sides (display asleep).</li><li>**The velocity target costs about 0.05 ms at 4K and is counted in the TAA row** (0.40 ms).</li></ul> | `out/m7r/timing/m9-m91`; `out/m9/captures/m91-motion-debug` |
| 2026-10-05 | **M9.2a: native TAA, opt-in** (`--anti-aliasing taa`; jitter follows it unless `--temporal-jitter` is given):<ul><li>`VulkanTemporalAntiAliasingFeature` declares `temporal.taa.resolve` (compute, after the last scene-colour writer and the scene capture hook, before `bloom-hook`) and `taa.history` (RGBA16F History, `OnCut`).</li><li>`bloom-hook` and `output-transform` read the resolved colour (`VulkanProductionGraphIds::resolvedSceneColor`); `final-capture-hook` also reads it for the new `--capture-point scene-resolved`. The `scene` capture stays pre-TAA (director decision).</li><li>The TAA and output descriptor sets are rewritten inside their execute callbacks each frame (history parity and view set). Without TAA the graph is the M7R/M9.1 topology, so the frozen route is unchanged.</li><li>`taa_resolve.comp`: a Blackman-Harris-weighted 3x3 reconstruction of the jittered samples; a YCoCg variance box in a pre-exposed, luma-compressed space; closest-depth motion with camera reprojection for background; 5-tap Catmull-Rom history; Lottes anti-flicker; motion-weighted feedback.</li><li>`--taa-settings` exposes the parameters for evidence runs. The parser initially accepted trailing text after six numbers (found by a sweep script); fixed, and the parity test pins it.</li></ul> | M9.2a implementation |
| 2026-10-05 | **TAA v0 against ground truth** (64-sample held accumulation references `ref64-h150`; TAA converged 30 frames on the held scene; tone-mapped RMSE in scene-linear; stability over 8 frames). Error drops in every fixture: TF-foliage 0.0072 → 0.0041, TF-specular 0.0035 → 0.0027, TF-static 0.0256 → 0.0204, TF-thin 0.0297 → 0.0236. Two failures remain:<ul><li>The share of pixels with error above 1/64 *rises* (TF-thin 12.5% → 19.4%).</li><li>A static camera flickers: 3.6% of pixels (TF-static) and 5.9% (TF-thin).</li></ul>By eye, sub-pixel wires stay dotted and thin dark wires vanish. Diagnosis: neighbourhood clipping. A still pixel's 3x3 box follows each jitter phase, so accumulated sub-pixel coverage is clipped away and the clipped history oscillates. Remedy under test: a wider clip box for still pixels (`staticVarianceGamma`), blended to the tight box by one pixel of motion. | `out/m9/eval-taa-v0.json` |
| 2026-10-05 | **G8 accepted** (`9dc44fe`):<ul><li>The cull pass writes one command slot per candidate (empty when culled), and `gpu_scene_compact_bins.comp` (one workgroup per bin, shared-memory prefix scan) compacts each bin in place in candidate order, writes the count without atomics and clears the tail.</li><li>Frozen `m9-g8` (sync validation): identical to R0 except the F3-stress/F7-lod depth-tie pixel (1 px, within envelope), 0 messages. F3-stress, F7-lod and F4-ord2 are byte-identical over 8 runs. **New M9 feature-set reference hashes: `out/m7r/captures/m9-g8/hashes.json`** (R0 untouched). F4-woit still varies (WeightedOIT accumulation order; not the opaque path).</li><li>Five-process native 4K (`m9-g8-admission`, A = `c76cd29`): `frustum_compact` +0.002 ms (T-F1), +0.010 ms (T-F7); T-F7 frame +0.011 ms, matching it.</li><li>The T-F1 frame median moved +0.025 ms because per-process GPU times are **bimodal on both sides** (about 1.122 or 1.152 ms; `forward.opaque` 0.441 or 0.468 ms), and A drew 3 fast processes against B's 1. This is the placement/environment watch item, not G8.</li></ul> | `out/m9/timing/m9-g8-admission`; `out/m7r/captures/m9-g8*` |
| 2026-10-05 | **G7 design** (audit): runtime probe captures promote when a non-blocking serial poll sees them complete, so a capture recorded in frame N publishes at N+1, N+2 or N+3 depending on GPU progress, and every later realtime recapture races again. A warm-up would only remove the first race. **Implemented:** a qualification-only finalize drain.<ul><li>`IQualificationBackend::drainReflectionProbeCaptures` waits for all frames in flight only when a capture is recorded but not promoted (scheduler state, not timing). The harness calls it at `PostSceneUpdate` under `--qualification-probe-finalize-drain`, so a capture in frame N always publishes at N+1.</li><li>No core interface, observer, orchestrator or probe-owner change.</li><li>`Run-QualificationSweep.ps1` gains an additive `-ExtraArgs`; the M7R sweep entries are unchanged, since they are a comparison baseline.</li><li>A steady T-F6-probecap route (300 frames) publishes at the same frames with and without the drain (6, 36, ... 276): the race shows only in cold-start routes. Cold-start repeat evidence is in the G7 acceptance entry.</li></ul> | G7 audit; `out/m9/timing/m9-g7-drain` |
| 2026-10-05 | **TAA tuning, sweeps s2–s3** (TF-static, TF-thin; tone-mapped RMSE against `ref64-h150`; flicker = share of pixels whose temporal std exceeds 1/255 over 8 still frames):<ul><li>A wider still-pixel clip (gamma 3) lowers flicker from 3.6 to 2.0% (TF-static) and from 5.9 to 3.3% (TF-thin).</li><li>A sharper reconstruction filter (exp(-3.0 d^2) instead of 2.29) lowers error and the share of pixels above 1/64 (13.4 → 9.3% on TF-static). The box-filtered reference favours the narrower kernel.</li><li>**Still-pixel history weight** (0.97, replacing the Lottes luma rule, which raised the current weight exactly where jittered sub-pixel samples differ) lowers flicker to 0.36% (TF-static) and 0.73% (TF-thin) at equal error. Higher weights (0.98) flicker less but converge more slowly than the 30-frame window.</li><li>Grille crops match the reference by eye: bars smooth, no streaks.</li><li>**Remaining defect, seen by eye:** horizontal sub-pixel wires vanish and the lower slanted wire stays dotted. In jitter phases that miss a wire, the 3x3 box has almost no variance, so clipping removes the accumulated wire whatever gamma is.</li><li>**Remedy tested in s4:** temporal neighbourhood extents (rejected; see the next entry).</li></ul> | `out/m9/eval-s2-*`, `eval-s3-*` |
| 2026-10-05 | **Temporal neighbourhood extents rejected** (sweep s4). Design: a `taa.extents` RG16F History pair held the compressed-luma min/max a still pixel's 3x3 box had spanned, decaying toward the current box, and still pixels clipped luma against it.<ul><li>Results, decay 0.85–0.93 (all similar; ex90 shown): TF-static RMSE 0.0164 → 0.0276, flicker 0.36 → 2.3%; TF-thin RMSE 0.0202 → 0.0353, flicker 0.73 → 3.7%. Decay 0 reproduced st97 exactly.</li><li>By eye, the wires reappear, but doubled in width and dashed. Once the box spans every jitter phase's value, the history is held by weight alone and keeps whichever phase values it last saw, with no convergence toward their mean.</li><li>**Removed.** No History pair or descriptor bindings remain, and `--taa-settings` stays at seven numbers.</li><li>Sub-pixel wires (a feature thinner than a pixel at 4K) remain a **known M9 TAA limit**. Candidates, deferred and logged as an M9.7 risk: authored or shader minimum-width wire AA (coverage fade), or TSR-style low-frequency history rejection instead of a per-pixel box.</li></ul> | `out/m9/eval-s4-*`; `out/m9/captures/s4-*` |
| 2026-10-05 | **M9.2b accepted** (still-pixel feedback, tuned defaults, SR contract wiring):<ul><li>**Defaults** are now the best measured tuning (st97: reconstruction exp(-3.0 d^2), still-pixel gamma 3, still-pixel history weight 0.97; other values unchanged). `s5-def` reproduces st97 exactly. The tuning is an M9.2 starting point; motion fixtures (next) may revise it.</li><li>**SR contract:** the TAA owner stages one `TemporalUpscaleInputs` per frame: provider NativeTaa, render and output extents, jitter in pixels, sequence length, exposure and previous exposure, reset, camera, and native tuning. M9b providers consume the same struct. `temporalInfo.w` carries the jitter sequence length (previously reserved). `resetHistory` (a view cut) now also discards history the graph still holds as valid.</li><li>The duplicate `VulkanTemporalAntiAliasingSettings` is gone: the backend keeps the RHI `TemporalAntiAliasingTuning`.</li><li>**Evidence:** frozen `m9-m92cd` (TAA off, sync validation) is byte-identical to the `m9-g8` feature-set hashes except F4-woit (known WeightedOIT order) and within the R0 envelopes, with 0 messages. Release 116/116.</li></ul> | `out/m7r/captures/m9-m92cd`; `out/m9/eval-s5-def.json` |
| 2026-10-05 | **G7 accepted** (qualification finalize drain, `--qualification-probe-finalize-drain`):<ul><li>Cold-start probe routes V02-reflection-probes and R00-auto-probe-sdr ran 5x with the drain (`m9-g7-drain-1..5`): every published field, counter and capture hash is identical across the five runs. The profile records the drain wait (`cpu.renderer.drain_all_frames`) only in the drained runs.</li><li>Three undrained repeats (`m9-g7-nodrain-1..3`) also matched one another on this quiet machine, and they differ from the drained runs only in profile scope names. The drain therefore changes no result; it removes the dependence on GPU progress that R4c.3 observed under load.</li><li>Debug and Release 116/116 (parity: qualification options 47, total 101). The M7R sweep entries are unchanged; the drain is opt-in through `-ExtraArgs`.</li></ul> | `out/m7r/sweeps/m9-g7-*` |
| 2026-10-05 | **M9.2c (editor setting), implemented** (`07aadf5`):<ul><li>Project Settings > Anti-aliasing (Off / Temporal). `ApplicationConfig` owns the mode; the editor state holds only the mode and the last diagnostic.</li><li>The orchestrator applies a request at the frame boundary through `IRenderBackend::setAntiAliasing`. Like a scene resize, it retires the frame slots, rebuilds the graph and the targets, and restores the previous mode on failure. Jitter follows the mode unless `--temporal-jitter` pins it.</li><li>Device test (Debug, validation on): TAA on, off, and same-mode no-op with the editor bridge, 0 messages.</li><li>**Open:** the overlay shimmer check by eye in the editor. The grid's occlusion test reads the jittered opaque depth, so its occlusion edge may move by a sub-pixel; the selection outline is unjittered (G5).</li></ul> | Debug tests |
| 2026-10-05 | **M9.6a: scene open cuts view history** (`3b213f8`). `EditorSceneDocumentService::openRevision` advances when a document replaces the world (open or recovery). The orchestrator adds it to the benchmark cut schedule (`viewHistoryResetRevision`), so the tracker cuts and no history blends across scenes. Benchmarks never open documents, so their revision sequence is unchanged. | Debug tests |
| 2026-10-05 | **M9.5 auto-exposure, integrated from Lane E** (`c62e812`, `83e8103`; opt-in `--exposure auto`, default Manual until M9.7 per the owner decision):<ul><li>`VulkanExposureFeature` declares `post.exposure.histogram` and `post.exposure.adapt` only in Auto mode, after TAA and the scene capture hook and before `bloom-hook`. The exposure state is a 16 B History *buffer* pair (`SurviveCut`). Buffer History already worked; a new executor test pins it.</li><li>**Deviation from the plan (accepted):** no global atomic accumulate. Each 128x128-tile workgroup writes its own 128-bin row (shared atomics), and the one-workgroup adapt pass reduces the rows in a fixed order. This is deterministic, needs no clear pass, and costs 510 rows (255 KB) at 4K.</li><li>Metering: EV100 from photometric luminance (K = 12.5), the 10–90% percentile-trimmed mean, clamped to EV100 limits. Linear adaptation at up to 3 EV/s toward brighter scenes and 1 EV/s toward darker ones, over the **per-view** time delta (`ViewMotionResult::deltaSeconds`; a deterministic 60 Hz clock under deterministic content). Output multiplier = 0.18 / adapted luminance x 2^EV compensation. Invalid history adapts instantly.</li><li>Output binding 5 (exposure buffer, mode bit 7). TAA binding 5 with flag `exposure.w` bit 0 pre-exposes current and history with the previous adapted exposure when it is valid. In Manual mode both bind a 16 B fallback buffer the shaders never read.</li><li>Qualification: `--qualification-exposure-trace` (per-frame EV100 and histogram summary through the final-capture hook, persistently mapped per slot, no waits).</li><li>Lane evidence:<ul><li>Frozen Manual set identical to `m9-g8` except F4-woit, with 0 messages and 0 sync hazards.</li><li>TAA with Manual exposure is byte-identical to the pre-lane shaders on TF-pan and TF-thin.</li><li>EV100 traces: TF-hdr adapts instantly to 9.01. TF-pan tracks its target, falls at exactly 1 EV/s, and stays continuous through the cut at 330 (SurviveCut).</li><li>No flash on the cut by eye.</li><li>Zero steady allocations (TAA + Auto).</li></ul></li><li>**Pending for admission:** the five-process cost. The histogram reads full-resolution colour, about 66 MB per frame at 4K; quarter-resolution metering is the fallback candidate if the 0.50 ms post budget is tight. Resize, rebuild, dual-view and preview exposure behaviour is checked in M9.6.</li></ul> | Lane E report; worktree `out/m9/captures/m95-*` |
| 2026-10-05 | **M9.2d: TAA in motion** (motion evaluation tooling `tools/m9/Run-MotionEvaluation.ps1` and `Evaluate-TemporalMotion.py`).<ul><li>**Method.** Every scored frame of a fixture's own motion is compared with a 64-phase accumulation reference *held at that frame* (`out/m9/motion/ref64/<fixture>-h<frame>`). Held benchmark frame H equals measured frame H − warmup byte for byte (verified on TF-disocclude). The trail mask is the set of pixels whose reference changed since the previous frame (ghosting, smear, disocclusion). The same plan without AA is the baseline.</li><li>**Finding: the M9.2b tuning was worse than no AA in motion.** TF-pan steady pan: tone-mapped RMSE 0.034 against 0.025, with 27% against 11% of pixels above 1/64. TF-disocclude: 0.017 against 0.0105, and the trail 0.022 against 0.003. Three causes, each confirmed by eye and by error heatmaps:<ol><li>**Too much history under motion.** The minimum weight was reached only at 32 px, so it was about 0.96 at a few px/frame, and each frame's sub-pixel resample blurred the result further. Fence bars lost contrast.</li><li>**A soft reconstruction kernel.** exp(-3 d²) amounts to σ ≈ 0.41 px against the reference's 1 px box (σ ≈ 0.29), which blurred static texture. Still pixels sample history exactly, so the kernel is the only source.</li><li>**Still-pixel trust applied to pixels whose history is not theirs.**<ul><li>(a) **Disocclusion:** background just uncovered by a moving occluder has zero motion of its own, so it kept the wide box (gamma 3) and the 0.97 weight. A salmon ghost line of the occluder survived along the uncovered box edge.</li><li>(b) **Moving shadows on static ground:** the old shadow faded over about 30 frames.</li></ul></li></ol></li><li>**Fixes:**<ul><li>Defaults: minimum weight 0.70 reached at 2 px of motion, and kernel sharpness 6.0.</li><li>**Velocity disagreement** at zero memory cost: history alpha stores the motion (px) of the content that wrote it. A pixel is treated as moving by \|historyMotion − motion\|.</li><li>**Still-trust gate:** the wide box and the still weight apply only while the history stays within its 3x3 neighbourhood's luma range, up to a relative 0.1–0.3 outside it. Jittered sub-pixel detail stays inside that range; a real lighting change does not.</li><li>A first gate on the distance from the neighbourhood *mean* was rejected: static flicker rose from 0.64% to 4.7%, because high-contrast texture differs from its mean legitimately.</li></ul></li><li>**Result** (`mv-go-sg3`; TAA / no AA):<ul><li>TF-disocclude 0.0099 / 0.0105, pixels above 1/64 6.0% / 6.5%, trail 0.0099 / 0.0029.</li><li>TF-pan steady 0.0206 / 0.0253 (above 1/64 18% / 11%), recovery after the cut 0.0197 / 0.0249.</li><li>Held TF-static 0.0157 (5.6% above 1/64, flicker 0.67%) and TF-thin 0.0183 (7.9%, 1.14%). Against M9.2b: lower error, and fewer pixels above 1/64 (9.5% and 15.4% before); flicker is higher than M9.2b's 0.36% and 0.73%, the price of the sharper kernel.</li></ul></li><li>**Full motion set at the new defaults** (`taa-m92d`, all nine moving fixtures; TAA / no AA, tone-mapped RMSE):<ul><li>Better than no AA: disocclude 0.0097 / 0.0105; teleport 0.0089–0.0105 / 0.0133 (the trail settles within about 6 frames of the teleport); thin 0.0212 / 0.0299; emissive 0.0076 / 0.0123; hdr 0.0067 / 0.0080; pan steady 0.018–0.024 / 0.025; specular 0.0030 / 0.0033.</li><li>**Worse than no AA, open:**<ul><li>TF-glass 0.0136 / 0.0125, trail 0.015 / 0.004: moving glass ghosts, which is M9.3's reactive work as planned.</li><li>TF-foliage 0.0089 / 0.0071, trail 0.023 / 0.012: rotating alpha-tested foliage, also an M9.3 candidate.</li></ul></li><li>**Jitter-phase pulse under motion:** TF-pan alternates 0.018 / 0.0235 between consecutive frames, because 30% current weight passes the sharp kernel's phase-dependent reconstruction through. Candidate: a softer kernel blended in with motion.</li><li>The cut frame itself (no history) is 0.0286 / 0.0248 and recovers on the next frame.</li><li>Held TF-static and TF-thin (`s9-def`) reproduce `go-sg3`.</li><li>Frozen `m9-m92d` (TAA off, sync validation) is identical to `m9-g8` except F4-woit, with 0 messages.</li><li>Debug and Release 116/116.</li></ul></li><li>**Open:** TF-pan still has more pixels above 1/64 than no AA (17–21% / 11%); a sharpening candidate (M9.2 item 7) and the motion-blended kernel are next.</li></ul> | `out/m9/motion/eval-mv-*.json`, `eval-taa-m92d.json`; `out/m9/eval-s6-*`, `eval-s8-*`, `eval-s9-def.json`; `out/m7r/captures/m9-m92d` |
| 2026-10-05 | **Two M9.2 TAA candidates rejected** (motion set against `taa-m92d`):<ul><li>**A softer reconstruction kernel under motion,** to address the TF-pan jitter-phase pulse: sharpness 6 for still pixels, blending to 2.29–4 at one pixel of motion. It is worse at every value. TF-pan steady 0.0206 becomes 0.0211 (k = 4), 0.0221 (k = 3) and 0.0238 (k = 2.29); disocclude is unchanged. The pulse does not come from the kernel. Removed; `--taa-settings` stays at seven numbers.</li><li>**A lighting-change gate from temporal neighbourhood-mean statistics.** A `taa.mean` RG16F History pair held a running average and mean absolute deviation of the 3x3 mean luma, and still trust dropped when the mean left about 3 MAD. This targets moving shadows on high-contrast texture, where the 3x3 range test cannot see the change.<ul><li>Gains: glass trail 0.0147 → 0.0121, disocclude trail 0.0099 → 0.0086.</li><li>Costs: static flicker 0.67 → 0.81% (TF-static) and 1.14 → 1.31% (TF-thin), plus a 66 MB pair and about 66 MB of traffic per frame at 4K.</li><li>Rejected as not worth its cost. Glass ghosting (about 3x no AA) remains M9.3's reactive work.</li></ul></li></ul> | `out/m9/motion/eval-mv-ms*.json`, `eval-mv-mean-def.json`; `out/m9/eval-s10-def.json` |
| 2026-10-05 | **M9.6b: robustness evidence with every M9 post feature on** (`--anti-aliasing taa --exposure auto --bloom on`):<ul><li>Qualification routes V01-residency-churn, V05-ord2-resize, V06-woit-resize, V09-deep-lifecycle, V14-transport-switch, V16-depth-pyramid-resize and X03-select-entity all pass under validation (`m96-post-on`) and under synchronization validation (`m96-post-on-sync`), with 0 messages and 0 hazards.</li><li>The editor-bridge device test now gives retained views their own identity and History set, as extraction does. It alternates two views with TAA (set 1 is created on first use, and the views keep distinct textures), then resizes the scene with TAA on and alternates again, under validation with 0 messages.</li><li>By construction:<ul><li>a resize, transport switch or graph rebuild invalidates every History pair, so TAA starts from the current frame and exposure adapts instantly (Lane E);</li><li>view cuts invalidate TAA (`OnCut`) while exposure survives (`SurviveCut`);</li><li>a scene open cuts (M9.6a).</li></ul></li><li>**Object teleport flag: not needed for TAA.** TF-teleport recovers within about 6 frames through velocity disagreement and the neighbourhood clip, and beats no AA (M9.2d). The flag stays for a future motion-blur consumer.</li><li>Zero extent: the orchestrator skips minimised frames, so History is untouched (unchanged behaviour).</li></ul> | `out/m7r/sweeps/m96-post-on*`; `VulkanBackendFrameTests` |
| 2026-10-05 | **Layered atlas request rects under jitter: no change needed** (closes the G5b note). Ordinary2 and the deep tiers project bounds with the *unjittered* view-projection (invariant) through `projectOrdinary2WorldBounds`. Its guard band (floor − 1, ceil + 1) already leaves at least one pixel beyond the exact bound, and TAA jitter moves raster coverage by at most half a pixel. Evidence: V03-ord2-capture, V07-deep-capture-hero4 and V08-deep-capture-cine8 pass with `--anti-aliasing taa`, 0 messages (`m96-layered-jitter`). | `out/m7r/sweeps/m96-layered-jitter` |
| 2026-10-05 | **M9.3 accepted (reactive), from Lane R** (`9a2a980`..`c6cbb09`, cherry-picked):<ul><li>**Fixture `TF-reactive`** (engine-authored):<ul><li>a frameless thin-glass sheet with attenuation (0.45, 0.7, 1.0) through 2 cm, sliding about 15 px per frame;</li><li>an emissive SortedSurface card (alpha 0.6) and an emissive WeightedOIT card (alpha 0.55), each about 7 px per frame;</li><li>a static camera over the colour-band wall and the high-frequency floor.</li></ul>The lane also found why TF-glass shows no tint: `thicknessFactor 0` makes the volume dormant, so only Fresnel is visible.</li><li>**Baseline:** TAA trail energy 0.017–0.019 against 0.002 without AA. Card rings smeared along their motion and the WeightedOIT card left a bright trail, because pixels under transparency report the background's zero motion and got the still-pixel trust.</li><li>**Mechanism: the reactive mask is 1 − scene-colour alpha.** Blended passes now leave revealage in `scene.color` alpha:<ul><li>opaque writers output 1;</li><li>sorted and compatibility AlphaBlend/Premultiplied, the layered resolve and the WeightedOIT resolve multiply alpha by 1 − coverage;</li><li>colour blend factors are unchanged, so RGB is bit-identical;</li><li>no pass, resource, binding or bandwidth is added, with TAA on or off.</li></ul></li><li>**TAA under the mask** (3x3 max, x4):<ul><li>no still-pixel trust;</li><li>the luma rule compares against the *unclipped* history with 3x rejection and a 0.2 floor (`reactiveHistoryWeight`);</li><li>the history weight is capped where coverage changed, using last frame's reactive stored as negative history alpha;</li><li>non-reactive pixels keep the M9.2d arithmetic bit for bit.</li></ul></li><li>**Rejected:**<ul><li>a coverage weight cap (lowest trail at 0.0035–0.0056, but static flicker under static glass reaches 1.9%);</li><li>a coverage-change-only mask (no gain);</li><li>auto-reactive from refraction pyramid mip 0 (absent on frames without a compatibility queue);</li><li>a dedicated R8 target (not needed).</li></ul></li><li>**Result in the main checkout** (`taa-m93`):<ul><li>TF-reactive tone-mapped RMSE 0.0074–0.0078 against 0.0120 without AA; above 1/64 5.5% against 8.5%; trail 0.006–0.008 against 0.002.</li><li>Held TF-reactive flicker 0.19 → 0.47% (lane).</li><li>The card rings match the reference by eye (lane).</li><li>Non-transparent fixtures are byte-identical to `taa-m92d` (TF-disocclude re-checked).</li><li>Frozen `m9-m93` (TAA off, sync validation) is identical to `m9-g8` except F4-woit, with 0 messages.</li><li>0 sync hazards and 0 steady allocations on TF-reactive with TAA (lane).</li><li>Debug and Release 116/116.</li></ul></li><li>**Contract (new invariant):** scene-colour alpha is revealage, so every opaque writer must output alpha 1. It is documented in `TemporalUpscaleInputs.h` (`reactiveMaskAvailable`).</li><li>**Open:**<ul><li>the WeightedOIT card's trailing halo is the largest remaining error;</li><li>layered-tier and additive transparency have no fixture yet;</li><li>`transparencyCompositionMaskAvailable` is false.</li></ul></li></ul> | Lane R report; `out/m9/motion/eval-taa-m93.json`; `out/m7r/captures/m9-m93` |
| 2026-10-06 | **Owner decision: bloom Karis prefilter Auto** (off with TAA, on without). The Karis weights lose 1.5% of TF-hdr's integrated luminance on sub-pixel highlights (above the 1% bar; 0.04% on ordinary scenes) and only slightly reduce halo flicker once TAA has stabilised the highlights (0.46% against 0.58% of pixels). `BloomSettings::karis` is now `BloomKarisMode{Auto, Off, On}`, default Auto, and `--bloom-settings` takes 0 off, 1 on or 2 auto. The bloom owner reads TAA from the graph it is built against. Verified on TF-hdr: with TAA, Auto is byte-identical to Off; without TAA, Auto is byte-identical to On. Debug and Release 116/116. | Owner answer 2026-10-06 |

### Post-acceptance follow-ups

| Date | Decision | Evidence |
|---|---|---|
| 2026-10-06 | **M9.8a (post-acceptance, owner feedback): authorable auto-exposure and bloom.** The owner reported that exposure "snaps" when flying between dark and bright areas, and that bloom is far too sensitive.<ul><li>**Exposure:** adaptation is now exponential by default (`ExposureAdaptation::Exponential`: each second closes 1 − e^(−speed) of the EV gap, so big changes start fast and ease in). The M9.5 linear curve stopped abruptly at the target. There is an optional max EV/s cap, the defaults are up 2/s and down 1/s, and linear stays selectable. TF-pan trace: the adapted EV100 eases toward its target (about 1.7% of the gap per 60 Hz frame at rate 1), continuous through the cut.</li><li>**Bloom:**<ul><li>a **radius** falloff: level i weighs radius^i, normalised so energy is conserved; radius 1 is the M9.4 equal-weight chain; the default is 0.6, a tight core with a falling tail;</li><li>the **threshold and knee are now in exposed units:** the prefilter reads the adapted exposure state (Auto) or 2^EV (Manual) through a new storage-buffer binding, and the graph declares the `exposure.current` read;</li><li>a linear **tint**.</li></ul></li><li>**Editor:** Project Settings > Post-processing exposes every auto-exposure setting (mode, curve, speeds, cap, EV100 limits, percentiles, centre weighting, histogram range) and every bloom setting (on/off, intensity, radius, threshold, knee, tint, levels, anti-firefly mode), each with a tooltip. All apply live; the exposure mode, bloom on/off and bloom levels rebuild the graph through `IRenderBackend::setExposure`/`setBloom`. Display > Scene exposure is labelled as compensation in Auto mode.</li><li>**Backend:** a `rebuildFrameTargets(restore)` helper replaces the duplicated retire/rebuild/rollback blocks of `setAntiAliasing` and `setBloom` and serves `setExposure`. `VulkanVertexBackend.cpp` is at 2,482 lines.</li><li>**Evidence:** frozen `m98a` (pinned route) identical to `m9-g8` except F4-woit; Debug and Release 116/116; shipping 109/109; the device test switches exposure Manual → Auto → settings → Manual and bloom with radius and tint under validation (0 messages). Bloom by eye on TF-hdr and TF-pan at the new defaults: a tight highlight glow and no haze on the ordinary scene.</li><li>**Open:** settings are not yet persisted (Project Settings are session-only engine-wide; next slice). TAA shimmer (still and moving) is the following slice.</li></ul> | Owner feedback 2026-10-06; `out/m9/m98`, `out/m7r/captures/m98a` |
| 2026-10-06 | **M9.8b: TAA shimmer investigation and authorable TAA tuning** (owner: fine edges crawl on car models, with the camera still and moving).<ul><li>**Measured:**<ul><li>Still camera, close-up Alfa (frozen F2-one, 8 frames): TAA alone changes 0.0005% of scene pixels frame to frame; with product defaults (auto-exposure and bloom), the SDR mean change is 4e-5 and 0.076% of pixels change. Both are far below visibility.</li><li>Held fixtures: TF-specular 0.004%, TF-foliage 0.08%, TF-static 0.67%, TF-thin 1.14% (fine floor and wires).</li><li>Visible editor (400 frames, still camera): no repeated history resets; one cut at the initial viewport settle.</li></ul></li><li>**In motion, a gap in the M9.2d evaluation:** it scored per-frame error only. The new `error-flicker` metric (the change of the error against per-frame references) shows TAA above no AA on moving content (TF-pan 32–36% against 28–32% of pixels; TF-specular 3.9% against 2.1%).</li><li>The metric also counts blur that travels with the content: raising the motion history weight (0.85 at 4 px up to 0.93 at 16 px) scores *worse* (TF-pan 36 → 38%) while RMSE rises (0.020 → 0.030). It is not a valid tuning target under motion; a motion-compensated shimmer measure is future work.</li><li>**Decision:** expose the trade-off instead of guessing.<ul><li>Project Settings > Anti-aliasing gains motion presets: Sharp (0.70 at 2 px, the M9.2d default), Balanced (0.85 at 6 px) and Stable (0.92 at 16 px).</li><li>An "Advanced TAA tuning" section covers the motion and still history weights, the motion reach, the still clip width and the reconstruction sharpness, with tooltips.</li><li>It applies live through `IRenderBackend::setTemporalAntiAliasingTuning`, with no graph change.</li><li>The default stays Sharp until the owner chooses by eye.</li></ul></li><li>**Diagnostics** for the owner's check: the `view.cut`/`view.cut_reason` counters are visible in the editor Profiler.</li><li>Debug and Release 117/117; shipping 110/110.</li></ul> | `out/m9/car`, `out/m9/motion/flicker-*.json`, `out/m9/eval-s11-cur.json`; `out/m9/editor-diag` |
| 2026-10-06 | **M9.8c: Project Settings persist** (`project.settings.json` at the repo root).<ul><li>**Contents:** anti-aliasing mode and TAA tuning, exposure mode and auto-exposure settings, bloom, display exposure, paper white and peak, shadows and reflection probes. The display transport stays per machine.</li><li>**Loading:** interactive runs load the file before the command line, so flags still win; `main` parses twice. Benchmark and capture runs never read or write it, so measurement routes stay pinned.</li><li>**Saving:** the editor saves 0.5 s after the last settings edit and at shutdown, atomically (temporary file, then rename).</li><li>**Tolerance:** missing or unknown fields keep their values; a malformed file is reported and ignored. Startup prints `Project settings: <path>` when a file is loaded.</li><li>**Tests:** `ProjectSettingsFileTests` (round trip, tolerance, malformed). End to end, the editor loaded a test file and a benchmark run ignored it. Debug and Release 118/118; shipping 110/110.</li><li>**Owner decision (2026-10-06):** the file is committed as the project's shared settings. An interactive run creates it at shutdown when it is missing, and floats are written at single precision so the file diffs cleanly.</li></ul> | `tests/core/ProjectSettingsFileTests.cpp` |
| 2026-10-06 | **Local asset library (owner request):** licensed third-party test content moved out of the repository to `D:\IridiumAssets`, a second asset root (`local`) configured by a gitignored `iridium.local.json`, with a main-checkout fallback for worktrees (Lane L: `f5f110e`..`8ada1bd`).<ul><li>The engine (manifests, catalog, preparation services, harness cook), the editor (two-root asset browser; imports go to the selected root, else the local library) and the tools resolve paths through both roots.</li><li>Cook keys are root-relative and unchanged: a cold cook of all 7 frozen models from D: matches the recorded keys and hashes.</li><li>**The move:** 510 gitignored files were hash-verified against D: before removal, with a reparse-point scan (0 links). The tracked `scenes/test_scene.json` stays.</li><li>From the main checkout: frozen `lar-main` is identical to `m9-g8` except F4-woit; the R04/R05/O02–O04/X01 sweep routes pass from D:; the editor smoke is clean with no DuplicateGuid. All 117/117 and shipping 110/110.</li><li>**Cause and lesson:** this followed the accidental 2026-10-05 deletion of local assets through a lane worktree's directory links. Recursive deletes are now scanned for reparse points first, and lanes never link assets (AGENTS.md).</li></ul> | Lane L report; `out/m7r/captures/lar-main`, `out/m7r/sweeps/lar-main-sweep` |
| 2026-10-06 | **M9.8d: owner videos of the exposure snap and TAA edge flicker; history survives compatible rebuilds (ADR-0017, accepted by the owner 2026-10-06).**<ul><li>**TAA flicker, root cause (reproduced):** the Porsche 911 paint is an opaque body plus a separate `coat` shell authored as glTF BLEND, alpha 0.458. TAA's M9.3 reactive mask therefore covers the whole body. Its luma rule compares history with the jittered centre sample at 3x, with a 0.2 floor, so every edge drops to the floor. Under the Belfast sunset with a held camera (F2-one route, Porsche artifact) flicker p99.9 is 0.055; with the reactive mask disabled it is 0.002. Weights, the clip, the still gate and motion each change nothing. Gating the reactive path on history-outside-range evidence fixes the Porsche but regresses TF-reactive trails (0.0061 to 0.008-0.018): moving patterns stay inside the 3x3 range. Not adopted. The fix needs motion-aware reactive (open).</li><li>**Exposure snap (recording):** each snap drops to full adaptation in one frame, after a normal smooth start and when the camera comes to rest. That signature means an invalid history or a huge dt. Rebuilds reset all history, which is fixed below. The editor-specific trigger is not reproduced yet: the exposure trace now records `delta_seconds`, `previous_valid` and `previous_ev100` per frame for an owner repro.</li><li>**ADR-0017:** a device-idle rebuild keeps every History pair with the same name, reset policy and slots (images, parity, access, validity, last view). Verified: three output-transport rebuilds keep exposure history valid (previous EV continuous), with 0 validation messages. Contract test added; it fails without the last-view carry.</li><li>**Temporal health counters** in the editor Profiler: `temporal.taa.history_valid`, `exposure.history_valid`, `render_graph.rebuilds`.</li><li>Release 118/118.</li></ul> | `out/m9/editor-diag/pb-*`, `out/m9/motion/eval-r-*.json`, `out/m9/editor-diag/switch` |
| 2026-10-06 | **M9.8e: motion-aware reactive coverage (fixes the owner's TAA edge flicker on car models).**<ul><li>**Rule:** sorted and compatibility layers count reactive coverage only where their own motion disagrees with the opaque velocity under them, by more than a pixel (smoothstep 0.25 to 1 px). A zero velocity texel (background) also accepts the far plane's camera motion, as TAA reconstructs it. Layered tiers and WeightedOIT stay reactive by coverage.</li><li>**Mechanism:**<ul><li>The extractor resolves previous transforms for the sorted and compatibility queues after sorting, from the stable-identity cache. This reverses the G4 "transparent queues carry no previous transform" decision now that there is a consumer.</li><li>Every material pipeline uses the motion vertex shader. The forward feature always pushes the motion block.</li><li>`complex_material_indexed.frag` and the SortedSurface program (`complex_opaque_material_indexed.frag` without velocity) write the reactive coverage as a second blend source. Alpha-blend and premultiplied alpha factors become ONE_MINUS_SRC1_ALPHA; RGB blending is unchanged.</li><li>The device requires `dualSrcBlend`. The scene set binds `gbuffer.velocity` at binding 3, and the sorted and compatibility passes declare a sampled read of it.</li></ul></li><li>**Results:**<ul><li>Porsche held under the Belfast sunset: flicker p99.9 0.055 to 0.002, and pixels above 0.01 from 0.64% to 0.00%.</li><li>TF-reactive identical to the baseline (tone-mapped RMSE 0.0074-0.0078, trail 0.0058-0.0080). TF-glass equal (trail 0.0145 / 0.0146). TF-thin identical.</li><li>Frozen set (TAA off, sync validation) byte-identical to `lar-main` except F4-woit (within its order class), 0 messages.</li><li>Validation clean on TF-reactive and on the Porsche route. Debug and Release 118/118.</li><li>**Cost** (one matched pair at native 4K, T-F1-all, A,B,B,A against `2821920`; machine quietness not controlled):<ul><li>GPU median 1.125 to 1.159 ms (+0.034 ms, +3.0%). The two B runs differ by 0.04 ms.</li><li>Non-wait CPU 0.453 to 0.485 ms (+0.03 ms): the transparent previous-transform lookups.</li><li>Steady-frame allocations 0.</li></ul></li></ul></li><li>**Root-cause note:** SortedSurface materials draw with the non-transmissive opaque-forward program in the transparent pass, so both forward programs carry the reactive output. The first candidate missed this, regressed TF-reactive trails, and was caught by the motion evaluation.</li><li>**Open:** transparent materials animated without moving (UV scroll, flipbooks) are no longer reactive; that needs a material-authored temporal-response override. Layered-tier glass on cars remains reactive by coverage.</li></ul> | `out/m9/editor-diag/pb-m98e2`, `out/m9/motion/eval-r-m98e2.json`, `out/m7r/captures/m98e`, `out/m7r/timing/m98e` |

## Completion report

**M9 is accepted (2026-10-06)** on `m9-temporal` (PR #8): 37 commits from `52fdb6e`
(plan) to the M9.7 defaults flip `449fbcc` and these documents. The next lead starts from
`docs/milestones/M9-to-M7.9-handoff.md`.

### Changed behaviour and architecture

- **Product defaults.** Native TAA (jitter follows it), GPU auto-exposure with the manual
  EV as compensation, and subtle bloom (4% scatter, no threshold, Karis prefilter only
  without TAA), per the owner decisions of 2026-10-05 and 2026-10-06.
  - Measurement tools pin the M7R route (`Get-M7REngineBaseArgs`:
    `--anti-aliasing none --exposure manual --bloom off`). The frozen set and every
    earlier baseline therefore stay comparable.
  - The editor switches anti-aliasing and bloom live in Project Settings.
- **History.** One `ViewHistoryContext` keys graph History per view, with up to two
  physical sets per view, a per-pair `HistoryReset{OnCut, SurviveCut}`, and validity
  defined as "written on this view's previous turn" (G1, G2; ADR-0016 item 5 as-built
  note).
  - The view tracker detects cuts.
  - Opening a scene document cuts (M9.6a).
- **Motion.**
  - GPU-scene settle publication (G3) and previous matrices for direct packets (G4).
  - Matrix jitter that culling, Hi-Z, LOD, shadows, probes and layered atlas rects never
    see (G5).
  - An RG16F velocity target written by every opaque writer (M9.1).
- **TAA** (M9.2, M9.3): `VulkanTemporalAntiAliasingFeature`, one compute pass.
  - Gaussian reconstruction (sharpness 6).
  - YCoCg variance clip in a pre-exposed, compressed space.
  - Closest-depth motion with camera reprojection.
  - Catmull-Rom history.
  - Still-pixel trust (gamma 3, weight 0.97), gated by the 3x3 range.
  - Velocity-disagreement disocclusion: history alpha holds content motion.
  - A reactive mask from scene-colour revealage alpha. This is a new contract: opaque
    writers output alpha 1.
- **Auto-exposure** (M9.5): a deterministic per-tile histogram, percentile metering,
  EV100 limits, and 3/1 EV/s adaptation on the per-view time delta. The state is a
  16 B `SurviveCut` History buffer read by the output (current) and by TAA (previous).
- **Bloom** (M9.4): a dual filter (13-tap down, tent up, 6 levels, one aliased transient
  chain) and an energy-conserving composite in scene-linear, before the single output
  transform.
- **Contract.** `TemporalUpscaleInputs` is the vendor-neutral SR input for M9b; native
  TAA consumes it.
- **Determinism and qualification.**
  - Deterministic opaque compaction (G8, new feature-set hashes `m9-g8`).
  - The qualification probe-finalize drain (G7).
  - The exposure trace.

### Interfaces and files

- **Core:**
  - `renderer/graph/ViewHistory.h`, RenderGraph and the executor (view sets, reset policy);
  - `renderer/rhi/ViewMotion.*`, `Mesh.h` (view ABI), `TemporalUpscaleInputs.h`,
    `RenderBackendConfig.h` (AA, exposure and bloom settings);
  - `IRenderBackend::setAntiAliasing`, `setBloom` and `resizeSceneRenderExtent` (unchanged).
- **New feature owners:** `VulkanTemporalAntiAliasingFeature`, `VulkanExposureFeature`,
  `VulkanBloomFeature`.
- **New shaders:** `taa_resolve.comp`, `exposure_histogram.comp`, `exposure_adapt.comp`,
  `bloom.comp`, `gpu_scene_compact_bins.comp`, `include/view_uniforms.glsl` and
  `include/motion_vectors.glsl`.
- **Qualification:** `IQualificationBackend::drainReflectionProbeCaptures`,
  `armExposureReadback` and `collectExposureReadbacks`. No core test hooks.
- **Tools:**
  - `tools/m9/*`: admission, temporal captures, metrics, the motion evaluation, eleven
    engine-authored temporal fixtures (TF-reactive added in M9.3);
  - `tools/m7r` route pins.

### Verification

- **Tests:** Debug and Release 116/116; shipping 109/109.
- **Frozen set:** `m9-defaults` is identical to the `m9-g8` hashes except F4-woit (known
  WeightedOIT order, within its envelope), with sync validation clean (0 messages, 0
  hazards). The route pins reproduce the M7R route under the new defaults.
- **Validation:**
  - Qualification routes for resize, transport switch, residency, deep-layered
    lifecycle, depth-pyramid resize and selection, with every M9 feature on: 0 messages
    under validation and sync validation.
  - Layered capture validators under jitter: pass.
  - The M9 fixture set at product defaults under sync validation (`m97-sync`, 11
    fixtures): 0 messages, 0 hazards.
- **Smoke:** the editor smoke (`--hidden-window --frame-limit 120 --validation-sync`, at
  product defaults) and the shipping smoke report 0 messages and 0 hazards, and leave
  `imgui.ini` unchanged.
- **Allocations:** steady-frame allocations are 0 on every admission route and side
  (TAA, Auto exposure and bloom, alone and combined, and on TF-reactive).

### Visual and motion evidence

The method: 64-phase held accumulation references per frame (`out/m9/motion/ref64`, 47
references), tone-mapped RMSE, the share of pixels above 1/64, trail energy over the
pixels whose reference changed, held-scene flicker, and crops and error heatmaps
inspected by eye (decision log).

- **Static** (TF-static / TF-thin, against no AA 0.0256 / 0.0297): RMSE 0.0157 / 0.0183,
  with 5.6% / 7.9% of pixels above 1/64 and flicker 0.67% / 1.14%.
- **Motion** (TAA / no AA):
  - disocclude 0.0097 / 0.0105;
  - teleport 0.0089–0.0105 / 0.0133;
  - thin 0.0212 / 0.0299;
  - emissive 0.0076 / 0.0123;
  - hdr 0.0067 / 0.0080;
  - pan steady 0.018–0.024 / 0.025;
  - specular 0.0030 / 0.0033;
  - reactive 0.0074–0.0078 / 0.0120 (trail 0.017–0.019 → 0.006–0.008).
- **Worse than no AA:** TF-glass 0.0135 / 0.0125 (motion softness on the opaque frame;
  the glass itself does not ghost) and TF-foliage 0.0089 / 0.0071 (rotating alpha-tested
  edges).
- **Rejected candidates**, each with its numbers in the log:
  - temporal neighbourhood extents;
  - a softer motion kernel;
  - a mean-statistics lighting gate;
  - a coverage weight cap;
  - a mean-distance gate.

### Performance and memory against the budget

These are the five-process native-4K results (`out/m9/timing/m9-adm-*`; FRAME_BUDGET "M9
temporal and post-processing admission").

- **TAA:** +0.18–0.21 ms GPU, from a 0.20 ms pass. Its 0.40 ms row is at about 0.25 ms
  with the velocity target.
- **Exposure:** +0.01–0.03 ms.
- **Bloom:** +0.13–0.14 ms.
- **Post row:** about 0.22 ms of 0.50.
- **All three:** +0.35–0.37 ms. The heaviest route goes from 3.60 to 3.95 ms GPU, against
  the 6.94 ms frame budget.
- **Memory:** TAA History is +127.5 MB at 4K. Exposure adds a 16 B pair plus a 255 KB
  transient. Bloom adds 0, because its chain aliases.
- **CPU:** non-wait CPU is unchanged within noise.
- **F6 watch item, closed:** +1.6% against M7R final, attributed pass by pass to the
  velocity targets (+0.052 ms) and G8 (+0.002 ms). No placement policy is needed.
- **Hitch rerun** (`m97-hitch`; A = pinned route, B = all three on, A,B,B,A):
  - per-event frame peaks match A on H-stress, H-probe and H-upload;
  - 0 drain frames and 0 upload waits;
  - medians rise by the GPU cost (GPU-bound);
  - one 175 ms frame outside any event window in H-stress B run 2 is the known
    intermittent-spike watch item.
- **Starvation:**
  - The first run (`m97-starvation`) failed two criteria. Comparison and repetition
    explain both:
    - The T-F7 non-wait median was +34.6%. Its A runs sat in the metric's known low
      state (1.53–1.75 ms, against 2.04–2.16 elsewhere); with-cook B was 2.16–2.25 ms.
      The repeat (`m97-starvation-2`) passes at −1.2% (A 2.16/2.07, B 2.10/2.08 ms),
      matching the M7R final build under today's conditions
      (`m97-starvation-m7rfinal`: −0.5%).
    - The T-F1 "no frame over 2x median from a frame-task wait" criterion fails on the
      M7R final build too (2, 0), and in the repeat it fails on side A without any cook
      (1, 1). It is environmental, not M9, and is logged as a watch item for M7.10.

### Deferred work and risks

These are listed in the hand-off, section 4.

- **Open TAA work:**
  - sub-pixel wires;
  - moving-shadow trails on high-contrast texture;
  - the WeightedOIT soft-edge halo;
  - the jitter-phase pulse under motion;
  - motion softness on TF-pan (pixels above 1/64);
  - a sharpening pass.
- **Not yet covered:** layered-tier reactive has no fixture, and the TF-glass fixture's
  glass carries no tint.
- **Code size:** `VulkanVertexBackend.cpp` is at 2,474 of its 2,500-line cap.
- **Moved out of M9:**
  - M9b: DLSS/FSR/XeSS and dynamic resolution;
  - M13: skinned motion;
  - M10: denoiser consumers.

### ADR and roadmap status

- **ADRs:** no new ADR. ADR-0016 item 5 carries the as-built note on view-keyed,
  reset-policy History (G1/G2). Buffer History needed no change.
- **New invariants** (documented in the plan and the hand-off):
  - scene-colour alpha is revealage;
  - measurement routes pin the M7R route.
- **ROADMAP:** M9 is `Accepted`, M9b is defined (Planned, before M11), and the schedule
  is updated.
- **Other documents:** FRAME_BUDGET gains the M9 admission section, and PROJECT_CONTEXT
  "Current direction" is updated.
