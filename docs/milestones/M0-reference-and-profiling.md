# M0 Execution Plan: Reference Scenes and Profiling Foundation

- Status: Accepted on 2026-07-18
- Roadmap milestone: M0
- Date: 2026-07-17
- Dependencies: completed RHI refactor baseline
- Relevant decisions: ADR-0001 through ADR-0005 establish later measurement needs; M0 does not implement those architectures

## Objective

Make renderer decisions from repeatable evidence. At acceptance, Iridium can reproduce key material, transparency, lighting, CPU-scaling, and temporal cases; capture deterministic images; and attribute frame time and memory to meaningful stages on the reference PC.

M0 should also make the sample car's current appearance explainable. It must distinguish imported source factors and defaults from compiled GPU values, shader inputs, lighting deficiencies, and color-pipeline errors without prematurely rewriting the material system.

## Current context

The post-RHI review found material ambiguity, incomplete glTF extension handling, ad hoc lighting, tone mapping before transparency, entity-origin transparent sorting, repeated per-draw data, and limited objective profiling. `docs/PROJECT_CONTEXT.md` records the detailed observations.

The M0.1 audit established that the reviewed RHI work is committed at `3025259` and the worktree was clean when M0 began. Fresh Debug and Release builds and architecture tests pass. A 20-second Debug renderer smoke selected the RTX 4090 and emitted no Vulkan validation messages. `docs/milestones/M0.1-measurement-contract.md` records the current system inventory, corrected stale findings, metric ownership, naming, asset constraints, and benchmark protocol.

## Invariants

- Preserve the new RHI boundary and keep Vulkan timestamp/query implementation in the backend.
- Instrumentation must be removable or inexpensive when disabled and must not force GPU/CPU synchronization in normal frames.
- Reference capture must identify output/color space; comparisons must not mix scene-linear and display-referred images.
- Do not alter source assets to hide importer or shading defects.
- Do not merge or discard pre-existing worktree changes.

## Scope

- CPU hierarchical timers and frame statistics.
- Vulkan GPU timestamps with delayed, nonblocking readback.
- Renderer/material/geometry/transparency/memory counters.
- Deterministic reference scenes and cameras.
- Capture naming, metadata, and comparison workflow.
- Material provenance/debug inspection sufficient to explain the sample car.
- A recorded 4K baseline on the reference PC.

## Non-goals

- Render graph or HDR output implementation (M1).
- New canonical material compiler or GBuffer layout (M2).
- Asset registry/browser rewrite (M3).
- General scene schema rewrite (M4).
- Production lighting, transparency, GPU scene, mesh shaders, reconstruction, GI, or ray tracing (M5-M11).

Small correctness fixes discovered while enabling a truthful baseline should be proposed separately, not folded into instrumentation without review.

## Slices

### M0.1 - Inventory and measurement contract

Status: `Accepted` on 2026-07-17. The durable result is
`docs/milestones/M0.1-measurement-contract.md`.

Inspect current frame stages, RHI/Vulkan marker facilities, allocator/resource tracking, shader compilation, scene construction, test infrastructure, and capture utilities. Produce a short checked-in inventory mapping desired metrics to owning systems and query APIs.

Define stable names and units for CPU/GPU ranges and counters. Define capture metadata and the benchmark run protocol from `docs/performance/FRAME_BUDGET.md`.

Completion criteria:

- no metric requires an unexplained global stall;
- instrumentation ownership and backend boundary are reviewed;
- reference-scene asset licensing/location and deterministic camera strategy are known;
- conflicts between this plan and current code are logged before implementation.

### M0.2 - CPU timings and renderer counters

Status: `Accepted` on 2026-07-17. M0.2a, the bounded CPU telemetry spine, was accepted on
2026-07-17. Its implementation, verification, exact-4K overhead evidence, rejected
measurement setups, and remaining limitations are recorded in
`docs/milestones/M0.2a-cpu-telemetry.md`.

M0.2b/c added truthful backend recorded-work counters, exact changed-transform
counting, and a decoupled minimal editor consumer. Its runtime cross-check,
validation evidence, final overhead gate, and deferred availability are recorded
in `docs/milestones/M0.2-counters-and-panel.md`. Allocation and Vulkan memory
visibility were subsequently completed together in M0.3 rather than splitting
ownership across milestones.

Add low-overhead scoped CPU timing with hierarchical frame stages, a ring of completed frames, and aggregation for current, median, p95, and p99 reporting. Use thread-safe event collection suitable for future jobs rather than one global mutex around every marker.

Add counters for draws, dispatches, triangles where known, instances, submeshes, materials, material switches, pipelines, lights, transparent primitives, transparent classification, allocations, upload bytes, and changed-data counts. Clearly label unavailable or estimated counters.

Expose results through an editor profiler panel and a machine-readable capture/export format. Runtime collection and editor presentation must remain separate modules.

Verification:

- unit tests for aggregation, nesting, wraparound, and disabled behavior;
- stress test multi-thread event collection;
- measure instrumentation overhead in Release with collection off and on;
- no per-frame unbounded allocation growth.

