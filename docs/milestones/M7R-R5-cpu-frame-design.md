# M7R R5 design: CPU frame and application decomposition

This is the implementation design for slice R5 of
`docs/milestones/M7R-architecture-consolidation.md`. It was drafted by a read-only
design pass against `6298757` (branch `m7r-consolidation`) on 2026-10-03. Line
numbers refer to that commit; re-verify them before editing. Every number below comes
from existing profiles: `out/m7r/timing/r4-accept` (the B runs are `c10c182`'s main
build), `out/m7r/hitch/r4-accept-hitch` and `out/m7r/sweeps/r4b-main-sweep`. No new
runs were made. Decisions and evidence go in the plan's log.

## Findings that shape R5

1. **The 1.5 ms F7 gap is per-frame byte hashing of every opaque caster.**
   `getShadowCasterRevision` (`Application.cpp:2717`) and `prepareDepthHistory`
   (`VulkanOpaqueFeature.cpp:155-172`) each run FNV-1a one byte at a time over
   7,232 casters (`VulkanShadowCasters.cpp:13-50`, about 100 B each). That is
   about 1.45 MB of serial multiply-xor per frame. The spot/point/probe revision
   runs even though F7 has no local lights and no probes. On H-stress (115,712
   primitives) the same gap is 20.0 ms. The publisher already keeps per-record
   revisions that make this hashing unnecessary.
2. **The H-stress event spikes are `GpuScenePublisher::synchronize`.** Its maximum
   is 128.0 ms in run B2 and 123.4 ms in run B3 at `add_instances`. Any observation
   change (including one moving instance) takes the slow path
   (`GpuScenePublisher.cpp:130-448`). That path does `std::map` lookups and
   inserts per primitive, deep-copies every observation (`:445`), repacks every
   table and `memcmp`s every packed record (`:625-649`). Extraction and sorting
   are the steady cost (≈14 ms per frame), not the spike.
3. **On both timing routes, the CPU waits for the frame it just submitted, so CPU
   and GPU work never overlap.** The 1.80 ms F7 wait is the *image-owner* fence
   after acquire (`VulkanFrameScheduler.cpp:276-281`), not the slot fence (2 µs at
   `:239`). Under mailbox present the acquired image is the one frame N-1 rendered.
   The frame time therefore equals non-wait CPU plus most of the GPU frame:
   5.38 ≈ 3.49 + 1.80 + 0.09 ms. This is outside R5's serial-CPU target, but
   it caps what R5 can buy in frame time (owner decision 3).
