# M7R R2 design — qualification harness, CLI registry, legacy removal, behavioral tests

This is the implementation design for slice R2 of
`docs/milestones/M7R-architecture-consolidation.md`. It was drafted by a read-only
design pass against `7781b03` (R1 step 1) and accepted by the lead on 2026-10-02.

Line numbers refer to that commit. Re-verify them before editing, because R1 step 2
changes the CMake layout. Decisions and evidence go in the plan's log, not here.

## 1. Inventory

### `src/app/Application.cpp` (≈2,500 lines move out)

| Lines | Feature | Destination |
|---|---|---|
| 120-166 | `Ordinary2FallbackModelStats` | harness (fallback validator) |
| 168-198 | Operator, gamut and transform-id names (metadata only) | `qualification/RunMetadata` |
| 363-372 | `collectFrameCaptures(true)` drain | `onShutdown` → extension |
| 373-772 | Eight end-of-run `IRIDIUM_*_VALIDATION` reports | `onShutdown` (`QualificationReport.cpp`) |
| 780-1003 | Capture-artifact metadata and `IRIDIUM_CAPTURE` | `CaptureArtifactExport.cpp` |
| 1005-1309 | CPU-profile run metadata | `RunReportExport.cpp` |
| 1348-1376 | Four oracles OR-ed with `enableValidation` | decoupled in R2.5 |
| 1357-1358, 1411-1423 | Legacy transparency plus its print | deleted in R2.2 |
| 1424-1522 | Texture- and material-table scale validators (allocation order matters) | `onStartup(BackendReady)` |
| 1531-1537, 1654-1656 | Benchmark ImGui ini, in-memory catalog, no source workers | `AppRunPolicy.deterministicContent` |
| 1664-1770 | Manifest, model, constant environment, camera, frame defaults | `onStartup(ContentLoad)` |
| 1828-1971 | Capture and validator preconditions | `onStartup(ScenePrerequisites)` |
| 1987-2004 | Topology prewarm print and rebuild baselines | `onStartup(TopologyReady)` |
| 2009-2475 | Benchmark grid, render-instance batch, fixture/stress/table-scale lights, probe validation entities, residency-churn probe | `onStartup(SceneConstruction)`, keeping the same entity order |
| 2512-2558 (called at 3679 inside the open frame) | Residency churn | `onFrameBegin(BackendFrameOpened)` |
| 2616 → 6660-6702 | `updateBenchmarkState` | `onFrameBegin(PreSceneUpdate)` |
| 2705-2736, 2781-3032 | Resize/lifecycle validators and capture intents | `onFrameBegin(PostSceneUpdate)` |
| 2761-2778 | `IRIDIUM_RUN_METRICS` | `onShutdown` |
| 3514, 3745, 3818-3834, 3860-3864, 4046 | `activeBenchmark_` gates (80 uses) | `AppRunPolicy` / `AppFrameRequests` |
| 5061-5097 | Validation/capture requests around forward/output submission | extension, armed at `BackendFrameOpened` |
| 5108-5138 | Transport-switch validator | `onFrameEnd` via `IAppControl::applyOutputTransport` |
| 5195-5236 | Freeing probe textures and materials | `onShutdown` |

### `VulkanVertexBackend.cpp` (≈2,200 lines move to `VulkanQualificationExtension` / `VulkanIndirectOracle`)

