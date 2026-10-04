# M7R Architecture Consolidation Execution Plan

## Header

- **Milestone:** M7R — Architecture consolidation
- **Status:** In Progress — plan approved by owner 2026-10-02; R0–R2 accepted 2026-10-02, R3 accepted 2026-10-03; R4 active
- **Lead:** M7R milestone-lead session (Claude Code); integration owner for all slices
- **Branch / PR:** `m7r-consolidation` off `Render-Refactor-for-Modularity`; one PR
  for the milestone
- **Last updated:** 2026-10-02
- **ADRs:** ADR-0002 (graph/HDR), ADR-0003, ADR-0006, ADR-0012 (transparency),
  ADR-0013 (transport switching), ADR-0014 (GPU-scene identity). Expected new:
  ADR-0015 threading/task model, ADR-0016 graph execution model (refines ADR-0002).
- **Dependencies:** accepted M0–M7.7, committed M7.8 checkpoint (`ae4cc4c`), program
  direction commit `23d9ced`
- **Lead prompt:** `docs/milestones/M7R-task-lead-prompt.md`
- **Evidence tier:** behavior-preserving refactor (`docs/performance/FRAME_BUDGET.md`)

## Objective and user-visible outcome

When M7R is accepted, the engine renders byte-identical images on the frozen set,
and the following are measurably better:

- **Build:** incremental rebuilds after touching `Application.cpp`, a common RHI
  header, or a shader include are faster. Each production source compiles once per
  configuration.
- **Hitches:** capacity growth, probe changes and uploads no longer drain the GPU,
  so p99 frame time and hitch counts improve.
- **CPU frame:** serial main-thread time reaches the ≤3.0 ms target on the
  CPU-heavy fixture, and steady frames are allocation-free again.
- **Structure:** M9 can add motion vectors, jitter, history, TAA, bloom and exposure
  as graph passes with declared resources, without editing a 13k-line backend or
  `drawFrame`.

## Current context (audit 2026-10-02, against `23d9ced`)

Four read-only audits re-checked the takeover findings. Corrections are in bold.

| Area | Finding |
|---|---|
| `VulkanVertexBackend.cpp` | 13,286 lines plus a 1,102-line header; ~306 data members and ~162 methods. 42 pass classes already exist; the monolith is mostly per-view orchestration, culling and qualification. |
| Culler duplication | Directional, spot, point and probe prepare/create/bind/validate total 2,577 lines, plus 889 lines of collectors. Spot and point collectors are **identical after renaming**, all four `bind*IndirectBuffers` are identical, and the four kinds already share one set-2 layout. **Main-view opaque is materially different** (Hi-Z, `GpuSceneIndirectPolicy`, a 6-binding layout), so it is an extension of the shared culler, not just another parameter set. |
| `submitForwardQueues` | 913 lines (11146–12058), ~480 of them transparency. |
| `IRenderBackend` | **67 methods** (62 pure), not 69. 8 are capture/oracle hooks; 8 are ImGui/editor-coupled (`beginUI`, `submitUIPass`, `get*TextureID`, retained views, grid overlay); 8 are fixed `submitX` stages. GLFW and scene-component headers also leak into `rhi/`. |
| Graph | 41 `addPass` sites. Pass count depends on configuration: **19 in the base SDR graph, up to 53** with every feature (56 with legacy transparency). Internally index-addressed. Kahn sort keeps declaration order. No culling. Transient slot reuse needs exactly equal descriptors. |
| Execution | **35 `beginPass` and 21 `skipPass` call sites**, string-matched by a sequential cursor that throws on mismatch. One sync1 barrier per resource; stage masks are broad. Compiled load/store ops and `QueueClass` are ignored. 33 manual barriers across the vulkan directory. Undeclared GPU work: probe capture, shadow and GPU-scene compaction, probe clustering, depth-pyramid mips. |
| ADR-0002 gap | **The executor does not own queue synchronization, history, or imported-image transitions,** although ADR-0002 says it does. `History` lifetime exists in the graph but has no users. History is CPU-owned in the backend (depth pyramid, LOD). |
| Vulkan features | API 1.3 requested. Synchronization2, dynamic rendering, timeline semaphores, BDA and `VkPipelineCache` are all unused. 15 `vkCreateRenderPass` and 12 `vkCreateFramebuffer` sites. Graphics plus present queues only; uploads also go on graphics. |
| Memory | `VulkanResourceAllocator` creates one `VkDeviceMemory` per resource, with 19 profile categories. No aliasing. |
| GPU drains | **22 `waitForAllFrames()` sites.** 8 are capacity growth, 4 are probe changes, 4 are capture collection, and the rest are LUT, resize, topology, retained-view and shutdown. 8 `uploadContext.flush()` calls do submit plus a `vkWaitForFences(UINT64_MAX)`, including one in `beginFrame` whenever uploads are pending. |
| Qualification in backend | About 2,560 lines, ~20%, always compiled. In Debug, `enableValidation` also turns on four CPU oracles. `lighting.cluster.readback` copies 64 bytes **every frame**. `shadow.virtual.request-readback` keeps depth as a TransferSource **whenever VSM is on**. |
| `Application.cpp` | `drawFrame` is 1,683 lines. **No 540-line lambda exists**: the extraction section is 569 lines and `appendModel` is 236. About 2,500 lines (~37%) are qualification/benchmark code. There are **≥10 ECS pool walks per frame, not three**. The parity stage rebuilds 7,232 × 240 B `DrawPacket`s per frame on the dense fixture. Its only real consumers are G-buffer binning (which needs only the primitive index and index range), depth-history hashing, wireframe and selection. Shadows and probes already use primitive spans. |
| Change tracking | `changedTransformEntities_` is filled but **never consumed**. `GpuScenePublisher` has revisions and a fast path, but the observation revision is a full per-frame `memcmp` scan. There is no registry-level component journal. |
| CLI | 83 flags in one 82-branch `else if` chain (`ApplicationConfig.cpp:85–726`). |
| Threads | 11 dedicated `std::jthread` services (deque + mutex + condition variable), an ad-hoc fork-join in `GltfModelImporter`, and `std::async` in the menu bar. **No task system.** Editor UI build, extraction and sorts all run after the fence wait. |
| **Allocations** | **Steady frames are not currently allocation-free.** Local runs at `5a9f705` (dirty tree) show 2 calls / 13,006 B per frame on `m7_occlusion_dense_depth_stack_v1` and 16 calls / 3,292 B on `m7_heterogeneous_shadow_warm_motion_v1`. Likely sources are local vectors in `drawFrame`. R0 re-measures at HEAD. |
| **CPU budget** | **The dense depth-stack fixture is already over target: 3.42 ms of non-wait main-thread CPU** (frame 4.83 ms minus 1.41 ms fence wait). 1.69 ms is not attributed to any scope, including a 1.43 ms gap between the transparent sort and G-buffer record. |
| Build | **117 `.cpp` files on the engine** (not ~98). 6 static libraries already exist. **9 sources are compiled into the engine and also into a library it links** (ODR hazard). 339 compilations of 191 distinct sources; `CpuAllocationProfile.cpp` is built 15×. No PCH, unity build or warning level. **The Debug preset has no `/Zi`, `/Od` or `/RTC1`.** Shaders: a GLOB without CONFIGURE_DEPENDS; every shader depends on all 20 includes; SPIR-V is written into the source tree. GLM uses the deprecated `FetchContent_Populate`. |
| Include cycles | rhi↔scene, rhi↔assets, rhi↔material, rhi↔lighting, rhi→vulkan (factory), ecs↔scene, and core→everything (17 modules). |
| Tests | 78 ctest entries, no labels. Two Vulkan tests are built but not registered. Source-text checks: **351 `find`/`npos` checks** in 15 of 34 `Stage3ArchitectureTests` cases (48 are literally `source.find`), plus ~107 more in `SceneColorTests`, `LightingReferenceTests`, `StandardMaterialShadingTests`, `M4ProductionCutoverTests`, `ReflectionProbeTests` and `LightExtractorTests`. Release 78/78 pass (2026-10-02). |
| Vendored | `src/vendor/imguizmo/example` and `vcpkg-example` hold a second imgui (1.92.5), sample code and images. They are tracked but not compiled. |

**Sizes over the 2,500-line guideline outside M7R scope:**
- `AssetBrowserPanel.cpp` (3,273)
- `GltfModelImporter.cpp` (3,537)

Both are recorded as deferred exceptions. M7R applies the guideline to the files it restructures.

## Invariants

- **Image output:** byte-identical scene-linear PFM and final-SDR TGA on the frozen
  set after every slice. A documented floating-point reassociation is the only
  exception; it switches the affected fixtures to the feature-admission thresholds.
- **Timing:** no median or p99 CPU/GPU regression beyond noise in the matched
  native-4K Release pair (A/B then B/A).
- **Validation:** Vulkan validation is clean on the frozen set. Synchronization
  validation is clean from R3 onward.
- **Allocations:** zero steady-frame C++ allocations on the timing routes, as measured
  at R0. No route may rise above its R0 count.
- **ADRs:**
  - one clustered-light representation;
  - scene-linear AP1 until the single output transform;
  - independent main/shadow/probe/selection visibility;
  - ADR-0014 generation-safe identity;
  - ADR-0013 live transport switching;
  - resize and zero-extent safety.
- **Feature flags:** LOD and Hi-Z remain workload-selectable and default-off. M7.8 VSM
  stays default-off, compiles, and passes its tests.
- **Layering:** the RHI stays backend-neutral. Runtime code gains no ImGui or editor
  dependency, and existing coupling only moves outward toward an editor host.
- **Evidence:** every qualification capability (captures, oracles, validators,
  benchmarks) still works, so evidence stays reproducible.
- **Content:** no third-party content is staged. `assets/` stays allowlisted.
  `imgui.ini` is never committed.

## Scope and non-goals

**In scope:** slices R0–R6 below.

**Out of scope:**
- TAA, bloom, auto-exposure, AO, GI and RT (all M9 or later);
- VSM completion (resumes after M8);
- M7.9–M7.12;
- mesh shaders (M8);
- material-editor follow-ups and the Porsche mixed-class glass defect (both after M9);
- an ECS or math-library rewrite;
- async compute (the graph can express it; it is recorded as a later candidate gated
  on timeline semaphores);
- enabling `/W4` or warnings-as-errors (a deferred cleanup that would add noise);
- splitting the editor and importer files over 2,500 lines.

## Design and data flow (target end state)

```text
app (main, FrameOrchestrator)
 ├─ editor-host (EditorHost, ImGui, ImGuiVulkanBridge)  ── optional qualification (IFrameObserver, CLI registrations)
 ├─ render-extraction (RenderExtractor → immutable RenderFrame)
 ├─ asset-integration (AssetIntegration::tick → publications)
 └─ task system (frame/high, normal, background priorities)
renderer-vulkan: VulkanBackend (frame lifecycle) + feature owners registering graph passes
   (IndirectViewCuller, Shadows, Probes, Opaque/GBuffer, Lighting, Transparency, Output, GpuSceneUpload)
renderer-graph (backend-neutral declaration, compile, history, aliasing plan)
renderer-rhi (types, capabilities, IRenderBackend: init/resources/beginFrame/submitFrame/endFrame)
material · assets · scene · ecs · profiling · core/types · platform
```

- **Module DAG:** core/types (AssetGuid, SceneEntityUuid, RenderHandles,
  TextureTypes, policy PODs) sits at the bottom. `RenderBackendFactory` leaves `rhi/`.
  `TransformSystem` moves to scene. Qualification is its own library that links
  everything and is omitted when `IRIDIUM_QUALIFICATION=OFF`.
- **Pass model:** feature owners register passes with the graph declaration (setup)
  plus a Vulkan execute callback. The executor runs callbacks in compiled order using
  `ResourceHandle`/pass indices, issues one `vkCmdPipelineBarrier2` per pass, and
  derives dynamic-rendering attachments (load/store/clear) from declared usages.
  During migration, imperative `beginPass(name)` coexists: it first drains any
  earlier callback passes.
- **History:** the graph gains real cross-frame history, as previous/current
  physical pairs with validity reset keyed to `ViewHistoryContext.resetRevision`.
  It is exercised by tests and unused by production until M9.
- **`IRenderBackend`:** shrinks to init/capabilities/resources, `beginFrame`,
  `submitFrame(const RenderFrame&)`, `endFrame`, transport/resize, and telemetry
  queries. The editor UI moves to an `IEditorRenderBridge` implemented by a
  renderer-vulkan-imgui target. Capture and oracle hooks move to a Vulkan-side
  qualification extension that the harness attaches.
- **Lifetime:** destruction and capacity growth go to a deletion queue keyed by
  timeline value or frame fence. Uploads go on a transfer queue (falling back to
  graphics) and signal a timeline semaphore that graphics waits on.
- **Threading:** one engine task system (ADR-0015). Services become long-running
  background tasks or pinned tasks. Frame work uses the highest priority.

## Vertical slices

Exactly one slice is `In Progress`. Every slice ends with:
- Debug and Release builds;
- all ctest suites;
- validation on the frozen set;
- byte-identical frozen captures;
- one matched timing pair;
- a `git status` check;
- a commit named `M7R <slice>: …`.

### R0 — Baseline freeze (`Accepted` 2026-10-02)

**Frozen capture set.** Native 3840x2160, Release, SDR transport, ACES 2.
- One `scene` PFM and one `final-sdr` TGA per fixture.
- Every capture uses `--require-capture-signal`, 12 warm-up frames and capture frame 4.

| ID | Fixture | Manifest | Content |
|---|---|---|---|
| F1 | `m7_three_dense_all_visible_v1` | `assets/m7-three-dense-assets-manifest.v1.json` | Alfa Romeo, local-only |
| F2 | `m7_three_dense_one_visible_v1` | same | local-only |
| F3 | `m7_many_instance_stress_v1` (256 instances) | same | local-only |
| F4 | `ordinary2_lit_closed_v1`, `cinematic8_nested_tetrahedra_v1`, `weighted_oit_particles_v1` | `assets/benchmarks/m6/ordinary2-runtime-manifest.v1.json` | tracked |
| F5 | `m7_heterogeneous_shadow_warm_motion_v1` (directional + spot), `point_shadow_contact_v1` | `assets/m7-heterogeneous-shadow-admission-manifest.v1.json`, `assets/m7-local-shadow-device-manifest.v1.json` | tracked |
| F6 | `m7_probe_lod_reflection_motion_v1` | `assets/m7-probe-lod-admission-manifest.v1.json` | local-only |
| F7 | `m7_occlusion_dense_depth_stack_v1` with Hi-Z on (`--experimental-depth-pyramid --experimental-depth-occlusion-rejection`); F3 with LOD on (`--experimental-gpu-lod-error-pixels 2 --gpu-lod-max-level 15`) | occlusion-performance and three-dense manifests | local-only. Keeps the workload-selectable paths covered. |

**As implemented (R0):**
- *Fixture definitions:* `tools/m7r/M7RFixtures.ps1` holds the exact fixture and route definitions.
- *Cooked inputs:* models are cooked at the R0 commit into `out/m7r/ddc` by `Cook-FrozenModels.ps1`. The LOD route uses a separately cooked Alfa artifact made with the M7.5 topology-transactional LOD settings (`out/m7r/meta/alfa_romeo.lod.iridium.meta`); the default artifact has no LOD chains, so LOD flags alone are a no-op.
- *Environment:* every fixture uses its declared constant environment, so no HDRI or environment artifact is involved.
- *Determinism:* two fixtures vary from run to run at R0. They use measured tolerance envelopes; all other captures must be byte-identical (see the decision log).

**Timing pair routes.** F1 is the GPU-representative route and F7 the CPU-heavy one. Settings:
- 500 warm-up / 10,000 measured frames;
- hidden borderless window, mailbox present mode;
- no validation, no capture;
- CPU and GPU profiling to JSONL;
- run order A/B then B/A.

**Also recorded:**
- per-frame allocation calls and bytes;
- per-frame `waitForAllFrames` and `uploadContext.flush` counts, through a new telemetry counter (no output effect);
- hitch count (frames over 2× median) and p99 for a scripted probe/capacity-change run. *As implemented:* no existing flag grows capacity or changes probes mid-run; table-scale validators run at startup. The scripted change run is therefore built as harness tooling at the start of R4c. Its baseline is measured then against the preserved R0 worktree build (`out/m7r/worktrees/r0`, commit `2c50b36`). R0 records steady-state drain counts on the timing routes;
- clean Debug and Release build times;
- incremental build times after touching `Application.cpp`, `rhi/Mesh.h`, one `.glsl` include and one `.comp` shader.

**Tooling.** `tools/m7r/Run-FrozenCaptures.ps1` and `tools/m7r/Run-TimingPair.ps1` make the runs reproducible:
- each writes a hash table under `out/m7r/` (not committed) and prints a markdown summary;
- `tools/m7r/Compare-FrozenCaptures.ps1` checks byte identity against the R0 table;
- these are harness tools and touch no production code.

**Completion:**
- the hash, timing, allocation, drain and build-time tables are recorded in the plan's evidence section;
- the hash table is valid for this machine only, because the local-only fixtures (F1–F3, F6) cannot be committed;
- the historical M7 hashes stay as historical record and are not overwritten.

### R1 — Build system and module DAG (`Accepted` 2026-10-02)

**Layering:**
- Break the include cycles by moving identity and handle PODs to `core/types`.
- Move `RenderBackendFactory` to renderer-vulkan, and `TransformSystem` to scene.
- Move `ReflectionProbeCapture` and grid-overlay types behind RHI-owned PODs.
- Remove the GLFW types from `rhi/` by passing an opaque platform window handle.

**Libraries:**
- One `CMakeLists.txt` per module, as STATIC libraries: core, ecs, scene, assets, material, renderer-rhi, renderer-graph, renderer-lighting, renderer-vulkan, renderer-vulkan-imgui, profiling, capture, benchmarks, editor and platform. Vendor code becomes its own libraries.
- The engine executable becomes `main.cpp` plus app.
- Tests and tools link the libraries; no production source is compiled twice.
- Remove the 9 double-compiled sources.

**Build speed:**
- `target_precompile_headers` on a shared PCH target covering std, glm, Vulkan and nlohmann_json.
- Unity builds only for vendor and test targets.

**Shaders:**
- glslc `-MD` depfiles, so a shader rebuilds only for the includes it uses.
- `CONFIGURE_DEPENDS`, an `iridium_shaders` custom target, and output to the build tree with a copy to the runtime location.
- `compile_shaders.bat` is deleted.

**Hygiene:**
- Proper Debug flags (`/Zi /Od /RTC1`; Release is unchanged).
- GLM moves to `FetchContent_MakeAvailable`.
- ctest labels (unit, vulkan, process, slow).
- Register the two unregistered Vulkan tests.
- Untrack the uncompiled `imguizmo/example`, `vcpkg-example` and `Images` directories (a `.gitignore` entry keeps them local).

**Completion:**
- build-time deltas against R0;
- Release code generation unchanged, so frozen hashes are identical and the timing pair is neutral.

**Rollback:** each module move is its own commit-sized step within the slice.

**As implemented (R1):**
- *Step 1 (`7781b03`):* include-cycle moves (see the decision log).
- *Step 2 (`6fcd762`):* a 40-line root `CMakeLists.txt` with helper files in `cmake/`, one CMakeLists per module, and the libraries listed in `src/CMakeLists.txt`.
- *Follow-up:* `iridium_scene` is split from `iridium_scene_authoring`, restoring the M4 JSON-free runtime boundary at link time.

Deviations from the plan text:
- **Release PCH:** engine-module PCH is Debug-only (option `IRIDIUM_ENGINE_PCH_IN_RELEASE`, default OFF). A PCH changes MSVC inlining in 176/191 Release objects, which would break byte identity. Tests and tools use the PCH in both configurations.
- **GLM and PCH:** GLM is excluded from the PCH of TUs that define `GLM_FORCE_DEPTH_ZERO_TO_ONE`.
- **Unity builds:** not used; there is no benefit for single-TU tests, and vendor codegen would change.
- **Shader output:** stays in `assets/shaders/`; depfiles were added.
- **Implicit layers:** `vulkan`-labelled tests run with `VK_LOADER_LAYERS_DISABLE=~implicit~`. The owner's ReShade implicit layer fails to load (error 1114) and was counted as a validation error.

### R2 — Qualification harness and CLI (`Accepted` 2026-10-02)

The implementation design, with the file inventory, interfaces, test disposition and
ordered sub-steps R2.0–R2.10, is in `docs/milestones/M7R-R2-qualification-harness-design.md`.

**Harness library:**
- `iridium-qualification` holds oracles, readbacks, capture validation, the `IRIDIUM_*` reporting, resize and lifecycle validators, benchmark scene construction and run-report export.
- Application side: an `IFrameObserver` with startup, frame-begin, before-submit, frame-end and shutdown hooks.
- Backend side: a `VulkanQualificationExtension` that the harness attaches through the backend factory. Readback and oracle passes are contributed through it.
- The 8 capture and oracle methods leave `IRenderBackend`.
- The four oracle `collect*` paths keep their GPU waits only when qualification is active.

**Configuration:**
- `IRIDIUM_QUALIFICATION` is a CMake option, ON by default. OFF builds a shipping-shaped engine with no oracle code.
- Decouple the CPU oracles from `enableValidation`. Debug no longer silently runs oracles; the `vulkan` ctest label runs them explicitly.

**Unconditional readbacks:**
- Make `lighting.cluster.readback` conditional on the telemetry that consumes it.
- Make the VSM request readback's depth TransferSource usage conditional on the oracle.
- Both are byte-identical; VSM is default-off.

**CLI:**
- A data-driven flag registry, `CliOptionRegistry`: name, arity, parser, help, and owning module.
- Modules register their own flags, so the qualification flags disappear with the library.
- Usage text is generated from the registry.
- The behavior of all 83 existing flags is preserved and tested.

**Tests:**
- Replace source-text tests with behavioral ones:
  - compiled-graph topology and usage assertions;
  - C++/GLSL ABI static assertions and CPU-side layout tests;
  - headless Vulkan pipeline-creation tests, where validation catches layout or push-constant drift;
  - glslc depfile checks for shared-BSDF include ownership;
  - fixture captures for pass wiring.
- An assertion with no behavioral equivalent is deleted, with a log entry.

**Owner-approved 2026-10-02:** remove `--developer-legacy-transparency` (see the
decision log):
- remove the legacy graph topology, glass-depth and two-bucket resources, and the 11 backend references;
- keep `LegacyTwoBucket` readable in serialized data so CookKeys are unchanged;
- amend ADR-0012 with a superseding note.

