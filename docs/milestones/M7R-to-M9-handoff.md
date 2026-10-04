# M7R to M9 Lead Hand-off

## Status and purpose

M7R R6, 2026-10-04. Researched against `m7r-consolidation` at `d2ffdb1`, then updated
for R5 acceptance (`e98261b`: R5c.7 parallel extraction and R5c.8 zero steady
allocations). For the M9 lead: where motion vectors,
jitter, history, TAA, bloom and exposure attach in current code, what the frame model
guarantees, and which watch items are open. Line numbers are for `d2ffdb1`. R5c.7–R5c.8 later edited
`src/extraction`, `src/renderer/rhi/CompactDrawSort.h` and the transform system, so
line numbers in those files may have shifted. Anything not checked against source is
marked *unverified*.

Read first: `AGENTS.md`, `ROADMAP.md` (M9), `docs/performance/FRAME_BUDGET.md`
(the 0.40 ms TAA row and the 0.50 ms post/exposure/output row), ADR-0002, ADR-0006,
ADR-0012, ADR-0013, ADR-0015, ADR-0016, and the M7R plan's decision log.

## Summary: gaps M9 must close first

1. **Graph History is not wired to the view.** `VulkanVertexBackend::beginFrame`
   calls the one-argument `beginFrameExecution(frame)` (`VulkanVertexBackend.cpp:1723`).
   That reuses the executor's last view, `{0, 0}` (`VulkanRenderGraphExecutor.h:415-417`).
   `RenderFrame::history` only arrives later, in `submitFrame` (`:1836`), and only
   reaches the Hi-Z owner.
2. **The "previous" transform means "before the last change", not "last frame".**
   `GpuScenePublisher` copies current into previous only when the transform changes
   (`GpuScenePublisher.cpp:666-677`). An object that stops keeps a stale previous
   transform indefinitely.
3. **Direct-packet draws carry no previous matrix.** This covers forward-opaque,
   selection, direct fallback and transparent draws. They use `push.renderMatrix`
   (`canonical_material.vert:38`) and the frame-local `instanceTransforms`
   (`RenderFrame.h:180-184`).
4. **History keying is per graph, not per view.** Every pair is re-keyed with one
   view (`RenderGraph.cpp:931-940`). The editor's dual retained views
   (`EditorHost.cpp:208-223`) and asset previews (`RenderExtractor.cpp:939-943`)
   would therefore invalidate each other every frame.
5. **`FrameCritical` work now runs every frame.** Since R5c.7, classification,
   extraction, merge, transparent sorts and intervals are frame-critical task sets
   that the main thread joins (`RenderExtractor`, `ParallelDrawSort`). M9's
   per-frame CPU work (jitter, history bookkeeping, histogram readback) must follow
   ADR-0015's rules for submitting code: per-index outputs, no order-deciding
   atomics, and no steady allocations (`--qualification-allocation-trace` finds any
   regressions).

## 1. Motion vectors

**G-buffer.** The `gbuffer` pass is declared at `VulkanProductionRenderGraph.cpp:476-478`,
through `addCompaction` with `gpu-scene.opaque.compact`. Its five colour targets are
created at `:217-226` and written with clears at `:481-490`:
`gbuffer.normal`, `.albedo`, `.emissive`, `.f0-roughness` and `.material-flags`.
Depth `depth.opaque` is written at `:491-492`.

**Formats.** Per layout (R/Q/C), formats come from `vulkanGBufferFormats`
(`VulkanGBufferLayout.h:22-39`). The attachment order is the usage-declaration order
(`:45-50`, a `std::array<VkFormat, 5>`).

**Adding a velocity target.** Create it beside `:226` and write it in `gbuffer` after
`material-flags`. Appending keeps attachment indices 0–4 stable for the R4a rendering
plan. With a clear, it is alias-eligible. `VulkanPipelineMaxColorTargets = 5`
(`VulkanPipelineLibrary.h:17`), the 3-or-5 colour check
(`VulkanPipelineLibrary.cpp:134-135`) and the target passed at
`VulkanVertexBackend.cpp:411-419` must change together.