- **Frame captures:** 12060-12251.
- **Ordinary2:** 10916-10998 and 12252-12449.
- **Deep-layered:** 10999-11145 and 12450-12797; scene-resolve counts are patched at 10900-10913.
- **Depth pyramid:** 12798-12993.
- **Destroy helpers:** 12994-13027.
- **Slot collect:** 3218-3221.
- **Cleanup:** 975-995.
- **Indirect validation:** 7485-8009 (directional, spot, point, probe) and 8463-8831 (opaque, LOD, occlusion). Device telemetry stays; the comparison moves.
- **Expectation emission:** stays behind `if constexpr` at directional 4022-4024/4109-4174, spot ~4609/4685, point ~5197/5281, probe 5611/5726, opaque 6251/6265/6357.
- **VSM depth oracle:** 3845-3902 and 11522-11545.
- **Probe capture-target startup validator:** 425-440.
- **Config fields:** 288-308 and 352.
- **Telemetry that stays:** `lighting.cluster.readback` at 9885-9893, consumed at 9780-9815 and `Application.cpp:5025-5058`.
- **Legacy transparency:** 130, 280, 532-538, 756, 1215-1221, 1828, 1996, 2099, 2210, 2290, 2367, 3164-3174, 11655-11800+, 13203.

### `VulkanProductionRenderGraph.cpp`

- **Validation hooks:** depth pyramid 532-542, layered 592-604, per tier 677-694, final capture 810-813.
- **Cluster readback:** 183-188 and 470-474.
- **VSM depth `TransferSource`:** 513.
- **Legacy:** 267-271, 721-757, and parameters at 122/138 (header 102/115).
- `skipPass` emits no barriers. A hook's only effect is its compiled usage flags and lifetimes, so keeping declarations identical keeps an ON build identical.

### `IRenderBackend.h`

- The 8 capture/oracle methods (243-262) and the `Ordinary2CaptureValidation.h` include leave.
- The result types (`Ordinary2CaptureValidation.h:10,54`, `DepthPyramid.h:151`) move to `src/qualification/QualificationResults.h`.
- `getGlassDepthTextureID` stays until R3c's `IEditorRenderBridge`.

### CLI ownership (83 branches, `ApplicationConfig.cpp:90-691`)

| Owner | Flags |
|---|---|
| Runtime (13) | validation on/off; CPU/GPU/overdraw profiling; hidden/borderless/window size; frame limit and warmup; cooked model and environment artifacts; help |
| Editor (4) | show profiler, show material diagnostics, open asset viewer, wireframe |
| Renderer settings (29) | G-buffer layout; exposure, operator, transport, nits; debug view; cluster tile/slices; shadow-directional ×7, shadow filter, spot atlas; shadow/GPU/probe LOD and LOD caps; depth pyramid, occlusion query/rejection; VSM resources |
| Qualification (36) | 9 `--validate-*` plus deep quality; 3 table scales and cluster stress; benchmark, manifest, disable-local-shadows, select entity, OIT seed; 3 reference-direct; 5 qualification oracles; depth-pyramid capture/resize; minimum resident LOD; capture frame/dir/point/require-signal; profile output; cache state |

`--developer-legacy-transparency` is removed.

Preserve these cross-module implications:
- the VSM oracle sets VSM;
- the depth-pyramid validators set the depth pyramid;
- `--profile-cpu-output` sets CPU profiling.

Post-parse checks: runtime keeps 700-718; qualification takes 693-699 and 719-723.

## 2. Interfaces

### App side (`src/app/FrameObserver.h`)

```cpp
struct AppRunPolicy { bool deterministicContent=false; bool fullscreenScenePresentation=false;
                      bool colorValidationOverlay=false; bool ownsStartupContent=false; };
enum class StartupPhase : uint8_t { Configure, BackendReady, ContentLoad, ScenePrerequisites,
                                    TopologyReady, SceneConstruction, Ready };
enum class FrameBeginPhase : uint8_t { PreSceneUpdate, PostSceneUpdate, BackendFrameOpened };
struct AppFrameRequests { std::optional<uint64_t> viewHistoryResetRevision; bool suppressGridOverlay=false; };
class IAppControl { public: virtual OutputTransportSwitchResult applyOutputTransport(Color::OutputTransport)=0;
                    virtual bool resizeSceneExtent(RenderExtent, std::string& diag)=0; protected: ~IAppControl()=default; };
class IFrameObserver {
public:
  virtual ~IFrameObserver() = default;
  [[nodiscard]] virtual AppRunPolicy runPolicy() const { return {}; }
  [[nodiscard]] virtual std::span<IRenderBackendExtension* const> backendExtensions() { return {}; }
  virtual void onStartup(StartupPhase, AppStartupContext&) {}
  virtual void onFrameBegin(FrameBeginPhase, AppFrameContext&) {}
  virtual void onBeforeSubmit(AppFrameContext&) {}   // after submitUIPass, before endFrame
  virtual void onFrameEnd(AppFrameContext&) {}       // after endFrame
  virtual void onShutdown(AppShutdownContext&) {}    // before backend cleanup; completed=false on failure path
};
```