**Completion:** no qualification capability is lost, which is checked by rerunning
the R0 tooling plus every validator flag once.

**As implemented (R2, `aad3048`…`8e2fae7` + R2.10):**
- **Harness:** `iridium_qualification` (`src/qualification`) holds the harness (`harness/`), the Vulkan extension, indirect/VSM oracle and readback analyzers (`vulkan/`), `QualificationOptions` (36 flags), `QualificationResults.h` and the neutral `IQualificationBackend`.
- **App side:** the app talks to it only through `IFrameObserver` (`src/app/FrameObserver.h`). Routing that changes production behavior (direct reference routes, OIT order seed, resident LOD floor) reaches the app through `AppRunPolicy::routing`.
- **Backend:** extensions attach through `RenderBackendCreateInfo`. `IRenderBackend` goes from 67 to 59 methods; `RenderBackendConfig` loses six oracle/validator fields.
- **CLI:** the `CliOptionRegistry` keeps per-owner registration. `main.cpp` holds the only `#if IRIDIUM_QUALIFICATION`.
- **Libraries:** build provenance lives in a generated `core/BuildInfo.cpp`. `renderer/transparency` is now its own `iridium_transparency` library.
- **Shipping preset:** `x64-release-shipping` builds with `IRIDIUM_QUALIFICATION=OFF` and contains no qualification strings.
- **Legacy and readbacks:** legacy transparency is removed (ADR-0012 amended). Cluster readback and the VSM depth transfer source are declared only for their consumers. The oracles are no longer implied by `--validation`.
- **Tests:** source-text tests are replaced by behavioral tests (see `M7R-R2-assertion-disposition.md`).
- **Sizes:** `Application.cpp` 6,703 → 4,544 lines; `VulkanVertexBackend.cpp` 13,286 → 11,554.

### R3 — Graph-driven execution and backend decomposition (`Accepted` 2026-10-03)

The implementation design, with the current-state inventory, culler interface, executor/sync2/history model, owner
migration order and ordered sub-steps R3.0–R3c.12, is in `docs/milestones/M7R-R3-graph-execution-design.md`.

**R3a — Shared indirect view culler.**
- `VulkanIndirectViewCuller` is configured per view kind:
  - set-0 layout;
  - shader;
  - push-constant schema;
  - work-item enumeration (cascade mask, spot slot, point slot × face, probe face mask);
  - LOD policy;
  - consumer flag;
  - capacity.
- It replaces the directional, spot, point and probe prepare/create/bind/collect code.
- Opaque main view uses the same core plus an occlusion/indirect-policy extension.
- Expected reduction is about 2,500 lines.
- Command streams must be identical, so recorded dispatch order and buffer contents are byte-compared in a test.

**R3b — Callback executor and sync2.**
- Enable `synchronization2`.
- Use index-addressed `beginPass`/resource lookup, with an O(1) name map only as a transitional path.
- Pass setup and execute callbacks.
- One `VkDependencyInfo` per pass. Keep the existing same-access storage re-barrier semantics.
- Declare the currently undeclared GPU work as graph passes and bring imported-image transitions under the executor:
  - probe capture;
  - shadow and GPU-scene compaction;
  - probe clustering;
  - depth-pyramid mips;
  - the two out-of-plan capture transitions.
- Real history lifetime with tests.
- Write ADR-0016.

**R3c — Feature owners.**
- Move passes into owners as each one becomes callback-driven:
  - leaf passes first: clusters, output, HDR10 encode, OIT resolve and hooks;
  - then shadows and probes;
  - G-buffer, lighting and forward/transparency last.
- `IRenderBackend` converges on `submitFrame(const RenderFrame&)`.
- The editor UI moves to `IEditorRenderBridge`.

**Completion:**
- `VulkanVertexBackend.cpp` is under 2,500 lines, and no new file exceeds the guideline;
- synchronization validation is clean;
- byte-identical output.

### R4 — Vulkan modernization (`Accepted` 2026-10-03)

The implementation design, covering the inventory, per-pass dynamic-rendering mapping, VMA and aliasing
planner, deletion queue, pipeline cache, hitch harness, transfer queue and sequencing, is in
`docs/milestones/M7R-R4-vulkan-modernization-design.md`.

**R4a — Dynamic rendering.**
- The graph carries clear values per attachment usage.
- Pipelines use `VkPipelineRenderingCreateInfo`.
- Remove the 15 render passes and 12 framebuffer sites. ImGui keeps its own dynamic-rendering mode.
- Audit each pass's load/store ops and initial/final layouts against the old render passes before removing them.

**R4b — VMA behind `VulkanResourceAllocator`.**
- The 19 memory-profile categories are preserved.
- Dedicated allocations for large render targets.
- `VK_EXT_memory_budget`.
- Aliased transient graph memory: the graph plans alias groups from lifetimes plus memory requirements, replacing exact-descriptor slot reuse. First use is always a discard (UNDEFINED layout).
- Report persistent and transient bytes and aliasing efficiency.

**R4c — Deferred deletion and pipeline cache.**
- A fence/timeline-keyed deletion queue replaces the 8 capacity-growth and 4 probe-change `waitForAllFrames` sites.
- Resize and transport changes keep their bounded stall, per ADR-0013.
- Capture collection stays a drain only in qualification.
- A persisted `VkPipelineCache` in the user cache directory, validated by header UUID, driver and vendor, with a safe discard on mismatch.

**R4d — Transfer queue and timeline semaphores.**
- Uploads go on a dedicated transfer queue with queue-family ownership transfers.
- Graphics waits on a timeline value instead of `vkWaitForFences` in `beginFrame`.
- Fallback to the graphics queue.
- Async compute is recorded as a later candidate.

**Completion:**
- hitch and p99 improvement on the scripted change run;
- VRAM deltas;
- synchronization validation clean.

### R5 — CPU frame and application decomposition (`Proposed`)

**R5a — Application decomposition.**
- Split `Application` into FrameOrchestrator, RenderExtractor, EditorHost and AssetIntegration. Qualification is already gone after R2.
- This is code motion; the editor's writes into runtime config move behind `EditorFrameRequests`.

**R5b — Task system.**
- Library evaluation: enkiTS against Taskflow.
- Write ADR-0015.
- Priorities: frame-critical, normal and background.
- Migrate the 11 service threads and the importer fork-join.
- Background cooking cannot starve the frame; this is measured under a cook-while-render run.

**R5c — Extraction.**
- Change-driven extraction using the transform dirty list, publisher revisions and light/probe revisions. The full-scan `memcmp` revision is removed.
- Parallel transform update and extraction.
- Compact `(key, index)` sorts with exact tie-breaks, so transparent order is identical.
- Retire the M7.2 parity packet path for ordinary opaque work:
  - a main-opaque consumer index list in `GpuScenePackedTables`;
  - an `OpaqueSubmission` like `ShadowCasterSubmission`;
  - mixed GPU-scene and direct bins;
  - an epoch-based depth-history revision;
  - selection driven by the instance flag;
  - an indirect wireframe variant.
- Restore zero steady-frame allocations.
- Close the 1.69 ms unattributed scope gap.

**Completion:** serial main-thread time is no more than 3.0 ms on F7, with
critical-path and aggregate worker time reported per stage. `FRAME_BUDGET.md` is
updated.

### R6 — Qualification and handoff (`Proposed`)

**Verification:**
- full Debug and Release suites;
- validation and synchronization validation on the frozen set;
- frozen-set byte identity;
- the timing pair;
- hitch and p99 evidence for the removed drains;
- build-time results.

**Documents:**
- the completion report;
- `docs/milestones/M7R-to-M9-handoff.md`: exactly where motion vectors (G-buffer velocity target and previous transforms), jitter (`updateCamera`/`ViewHistoryContext`, `ViewTransportRecord`), history resources (graph `History`), TAA (after the transparency resolve, before bloom), bloom (the `bloom-hook` input to output-transform) and exposure (a histogram pass before output-transform plus history luminance) attach;
- updates to `ROADMAP.md` (M7R `Accepted`), `FRAME_BUDGET.md` and `PROJECT_CONTEXT.md` "Current direction".

## Delegation and integration

- The lead owns integration, interface decisions and acceptance.
- Subagents do read-only audits, test ports, CMake mechanics (R1) and isolated
  kernels such as the R3a culler core and the R2 CLI registry.
- Each parallel writer gets its own git worktree.
- No two writers touch backend/RHI headers, common shaders or CMake at the same time.
- All delegated work is reviewed against this plan and re-verified with the R0
  tooling before it is accepted.

## Verification

- **Builds and tests:** `x64-debug` and `x64-release` presets, and ctest by label.
- **Validation:** the Khronos validation layer on F1–F7. Synchronization validation is added from R3.
- **Hardware:** RTX 4090, i9-14900K, current driver (recorded in the capture sidecars).
- **Timing setup:** native 3840x2160, SDR transport, ACES 2, mailbox present mode.
- **Image checks:** byte identity uses the SHA-256 in each capture sidecar.
- **Timing checks:** the pair compares median, p95 and p99 CPU (non-wait) and GPU frame time, with noise bands taken from the A/B against B/A spread.

## Risks, fallback, and rollback

- **R3 and R4 barrier and layout changes may alter output** through load/store, clear
  or layout differences. Mitigation: per-pass migration, byte comparison after every
  pass, and synchronization validation. Rollback per pass is the imperative path,
  until R3c ends.
- **Aliasing exposes reads of uninitialized memory** that exact-slot reuse hid.
  Validation plus captures catch them, and an aliasing-off switch is kept until R6.
- **Pipeline-cache corruption.** Validate the header and discard on mismatch.
- **Driver transfer-queue quirks.** A runtime switch falls back to the graphics queue.
- **Parallel extraction breaks determinism.** Stable per-index outputs and an exact
  sort tie-break; frozen captures detect any regression.
- **Local-only fixtures:** the frozen hashes cannot be reproduced on another machine.
  F4 and F5 are tracked and remain portable evidence.

## Decision log