**Forward-opaque.** It writes only `depth.opaque` and `scene.color` (`:612-618`).
Complex opaque materials need a velocity write there too, so the forward pipeline
target gains a second colour format (`VulkanVertexBackend.cpp:415-416`). Today
transparent pipelines share that layout (`:417-418`); keep transparent targets at one
colour. Transparent passes write no velocity; they feed a reactive mask (ROADMAP M9).

**Previous transforms (GPU scene).** The packed table holds two affine transforms per
instance, at dense index `2d` (current) and `2d + 1` (previous), written at
`GpuScenePublisher.cpp:1068-1069` and `:1135-1139`.
`GpuSceneInstanceRecord::references = {2d, 2d+1, firstPrimitive, count}`
(`GpuScenePublisher.cpp:940-947`; struct at `GpuScene.h:105-111`). `transformRevisions`
is the parallel array (`GpuScene.h:289`; pushed at `GpuScenePublisher.cpp:1070-1073`).

No shader reads `references.y` yet. The G-buffer vertex shader reads only
`references.x` (`gpu_scene_material.vert:54-57`).

The shadow/probe content watermark reads only the current-transform revision
(`GpuScene.cpp:98-101`), so changing how previous is maintained leaves those cache
triggers alone (*unverified*: upload bytes will rise).

**Settle problem (gap 2).** M9 must add a "settle" publication: the frame after a
transform stops, set previous = current. Extraction is change-driven (R5c.5,
`extraction/GpuSceneObservation`), so a stopped entity is not re-observed. A
"moved last frame" list is needed, and the extraction verifier
(`--qualification-extraction-verifier`) must keep passing.

**`GpuSceneInstanceHistoryReset`** (`GpuScene.h:48`) is set on new instances
(`GpuScenePublisher.cpp:643`) and cleared on their first move (`:673`). Use it for
no-history motion. Teleports are not flagged; *unverified* whether any authoring path
should set it.

**Not available:** skinned motion (no skinning runtime until M13) and a visibility
buffer (ADR-0006). Velocity must stay derivable from GPU-scene current/previous records, so a later
visibility/material resolve and M8 mesh-shader emitters can produce the same vectors.

## 2. Jitter

**Flow today.** `RenderExtractor` builds the projection (`RenderExtractor.cpp:899-904`);
`finalizeView` (`:918-948`) packs it into `RenderFrame::view`, a `ViewTransportRecord`
(`Mesh.h:34-46`, 320 B `static_assert`), and fills `RenderFrame::history` (rhi
`ViewHistoryContext`, `Mesh.h:20-24`). `submitFrame` (`VulkanVertexBackend.cpp:1822-1869`)
calls `updateCamera` (`:2088-2093`), which feeds `VulkanOpaqueFeature::updateView`
(Hi-Z `projectionRevision_` and LOD history, `VulkanOpaqueFeature.cpp:167-172`),
`gpuScene_.views()` (GPU culling) and `VulkanViewUniforms::update`
(`VulkanViewUniforms.cpp:40-57`; the set-0 UBO, `Mesh.h:137-151`, 384 B). Clusters and
the layered view-projection also use `frame.view.projection` (`:1859-1861`, `:2101-2119`).

**Recommendation.** Do not bake jitter into `ViewTransportRecord::projection`:
- `projectionRevision_` hashes it, so every frame would reject Hi-Z history as
  `ProjectionChanged` (`DepthPyramid.cpp:105-106`);
- the CPU frustum classification kept by R5c.4e would also see it.

Instead, carry the jitter and the previous unjittered view-projection as new
`ViewTransportRecord`/UBO fields, and apply the jitter only in raster vertex stages.
The ABI `static_assert`s and the `ShaderAbiContract` tests move together.

**Sequence index.** Derive it from a per-view frame counter, never wall time, so
frozen captures stay reproducible. `finalizeView`'s inputs
(`FrameOrchestrator.cpp:377-384`) do not include `applicationFrameIndex`; `extract`
gets it (`:391-397`).

## 3. History resources

**API.** `RenderGraphBuilder::createHistory(name, desc)` returns
`{previous, current, pair}` (`RenderGraph.h:414-423`; `RenderGraph.cpp:211-219`). The
descriptor must not be imported and must start `Undefined`; `previous` is read-only
(`RenderGraph.cpp:292-294`); each pair gets two slots outside the per-frame pool
(`RenderGraph.h:307-315`, `:367-370`).