Phases preserve today's ordering:
- resizes happen between frames;
- churn and capture arming happen inside the open frame;
- table-scale allocations come before asset services, because they shift texture and material indices.

Other pieces:
- `AppRunSnapshot` replaces the member reads at 780-1309.
- Build macros (`IRIDIUM_BUILD_*`) become a generated `core/BuildInfo.cpp`.

### CLI registry (`src/core/cli/CliOptionRegistry.h`)

```cpp
struct CliOption { std::string_view name; std::vector<std::string_view> aliases; uint8_t arity;
  std::string_view valueName, help, owner, missingValueMessage; bool rejectEmptyValue=false;
  std::function<void(std::string_view)> parse; };
class CliOptionRegistry { public:
  void add(CliOption); void addValidator(std::string_view owner, std::function<void()>);
  void parse(std::span<const std::string_view>);   // "Unknown option: X"; last wins
  [[nodiscard]] std::string usage() const; [[nodiscard]] std::span<const CliOption> options() const; };
```

- Runtime, editor and renderer options register from `app/cli/`. Qualification registers from its library.
- `main.cpp` holds the only `#if IRIDIUM_QUALIFICATION`. `createHarness` returns null unless a qualification flag is active.

### Backend side

**RHI (neutral):**
- `IRenderBackendExtension { api() }`;
- `RenderBackendCreateInfo { api, span<IRenderBackendExtension* const> extensions }`;
- `createRenderBackend(const RenderBackendCreateInfo&)`.

**renderer-vulkan, `VulkanBackendExtension.h` (interfaces always compiled):**
- `VulkanHookPoint` (SceneColorComplete, FinalCaptureHook, DepthPyramidValidation, Ordinary2Validation, DeepLayeredValidation, DeepLayeredResolveCounts, VirtualShadowDepthSnapshot);
- `VulkanGraphHooks` (which hook passes to declare);
- `VulkanBackendServices` (device, allocator, scheduler, graph executor, frame targets, probe targets, profiler);
- `VulkanHookContext` (cmd, slot, extents/formats, payload variant);
- `IVulkanIndirectOracle` (`enabled`/`beginWork`/`expect`/`verify` per view kind, plus occlusion/VSM variants);
- `IVulkanBackendExtension` (`onAttach`, `onBackendInitialized`, `graphHooks`, `wantsHook`, `onHook`, `onFrameSlotRetired`, `indirectOracle`, `onBeforeDeviceDestroy`).

**Wiring:**
- The factory attaches Vulkan extensions before `init()`.
- `buildVulkanProductionRenderGraph` takes `VulkanProductionGraphFeatures { depthPyramid, virtualShadowWorkingSetBytes, clusterTelemetryReadback, VulkanGraphHooks hooks }`.
- One helper, `runHook(point, passName, ctx)`, does `beginPass`+`onHook`, or `skipPass`, or nothing.
- The scene-linear capture keeps its out-of-plan `transitionImage` at `SceneColorComplete`.
- GPU range names stay unchanged.
- The harness drives the extension through a neutral `IQualificationBackend` (`arm*`/`collect*`) at `BackendFrameOpened`.

### As implemented at R2.9 (deviations)

