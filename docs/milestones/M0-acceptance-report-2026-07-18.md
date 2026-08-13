# M0 Final Acceptance Report

- Milestone: M0 - Reference Scenes and Profiling Foundation
- Status: Accepted
- Acceptance date: 2026-07-18
- Source baseline: `30252593f8fdd2de5dffbb8da31bb570ff49a7c0`
- Branch: `Render-Refactor-for-Modularity`
- Worktree: dirty with the ongoing RHI/M0 work; unrelated changes preserved
- Reference machine: RTX 4090, Core i9-14900K, 64 GB DDR5-6000, 3840x2160 display
- Governing plan: `docs/milestones/M0-reference-and-profiling.md`
- Reopen audit: `docs/milestones/M0-acceptance-audit-2026-07-18.md`

## Acceptance decision

M0 satisfies every frozen criterion in the governing plan and the seven closure
conditions in the integrated audit. The lead inspected the implementation and
artifacts directly, reran the integrated Debug and Release gates, visually inspected
the final fixture suite, and regenerated full-run CPU, GPU, memory, image, cold-start,
run-spread, allocation, and transparent-work evidence.

This acceptance establishes a regression and attribution foundation. It does not
claim that the small M0 proxy fixtures represent fully dressed gameplay or that the
legacy renderer is colorimetrically correct. M1 source implementation remains
unauthorized until its proposed execution plan is approved.

## Reopen-audit closure

| Audit closure condition | Final result |
|---|---|
| Full-run percentiles | Fixed-capacity per-range storage covers the complete contracted 10,000 frames while the detailed frame ring remains 512. Exact nearest-rank CPU/GPU summaries report 10,000 samples and zero missing samples for every long run. Overflow is explicit. |
| Frozen run metadata | JSONL headers now record compiler, shader compiler, OS, CPU, memory, GPU/UUID/driver, Vulkan device/loader/SDK, application layers, active tools, swapchain format/color space/present mode, output/base resolution/reconstruction/quality, cache state, exposure/operator, and startup phases. |
| Cold import/upload and spread | Five separately labelled fresh-process runs record startup, manifest verification, source import, upload bytes/batches/wait, and uncontrolled OS/driver-cache scope. Every required fixture has five independent Release runs. |
| CPU allocations per frame | A bounded global C++ `new/new[]` diagnostic records `allocation.cpp.calls` and `allocation.cpp.bytes`; it does not claim C allocation or driver allocation. |
| Transparent workload | Optional fence-delayed Vulkan pipeline-statistics queries record fragment invocations and fixed-point fullscreen equivalents on the original frame ID. Query state and overhead are explicit. |
| Missing fixture axes | `transparency_v1` revision 2 adds distinct concentric closed shells; `opaque_emissive_v1` adds a six-step opaque linear-emissive range. Both are hash-verified and required. |
| Integrated rerun | Debug/Release builds, 9/9 tests, 4K validation, long runs, paired captures, comparisons, memory reconciliation, and overhead gates pass. Existing Nsight marker attribution remains valid because no marker boundary changed. |

## Criterion-by-criterion review

### Invariants and M0.1

| Criterion | Result | Final evidence |
|---|---|---|
| RHI boundary is preserved | Pass | Backend facts use `RenderBackendRuntimeInfo`; Vulkan properties, query pools, enums, and readback remain Vulkan-owned. |
| No unexplained normal-frame stall | Pass | Timestamp and pipeline-statistics results are read only after existing frame fences and never use `VK_QUERY_RESULT_WAIT_BIT`. Capture readback is delayed. Upload waits remain separately labelled. |
| Capture identifies color/output domain | Pass | TGA sidecars identify legacy display-referred sRGB-target encoding, output operator, exposure state, swapchain/output configuration, fixture state, and hashes. |
| Source defects are not hidden | Pass | Fixtures and material provenance record unsupported/default behavior; source assets were not edited to make the car appear correct. |
| Dirty worktree is preserved | Pass | No reset, checkout, stage, commit, or unrelated reformat was performed. `imgui.ini` content still hashes exactly to the baseline blob. |
| Metric ownership and backend boundary reviewed | Pass | M0.1 contracts plus final source inspection identify every collection point and availability scope. |
| Asset licensing and deterministic cameras are known | Pass | Required assets are tracked/procedural and hash-verified; the licensed car remains an optional local diagnostic. Manifest cameras/state are deterministic. |
| Source/plan conflicts are logged | Pass | M0.1 and the reopen audit retain the corrected inventory and all superseded findings. |