**Executor.** History slots are global, not per frame slot
(`VulkanRenderGraphExecutor.h:585-601`). Parity flips when the writer begins
(`VulkanRenderGraphExecutor.cpp:1466-1469`); an invalid `previous` transitions from
`UNDEFINED` (`:1406-1410`); consumers query `historyValid(id)` (`:1089-1095`).
`image(frame, id)` resolves this frame's parity slot (`:1999-2000`), so descriptors
need one set per parity.

**Validity key.** `HistoryValidityKey {identity, resetRevision, extent, format,
topologyHash}` (`RenderGraph.h:541-550`). A pair is valid only if its writer ran last
frame under an identical key (`RenderGraph.cpp:931-954`). This matches ADR-0016
item 5.

**`resetRevision` today.** The editor scene view always sends 0. Only the benchmark
camera cut bumps it (`BenchmarkScene.cpp:585-587`); asset previews use
`previewFramingRevision`. M9 owns cut and teleport detection.

**Wiring needed (gaps 1 and 4).**
- Convert rhi `ViewHistoryContext` (identity default 1, `Mesh.h:21`) to
  `RenderGraph::ViewHistoryContext` (identity default 0, `RenderGraph.h:531-537`).
- Supply it before the first graph pass. That means a new executor call in
  `submitFrame`, or passing the view into `beginFrame`.
- Give History a per-view dimension, as the Hi-Z history already has per retained
  view (`VulkanOpaqueFeature.cpp:150-165`), or accept invalid history in dual-view
  editing.
- Auto-exposure normally survives camera cuts, but one `resetRevision` invalidates
  every pair. A per-pair reset policy is missing.

**Contract tests that assert no production history.** Update them deliberately:
- `testProductionDeclaresNoHistory` (`VulkanRenderGraphExecutionTests.cpp:1457-1464`,
  registered at `:2045`);
- the R3b.6 golden subsequence and slot test (`ProductionGraphContractTests.cpp:384-419`,
  `fixtures/ProductionGraphR3b6Golden.inc`);
- `testSingleOutputTransform` (`:236-274`);
- the pass-index pin `passes()[22] == "bloom-hook"` (`VulkanRenderGraphExecutorTests.cpp:188`).

Keep the History unit and device tests (`RenderGraphTests.cpp:142-240`,
`VulkanRenderGraphDeviceTests.cpp:186-230`).

**The Hi-Z history is not graph History.** `depth.occlusion-pyramid.history` is an
ExecutorOwned global import with CPU-side validity (`VulkanProductionRenderGraph.cpp:457-474`).

## 4. TAA placement

**Last writer of `scene.color`:**
- `transparent.oit.resolve` (`VulkanProductionRenderGraph.cpp:899-904`) when
  WeightedOIT is resident;
- otherwise `transparent.compatibility.forward` (`:869-881`), which is also the last
  `depth.opaque` writer.

Next come `scene-color-capture-hook` (`:910-914`, when declared) and `bloom-hook`
(`:916-917`). TAA goes after the last transparency writer and before `bloom-hook`.

**Decision for M9.** Should the scene-linear capture stay pre-TAA (single-frame and
deterministic) or move post-TAA? Frozen-set comparability depends on it. A TAA-off
route keeps the M7R frozen set usable.

**Backend drain point.** It lies between `submitForwardQueues` (ends with the
scene-colour capture, `VulkanVertexBackend.cpp:2253-2254`) and `submitOutputPass`,
i.e. between `:1865` and `:1866`. Add a feature owner implementing `IVulkanFeature`
(`VulkanFeatureContext.h:87-95`) that drains through its pass with
`drainRegisteredThrough` (`VulkanRenderGraphExecutor.h:467`). This follows the pattern
in `VulkanFeatureContext.h:16-24`.

**Overlay readers after TAA.** `output-transform` reads `gbuffer.emissive` (the
selection mask) and `depth.opaque` (grid occlusion) (`:922-923`; descriptors at
`VulkanOutputPass.cpp:192-221`). With jitter, both overlays shimmer unless they read
unjittered inputs.

`final-capture-hook` also reads `scene.color` (`:929`; pinned by
`ProductionGraphContractTests.cpp:227-228`): if TAA writes a new resource, decide what
the hooks and output read.

