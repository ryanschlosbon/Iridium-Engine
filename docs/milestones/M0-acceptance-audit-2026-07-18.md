# M0 Integrated Acceptance Audit

- Milestone: M0 - Reference Scenes and Profiling Foundation
- Status: Closed by final acceptance on 2026-07-18
- Audit date: 2026-07-18
- Source baseline: `30252593f8fdd2de5dffbb8da31bb570ff49a7c0`
- Branch: `Render-Refactor-for-Modularity`
- Worktree: dirty with the ongoing RHI/M0 work; unrelated changes preserved
- Reference machine: RTX 4090, Core i9-14900K, 64 GiB DDR5-6000, 3840x2160 display
- Governing records: `AGENTS.md`, `ROADMAP.md`, `PLANS.md`,
  `docs/PROJECT_CONTEXT.md`, `docs/performance/FRAME_BUDGET.md`, ADR-0001 through
  ADR-0005, and `docs/milestones/M0-reference-and-profiling.md`

## Decision

This section records the point-in-time reopen decision. All seven required closure
items were subsequently implemented and rerun. The superseding final decision is
`docs/milestones/M0-acceptance-report-2026-07-18.md`.

M0 is not accepted against every frozen criterion. The implementation builds and its
existing telemetry, capture, comparison, material-provenance, Vulkan timestamp, and
memory-accounting paths are useful and substantially verified. Acceptance is reopened
because several explicit contracts were reported as limitations instead of being
satisfied or formally amended before acceptance.

This audit supersedes the earlier acceptance wording, but it does not discard the
valid evidence in the M0.1-M0.5 slice reports. M1 remains `Proposed`; no M1 source
implementation or execution plan is authorized by this audit.

## Source inspection findings

1. `CpuProfiler::CompletedFrameCapacity` is 512. `CpuProfileExport` aggregates only
   `snapshotCompletedFrames()`. Four fresh 10,000-frame runs therefore report
   `frames_completed=10000`, `frames_retained=512`, and percentile sample counts of
   512. This does not satisfy M0.1's requirement to measure at least 10,000 frames
   for the initial percentile baseline.
2. `CpuProfileRunMetadata` does not contain the required compiler, shader compiler,
   OS, CPU, system-memory, GPU identity/driver, Vulkan version/SDK/layers, swapchain
   format/color space, present mode, output mode, base resolution, reconstruction,
   quality, or cache-state fields. `Application` serializes these as unavailable even
   on the reference Vulkan device. The baseline therefore does not follow the full
   `FRAME_BUDGET.md` run-record format.
3. The 31 current per-frame counters contain no transparent fragment, overdraw, or
   full-screen-equivalent measurement. Legacy bucket packet counts are not the
   transparent-overdraw statistic named by `ROADMAP.md`.
4. Vulkan allocation snapshots are implemented, but the CPU allocation count/bytes
   per frame required by `FRAME_BUDGET.md` and the M0.2 counter scope are not.
5. `transparency_v1` contains disconnected overlapping surfaces but explicitly does
   not cover nested closed shells. The roadmap calls for transparent-nesting reference
   coverage. Opaque emissive also has no required visual/debug fixture; the existing
   emissive debug capture is black because the only emissive material is transparent
   and forward transparency is deliberately suppressed in G-buffer debug views.
6. The material, lighting, geometry, and temporal fixtures truthfully list other
   unavailable future axes. Those unavailable M1-M11 features are not themselves M0
   implementation blockers, but they must not be described as tested behavior.
7. Cold import/upload and cache state remain unavailable, and the baseline records no
   multi-run spread. M0.1 requires separate cold/warm reporting and run-to-run spread.

## Criterion matrix

### Invariants

| Criterion | Result | Evidence |
|---|---|---|
| Preserve the RHI boundary; Vulkan query details remain backend-owned | Pass | RHI exposes completed `GpuProfileRange` data and capabilities; query pools remain in `VulkanFrameScheduler`. |
| No instrumentation-induced normal-frame synchronization | Pass | GPU results and captures are collected after existing frame-slot fences; no query path uses `VK_QUERY_RESULT_WAIT_BIT`. |
| Capture identifies output/color domain | Pass | Sidecars label `legacy_display_referred_srgb_target` and reject incompatible domains. |
| Do not alter source assets to hide defects | Pass | Material defaults/extensions are retained as provenance and warnings. |
| Preserve pre-existing worktree changes | Pass | No reset, checkout, stage, or commit was used; generated ImGui layout changes were restored. |