### M0.2 and M0.3

| Criterion | Result | Final evidence |
|---|---|---|
| Aggregation, nesting, wraparound, and disabled behavior | Pass | `CpuProfilerTests` covers exact 10,000-frame CPU/GPU aggregation beyond the 512-frame ring, nearest-rank values, overflow, delayed attachment, nesting, and disabled behavior. |
| Multithread event collection | Pass | Existing eight-thread stress remains green. |
| Bounded storage and allocation behavior | Pass | Detailed storage is 512 frames; aggregate storage is 10,000 samples for 64 CPU and 32 GPU names. Profiling-enabled storage adds about 7.33 MiB; disabled construction allocates nothing. |
| CPU allocation count/bytes | Pass | Steady static Release tails report zero C++ allocations; the 240-frame temporal proxy reports median zero and maximum 13 calls/1,856 bytes. Scope is global C++ allocation only. |
| Timestamp conversion and frame/query reuse | Pass | Automated conversion/reuse tests pass; runtime GPU samples have zero missing results. |
| Vulkan validation | Pass | Final 3840x2160 Debug validation runs for nested transparency and opaque emissive emit no validation messages; each returns 8/8 CPU and GPU samples. |
| Unavailable queries do not block/corrupt | Pass | Availability is attached to the original frame; missing/unavailable/overflow counts remain separate. |
| External attribution | Pass | Retained Nsight Graphics 2026.2 evidence exercises all eight unchanged Debug Utils boundaries on two frames; 16/16 internal/external durations differ by 0 or 32 ns. A final rerun was attempted, but Windows rejected the elevated counter launch and produced no artifact. |
| Instrumentation overhead | Pass | Existing core-marker gates remain valid. Final optional transparent-query five-pair median deltas are +0.0064 ms CPU and +0.001568 ms GPU; median CPU-p99 delta is +0.0086 ms. The final five-repetition Release benchmark measured 7.2536 ns per tracked allocation. |

### M0.4 and M0.5

| Criterion | Result | Final evidence |
|---|---|---|
| Known glTF defaults and fixture values are tested | Pass | `MaterialProvenanceTests` and `BenchmarkManifestTests` validate source defaults, hashes, nested closed topology, primitive/material separation, opacity, and monotonic emissive strengths. |
| Debug values agree with captures | Pass | Opaque-emissive final code values are `36,69,130,209,233,245`; emissive-debug values are `0,49,94,156,188,213`, strictly increasing as authored strength rises. Existing base-color/normal/roughness/metallic/depth probes remain valid. |
| Fixtures reopen deterministically | Pass | Seven paired final/debug 4K comparisons plus the frame-120 temporal-cut pair are bit-exact. |
| Disabled debug view preserves final rendering | Pass | Existing default-final versus explicit-final exact comparison remains valid. |
| Every benchmark has reproducible state and expected behavior | Pass | Six required fixtures have versioned manifest state, content hashes, cameras, expected behavior/failure notes, and exact captures. The optional car diagnostic retains license/hash/provenance scope. |
| Baseline follows `FRAME_BUDGET.md` | Pass | Environment, render configuration, 10,000-frame percentiles, five-run spread, cold/import/upload scope, memory, counters, images, validation, and interpretation are all recorded below. |
| Captures support M1/M2 before/after use | Pass | Stable final display-referred captures and metadata exist. M1 must add a separate high-precision scene-linear artifact rather than relabel these images. |
| Car/glass/emissive failures are localized | Pass | Material defaults/unsupported extensions, hardcoded lighting/incomplete IBL, pre-glass tone mapping, second glass tone map, entity-origin sorting, and two-bucket transparency are distinct recorded causes. |
| Overhead and limitations are recorded | Pass | Core, query, allocation, capture-perturbation, external-profiler, and proxy-content limitations are durable. |