## 5. Bloom

**Today.** `bloom-hook` reads `scene.color` as `SampledRead` (`:916-917`) and is
registered as always inactive: `{this, &never, &executeNothing}`
(`VulkanOutputFeature.cpp:41`). `output-transform` reads `scene.color`, `emissive` and
`depth` (`:919-925`), and its callback draws one pass (`VulkanOutputFeature.cpp:116-138`).

**For M9.** Replace the hook with the bloom chain as transient graph images:
- declare whole-resource writes so the images alias (`RenderGraph.h:440-445`);
- add `graph.read(outputTransform, <bloom>)`;
- add a descriptor binding to the output set, which has 4 bindings today
  (`VulkanOutputPass.cpp:59`; `output.frag:7-10`).

Bloom stays scene-linear AP1 before the single output transform (ADR-0002).

## 6. Exposure

**Today.** Manual EV is a push constant (`VulkanOutputPass.cpp:17-29`, `:285`),
applied as `exp2(manualExposureEv)` at `output.frag:311-312`. It is sourced from
`RenderFrameOutputSettings::manualExposureEv` (`RenderFrame.h:123-127`). Extraction
selects the preview or scene EV (`RenderExtractor.cpp:934`).

**For M9:**
- A luminance histogram pass between the TAA output and `output-transform`, in the
  same drain as `submitOutputPass` (`VulkanVertexBackend.cpp:2273-2287`), or a new
  owner ahead of it.
- Adapted luminance as a History pair. Buffers are supported:
  `HistoryValidityTracker` keys buffers by size (`RenderGraph.cpp:891-893`), and the
  executor issues buffer barriers (`VulkanRenderGraphExecutor.cpp:1413-1414`).
- `output-transform` reads exposure through a descriptor. Manual EV remains the
  compensation term the editor already drives.
- The reset-policy gap in section 3 applies.

## 7. The frame model M9 inherits

| Area | Contract | Where |
|---|---|---|
| Frames in flight | 2 slots. Timeline semaphore per frame serial (fence fallback); per-image serials, no image-owner serialization. | `VulkanFrameScheduler.h:82`, `:74-80`; ADR-0016 item 7 note |
| Deletion queue | `retire*` keys on `retireValue()` (raised by the upload retire floor); collected in `beginFrame`. Use it for any capacity growth or resize of M9 resources; never `waitForAllFrames`. | `VulkanFrameScheduler.h:116-158`, `.cpp:277`, `:542` |
| Transient aliasing | Planner from compiled lifetimes; on by default (`--render-graph-aliasing`, `RendererOptions.cpp:314`). History, imports and `excludeFromAliasing` are never aliased. First use must clear or be declared whole-resource; reading an aliased image before its writer throws. | `RenderGraph.cpp:130-132`; `RenderGraphAliasing.h:94`; ADR-0016 item 9 |
| Dynamic rendering | Register with `VulkanPassCallbacks::dynamicRendering`; attachments, load/store and clears come from declared usages. No `VkRenderPass` exists. | `VulkanRenderGraphExecutor.h:358-371`, `.cpp:239` |
| Synchronization2 | One `vkCmdPipelineBarrier2` per pass, from declared usages; in-pass barriers only for ADR-0016 item 6 cases. Synchronization validation does not catch missing same-access or alias-predecessor scopes; the fake-sink tests do. | `VulkanRenderGraphExecutor.cpp:111`; decision log R4a.0 and R4b.5 |
| Pipeline cache | Every pipeline passes `VulkanFeatureContext::pipelineCache`; `--pipeline-cache DIR\|off`; evidence scripts default to `off`. | `VulkanFeatureContext.h:56`; `VulkanPipelineCache.h:7-41`; `RendererOptions.cpp:296` |
| Uploads | 64 MiB staging ring; transfer queue (family 1 on the 4090) with ownership acquires at frame start; `--upload-queue auto\|graphics\|legacy-blocking`. | `VulkanUploadContext.h:47-48`, `:89`, `:102`; `VulkanVertexBackend.cpp:1700-1709`; `VkContext.cpp:390` |
| TaskSystem | Priorities `FrameCritical`/`Normal`/`Background`; the main thread joins only `FrameCritical`. M9 CPU work (jitter, history bookkeeping, histogram readback, if any) is frame-critical or inline, never `Background`. | `TaskSystem.h:42-46`, `:212`; `Application.cpp:46`; `FrameOrchestrator.cpp:159`, `:202`; ADR-0015 |
| Allocation | 0 steady-frame C++ allocations on T-F1 and T-F7. Owners stage into fixed members. | `VulkanFeatureContext.h:9-14` |

