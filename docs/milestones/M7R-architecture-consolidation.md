# M7R Architecture Consolidation Execution Plan

## Header

- **Milestone:** M7R — Architecture consolidation
- **Status:** In Progress — plan approved by owner 2026-10-02; R0 accepted 2026-10-02; R1 active
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

### R1 — Build system and module DAG (`In Progress`)

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

### R2 — Qualification harness and CLI (`Proposed`)

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

### R3 — Graph-driven execution and backend decomposition (`Proposed`)

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
- Debug is faster than Release only because the Debug preset currently compiles without `/Od /Zi /RTC1` (fixed in R1). R1 will report the deltas with a corrected Debug baseline.
- Shader include fan-out (all 69 shaders) is cheap in wall time, but R1 still adds depfiles.

**Tests.** Debug 78/78 and Release 78/78 pass at `2c50b36`.

## Completion report

(Written at R6.)