## Final build and runtime environment

- Debug and Release used MSVC `19.51.36248.0`, VS Ninja, glslc reported
  `shaderc v2023.8 v2025.5`, and Vulkan SDK `1.4.335`.
- Runtime: Windows `10.0.26220`, Intel Core i9-14900K, 68,466,892,800
  physical bytes, NVIDIA GeForce RTX 4090 UUID
  `3e45ec9c96fc398d79613146f37d826d`, driver `610.74`.
- Vulkan device/loader API: `1.4.341` / `1.4.341`.
- Swapchain: `VK_FORMAT_B8G8R8A8_SRGB`,
  `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, Mailbox, three images.
- Output: native 3840x2160, no reconstruction, legacy display-referred sRGB SDR,
  fixed M0 quality, validation disabled for performance.
- Post-suite `nvidia-smi` snapshot: P0, 2,580 MHz graphics, 10,501 MHz memory,
  73.05 W of a 450 W limit, 43 C. This is a labelled snapshot, not a per-frame
  clock trace.

Both build trees were configured from presets and independently checked. The Release
CMake cache and emitted JSONL headers both reported `Release`. Final CTest result is
9/9 in Debug and 9/9 in Release.

## Exact 4K steady-state baseline

The five long fixtures use 500 warm-up and 10,000 measured frames. The temporal
proxy intentionally uses its manifest contract of 0 warm-up and 240 frames. The
table reports full-run nearest-rank statistics; sample and missing counts are
10,000/0 for every long CPU and GPU range, and 240/0 for temporal.

| Fixture | Wall average | CPU median / p95 / p99 | GPU median / p95 / p99 |
|---|---:|---:|---:|
| Material laboratory | 0.520633 ms | 0.4878 / 0.7410 / 0.9322 ms | 0.351776 / 0.357536 / 0.361504 ms |
| Nested transparency | 0.608350 ms | 0.5701 / 0.7553 / 1.0487 ms | 0.425664 / 0.433248 / 0.749248 ms |
| Opaque emissive range | 0.390210 ms | 0.3686 / 0.4651 / 0.6338 ms | 0.280160 / 0.286048 / 0.288064 ms |
| Legacy lighting | 0.517353 ms | 0.4802 / 0.7432 / 0.8759 ms | 0.352000 / 0.360224 / 0.365184 ms |
| Geometry/CPU | 0.589300 ms | 0.6009 / 0.6537 / 0.7564 ms | 0.388416 / 0.394720 / 0.396896 ms |
| Temporal proxy | 0.482022 ms | 0.4512 / 0.6207 / 0.7558 ms | 0.351552 / 0.357600 / 0.367968 ms |

These are attribution proxies, not a dressed-gameplay performance claim.

### Five-run spread

Values are the median and min-max spread across five independent run wall averages
or per-run CPU/GPU medians. The transparency wall spread records substantial host
noise; its GPU medians remain tightly grouped.

| Fixture | Wall median [min,max] | CPU median-of-medians [min,max] | GPU median-of-medians [min,max] |
|---|---:|---:|---:|
| Material | 0.536951 [0.520633,0.540930] ms | 0.4874 [0.4845,0.4901] ms | 0.352224 [0.351776,0.352800] ms |
| Transparency | 0.785672 [0.685668,0.924452] ms | 0.6010 [0.5658,0.6245] ms | 0.428544 [0.427232,0.429632] ms |
| Opaque emissive | 0.428309 [0.375659,0.438489] ms | 0.3686 [0.3568,0.3921] ms | 0.280576 [0.280160,0.281216] ms |
| Lighting | 0.516913 [0.514626,0.517641] ms | 0.4523 [0.4483,0.4802] ms | 0.352992 [0.352000,0.353152] ms |
| Geometry/CPU | 0.604394 [0.589300,0.606130] ms | 0.5502 [0.5467,0.6009] ms | 0.388576 [0.388416,0.389792] ms |
| Temporal | 0.532122 [0.482022,0.553781] ms | 0.4560 [0.4512,0.4894] ms | 0.353856 [0.351552,0.354528] ms |

## Memory, upload, and transparent workload

Static fixtures use about 949.220 MiB logical requested live memory and 956.253 MiB
engine-committed live memory; nested transparency adds about 1 KiB. There are 35
peak tracked engine allocations. Driver heap/budget and external swapchain scope
remain separate.

At 4K with three swapchain images, each of the three `RGBA16F` GBuffer categories is
199,065,600 requested bytes. Opaque depth, glass depth, legacy scene color, legacy
scene copy, and external swapchain are each 99,532,800 bytes. These exact categories
are the M1 transient-lifetime baseline.

The optional nested-transparency query reports 2,055,678 fragment-shader invocations,
or 247,839 millionths (0.247839) of a 4K fullscreen, identically in all 2,560 retained
samples across five enabled runs. It is a fragment-work proxy, not an exact layer
count.

Five fresh-process material runs, explicitly labelled that OS/driver caches were
uncontrolled, report:

| Phase | Median [min,max] |
|---|---:|
| Startup total | 430.2685 [422.3325,454.9476] ms |
| Vulkan/backend initialization | 309.0697 [304.6200,317.9620] ms |
| Manifest verification | 1.5901 [1.5233,1.6244] ms |
| Source import | 26.7801 [26.3714,27.1414] ms |
| Upload submit/wait | 1.1457 [1.0435,1.3413] ms |

Each run submitted 1,100 upload bytes in two batches. The scope does not include a
future asset cooker or derived-data cache.

## Deterministic image evidence

All artifacts are Release, 3840x2160, post-transparency/pre-UI, legacy
display-referred sRGB-target captures. Paired runs pass strict metadata-aware
comparison with maximum RGBA delta 0, changed fraction 0, and mean luma SSIM 1.0.

| Fixture/state | SHA-256 |
|---|---|
| Material final, warm-up 500, measured frame 0 | `a0f404a5783d7af70d5d4b418d399397eeb09e6a1ab58f720e57725f8d5032d2` |
| Nested transparency final, r2, warm-up 500, frame 0 | `e53ab8570cc4e5e198c9a294ca06297df8ad9bead977cff32a334dd9e55aad10` |
| Opaque emissive final, warm-up 500, frame 0 | `78bab950c297577489d65642b7a758e7c9246b154ede2b6ac5ee2c395d0c51c2` |
| Opaque emissive debug, warm-up 500, frame 0 | `edabf19279b854890a1cb5da7b0de9c6e0158eed40b718bf0c5cba8c4b71dcef` |
| Legacy lighting final, warm-up 500, frame 0 | `04e5869ca1b1334a8b3834ad2cc814a1d6bdb8e896653e39efa0b9d5cf8c5d27` |
| Geometry/CPU final, warm-up 500, frame 0 | `0ea8c725383785e2287681fb71c3090c2c961f822344bccd93bcb6f9db5b1eac` |
| Temporal initial state, frame 0 | `062a5bd85eea339fae29a58b851cf1a4ec12159778b62e1e314928857df37895` |
| Temporal camera cut, measured frame 120 | `63149433c5e10152a512ce5657cf9048d7c96f25e24bb4fb04ad1d2b974d87d2` |

The manifest SHA-256 is
`673997b46611f1503ba7bade96c3a308f39f20526cb300f5545d175c1ccbbba8`.

## Remaining limitations and M1 handoff

- M0 captures are 8-bit legacy display-referred images. M1 must add a distinct
  high-precision scene-linear capture and retain final-output captures.
- Material ID and motion-vector images remain truthfully unavailable because the
  current renderer has no such targets.
- The fixtures are deliberately small deterministic attribution cases. Production
  lights, shadows, probes, GI, LODs, animation, particles, reconstruction, and RT
  counters become applicable only in their owning milestones.
- Capture still perturbs its selected frame and remains excluded from percentile
  timing.
- A changed marker set, new queue, or async schedule requires a new external trace.

No accepted ADR was contradicted or superseded. ADR-0002 receives an evidence
addendum for M1 resource/color design. `ROADMAP.md` marks M0 `Accepted`; M1 remains
`Proposed` pending approval of its execution plan.