### M0.3 - Vulkan GPU timestamps and memory visibility

Status: `Accepted` on 2026-07-17. Backend query ownership, delayed readback,
stable ranges, allocator/heap accounting, validation, overhead, 4K reconciliation,
and the external-profiler limitation are recorded in
`docs/milestones/M0.3-gpu-timestamps-and-memory.md`.

Add RHI-level timestamp capability/query interfaces only to the extent needed by multiple backends. Implement Vulkan timestamp query pools, calibrated period conversion, per-frame allocation, delayed availability checks, and nonblocking readback. Annotate major existing passes without imposing pass architecture that belongs to M1.

Add trustworthy persistent/transient resource accounting from engine allocators and Vulkan allocations. If external/driver-resident memory cannot be measured exactly, label the scope rather than presenting false precision.

Verification:

- test timestamp conversion and frame/query reuse;
- run with Vulkan validation enabled;
- verify unavailable results do not block or corrupt later frames;
- compare summed/nested ranges sensibly against external captures such as Nsight Graphics;
- measure query overhead.

### M0.4 - Reference scenes, debug views, and material provenance

Status: `Accepted` on 2026-07-17. The durable implementation, fixture inventory,
provenance findings, validation, repeatability, exact-4K attribution smoke, and
capture-based GPU/image agreement are recorded in
`docs/milestones/M0.4-reference-scenes-debug-and-provenance.md`.

Create deterministic scenes/cameras for the benchmark categories in the performance contract. Reuse existing source assets where licensing and repository policy allow; otherwise use procedural primitives and small purpose-built test assets.

Add or consolidate debug views for base color/diffuse, metallic or F0 as currently available, roughness, normals, emissive, depth, material ID, and motion vectors when the renderer supplies them. A view may state `not available` rather than fabricate data.

For a selected material, display:

- source asset and material identity;
- source factors and whether each was explicit or a format default;
- texture identity, channels, UV set, sampler, and color-space interpretation;
- extension values and unsupported-extension warnings;
- CPU runtime values and GPU-packed values;
- selected pipeline/material flags.

This slice should make the car's red paint, windows, and headlight materials traceable from glTF input to shader-visible values.

Verification:

- automated tests for glTF default reporting and known fixture values;
- debug values agree with GPU captures for representative materials;
- reference scenes reopen into the same state;
- no debug view changes normal rendering when disabled.

### M0.5 - Capture, comparison, and baseline report

Status: `Accepted` on 2026-07-18. The reopened telemetry, metadata, cold/import,
run-spread, allocation, transparent-work, nested-transparency, and opaque-emissive
gaps are closed. The criterion-by-criterion decision and regenerated baseline are
recorded in `docs/milestones/M0-acceptance-report-2026-07-18.md`.

Add deterministic screenshot/capture commands with stable naming and sidecar metadata. Prefer a scene-linear high-precision capture plus the final display-referred output when the current pipeline makes both available. Do not treat differently exposed or tone-mapped outputs as pixel-equivalent.

Provide an image-comparison tool/report with absolute and perceptual metrics, heatmaps, and configurable thresholds. Exact thresholds are scene/output specific and should initially detect change rather than certify physical correctness.

Run the benchmark suite at 4K on the reference PC in Release, plus targeted Debug/validation runs. Record warm-cache and cold/import behavior separately where relevant. Compare selected timings with an external GPU capture to validate attribution.

Completion criteria:

- every benchmark has a reproducible scene/camera and documented expected behavior;
- the baseline report follows `docs/performance/FRAME_BUDGET.md`;
- captures and metadata are sufficient for later M1/M2 before/after comparison;
- current car darkness/metallicity and glass/emissive failures are localized to specific data, shading, lighting, or color-pipeline stages;
- instrumentation overhead and limitations are recorded.

## Delegation and integration

The milestone lead first completes M0.1 and freezes the minimal interfaces. After that, read-only scene/material investigation and test-fixture preparation may run in parallel with CPU instrumentation. GPU query work should have one owner because it touches shared frame/RHI/Vulkan lifecycle code. Reference/debug UI should integrate only after data contracts are stable.

Subagents must own disjoint files or worktrees for write-heavy slices. The lead reviews every result, runs integrated verification, and updates this plan. Agent summaries are evidence, not a substitute for source review.

## Verification matrix

At minimum:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug --output-on-failure