## 8. Evidence tooling to reuse

| Tool | Use |
|---|---|
| `tools/m7r/M7RFixtures.ps1` | Frozen set F1–F7 + F6-probecap (`:24-39`), tolerance envelopes `depth-tie` and `woit-order` (`:46-49`), timing routes T-F1/T-F7 (`:52-55`) |
| `Run-FrozenCaptures.ps1`, `Compare-FrozenCaptures.ps1`, `Diff-Images.py` | Scene PFM and final-SDR TGA hashes against `captures/r0` (local-only hashes); `-SyncValidation` counts hazards |
| `Run-IndirectDigest.ps1` (`--qualification-indirect-stream-digest`) | Command-stream identity against `r3a0`/`r3a0-ext` |
| `Run-QualificationSweep.ps1`, `Compare-QualificationSweep.ps1` | 36 entries (validators, oracles, resize, transport); counter and profile diff |
| `Run-TimingPair.ps1`, `Summarize-Profiles.py` | A,B,B,A, 500 + 10,000 frames, native 4K |
| `Run-HitchScenario.ps1`, `Analyze-Hitches.py` | H-stress, H-probe, H-upload (`assets/benchmarks/m7r/hitch-*.v1.json`); frames of 250 ms or more record `scripted_slow_frame` scope detail |
| `Run-StarvationTest.ps1`, `Analyze-Starvation.py` | Cook-while-render (`--qualification-background-cook`, `--qualification-frame-task-probe`) |
| Oracles and verifiers | `--qualification-caster-revision-oracle`, `--qualification-extraction-verifier`, `--qualification-alias-poison`, `--validation-sync` (`QualificationOptions.cpp:212-237`) |

**Adding a feature-tier fixture** (FRAME_BUDGET "Evidence tiers"):
1. Add a fixture entry to a tracked benchmark manifest under `assets/` with an `id`,
   `revision`, camera and `scene_factory`. Motion and cuts use `camera_motion`; see
   `assets/m7-occlusion-temporal-manifest.v1.json`. The manifest SHAs are pinned in
   `tests/benchmarks/BenchmarkManifestTests.cpp`, so update the pin in the same commit.
2. Third-party content stays local-only (`content_files` SHA only).
3. Add the fixture to a new feature set beside `$M7RFrozenSet`. Do not edit the frozen
   R0 entries. Give it a measured envelope in `$M7RTolerances` when the output is
   history- or order-dependent.
4. Run five fresh native-4K Release processes per route. *Unverified:* no M7R script
   runs five processes; `Run-TimingPair.ps1` runs 4 (A,B,B,A), so extend it or loop it.

M9 needs the ROADMAP/FRAME_BUDGET temporal scene: disocclusion, thin coverage,
emissive and transparent motion, specular aliasing and cuts. Ghosting and stability
metrics are new tooling (*unverified*: none exists).

## 9. Open watch items and risks

**R3/R4 GPU watch item.** The refraction-pyramid pass runs at 0.130 ms in a quiet
machine state and 0.205 ms in a slower one, with the same graph and alias plan; the
cause is environmental. M9 adds alias-eligible images, which changes heap placement.
- Run every M9 pair on a verified-quiet machine and record the machine state.
- Bisect by pass GPU ranges before attributing a delta to TAA or bloom.

**Probe-promotion race.** Since R4c.3, runtime probe captures publish when the frame
serial completes, so probe counters on V02, O03 and R00–R03 vary run to run; images
are identical (the routes capture at frame 4). With TAA, publication timing enters
history. Probe routes then need warm-up past publication, or a qualification-only
finalize drain.

**Intermittent scripted-event spike (unattributed).** Spikes of 668, 894 and 1,155 ms
hit 3 of 14 post-R4c H-stress runs, plus a 502.6 ms baseline frame with about 338 ms
outside every scope. Not reproduced in R5 wave 2 (H-stress max 46.6 ms). If it recurs
in M9 hitch runs, use the `scripted_slow_frame` detail or an ETW sample.