### M0.1 completion criteria

| Criterion | Result | Evidence |
|---|---|---|
| No metric requires an unexplained global stall | Pass | Delayed fence-owned query/capture collection and explicit blocking upload scope. |
| Instrumentation ownership/backend boundary reviewed | Pass | M0.1 contract plus current RHI/Vulkan source inspection. |
| Asset licensing and deterministic camera strategy known | Pass | Versioned manifest, tracked hashes, redistributable required assets, optional licensed car diagnostic. |
| Plan/source conflicts logged before implementation | Pass | M0.1 inventory records stale G-buffer and asset-location findings. |

### M0.2 verification

| Criterion | Result | Evidence |
|---|---|---|
| Aggregation, nesting, wraparound, disabled behavior | Pass | Fresh Debug and Release `CpuProfilerTests`. |
| Multithread event stress | Pass | Eight-thread/256-event test with no drops. |
| Release overhead within contract | Pass on retained historical evidence | Five interleaved application pairs and the microbenchmark are recorded in M0.2a/M0.2. A fresh overhead A/B was not required to diagnose the open blockers. |
| No unbounded per-frame allocation growth | Pass for profiler storage | Fixed event/counter/range arrays and a 512-frame ring. CPU application allocations per frame remain a separate missing counter. |

### M0.3 verification

| Criterion | Result | Evidence |
|---|---|---|
| Timestamp conversion and frame/query reuse tests | Pass | Fresh `CpuProfilerTests`; prior validation covered repeated frame-slot reuse. |
| Vulkan validation | Pass | Fresh 16-frame Debug validation run and capture emitted no validation message. |
| Unavailable results do not block/corrupt later frames | Pass | Fixed availability path, unit coverage, and zero fresh GPU drops. |
| External profiler comparison | Pass | Nsight Graphics exported two consecutive frames and eight identical markers; all 16 paired durations differ by 0 or 32 ns. |
| Query overhead measured | Pass on retained historical evidence | M0.3 five-pair exact-4K gate found no measurable regression above noise. |

### M0.4 verification

| Criterion | Result | Evidence |
|---|---|---|
| glTF defaults and fixture values tested | Pass | Fresh `MaterialProvenanceTests` and `BenchmarkManifestTests`. |
| Debug values agree with GPU-visible capture | Pass for covered opaque axes | Recorded pixel probes agree for base color, normal, roughness, metallic, and depth. Opaque emissive remains uncovered. |
| Reference scenes reopen deterministically | Pass | Five fresh 4K captures match their recorded SHA-256 values with the same benchmark state. |
| Disabled debug view preserves normal rendering | Pass | Recorded default-final versus explicit-final strict comparison is bit-exact. |

### M0.5 completion criteria

| Criterion | Result | Evidence |
|---|---|---|
| Every benchmark has reproducible state and expected behavior | Partial | Manifest state and existing proxy images are deterministic, but required transparent-nesting and opaque-emissive axes are absent. |
| Baseline follows `FRAME_BUDGET.md` | Fail | Long-run percentiles cover 512/10,000 frames; required environment/output/cache metadata, cold/import timing, and run spread are absent. |
| Captures support later M1/M2 before/after comparison | Pass with declared scope | Stable 4K display-referred final/debug captures exist; M1 must add separate scene-linear HDR and final-output artifacts. |
| Car darkness, metallicity, glass, and emissive failures localized | Pass | Provenance records omitted metallic defaults, ignored clearcoat/specular, transmission/IOR behavior, hardcoded lighting, and double tone mapping. |
| Instrumentation overhead and limitations recorded | Pass | M0.2/M0.3 overhead gates and M0.5 limitations are durable. |

### Integrated verification matrix