4. **Steady frames are allocation-free only on the two timing routes.** Sweep
   routes with lights, probes, wireframe or Ordinary2 allocate every frame. Two
   further caveats:
   - The counter in `CpuAllocationProfile.cpp:15-21` counts every thread. Once a
     task system runs background work during frames, the per-frame number stops
     meaning "main-thread allocations" unless it is made thread-scoped.
   - Ordinary2 reached zero at M6.8 (`FRAME_BUDGET.md`, "M6.8 steady-allocation
     checkpoint") and now allocates 2 calls / 160 B per frame. That is a
     regression.
5. **The editor's runtime-config writes and the parity path are narrow.** The
   editor writes runtime config in exactly three places in one block
   (`Application.cpp:1759-1800`). The M7.2 parity path has exactly four consumers:
   G-buffer binning, the depth-history hash, wireframe and selection. Both make
   the R5a split and the R5c retirement tractable as byte-identical steps.

## 1. Inventory

### 1.1 `Application.cpp` (4,563 lines) by target unit

| Unit | Lines | Content |
|---|---|---|
| **FrameOrchestrator** | 721-921 | main loop, profiler/allocation frame, FPS title, observer phases, asset tick counters (803-877), transforms (881-885) |
| | 1516-1528, 1666-1686, 2966-3036 | dual-view cadence; `beginFrame`/recreate; `publishGpuScene`; `submitFrame`/`endFrame`; observer `onFrameEnd`; transport switch; viewport resize |
| | 3078-3172, 4413-4520 | GLFW callbacks and `processInput` (read panel focus); swapchain recreate, output LUT, transport switch/status, `IAppControl` |
| **RenderExtractor** | 1108-1514 | GPU-scene observation (mesh-pool walk, override signature, per-observation `memcmp` 1337-1342), synchronize, prepare, counters; `onRenderFrameStage` counter/cache bookkeeping (1399-1514) |
| | 1546-1630 | light extract/prepare, probe extract/publish, probe capacity |
| | 1688-1744, 1841-2961 | queue clears, camera, view record, frustum classify (1892-1899), `appendModel` (1903-2138), mesh-pool walk (2149-2181), M7.2 parity stage (2187-2274), counters (2277-2440), sorts (2445-2523), interval sweep (2536-2544), directional/spot/point scheduling (2548-2894), probe capture schedule (2897-2937), `RenderFrame` assembly (2945-2961) |
| **EditorHost** | 1549-1562 | preview-lighting world (preview sun) |
| | 1631-1670 | material previews; asset-viewer environment selection and diagnostics (writes `viewer.environmentDiagnostic`); retained views; writes `editor.retainedScene/AssetTexture` |
| | 1749-1839 | `beginUI`, `editor.update`, settings consumption, fullscreen benchmark image, colour overlay |
| | 3843-3930 | `resolveEditorAssetPreview` |
| **AssetIntegration** | 923-1106 | `persistBakedReflectionProbe` (serial irradiance on the main thread, 987) |
| | 1529-1545 | capture completions → scheduler, probe-component diagnostic |
| | 3174-3841 | `ProcessMeshSwaps`: preparation results, environments, thumbnails and their upload budget, probe/sky/mesh pool walks (3484, 3542, 3658) |
| | 3932-4411, 4522-4562 | cooked model/environment hot reload (second DDC at 4038); startup model/environment load and publication |
| **Composition root (none of the four)** | 212-334, 336-719, 3039-3075 | constructor, window, `initRenderer` (backend, bridge, services, editor init, startup scene, viewer wait loop 690-717), cleanup order |
| | 273-313 | run snapshot and observer notification |

**Editor writes into runtime state.** Every runtime-config write the editor causes
is in `drawFrame`, inside `cpu.editor.build`:

- `consumeOutputSettings` (1760-1768) sets `config_.outputTransport` and
  `pendingOutputTransport_`, plus `manualExposureEv`, `paperWhiteNits` and
  `peakNits`.
- `consumeShadowSettings` (1770-1784) sets `config_.shadowSettings`. The resolution
  and pool fields are preserved, because they are immutable for the active backend
  allocation.
- `consumeReflectionProbeSettings` (1786-1800) sets
  `config_.reflectionProbeSettings`, and also calls
  `reflectionProbeCaptureScheduler_.configure` and
  `renderBackend->configureReflectionProbeCaptures`.

The renderer also reads editor state directly:

- **Debug and render mode:** `getDebugView` (1851), `currentRenderMode` /
  `debugRenderMode` for wireframe (2947), and `layeredInterfaceOverride` (2052).
- **Preview document and part:** selection, isolation and hover (1972-1977,
  2126-2135).
- **Preview camera and lighting:** 1733-1744 and 1852-1855.
- **Selection:** 1634 and 1715.
- **Extents and grid:** `requestedRenderExtent` (1842, 2989) and
  `viewportGridOverlay` (1870).

Application writes back into editor state at 1528 (`renderingAssetView`), 1639-1660
and 1669-1670.

### 1.2 Threads and services

There are 10 classes with a persistent `std::jthread`, up to 11 live instances. In
deterministic (benchmark) runs only the reimport scheduler is started (plus the
lazy ones). Only rows 1-5 and 7 are deque + mutex + condition-variable services.

| # | Thread | Created | Owner | Wake | Work | Results drained by (scope) |
|---|---|---|---|---|---|---|
| 1 | `AssetCatalogService` | `.cpp:73` | `Application` (469), editor only | CV `:565` | import/refresh/move/rename/delete, SQLite `rebuild` (`:535`) | `takeResults` in `AssetBrowserPanel.cpp:2627` (`cpu.editor.build`), only while the browser is open |
| 2 | `AssetModelPreparationService` | `.cpp:105` | `Application` (483) | CV + stop_token `:392` | glTF prepare, DDC submit, **polls `wait_for(250ms)` `:300`** | `ProcessMeshSwaps` 3182 (`cpu.scene.asset_swaps`) |
| 3 | `AssetEnvironmentPreparationService` | `.cpp:56` | `Application` (496) | CV `:179` | HDRI prepare, blocks on DDC `.get()` `:138` | 3272 (`asset_swaps`) |
| 4 | `AssetThumbnailService` | `.cpp:108` | `Application` (509) | CV `:888`; `BELOW_NORMAL` `:880` | thumbnails, previews, source-material reimport | 3356, upload budget 3382 (`asset_swaps`) |
| 5 | `LocalDerivedDataCache` (editor DDC) | `.cpp:69` | `shared_ptr` (476), shared by 2-4 | CV `:367` | **all cooking**: `importer->cook`, including the glTF fork-join and the convolution | promises (`:381`) to threads 2/3/4/7 only |
| 6 | `LocalDerivedDataCache` (hot reload) | `.cpp:69` via 4038 | `ModelSourceReimportContext` | CV `:367` | source re-cook | through 7 |
| 7 | `AssetReimportScheduler` | `.cpp:10`, always | `AssetRuntimeService` (441) | CV `:203`, stop_source per item | hot-reload prepare callbacks (4097, 4332) | `AssetRuntimeService::tick` (`cpu.asset_runtime.tick`, 804) |
| 8 | `AssetSourceMonitor` | `.cpp:46` | `AssetRuntimeService` | **`sleep_for(10ms)` poll** `:175-181` | debounce; SHA-256 of changed sources **under its mutex** (`:152-162`) | `drainBatches` in `tick` |
| 9 | `SourceFileWatcher` | `.cpp:30` | thread 8's monitor | timed CV 250 ms `:203` | `stat` of all watched files **under its mutex** | drained by thread 8 |
| 10 | `MaterialPreviewCompileQueue` | lazily, `.h:31` | `AssetManager` | CV `.h:52`, one slot | material preview compile | `processMaterialPreviews` 1633 (**no scope**) |
| 11 | `WicFactoryLifetime` | `TextureImporter.cpp:178`, static | process | parked CV `:197` | holds the COM MTA and WIC factory | none |

**Transient mechanisms:**

- **The importer fork-join** (`GltfModelImporter.cpp:1239-1323`, called from
  `cook` at 2851). It runs one job per unique embedded texture view, on up to
  `min(16, hw/2)` transient `std::jthread`s that pull work from an atomic counter.
  Each job is a DDC sub-entry check, then decode, cook and store. The join is
  implicit, and results are merged serially. It always runs on DDC thread 5 or 6,
  so it runs concurrently with rendering at normal OS priority.
- **The menu-bar scan.** `std::async` at `MenuBarPanel.cpp:62` runs an orphaned
  scene-temporary scan that is polled every frame. The future's destructor blocks
  at shutdown.
- **`std::execution::par`** in `EnvironmentConvolution.cpp:492/528/565/618`. This
  one is not in the plan.

No worker thread emits `CpuScope`s, and none is named.

### 1.3 Per-frame CPU stages in order (F7, 10,000-frame medians, mean of B2/B3)

| # | Scope (site) | µs | Note |
|---|---|---:|---|
| 1 | `cpu.platform.events` (755-759) | 0.5 | |
| 2 | `cpu.scene.asset_swaps` (3175), `cpu.asset_runtime.tick` (804), `cpu.scene.transforms` (882) | 5.3 | |
| 3 | unscoped: cadence, probe-capture finalize (1518-1545) | — | |
| 4 | `cpu.light.extract` / `.prepare`, `cpu.probe.extract` / `.publish` (1548-1616) | 3.0 | |
| 5 | unscoped: probe prepare, material previews (1617-1633) | — | |
| 6 | `cpu.gpu_scene.publish` > `observe` / `synchronize` / `prepare` (1112-1376) | 5.6 | |
| 7 | `cpu.renderer.begin_frame` (`VulkanVertexBackend.cpp:1631`) > slot `frame_fence_wait`, `acquire`, image-owner `frame_fence_wait`, `render_graph.lookup` | 1,881.6 | 1,802 µs wait + 0.7 µs acquire; ≈79 µs CPU |
| 8 | `cpu.gpu_scene.upload` (`:1969`) | 5.5 | |
| 9 | `cpu.editor.build` (1750) | 45.7 | |
| 10 | **unscoped**: view record, environment settings, grid, `classifyGpuSceneFrustum` (1841-1899) | **≈182** | 7,232 transformed AABBs |
| 11 | `cpu.render.extract` (1902) | 466.6 | |
| 12 | `cpu.render.sort.opaque` / `.forward_opaque` (2446, 2454) | 219.8 / 13.0 | 6,464 / 768 packets of 240 B |
| 13 | `cpu.render.sort.transparent` (2518) | 275.2 | 3,904 packets |
| 14 | **unscoped** (2524 → `VulkanOpaqueFeature.cpp:213`) | **≈1,535** | see below |
| 15 | `cpu.render.record.gbuffer` (`VulkanOpaqueFeature.cpp:213`) | 203.1 | includes culler `plan` |
| 16 | `cpu.render.record.lighting` (`VulkanVertexBackend.cpp:1991`) | 28.6 | |
| 17 | `cpu.render.record.forward` (`:2047`) | 307.8 | 4,672 draws |
| 18 | `output_transform` (`VulkanOutputFeature.cpp:97`), `ui` (`:2181`) | 12.3 | |
| 19 | `cpu.renderer.submit` / `present` (`VulkanFrameScheduler.cpp:350/399`) | 44.9 / 93.5 | present counts as a wait |

**Gap 14** contains, in order:

- **Application side:**
  - count_if passes;
  - `sweepAmbiguousTransparentIntervals` (`DrawPacket.h:275-348`, 41,920 ambiguous intervals);
  - directional selection;
  - `getShadowCasterRevision` (2717), which hashes 7,232 GPU-scene casters;
  - local shadow requests and schedules;
  - the probe request loop.
- **Backend side, inside `submitFrame`:**
  - `applyOutputSettings` / `updateCamera`;
  - shadow and probe stages, which only skip passes on F7;
  - `prepareDepthPyramidHistory` (`VulkanVertexBackend.cpp:1767`), which hashes the 6,464 + 768 parity packets.

Static cost model (to be confirmed in R5.0):

| Item | Estimate |
|---|---|
| Three caster hashes | 1.0–1.3 ms |
| Sweep | about 0.1 ms |
| Everything else | below 0.1 ms |

F1 has 21× fewer primitives and 21× fewer transparent packets, and its gap is 75.5
µs, which is consistent with linear cost. The gap cannot separate the two terms,
because both counts scale by the same factor.

### 1.4 Extraction path today

| Mechanism | Where | Cost model |
|---|---|---|
| Transform update | `TransformSystem.cpp:23-66`; dirty list filled (`:37`, `:61`) into `changedTransformEntities_` (`Application.h:110`), **never read** | O(transforms) walk ×2 |
| Observation "revision" | per-entity rebuild plus `memcmp` of world/flags/LOD/mask (1337-1342); override signature via `findCookedMaterialRuntime` per override (1157-1169) | O(mesh entities) per frame |
| Publisher fast path | `observationRevisionsEqual` O(instances) (`GpuScenePublisher.cpp:40-52`, 117-129) | 4.4 µs on F7, 75 µs on H-stress |
| Publisher slow path | any changed observation; `std::map` per instance/primitive/geometry (`.h:139-141`); deep copy `:445`; `packActiveSources` repacks all (`:479-620`); `revise` `bitsEqual` on every record (`:625-649`); membership lists (`GpuScene.cpp:85-150`) | O(all primitives · log n) **and allocation-heavy**; 123-128 ms at 1,024 instances |
| Lights | `LightExtractor::extract` (`LightExtractor.cpp:316`): pool walk, pack, `memcmp` per record (`:27`), candidate sorts (`:373`, `:391`, `:417`); `recordRevisions_` / `activeListRevision_` already exist | 1 µs F7, 140-290 µs H-stress |
| Probes | `extractReflectionProbes` (`ReflectionProbe.cpp:152`, sort `:217`, returns a vector); `ReflectionProbePublisher` revisions (`:236-441`, sorts `:302`, `:353`, `:423`, `:500`) | small today |
| Sorts | `std::sort` of 240 B `DrawPacket`s (2447, 2455, 2519-2522); comparators `DrawPacket.h:202-243`. **The opaque comparator is not a total order** (key, geometry, firstIndex) | O(n log n) moving 240 B |

**Per-frame ECS pool walks.** At least 10 per frame:

- transforms ×2;
- asset swaps: probe, sky and mesh;
- light pools;
- probe pool;
- observation mesh walk;
- extraction mesh walk.

### 1.5 M7.2 parity packets for ordinary opaque work

`Application.cpp:2187-2274` rebuilds one 240 B `DrawPacket` for every published
primitive every frame (7,232 on F7, 115,712 on H-stress). Each packet carries:

- `firstInstanceTransform = primitiveIndex`;
- `DrawPacketGpuScenePrimitive`;
- `DrawPacketCpuVisibilityOracle` from the frustum classification.

MainOpaque packets go to `opaqueQueue`. ForwardOpaque packets go to
`forwardOpaqueQueue`, but only when CPU-visible. Selected, CPU-visible packets go to
`selectionQueue`.

| Consumer | Site | What it actually needs |
|---|---|---|
| G-buffer binning | `VulkanOpaqueIndirectCuller::plan` (`.cpp:273-360`), via `buildGpuSceneIndirectPlan` (`GpuSceneIndirect.cpp:8-66`) | primitive index; pipeline, material and geometry handles; index range. All are already in the primitive and geometry records |
| Expected bin counts | `:445` | the CPU visibility bit; qualification only, but it runs every frame |
| Depth history | `VulkanOpaqueFeature.cpp:161-169` | a content revision of the opaque set |
| Wireframe | `recordGBuffer` direct loop (`:311-346`) | per-draw transform, geometry, index range |
| Selection | selection mask pass; `selectionQueue` | the selected instance's visible primitives |
| Direct fallback | any non-GPU-scene opaque packet makes `buildGpuSceneIndirectPlan` drop the **whole** queue to direct (`InvalidPacket`, `:59-65`) | — |

Retiring the path needs the plan's six items:

1. **`mainOpaqueConsumerPrimitiveIndices`** and its membership revision, next to the
   shadow/probe lists (`GpuScene.h:281-284`, `GpuScene.cpp:85-150`).
2. **An `OpaqueSubmission {gpuScenePrimitiveIndices, directPackets, membershipRevision}`**
   in `RenderFrame.h`, modelled on `ShadowCasterSubmission` (`:41`).
3. **Mixed bins**: GPU-scene primitives drawn indirectly, direct packets drawn
   directly in the same pass.
4. **An epoch-based depth-history revision.**
5. **Selection from `GpuSceneInstanceSelected`.**
6. **An indirect wireframe pipeline variant.**

### 1.6 Steady-frame allocations (sweep `r4b-main-sweep`, second half of measured frames)

| Route(s) | Calls / bytes per frame | Likely sources (static reading) |
|---|---|---|
| T-F1, T-F7, R04/R05 dense, O02/O04, X01 | 0 | — |
| X06, V03, V06, V14 (Ordinary2/WOIT, **no lights or probes**) | 2 / 160 B | not identified statically. Regression since M6.8 |
| X05 (point fixture, local shadows off) | 1 | `selectDirectionalShadowLights` result or `buildLocalShadowRequests` (`LocalShadow.h:31`) |
| X04 wireframe | 8 / 1.7 KB | the above plus the wireframe path |
| R00-R03, V02 (2 probes, 2 point shadows) | 15-17 / 3.7 KB | locals at 1576, 2548-2549, 2707, 2719, 2754, 2813, 2847, 2898; `LocalShadow.cpp:22`; `ReflectionProbe.cpp:152` (candidates), `:489`; `finalizeCaptures` (`VulkanReflectionProbeFeature.cpp:357`) |
| V12 light table / V13 cluster stress | 34 / 705 KB; 19 / 98 KB | light-table growth paths (validator-driven) |
| O01/O03/O05, V15 | 420 / 13 / 122 calls, 4.3 MB | qualification oracles and readbacks (excluded, as at M6.8) |

## 2. CPU attribution

### 2.1 F7 main thread (B runs, 10,000 frames)

| Quantity | B2 | B3 | Used |
|---|---:|---:|---:|
| Frame total median | 5.375 ms | 5.389 ms | 5.382 |
| Fence waits (slot + image owner) | 1.801 | 1.804 | 1.802 (**33.5 %** of the frame) |
| Present / acquire | 0.094 / 0.001 | 0.093 / 0.001 | |
| **Non-wait CPU** (the R5 metric) | 3.468 | 3.522 | **3.495** |
| Unattributed = total − top-level children (512 detail frames) | 1.764 | 1.773 | **1.77** (1.54 after the transparent sort, 0.18 before extract, 0.04 elsewhere) |

The audit figure of 1.69 ms unattributed included a 1.43 ms gap between the
transparent sort and the G-buffer record. At `c10c182` that gap is 1.54 ms and the
total is 1.77 ms; the frame around them shrank in R3/R4.

**Serial versus parallelizable** (non-wait 3.49 ms):

| Class | ms | Content |
|---|---:|---|
| Inherently serial today | ≈0.65 | GLFW events; ImGui build (0.05); recording into one primary command buffer: gbuffer, lighting, forward, output, ui (0.55); submit (0.05). Parallel recording through secondary buffers is out of R5 scope and kept as a reserve lever |
| Removable (change-driven) | ≈1.3–1.6 | caster and depth-history hashing (≈1.0–1.3, estimate); frustum classification used only for oracle counts and forward/selection (0.18); parity packet build (part of extract) |
| Shrinkable (compact sorts) | ≈0.35 | opaque 0.22 → ~0 after parity retirement; transparent 0.28 → ~0.08 |
| Parallelizable | ≈0.25 | transparent `appendModel` and intervals (the rest of extract); classification; light/probe/transform updates (tiny on F7) |
| Fixed small | ≈0.25 | `begin_frame` bookkeeping 0.08, publish/upload 0.01, gaps and counters |

### 2.2 What each step buys against the ≤ 3.0 ms F7 target (estimates; R5.0 makes them measurable)

| Step | Δ non-wait | F7 after |
|---|---:|---:|
| R5a decomposition, R5b task system (no parallel users yet) | 0 ± noise | 3.49 |
| R5c.1 + R5c.2: caster and depth-history revisions without hashing | −1.0 to −1.3 | **2.2–2.5 (target met)** |
| R5c.3 compact sorts | −0.3 to −0.4 | 1.9–2.2 |
| R5c.4 parity retirement (no 7,232-packet build; opaque sort ≈ 0; classification restricted to forward/selection) | −0.3 to −0.45 | 1.5–1.9 |
| R5c.7 parallel transparent extraction and intervals (critical path) | −0.1 to −0.2 | 1.3–1.8 |

Because of finding 3, F7's *frame* time falls only by the non-wait reduction, while
the image-owner wait remains.

### 2.3 H-stress (F3-stress plus scripted events, B runs)

**Steady frame (last 512 frames, 1,024 instances, 115,712 primitives, 1,100 lights).**

| Quantity | ms |
|---|---:|
| Frame total median | 45.95 |
| Non-wait | 39.60 |
| Unattributed | 21.98 |
| — of which between transparent sort and gbuffer (caster hashing ×2 over 115,712, plus shadow scheduling for 1,100 lights) | 20.05 |
| `cpu.render.extract` | 7.24 |
| `sort.opaque` | 4.12 |
| `sort.transparent` | 2.48 |
| `record.gbuffer` | 1.87 |
| `record.forward` | 1.06 |
| `light.extract` | 0.29 |

**Event spikes.** Per-scope maxima over 10,000 frames, runs B2 / B3:

| Scope | Max (ms) |
|---|---:|
| `cpu.gpu_scene.synchronize` | **128.0 / 123.4** |
| `observe` | 6.0 / 5.8 |
| `prepare` (capacity growth) | 5.8 / 4.7 |
| `extract` | 14.2 / 13.6 |
| `sort.opaque` | 5.8 / 5.7 |
| `gbuffer` | 3.5 / 7.1 |

The 198 ms `add_instances` frame at 3500 decomposes as:

| Part | ms |
|---|---:|
| Steady frame | ≈46 |
| `synchronize` | ≈128 |
| `observe` + `prepare` | ≈12 |
| Larger extract | ≈7 |
| **Total** | **≈193** |

`remove_instances` (85 ms) and `add_instances` at 500 (95-105 ms) are the same slow
path at smaller sizes. `add_lights` and `add_materials` events cost nothing
measurable.

**The single 894 ms frame** (B3, `add_lights` at 1500) lies **outside every
scope**. No scope maximum in that run exceeds 0.75 ms apart from the GPU-scene
scopes, which belong to the instance events. That build predates the slow-frame
detail recorder (`b360e3a`). R5.0's extra scopes and that recorder will localize it
if it recurs.

## 3. R5a: application decomposition (code motion)

### 3.1 Boundaries

| Unit | Location / target | Owns | Must not include |
|---|---|---|---|
| `FrameOrchestrator` | `src/app/FrameOrchestrator.*` (`iridium_app`) | main loop, frame context, profiler and allocation frames, observer phases, `beginFrame`/`submitFrame`/`endFrame`, swapchain/transport/resize application, input routing | ImGui |
| `RenderExtractor` | `src/extraction/` → new `iridium_render_extraction` (links scene, assets runtime, renderer-lighting, rhi, profiling) | queues and scratch, `GpuScenePublisher` and observations, light extractor, probe publisher, capture scheduler, shadow caches/atlas/pools, classification, extraction, sorts, schedules, `RenderFrame` assembly, `IRenderFrameStageObserver` counters | ImGui, editor, GLFW (configure-time link guard, as for `iridium_vulkan` in `tests/CMakeLists.txt`) |
| `EditorHost` | `src/editor/EditorHost.*` (`iridium_editor`) | `EditorSystem`, `IEditorRenderBridge`, retained views, view cadence, preview world and sun, asset-preview resolution, environment preview diagnostics, the editor build | runtime component mutation outside transactions |
| `AssetIntegration` | `src/app/AssetIntegration.*` | asset services, `ProcessMeshSwaps`, `tick` and counters, hot reload, loaded environments, baked-probe persistence, capture completions, material previews, startup model/environment | ImGui |
| `Application` | composition root | construction order, `initRenderer` wiring, cleanup order, `IAppControl` and run snapshot (both delegate) | frame logic |

### 3.2 Per-frame data flow (same order as today)

1. `Orchestrator`: platform events, input, then the observer's `PreSceneUpdate`.
2. `AssetIntegration::tick`: swaps, then runtime tick.
3. Transforms (into `SceneChangeSet`, R5c), then the observer's `PostSceneUpdate`.
4. `EditorHost::viewState()`: the **previous-frame** editor state that extraction
   already reads before acquire today (render view, selection, preview document,
   preview camera and lighting).
5. `AssetIntegration::onCaptureCompletions(...)`.
6. `RenderExtractor::prepare(view)`: lights, probes, publication (all before
   `beginFrame`).
7. `beginFrame`. On recreate, return, as today.
8. `publishGpuScene`, then the observer's `BackendFrameOpened`.
9. `EditorHost::build(...)` returns `EditorFrameRequests`.
10. `Orchestrator::apply(requests)`, at the same point and in the same order as
    1760-1800.
11. `RenderExtractor::extract(view', config)` returns `const RenderFrame&`.
12. `submitFrame`, then `endFrame`, then the observer's `onFrameEnd`, transport and
    resize.

### 3.3 `EditorFrameRequests` (in `app`, beside `AppFrameRequests`)

```cpp
struct EditorFrameRequests {            // produced once per frame by EditorHost::build
    std::optional<EditorOutputSettings> output;             // transport, EV, paper white, peak
    std::optional<ProjectShadowSettings> shadows;           // allocation fields ignored on apply
    std::optional<ProjectReflectionProbeSettings> probes;   // also reconfigures scheduler + backend
    EditorViewState view;   // render view, selected entity, preview document id/serial/framing,
                            // preview part selection/isolation/hover, orbit camera, preview
                            // lighting, debug view, wireframe, layered override, grid request
    RenderExtent requestedSceneExtent;                      // read after endFrame (2987-3036)
};
```

**Rules:**

- The editor never writes `ApplicationConfig`, the backend or the extractor.
  `Orchestrator::apply` performs today's three writes verbatim.
- The extractor sees only the PODs in `EditorViewState` and never an `EditorSystem&`.
  `appendModel`'s preview tests (1972-1977, 2126-2135) read the POD.
- The values Application writes into the editor today (1528, 1639-1660, 1669-1670)
  become `EditorHost` inputs (`setRenderingAssetView`, `setRetainedTextures`,
  diagnostic strings returned by `AssetIntegration`).
- Qualification stays behind `IFrameObserver` (`FrameObserver.h`). No unit gains a
  test hook. Equivalence is checked through captures, profiles and the digest.

### 3.4 Code-motion order (each step byte-identical; one commit each)

| Step | Change | Extra check |
|---|---|---|
| R5a.0 | `EditorFrameRequests`/`EditorViewState` introduced *inside* `Application.cpp`. The editor block returns requests; apply in place | profile counters/scopes identical |
| R5a.1 | `AssetIntegration` (923-1106, 1529-1545, 3174-3841, 3932-4411, 4522-4562) | editor smoke, hot-reload test, cook tests |
| R5a.2 | `EditorHost` (1549-1562, 1631-1670, 1749-1839, 3843-3930, cadence, input focus) | editor path `--validation-sync`, asset-viewer run |
| R5a.3a-d | `RenderExtractor` in four moves: (a) publication 1108-1397; (b) lights/probes 1546-1630; (c) shadow/probe schedules 2548-2937 plus `onRenderFrameStage`; (d) extraction, sorts, `RenderFrame` 1688-2547, 2945-2961, then the library target and link guard | digest identical to `r3a0`/`-ext` after each move |
| R5a.4 | `FrameOrchestrator`; `Application` becomes the composition root | `Application.cpp` < 800 lines; no file > 2,500 |

**Risk.** Moving code across TUs changes MSVC inlining (as found in R1). Under
`/fp:precise` this should not change results, and frozen captures would detect any
reassociation.

## 4. R5b: task system

### 4.1 enkiTS against Taskflow (verified against the repositories and docs, 2026-10-03)

| | **enkiTS v1.12** (2026-07-04, `0289cf6`; master `404a3bf` 2026-09-29 fixes dependency move/ClearDependency) | **Taskflow v4.1.0** (2026-06-20, `45366fe`) |
|---|---|---|
| License | zlib | MIT |
| API | `ITaskSet::ExecuteRange(TaskSetPartition, thread)`, `IPinnedTask`, `Dependency`; caller-owned task objects | `tf::Taskflow` graph plus `tf::Executor` (`run`, `async`, `dependent_async`, `corun`); `Subflow`, `Runtime`, `TaskGroup` |
| Priorities | **Yes**: `ENKITS_TASK_PRIORITIES_NUM` 1-5 (default 3) | **None in any release** (removed in 3.8.0); master has an opt-in `TF_ENABLE_TASK_PRIORITY`, not released |
| Pinned tasks | **Yes**: `IPinnedTask` on any thread number (0 = main); `RunPinnedTasks`/`WaitForNewPinnedTasks` for I/O threads | No (affinity only via `WorkerInterface`) |
| Allocation | none after `Initialize` (custom allocator hooks); a full pipe runs inline; allocation-free frames with `ITaskSet` subclasses (not the `std::function` wrappers) | node `new` per task unless `TF_ENABLE_TASK_POOL`; `make_shared<Topology>` per `run`; **not allocation-free per frame as shipped** |
| Waiting inside tasks | `WaitforTask(task, lowestPriorityToRun)` runs other tasks; only child tasks may be waited on | `corun`/`corun_until`; `future.wait()` in a worker can deadlock |
| External threads | `RegisterExternalTaskThread` | any thread may submit; no registration |
| Integration | 2 .cpp files plus a header, CMake ≥3.16, C API optional, no dependencies | header-only, CMake ≥3.18, turn tests/examples off |
| Standard / MSVC | C++11 | C++20 required since 4.0 (MSVC ≥19.29) |
| Maintenance | small and steady (commits through 2026-09); 7 open issues | very active; about 12k stars; 21 open issues |
| Profiling | thread-state callbacks only (wrap `ExecuteRange` for zones) | per-task observer `on_entry`/`on_exit`, TFProf |

**Recommendation: enkiTS, pinned to tag `v1.12`.** It is the only one of the two
that provides all three hard requirements in a release: priorities, pinned tasks
and zero steady-frame allocation. Its waiting rule (wait only on children, filtered
by priority) is the rule a frame scheduler wants. Taskflow's graph features are not
needed, and its missing priorities and per-task allocation conflict with this
plan's invariants.

The engine must not move `enki::Dependency` objects, because they are stored in
fixed arrays. Otherwise pin `404a3bf` (owner decision 1).

### 4.2 ADR-0015 draft: threading and task model

The number is reserved only in the plan header (line 13). No file exists, and
ADR-0016 is accepted. The ADR text is drafted below.

- **Decision.** One engine task system, `TaskSystem` (`src/core/tasks`, library
  `iridium_tasks`). It is the only owner of worker threads. Every enkiTS header is
  confined to its .cpp, and the public API is engine types:
  - `ParallelFor`;
  - `TaskHandle`;
  - `Strand` (a serial queue);
  - `Periodic`;
  - `PinnedIo`.
- **Priorities (enkiTS 3 levels):**
  - `FrameCritical` (HIGH): extraction, sorts and parallel-for work the main thread
    joins this frame.
  - `Normal` (MED): editor responsiveness, such as material-preview compile,
    menu-bar scans and upload preparation.
  - `Background` (LOW): cooking, thumbnails, hashing, catalog rebuilds and baking.
- **Main-thread rule.** The main thread joins frame work with
  `WaitforTask(t, FrameCritical)`, so it can never pick up a Normal or Background
  task.
- **Threads.** The default is hardware threads − 2: one for the main thread and one
  `PinnedIo` thread for blocking file and SQLite I/O.
  - On hybrid CPUs, the `threadStart` callback (also used for `CoInitializeEx(MTA)`
    on every worker, which replaces per-thread `ComApartment` setup) records P/E
    class through `GetLogicalProcessorInformationEx`.
  - Per-core affinity is not set by default. R5b.3 measures it.
- **Starvation bound.** An admission gate allows at most `workers − R` concurrent
  Background tasks, with R = 8 by default (the P-core count). Background work is
  split into tasks of at most about 5 ms where the code allows, such as per texture
  or per mip chain. A Frame task therefore waits at most one Background task's
  remaining time, and only when all R reserved workers are busy with Frame or Normal
  work.
- **Telemetry.**
  - `CpuProfiler` gains per-worker scope streams that are merged at `endFrame`,
    which provides critical-path and aggregate worker time per stage.
  - New counters: `task.frame.count`, `task.frame.start_latency_us` (enqueue to
    start), `task.background.active`.
  - The allocation counters become thread-scoped. `allocation.cpp.*` counts the
    main thread plus threads while they execute Frame tasks; `allocation.cpp.background.*`
    counts the rest.
  - Tracy is not adopted in R5 (owner decision 2).
- **Consequences.**
  - No `std::thread`, `std::async` or `std::execution::par` is used outside
    `iridium_tasks`.
  - Services keep their public APIs and change only their execution backing.
  - A failing task reports through the existing result queues; it never crashes a
    worker.

**Migration of each service:**

| Service | New form |
|---|---|
| 1 Catalog | `Strand` (Background) for imports and rebuilds; SQLite transaction on `PinnedIo`; results unchanged |
| 2 Model preparation | `Strand`; the DDC future poll (`:300`) becomes a continuation task |
| 3 Environment preparation | `Strand`; `.get()` becomes a continuation |
| 4 Thumbnails | `Strand` (Background; was BELOW_NORMAL) |
| 5/6 DDC | each cook key is a Background task (de-duplication map kept); the fork-join becomes a `ParallelFor` over texture-view jobs joined with `WaitforTask(child, Background)`; the convolution's `par` becomes a `ParallelFor` |
| 7 Reimport | `Strand` with the per-item `stop_source` kept |
| 8 Source monitor | `Periodic` (10 ms debounce tick) on the orchestrator's frame tick; SHA-256 hashing as a Background task **outside** the monitor mutex |
| 9 Watcher | `Periodic` 250 ms `stat` scan on `PinnedIo`, outside the mutex |
| 10 Material preview | Normal single-slot task, latest wins |
| 11 WIC lifetime | removed: every worker initializes the MTA in `threadStart` (or the thread is kept as the documented exception, if R5b.0 shows WIC needs a dedicated owner) |
| Menu-bar `std::async` | Normal task plus a completion flag; shutdown cancels it instead of blocking |

**Cook-while-render starvation test** (harness only):

- **Setup.** A qualification flag `--qualification-background-cook <manifest> --repeat`
  re-cooks a texture-heavy model (the Alfa source) with the DDC bypassed, in a loop,
  during T-F7 and T-F1.
- **Pass criteria**, against the same build without cooking (A,B,B,A, 500 + 10,000
  frames):
  - non-wait CPU median and p99 within the noise band plus 3 %;
  - `task.frame.start_latency` p99 ≤ 50 µs;
  - no frame over 2× median caused by a Frame task waiting;
  - cook throughput at least 90 % of the pre-R5b dedicated-thread build.
- **Also in R5b:** make `AssetRuntimeServiceTests` budget-deterministic (decision log
  2026-10-02).

## 5. R5c: extraction

### 5.1 Change sources

A `SceneChangeSet` is filled each frame. It is not an ECS rewrite. Its inputs:

- `TransformSystem`'s existing dirty list, which is finally consumed.
- Entity create/destroy events through `RegistryEntityObserver` (`Registry.h:75`).
- A new `ComponentPool::structureRevision`, bumped on add/remove/clear.
- An explicit `MeshComponent::revision` / `LightComponent::revision` /
  `ReflectionProbeComponent::revision`, bumped by every writer: editor transactions,
  `ProcessMeshSwaps`, startup, and the harness's scripted changes.

**Safety net.** A qualification-only `ExtractionVerifier` observer re-runs today's
full-scan observation every N frames (every frame on the frozen set and sweep). It
fails on any divergence, which catches writers that bypass the revision.

### 5.2 Steps (each keeps the frozen set byte-identical and the digest identical to `r3a0`/`r3a0-ext`)

**R5c.1: caster revisions without hashing.**

- **Change.** `shadowCasterRevision` becomes a combination of:
  - the publisher's `publicationRevision`;
  - the consumer membership revision;
  - a running maximum of the member records' transform, instance, primitive and
    geometry revisions, kept per consumer list at pack time;
  - the material table's `packedRevision` watermark;
  - a direct-packet revision.

  Per-cascade revisions use the same combination over the cascade's members.
- **Invariant.** The *change relation* must be identical: the new value changes in
  exactly the frames where the old hash changed.
- **Corner case.** A→B→A content returns the old hash and keeps a cached tile valid.
  The monotonic revision would re-render it. Static frozen scenes cannot reach it.
- **Check.** A harness-side `RevisionEquivalenceOracle`, attached through the
  backend extension, computes the old FNV beside the new revision on the frozen set,
  sweep and H-stress, and asserts identical change frames.

**R5c.2: epoch-based depth-history revision.**

- `depthContentRevision_` becomes the main-opaque membership revision plus the
  record-revision watermark plus the direct-packet revision.
- It uses the same equivalence oracle.

**R5c.3: compact sorts.**

- Each queue sorts a `{key…, uint32 index}` array of 16-24 B. The same `std::sort`
  is used, with a comparator that reads only the same key fields in the same input
  order, and the 240 B packets are then gathered once.
- `std::sort`'s comparisons and moves depend only on comparison results, so the
  permutation, including today's ties in the non-total opaque comparator, is
  identical.
- A unit test checks permutation identity on randomized and dumped F1/F7/H-stress
  queues. The transparent comparator already ends in the unique
  `TransparentWorkIdentity`.
- A deterministic total-order opaque tie-break (owner UUID, then primitive) is a
  separate image-changing option (owner decision 4).

**R5c.4: parity retirement (sub-steps a-f).**

- **(a)** Publish `mainOpaqueConsumerPrimitiveIndices` and its membership revision.
- **(b)** `OpaqueSubmission` in `RenderFrame`. The extractor emits the index list
  sorted by the compact key `((pipeline<<32)|material, geometry handle, firstIndex)`,
  read from the primitive and geometry records. Its input order is ascending
  primitive order, which is exactly the parity loop's order, so the permutation,
  bins and candidate host bytes are unchanged. The culler `plan` reads records
  instead of packets.
- **(c)** Mixed bins. Direct packets are merged into the same key sequence (directs
  first, as appended today) and drawn directly inside the pass. This changes
  behaviour only when direct and GPU-scene opaque work coexist, because today that
  case falls back for the whole queue. The frozen set has no such fixture; confirm
  with `opaque.indirect.fallback_reason`.
- **(d)** Depth history (R5c.2).
- **(e)** Selection from the instance flag plus a frustum test of the selected
  instance's primitives only. The 182 µs classification is restricted to
  ForwardOpaque primitives plus the selected instance. Oracle bin expectations
  (`:445`) move to the qualification oracle.
- **(f)** An indirect wireframe pipeline variant (`polygonMode LINE`) over the same
  bins. Compare X04 against the base build; if the draw order changes the image,
  log a wireframe envelope.
- **Not retired:** ForwardOpaque parity packets (768 on F7). They remain until the
  forward path is index-driven (M8 or later).

**R5c.5: incremental GPU-scene publication.**

- **Application side.** Observation walks only changed entities (`SceneChangeSet`)
  instead of the mesh pool, and drops the per-entity `memcmp`.
- **Publisher, data structures.** It switches to dense handle-indexed state with
  sorted flat key arrays instead of `std::map`. Observations are no longer
  deep-copied (`:445`).
- **Publisher, repack rule.** When no instance is added or removed, changed instances
  repack in place: same dense ranges, revisions bumped only for the touched records.
  `revise` runs only over touched ranges. A topology change still does a full
  repack, but without map nodes or allocation in steady growth.
- **Byte-identity.** Packed tables and revision numbers must be byte-identical to
  today's for the same input sequence. A unit test replays the H-stress script and
  F-fixture observations against the old publisher.
- **Target.** `synchronize` at `add_instances` falls from about 125 ms to under
  10 ms.

**R5c.6: change-driven lights and probes.**

- `LightExtractor` and `ReflectionProbePublisher` re-pack only changed entities and
  transforms. Their existing record and active-list revisions stay exact.
- The per-frame result vectors become persistent scratch.

**R5c.7: parallel extraction and transforms.**

- **Extraction.** The transparent `appendModel` pass is chunked by mesh-pool order,
  and each chunk writes its own scratch. Chunks are concatenated in chunk order, so
  the pre-sort sequence is identical. Intervals are computed per chunk. A per-model
  cached list of transparent submeshes avoids visiting the opaque ones (F7: 11,136
  visits become 3,904).
- **Transforms.** `TransformSystem` parallelizes dirty roots only above a threshold
  (on F7 it costs 2 µs, so it is kept serial unless measured useful).
- **Determinism.** Outputs are per index, with no atomics in the result order.

**R5c.8: zero steady allocations on every non-qualification route.**

- The sources in §1.6 become persistent scratch: return by `span` into owned
  buffers.
- Find the Ordinary2 2-call source with a harness allocation-site tracer: a
  qualification-only `--qualification-allocation-trace`, which captures a stack for
  steady-frame allocations.
- Add F5-hetero and F6-probecap profiles to the allocation check (owner decision 5).

**R5.0 (first; profiling only): attribution.** It adds these scopes:

| Scope | Covers |
|---|---|
| `cpu.render.frustum_classify` | |
| `cpu.render.transparent_intervals` | |
| `cpu.shadow.directional.schedule` | |
| `cpu.shadow.local.schedule` | including the caster revision |
| `cpu.probe.capture_schedule` | |
| `cpu.render.depth_history` | |
| `cpu.renderer.stage.shadows` / `.probes` | |
| `cpu.asset.material_previews` | |
| `cpu.probe.finalize` | |

Target: unattributed ≤ 0.10 ms on F7. Counters are unchanged and the scope tree
grows by design, so this is logged as a profile-schema delta.

## 6. Slices and verification

### Serial order

R5.0 → R5a.0 → R5a.1 → R5a.2 → R5a.3a-d → R5a.4 → R5b.0 (enkiTS approval and spike) →
R5b.1 (`TaskSystem`, ADR-0015, worker scopes, thread-scoped allocation counters) →
R5b.2 (service migrations, one per commit) → R5b.3 (starvation test) → R5c.1 → R5c.2
→ R5c.3 → R5c.4a-f → R5c.5 → R5c.6 → R5c.7 → R5c.8 → R5 final (F7 timing pair, H-stress
hitch pair, `FRAME_BUDGET.md`).

### Parallel lanes (disjoint writers only)

| Lane | Scope | Constraint |
|---|---|---|
| Main checkout | R5.0, R5a, then R5c.4/5c.7 integration in the extractor | owns `src/app`, `src/extraction`, `src/editor` |
| Worktree T | R5b.0-1: `src/core/tasks`, enkiTS FetchContent, worker scopes in `src/profiling`, tests | its CMake lands before or after R5a.4's targets, never at the same time |
| Worktree P | R5c.5 publisher internals (`renderer/scene`) plus the replay test; R5c.3 compact-sort helpers and the permutation test (`renderer/rhi`) | no `src/app` edits; integrated after R5a.3 |
| Worktree O | R5c.1/R5c.2 backend revision APIs (`VulkanShadowCasters`, `VulkanOpaqueFeature`, `GpuScene.cpp` membership watermarks) plus the harness oracle | backend headers are not touched by another lane |

### Evidence per slice

| Slice | Tier (`FRAME_BUDGET.md`) | Evidence |
|---|---|---|
| R5.0 | refactor | identical captures and counters; scope-tree delta logged; unattributed figure |
| R5a.0-4 | refactor | byte-identical frozen set; digest = `r3a0`/`-ext`; identical scope tree, counters and memory on F1-all, F4-*, F5-*, F6-probecap, F7-hiz; sweep 36/36, 0 changes; editor/viewer runs with `--validation-sync`; one T-F1/T-F7 pair |
| R5b.1-2 | refactor | the above, plus cooked-artifact hashes identical on the cook tests and a scripted hot reload |
| R5b.3 | refactor (timing) | starvation-test pass criteria (§4.2) |
| R5c.1, R5c.2, R5c.3, R5c.5, R5c.6, R5c.8 | refactor | byte-identical; equivalence oracle, replay or permutation tests; T-F1/T-F7 pair; R5c.5 and R5c.6 add an H-stress hitch pair |
| R5c.4a/b/d/e | refactor | byte-identical, digest identical (candidate host bytes) |
| R5c.4c, R5c.4f | refactor; **feature thresholds only for a route whose image changes** (mixed-bin fixture, X04 wireframe) | logged envelope |
| R5c.7 | refactor | byte-identical; per-stage critical path and worker time |
| R5 final | — | F7 non-wait ≤ 3.0 ms in A,B,B,A against `6298757`; per-stage critical path and aggregate worker time; 0 steady allocations on T-F1, T-F7, F5-hetero and F6-probecap; `FRAME_BUDGET.md` updated |

### Risks

- **Revision semantics.** A monotonic revision can invalidate a cache that content
  hashing kept valid (A→B→A). This is guarded by the equivalence oracle, and any
  difference on dynamic fixtures is logged.
- **Missed writers.** A component write that bypasses the revision would silently
  skip extraction. Guarded by the full-scan `ExtractionVerifier` and by keeping the
  full scan as a qualification switch until R6.
- **Sort permutation.** It relies on `std::sort` behaving the same for different
  element types. The permutation test is the gate; the fallback is to keep packet
  sorts for that queue.
- **Task-system starvation and E-cores.** Guarded by the admission gate, the
  starvation test and the main-thread priority filter. Affinity is measured, not
  assumed.
- **Allocation metric drift.** Background allocations would pollute the steady
  metric unless R5b.1 makes the counters thread-scoped first.
- **Parallel nondeterminism.** Mitigated by per-index outputs and chunk-ordered
  concatenation; frozen captures detect it.
- **Present serialization (finding 3).** It hides R5's gains in frame time. It is
  not an R5 regression.

### Decisions (resolved 2026-10-03)

1. **enkiTS adopted.** Owner approved enkiTS (zlib) at `v1.12` (`0289cf6`) for R5b.
   If R5b needs movable `Dependency` objects, the move to `404a3bf` goes back to the
   owner as a new pin.
2. **Tracy is deferred.** The per-worker profiler scopes in R5b cover attribution.
3. **The image-owner fence wait is fixed in R4d.4.** The cause was not mailbox
   presentation. The per-image map stored the slot fence, and with 3 images and 2
   slots that fence had already been reused by the newer frame, so every frame
   waited on the previous frame's GPU work. Per-image timeline serials took the
   T-F7 wait from 1.97 ms to 0.003 ms; T-F7 CPU frame went from 5.41 to 3.40 ms
   (`timing/r4d-short`). Finding 3 above describes the pre-R4d state.
4. **The deterministic opaque tie-break is out of scope for M7R.** It changes
   images, so it would need the feature tier. It is recorded for M9, where TAA
   history makes tie stability matter.
5. **The allocation invariant is widened** to every non-qualification route in
   R5c.8. A lit route and a probe route join the timing set.
6. **Mixed bins (R5c.4c) are accepted.** Frozen images must stay identical. The
   indirect digest changes only on fixtures with direct packets, and each change
   is explained in the slice's evidence.