**R5c.4e.** The main-opaque CPU frustum classification stays, because it feeds
production counters (about 0.23 ms on T-F7). It must use the unjittered projection
(section 2). Revisit when GPU-driven visibility replaces it.

**Deterministic opaque tie-break (deferred to M9).** F3/F7-lod differ by one
depth-tie pixel run to run, because draw order follows GPU compaction order (R0
determinism table; R5 design decision 4). TAA history accumulates such ties into
visible shimmer. Fixing it changes images, so it needs the feature tier.

**VSM.** Default-off (`--experimental-virtual-shadow-resources`, `RendererOptions.cpp:237`).
It resumes after M8, and must keep compiling and passing tests. `shadow.virtual.depth-mark`
reads `depth.opaque` after `forward-opaque` (`:620-630`), so jittered depth reaches its
page marking. The F5 VSM depth-oracle mismatch is pre-existing (decision log).

**Files over 2,500 lines** (`wc -l`, deferred exceptions): `GltfModelImporter.cpp`
3,550 and `AssetBrowserPanel.cpp` 3,273. Near the limit: `VulkanVertexBackend.cpp`
2,334 (2,073 at R3) and `VulkanRenderGraphExecutor.cpp` 2,038. Put M9 work in new
feature owners, not in the backend.

**Other risks.** Transport switches and resizes rebuild the graph and invalidate all
history (ADR-0013), so expect one unconverged frame. `transparent.compatibility.forward`
writes depth (`:878-879`), so depth-based reprojection sees fallback glass depth.

## 10. What not to do (invariants M9 keeps)

- **Colour:** one output transform, scene-linear AP1 until it; bloom, TAA and exposure
  work on scene-linear data (ADR-0002); `output.display` keeps one writer.
- **Shading:** one clustered-light representation (ADR-0006/0007); BSDFs shared by
  deferred, forward and future RT; velocity is not a GBuffer-only contract (ADR-0006).
- **Graph:** M9 GPU work is declared passes and resources; no hand-written barriers
  outside ADR-0016 item 6; history is never aliased; no transient is read before its
  writer.
- **Frame path:** no `std::function` callbacks, render passes, `waitForAllFrames` or
  blocking upload flushes; zero steady-frame allocations on the timing routes.
- **Visibility:** do not jitter the culling, clustering, Hi-Z, shadow or probe
  projections; main, shadow, probe and selection visibility stay independent; keep
  ADR-0014 identity, ADR-0013 live transport switching, and zero-extent/resize safety.
- **Layering:** no test hooks in core runtime interfaces; qualification stays in
  `iridium_qualification`; the shipping preset stays clean. LOD, Hi-Z and VSM stay
  workload-selectable and default-off unless their own gates pass.
- **Evidence and content:** change images only under the feature tier, and keep a
  TAA-off route so refactor-tier evidence stays possible. Never commit third-party
  content or `imgui.ini`.

## Inconsistencies found while writing this hand-off

- **ADR-0016 item 5 vs production.** The ADR describes validity keyed by the view.
  Production never passes the view to the executor (gap 1), and the rhi and graph
  `ViewHistoryContext` are two types with different defaults.
- **ADR-0016 vs dual views.** It does not mention per-view history; the tracker is
  per graph (gap 4).
- **ROADMAP M9 dependency text.** It says previous transforms are "already provided".
  They are, but with change-based semantics (gap 2), and only for GPU-scene draws
  (gap 3).
- **Stale status lines** (plan header, ROADMAP, ADR-0015): fixed in R6.
- **ADR-0016 item 5:** an "as implemented" note now records gaps 1 and 4.
- **R5 scope:** R5c.7 and R5c.8 were completed and R5 was accepted on 2026-10-04.
  The R5c.4e classification restriction is deferred by decision.
- **Importer size.** The M7R plan's 2026-10-02 audit lists `GltfModelImporter.cpp` at
  3,537 lines; it is now 3,550, after the R5b.2 fork-join migration. It and
  `AssetBrowserPanel.cpp` (3,273) remain the two deferred exceptions to the
  2,500-line guideline.