| Gate | Result |
|---|---|
| Fresh Debug configure/build | Pass with MSVC/Ninja developer environment; pinned dependencies fetched |
| Fresh Debug CTest | 8/8 pass |
| Fresh Release configure/build | Pass; runtime configuration reports `Release` |
| Fresh Release CTest | 8/8 pass |
| Fresh Debug Vulkan validation | Pass, 16 measured frames, zero emitted validation messages |
| Deterministic scene suite | Five fixture hashes reproduced when warm-up/frame state matched |
| Strict image comparison | Five passes: maximum RGBA delta 0, changed fraction 0, mean luma SSIM 1.0 |
| External GPU cross-check | 16/16 paired marker durations within 32 ns |
| `git diff --check` | Pass except informational line-ending warnings |

The fresh commands were run from the repository root after loading the Visual Studio
x64 developer environment (MSVC 19.51.36248.0 and the VS-bundled Ninja):

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug --output-on-failure

cmake --preset x64-release
cmake --build out/build/x64-release
ctest --test-dir out/build/x64-release --output-on-failure

out/build/x64-debug/bin/IridiumEngine.exe `
  --benchmark material_lab_v1 --warmup-frames 8 --frame-limit 16 `
  --profile-gpu `
  --profile-cpu-output out/profiles/m0-reaudit-debug-validation.jsonl `
  --capture-frame 0 `
  --capture-directory out/captures/m0-reaudit-debug-validation `
  --window-size 1280x720 --borderless-window --validation

out/build/x64-release/bin/IridiumEngine.exe `
  --benchmark <fixture-id> --profile-gpu `
  --profile-cpu-output out/profiles/m0-reaudit-final-<fixture-id>.jsonl `
  --window-size 3840x2160 --borderless-window --no-validation

out/build/x64-release/bin/IridiumImageCompare.exe `
  --reference <recorded-baseline.tga> --candidate <fresh-capture.tga> `
  --report out/captures/m0-reaudit-baseline-<fixture>.compare.json `
  --heatmap out/captures/m0-reaudit-baseline-<fixture>.heatmap.tga
