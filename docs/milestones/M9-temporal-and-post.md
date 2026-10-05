# M9 — Temporal Rendering and Core Post-Processing

## Header

| Item | Value |
|---|---|
| Milestone | M9 — native motion vectors, jitter, native TAA (DLAA-class at 1:1), reactive handling, bloom, auto-exposure, generic reprojection/history utilities, vendor-neutral super-resolution input contract |
| Status | **Approved** by the owner 2026-10-05. G1 `In Progress`. |
| Lead | M9 lead session (Claude Code), from `docs/milestones/M9-task-lead-prompt.md` |
| Branch / PR | `m9-temporal` from `Render-Refactor-for-Modularity` at `da8e4e8` (PR #7 merged). One PR into `Render-Refactor-for-Modularity`. |
| ADRs | ADR-0002 (scene-linear HDR, single output transform, auto-exposure deferred to M9), ADR-0006 (velocity not GBuffer-only), ADR-0012, ADR-0013 (transport switch rebuilds the graph), ADR-0014 (identity; current/previous records), ADR-0015 (task rules), ADR-0016 (graph execution; History item 5) |
| Dependencies | M1, M2, M7R (accepted 2026-10-04), the M7.1/M7.2 GPU scene |
| Next lead | M7.9 (hand-off written at M9.7) |
| Last updated | 2026-10-05 |

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
the contract rather than completing it, ADR-0017 supersedes item 5 instead.

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
| G1 | refactor | View supplied before the first pass; one `ViewHistoryContext` type |
| G2 | refactor | Per-view History sets, per-pair reset policy, `ViewMotionTracker` cut detection, ADR-0016 note |
| G5a | refactor | Shared `view_uniforms.glsl`; offset asserts; ABI tests for every declarer |
| G5b | refactor | Jitter, previous view-projection and temporal-info fields; per-view counter; Halton; zero-jitter identity |
| G3 | refactor | Publisher settle and teleport flag; upload-byte report |
| G4 | refactor | Previous transforms for primitive-indexed and matrix-only packets and OIT instances |
| G6a | tooling | Five-process feature-admission runner (lane) |
| G6b | tooling | Engine-authored temporal geometry, composition scene factory, camera paths, M9 fixture set (lane) |
| G6c | tooling | Capture sequences, post-TAA capture domain, jitter metadata, accumulation reference |
| G6d | tooling | Temporal metrics tool (lane) |
| G7 | refactor | Probe-promotion race: qualification finalize drain or warm-up past publication |
| G8 | feature | Deterministic opaque compaction; new feature-set hashes |
| M9.1 | refactor (images) | Velocity target, writers, debug view, oracle; GPU cost and bandwidth |
| M9.2 | feature | Native TAA feature owner, SR contract, overlays; quality candidates |
| M9.3 | feature | Reactive and transparency handling |
| M9.5 | feature | Auto-exposure |
| M9.4 | feature | Bloom |
| M9.6 | feature | History invalidation and robustness matrix |
| M9.7 | feature | Admission, F6 re-measure, completion report, M9-to-M7.9 hand-off, M9b definition |

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

## Completion report

To be written at M9.7.