- **Routing fields.** Five qualification flags change how the Application drives
  the renderer (the three reference routes, the WeightedOIT order seed and the
  resident LOD floor). They live in `QualificationOptions` and reach the
  Application as `AppRunPolicy::routing` (`AppRenderRouting`), not through
  `ApplicationConfig`. `RenderBackendConfig` keeps `forceDirect*` and
  `weightedOitOrderSeed`; it loses the six oracle/probe-validator fields, which
  the harness hands to the extension at `StartupPhase::Configure`
  (`QualificationBackendConfig`).
- **`IQualificationBackend`.** The harness owns a `VulkanQualificationExtension`
  through the neutral `src/qualification/QualificationBackend.h` (as §2 says), so
  the harness stays free of Vulkan headers and its tests use a device-free fake.
  Captures arm at `BackendFrameOpened` with the validations;
  `FrameSubmitPoint` stays on `IFrameObserver` but the harness no longer uses it.
- **Harness attachment.** As at R2.6, qualification builds attach the harness to
  every run (it prints the `IRIDIUM_*` startup and run-metric lines), so the
  extension and its hook declarations are present on every ON run.
- **Libraries.** `iridium_transparency` is a header-only INTERFACE library over
  `iridium_rhi`; the result types are in `src/qualification/QualificationResults.h`.
  `main.cpp`'s `#if IRIDIUM_QUALIFICATION` uses a macro from the generated
  `core/BuildFeatures.h`.

## 3. `IRIDIUM_QUALIFICATION=OFF`

- **Library:** `src/qualification/` (`iridium-qualification`) is added only when ON.
- **Configuration:** a generated `core/BuildFeatures.h` provides `kQualificationBuild`; preset `x64-release-shipping`.
- **Moved out:** harness, Vulkan extension, oracles and result types are moved, not `#if`'d. `ApplicationConfig` loses 36 fields; `RenderBackendConfig` loses the oracle and legacy flags.
- **Backend:** `extension_ == nullptr` is the null object. The only conditional code left in the backend is the five expectation-emission sites and the VSM depth snapshot (`if constexpr`); R3a collapses them into one.
- **Profile schema:** the counter schema is unchanged; oracle counters emit 0.

## 4. Legacy transparency removal (R2.2)

**Delete:**
- `GlassDepthPipeline.*`, `GlassDepthRenderPass.*`, `glass_depth.vert`;
- graph builder 267-271 and 721-757, with the parameter;
- the `VulkanFrameTargets` legacy targets;
- the backend sites in §1 (keep pipeline-identity value 130 reserved);
- the config flag, CLI branch and usage line;
- `Application.cpp` 1357-1358 and 1411-1423 (`AssetManager` gets `Classified`);
- the `RenderBackendConfig` flag.

**Edit:** the asset-browser help text and the legacy profile prefixes. Keep the zero counters for schema stability.

**Keep:** the enum, its name function, the importer mapping and `ModelProduct.cpp:582-590`, so CookKeys are unchanged.

**Tests:** the graph test becomes "legacy topology absent"; config tests reject the flag. ADR-0012 gets an amendment.

**Byte-identity risk:** none for the default path; verify with the topology test and the frozen set.

## 5. Source-text test replacement

New support code:
- `tests/support/SpirvInspector`: member offsets, LocalSize, set/binding, push-block size, vertex inputs;
- `HeadlessVulkanDevice`;
- a `ShaderDepfile` reader;
- `GraphQuery` helpers.