```

The first Debug configure attempt correctly failed in the restricted environment
while fetching the pinned GLFW source. Repeating it with network access and the
documented MSVC/Ninja environment succeeded; this was an environment prerequisite,
not an engine failure.

The first attempted fresh `x64-release` suite was also rejected. CMake regenerated
the cache after detecting a compiler-path change and the resulting cache reported
`CMAKE_BUILD_TYPE=Debug`; the runtime JSONL headers and capture sidecars likewise
reported `Debug`. No timing or capture from `m0-reaudit-final-*` or
`m0-reaudit-4k-*` is used below. After stabilizing the MSVC/Ninja environment, the
Release preset was reapplied, the cache reported `Release`, the runtime header
reported `Release`, Release CTest passed again, and the complete valid suite was
regenerated under `m0-reaudit-release-*`.

## Fresh visual baseline

All images are 3840x2160 Release captures, validation off, post-transparency and
pre-UI, in the current legacy display-referred sRGB-target domain.

| Fixture/state | SHA-256 | Strict comparison |
|---|---|---|
| `material_lab_v1`, warm-up 500, measured frame 0 | `a0f404a5783d7af70d5d4b418d399397eeb09e6a1ab58f720e57725f8d5032d2` | Exact |
| `transparency_v1`, warm-up 500, measured frame 0 | `aadc384db66c4ea90f299252a2db5feb608f31e180c4a3e20de9517ca0ffaf11` | Exact |
| `lighting_legacy_v1`, warm-up 500, measured frame 0 | `04e5869ca1b1334a8b3834ad2cc814a1d6bdb8e896653e39efa0b9d5cf8c5d27` | Exact |
| `geometry_cpu_v1`, warm-up 500, measured frame 0 | `0ea8c725383785e2287681fb71c3090c2c961f822344bccd93bcb6f9db5b1eac` | Exact |
| `temporal_proxy_v1`, warm-up 0, measured frame 120 cut | `63149433c5e10152a512ce5657cf9048d7c96f25e24bb4fb04ad1d2b974d87d2` | Exact |

A geometry capture with only 32 warm-up frames intentionally produced a different
hash, `6005c497da9f8ee399672ab5a9dd85e0876775e82fa343d1ca5d3b67cf59e616`,
because motion is application-frame-indexed. Repeating the contracted 500-frame
state restored the baseline hash. The comparator likewise rejected mismatched
benchmark-state metadata before treating equal static pixels as comparable.

Visual inspection confirms the current proxy content: a five-material primitive row,
three overlapping transparent triangles over a backdrop, the low-environment legacy
lighting row, a 128-instance grid, and the temporal cut. These are regression images,
not proof of nested glass, opaque emissive range, production lighting, LOD/animation,
or temporal reconstruction quality.

## Fresh CPU, GPU, and memory observations

The four static fixtures used 500 warm-up and 10,000 measured frames. The temporal
proxy used its contracted 0 warm-up and 240 measured frames. All were Release,
validation off, visible borderless 3840x2160, CPU/GPU/counter/memory collection on,
and reported zero dropped frames.

| Fixture | Frames completed/retained | Wall average | CPU median/p95/p99 | GPU median/p95/p99 | Requested/committed live |
|---|---:|---:|---:|---:|---:|
| material | 10,000/512 | 0.496429 ms | 0.5202/0.7933/0.8919 ms | 0.351712/0.356128/0.357952 ms | 949.220/956.253 MiB |
| transparency | 10,000/512 | 0.654886 ms | 0.5729/0.9441/1.0750 ms | 0.400288/0.408544/0.410048 ms | 949.220/956.253 MiB |
| lighting | 10,000/512 | 0.551567 ms | 0.5121/0.8460/0.9564 ms | 0.351456/0.357728/0.358880 ms | 949.220/956.253 MiB |
| geometry CPU | 10,000/512 | 0.670195 ms | 0.6896/0.9353/1.1004 ms | 0.388256/0.396352/0.400032 ms | 949.220/956.253 MiB |
| temporal proxy | 240/240 | 0.571076 ms | 0.4896/0.8508/1.0860 ms | 0.352608/0.359360/0.368896 ms | 949.220/956.253 MiB |

The wall average covers the full run. The listed static CPU/GPU percentiles cover
only the retained final 512 frames and are observations, not an accepted 10,000-frame
percentile baseline. The fresh results also differ materially from the earlier M0.5
run, reinforcing the need to record run environment and multi-run spread.

Memory scope remains sound: requested bytes are logical engine allocation payload,
committed bytes are Vulkan memory requirements, external swapchain commitment is
unavailable, and driver heap usage/budget remains separate.

## Required closure work

1. Add bounded streaming/online statistics, or an equivalently bounded export path,
   so median/p95/p99 and missing counts cover every measured frame. Test it beyond
   ring wrap and regenerate all long baselines.
2. Populate the frozen run header fields that the platform/backend can know. Record
   exact GPU/driver/Vulkan/swapchain/present/output state, compiler/toolchain, active
   layers, quality/reconstruction state, and a controlled cache-state label.
3. Add separately scoped cold import/upload timing or formally revise the frozen
   contract with an approved rationale and owning future milestone. Record at least
   five comparable Release runs and run-to-run spread.
4. Add CPU allocation count/bytes per frame with bounded, low-overhead collection,
   or formally amend the M0.2 requirement before acceptance.
5. Add a truthful transparent-overdraw measurement, such as optional pipeline
   statistics/full-screen equivalents with measured overhead, or amend the roadmap
   deliverable explicitly. Bucket packet counts alone are insufficient.
6. Add required deterministic nested-transparency and opaque-emissive coverage with
   captures, expected failure/behavior notes, counters, validation, and timings.
7. Rerun Debug/Release builds and tests, validation, all fixture images, the complete
   long-run suite, memory reconciliation, and external attribution where marker
   boundaries changed. Then write the final M0 completion report.

## M1 disposition

No `docs/milestones/M1-render-graph-hdr-color.md` is created by this audit because
the request makes that work conditional on genuine M0 completion. ADR-0002 remains
accepted direction, not an approved execution design. A future lead can resume by
reading the active M0 plan and this audit without relying on chat history.

No ADR is added, changed, or superseded by this audit.
