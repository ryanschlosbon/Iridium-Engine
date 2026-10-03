# M7R Architecture Consolidation Execution Plan

## Header

- **Milestone:** M7R — Architecture consolidation
- **Status:** In Progress — plan approved by owner 2026-10-02; R0, R1 and R2 accepted 2026-10-02; R3 active
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

### R3 — Graph-driven execution and backend decomposition (`In Progress`)

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

### R4 — Vulkan modernization (`Proposed`)

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
| 2026-10-02 | Async compute and `/W4` are out of scope. The editor and importer files over 2,500 lines are deferred exceptions. | Scope control. |
| 2026-10-02 | ADR-0016 will record the actual executor model, because ADR-0002's claims about queue, history and imported resources are unimplemented. | Graph audit. |
| 2026-10-02 | Owner approved removing `--developer-legacy-transparency` in R2. It is a post-M6 diagnostic A/B, never an automatic fallback (ADR-0012). It costs 3 graph passes, extra frame targets and 11 backend references, and complicates the R3 transparency owner. Serialized `LegacyTwoBucket` stays readable. | ADR-0012:94–113 |

### Third-party library decisions

| Library | Status | Pin |
|---|---|---|
| VMA (MIT) | Explained by the director; adopt in R4b behind `VulkanResourceAllocator`. | Exact release tag, recorded at adoption. |
| enkiTS (zlib) / Taskflow (MIT) | Evaluate in R5b with a frame-plus-background-cook benchmark; decide in ADR-0015. | Exact tag. |
| Tracy (BSD-3) | Optional; decided in R5b; compiled out by default. | Exact tag. |

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

## Completion report

(Written at R6.)