| Date | Decision | Evidence |
|---|---|---|
| 2026-10-02 | Owner approved this plan as written. | Owner, in the lead session. |
| 2026-10-02 | Corrected audit figures recorded above; the plan uses them. | Four audit reports against `23d9ced`. |
| 2026-10-02 | Allocation invariant: steady frames are already allocation-free on both timing routes at R0, so the invariant is "zero on the timing routes". The audit's nonzero counts came from older or dirty-tree runs. | R0 timing pair. |
| 2026-10-02 | Captures at HEAD are not fully deterministic, and the frozen set is adjusted to match (details below). | Repeated R0 captures; `tools/m7r/Diff-Images.py`. |
| 2026-10-02 | Historical M7 hashes are not comparable with R0 and stay as historical record. They used different cooked artifacts, resolutions or EV and older code; for example, `ordinary2` final-SDR was `25d690aa…` historically and is `5620cf98…` at R0. | R0 captures. |
| 2026-10-02 | R1 layering, step 1 (`7781b03`). Moves to `src/core/types`: identity, handle and texture/capture value types, plus the reflection-probe authoring enums. `Application` moves to `src/app`, `TransformSystem` to `scene/systems`, `ReflectionProbeCapture` to `renderer/rhi`, and the factory implementation to the Vulkan backend. Rationale: these were the only edges creating module cycles. rhi↔transparency remain one library until R2 removes the capture-validation hook that couples them. | Include-graph scan; Release/Debug 78/78. |
| 2026-10-02 | The M5 fixture-contract test no longer pins hashes of engine code (C++, CMake, shader source, SPIR-V); fixture and evidence content stays pinned. Rationale: code hashes break under any refactor, and provenance is preserved in the historical manifests. | Test failure caused by R1 moves. |
| 2026-10-02 | SPIR-V output stays in `assets/shaders/` for R1, because the runtime loads that relative path. Moving it into the build tree is deferred to R4a or later, together with any runtime path change. | R1 scope. |
| 2026-10-02 | `AssetRuntimeServiceTests` is flaky under CPU load: `deferredByBudget == 1` at line 237 failed in 1 of 6 runs while parallel agents were building. It is pre-existing and budget-timing dependent, not caused by M7R. Fix candidate: R5, when asset services move to the task system and frame budgets become explicit. | ctest reruns. |
| 2026-10-02 | The VSM depth qualification oracle reports a request/coverage mismatch on `m7_heterogeneous_shadow_warm_motion_v1` at R0 and at R2.3. It is pre-existing (M7.8 checkpoint state); VSM stays default-off and its own fixture is exercised by the R2.0 sweep. | `captures/r0-vsm-oracle`. |
| 2026-10-02 | R2.4 CLI registry (`bac5b8c`): `CliOptionRegistry` in core, with per-owner registration in `src/app/cli` (runtime 13, editor 4, renderer 29, qualification 36). The parser matches the frozen 6b000ad parser on 15,821 argument vectors, including configs and exact messages. Validators carry an explicit order, because the original post-parse checks interleave owners and must keep their error precedence. The `final-sdr` capture check is owned by qualification. The frozen-parser test copy is deleted at R2.10. | `ApplicationCliParityTests`. |
| 2026-10-02 | The R2.0 sweep (`aad3048`) found that F6 does not instantiate its reflection probe: the manifest probe needs `--validate-reflection-probes` and an environment artifact. `F6-probecap` was added (F6 + `--validate-reflection-probes` + cooked Belfast HDRI, local-only). Its R0 hashes were captured with the preserved R0 worktree build and merged into `captures/r0`; R0 and R2 are byte-identical and repeats are deterministic. The VSM depth oracle passes on its own fixture (`m7_virtual_shadow_complex_v1`, sweep O05); the F5 mismatch logged earlier comes from using it on an unsupported fixture. | Sweep `r2-baseline`, `captures/r0-probecap`. |
| 2026-10-02 | Sweep finding, pre-existing and not fixed in M7R: `--reference-direct-gbuffer` and `--reference-direct-probe-capture` differ from the automatic route on the probe fixture (final-SDR up to 1 code on 0.27% of pixels; scene-linear also differs). The M7.7 record called them identical. They use a recorded envelope in the sweep. Candidate follow-up for M7.12 qualification. | Sweep R01/R03. |
| 2026-10-02 | Sweep `r2-head-7b89df9` passes 36/36 and is the comparison baseline for the R2.6–R2.9 lanes. Every difference from `r2-baseline` (6b71bd2) is expected: R2.5 records oracles as off in `quality_settings` and drops oracle counters for validator runs. The changed `manifest_sha256` values are an environment artifact; the R2.0 worktree checked out manifests with CRLF. | Compare output. |
| 2026-10-02 | R2.1 (`cefee7d`): about 445 source-text assertions are replaced by behavioral tests: ShaderAbiContract, ShaderOwnership (depfile and linked-function), ProductionGraphContract, VulkanPipelineContract (headless) and VulkanShaderParity (GPU against CPU for scene color, direct lighting, BSDF and the four compact kernels). Assertions with no behavioral equivalent were deleted with reasons; see `docs/milestones/M7R-R2-assertion-disposition.md`. No test reads `src/` or GLSL text any more. Testability gaps to address in R3: lighting layout, compact pipelines and output/HDR-encode passes need a windowed `VkContext`. | 89/89 Release and Debug. |
| 2026-10-02 | R2.6 lane A (`d52fb52`, `05b59ee`): the `IFrameObserver` phases (`StartupPhase`, `FrameBeginPhase`, `FrameSubmitPoint`, `ShutdownPhase`), `AppRunPolicy`, `AppFrameRequests` and `IAppControl` are in `src/app/FrameObserver.h`. The `QualificationHarness` and its benchmark scene, validators, reports and exports are in `src/qualification/harness`. `Application.cpp` goes from 6,687 to 4,552 lines. Deviation from the design: captures arm at `FrameSubmitPoint::{SceneLinearReady, OutputReady}` instead of `onBeforeSubmit`, because `captureCurrentFrame` records commands when called. Verification: frozen set `r2-6` identical or within envelopes; sweep 0/36 changed against `r2-head-7b89df9`; 91/91 Release and Debug. | Lane A report; main-checkout reruns. |
| 2026-10-02 | R2.7/R2.8 lane B (`f35b2ac`):
- **Moves:** `VulkanQualificationExtension`, `VulkanIndirectOracle`, readback analyzers and installation go to `src/qualification/vulkan`. Interfaces are in `renderer/vulkan/VulkanBackendExtension.h`; the RHI gains a neutral `IRenderBackendExtension` and `RenderBackendCreateInfo`.
- **Graph:** hook passes are declared through `VulkanGraphHooks` (defaults equal the old graph). `final-capture-hook` stays unconditional because retained editor views use it.
- **Interim wiring:** a legacy factory overload plus an explicit `installQualificationBackendExtensions()` call keep behavior until R2.9. Renderer code never includes qualification code.
- **Size:** `VulkanVertexBackend.cpp` goes from 13,122 to 11,635 lines. The four shadow/probe oracle collectors become one table-driven collector.
- **Verification:** frozen `r2-8` identical or within envelopes. The sweep against `r2-head-7b89df9` is identical apart from fewer allocations: the occlusion oracle's 2 calls / 13,006 B per frame (the audit's figure) and 46 shadow-oracle calls per frame are gone. 92/92 Release and Debug. | Lane B report; main-checkout reruns. |
| 2026-10-02 | R3.0: `synchronization2` is enabled on the device when supported, and `hasSynchronization2()` is added. A runtime flag `--validation-sync` chains `VkValidationFeaturesEXT` (synchronization validation) at instance creation, and the capture script's `-SyncValidation` counts hazards separately. The frozen set with sync2 enabled is byte-identical to R0 or within envelopes. Sync-hazard baseline (`captures/r3-0-sync`): **pre-existing READ_AFTER_WRITE hazards on F5-hetero and F5-point**, 10 per run before the layer's duplicate limit. `vkCmdBeginRenderPass` loads a depth attachment (LOAD_OP_LOAD) whose layout transition is not synchronized with the load; this points to the local-shadow atlas/pool render passes' external subpass dependencies. All other fixtures are hazard-free. R3b.6 (imported-image policies) fixes it, and R3 requires zero hazards at completion. | Sync-validation run. |
| 2026-10-02 | R3b lane B (`c401097`…`d1c53db`). The executor is index-addressed through `PassId` and `GraphResourceId`, with O(1) name maps; the string forms forward and keep the cursor check. Barriers: one `vkCmdPipelineBarrier2` per pass with the equivalence-first sync1→sync2 mapping and a sync1 fallback; same-access rules are kept verbatim. The equivalence test covers 11 production topologies × 4 scripted frames: 3,173 barriers, identical per pass, now issued in 801 calls instead of 3,173. Also added: a callback registry with drain semantics and GPU-range placement/groups; real History pairs with parity, validity keys and retirement; `bindExternalImage` with ExecutorOwned/RenderPassManaged/OwnerManaged policies; `variableSize` imported buffers. These have no production call sites yet; that is R3b.4+. Steady frames make no allocations. Rebuild frames make 47–82 more C++ allocations per rebuild (name-map nodes), acceptable at a rebuild boundary. The executor detects sync2 from the allocator's physical device; R3b.4 sets it explicitly from `VkContext::hasSynchronization2()`. Verification: frozen `r3b` identical or within envelopes; sync hazards unchanged from R3.0 (F5 only); sweep differs only in resize-entry rebuild allocations; 94/94 Release and Debug. | Lane B report. |
| 2026-10-02 | R3a lane A (`3429947`…`e5d2fde`). `VulkanIndirectViewCuller` serves the directional, spot, point and probe kinds through a per-kind config table: push words, consumer, membership cache, capacity and maximum work, compaction placement, LOD selector. `VulkanOpaqueIndirectCuller` is a sibling on the shared core in `VulkanIndirectCullerShared`, with the command and resource seams, the policy ladder, the LOD prefix and the buffer set. Expectation emission and collect are single sites. `VulkanVertexBackend.cpp` goes from 11,555 to 8,309 lines. The new qualification-only `--qualification-indirect-stream-digest` (`tools/m7r/Run-IndirectDigest.ps1`) hashes dispatch logs, host bytes and device commands. Device commands are keyed by instance transform, because GPU-scene slot assignment follows per-process UUIDs. Every step reproduced the `r3a0` digest exactly. Combined with lane B in the main checkout: frozen `r3ab` identical or within envelopes; digest identical on 7 fixtures; sweep identical to lane B's; sync hazards still only the F5 baseline; 95/95 Release and Debug. | Lane A report; main-checkout reruns. |
| 2026-10-02 | R3b serial steps R3b.4-R3b.9 (`b000872`...`ae20119`). **R3b.4:** `VulkanProductionGraphIds` is resolved once per rebuild; the 82 string call sites (backend 53, clustered lighting 7, frame targets 22) address passes and resources by id, and the executor's string forms and `transitionImage` are deleted (names remain for `findPass`/`findResource`). The backend sets the barrier API from `VkContext::hasSynchronization2()`, the GPU-range sink and the per-frame record context. **R3b.5:** scene-linear captures run in a declared `scene-color-capture-hook` pass (Transfer, `VulkanGraphHooks::sceneColorCapture`), and output-transform returns scene.color to SampledRead. **R3b.6:** the swapchain is bound per frame after acquire, render-pass managed (Undefined to Present); the shadow maps are bound globally, render-pass managed (SampledRead to SampledRead); the point-pool declarations follow the configured capacities. *Hazard fix:* the spot-atlas and point-pool render passes load depth with LOAD_OP_LOAD, but their EXTERNAL-to-0 dependency granted only DEPTH_STENCIL_ATTACHMENT_WRITE, so the load raced the layout transition. Adding DEPTH_STENCIL_ATTACHMENT_READ fixes the R3.0 F5 baseline; images are unchanged. **R3b.7:** `shadow.{directional,spot,point}.compact`, `gpu-scene.opaque.compact` and `lighting.probe-cluster` are declared. Ten per-slot imported variableSize buffers are rebound after every rebuild and capacity growth (`unbindExternalBuffer` first). Five manual barriers become executor buffer barriers: four compute-to-indirect and one compute-to-fragment. The opaque culler keeps a compute-to-host barrier, and the digest records the same logical dependency. A golden test (`tests/renderer/fixtures/ProductionGraphR3b6Golden.inc`) shows the old order is preserved as a subsequence and the physical slots are unchanged. **R3b.8:** `probe.capture` is declared after `shadow.point` and reads the shadow maps; staging and per-face compaction stay owner-managed. **R3b.9:** the depth-pyramid history is an ExecutorOwned global import that follows the retained view. Its begin and ready barriers move to the executor (the ready barrier now sits at the next reader), and its mip barriers stay in the pass. Verification at every step: the frozen set is identical to `r0` or within envelopes (`r3b4`-`r3b9`), and the indirect digest is identical to `r3a0`. Final: synchronization validation shows **0 hazards on all fixtures** (`r3b-final-sync`); Release and Debug pass 95/95 (three cook tests use short work directories); the sweep `r3b-sweep-final` passes 36/36 against `r3ab-sweep`, differing only in topology counters, resize/lifecycle rebuild-frame allocations, and the display having no HDR surface today (main's build shows the same); steady-frame allocations are 0 on T-F1/T-F7. `VulkanVertexBackend.cpp` goes from 8,309 to 8,444 lines. | R3b serial report. |
| 2026-10-02 | R3c leaf owners R3c.0-R3c.4 (`3ad2048`...`5955f8c`). **R3c.0:** `VulkanFeatureContext` (shared device services, registry, GPU-scene state, profiler, frame-counter sink, extension hooks, open-frame flag) and the `IVulkanFeature` lifecycle (create, onGraphRebuilt, registerPasses, onGraphReleased, onFrameSlotRetired, destroy). Code motion out of the backend: `VulkanFrameTelemetry`, `VulkanResourceRegistry`, `VulkanGpuSceneState`, `VulkanExtensionHooks`; `IRenderBackend` is unchanged and its resource methods forward. **Drain points:** the executor gains `drainRegisteredThrough(PassId)`. Owners stage per-frame inputs in the existing submit* call and drain their registered passes where the imperative code ran, inside the same CPU scopes (pattern in `VulkanFeatureContext.h`). An implicit drain at the next imperative `beginPass` would have moved work across the transparent pipeline-statistics query, other passes' GPU-range timestamps and CPU scopes. R3c.11 (`submitFrame`) can drop the drain points. **R3c.1:** `VulkanClusterLightingFeature` (cluster build and probe-cluster pipelines, light records, parameters, fallback candidates, diagnostics readback, shadow-slot mapping) registers `lighting.probe-cluster` (range before its barriers), `lighting.cluster.{clear,count,scan,fill,finalize}` and, when declared, `lighting.cluster.readback`, with a `gpu.lighting.cluster` range group; the cluster build is split into one record function per pass. **R3c.2:** `VulkanOutputFeature` (output transform, HDR10 encode, LUT, exposure/operator, grid overlay) registers `bloom-hook` (always inactive), `output-transform` and `hdr10-encode-present`. New `GpuRangePlacement::AroundBarriers` keeps `gpu.output.graph_transition` a barrier-only range. **R3c.3:** `VulkanWeightedOitFeature` (passes, pipelines, instance streams and capacity, order seed) registers `transparent.oit.{accumulate,resolve}`, drained inside the pipeline-statistics bracket. **R3c.4:** `VulkanHookPasses` registers the four validation readback hooks (runPassHook semantics), `scene-color-capture-hook` and `final-capture-hook`; the retained editor views consume the final hook through `VulkanFinalCaptureConsumer` (image initialization keeps its old position, before the pass or after the capture copy). Up to 18 passes are callback-registered. **Verification at every step:** Release build and tests, frozen set `r3c0`-`r3c4` identical to `r0` or within envelopes, indirect digest identical to `r3a0`, X06 profile GPU ranges, CPU scopes and counter values identical to `r3b-sweep-final`. Final: Debug and Release 95/95 (three cook tests with short work directories); `r3c-final-sync` 0 validation messages and **0 sync hazards**; sweep `r3c-sweep-final` passes 36/36 with **0 changed entries** against `r3b-sweep-final`; T-F1/T-F7 profiled runs (240 frames) have 0 steady-frame allocations and the same GPU ranges, CPU scopes and counters as the base build. HDR10 encode is not exercised locally (no HDR surface). `VulkanVertexBackend.cpp` goes from 8,444 to 6,418 lines. | R3c leaf report |
| 2026-10-03 | R3c shadow and probe owners R3c.5-R3c.6 (`f1e2f89`, `8a17698`). **R3c.5:** `VulkanShadowFeature` (directional cascade map, its view culler, the cascade draw loop with the direct fallback, and the default-off M7.8 VSM resources: clip publication, depth-demand marking, request readback and its oracle verdict) registers `shadow.virtual.clip-upload`, `shadow.directional.compact`, `shadow.directional` (`gpu.shadow.directional` after its barriers), `shadow.virtual.depth-mark` (`gpu.shadow.virtual.depth-demand` before its barriers) and `shadow.virtual.request-readback`. `VulkanLocalShadowFeature` (spot atlas, point pools, both cullers and draw loops; publishes the lights' shadow-data slots to the clustered-light owner) registers `shadow.{spot,point}.compact`, `shadow.spot` and `shadow.point` (drawing ranges after their barriers). `VulkanShadowCasters` holds what the shadow owners and the probe capture share: caster resolution and revision, one `VulkanCasterScratch` (so allocation behaviour is unchanged), the direct-fallback loop, oracle draw counters and LOD selectors. `VulkanFeatureContext` gains the upload context. The submit calls stage, plan the culler and resolve casters inside the old CPU scopes, then drain through the drawing pass; the VSM demand drains after `forward.opaque`. Capacity growth (opaque, directional, spot, point, probe, then the imported-buffer rebind) and slot collection (VSM requests before the features, view cullers after the opaque culler) keep their order through explicit calls instead of `onFrameSlotRetired`. **R3c.6:** `VulkanReflectionProbeFeature` (capture pass and targets, probe culler, probe record/active/parameter/cluster buffers and uploads, asset and captured environment tables, pending prefilter publications, owner synchronization, capture telemetry) registers `probe.capture`. `gpu.probe.capture` is not an executor range: it still opens inside the callback after the culler's host-to-compute barrier. Asset-preview frames drain the pass as skipped. Buffer replacement and environment-table changes call back into the backend (`Bindings`) at the original points, so the drains and rebinding order are unchanged. The `IRenderBackend` probe methods forward. Up to 28 passes are callback-registered. **Verification at every step:** Release build and tests, frozen set `r3c5`/`r3c6` identical to `r0` or within envelopes, indirect digest identical to `r3a0` (and `r3a0-ext` at R3c.6), F5-hetero (with and without VSM), F5-point and F6-probecap profile GPU ranges, CPU scopes and counter values identical to the base build. Final: Debug and Release 95/95 (three cook tests with short work directories; `AssetRuntimeServiceTests` flaked once under `-j 8` load, see above); `r3c6-final-sync` 0 validation messages and **0 sync hazards**; sweep `r3c6-sweep` passes 36/36 with **0 changed entries** against `r3c-sweep-final` (X02 WeightedOIT order seed within its woit-order envelope); T-F1/T-F7 profiled runs (240 frames) have 0 steady-frame allocations and the same GPU ranges, CPU scopes and counters as the base build. *Environment:* the worktree checked out `eol=lf` fixtures with CRLF, which broke the sha-pinned BenchmarkManifest, M5FixtureContract and AssetCooker tests until the files were rewritten as LF (no content change). `VulkanVertexBackend.cpp` goes from 6,418 to 5,340 (R3c.5) and 4,559 (R3c.6) lines. | R3c shadow/probe report |
| 2026-10-03 | R3c opaque, lighting, forward and layered owners R3c.7-R3c.9 (`76f815f`, `b5382bf`, `1d9de2c`). **R3c.7:** `VulkanOpaqueFeature` (G-buffer render pass and fixed wireframe/selection pipelines, `VulkanOpaqueIndirectCuller`, the indirect-bin, direct-fallback, wireframe and selection-mask loops, and the depth pyramid with its history decision, executor-owned history import and build) registers `gpu-scene.opaque.compact`, `gbuffer` (`gpu.gbuffer.opaque`/`selection` still open inside the render pass) and `depth.occlusion-pyramid.build` (`gpu.depth.occlusion-pyramid` before its barriers). `submitOpaqueQueue` stages and drains through `gbuffer` inside `cpu.render.record.gbuffer`; the pyramid build drains where it was recorded, before its validation hook. **R3c.8:** `VulkanDeferredLightingFeature` (lighting render pass and pipeline, the lighting set, and the environment products: active image-based lighting, its settings, the neutral cube/BRDF-LUT fallback) registers `lighting`. The lighting set's shadow and probe bindings move here from the backend; the probe-clustering sets are rebound with the probe buffers in the old order. *Decision:* the lighting set belongs to the lighting owner, and its other consumers (probe capture, forward, layered, WeightedOIT) read it through `sceneSet(frame)`. The camera uniform buffer and set 0 become `VulkanViewUniforms`, a backend-owned service that `updateCamera` writes; the backend stages `globalSet(frame)` into each owner's inputs, because every owner binds it. The resize, transport and topology paths share `releaseFrameTargets`/`createFrameTargets` and the editor target-texture helpers, in the same order. **R3c.9:** `VulkanForwardFeature` (forward and transparent render passes, `forward-opaque`, `transparent.sorted.forward`, `transparent.compatibility.forward`, and the refraction pyramids with their residency) and `VulkanLayeredTransparencyFeature` (Ordinary2 and Hero4/Cinematic8 atlas residency and extents, request collection, atlas and capture-draw plans, resolved-packet indices, and the capture, tile-termination, local-composition and scene-resolve passes) register the remaining passes. The WeightedOIT owner gains its residency and the per-frame packet/instance-capacity decision. `submitForwardQueues` is staging plus explicit drains at the original points, inside the transparent pipeline-statistics bracket. *Equivalence notes:* resident-input checks still throw before their pass begins; the deep scene-resolve range name is chosen at registration from the resident tier set (it only changes with a graph rebuild); the deep resolve-count notification still follows the pass's range. Only `ui` remains imperative; up to 59 passes are callback-registered. **Verification at every step:** Release build and tests; frozen set `r3c7`/`r3c8`/`r3c9` identical to `r0` or within envelopes; indirect digest identical to `r3a0` and `r3a0-ext`; profile GPU-range sequences, CPU scope trees, counters (names, order, values) and memory identical to the base build (`e22431d`) on F1-all, F4-ord2, F4-cine8, F4-woit, F5-hetero, F5-point, F6-probecap and F7-hiz, and with `--profile-transparent-overdraw` on F4-ord2/F4-cine8. **Final:** Debug and Release 95/95 (three cook process tests with short work directories); `r3c9-final-sync` 0 validation messages and **0 sync hazards**; sweep `r3c9-sweep` passes 36/36 with **0 changed entries** against `r3c6-sweep` (X02 WeightedOIT order seed within its woit-order envelope); T-F1/T-F7 profiled runs (240 frames, `timing/r3c9-short`) have 0 steady-frame allocations and the same GPU ranges, CPU scopes, counters and memory as the base build. *Environment:* the worktree also checked out `tests/assets` fixtures with CRLF (BenchmarkManifest, AssetCooker sha checks) until rewritten as LF. `VulkanVertexBackend.cpp` goes from 4,559 to 4,093 (R3c.7), 3,638 (R3c.8) and 2,250 (R3c.9) lines. | R3c opaque/lighting/forward report |
| 2026-10-03 | R3c editor bridge and frame submission R3c.10-R3c.11 (`94db69d`, `6117760`). **R3c.10:** the editor's renderer services leave `IRenderBackend` for the backend-neutral `IEditorRenderBridge` (`renderer/rhi/EditorRenderBridge.h`: `beginUI`, scene/glass-depth/editor texture ids, `prepareRetainedViews`, retained-view ids). The new `iridium_vulkan_imgui` library (`renderer/vulkan_imgui`, links `iridium_vulkan`, imgui, glfw) implements it as a Vulkan backend extension serving `IVulkanEditorUi` (`VulkanEditorUi.h`): it owns the ImGui context and GLFW/Vulkan backends, their descriptor pool, the color-managed fragment shader, the viewport target and editor-texture registrations, the retained views (the `final-capture-hook` consumer) and the UI draw telemetry. The backend raises its events at the original points (device ready, frame targets released/created, presentation and image-count changes, display colour, shutdown after the extensions' `onBeforeDeviceDestroy`); the registry keeps a neutral release hook for retired textures' editor descriptors. The depth-pyramid history still follows the rendered retained view through a callback the bridge calls. `VulkanUiFeature` owns the UI render pass and registers `ui-present`/`ui-compose` as a callback (`gpu.ui` after the barriers); it clears and presents with or without a bridge. `iridium_vulkan` no longer links imgui (configure-time link guard for `iridium_rhi` and `iridium_vulkan` in `tests/CMakeLists.txt`). `Application` creates the bridge and attaches it after the qualification extensions (benchmark and capture runs keep it, so the UI pass's draws and counters are unchanged); the editor and the `AssetManager` previews take ids from it. **R3c.11:** `RenderFrame` (`renderer/rhi/RenderFrame.h`, non-owning spans valid until `submitFrame` returns: view transport record and history, debug view, output settings, grid overlay, the three shadow {casters, packets} pairs, probe casters and capture schedule, opaque/selection/forward-opaque/sorted/compatibility queues, wireframe flag, instance transforms, lighting and probe frame packets) and `IRenderBackend::submitFrame` replace the 14 fixed stage methods; the lighting stage takes the camera position, matrices and planes from the view record (bit-identical to its former arguments). `submitFrame` calls the former stages privately in the same order, so every drain point, CPU scope, pipeline-statistics bracket and GPU range is unchanged. *Decision:* an optional `IRenderFrameStageObserver` reports the stage boundaries (shadows ×3, probe captures, lighting, scene-linear, output); `Application` records its schedule counters, cache bookkeeping and the observer's `SceneLinearReady`/`OutputReady` points there, which keeps the profile's counter order identical (the backend records counters inside the probe and lighting stages). `frameTelemetry()` replaces four telemetry getters. The schedules' stats are now read before the schedules are completed (they were read after completion had retired them). *Not folded:* `prepareGpuScene`/`prepareLighting`/`prepareReflectionProbes` run under different CPU scopes at different points of the frame, so `prepareCapacities` would change the scope tree; `publishGpuScene` stays after `beginFrame` because caster revisions read the published mirror during extraction. `IRenderBackend` goes from 59 to 53 (R3c.10) and 37 methods. **Verification at every step:** Release build and tests (97/97; three cook process tests with short work directories, local Alfa files hard-linked); frozen set `r3c10`/`r3c11` identical to `r0` or within envelopes; indirect digest identical to `r3a0` and `r3a0-ext`; profile GPU-range sequences, CPU scope trees, counters (names, order, values) and memory identical to the base build on F1-all, F4-ord2, F4-cine8, F4-woit, F5-hetero, F5-point, F6-probecap and F7-hiz. New tests: `EditorRenderBridgeTests` (device-free) and `VulkanBackendFrameTests` (device, hidden window, synchronization validation: frames without a bridge, with the bridge including editor textures, dual retained views, a scene resize, a transport recreate and live output settings, and the stage order). **Final:** Debug and Release 97/97 (three cook process tests with short work directories); the shipping preset builds and passes 91/91 (`CookSceneCliTests` with a short work directory), its hidden 120-frame `--validation` run exits 0 with 0 messages, `--benchmark` is rejected and no qualification strings are present (5.7 MB); `r3c11-final-sync` 0 validation messages and **0 sync hazards** on all 24 captures; sweep `r3c11-sweep` passes 36/36 with **0 changed entries** against `r3c9-sweep` (X02 WeightedOIT order seed within its woit-order envelope); T-F1/T-F7 profiled runs (240 frames, `timing/r3c11-short`) have 0 steady-frame allocations and the same GPU ranges, CPU scopes, counters and memory as the base build; the editor path (`--hidden-window --frame-limit 120 --validation`) and an asset-viewer run (`--open-asset-viewer` on the tracked M5 shadow-contact model, 600 frames, cooked in the run) report 0 validation messages. *Sizes:* `VulkanVertexBackend.cpp` goes from 2,250 to 2,010 (R3c.10) and 2,073 (R3c.11) lines; `Application.cpp` is 4,547 lines (R5a splits it); `AssetBrowserPanel.cpp` (3,273) is the other M7R-touched file over 2,500 lines (deferred exception). | R3c bridge/frame report |
| 2026-10-03 | R4 design accepted. It records five issues that shape R4:
1. Same-access attachment writes (for example `scene.color` across lighting, forward and transparency) are ordered today only by render-pass external dependencies. Dynamic rendering therefore needs an attachment re-barrier rule.
2. Swapchain acquire chaining and the frame-end Present export need executor support.
3. **Latent use-after-free:** `VulkanFrameScheduler::defer` queues on the already-advanced slot. It is fixed by R4c's deletion queue.
4. The OIT accumulation pass stores to read-only depth; it must be STORE_OP_NONE.
5. The editor's depth sample is undeclared, and must be declared before aliasing.