| Old test | Replacement |
|---|---|
| Stage3 group 1: device indirect/LOD contracts | Headless compute dispatch of each `*_compact.comp` against the CPU LOD selectors; SPIR-V layout checks. Backend call-text checks are deleted (covered by F5 captures and oracle runs). |
| Stage3 group 2: layered Vulkan objects | Compiled-graph assertions (formats, load ops, pass order and counts per tier, `TransferSource` only on hooks); headless pass/pipeline creation; push-constant `static_assert` plus SPIR-V sizes; F4 captures and validation process tests. |
| Stage3 group 3: layered shaders | Depfile ownership of `complex_material_body.glsl` and `material_bsdf.glsl`; SPIR-V entries. Expression-text checks are deleted. |
| Stage3 group 4: topology/lifecycle | Keep the behavioral prewarm checks; a harness phase-order unit test; resize validators under the vulkan label. |
| Stage3 group 5: debug view/editor text | Keep the `RenderDebugView` round-trip; delete shader and UI copy checks. |
| Stage3 group 6: position-only shadow fetch | SPIR-V vertex-input count and headless pipeline attribute count. |
| SceneColorTests | Compute parity of `scene_color.glsl` with `SceneColor.h`; depfile check that only `output.frag` owns the output transform. |
| LightingReferenceTests | Compute parity of `direct_lighting.glsl`; depfiles proving both lighting bodies include the clustered-light access (the one-representation ADR invariant). |
| StandardMaterialShadingTests | Depfile ownership of the BSDF/normal/transport includes; BSDF parity. |
| ReflectionProbeTests, LightExtractorTests | SPIR-V offsets against `offsetof` and `static_assert` sizes. |
| M4ProductionCutoverTests | Keep the canonical-bytes checks; delete the CMake and editor-source text checks. |

Every deleted assertion is recorded in an "R2 assertion disposition" table in the plan.

## 6. Ordered sub-steps

| Step | Content | Parallel |
|---|---|---|
| R2.0 | `tools/m7r/Run-QualificationSweep.ps1`: every validator/oracle flag on its fixture; store the `IRIDIUM_*` JSON at HEAD | serial, first |
| R2.2 | Legacy removal; `VulkanProductionGraphFeatures` with defaults equal to today; ADR-0012 amendment | serial |
| R2.3 + R2.1a | Cluster readback only when profiling (process-fixed); VSM depth read only with the oracle; graph-topology tests | writer A |
| R2.1b | SPIR-V, ABI and depfile tests; the other five test files | writer B (tests only) |
| R2.1c | Headless device and pipeline tests | writer C (tests only) |
| R2.4 | `CliOptionRegistry`; per-owner registration; generated usage; table-driven flag parity test | writer D |
| R2.5 | Stop OR-ing `enableValidation` into the oracles; vulkan-label oracle process tests on tracked models | serial |
| R2.6a/b/c | `IFrameObserver` and `QualificationHarness`: startup phases, frame validators, shutdown and exports | lane A |
| R2.7 | `IVulkanBackendExtension`, factory `CreateInfo`, `VulkanQualificationExtension` (the 8 methods forward temporarily) | lane B |
| R2.8 | `VulkanIndirectOracle` and `if constexpr` emission sites | lane B |
| R2.9 | Join: delete the 8 methods and the oracle config fields; library-registered flags; OFF build, preset and `main.cpp` `#if` | serial |
| R2.10 | Evidence: frozen set, timing pair, sweep diff, OFF checks, disposition log | serial |

Writers A–D touch disjoint files; test CMake registration is merged by the lead. Lanes A and B are disjoint until R2.9.

## 7. Risks and verification

1. **ON-build identity.** Hook declarations must match R0 whenever the harness is active. Compare `renderGraphTopologyHash` from the timing profiles and run the frozen set after every step.
2. **Entity and allocation order.** Keep the phase order, and rerun the frozen set and table-scale sweep after R2.6a.
3. **Lost requests.** A request lost to a swapchain recreate in `beginFrame` must reproduce today's behavior; covered by a fake-backend unit test.
4. **Lost qualification paths.** Diff the R2.0 and R2.10 sweep JSON payloads, not just exit codes.
5. **Debug oracles off by default.** Intended; covered by vulkan-label oracle process tests.
6. **VSM `TransferSource` removal.** Run an F5 A/B with VSM on, with and without the oracle, plus the VSM tests.
7. **Timing and allocations.** Run the matched pair at R2.7, R2.8 and R2.10; allocations must stay 0 on both routes.
8. **CLI parity.** The table test covers every flag; an OFF-shaped registry must reject `--benchmark`.
9. **OFF leakage.** Search the OFF exe for qualification strings, and smoke-run it under validation.