cmake --preset x64-release
cmake --build out/build/x64-release
ctest --test-dir out/build/x64-release --output-on-failure
```

Also run Vulkan validation, the deterministic scene suite, image comparison, Release overhead tests, and one external GPU-profiler cross-check. The lead records exact commands and results in the completion report.

## Risks and fallbacks

- GPU queries can cause stalls if read too early. Use delayed availability and drop a sample rather than block.
- Instrumentation can perturb the workload. Measure disabled/enabled overhead and control detail levels.
- Screenshot tests can be unstable across drivers or output transforms. Separate exact data tests from tolerance-based visual comparisons and retain metadata.
- Reference assets can encode surprising defaults. Preserve them and make defaults visible instead of editing the asset.
- A large profiler UI can distract from the data foundation. Prefer a minimal consumer plus export in M0; richer tooling can follow.

## Decision log

- 2026-07-17: M0 is the first milestone because material, HDR, transparency, and GPU-driven choices need trustworthy visual/performance baselines.
- 2026-07-17: Frame generation is excluded from the base-frame target.
- 2026-07-17: The high-precision current GBuffer remains a reference until M2 demonstrates a production layout.
- 2026-07-17: M0.1 accepted `3025259` as the candidate RHI baseline after fresh Debug and Release builds/tests and a validation smoke run. This does not imply the refactor branch is merged to `main`.
- 2026-07-17: Vulkan query pools remain backend-owned and frame-context-owned. The RHI exposes only completed backend-neutral telemetry; unavailable GPU samples are dropped rather than waited on.
- 2026-07-17: Release performance runs disable validation explicitly and record that state. Debug and targeted validation runs keep it enabled.
- 2026-07-17: Required M0 fixtures must be tracked/procedural or redistributable. The current Alfa Romeo asset remains a hash-identified local diagnostic because its CC-BY-NC-SA license is non-commercial; the local HDRI cannot be required until provenance is recorded or it is replaced.
- 2026-07-17: Current output captures are labelled legacy display-referred. M0 does not implement M1's scene-linear HDR rewrite to manufacture a scene-linear baseline.
- 2026-07-17: Pipeline-statistics transparency data is optional diagnostic evidence and is labelled as fragment invocations/full-screen equivalents, not exact transparent layer count.
- 2026-07-17: M0.2a accepted the bounded CPU telemetry spine after Debug/Release tests, a clean Debug validation run, a zero-overflow 1,000-frame Release export, and five interleaved exact-4K overhead pairs. Visible borderless extent was required because decorated windows were clamped and hidden windows were compositor-throttled.
- 2026-07-17: M0.2 accepted exact recorded-work counters, transform change counts, and a decoupled profiler panel after runtime counter cross-checks, Debug/Release tests, validation, and a post-counter exact-4K overhead gate. Allocation visibility remains M0.3 so allocator/query lifecycle has one owner.
- 2026-07-17: M0.3 accepted frame-context-owned Vulkan timestamps, delayed availability readback, Debug Utils range annotations, centralized engine allocation accounting, external swapchain scope, and separate `VK_EXT_memory_budget` data after Debug/Release tests, validation reuse runs, exact-4K overhead gates, and memory reconciliation. Nsight Systems validated marker discovery but its individual-workload projection was too intrusive for absolute agreement; an exact external-boundary timing comparison remains in M0.5.
- 2026-07-17: M0.4 accepted an LF-stable, hash-verified M0 manifest with five required fixtures and one optional licensed sample-car diagnostic; retained glTF provenance outside the RHI contract; exposed truthful opaque G-buffer views; and closed its capture gate with bit-exact Release readback plus target-pixel agreement.
- 2026-07-17: M0.5 implementation completed deterministic post-transparency/pre-UI readback, strict TGA/JSON artifacts, metadata-compatible image comparison, and the 4K Release baseline. A mislabeled Debug-configured candidate was detected through runtime metadata and rejected before the suite was regenerated correctly. Nsight Systems again proved unsuitable for exact numerical agreement: its outer batch workload median was 2.14 times the matching internal command-buffer timestamp.
- 2026-07-18: Nsight Graphics 2026.2 GPU Trace measured two consecutive 4K frames at the same eight Debug Utils marker boundaries as Iridium's in-command-buffer timestamps. All 16 paired durations agreed exactly or within one observed 32 ns timing/export quantum. This closed the external-attribution gate and was initially treated as final M0 acceptance; the later integrated audit retained this evidence but reopened other unmet criteria.
- 2026-07-18: A fresh integrated acceptance audit reopened M0. Debug/Release builds and tests, validation, deterministic images, Vulkan timing, and memory accounting remain valid. M0.1's 10,000-frame percentile contract is not met because summaries aggregate only the retained 512-frame tail; required run metadata, CPU allocation-per-frame counters, transparent-overdraw evidence, transparent-nesting coverage, and opaque-emissive coverage are also absent. `docs/milestones/M0-acceptance-audit-2026-07-18.md` is authoritative for closure.
- 2026-07-18: The audit's seven closure conditions were implemented and independently rerun. Full-run exact statistics now cover 10,000/10,000 CPU/GPU samples; headers contain the frozen machine/output/cache fields; five-run warm and cold/import evidence is recorded; C++ allocations and optional transparent fragment/fullscreen-equivalent counters are available; nested closed transparency and opaque emissive are required fixtures; Debug/Release 9/9 tests, 4K validation, strict images, memory, and overhead gates pass. M0 is accepted by `docs/milestones/M0-acceptance-report-2026-07-18.md`.

## Acceptance report

The final decision is
`docs/milestones/M0-acceptance-report-2026-07-18.md`. The earlier M0.5 report and
integrated audit remain point-in-time implementation/reopen evidence.

M0 acceptance satisfies M1's dependency. It does not authorize M1 source
implementation; the proposed M1 execution plan still requires owner approval.