Hitch baseline decision: the R0 worktree predates `IFrameObserver`, so the scripted change runner cannot run there. The R4c hitch baseline is measured at the last pre-R4c commit with the harness; drain sites are unchanged since R0, and the drain counters confirm it. | R4 design pass. |
| 2026-10-03 | R4b.1-R4b.2 VMA behind `VulkanResourceAllocator` (`075083e` and the R4b.2 commit that records this entry). **R4b.1:** VMA **v3.4.0** by FetchContent (the latest stable release; the design expected v3.3.0), compiled once in `VulkanMemoryAllocatorImpl.cpp`; `VulkanVma.h` is the single include point, so every TU sees the same configuration macros. **R4b.2:** the allocator owns a `VmaAllocator` (instance-aware `init`, API version min(device, 1.3), `EXT_MEMORY_BUDGET` when enabled). Buffers and images are still created by the allocator, then `vmaAllocateMemoryFor{Buffer,Image}` with `requiredFlags` unchanged and `memoryTypeBits` restricted to exactly the type the old `findMemoryType` picks (`legacyMemoryTypeIndex`), so memory types are identical. Images whose requirement is 16 MiB or more get `DEDICATED_MEMORY_BIT`; everything else is suballocated unless the driver prefers dedicated. Persistent mappings use `MAPPED_BIT`: the pointer is the buffer's first byte, as before. The resource structs gain `vmaAllocation` and `memoryOffset`; `memory` is the bound block (`VmaAllocationInfo::deviceMemory`), so `isValid` and the fakes are unchanged. Category accounting stays engine-side: requested bytes as before, committed = the VMA allocation size, and each allocation is named after its category. `memorySnapshot` reads `vmaGetHeapBudgets`; `beginFrame` calls `vmaSetCurrentFrameIndex` (which refreshes the budget; 0.9 µs per call) from the backend's `beginFrame` after the fence wait. *Measured:* all 19 categories have identical requested and committed bytes, counts and memory-type masks on F1-all and F7-hiz (committed equals the old per-resource requirement); driver heap usage grows by the VMA block slack (+3 MB device-local, +26 MB host heap on F1), and VMA clamps the host heap's reported budget to the heap size. GPU frame median falls on every A/B pair (F1 −4.1%/−4.8%, F7-hiz −4.1%, F7-stack −2.8% short and ±0 long), so the R3 F7 watch item (+0.57%) is not a regression after R4b; CPU deltas were within the noise of a loaded machine (other agents were running). **Verification:** Release and Debug 98/98 and shipping 92/92 (three cook process tests with short work directories, local Alfa files hard-linked); new `VulkanResourceAllocatorTests` (legacy type rule, dedicated threshold, suballocation and dedicated images, persistent mappings, category accounting, budget snapshot, lifecycle) under validation; frozen `r4b12` and `r4b12-sync` identical to `r0` or within envelopes, 0 validation messages, **0 sync hazards**; indirect digest identical to `r3a0` and `r3a0-ext`; sweep `r4b12-sweep` 36/36 pass: against a same-depth base build (`r4b12-sweep-base`, itself identical to `r3-sweep` apart from the worktree-relative manifest path) only C++ allocation *bytes* on startup and rebuild frames change, by 16 B per resource struct element (the two new fields), with identical call counts; T-F1/T-F7 and F7-hiz profiled runs have 0 steady-frame allocations. R4b.3+ (aliasing) unchanged. | R4b.1-2 report; `timing/r4b12-short`, `r4b12-long`, `r4b12-f7hiz`. |
| 2026-10-03 | R4a.0 dynamic-rendering groundwork (no pass migrated; render passes and framebuffers stay). **Graph:** `StoreOp::None`; bit-exact `ClearValue {colorBits[4], depth, stencil}` (`color`/`colorUint`/`depthStencil`, bit equality); `write(pass, res, access, load, store, ClearValue)` stores the value in `UsageRecord`/`CompiledUsage` only for `LoadOp::Clear` (otherwise the default) and hashes it only then; a `DepthAttachmentRead` usage compiles to Load + None; a write storing None counts as discarded. The production graph declares the inventory's clears on every clearing usage (gbuffer (0,0,0,1)/(0,0,0,1)/(0,0,0,0)/(0,0,0,1)/uint 0/depth 1, lighting, output, UI and HDR10 swapchain (0,0,0,1), layered captures uint 0 + depth 1, local compositions (0,0,0,0), OIT (0,0,0,0)/(1,0,0,0), directional depth 1); spot and point keep Load. **Executor:** a fixed-size `VulkanPassRenderingPlan` per pass at rebuild (colour usages in declaration order, up to 4 depth candidates (the point pass has 3 pools), layouts, ops, clear values, extents); `VulkanPassContext::beginRendering(const VulkanRenderingOverrides&)`/`endRendering()` (render area, layer count, per-attachment views, `depthIndex`, `loadInsteadOfClear`) record `vkCmdBeginRendering`/`vkCmdEndRendering` through the barrier sink, only for passes registered with `VulkanPassCallbacks::dynamicRendering` (default false), never nested or left open. Flagged passes get the same-access `ColorAttachment`/`DepthAttachmentWrite` memory-only re-barrier (attachment write → read/write, layout kept) on graph slots, History and executor-owned imports. `ExternalSyncPolicy::discardOnFirstUse()` (ExecutorOwned): each frame's first use must be a write and transitions from UNDEFINED with the tracked source scope; such bindings may share an image across frame slots. `finishFrameExecution` records the frame-end exports (state changes only; render-pass- and owner-managed imports untouched). `VkContext::hasDynamicRendering()`; the feature is enabled when supported (also in `HeadlessVulkanDevice`). *Finding:* synchronization validation does not report a missing same-access attachment barrier between rendering instances (it treats attachment accesses as raster-ordered; checked by disabling the rule on the device test), so the fake-sink tests are the guard for that rule. **Verification:** Release and Debug 98/98 (three cook process tests with short work directories, local Alfa files hard-linked); frozen `r4a0` identical to `r0` or within envelopes (F3 scene depth-tie, F4-woit woit-order); `r4a0-sync` 0 validation messages, **0 hazards** on all 24 captures; digest identical to `r3a0` and `r3a0-ext`; sweep `r4a0-sweep` 36/36 passing against `r3-sweep`, 33 entries changed only in `render_graph.topology_hash` (clear values hashed; C01–C03 record no profile header), and V05/V06/V09/V16 also in rebuild-frame `allocation.cpp.bytes` (+18–33 KB from the larger `CompiledUsage` copied with each rebuilt plan; counts unchanged); X02 within woit-order. Steady frames that begin rendering allocate nothing. | R4a.0 report. |
| 2026-10-03 | **Pre-existing use-after-free fixed: `LightExtractor::extract`.** `newCandidates_` holds pointers into `candidates_`, and `ensureCapacity()` then reserved `candidates_` at the new record capacity. When light capacity grew (for example 1024→2048), the vector reallocated and every pointer dangled. The R4c.0 hitch harness found it: an access violation (0xC0000005) in 1 of 3 runs at the 1,100-light event, mapped to `LightExtractor.cpp:397-405`. Fix: `ensureCapacity` no longer reserves `candidates_`; `extract()` sizes it before taking pointers. A regression test grows capacity inside one extract; it fails on the old code (Debug heap poisoning) and passes now. Frozen output is unaffected: no frozen fixture grows light capacity mid-run. | R4c.0 report; `LightExtractorTests`. |
| 2026-10-03 | R4a lane X: main-scene passes on dynamic rendering (`0adb78a`, `a2ade87`, `fd8fc89`, `9699e8e`, `97e5018`, `a6559da`; R4a.1 output, R4a.2 lighting, R4a.3 layered captures and local compositions, R4a.4 WeightedOIT, R4a.5 gbuffer, R4a.6 forward-opaque/sorted/compatibility, layered scene resolves and the pipeline library). Every pass registers with `dynamicRendering`, records `beginRendering`/`endRendering` over its graph rendering plan with the old render area, and its pipelines chain `VkPipelineRenderingCreateInfo` (`renderPass = VK_NULL_HANDLE`): output (output format), lighting, local composition and OIT resolve (RGBA16F), gbuffer and the fixed wireframe/selection pipelines (the layout's five formats in attachment order, `vulkanGBufferColorAttachmentFormats`, + D32), layered capture (R32_UINT + D32), OIT accumulation (RGBA16F + R16F + D32), forward/transparent/scene resolve (RGBA16F + D32). `VulkanPipelineTarget` is `{colorFormats, colorAttachmentCount, depthFormat, pipelineLayout}`; `VulkanLayeredSceneResolvePass::init` and `VulkanLayeredTransparencyFeature::configure` no longer take the transparent render pass; the layered feature's framebuffer residency checks are gone (the other residency checks stay). *Dependencies:* the executor's transitions replace every EXTERNAL->0 dependency (each pass's attachments change access from last frame's or the previous pass's readers), and the same-access re-barrier orders the `scene.color` chain lighting -> forward-opaque -> sorted -> compose hooks -> compatibility -> OIT resolve where consecutive active passes write it, and depth forward-opaque -> compatibility when no depth reader is active in between. Read-only depth (sorted, compose hooks, OIT accumulation) is DEPTH_STENCIL_READ_ONLY_OPTIMAL, LOAD, STORE_OP_NONE (design finding 4 closed for OIT). Layered CLEAR on the preserved layout replaces the UNDEFINED initial layout. The compiled barrier count (`render_graph.barrier_count`) is unchanged; the runtime re-barriers are memory-only and merge into each pass's existing dependency call. *Left for R4a.final:* the render-pass objects and every framebuffer field are still created (`VulkanTargetRenderPasses`, `VulkanFrameTargets`, `VkRenderPass*`, `VkForwardRenderPass*`, the lighting/output/layered/OIT render passes, the forward/transparent accessors and `VkForwardRenderPass::depthStoreOperation`'s Stage3 check) but none of these passes uses them. **Verification at every step:** Release build and tests (three cook process tests with short work directories, local Alfa files hard-linked); frozen set `r4ax-1`...`r4ax-6` with synchronization validation identical to `r0` or within envelopes (F3/F7-lod depth-tie, F4-woit woit-order), **0 validation messages, 0 hazards**; indirect digest identical to `r3a0`; F1-all, F4-ord2, F4-cine8 and F4-woit profile GPU-range sequences, CPU scopes, counters and memory identical to the base build (pass counts unchanged). **Final:** Debug and Release 99/99 (VulkanPipelineContractTests ported: scene resolve without a render pass, new material-pipeline-library test over the three G-buffer layouts); sweep `r4ax-sweep` 36/36, **0 changed entries** against a same-depth base sweep (`r4ax-sweep-base`); against `r3-sweep` only the known R4a.0/R4b.2 deltas (topology hash, startup/rebuild allocation bytes); T-F1/T-F7 A,B,B,A (`timing/r4ax-short`, 1500 frames) 0 steady allocations, GPU median F1 1.0714 -> 1.0717 ms, F7 1.930 -> 1.877 ms (A run 1 was a slow outlier; run 4 A 1.878), per-pass deltas within +-2 us. | Lane X report. |
| 2026-10-03 | R4a lane Y: shadows, probe capture, UI/HDR10/swapchain and ImGui on dynamic rendering (`1cdc30a`, `fc20468`, `f5943cf`, `75d7b06`, `ea7168d`, `b8cc848`; R4a.7a directional, R4a.7b spot, R4a.7c point, R4a.8 probe capture, R4a.9 UI/HDR10/swapchain/ImGui, R4a.9b policy retirement; rebased onto lane X at `254c11c`; the R4a.7a-c labels are the design's item 7 and overlap lane X's "R4a.7" plan-entry label). **Shadows:** the five shadow maps are `ExecutorOwned` global imports bound `SampledRead`; each drawing pass registers with `dynamicRendering`, so the executor moves the whole image DS_RO -> DS_ATT with its contents kept (old layout never UNDEFINED, also for directional `LoadOp::Clear`) and the next reader (`probe.capture`, else lighting) moves it back; every frame that writes a map also runs lighting, so frames and rebuilds always leave them `SampledRead`. Directional begins rendering per cascade layer view (CLEAR 1.0 from the plan); the spot atlas is **one** rendering instance for the pass (LOAD) with `vkCmdClearAttachments`, viewport and scissor per tile; point pools begin per face layer view (plan depth candidate = tier, LOAD) with the full-face clear. Shadow pipelines chain depth-only D32 `VkPipelineRenderingCreateInfo`; the three classes' own render passes and framebuffers are deleted. **Probe capture** (owner-managed, ADR-0016 rule 6): per face, one barrier moves the raw-radiance layer UNDEFINED -> COLOR (after FRAG|COMPUTE reads) and the depth layer UNDEFINED -> DS_ATT (also after earlier LATE_FRAGMENT_TESTS depth writes, a small superset of the old EXTERNAL dependency), `vkCmdBeginRendering` with the old clears (0,0,0,1)/1.0 and depth store DONT_CARE, and after the face COLOR -> SHADER_RO (COLOR_OUT write -> COMPUTE read); the capture targets no longer create framebuffers or take a render pass. **UI/swapchain:** the swapchain is bound per frame after acquire with `current = Present` and `discardOnFirstUse()`: its writer (ui-present, or hdr10-encode-present) transitions from UNDEFINED with the Present (BOTTOM_OF_PIPE = ALL_COMMANDS in sync2) source scope, which chains with the acquire semaphore's COLOR_ATTACHMENT_OUTPUT wait, and the frame-end export moves COLOR -> PRESENT_SRC (reviewed by hand, and the fake-sink tests pin both barriers; the core layer checks the present layout, 0 messages). The UI pass registers with `dynamicRendering` (plan attachment: swapchain or the RGBA16F composition target, render area = swapchain extent); the HDR10 encode pass records `vkCmdBeginRendering` itself on the acquired view (its registration in `VulkanOutputFeature` stays unflagged; it has no same-access writes). ImGui uses `UseDynamicRendering`, `ApiVersion = VK_API_VERSION_1_3` and `PipelineInfoMain.PipelineRenderingCreateInfo` with the UI colour format (also on `onPresentationChanged`); `VulkanEditorUiPresentation.renderPass` is now `colorFormat`. **R4a.9b:** with no production user left, `ExternalSyncMode::RenderPassManaged` and the policy's `initial`/`final` fields are deleted from the executor (tests moved to owner-managed/discard bindings). *Left for R4a.final:* `VkUIRenderPass` (still made by `VulkanUiFeature::createRenderPass` only for the frame targets' UI framebuffers), the `uiFramebuffer`/`uiCompositionFramebuffer` fields and `VulkanTargetRenderPasses`; ADR-0016 item 4 still lists `RenderPassManaged` (an "as implemented (R4)" note is due). **Verification at every step** (`r4ay-7a`, `-7b`, `-7c`, `-8`, `-9`, `-9b`): Release build and tests (three cook process tests with short work directories, local Alfa files hard-linked); frozen set with synchronization validation identical to `r0` or within envelopes (F3/F7-lod depth-tie, F4-woit woit-order), **0 validation messages, 0 hazards** on all 24 captures; indirect digest identical to `r3a0` (and `r3a0-ext` from 7c); F1-all, F5-hetero, F5-point and F6-probecap profile GPU-range sequences, CPU scopes and counters identical to the base build. *GPU ranges:* the shadow layout transitions now sit at the executor's barriers outside the AfterBarriers ranges, and the spot atlas no longer runs one render pass per tile: on F5-hetero `gpu.shadow.directional` 24.7 -> 22.0 us, `gpu.shadow.spot` 14.6 -> 12.6 us, `gpu.shadow.point` 35.8 -> 18.3 us, with `gpu.frame` unchanged within noise (-0.8 to +0.4% on quiet runs), so the whole-array transitions need no per-layer fallback; F6-probecap A/B/B/A at 240 frames 3,546-3,568 us for base, 7c, 8 and 9b alike (two single +6% runs were whole-GPU clock states: every range scaled). Resize/transport validators (V05, V06, V09, V14, V16), V02 and the editor path (`--hidden-window --frame-limit 120 --validation-sync`) report 0 messages. *HDR10:* the display offered HDR10/scRGB only intermittently during the session (the sweep headers flip between runs); while available, `--output-transport hdr10`, `--output-transport scrgb` and `--validate-output-transport-switch` (scRGB, HDR10, SDR all effective) ran with synchronization validation at 0 messages, 0 hazards, and HDR10 `final-output` captures of F1-all, F4-ord2 and F5-hetero are byte-identical to the base build. **Final (rebased on `254c11c`):** Release 100/100 and Debug 99/99 (before the rebase), shipping 93/93 with a hidden 120-frame `--validation`/`--validation-sync` run at 0 messages, `--benchmark` rejected and no qualification strings (cook process tests with short work directories); frozen `r4ay-final` with sync validation identical/within envelopes, 0 messages, 0 hazards; digest identical to `r3a0` and `r3a0-ext`; sweep `r4ay-sweep` 36/36 pass, against a same-depth base sweep (`r4ay-sweep-base`) only the display's HDR availability differs (transport-switch effective transport, supported transports, the HDR-only startup uploads), against `r3-sweep` only the known R4a.0/R4b.2 deltas (topology hash; startup/rebuild allocation bytes); T-F1/T-F7 A,B,B,A against `254c11c` (`timing/r4ay-short`, 1500 frames) 0 steady allocations, GPU median F1 1.1466 -> 1.1398 ms, F7 1.9539 -> 1.9378 ms. | Lane Y report. |
| 2026-10-03 | R4a.final: render passes and framebuffers deleted (`23c19b2`). **Deleted:** `VkRenderPass.{h,cpp}` (`VkRenderPassWrapper`, gbuffer), `VkForwardRenderPass.{h,cpp}` (forward/transparent, with `depthStoreOperation` and its Stage3 test; read-only depth STORE_OP_NONE stays pinned by `ProductionGraphContractTests` and `RenderGraphTests`), `VkUIRenderPass.{h,cpp}`; the lighting, output, layered capture, layered local composition and OIT accumulation/resolve render passes their owners still created; every accessor that exposed a render pass (`gBufferRenderPass`, `forward/transparentRenderPass`, `interfaceCapture/localCompositionRenderPass`, `accumulation/resolveRenderPass`, `outputRenderPass`, `VulkanUiFeature::renderPass`); `VulkanTargetRenderPasses`; and all framebuffers in `VulkanFrameTargets` (11 per-frame fields, the deep-tier interface/local-composition arrays, the per-swapchain-image UI framebuffers with `uiFramebuffer`/`uiFramebufferCount`). `VulkanFrameTargets::init` no longer takes render passes or the swapchain (it now holds only graph-image views, the refraction mip views and the samplers). `VulkanUiFeature::createRenderPass`/`destroyRenderPass` become `setColorFormat`/`resetColorFormat`. `VulkanPipelineContractTests` drops its render-pass handle checks. **HDR10 encode:** `hdr10-encode-present` now registers with `dynamicRendering` and begins rendering through the executor's plan (the swapchain view bound after acquire, CLEAR (0,0,0,1), STORE, swapchain-extent render area: the same `VkRenderingInfo` the pass built by hand); the swapchain is its only write, so no re-barrier is added. `VulkanHdrEncodePass` no longer keeps swapchain views and `rebuildHdr10Targets()` takes no arguments. **Result:** 0 `vkCreateRenderPass`/`vkCreateFramebuffer` calls remain in `src/` outside `src/vendor` (only ImGui's backend, unused in dynamic-rendering mode); 38 files, +68/-1,144 lines; `VulkanVertexBackend.cpp` goes from 2,079 to 2,067 lines, `VulkanFrameTargets.cpp` from 670 to 348. ADR-0016 item 4 gains an "as implemented (R4a)" note. **Verification:** Release and Debug 100/100 (three cook process tests with short work directories; local Alfa files hard-linked; worktree `eol=lf` and `tests/assets` files rewritten as LF); shipping preset builds, hidden 120-frame `--validation-sync` run 0 messages, `--benchmark` rejected; frozen `r4a-final` with synchronization validation identical to `r0` or within envelopes (F4-woit woit-order, F7-lod depth-tie), **0 validation messages, 0 hazards** on all 24 captures; indirect digest identical to `r3a0` and `r3a0-ext`; resize/lifecycle validators V05, V06, V09, V14 (scRGB, HDR10 and SDR all effective) and V16 (`r4a-final-resize`) pass with 0 messages; editor path `--hidden-window --frame-limit 120 --validation-sync` 0 messages and 0 hazards in SDR, HDR10 and scRGB; HDR10 `final-output` captures of F1-all, F4-ord2 and F5-hetero byte-identical to a `6f8b282` base build (that point is pre-encode, so the encode pass itself is covered by command equivalence and validation, not by an image). | R4a.final report. |
| 2026-10-03 | R4b.3 transient aliasing planner (`renderer/graph`, backend-neutral; no executor or Vulkan change, production output unchanged). **Eligibility** is computed by the compiler for every resource (`CompiledResource::aliasEligibility`, first failing rule: unused, imported, History, not transient, exported, buffer, first use not a discard): a transient, unexported, non-History, non-imported image whose first use is a `LoadOp::Clear` write, or a DontCare write declared whole-resource with the new `RenderGraphBuilder::declareWholeResourceWrite(version)` (any write access; rejected on a load or an initial version; hashed only when declared). **Slots:** `compile(CompileOptions{ .transientAliasing })`, default off, gives every eligible image its own slot (`PhysicalResourceSlot::aliased`) and keeps exact-descriptor reuse for the rest; hashed only when on. With it off the 11 production topologies keep their pre-R4b.3 slots (golden digests generated from `6f8b282`). **Planner:** `planTransientAliasing(graph, span<TransientMemoryRequirement{size, alignment, typeMask}> per logical resource, AliasingOptions{minimumAlignment})` → `AliasPlan{heaps{size, alignment, typeMask}, placements[logical]{heap, offset, size}, placementOrder, aliasPredecessors (CSR: overlapping range in the same heap and lastUse < firstUse, ordered by lastUse), requestedBytes, committedBytes, peakLiveBytes/peakLivePass}`; interference on inclusive [firstUse, lastUse]; greedy by size desc, firstUse, logical index; each resource joins the typeMask-intersecting heap it grows least (lowest index on ties, else a new heap) at the lowest aligned offset among 0 and the ends of interfering members that overlaps none. **4K estimate** (default SDR, CanonicalReference, pyramids on; RGBA16F 8 B/px, R32/D32 4 B/px, mips x4/3, 64 KiB alignment): requested 564.3 MB per frame slot, peak 398.4 MB at `lighting`. As declared today the refraction pyramids' first use is an undeclared `StorageReadWrite`, so only the output aliases (431.6 MB aliased into 398.4 MB, plus 132.7 MB dedicated = 531.1 MB, 33.2 MB saved per slot). With the pyramid build declared whole-resource the plan reaches the peak: 398.4 MB, saving 165.9 MB per slot (331.9 MB over two slots, 29%), as the design expected. **R4b.4 must:** thread the option through `buildVulkanProductionRenderGraph`; declare the pyramid build whole-resource (and audit other storage first writes, such as the deep-tier termination tiles); check that no reader of an aliased resource runs when its first-use writer is skipped; size requirements with `vkGetDeviceImageMemoryRequirements` restricted to the legacy memory type; allocate one dedicated VMA heap per `AliasHeap` per frame slot and bind with `vmaCreateAliasingImage2`; reset aliased slots to Undefined at frame start and use the predecessors' final accesses as the first-use source scope. | `RenderGraphAliasingTests` (10 cases); Release and Debug ctest; frozen `r4b3` against `r0`. |
| 2026-10-03 | R4c.1-R4c.3: deferred deletion and drain-free capacity growth and probe changes (`9e583aa`, `b1d6cae`, `5ffd237`). **R4c.1:** `VulkanDeletionQueue` {retireValue, kind, payload}: buffer, image, image view, sampler, pipeline, pipeline layout, descriptor set (with its allocator) and a function-pointer callback with four 64-bit arguments (no `std::function`). 256 entries are reserved and the vector grows only on a frame that retires more. Collection is FIFO; an out-of-order key only delays. The scheduler owns the queue: `retireValue() = frameRecording ? lastSubmittedSerial + 1 : lastSubmittedSerial`; `beginFrame` collects every entry with `completedSerial >= key` right after the slot's fence wait, and `cleanup` flushes the rest. New `slotInFlight(slot)`, and `refreshCompletedSerial()`, which raises the completed serial from fences that have already signalled (`vkGetFenceStatus`, no wait). `defer`, the per-slot `DeletionQueue` and its slot keying are gone. The registry's three users (geometry, geometry arena, and texture, whose callback releases the editor descriptor and the vault slot before the image) retire through the queue, which closes design finding 3. New `VulkanDeletionQueueTests` reproduce that ordering on a hidden window under synchronization validation: a buffer filled by frame N and retired after its `endFrame` survives the next `beginFrame` (where the old `defer` freed it) and is destroyed only once serial N has completed; a retire inside an open frame keys the frame being recorded. **R4c.2:** slot-retirement swap at every capacity-growth site: GPU-scene tables, the four view cullers and the opaque culler, canonical materials (per-slot `bindMaterialBuffer`), cluster light records/active lists (per-slot cluster-set rewrite) and probe records/active/parameter/cluster buffers (per-slot probe-clustering set). Replacements are created at once. A slot that is not in flight (startup, or after a bounded stall) is collected and swapped now. An in-flight slot parks its replacement; a parked set that a second growth replaces was never used and is destroyed. The parked set swaps in `VulkanVertexBackend::beginFrame`, right after the executor retires the slot and after the extension, feature and culler collects. The swap destroys the old per-slot buffers, rewrites the slot's own sets and the lighting set's slot (`VulkanSceneDescriptors::set*` and `VulkanDeferredLightingFeature::bind*` take a slot), and rebinds the slot's graph imports (`rebindGraphImportedBuffers(slot)`). Only those frames carry the new `cpu.renderer.slot_swap` scope. The opaque culler's shared LOD-history buffer goes to the deletion queue while a slot is in flight. `bindGraphImportedBuffers` now runs only after a graph rebuild (device idle) and has no drain; growth calls `rebindIdleSlotImports` (idle slots now, in-flight slots at retirement). The culler seam's `waitForAllFrames` is replaced by `slotInFlight` and `retireBuffer`; the fake-seam tests cover the idle/in-flight split, re-growth before retirement and the history retire. With every slot idle the old whole-table paths run, so startup is unchanged. **R4c.3:** probe owner removal retires the removed owners' capture targets through the deletion queue (`VulkanReflectionProbeCaptureTargets::setDeferredDestruction`; destroyed at once when nothing is in flight) and drops their unpromoted captures (filter sets and baked readbacks retired). Environment-table changes rebind the lighting set's probe table per slot (idle slots now, in-flight slots at retirement). `finalizeCaptures` promotes only captures whose recording serial has completed, after `refreshCompletedSerial`; the replaced published cube goes to the deletion queue. F6-probecap stays byte-identical, so no qualification-only drain was kept. *Remaining drains (bounded):* output LUT change, transparency topology change, resize `vkDeviceWaitIdle` and scene-extent change (ADR-0013), shutdown, editor retained views, and the four qualification capture collections. **Verification at every step:** Release build and tests (98/101 in ctest; the three cook process tests pass with short work directories; local Alfa files hard-linked; the non-LF `tests/assets` fixtures re-checked-out LF). Frozen `r4c1`, `r4c2` and `r4c3` with synchronization validation are identical to `r0` or within envelopes (F3/F7-lod depth-tie, F4-woit woit-order), with 0 validation messages and 0 hazards on all 24 captures. Indirect digest identical to `r3a0` and `r3a0-ext`. Compressed scripted-change runs of both hitch routes under synchronization validation report 0 messages and 0 hazards. **Final:** Debug 98/101 plus the three short-directory process tests. Sweep `r4c-sweep` 36/36 pass. Against a same-depth base sweep (`r4c-sweep-base`, a Release build of `6f8b282`), apart from the checkout-relative manifest path, only the probe entries change (V02, O03, R00-R03): `cpu.renderer.drain_all_frames` leaves the CPU range set, `cpu.renderer.slot_swap` joins it, and the startup frame allocates 8 more C++ bytes (counts unchanged). V05/V06/V09/V16 are unchanged; against `r3-sweep` the rest are the known R4a.0/R4b.2 deltas. **Hitch** (`hitch/r4c-hitch`, A = `6f8b282`, B = R4c.3, A,B,B,A, 500 + 10,000 frames; OneDrive sync held about one core throughout): H-stress drain frames 8 -> 0, hitches 4.0 -> 3.5, p99 79.8 -> 77.4 ms; median 72.8 -> 61.2 ms, dominated by noisy per-instance CPU work. The event maxima are unchanged (add_instances 150/286/260 ms vs 130/287/222 ms), because they are CPU scene work: the drains they contained measured 0.01 ms. H-probe drain frames 272 -> 0 (266 were the realtime capture's periodic finalize outside events), hitches 5.5 -> 1.0, p99 10.80 -> 10.88 ms. The first pair's median gap (6.30 vs 6.59 ms; GPU sub-ranges identical, only the acquire-inclusive `gpu.frame` moved) vanished in a B,A,A,B rerun (`hitch/r4c-hitch-probe-baab`: median 6.56 vs 6.55 ms, p99 10.87 vs 10.93 ms, hitches 3.0 vs 2.5). Probe event maxima are bounded by a periodic ~12 ms present stall (every ~30 frames in both builds). No ADR change. | R4c.1-3 report; `hitch/r4c-hitch`, `hitch/r4c-hitch-probe-baab`; sweeps `r4c-sweep`, `r4c-sweep-base`. |
| 2026-10-03 | R4c.4: persisted pipeline cache. **Service:** `VulkanPipelineCache` (`renderer/vulkan`), owned by the backend, created right after the device and before any pipeline, saved and destroyed at cleanup after device idle and before the device; the handle reaches feature owners as `VulkanFeatureContext::pipelineCache`, the VSM resources and the material pipeline library from the backend, and ImGui as `VulkanEditorUiDevice::pipelineCache` (`initInfo.PipelineCache`). All 26 `vkCreateGraphicsPipelines`/`vkCreateComputePipelines` sites in `renderer/vulkan` pass it (21 files; each owner's `init` takes `VkPipelineCache` after the device, the culler helpers `createComputePipeline`/`createIndirectViewPipeline`/`createOpaqueCullPipelines` take it explicitly); 0 null-cache sites remain outside `src/vendor`. **File:** `<dir>/<vendor>-<device>.ircache` (lower-case hex), a 64-byte little-endian header (magic `IRPCACHE`, version 1, header size, vendorID, deviceID, driverVersion, driverID, pipelineCacheUUID, payload size, FNV-1a 64 of the payload) followed by the `vkGetPipelineCacheData` payload, whose `VkPipelineCacheHeaderVersionOne` (size, version, vendor, device, UUID) is validated too. Any mismatch or corruption (truncated, trailing bytes, bad magic/version, identity, hash, Vulkan header, driver rejection) discards the file with one log line and starts empty; a file above header + 512 MiB is rejected without being read. Saves are skipped when the payload hash equals the loaded one, else written to `<file>.<pid>.tmp` and renamed over the file. **Location:** `platform/UserCacheDirectory` (`%LOCALAPPDATA%/Iridium/<sub>`, XDG/`~/.cache` elsewhere, temp-directory fallback); new renderer flag `--pipeline-cache DIR\|off` (default: the user cache directory; `off` = no `VkPipelineCache`, exactly the pre-R4c.4 creation path; `RenderBackendConfig::pipelineCacheDirectory`, empty = off). CLI registry: renderer 30 flags, 86 in a qualification build, 48 without. **Telemetry:** the profile header's `startup.pipeline_cache {state off\|cold\|warm\|discarded, loaded_bytes}` (via `RenderBackendRuntimeInfo`); saves are logged (`Pipeline cache: saved N bytes ...` / `unchanged`). **Harness (decision):** every M7R run script (`Run-FrozenCaptures`, `Run-TimingPair`, `Run-HitchScenario`, `Run-IndirectDigest`, `Run-QualificationSweep`) takes `-PipelineCache` and defaults to `off`, so evidence never depends on what earlier runs left on disk and both sides of a pair create pipelines the same way (a pre-R4c.4 baseline has no cache); a directory gives an explicitly warm run. `Get-M7RPipelineCacheArgs` (`M7RFixtures.ps1`) omits the flag for executables that predate it (checked via `--help`). **Cold vs warm** (F1-all, native 4K, Release, three fresh cache directories, `out/m7r/pipeline-cache/r4c4-coldwarm`): with the NVIDIA driver's own shader disk cache warm, backend init is off 336-338 ms, cold 380-402 ms, warm 342-349 ms (total startup off 1,486-1,510, cold 1,566-1,637, warm 1,495-1,557 ms); the cache holds 2.51 MB. With the driver cache disabled (`__GL_SHADER_DISK_CACHE=0`, `r4c4-coldwarm-nodrv`): off 392-422, cold 374-376, warm 349-359 ms. So on this machine a warm cache saves 35-70 ms only when the driver cache is cold (first run, driver update), and the first run with an empty app cache costs about 45-65 ms once, because the driver then compiles into the app cache instead of reusing its own. **Verification:** Release and Debug 100/103 in ctest plus the three cook process tests with short work directories (local Alfa files hard-linked; `eol=lf` and `tests/assets` files rewritten LF); new `VulkanPipelineCacheFileTests` (7 device-free cases: round trip, each identity mismatch, truncation/trailing/hash/magic/version, the Vulkan header fields, the oversized cap without a read, atomic replace and failure without a leftover temporary, the user cache directory) and `VulkanPipelineContractTests` now creates every production pipeline through one cache and saves, reloads warm (payload identical, non-empty), reuses, discards a corrupted file and repairs it, under validation; CLI parity row and counts. Shipping 95/96 plus the short-directory cook test; hidden 120-frame `--validation-sync` editor runs cold then warm (`loaded` / `unchanged`) and `off` with 0 messages, `--benchmark` rejected. Frozen `r4c4` (the main checkout's scripts, so the default user cache directory, warm) with synchronization validation identical to `r0` or within envelopes (F3-stress depth-tie, F4-woit woit-order), 0 validation messages, 0 hazards on all 24 captures. Indirect digest `r4c4` (`off`) identical to `r3a0`. Sweep `r4c4-sweep` (`off`): against `r4c-sweep` only the new `pipeline_cache` header fields and the checkout-relative manifest path change, apart from the probe entries below; against `r3-sweep` the rest are the known R4a.0/R4b.2/R4c deltas and the display's HDR availability. **Finding (pre-existing, R4c.3):** the probe-route sweep entries R00-R03 (0 warm-up frames, capture at frame 2) are no longer deterministic, because `finalizeCaptures` now promotes a capture only once its serial has completed: across six runs each route produced either the historical image or `a0dfab55` (the capture not yet promoted), and R02 failed its capture match in `r4c4-sweep`. The base build (`d2a0bf9`) reproduces it (`r4c4-sweep-rbase1`, `-rbase2`), and the V02/O03 probe counters move with it. Candidate fix: warm-up frames for R00-R03, or a qualification-only finalize drain on those routes. No ADR change. | R4c.4 report; `out/m7r/pipeline-cache/r4c4-coldwarm`, `-nodrv`; captures `r4c4`; digest `r4c4`; sweeps `r4c4-sweep`, `r4c4-sweep-r*`. |
| 2026-10-03 | R4b.4-R4b.6 aliased transient graph memory (`42c8831`, `23f6992` and the R4b.6 commit that records this entry). **R4b.4 (executor/allocator, switch default off):** `VulkanResourceAllocator` gains `imageMemoryRequirements` (`vkGetDeviceImageMemoryRequirements`, memoryTypeBits reduced to the legacy DEVICE_LOCAL type), `createAliasHeap` (one dedicated VMA block, rounded to 64 KiB, accounted under `RenderGraphTransient` with its members' requirement sum as requested bytes), `createAliasingImage2D` (`vmaCreateAliasingImage2` at the planned offset; the image's own profile record is uncounted) and `createAliasingBuffer`. The executor plans a graph compiled with `CompileOptions::transientAliasing` at rebuild (`planTransientAliasing` over factory requirements), creates one heap set per frame slot (heaps retire with their slot's resources), resets aliased slots to Undefined in `beginFrameExecution`, and transitions each first use from UNDEFINED with the source scope = the union of the alias predecessors' *tracked* accesses in the frame (so a skipped last reader narrows nothing; no predecessor ran => NONE, the slot's previous frame having retired with its fence). **Skip-path guard:** using an aliased image before its first-use writer ran this frame (or with a load) throws. `--render-graph-aliasing on\|off` reaches `VulkanProductionGraphFeatures::transientAliasing` through `RenderBackendConfig`; profile metadata adds `transient_aliasing`, `alias_heap_count`, `aliased_resource_count`, `aliased_requested_bytes`, `alias_heap_committed_bytes` (heaps per slot; bytes over all slots; `committed_bytes` includes the heaps). The fake factory defaults keep device-free tests working (nominal requirements, memoryless heaps). **R4b.5:** the editor bridge requests the new `VulkanGraphHooks::editorDepthSample`, so `ui-*` declares its glass-depth read of `depth.opaque` (design finding 5; no new transition: depth stays SampledRead after output-transform; the bridge is attached in every Application run, so production topology hashes change). `declareWholeResourceWrite` on both refraction pyramids (every mip written in order from scene colour/depth) and on the deep-tier tile masks (one workgroup per 16x16 tile). *Skip-path audit:* the pyramids' readers (compatibility forward, Ordinary2/deep local compositions) run only for a non-empty compatibility queue, which is exactly when the build runs; each layered tier's captures, terminations, compositions and resolve share one activity condition; the validation hooks require draws; OIT accumulate/resolve share theirs. The one exception, the shared deep resolve with both deep tiers resident (it runs when either tier has draws and reads both tiers' local colour and first identity), uses the new `RenderGraphBuilder::excludeFromAliasing` (`AliasEligibility::Excluded`, hashed only when declared). Qualification-only `--qualification-alias-poison` (implies aliasing on): at the new notification hook `VulkanHookPoint::FrameGraphBegin` (raised after `setFrameRecordContext`, before any pass) the extension fills every alias heap of the slot through a whole-heap aliasing buffer with `0x7FC07FC0` (a NaN as f32 and as two f16) followed by a TRANSFER -> ALL_COMMANDS memory barrier; buffers are recreated when the graph's rebuild count changes and print `IRIDIUM_ALIAS_POISON`. Fresh non-aliased VMA suballocations are not poisoned (deferred). *Finding:* synchronization validation does not report a missing alias-predecessor scope (checked by disabling the predecessor loop: the aliased device topologies still validate clean), so the fake-sink test pins the first-use scopes and the poison captures are the runtime check. **R4b.6 (default on):** gates all met. *Fix found by the gate:* with aliasing on, the indirect digest's F7-hiz run (no validation layers, which wrap and never recycle handles) aborted at startup with "Tracked external graph buffers must not alias resources or frame slots": a capacity growth with both slots idle swaps every slot at once, and `rebindIdleSlotImports` rebound slot 0 while slot 1 still held a destroyed handle that the point culler's replacement had recycled (`shadow.point.indirect-commands` slot 0 vs `gpu-scene.opaque.indirect-counts` slot 1). It is a latent R4c.2 ordering bug that aliasing only exposed by changing the allocation pattern; idle slots are now all unbound before any is bound (as `bindGraphImportedBuffers` does), and the executor's error names both bindings. **Memory at 4K** (T-F1/T-F7, default SDR, pyramids resident, 2 frame slots): graph committed 1,183.6 MB off -> 842.0 MB on (**-341.6 MB, -170.8 MB per slot**; the design expected ~332 MB from texel estimates); one heap per slot, 10 aliased images, 571.9 MB requested -> 401.1 MB committed per slot (1.43x, 29.9% saved, equal to the plan's peak-live bound); physical slots 19 both ways. The 4K planner estimate in `RenderGraphAliasingTests` is now the as-declared graph: 564.3 MB -> 398.4 MB per slot, every transient image eligible. **Verification:** Release 99/102 + the three cook process tests with short work directories at every step (local Alfa files hard-linked; worktree `eol=lf` and `tests/assets` files rewritten as LF); final Debug likewise 99/102 + 3; shipping preset 94/95 + `CookSceneCliTests` with a short work directory, hidden 120-frame `--validation` and `--validation-sync` runs 0 messages / 0 hazards, `--benchmark` rejected, no poison option string in the 6.06 MB binary. New tests: fake-sink first-use scopes, skip guard and aliased steady frames (`VulkanRenderGraphExecutionTests`), aliased base/HDR10/all-features topologies with the poison fill on a device under synchronization validation (`VulkanRenderGraphDeviceTests`), exclusion hashing and the production declarations (`RenderGraphAliasingTests`). Frozen set with synchronization validation identical to `r0` or within envelopes (F3/F7-lod depth-tie, F4-woit woit-order) and 0 validation messages / 0 hazards on all 24 captures in every mode: `r4b4`, `r4b4-on`, `r4b5`, `r4b5-on`, `r4b5-poison`, `r4b6`, `r4b6-final`, `r4b6-final-poison`; `r4b6-noval` (no layers) likewise. The F1 poison run fills one 401,080,320-byte heap per slot and its final-SDR hash equals `r0`. Indirect digest identical to `r3a0` (`r4b4`, `r4b5`, `r4b6-fix`) and `r3a0-ext` (`r4b6-fix-ext`). Sweep `r4b6-sweep` 36/36 pass; against `r4c-sweep` the only non-volatile differences outside the probe entries are the aliasing fields (topology hash, graph committed bytes, alias counters; physical slots V06/X02 18 -> 19 and V08/V09 37 -> 39 because eligible images leave exact-descriptor reuse), the memory profile (fewer live allocations, lower committed, requested +14-99 MB because a heap records its members' requirement sizes and the images no longer share reuse slots) and startup/rebuild-frame C++ allocations on the resize/lifecycle entries (V05/V06/V09/V16, +37-53 calls on the first frame: plan, CSR and heap vectors; steady frames unchanged). V02/R00-R03 (`r4b6-sweep-probe`, current frame-4 table) are identical to `r4c4-probefix-1` except R00's probe publish counters, which vary run to run with aliasing on or off (3 or 4 published; same image); O03 publishes 1 capture with aliasing on and off alike (its counters and frame-2 capture differ from `r4c-sweep`/`r4c4-probefix-1` only through the R4c.3/R4c.4 sweep-table history). V14 transport switch passes. Editor path (`--hidden-window --frame-limit 120 --validation-sync`) 0 messages / 0 hazards with `--output-transport` sdr, hdr10 and scrgb requested. **Timing** (`timing/r4b6-onoff-short`, one build, A = off, B = on, A,B,B,A, 500 + 1,500 frames): T-F1 GPU median 1.0826 -> 1.0134 ms (-6.4%), CPU 1.4497 -> 1.3687 ms; T-F7 GPU 1.9893 -> 1.9261 ms (-3.2%), CPU 5.4603 -> 5.3954 ms; 0 steady-frame allocations on both routes. The switch stays until R6. No ADR change. | R4b.4-6 report; captures above; `digests/r4b6-fix*`; sweeps `r4b6-sweep`, `r4b6-sweep-probe`; `timing/r4b6-onoff-short`. |
| 2026-10-03 | R4d.1-R4d.4: transfer queue, timeline semaphores, staging ring and `vkQueueSubmit2` (`9335fbc`, `41f64d1`, `52dc287`, `88114bb`; fixes `981cdfc`, `6bb817d`; harness `cd69afe`; this entry and the ADR-0016 note in the R4d.4 docs commit). **R4d.1:** `VkContext` selects the upload family with `selectVulkanTransferQueueFamily` (a TRANSFER family without GRAPHICS or COMPUTE and with (1,1,1) image granularity, else a graphics-free compute family, else graphics) and creates its queue. On the RTX 4090 that is **family 1, dedicated transfer**. It also enables `Vulkan12Features.timelineSemaphore` when supported (`hasTimelineSemaphore()`); `HeadlessVulkanDevice` mirrors both. New renderer flag `--upload-queue auto\|graphics\|legacy-blocking` (default auto) via `RenderBackendConfig::uploadQueue`; CLI registry 89 flags, renderer 32, 50 without qualification. **R4d.2:** outside legacy-blocking, uploads stage into one 64 MiB VMA persistently mapped ring (`UploadStaging`, 256-byte aligned, device-free `StagingRingAllocator`). An upload larger than a quarter of the ring, or one that the open batch leaves no room for, gets dedicated staging. Batches, dedicated staging and each upload lane's four command buffers are reused once that lane's timeline passes the batch's value. A full ring waits for the oldest submitted batch inside `cpu.renderer.upload_wait`, so Analyze-Hitches counts it. Image regions use a reused member vector, so ring uploads allocate nothing per upload. **R4d.3:** in auto mode with a separate family, uploads into fresh destinations (state Undefined) record on the transfer lane as copy plus release (TRANSFER_WRITE to none, transfer to graphics family; images carry the final layout). The matching acquires (same families and layouts, ALL_COMMANDS with MEMORY_READ\|WRITE) are recorded at the start of the next frame command buffer, before any pass. Non-fresh destinations, layout-only transitions and every upload in `graphics` mode stay on the graphics lane without an ownership transfer, published to ALL_COMMANDS since no CPU wait follows. `beginFrame` calls `submitAsync`: both lanes signal their timelines and the CPU does not wait. The frame submission waits on the returned values at ALL_COMMANDS. Blocking flushes (init, `createFrameTargets`, registry error paths, cleanup) stay bounded: submit, acquire in a graphics batch that waits on the transfer timeline, then a CPU wait. `legacy-blocking` is the pre-R4d code path unchanged: per-upload staging, graphics submit and a fence wait in every flush. A *retire floor* (`VulkanFrameScheduler::setRetireFloor`) keeps any resource an outstanding upload writes alive until the frame that waits on that upload (lastSubmitted + 1, or + 2 while a frame records). Without it, a texture freed before its transfer completed could be destroyed when the previous frame retired. Profile header `startup.upload_queue {mode, kind, family, staging_ring_bytes, staging_ring_waits, dedicated_staging_uploads, async_submits}`; scope `cpu.renderer.upload_submit`. **R4d.4:** with timeline semaphores the scheduler creates a graphics timeline instead of frame fences. Each frame submission (`vkQueueSubmit2`; synchronization1 `vkQueueSubmit` with `VkTimelineSemaphoreSubmitInfo` when synchronization2 is absent) waits on imageAvailable at COLOR_ATTACHMENT_OUTPUT and on the upload timelines at ALL_COMMANDS. It signals renderFinished and the graphics timeline = the frame serial. Slot, swapchain-image-owner and drain waits wait on that serial. `refreshCompletedSerial` reads the counter, which feeds the deletion queue. Without timeline semaphores the fence path is unchanged. *Finding (pre-existing, fixed by R4d.4):* the fence path's per-image owner map stored the *fence* of the slot that last used an image. With 3 swapchain images and 2 frame slots that fence had since been reused by the newer frame, so every frame waited for the previous frame's GPU work, serializing CPU and GPU. Per-image serials wait for exactly the right frame. *Fix `981cdfc` (pre-existing, also in legacy-blocking):* `allocateTexture` between frames grew the current slot's indexed texture table while that slot's last frame could still execute, destroying a descriptor pool in use. The new mid-run texture uploads exposed it once the table passed 64 entries. The registry now grows only a slot that is not in flight; `beginFrame` grows it after its wait. *Fix `6bb817d`:* the runtime-info upload names are static `string_view`s ("dedicated-transfer" exceeded the small-string buffer, +1 allocation per `getRuntimeInfo`, visible in sweep V09). **Harness `cd69afe`:** scripted change `add_textures {count, resolution}` (fresh RGBA8 mip chains, pixels built at startup, never sampled), scenario `hitch-upload.v1.json` (route H-upload, F1-all, 500 + 5,000 frames: 8×1024², 16×1024² (overflows the ring), 64×256², 2×2048², 1×4096²), and `Run-HitchScenario -BaselineArgs/-CandidateArgs`. **Verification:** Release and Debug 102/105 plus the three cook process tests with short work directories; shipping 96/97 plus `CookSceneCliTests` with a short directory, hidden 120-frame `--validation` and `--validation-sync` runs with 0 messages, `--benchmark` rejected (local Alfa files hard-linked; worktree `eol=lf`/fixture files rewritten LF). New device-free `VulkanUploadQueueTests` (selection; ring alignment, wrap, FIFO retirement on both lanes, full ring, batch-list merge). New `VulkanUploadContextTests` under synchronization validation: legacy, graphics-ring and transfer-queue blocking round trips; transfer-queue frame acquire and graphics-fallback frame wait; non-fresh re-upload on graphics; oversized and open-batch overflow staging; asynchronous ring wrap with full-ring wait; retire floor. `VulkanDeletionQueueTests` cover the retirement ordering and non-waiting refresh on the timeline and fence paths. Frozen sets with synchronization validation are identical to `r0` or within envelopes (F3/F7-lod depth-tie, F4-woit woit-order), with **0 validation messages and 0 hazards on all 24 captures** for each label: `r4d1`, `r4d2`, `r4d3`, `r4d4` (auto), `r4d4-graphics`, `r4d4-legacy`. Digest `r4d` identical to `r3a0`; `r4d-ext` identical to `r3a0-ext`. Sweep `r4d-sweep-final` 36/36 pass; against `r4b-main-sweep` the differences are: the new `upload_queue` header fields; +64 MiB committed (the ring) and fewer peak allocations in every memory profile; the checkout-relative manifest path; `cpu.renderer.upload_wait` becoming `upload_submit` in the probe routes' range sets (V02, R00-R03); and O03, which now promotes its realtime capture at frame 9 instead of 8 (the route ends at 8), with the same capture image. The O03 shift happens with all three upload modes and not with the base build, so it is the R4d.4 frame path: the completion race `finalizeCaptures` accepts since R4c.3. R00's publish count (3, base also 3; `r4b-main-sweep` 4) is the known run-to-run variance. **Timing** (`timing/r4d-short`, A = main `b360e3a`, A,B,B,A, 500 + 2,000 frames; 0 steady-frame allocations): T-F1 CPU frame median 1.479 -> 1.131 ms, GPU 1.144 -> 1.138 ms; T-F7 CPU 5.407 -> 3.404 ms, GPU 2.053 -> 1.944 ms. Both come from the image-owner wait (F7 1.97 -> 0.003 ms): CPU and GPU now overlap. Non-wait CPU is F1 0.339 -> 0.369 ms (+12 µs forward recording, +9 µs submit, +5 µs gbuffer recording, now running concurrently with the GPU) and F7 3.367 -> 3.333 ms. **Hitch** (`hitch/r4d-hitch`, A = main exe, A,B,B,A, 500 + 10,000 frames). H-stress: median 46.05 -> 39.58 ms, p99 47.49 -> 41.05 ms, upload-wait frames 1 -> 0 (the 1×1 add_materials texture), hitches 3 -> 4 (threshold 2× a lower median), every event maximum lower (add_lights 30.9 -> 24.7, add_materials 31.0 -> 24.8, add_instances@3500 186.9/502.6 -> 180.4/181.3 ms). H-probe: median 4.10 -> 3.70 ms, p99 7.30 -> 7.00 ms, upload waits 0/0, event maxima lower at 9 of 10 events (remove_capture_probe at 5500: 4.14 -> 4.36 ms). *Slow frame:* the only `scripted_slow_frame` record is a **baseline** (pre-R4d) run: H-stress run 4, add_instances at 3500, 502.6 ms. Its scopes add to about 165 ms (`cpu.gpu_scene.publish` 139 ms with `synchronize` 127.8 ms, extract 13.7 ms, sorts and recording about 10 ms), consistent with the 187 ms event frame of the other runs. The remaining about 338 ms falls outside every profiled scope (`drawFrame`'s unscoped stretches or the PreSceneUpdate observer; the scripted apply itself took 0.36 ms), so the scope detail cannot localize it further. It is unrelated to the upload path. Finer scopes or an ETW sample are needed next time. **H-upload** (`hitch/r4d-hitch-upload`, one build, A = `--upload-queue legacy-blocking`, B = auto): upload-wait frames 5 -> 0; event maxima 7.82 -> 3.96, 12.98 -> 7.34, 12.19 -> 6.12, 6.89 -> 2.91, 11.02 -> 5.57 ms; median 1.13 both, p99 1.30 -> 1.29 ms, hitches 5 -> 9. In auto, part of the cost moves two frames later: that frame's submission waits on the transfer timeline at ALL_COMMANDS, and the slot wait two frames on shows it (2.6-6.1 ms). Narrowing that wait (first consuming stage, or acquiring only once the timeline has passed) is the next step. The R4 design already noted to narrow the wait later. Async compute remains a later candidate. ADR-0016 item 7 gains an "as implemented (R4d)" note. | R4d report; captures `r4d1`-`r4d4*`; digests `r4d`, `r4d-ext`; sweeps `r4d-sweep-final` (`r4d-sweep`, `r4d-sweep-base-v09`, `r4d-probe-*` for attribution); `timing/r4d-short`; `hitch/r4d-hitch`, `hitch/r4d-hitch-upload`. |
| 2026-10-03 | R5 design accepted (`docs/milestones/M7R-R5-cpu-frame-design.md`). The owner approved enkiTS v1.12. Tracy is deferred, and the opaque tie-break goes to M9. The allocation invariant widens to every non-qualification route, and mixed bins are accepted. The image-owner fence serialization that R5 attributed (1.8 ms of F7) was fixed in R4d.4. | Owner decision (enkiTS); lead decisions on the rest. |
| 2026-10-03 | R5b.0-R5b.1 task system (lane T). **R5b.0:** enkiTS v1.12 through FetchContent (GIT_TAG `0289cf6ffce8`, full clone, static C++ library only; C interface, examples, install and sanitizers off; `ENKITS_TASK_PRIORITIES_NUM=3`; never vendored). `EnkiTsSpikeTests` proves on MSVC/C++20, against enkiTS directly: three priorities; a HIGH-filtered main-thread wait never runs MED/LOW work; pinned tasks on a dedicated I/O thread that never runs task sets; waits inside tasks; no allocation after `Initialize` (15 allocations / 78,456 B at init, then 0 in 64 warm-up + 2,000 steady iterations by both the enkiTS allocator hook and the engine operator new counter). **R5b.1:** `Iridium::Tasks::TaskSystem` (`src/core/tasks`, library `iridium_tasks`; enkiTS linked privately): caller-owned `TaskSet`/`FunctionTaskSet` (the object is the design's `TaskHandle`), `parallelFor`, `PinnedTask` on the pinned I/O thread, `Strand`/`StrandItem`; `Periodic` deferred to R5b.2 (it needs the orchestrator tick). Priorities FrameCritical/Normal/Background = enkiTS HIGH/MED/LOW. Waiting rule: a waiter helps only at or above its own priority (main thread = FrameCritical), otherwise it blocks on `std::atomic::wait`; enkiTS raises the wait filter to the waited task's priority, so lower-priority waits never reach `WaitforTask`. Admission gate: `workers - R` (R = 8, at least 1) thread slots; a background task set is split into at most as many chunks as it holds slots; FIFO admission; a background waiter admits a pending background child under its own slot (no fork-join deadlock). Shutdown cancels pending background work, strand items behind it and later submissions. Task exceptions are caught and counted. Per-worker `CpuProfiler` streams (single-producer rings merged at `endFrame` into `workerEvents`, per-name `workerRanges` = aggregate worker time; exported only when streams exist). Allocation counters are thread-scoped: the `beginCpuAllocationFrame` caller plus threads inside `CpuAllocationFrameScope` (frame-critical tasks) count as frame; every other thread goes to the new `background*` fields of `CpuAllocationFrameSample` (API source-compatible; recording them in profiles is an `src/app` change for R5b.2). **Production construction:** none in R5b.1. `iridium_tasks` is not linked into `IridiumEngine`; R5b.2 constructs it in the frame orchestrator (R5a) with its first consumer. Rationale: `src/app` is R5a's lane, an idle 31-thread pool has no consumer, and its startup cost and profiler-stream memory would perturb the refactor-tier evidence for nothing. ADR-0015 accepted. **Verification:** Release 107/107 and Debug 107/107 (three cook process tests with short work directories; local Alfa files hard-linked; the worktree's CRLF-checked-out fixtures and shaders rewritten as LF; Debug `AssetRuntimeServiceTests` flaked once under the parallel sweep and passed on rerun); `TaskSystemTests` 20/20 Release and 10/10 Debug reruns; shipping preset 100/100 (`CookSceneCliTests` short directory), hidden 120-frame `--validation --validation-sync` run 0 messages / 0 hazards, `--benchmark` rejected, no qualification strings (5.8 MB). Frozen `r5b1` with synchronization validation: 0 messages, 0 hazards, identical to `r0` or within envelopes (F3-stress/F7-lod depth-tie, F4-woit woit-order). Indirect digest `r5b1` identical to `r3a0` on all seven fixtures. Sweep `r5b1-sweep` 36/36 pass; against `r4d-sweep-final` 34 entries identical and R00/R03 differ only by the known probe-publish variance (4 captures published instead of 3; the extra publication frame adds 1 main-thread call / 64 B to `allocation.cpp`). Reruns `r5b1-sweep-probe1..3` give R00 and R03 identical to `r4d-sweep-final` in runs 1 and 3 and the same 4-publish variant of R00 in run 2, so main-thread allocation counters are unchanged. No timing or hitch runs (parallel lanes). | `TaskSystemTests`, `EnkiTsSpikeTests`, `CpuProfilerTests`, `CpuAllocationProfileTests`; captures `r5b1`; digest `r5b1`; sweeps `r5b1-sweep`, `r5b1-sweep-probe1..3`. |
| 2026-10-03 | R5c.1/R5c.2 (lane O): caster and depth-history revisions without per-frame hashing (`f199048`, and the R5c.2 commit that records this entry). **As built:** each revision is a monotonic counter that advances only when the exact bytes the retired FNV-1a consumed differ; the design's sources are the change *triggers*, and an exact field-by-field compare on a triggered frame decides. **Shadow submission** (local shadows, probe scene revision, VSM clip key): triggers are the published shadow membership revision, a new per-list content watermark in `GpuScene.cpp` (max primitive, current-transform and LOD0 geometry record revision of the members; monotonic because the publisher's record revisions share one counter), `VulkanResourceRegistry::materialRevision` (allocate/update/free) and a per-call field check of direct packets. The publication revision and instance revisions are not triggers (they move on non-caster changes such as selection). **Directional cascades:** per light (keyed by evaluation order = shadow index), membership is recomputed only for cascades whose clip matrix bytes changed or when the sequence changed, and each cascade's ordered member sequence is compared exactly (the previous sequence stays in the rebuild scratch). **Depth history (R5c.2):** evaluated only with the depth pyramid (its only consumer); triggers are the GPU-scene publication revision and material revision, plus a per-frame scan of both queues' GPU-scene primitive indices and direct-packet fields (about 25 us on O04). **Allowed divergences (cache-safe, a cache re-renders identical content):** A to B to A content gets new revisions where the hash returned to A; a directional light not evaluated in a frame where casters changed treats all cascades as changed. **Oracle:** `--qualification-caster-revision-oracle` (`VulkanCasterRevisionOracle`) recomputes the old hashes per evaluation and classifies change frames; hash-only fails. Zero revision-only and zero hash-only divergences on the frozen set, all 36 sweep entries (8 frames, and 300 frames on O01/O03/O05/V02/R00/V12/V16), two Hi-Z motion fixtures and compressed H-stress/H-probe scenarios (all ten events, 3,300 frames). **Evidence:** frozen set `r5c2` byte-identical to `r0` except the known depth-tie/woit-order envelopes, sync validation clean; digest identical to `r3a0`/`r3a0-ext`; sweep `r5c2-sweep` against `r4d-sweep-final`: shadow/probe/depth counters identical, deltas are the three new scopes (`cpu.shadow.caster_revision`, `cpu.shadow.directional.caster_revision`, `cpu.render.depth_history.revision`; profile-schema delta) and 2-4 one-time allocations in the first rebuild frame. Scope cost (noisy, untimed): shadow revision 0.1-0.6 us on static frames, directional 10 us on O01 motion, one H-stress event-frame rebuild 6.9 ms at 115k casters (the old hashes cost about 20 ms every frame). No `src/app` change. | Lane O verification; oracle summaries under `out/m7r/sweeps/r5c12-oracle*`, `out/m7r/hitch/r5c12-oracle`. |
| 2026-10-03 | R5 lane P: R5c.5 incremental publisher (`f7b532f`) and R5c.3 compact-sort helpers (`d25e832`), library side only. **R5c.5:** `GpuScenePublisher` keeps dense pools with sorted flat key arrays (instances by owner, primitives per instance by GUID, geometries by identity plus an open-addressed identity table) and caches only (owner, revision) per observation index. It re-reads only observations whose (index, owner, nonzero revision) changed, which is the fast path's own trust, applied per index; the Application's per-slot revision makes that exact. Without an added or removed instance, primitive or geometry it rewrites and revises only the touched records and skips the membership hash when no membership input changed; otherwise it repacks from the pools. A pre-mutation check falls back to re-reading everything when a skipped observation shares a geometry that a re-read one would change, or the scene holds two values for one geometry. Output (tables, revisions, handles, membership, stats, fallback owners) is byte-identical to the old full scan, kept test-only as `ReferenceGpuScenePublisher`. **Remaining floor:** at H-stress `add_instances` 512, `synchronize` fell from 123-128 to about 51-56 ms (untimed run, noisy), of which `publishGpuSceneConsumerMembership`'s byte-serial FNV over 115,712 members takes about 38 ms; the publisher's own work is about 13 ms. The < 10 ms target needs a non-hash membership revision (`GpuScene.cpp`, lane O / R5c.1 territory), which changes revision values and so needs the equivalence oracle. **R5c.3:** `sortOpaqueDrawPackets`, `sortTransparentWorkDrawPackets` and `sortTransparentCompatibilityDrawPackets` sort 20-40 B keys with the same comparisons, then permute packets in place; indicative gains are about 27 % (opaque, 6,464) and 15 % (transparent, 3,904), because comparisons, not 240 B moves, dominate. Call sites are not switched; after R5a.3 each `std::sort` in the sort phase becomes the matching helper with one `CompactDrawSortScratch` member, scopes unchanged. | `GpuScenePublisherReplayTests` (400 randomized sequences, 32,400 publications; H-stress events at two scales; animated; mutations of four safeguards all caught). `CompactDrawSortTests` (1,080 queues; a `stable_sort` mutation fails). Temporary uncommitted engine patches: the reference publisher beside production on a compressed H-stress (4,100 frames, every frame identical, 5 changed publications) plus F5-hetero and F6-probecap (320 changed publications each); compact sorts against packet sorts on 10 frozen fixtures (all but F5-point and F6-probe) and H-stress (queues up to 103,424 packets). Frozen `r5p1` identical or within envelopes, validation and sync 0; digests `r5p1`/`r5p1-ext` identical to `r3a0`/`r3a0-ext`; sweep `r5p1-sweep` against `r4d-sweep-final` changes only first-frame allocation counters (publisher state). Release and Debug 104/107 plus the three MAX_PATH process tests passing in a short directory. |
| 2026-10-03 | R5.0 and R5a.0–R5a.4 (branch `r5a` from `9f2a28e`: `a505353`, `c0f3818`, `97e7719`, `cf1b938`, `413daa1`, `278dd30`, `051aac3`, `8525f4c`, `5ad7ddc`, `1c3432b`). **R5.0 scopes** (12 new `cpu.*` ranges, behaviour unchanged): `cpu.probe.capture.finalize`, `cpu.probe.prepare`, `cpu.asset.material_previews`, `cpu.render.classify`, `cpu.render.extract.parity` (inside `cpu.render.extract`), `cpu.render.transparent.intervals`, `cpu.shadow.directional.schedule` > `cpu.shadow.caster_revision.directional` (per light), `cpu.shadow.local.schedule` > `cpu.shadow.caster_revision.local`, `cpu.probe.capture.schedule`, and in the backend's `submitFrame` `cpu.render.prepare.depth_history` (one scope at the `prepareDepthPyramidHistory` call in `VulkanVertexBackend.cpp`). Scope-tree delta = these names only (at most 53 ranges per sweep entry, below the 64 run-statistic slots). **R5a code motion:** `AssetIntegration` (`src/app`), `EditorHost` (`src/editor`), `RenderExtractor` (`src/extraction`, new `iridium_render_extraction` library with a configure-time guard against ImGui, `iridium_editor`, `iridium_vulkan_imgui` and GLFW links), `FrameOrchestrator` (`src/app`); `Application` is the composition root. **EditorFrameRequests** {optional output (transport, EV, paper white, peak), optional shadows (allocation fields ignored on apply), optional probes (also reconfigures the capture scheduler and the backend), `EditorViewState view`, `requestedSceneExtent`}; `FrameOrchestrator::applyEditorFrameRequests` performs the three former writes verbatim; extraction reads only the `EditorViewState` PODs. **Deviations from the design, recorded:** `EditorFrameRequests` lives in `src/editor` and `EditorViewState` in `src/extraction` (the editor produces both and cannot include `src/app`; extraction cannot include the editor); the editor bridge stays owned by `Application` (it must outlive the backend; `EditorHost` uses it); the post-build preview re-resolution now precedes the three config writes (no shared state); the asset-preview sun update now runs just before `cpu.light.extract` instead of inside it (asset view only); the preview image fit is computed by `EditorHost` (`previewProjectionScale`); per-frame packets are extractor members released after `submitFrame`. **Evidence:** frozen set `r50`, `r5a1`, `r5a3d`, `r5a4` (`--validation-sync`) vs `r0` identical or within the depth-tie/woit-order envelopes, 0 validation messages, 0 hazards; digest identical to `r3a0` and `r3a0-ext` at `r50`, `r5a1`, `r5a3b`, `r5a3d`, `r5a4` (and `r3a0` at `r5a0`, `r5a2`); sweeps 36/36 at every step. Against `r4d-sweep-final` the deltas are the R5.0 range names, the run-root manifest path and the probe-promotion race; against `r50-sweep` only the race. **Probe-promotion race:** since R4c.3 runtime captures publish when `refreshCompletedSerial` sees the fence, so `probe.capture.*`, `probe.active`/`nonresident`, `probe.gpu_upload_*`, probe-route allocation counters and O03's GPU memory and `cpu.renderer.slot_swap` flip between builds and machine load on V02, O03 and R00–R03 (a same-binary repeat is identical; the baseline worktree build at `9f2a28e` already differs from `r4d-sweep-final`; images identical). **Regression found and fixed:** at R5a.3c–R5a.3d `vector = {}` (initializer-list assignment) kept the shadow-packet capacity and removed 1–3 steady-frame allocations on V12, V13, V15, V16, O01, O05 and X04; `1c3432b` frees them, and those entries are identical to `r50-sweep` again. **Unattributed figure** (one T-F7 profile, 300 + 1,500 frames, 512 detail frames, machine loaded by other lanes, so absolute times are indicative only): total minus top-level children 0.08 ms (was 1.77 ms); the sort.transparent → record.gbuffer gap is 2.54 ms here, 2.52 ms of it inside named scopes, led by `cpu.shadow.caster_revision.local` 1.14 ms, `cpu.render.prepare.depth_history` 1.07 ms and `cpu.render.transparent.intervals` 0.31 ms; `cpu.render.classify` 0.27 ms; `cpu.render.extract.parity` 0.19 ms of `cpu.render.extract` 0.68 ms. F7 has no directional shadow light, so its caster hashing is the local revision plus the depth history. `Application.cpp` 4,564 → 522 lines; no file above 2,500. Timing was not run (lead runs the pair at integration). | `out/m7r/captures/{r50,r5a1,r5a3d,r5a4}`, `digests/r5a*`, `sweeps/{r5a-base-sweep,r50-sweep,r5a*-sweep,r5a4fix-subset}`. |
| 2026-10-03 | R5c.6 (lane L, branch `r5l` from `f2cc899`): change-driven lights and probes (`c715a55`, `582e5af`, and the commit that records this entry). **Change source.** No component or writer revisions exist yet (the design's `SceneChangeSet` and `LightComponent`/`ReflectionProbeComponent` revisions need editor, harness and app writers), so change detection compares inputs: it is exact for every writer, including ones that would bypass a revision. **`LightExtractor`:** each light's packed candidate is memoized per light-pool dense index against every input `buildCandidate` reads (entity, persistent identity, all `LightComponent` fields bitwise behind a `sizeof == 52` assertion, world position bits, and the emission rotation chain walked exactly as before, up to 4 links; deeper chains rebuild every frame). Only lights whose inputs changed are rebuilt; failed builds are never memoized, so diagnostics always come from the full build. Existing owners reach their slot through a hint checked against `selectionMetadata_` (the owner-slot bijection) instead of the hash map; `writeRecord` still decides record changes. The removal walk runs only when a mapped owner went unclaimed, the active-list rebuild only after an insertion, removal or world reset, the type counts only when a record or membership changed. Diagnostics are assigned over persistent entries. **`ReflectionProbePublisher`:** each slot remembers the exact inputs its record and selection metadata were packed from (padding-free `PackInputs`, compared bitwise; cleared on clear, reset and growth) and skips re-packing a bit-identical probe; removal and active-list rebuild are gated as for lights. `extractReflectionProbes` gains an in-place overload over a caller-owned packet (the by-value form wraps it); probe transforms are still recomputed each frame (a handful of probes). **Light-route scratch** (the §1.6 sources that live in `renderer/lighting`, taken here rather than in R5c.8): in-place overloads of `selectDirectionalShadowLights` and `buildLocalShadowRequests`; member ranking/layout scratch in `StableSpotShadowAtlas`/`StablePointShadowPools` (the finished layout is swapped into `allocations_`); `LocalShadowCacheScheduler` keeps its ranking vector and a persistent schedule with a `scheduled_` flag instead of re-creating an optional. **`src/extraction` hunks** (lane X owns the file): `RenderExtractor.h` two members (`directionalShadowSelections_`, `localShadowRequests_`); `RenderExtractor.cpp` the two selection call sites, the in-place probe extraction call, and `releaseFrame` no longer resets `extractedProbes_` (it freed the packet every frame). **Steady allocations per frame** (sweep `r5-main-2-sweep` -> `r5l2-sweep`, last measured frame, calls/bytes): V12 34/704,738 -> 5/13,215; V13 19/97,914 -> 5/13,215; R00-R03 and V02 15/3,632 -> 6/2,072; X04, V15, V16 8/1,648 -> 2/1,088; X05 1/72 -> 0; O01 420 -> 409; O03 9 -> 6; O05 122 -> 120. **Remaining on these routes** (R5c.8, outside lane L): `RenderExtractor` locals `spotCacheInputs`, `pointCacheInputs`, `probeCaptureRequests`, `runtimeCaptureOwners`; the three shadow-packet vectors that `releaseFrame` swap-frees; `ReflectionProbeCaptureScheduler::schedule` (`renderer/rhi`). | `LightProbeReplayTests` against test-only `ReferenceLightExtractor` and `referenceExtractReflectionProbes`/`ReferenceReflectionProbePublisher` (the `f2cc899` code): 300 randomized light sequences (48,000 frames; three capacity policies incl. exhaustion; fields, transforms, parenting incl. cycles, identity, component/entity lifetime, world clears and switches) and 300 probe sequences (48,000 frames; environment table and residency changes, runtime capture slots), H-stress add_lights (110 frames) and H-probe (125 frames) shapes, all identical; 12 of 13 mutations caught (the uncaught one, skipping memo invalidation on world reset, is equivalent because the memo keys every input); zero steady-frame allocations in Release for extraction, publication and the shadow selection/scheduling path (Debug reports MSVC iterator-proxy allocations from the candidate sort). Temporary uncommitted engine patch running the references beside production: compressed H-stress and H-probe (1,200 frames each, 1,100 lights, 82 probes) and seven light/probe sweep routes, 0 differences. Frozen `r5l1` identical or within envelopes, validation and sync 0; digests `r5l1`/`r5l1-ext` identical to `r3a0`/`r3a0-ext`; sweep `r5l2-sweep` 36/36 pass, light and probe counters identical, only allocation counters fall (probe-capture publish counts vary run to run, as in `r5b1-sweep-probe1..3`). Release and Debug ctest pass apart from the three MAX_PATH process tests, which pass in a short directory, and `AssetRuntimeServiceTests`, which failed in 2 of 4 full `-j 8` runs (budget timing) and passes alone (8 of 8); it does not exercise lighting. No timing (lead). |
| 2026-10-03 | R5b.2-R5b.3 service migrations and starvation test (lane B, branch `r5bb` from `f2cc899`: `7debf37`, `96d754d`, `2947db5`, `794e294`, `ba40884`, `47e4f46`, `1cc1e90`, `45ec6ee`, `81b3053`, `daa276e`, `2c52daa`, `e79cf86`, `c26e303`, `3c731ed`, `525ac39` and the commit that records this entry). **Composition root:** `Application` owns the `TaskSystem` (constructed on the main thread before every unit, `shutdown()` in `cleanup` after `AssetIntegration::shutdown`); it reaches `FrameOrchestrator` (calls `tickPeriodic()` once per frame before the asset tick, and `recordFrameCounters` per profiled frame), `AssetIntegration`, `EditorHost` and every service by reference. Worker profiler streams are always prepared (capped at `MaxWorkerStreams`). Threads are named (`iridium.task.worker.N`, `iridium.task.io`) by an enkiTS thread-start hook. **New primitives (`iridium_tasks`):** `Periodic` (Background, Normal or pinned-I/O work restarted by the frame tick once its interval passed and its previous run completed; never overlaps; `stop()` unregisters and waits), `FunctionStrand` (callables on a `Strand`, one heap item per post, completed items reclaimed), `runOnPinnedIo`, `TaskSystem::current()`/`forCurrentThread()` (the live system when the calling thread is one it runs; library kernels use it and fall back to a serial loop). **Profiles (additive):** counters `allocation.cpp.background.calls/bytes`, `task.frame.count`, `task.frame.start_latency_us`, `task.background.active`; `worker_events`/`worker_ranges` and their aggregate-storage fields. O01 now records 318 of the 320 counter slots. **Per service (one commit each):** material preview: a Normal single-slot task (inline without a task system); menu bar: the orphan scan is a Normal `TaskSet` polled by completion (the panel destructor waits for a scan in flight, a single directory listing; a started Normal task cannot be cancelled); WIC: `CoIncrementMTAUsage` holds the MTA for the process instead of the parked owner thread, and every decoding thread still joins it through its thread-local `ComApartment` (no thread-start COM hook needed); watcher: a 250 ms `Periodic` on the pinned I/O thread, and `scanNow` copies paths, stats them without the mutex and applies results only to entries of the same generation (registration also stats before locking); source monitor: a 10 ms Background `Periodic`, and `SourceChangeTracker::poll` splits into `takeDue` / `hashDueSources` / `completePoll` so SHA-256 runs outside the monitor mutex (an asset untracked while hashing is skipped); catalog: a Background strand item per job, with `catalog->rebuild` on the pinned I/O thread; reimport: a Background strand item per queued request, per-item `stop_source` kept; model and environment preparation and thumbnails: strands (thumbnails was BELOW_NORMAL). **DDC:** each cook key in flight is a Background `CookTask` (de-duplication map kept); `request(..., DdcCompletion)` calls continuations after the future is ready, and `resolve()` cooks on the calling thread. *Rule as built:* no Background task blocks on a cook task. With work-stealing waits, a blocked strand item could run nested above the cook it waits for (deadlock), and under a small admission gate blocked waiters could hold every slot. So model and environment preparation finish in the cook's continuation (the 250 ms poll and its 5 s heartbeat log are gone; the strand moves on, so several cooks can be in flight), a thumbnail root resumes the drain from its continuation (roots still run one at a time), and hot reload cooks inline with `resolvePreparedCook` on the reimport strand (its DDC has no other requester). Shutdowns wait for in-flight continuations. Every `LocalDerivedDataCache` takes the task system; the cook tools construct one. **Kernels:** the glTF texture-view fork-join (up to 16 `jthread`s) is a Background `parallelFor` (joined under the gate's nested rule), the convolution's four `std::execution::par` loops are Background `parallelFor`s; results merge by index, so bytes do not depend on the schedule. No `std::thread`, `std::jthread`, `std::async` or `std::execution::par` remains in `src` outside `iridium_tasks`. **R5b.3:** a short functional starvation run at normal OS priority showed one B run with frame-period p95 44.7 ms and frame-task joins up to 44 ms: 22 cooking workers plus the main, reserved and driver threads oversubscribe the 32 hardware threads, so cooking preempted frame work. Background ranges now run at `THREAD_PRIORITY_BELOW_NORMAL` (restored per range, so a background waiter that helps with frame work runs it at normal priority; the main thread is never retuned; `TaskSystemConfig::lowerBackgroundOsPriority`). The rerun (`starvation/r5bb-starvation-smoke2`, T-F7, 20,000 frames, loaded machine) had B frame periods p95 3.6-3.7 ms against A 3.8-4.2 ms, frame-task worker start p99 34-35 us, and in-render Alfa cooks of 36.2-36.3 s against 37.4 s isolated. The harness flags `--qualification-background-cook PATH` (full cook with no DDC, one after another on a Background strand; `IRIDIUM_BACKGROUND_COOK` reports cooks, durations and hash agreement) and `--qualification-frame-task-probe` (a 32 x 20 us frame-critical `parallelFor` joined every frame; `IRIDIUM_FRAME_TASK_PROBE` reports worker start, join, frame period and slow frames caused by the join), plus `tools/m7r/Run-StarvationTest.ps1` / `Analyze-Starvation.py`, implement the section 4.2 criteria. Timing criteria are for the lead on a quiet machine: `powershell -NoProfile -ExecutionPolicy Bypass -File tools/m7r/Run-StarvationTest.ps1 -Label r5b3 -BaselineCookExe <pre-R5b.2 build>\out\build\x64-release\bin\IridiumCookAsset.exe`. **Flake:** `AssetRuntimeServiceTests` waited for the background reimport by iteration count (100 or 10,000 yields, 2,000 x 1 ms), so a loaded machine gave up before the preparation finished (`deferredByBudget == 1`, line 237, and the three-asset rebuild order). The waits are deadline-based (60 s); 16 concurrent runs passed. **Verification:** Release 110/110 after every commit (the three MAX_PATH cook process tests in a short directory; worktree fixtures, test assets and shaders rewritten as LF, Alfa hard-linked); Debug 110/110 (same short-directory rerun); shipping 103/103 (`CookSceneCliTests` short directory), hidden 120-frame `--validation-sync` 0 messages, `--benchmark` rejected, no harness flags in the binary (5.9 MB). Frozen `r5bb1` and `r5bb2` with synchronization validation: 0 messages, 0 hazards, identical to `r0` or within envelopes (F3-stress/F7-lod depth-tie, F4-woit woit-order). Digests `r5bb1-digest` and `r5bb2-digest` identical to `r3a0`. Sweeps `r5bb1-sweep` and `r5bb2-sweep` 36/36, 0 changed entries against `r5-main-2-sweep` apart from the new task and background-allocation fields (`-Ignore 'profile\.counters\.task\.','profile\.counters\.allocation\.cpp\.background\.','profile\.summary\.aggregate_storage\.(worker_|unaggregated_worker)'`); main-thread allocation counters unchanged, background allocations 0 in every deterministic entry. `tools/m7r/Verify-CookedArtifacts.ps1`: cold cooks of all seven frozen models (Alfa, Alfa LOD, contact, ordinary2, cine8, WOIT, Belfast environment) give the recorded cook keys and artifact hashes. Scripted hot reload (an artifact revision, a source edit and its restore, `--validation-sync`): 3 rebuilds, 3 publications, 0 failures, 0 validation messages, the source recook in the hot-reload DDC. Editor and asset-viewer (`019fe593-65be-799f-8a19-1bac0f4a3612`, cooked in the run) smokes exit 0 with 0 messages. Thread inventory (`tools/m7r/List-EngineThreads.ps1`, editor run): before, 8 unnamed CRT threads (catalog, model, environment, thumbnails, editor DDC, reimport, monitor, watcher; hot-reload DDC, material preview and WIC start on demand); after, 31 named task-system threads and no other CRT thread. No timing pairs or hitch runs. | Lane B report; `captures/r5bb1`, `captures/r5bb2`, `digests/r5bb*-digest`, `sweeps/r5bb*-sweep`, `starvation/r5bb-starvation-smoke*`. |
| 2026-10-03 | R5c.3 call sites (lane X, branch `r5x` from `f2cc899`: `b213aec`). `RenderExtractor` owns one `CompactDrawSortScratch` beside the queues; the opaque and forward-opaque queues use `sortOpaqueDrawPackets`, the compatibility transparent queue `sortTransparentCompatibilityDrawPackets` and the sorted-surface queue `sortTransparentWorkDrawPackets`; scopes unchanged. **Evidence:** frozen `r5x1` (`--validation-sync`) identical to `r0` or within the depth-tie/woit-order envelopes, 0 messages, 0 hazards; digests `r5x1`/`r5x1-ext` identical to `r3a0`/`r3a0-ext`; sweep `r5x1-sweep` 36/36 against `r5-main-2-sweep`: deltas are first-frame allocations only (+1 to +3 calls for the key vectors, `sum` delta equal to `first` delta) and the known probe-promotion race on R01; editor smoke 0. | `CompactDrawSortTests`; captures `r5x1`; digests `r5x1`; sweep `r5x1-sweep`. |
| 2026-10-03 | R5c.5 remainder (lane X: `2ee9d9c`). **(a) Membership revisions without hashing.** `publishGpuSceneConsumerMembership` keeps the previous publication's membership inputs in a per-table `GpuSceneMembershipHistory` (lists, primitive records, instance state words, geometry records and revisions) and advances each list's revision from one shared per-table counter (starting at 1; shadow and probe never share a value) exactly when a value the retired FNV-1a read differs: epoch, list, per member binding/state x,y,w/revisions, instance state x,z,w, LOD chain draw/storage/state x,z,w/record revision (memoized per instance and chain head; snapshot refreshed only after a change). Change relation: identical frame by frame (A to B to A changes both twice); values are unique only within one table's lifetime, which every consumer satisfies (one publisher per extractor). The retired hash stays as `qualification/LegacyGpuSceneMembershipHash.h` for the oracle and tests. **(b) Change-driven observation.** The observation moves to `extraction/GpuSceneObservation`. The full walk (the former code, const access) runs on structural change; otherwise only the pool runs (the entities processed at one observation slot) of changed entities are re-run with the same per-entity step. Change sources: a new opt-in `ComponentWriteJournal` on `ComponentPool` (non-const `get`, `getVoid`, replacing `add` record the entity; new `add`, `remove`, `clear` advance `structureRevision`), the transform system's changed-entity journal (now passed to the extractor), old/new selection, entities with material overrides (their bindings come from the asset manager) and volatile runs: the full walk lets an invalid-metadata entity overwrite its slot's metadata, so its successor rebuilds every frame, and that is reproduced. Full-walk triggers: mesh/transform/batch pool pointer or structure revision, scene epoch, journal overflow, a referenced model whose geometry, cook key or GUID changed in place or expired, a visited entity whose model pointer or override presence changed, and a run whose kinds would change (checked by a side-effect-free simulation before any state is touched). Observations, revisions, metadata and the fallback count are therefore the full walk's. **ExtractionVerifier:** `--qualification-extraction-verifier` (qualification builds) runs a full-walk instance beside it and compares every field every frame, failing on a difference. **src/app edits (small, local):** the transform journal argument (`FrameOrchestrator`), the verifier routing flag (`FrameObserver.h`, `Application.cpp`), and a const read before the `ProcessMeshSwaps` mesh walk's mutable access (`AssetIntegration`; otherwise every mesh entity is journaled every frame). **Evidence:** `GpuSceneTests` randomized membership relation (12,160 comparisons), replay test relation on every publication, `GpuSceneObservationTests` (48 x 400 randomized edit frames against the full walk; a journal-bypassing write is caught); caster-revision oracle `membership` stream and the verifier: 0 divergences on the frozen set, all 36 sweep entries, compressed H-stress/H-probe (3,500 frames each) and the Hi-Z motion fixtures; frozen `r5x2` identical or within envelopes, sync 0; digests identical; sweep `r5x2-sweep`: first-frame allocation deltas only plus one one-time allocation in a later frame (`runStamp_`/journal growth). Scale replay (`--scale`, noisy): `add_instances` 512 synchronize 54.6 to 15.3 ms, `select`/`deselect`/first `move one` 40 to 2 ms. Untimed H-stress (noisy, machine shared): synchronize max 123-128 ms (B runs) to 19.1 ms, observe median 155 to 1.3 us. | `GpuSceneTests`, `GpuScenePublisherReplayTests`, `GpuSceneObservationTests`; captures `r5x2`, `r5x2-verify`; sweep `r5x2-sweep`; lane scratch oracle/verifier runs. |
| 2026-10-03 | R5c.4a-f parity retirement (lane X: `ecfcf2a` a, `4989c35` b/d, `fe4085e` c, `f3f976c` e, `be022fe` f). **(a)** `mainOpaqueConsumerPrimitiveIndices` (MainOpaque primitives of enabled instances, ascending) with its membership revision (R5c.5 rule) and content watermark. **(b)** `RenderFrame::opaque` is an `OpaqueSubmission`: the draw order (one `std::sort` with the opaque comparator over the direct packets in append order followed by the main-opaque primitives ascending, i.e. the parity queue's input, so the permutation is identical), the direct packets in that order, the membership revision and the view's CPU visibility. The GPU-scene order is cached per main-opaque membership revision. No packet is built per main-opaque primitive; packets remain for visible forward-opaque primitives, the selection and the direct probe-capture reference routes (identical to the parity packets). The culler plans from the records (`buildGpuSceneIndirectPlan` over primitive indices: same commands, candidates, bins and fallback reasons); the direct-fallback and wireframe loops resolve GPU-scene entries from the slot's records. **(c)** With GPU-scene work, direct packets are drawn directly at their place in the draw order between indirect bins (a direct entry ends a bin; bind state reset between pipeline families) instead of dropping the queue to the direct fallback; no frozen or sweep fixture holds both kinds (`opaque.indirect.fallback_reason` unchanged everywhere). **(d)** The depth-history revision and the oracle's retired hash read the submission (GPU-scene contents resolved from the records). **(e)** The selection outline comes from the instance selection flag (selected instances' visible primitives, dense order). Not done, for a telemetry decision: restricting the frustum classification to forward-opaque plus the selected instance and moving the bin expectations to the qualification oracle. MainOpaque visibility still feeds production counters (`gpu_scene.visibility.*`, `opaque.primitive.visible/frustum_rejected`, `opaque.triangle.visible`, `draw.recorded.opaque`, `opaque.indirect.command_count`, `gpu_scene.visibility.oracle_commands`, `device_mismatched_bins`) and the direct-fallback draw filter, so the change alters those counters in every GPU-scene run; `cpu.render.classify` is about 0.26 ms of T-F7 (untimed). **(f)** A fixed wireframe variant with the GPU-scene vertex shader; wireframe frames draw the same bins with `vkCmdDrawIndexedIndirect` over host-written plan commands (all primitives, no culling or validation); X04 byte-identical to the base build and clean under synchronization validation (no envelope), `opaque.indirect.bin_count` now recorded in wireframe frames (X04 0 to 3). **Evidence per sub-step:** frozen `r5x4a/b/c/e/f` identical or within envelopes, sync 0; digests identical to `r3a0`/`r3a0-ext` at every step (candidate host bytes); sweeps `r5x4*-sweep` 36/36: first-frame allocation deltas only (smaller first-frame bytes without the parity queue) and the probe race, plus X04 `bin_count` at (f); oracle and verifier 0 divergences on the sweep, H-stress/H-probe and the Hi-Z motion fixtures (`depth_history` 1 change each, as R5c.2); record-plan equivalence and mixed-bin tests. Untimed indications (noisy, shared machine): T-F7 `sort.opaque` 0.22 to 0.002 ms, `extract.parity` 0.19 to 0.075 ms; H-stress steady frame median 31.8 to 17.3 ms (`sort.opaque` 5.9 to 0.04 ms, `extract` 7.3 to 4.8 ms). | `GpuSceneIndirectTests`, `VulkanIndirectCullerTests`, `VulkanCasterRevisionTests`; captures `r5x4a`-`r5x4f`; sweeps `r5x4*-sweep`. |
| 2026-10-04 | R5c.4e remainder not done: the main-opaque CPU frustum classification stays whole, and its bin expectations are not moved to the qualification oracle. It feeds production telemetry (`gpu_scene.visibility.*`, `opaque.primitive.visible`/`frustum_rejected`, `opaque.triangle.visible`, `draw.recorded.opaque`, `opaque.indirect.command_count`, the oracle command/mismatch counters) and the direct-fallback draw filter. Restricting it would save about 0.23 ms on T-F7 but blank those counters in every run without an oracle. F7 already meets the ≤ 3.0 ms target. Revisit when GPU-driven visibility replaces CPU classification. | Lead decision (keeps telemetry; target already met). |
| 2026-10-02 | Async compute and `/W4` are out of scope. The editor and importer files over 2,500 lines are deferred exceptions. | Scope control. |
| 2026-10-02 | ADR-0016 will record the actual executor model, because ADR-0002's claims about queue, history and imported resources are unimplemented. | Graph audit. |
| 2026-10-02 | Owner approved removing `--developer-legacy-transparency` in R2. It is a post-M6 diagnostic A/B, never an automatic fallback (ADR-0012). It costs 3 graph passes, extra frame targets and 11 backend references, and complicates the R3 transparency owner. Serialized `LegacyTwoBucket` stays readable. | ADR-0012:94–113 |

### Third-party library decisions

| Library | Status | Pin |
|---|---|---|
| VMA (MIT) | Explained by the director; adopted in R4b.1 behind `VulkanResourceAllocator` (FetchContent, header-only, one implementation TU; never vendored). | **v3.4.0** (`3aa9212`, the latest stable release on 2026-10-03; the design expected v3.3.0). |
| enkiTS (zlib) | **Adopted in R5b.0/R5b.1 (2026-10-03)** behind `Iridium::Tasks::TaskSystem` (FetchContent, static library, linked privately into `iridium_tasks`; never vendored). Approved by the owner 2026-10-03 for the R5b task system: fixed worker pool, task sets with range splitting and work stealing, priorities, pinned tasks, waits from inside tasks, and no allocation after init. Taskflow 4.1.0 (MIT) was rejected: it has no released priorities or pinned tasks and allocates a node per task (`M7R-R5-cpu-frame-design.md`). ADR-0015 records the scheduling model. | **v1.12** (`0289cf6`, 2026-07-04). |
| Tracy (BSD-3) | Deferred (2026-10-03); per-worker profiler scopes cover R5 attribution. | Not adopted. |

Any other library needs an explanation to the owner first.

## Evidence

### R0 baseline (2026-10-02, commit `2c50b36`)

**Environment and inputs**

| Item | Value |
|---|---|
| Hardware | RTX 4090, i9-14900K, Windows 10.0.26220 |
| Toolchain | MSVC 19.51, Vulkan SDK 1.4.335 |
| Release `IridiumEngine.exe` SHA-256 | `253bfe0a…` |
| Raw artifacts | `out/m7r/` (local only). Captures: `captures/r0` and `captures/r0-repeat`. Timing: `timing/r0-aa`. Build times: `build-times/r0-*.json`. |
| Baseline worktree | `out/m7r/worktrees/r0`, used as side A of every later timing pair |

Cooked inputs in `out/m7r/ddc/artifacts.json` (cook-key prefixes):

| Model | Cook key |
|---|---|
| alfa | `a5431400` |
| alfa-lod | `de6c732f` |
| contact | `ba56a26c` |
| ordinary2 | `926155db` |
| cine8 | `58ca6f81` |
| woit | `d31b89ce` |

**Frozen captures** (native 4K, Release, SDR, ACES 2, 0 EV; SHA-256 prefix; full hashes are in `captures/r0/hashes.json`)

| Fixture | Scene-linear PFM | Final-SDR TGA | Tolerance |
|---|---|---|---|
| F1-all | `d84fcb0d5b072598` | `eac6ee9364ce5d28` | exact |
| F2-one | `c3d77c9e69a4664f` | `b6363dff7ef41d80` | exact |
| F3-stress | `94e7822d77dc1f64` | `931fbdd2b624f7f2` | depth-tie |
| F4-ord2 | `9ae03606a1d63f62` | `5620cf982001b9a3` | exact |
| F4-cine8 | `18f090fdcd5b5d07` | `da270d13e67f5992` | exact |
| F4-woit | `e9ca4c75490bb73b` | `fed121187d086b6b` | woit-order |
| F5-hetero | `a522245d3ffddb8d` | `7e28ab5b55d40a2b` | exact |
| F5-point | `015da6da08efc82d` | `0a5ae0fb1d4abab6` | exact |
| F6-probe | `af8d80c0adc54f0f` | `445386281f9384c0` | exact |
| F7-hiz | `fa9076ebb4c6f69a` | `83ba524a95b5523c` | exact |
| F7-lod | `44965178cfa42171` | `25ac64369f55d0b9` | depth-tie |
| F6-probecap (added in R2) | `d9029d5b3df2c721` | `0f1a7fe448a4ca18` | exact |

Every capture passed `--require-capture-signal`. A Release run with `--validation` over the whole set produced **zero** validation messages, with images identical or within tolerance.

**Determinism.** Eight F3/F7-lod runs and eight F4-woit runs were compared.

| Fixture | Run-to-run behavior | Likely cause | Envelope |
|---|---|---|---|
| F3, F7-lod (256 instances) | Rotate among four scene-linear and two final-SDR hashes. Each differs by **one pixel** (x = 1320). | A depth tie between distant instances whose draw order follows GPU compaction order. | `depth-tie`: at most 64 changed pixels |
| F4-woit | Differs every run: about 1% of pixels, max abs AP1 0.031, max rel 0.0032; final-SDR at most 1 code on about 0.04% of pixels. | Order-dependent accumulation. | `woit-order`: the frozen M6.7 draw-order thresholds (0.0625 abs, 0.002 RMSE, 0.005 rel; SDR at most 1 code on at most 0.5% of pixels) |

This variation is pre-existing and recorded, not fixed in M7R. A deterministic visibility and draw order is a candidate for the R3a culler, needing owner approval because it changes images.

**Timing (A/A pair, two identical builds, A,B,B,A)**

Settings: native 3840x2160, hidden borderless window, mailbox present, SDR, 500 warm-up / 10,000 measured frames. Values are in ms. "Non-wait CPU" is frame total minus fence wait, acquire and present.

| Route | CPU frame median (A / B) | Non-wait CPU median (A / B) | GPU median (A / B) | GPU p99 | Alloc calls per frame | GPU drains per frame |
|---|---|---|---|---|---|---|
| T-F1-all | 3.290 / 3.280 | 0.645 / 0.654 | 1.171 / 1.171 | 1.45 | 0 | 0 drain, 0 upload-wait |
| T-F7-stack | 9.838 / 9.576 | 6.122 / 5.968 | 2.099 / 2.100 | 2.66 | 0 | 0 / 0 |

**Noise band** (from the A/A spread):
- CPU about ±1.5% on F1 and ±2.6% on F7. The first cold process accounts for most of the F7 spread.
- GPU ±0.3%.

A regression must exceed this band in both orders.

**Allocations.** Steady frames are **allocation-free** on both timing routes at R0. The audit's 2 and 16 calls per frame came from older or dirty-tree runs on other routes. The invariant is therefore zero allocations on the timing routes from R0 onward.

**CPU on F7** (dense depth stack: 64 instances, 7,232 primitives, 3,904 transparent packets, 41,920 ambiguous intervals)
- Non-wait CPU is **about 6.0 ms, twice the 3.0 ms target**.
- `cpu.renderer.present` takes 3.54 ms but is excluded as a wait.
- The largest named stages are extract 0.70 ms, forward record 0.45, transparent sort 0.39, opaque sort 0.34 and G-buffer record 0.24 ms.
- **About 3.6 ms of non-wait CPU is in no named scope.** R5 attributes it first.

**Build times** (14900K, Ninja, clean worktree)

| Step | Release | Debug |
|---|---:|---:|
| Configure (clean, includes FetchContent clones) | 53.7 s | 53.3 s |
| Clean build (664 Ninja steps) | 109.5 s | 90.8 s |
| No-op build | 0.2 s | 0.2 s |
| Touch `Application.cpp` (2 steps) | 10.9 s | 8.6 s |
| Touch `rhi/Mesh.h` (108 steps) | 36.6 s | 20.7 s |
| Touch `shadow_filter.glsl` (69 shader steps) | 0.9 s | 1.1 s |
| Touch `cluster_count.comp` (1 step) | 0.3 s | 0.3 s |

Notes:
- *Correction (R1):* the R0 worktree was configured cleanly with `/Zi /Ob0 /Od /RTC1`, so these are true Debug numbers. The empty Debug flags were a stale cache in the main checkout only, caused by a configure outside the MSVC environment. R1 adds a guard that restores the defaults.
- Shader include fan-out (all 69 shaders) is cheap in wall time, but R1 still adds depfiles.

**Tests.** Debug 78/78 and Release 78/78 pass at `2c50b36`.

### R1 result (2026-10-02, commits `7781b03`, `6fcd762`, follow-up)

**Code generation**
- Release objects: 191/191 have identical disassembly (`dumpbin`, normalized), across 234,743 functions. The only exception is the configure-time commit-hash strings in `Application.cpp`.
- SPIR-V: all 69 `.spv` files are byte-identical.
- Effective defines: no difference for any production TU.

**Compilations**
- 191 production sources and 191 compilations, down from 339. No source compiles twice.

**Frozen set** (`captures/r1` against `captures/r0`)
- 16/22 captures are byte-identical.
- F3, F7-lod (1 pixel each) and F4-woit are within their R0 envelopes.
- Zero validation messages.

**Tests**
- Debug and Release both pass 80/80: the 78 previous tests plus `VulkanDepthPyramidTests` and `VulkanVirtualShadowMarkingTests`, now registered.

**Build times** (clean worktree, 14900K)

| Step | R0 Release | R1 Release | R0 Debug | R1 Debug |
|---|---:|---:|---:|---:|
| Configure (clean) | 53.7 s | 32.2 s | 53.3 s | 32.7 s |
| Clean build | 109.5 s | **61.3 s** | 90.8 s | **48.1 s** |
| Touch `Application.cpp` | 10.9 s | 8.7 s | 8.6 s | 8.4 s |
| Touch `rhi/Mesh.h` | 36.6 s | **16.2 s** | 20.7 s | 13.7 s |
| Touch `shadow_filter.glsl` | 0.9 s (69 shaders) | 0.4 s (10 steps) | 1.1 s | 0.4 s |
| Touch `cluster_count.comp` | 0.3 s | 0.3 s | 0.3 s | 0.3 s |

**Timing pair** (`timing/r1`, A = R0 worktree, B = R1)
- The owner was using the machine during the run (League client, OP.GG, OneDrive sync; about 32% CPU load).
- Both sides were noisy. The unchanged baseline's F1 GPU median ranged from 1.25 to 1.48 ms, against 1.17 ms at R0.
- Steady-frame allocations were 0 and drains 0 on both routes.
- Because Release codegen is proven identical, the code-identity proof is the primary R1 performance evidence. A rerun is recorded below.

Rerun (`timing/r1-rerun`, A,B,B,A; the machine was still in use):

| Route | Side | Non-wait CPU median | GPU median |
|---|---|---:|---:|
| T-F1-all | A | 0.644 ms | 1.505 ms |
| T-F1-all | B | 0.622 ms | 1.331 ms |
| T-F7-stack | A | 6.346 ms | 2.219 ms |
| T-F7-stack | B | 6.275 ms | 2.231 ms |

- No regression beyond noise; B is equal or better on non-wait CPU.
- GPU on F7 is +0.5%, inside the band.
- Allocations 0 and drains 0 on both routes.
- Timing pairs need an idle machine; later slices note the machine state.

### R2 result (2026-10-02, through `8e2fae7` + frozen-parser retirement)

**Frozen set:**
- `captures/r2-10` and `captures/r2-10-validation` against `r0`: every capture is byte-identical or within its R0 envelope (F3/F7-lod one depth-tie pixel; F4-woit order).
- Zero validation messages on all 24 captures.
- F6-probecap is byte-identical.

**Qualification sweep:**
- `r2-10-sweep` passes 36/36 and shows 0 changed entries against `r2-9-sweep`.
- Over R2, every difference from `r2-head-7b89df9` is explained:
  - R2.5 oracle-off metadata;
  - lane B's allocation removals (the occlusion oracle's 2 calls / 13,006 B per frame, and 46 shadow-oracle calls per frame).

**Tests:**
- Release and Debug pass 92/92, including the ABI, depfile, graph-contract, headless-pipeline and GPU-parity tests, the harness and observer tests, and the oracle process tests.
- The shipping preset passes 86/86.

**Shipping build (`x64-release-shipping`):**
- A hidden 120-frame run with `--validation` exits 0 with 0 messages.
- `--benchmark` is rejected as an unknown option.
- `IRIDIUM_ORDINARY2_CAPTURE_VALIDATION`, `qualification-oracle`, `--benchmark` and `IRIDIUM_CAPTURE` are absent from the binary.
- The exe is 5.91 MB, against 6.30 MB for the qualification build.

**Timing pair** (`timing/r2`, A = R0 worktree, B = R2, A,B,B,A; the owner's League client was running, about 16% CPU)

| Route | Non-wait CPU median A / B (mean of 2) | GPU median A / B |
|---|---|---|
| T-F1-all | 0.658 / 0.680 ms | 1.372 / 1.215 ms |
| T-F7-stack | 6.407 / 5.689 ms | 2.293 / 2.239 ms |

- The noise is large. A's first F1 run had a 24 ms p95 spike from outside load, and B's F7 runs were 5.03 and 6.35 ms.
- No route regresses in both orders: F1 B run 3 equals A, and F7 B run 2 is better.
- Allocations are 0 and drains 0 on every run.
- R2 removes work (oracles, readbacks) from the frame and adds none, so a real regression is implausible.

**R2.10 cleanup:** the frozen 6b000ad parser copy and the parity corpus are retired. The per-flag table keeps every flag's config, implication and exact-message contract.

### R3 result (2026-10-03, `bd89494`)

**Structure**
- **Executor (ADR-0016, accepted):** index-addressed and drives callbacks; one `vkCmdPipelineBarrier2` per pass; History, imported-image policies and `variableSize` imports.
- **Passes:** 59 of the production passes plus `ui` run as registered feature-owner callbacks.
- **Feature owners:** `VulkanClusterLightingFeature`, `VulkanOutputFeature`, `VulkanWeightedOitFeature`, `VulkanHookPasses`, `VulkanShadowFeature`, `VulkanLocalShadowFeature`, `VulkanReflectionProbeFeature`, `VulkanOpaqueFeature`, `VulkanDeferredLightingFeature`, `VulkanForwardFeature`, `VulkanLayeredTransparencyFeature` and `VulkanUiFeature`.
- **Shared culling:** `VulkanIndirectViewCuller` plus `VulkanOpaqueIndirectCuller` replace five duplicated culler paths.
- **Editor UI:** lives in `iridium_vulkan_imgui` behind `IEditorRenderBridge`; `iridium_vulkan` no longer links ImGui.
- **`IRenderBackend`:** 67 → 37 methods. Frame work goes through `submitFrame(const RenderFrame&)`.
- **Size:** `VulkanVertexBackend.cpp` 13,286 → **2,073** lines.

**Verification**

| Check | Result |
|---|---|
| Frozen set `r3` with `--validation-sync`, vs `r0` | Identical or within envelopes; **0 hazards**, 0 validation messages |
| Pre-existing local-shadow READ_AFTER_WRITE hazard | Fixed in R3b.6 (spot-atlas and point-pool render-pass dependencies) |
| Indirect digest vs `r3a0` | Identical |
| Sweep `r3-sweep` vs `r3c11-sweep` | 0 changed entries |
| Tests | 97/97 Release and Debug; shipping preset 91/91 |

**Timing pair** (`timing/r3`, A = R0 worktree, B = R3, A,B,B,A; machine idle, 0% CPU load)

| Route | CPU frame median A / B | Non-wait CPU median A / B | GPU median A / B | GPU p99 A / B |
|---|---|---|---|---|
| T-F1-all | 1.536 / 1.478 ms | 0.417 / **0.371** ms (−11%) | 1.1204 / 1.1235 ms | 1.120–1.135 / 1.132–1.133 |
| T-F7-stack | 6.978 / 5.558 ms | 4.958 / **3.539** ms (−29%) | 2.0442 / 2.0558 ms (+0.57%) | 2.056–2.069 / 2.070–2.073 |

- **CPU:** improves strongly, from O(1) graph lookups, batched barriers and less per-pass bookkeeping. Allocations and drains are 0 on every run.
- **GPU:** +0.28% on F1 and +0.57% (+11.6 µs) on F7, consistent in both orders. The F7 delta is slightly above the ±0.3% R0 noise band.
  - Per pass it is spread inside draw-heavy passes: forward-opaque +4.6 µs, deferred lighting +2.6 µs, G-buffer +2.0 µs. Barrier and compaction ranges are unchanged, and the command-stream digest is identical.
  - The likely cause is memory placement from the changed resource-creation order, not extra GPU work.
- **Accepted as a watch item:** R4b re-places all memory (VMA), and R4b and R6 re-measure. If the delta persists after R4b, bisect by pass ownership.

### R4 result (2026-10-03, R4a–R4c through `c10c182`, R4d through `1edaab6`)

**Structure**
- **Dynamic rendering (R4a):** 0 `vkCreateRenderPass`/`vkCreateFramebuffer` calls outside `src/vendor`. Pipelines use `VkPipelineRenderingCreateInfo`. The swapchain and shadow maps are `ExecutorOwned` imports (ADR-0016 item 4 note).
- **VMA v3.4.0 (R4b.1–2):** behind `VulkanResourceAllocator`, with the 19 memory-profile categories preserved.
- **Transient aliasing (R4b.3–6):** on by default; `--render-graph-aliasing off` is kept until R6 (ADR-0016 item 9 note).
- **Deferred deletion (R4c.1–3):** a fence-keyed deletion queue, capacity growth by slot-retirement swap, and probe changes that no longer drain.
- **Pipeline cache (R4c.4):** a persisted `VkPipelineCache` with a validated header; `--pipeline-cache DIR|off`. Evidence scripts default to `off`.

**Memory** (native 4K, default SDR, pyramids resident; render-graph committed bytes, both frame slots)

| | Aliasing off | Aliasing on |
|---|---|---|
| Graph committed | 1,183.6 MB | **842.0 MB** (−341.6 MB) |
| Per slot | | 571.9 MB requested → 401.1 MB in one alias heap (1.43x); 10 aliased images |

**Pipeline cache** (F1-all, backend init; the payload is about 2.51 MB)

| Driver shader cache | off | cold | warm |
|---|---|---|---|
| Warm (normal) | 336–338 ms | 380–402 ms | 342–349 ms |
| Disabled | 392–422 ms | 374–376 ms | 349–359 ms |

A warm cache saves time only when the driver's own cache is cold, for example on first run or after a driver update.

**Verification** (main at `c10c182`)

| Check | Result |
|---|---|
| Frozen set `r4b-main`, `--validation-sync`, vs `r0` | Identical or within envelopes; 0 hazards, 0 validation messages |
| Indirect digest `r4b-main-digest` vs `r3a0` | Identical |
| Sweep `r4b-main-sweep` | 36/36. Deltas: pipeline-cache header fields, the manifest path, R00 probe publish count (varies run to run), X02 within the woit-order envelope |
| Probe sweep routes (V02, R00–R03) | Capture moved to frame 4. Since R4c.3, frame 2 can race the fence-gated probe promotion. The images equal `r3-sweep`; 3/3 repeat runs are identical |
| Aliasing gates (lane) | Frozen set with aliasing on, and with alias poison, identical; editor smoke with sdr/hdr10/scrgb clean; shipping smoke clean |
| Tests | 103/103 Release and Debug |

**Timing pair** (`timing/r4-accept`, A = R0 worktree, B = `c10c182`, A,B,B,A, 500 + 10,000 frames, quiet machine, steady-frame allocations 0)

| Route | CPU median A / B | Non-wait CPU median A / B | GPU median A / B |
|---|---|---|---|
| T-F1-all | 1.540 / 1.347 ms (−12.5%) | 0.418 / 0.348 ms (−16.8%) | 1.126 / **1.003** ms (−11.0%) |
| T-F7-stack | 6.968 / 5.382 ms (−22.8%) | 4.957 / 3.495 ms (−29.5%) | 2.034 / **1.918** ms (−5.7%) |

This pair's GPU gain is not stable across machine states (see "Final R4 timing" below). Aliasing alone accounts for −6.4% (F1) and −3.2% (F7) in its on/off pair (`timing/r4b6-onoff-short`), and most of that is one pass, `gpu.transparency.refraction-pyramids` (0.197 → 0.130 ms).

**Hitch scenario** (`hitch/r4-accept-hitch`, A = `c666d32` (R4b.3, before R4c.1), B = `c10c182`, A,B,B,A, quiet machine)

| Route | Side | Median ms | p99 ms | Drain frames | Hitches (over 2x the run median) |
|---|---|---|---|---|---|
| H-stress | A | 46.02 | 48.82 | 8 | 4.5 |
| H-stress | B | 45.81 | 48.60 | **0** | 4.0 |
| H-probe | A | 4.09 | 8.38 | 272 | 222.5 |
| H-probe | B | 4.10 | **7.28** | **0** | **84** |

- The remaining H-stress event costs (85–200 ms at `add_instances`) are CPU work in extraction and sorting, not GPU drains. They go to R5 (change-driven extraction).
- **Watch item, intermittent spike:**
  - A single-frame 0.7–1.2 s spike lands on a scripted-event frame in 3 of 14 H-stress runs of post-R4c builds: `add_lights` at 1500 (894 ms), `add_instances` at 3500 (1,155 ms, aliasing off) and `add_lights` at 8500 (668 ms).
  - It occurs with aliasing on and off. It was not seen in 2 runs of the pre-R4c baseline, so the evidence is too thin to bisect.
  - The profiler's detailed window (512 frames) did not cover those frames. The harness now records scope detail for any frame of 250 ms or more (`b360e3a`, `scripted_slow_frame`), and 4 further runs did not reproduce it.
  - R4d and R6 hitch runs will attribute it if it recurs.

**R4d** (transfer queue, timeline semaphores, staging ring, `vkQueueSubmit2`; details in the decision log)
- **Upload path:** the RTX 4090 uses queue family 1, a dedicated transfer queue. `beginFrame` no longer waits on uploads. Upload-wait frames are 0 on every hitch route.
- **Fence serialization fixed:** the per-image fence map had made every frame wait for the previous frame's GPU work, so CPU and GPU never overlapped. Per-image timeline serials took the T-F7 image-owner wait from 1.97 ms to 0.003 ms.
- **Texture-table growth bug fixed:** growing the table destroyed a descriptor pool still in use. It was pre-existing, exposed by the new `add_textures` H-upload route.
- **Verification (main at `1edaab6`):**
  - frozen set `r4d-main` with `--validation-sync`: identical or within envelopes, 0 hazards, 0 validation messages (the lane also ran `graphics` and `legacy-blocking` modes);
  - digest identical to `r3a0`;
  - sweep 36/36 (lane);
  - tests 105/105 in Release and Debug.

| Hitch (`hitch/r4d-hitch`, A = `b360e3a`, B = R4d) | Median ms A / B | p99 ms A / B | Upload-wait frames A / B |
|---|---|---|---|
| H-stress | 46.05 / 39.58 | 47.49 / 41.05 | 1 / 0 |
| H-probe | 4.10 / 3.70 | 7.30 / 7.00 | 0 / 0 |
| H-upload (`legacy-blocking` / `auto`) | 1.13 / 1.13 | 1.30 / 1.29 | 5 / 0 |

**Final R4 timing**
- The two late full pairs against R0 (`timing/r4-final`, `r4-final-2`) ran in a slower machine state:
  - R0's own F7 non-wait CPU was 5.77 ms against 4.96 ms in the quiet pair, and its UI pass rose from 0.060 to 0.071 ms.
  - Desktop applications were open (game launchers, an overlay, Discord, Settings).
  - `r4-final` is discarded: its R0 runs varied up to 9.2 ms. `r4-final-2` is internally consistent and serves as the matched pair for that state.
- In this state, the refraction-pyramid pass of aliased builds runs at 0.205 ms instead of 0.130, the same as R0, with an identical graph and alias plan. The cause is environmental (memory placement or clock state) and was not identified.
- A same-state control (`timing/r4d-vs-pre`, A = `c10c182` rebuilt, B = `1edaab6`, 2,000 frames) shows R4d itself regresses nothing:

| Route | CPU frame A / B | Non-wait CPU A / B | GPU A / B |
|---|---|---|---|
| T-F1-all | 1.677 / 1.125 ms | 0.557 / 0.590 ms | 1.144 / 1.135 ms |
| T-F7-stack | 7.366 / 5.265 ms | 5.336 / 5.153 ms | 2.052 / 2.041 ms |

R0 vs R4 (`timing/r4-final-2`, A = R0, B = `1edaab6`, A,B,B,A, 10,000 frames, slower state; steady-frame allocations 0):

| Route | CPU frame median A / B | Non-wait CPU A / B | GPU median A / B |
|---|---|---|---|
| T-F1-all | 1.648 / **1.138** ms (−31%) | 0.533 / 0.516 ms | 1.141 / 1.147 ms (+0.5%; one B run at 1.157, the other 1.136) |
| T-F7-stack | 7.902 / **5.260** ms (−33%) | 5.772 / 5.111 ms (−11%) | 2.057 / 2.060 ms (+0.1%) |

**R3 GPU watch item:**
- In the quiet state R4 is 5.7–11% faster on GPU than R0.
- In the slower state it is within +0.1–0.5% of R0, inside or at the R0 noise band (±0.3%).
- The watch item therefore stays open, with the refraction-pyramid placement sensitivity named as its main component. R6 re-measures on a verified-quiet machine and records the machine state with the run.

**Acceptance.** The R4 completion criteria are met:
- hitch and p99 improve on the scripted-change runs: drains 8→0 and 272→0, upload waits →0, H-probe p99 8.4→7.0 ms, H-stress median −14% through R4d;
- VRAM: −341.6 MB of graph memory at 4K, +64 MiB staging ring;
- synchronization validation clean on the frozen set in every upload mode;
- no timing regression beyond noise in either machine state.

### R5 interim (2026-10-03, wave 1 at `f2cc899`: R5.0, R5a, R5b.0–1, R5c.1–2, lane P's R5c.3/R5c.5 library side)

- **Structure:**
  - `Application.cpp`: 4,564 → 476 lines (composition root).
  - `FrameOrchestrator`, `AssetIntegration`, `EditorHost` and `RenderExtractor` (new library `iridium_render_extraction`, with a link guard against ImGui, the editor and GLFW).
  - `EditorFrameRequests`.
  - `TaskSystem` built on enkiTS, not yet constructed in production.
  - Change-driven caster and depth-history revisions, with a qualification oracle.
  - An incremental `GpuScenePublisher`.
- **Integration fix (`3ffa06d`):** lane P's in-place publication skipped the membership republish on transform-only changes, so lane O's content watermarks went stale. The randomized equivalence test caught it after both lanes merged. The in-place path now refreshes the watermarks, and the replay test compares them.
- **Verification:**
  - frozen set `r5-main-2` with `--validation-sync`: identical or within envelopes, 0 hazards, 0 validation messages;
  - digest identical to `r3a0`;
  - sweep `r5-main-2-sweep` 36/36 against `r4d-sweep-final`. Only the new scope names, the manifest path, and first-frame allocation changes from the tracker and the publisher pools differ; steady-state counters are unchanged;
  - tests 110/110 in Release and Debug;
  - editor smoke with `--validation-sync` clean, and `imgui.ini` untouched.

| Timing (`timing/r5-interim`, A = `9f2a28e` (R4 accepted), B = `f2cc899`, A,B,B,A, 10,000 frames, slower machine state) | CPU frame median A / B | Non-wait CPU A / B | GPU median A / B |
|---|---|---|---|
| T-F1-all | 1.116 / 1.116 ms | 0.565 / **0.458** ms (−19%) | 1.125 / 1.138 ms (+1.1%) |
| T-F7-stack | 5.269 / **3.045** ms (−42%) | 5.100 / **2.937** ms (−42%) | 2.058 / 1.947 ms (−5.4%) |

- **F7 target:** non-wait main-thread CPU is already 2.94 ms, under the ≤ 3.0 ms R5 target, even in the slower machine state.
- **GPU:** the deltas are spread evenly over the draw-heavy passes in both directions (F1 forward/lighting about +5 µs each; F7 G-buffer −42 µs and forward −39 µs), and the command stream and images are identical. This reads as GPU clock and pacing behaviour, not workload; on F7 the GPU now idles far less between frames. The R5 final pair re-measures it.
- Steady-frame allocations stay at 0.

## Completion report

(Written at R6.)
