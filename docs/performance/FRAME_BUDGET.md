# Iridium Engine Performance Contract

## Reference target

- Hardware: RTX 4090, Core i9-14900K, 64 GB DDR5-6000, fast NVMe SSD.
- Display workload: 3840x2160, HDR where supported.
- **Raster goal (owner decision, 2026-10-02): 144 FPS at native 3840x2160 without
  ray tracing, in fully dressed, active gameplay scenes.** Base-render frame budget:
  **6.94 ms**. Native means the scene is shaded at output resolution; native
  temporal anti-aliasing (TAA/DLAA-class at 1:1) is allowed and expected.
- **Ray-traced goal:** when hybrid ray tracing (M11) is enabled, temporal
  reconstruction (DLSS/FSR/XeSS-class or native TAAU) may render below output
  resolution. The displayed target remains 144 FPS; reports state internal
  resolution, reconstruction mode, and displayed versus base-render frame time.
- Frame generation, if used, is reported separately and never satisfies the
  6.94 ms simulation/base-render target.
- The earlier ">100 FPS / 10.0 ms" contract governed M0-M7.8 evidence. Historical
  reports remain valid against the budget in force when they were written.

This is an engineering contract, not a promise that every pathological authoring case runs at 144 FPS. Quality tiers and explicit hero-material budgets should keep normal production content predictable.

## Measurement rules

- Use Release builds for performance decisions. Debug captures are diagnostic only.
- Record resolution, reconstruction mode and base resolution, output mode, scene revision, camera, quality settings, driver, GPU clocks/power behavior, and warm-up duration.
- Report median, 95th percentile, and 99th percentile frame time over a representative interval. Report one-percent-low FPS only as a supplement.
- Separate simulation, render preparation, submission, GPU execution, presentation, and asynchronous work.
- Do not compare results captured with different content, camera paths, shader-cache state, or output modes without labeling the difference.
- Treat image quality, temporal stability, memory, and latency as first-class results alongside average frame time.

## M6 high-fidelity multi-asset observation and M7 scaling gate

On 2026-08-23 the owner reported approximately 180 FPS with three newly acquired
high-fidelity assets active in the editor, versus approximately 1,700 FPS with an
empty scene on the same 240 Hz display. Those title values correspond to about
5.56 ms and 0.59 ms per completed frame, or roughly 4.97 ms of scene-dependent wall
time. The multi-asset result is already inside the 10.0 ms / greater-than-100-FPS
product target, but it is not yet a renderer qualification result: resolution,
camera, asset revisions, triangle/draw/material counts, shadow updates, and CPU/GPU
pass timings were not captured with the observation.

The title-bar value measures completed wall-clock frames in a coarse approximately
one-second window. Iridium currently prefers `VK_PRESENT_MODE_MAILBOX_KHR`, and
accepted M6 evidence has shown swapchain image acquisition dominating some CPU
totals while the GPU completed earlier. However, the observed counter exceeding the
240 Hz refresh rate and reaching roughly 1,700 FPS in the empty scene rules out a
180-FPS refresh ceiling in this case. The approximately 4.97 ms delta is a credible
scene-scaling signal; performance diagnosis still compares GPU-frame time,
non-waiting CPU work, and `cpu.renderer.acquire`/`cpu.renderer.present` to determine
which work owns it.

Current-source inspection nevertheless predicts near-linear scaling for dense
visible models until M7. Every enabled opaque submesh is extracted into a CPU
`DrawPacket`, sorted, and submitted through an individual `vkCmdDrawIndexed` call.
There is no general opaque main-view frustum or Hi-Z occlusion culling, and cooked
LOD/meshlet section fields are currently unpopulated. Classified transparent bounds
perform limited CPU rejection, and local shadow passes have view-specific sphere
culling, but the main opaque path, forward-complex work, and eligible shadow/capture
views still receive the full authored primitive detail. Three dense assets can
therefore increase CPU extraction/sort/record cost, vertex/triangle work, material
state changes, transparent pixels, and shadow casters roughly with their visible
content. Which term dominates this particular scene remains unknown until profiled.

Before M7 implementation, freeze a native-4K Release benchmark containing those
three asset revisions and fixed cameras for these cases: all visible; one and two
off-frustum; large occluder; near/mid/far LOD ranges; static transforms; one moving
asset; shadow-only caster; and representative transparency. Use at least five fresh
processes with the standard warmup/measured-frame protocol. Record:

- GPU median/p95/p99 for frame, GBuffer/visibility, cluster assignment, deferred,
  forward opaque, transparency, every shadow/capture pass, output, and UI;
- CPU extraction, culling, sorting, recording, asset/streaming work, frame-context
  waits, swapchain acquire/present waits, allocations, and worker utilization;
- requested versus visible instances/primitives/meshlets/triangles, LOD choices,
  indirect commands, draw/dispatch calls, material/pipeline binds, and occlusion
  rejection reasons;
- upload/residency bytes and peaks, plus image/capture comparisons at LOD and
  occlusion transitions.

M7 must demonstrate that off-screen and conservatively occluded content stops
generating main-view geometry work, unchanged static content produces no instance
upload, distant content selects bounded-error LODs without visible popping, and CPU
submission scales with compacted visible batches instead of source submesh count.
M8 then measures meshlet/normal-cone rejection for dense visible assets. Neither
milestone may trade away silhouettes, material response, correct transparent order,
or shadow/probe visibility to improve the counter.

## Import, cooking, and publication performance contract

Cook performance reports separate preparation/receipt, parse, material compile,
texture decode/mip/compression, geometry decode/optimization, LOD, meshlet, RT data,
parent serialization, DDC read/write, thumbnail, and GPU publication. Record wall
time, aggregate CPU time, worker occupancy, cache hits/misses, cancellation latency,
peak CPU memory, derived bytes, and upload bytes. A faster cook that oversubscribes
the machine, makes the editor unresponsive, or loses byte determinism is a failure.

M7 fine-grained cooking must prove these edit cases independently:

- material/policy-only: no texture recompression and no geometry/LOD/meshlet rebuild;
- one source primitive: only that primitive and its dependent children rebuild;
- one texture: only dependent semantic views/material parents rebuild;
- unchanged source: preparation receipt plus complete parent/child DDC hits;
- superseded edit: bounded cancellation and newest-revision-only publication.

The 2026-09-01 topology-transactional LOD checkpoint provides a concrete negative
control for this gate: adding one geometry-only LOD policy setting caused all 76
embedded texture views to miss and spent 457.241 of 462.547 cook seconds in texture
work. Treat parent-setting-coupled semantic texture keys as a measured M7.9 defect;
do not attribute that time to the 2.752-second LOD stage or accept it as the final
cache topology.

The 2026-09-02 M7.5 admission checkpoint is the runtime negative control. On the
frozen three-asset native-4K near/mid/far fixture, 2-pixel LOD selection reduces
triangles by 2.8282% but regresses the median of five 10,000-frame process averages by
1.5206%. Median GBuffer GPU time is unchanged; median GBuffer CPU selection grows by
0.0346 ms and compaction GPU time by 0.0064 ms. Do not promote a geometry-saving path
on triangle count alone: require non-regressing complete-path wall/CPU/GPU evidence
with the CPU oracle separated from the deployable configuration. See
`docs/performance/M7.5-topology-admission-2026-09-02.md`.

The follow-up implements that separation: validation or the explicit
`--gpu-lod-qualification-oracle` switch pays for CPU LOD command reconstruction and
exact comparison; deployable timing does not. Reusing invariant base-bounds projection
across each GPU chain reduces compaction further. The repeated five-process gate is
effectively wall-time neutral (+0.0378%), but complete GPU median remains +0.5642% and
tails regress. Treat this as accepted instrumentation/overhead repair, not LOD
production admission. See
`docs/performance/M7.5-lod-oracle-separation-2026-09-02.md`.

The physical-fallback qualification seam provides a measured residency control. With
device LOD pinned, floor one compacts 70 fine ranges out of the parent arena and
reduces live model index residency by exactly 1,737,714 bytes while preserving 303
commands, exact CPU/device parity, zero direct fallback, and a complete native-4K
image. Vertex residency is unchanged because M7.9 still owns independently resident
child vertex/index products. Do not count this fixed load-time floor as progressive
streaming or a production performance admission. See
`docs/performance/M7.5-physical-coarser-residency-2026-09-02.md`.

The final M7.5 workload gate admits generated LOD selectively rather than globally.
On the frozen 256-instance native-4K stress fixture, 2-pixel selection removes
12,650,344 of 51,612,672 main-view triangles (24.5102%). Across five fresh processes
per route and 100,000 aggregate measured frames, median complete-frame wall time
improves by 0.310995 ms (1.4487%), CPU median by 0.4550 ms, GPU median by
0.871328 ms, and CPU/GPU p95 and p99 all improve. Four of five matched pairs favor
the candidate; one reverses. Memory is identical, validation/oracle results are exact,
and the image changes 0.178494% of pixels at 0.9999969265 mean luma SSIM. Retain the
small three-asset negative control and LOD0 default: select generated LOD only for
workloads where complete-path evidence wins. See
`docs/performance/M7.5-material-workload-admission-2026-09-02.md`.

Independent child products may run in a bounded job graph, with texture compression
and geometry/LOD/meshlet work parallelized only where profiles show useful CPU work.
Progressive GPU residency keeps the last complete revision or a semantic
texture/coarser-LOD/proxy fallback visible while child products upload within a
per-frame byte/time budget. The current one-shot atomic model path remains a
compatibility fallback, not the intended steady solution for very large assets.

## 6.94 ms native-4K raster GPU budget (2026-10-02)

This table replaces the M0 10 ms hypothesis. It is a planning allocation, not a
scheduling model; rows may overlap if asynchronous compute is later admitted.
Measured context at adoption: the M5 dressed car measured 4.238 ms GPU median and
the M6 Alfa scene 5.821 ms, both **before** any AA, AO, GI, bloom, auto-exposure, or
motion-vector work existed. The raster path therefore needs real efficiency gains,
not only feature additions, to reach this target in dressed scenes.

| Area | Budget | Notes |
|---|---:|---|
| Visibility, depth, and surface data | 1.20 ms | Geometry plus GBuffer or visibility/material resolve; M7/M8 culling, LOD, and meshlets must earn this. |
| Shadows | 1.00 ms | Conventional cached maps by default; virtual shadows only where they win. |
| Direct lighting and IBL | 0.90 ms | Deferred plus complex-forward; includes clustered assignment. |
| Transparency and refraction | 0.70 ms | Ordinary scenes; marked hero glass is budgeted separately and reported. |
| Non-RT GI, AO, probes, and reflections | 1.20 ms | M10 technique mix (GTAO-class AO, probes/volumes, screen-space). |
| Native temporal AA | 0.40 ms | 1:1 TAA/DLAA-class; reconstruction below native is an RT-tier tool. |
| Post-processing, bloom, exposure, output | 0.50 ms | Includes HDR output transform; excludes separately timed UI. |
| UI, particles, and miscellaneous | 0.40 ms | Content dependent. |
| Scheduling margin | 0.64 ms | Spikes, p95/p99 headroom, and unrepresented features. |
| **Total** | **6.94 ms** | 144 FPS at native 3840x2160. |

The RT tier is budgeted at internal (reconstructed) resolution plus a separately
reported reconstruction cost; it defines its own table when M11 begins.

CPU work must not limit the 144 FPS target. Provisional targets on the reference
CPU: no more than 3.0 ms of serial main-thread simulation plus render preparation
and submission per frame, with additional work parallelized across worker threads;
every steady per-frame stage reports its critical-path and aggregate worker time.
Presentation/acquire waits are reported separately and never counted as CPU work.

### M7R CPU frame baseline (2026-10-04, `e98261b`)

M7R R5 measured the CPU target on the reference system (native 4K, Release,
A,B,B,A, 500 + 10,000 frames, quiet machine; `out/m7r/timing/r5-final`;
A = R4 accepted `9f2a28e`, B = R5 complete).

| Route | Serial main-thread non-wait CPU, A / B | CPU frame median, B | GPU median, B | Steady allocations, A / B |
|---|---|---|---|---|
| T-F1-all | 0.363 / **0.282** ms | 1.117 ms (GPU-bound) | 1.132 ms | 0 / 0 |
| T-F7-stack (7,232 opaque casters, Hi-Z + LOD) | 3.434 / **1.373** ms | 1.917 ms (GPU-bound) | 1.939 ms | 0 / 0 |
| T-F5-hetero (lit, local shadows) | 0.654 / **0.557** ms | 2.983 ms (GPU-bound) | 3.006 ms | 16 / **0** |
| T-F6-probecap (realtime probe capture) | 0.699 / **0.366** ms | 3.639 ms (GPU-bound) | 3.658 ms | 8 / **0** |

- Every route is GPU-bound, and the serial main thread is well under the 3.0 ms
  target. GPU medians are within noise of A (−0.3% to +0.2%).
- Extraction stages run as frame-critical task sets on the ADR-0015 task system.
- On T-F7, the stage critical paths (main thread) are 77 µs classify, 149 µs
  extract, 214 µs transparent sort and 184 µs intervals. Aggregate worker time is
  187, 528, 318 and 251 µs (`timing/r5c78-short2`, 2,000 frames).
- Steady-frame allocations are zero on every non-qualification route of the M7R
  sweep. `--qualification-allocation-trace` captures a stack for any regression.
- Before M7R, the T-F7 frame was 7.0 ms against the R0 worktree (CPU and GPU never
  overlapped, because of a fence-reuse bug fixed in M7R R4d). See the M7R plan's
  evidence sections.

### M9 temporal and post-processing admission (2026-10-06, `9c5f5fc`)

Five-process feature admission (`tools/m9/Run-FeatureAdmission.ps1`): native 4K, Release,
A,B,B,A,A,B,B,A,A,B, 500 + 10,000 frames, quiet machine (`-RequireQuiet`). A is the same
build with the feature off; B turns it on. Results are in `out/m9/timing/m9-adm-*`.

GPU frame median, B − A, in ms:

| Route | TAA | Auto-exposure | Bloom | All three |
|---|---|---|---|---|
| T-F1-all | +0.196 | +0.027 | +0.134 | +0.367 |
| T-F7-stack | +0.184 | +0.020 | +0.136 | +0.369 |
| T-F5-hetero | +0.213 | +0.028 | +0.139 | +0.374 |
| T-F6-probecap | +0.206 | +0.013 | +0.139 | +0.353 |

GPU pass times (B, median of run medians):

| Pass | Time (ms) |
|---|---|
| `temporal.taa` | 0.191–0.207 |
| `post.exposure.histogram` + `post.exposure.adapt` | 0.013–0.028 + 0.008–0.011 |
| `post.bloom` | 0.108–0.113 |
| Output transform with bloom sampling | +0.02–0.04 |

Against the budget table:

- **Native temporal AA (0.40 ms):** about 0.25 ms used. That is the TAA pass at 0.20 ms
  plus the M9.1 velocity target at about 0.05 ms, which is present on both sides and
  charged to this row.
- **Post-processing, bloom, exposure, output (0.50 ms):** about 0.21–0.23 ms used
  (bloom 0.11, exposure 0.03, output 0.075–0.095).
- **Total:** the heaviest admission route, T-F6-probecap, goes from 3.60 to 3.95 ms GPU
  with all three on.

Memory, committed graph:

- TAA adds 127.5 MB, its 4K RGBA16F History pair.
- Exposure adds a 16 B History buffer pair plus a 255 KB transient.
- Bloom adds nothing: its half-resolution mip chain aliases.

Steady allocations are 0 on every route and side. Since M9.7 these three features are
the **product defaults**. Measurement tools pin the M7R route (`--anti-aliasing none
--exposure manual --bloom off`) through `Get-M7REngineBaseArgs`, so the frozen set and
older baselines stay comparable.

**F6 re-measure** (the M7R watch item; `out/m9/timing/m9-f6-remeasure`; A = M7R final
`da8e4e8`, B = M9 with the measurement route pinned):

- T-F6-probecap GPU changed by +0.057 ms (+1.6%). Passes: `forward.opaque` +0.040 and
  `gbuffer.opaque` +0.012 (the M9.1 velocity targets), and `frustum_compact` +0.002 (G8
  deterministic compaction). Under 0.005 ms (about 0.1%) remains unattributed.
- No placement-attributed regression above 1% remains, so no placement policy is
  applied (director decision rule).

## Evidence tiers (owner decision, 2026-10-02)

| Change type | Required evidence |
|---|---|
| **Refactor / behavior-preserving** (build, code motion, API reshaping, allocator or synchronization changes that must not alter output) | Debug and Release builds and tests; Vulkan validation clean on representative fixtures; byte-identical scene-linear and final-output captures on the frozen fixture set; one matched native-4K Release timing pair (before/after, reversed order) showing no CPU/GPU median or p99 regression beyond noise. |
| **Feature admission / optimization promotion** (new visible feature, new default, technique replacing another) | The full protocol: five fresh native-4K Release processes per route, median/p95/p99, matched captures with per-fixture thresholds, memory and counters, as defined below and in the milestone contract. |

A refactor that cannot remain byte-identical (for example, floating-point
reassociation from reordered work) must state why and use the feature-admission
image thresholds for the affected fixtures.

## Required counters

### CPU

- total frame, simulation, scene update, animation, render extraction, culling, packet/update generation, submission, editor, streaming, and blocking waits;
- job counts, worker utilization, allocations and bytes allocated per frame;
- changed versus total instances/materials/lights;
- draw/dispatch/API call counts.

### GPU

- timestamp ranges for every major pass and queue;
- primitives/triangles submitted and surviving visibility where available;
- visibility-buffer pixels/identities, material-resolve pixels, reconstructed
  attributes, and material/texture divergence when that path is active;
- indirect command and visible-instance counts;
- transparent pixel/layer/overflow statistics;
- light/shadow/probe counts and update work;
- history invalidations and reconstruction mode;
- RT rays, instances, build/update cost, and denoiser cost when introduced.

### Memory and streaming

- persistent and transient GPU allocations by category;
- peak render-graph transient use and aliasing efficiency;
- upload bytes, staging pressure, residency changes, and evictions;
- asset derived-data size and load/cook/upload time;
- CPU resident asset and scene memory.

## Fidelity and bandwidth policy

High VRAM permits richer assets and history, but it does not make GBuffer bandwidth free. At 4K, an additional full-screen 8-byte target is roughly 66 MiB of storage and may be read/written multiple times each frame. The relevant costs include memory bandwidth, cache locality, ROP traffic, synchronization, and power, not merely allocation capacity.

Therefore:

- retain a high-precision reference path;
- propose packing only with an equivalent-image comparison and measured frame-time benefit;
- reject optimizations that introduce visible banding, unstable normals, broken highlights, or temporal artifacts in reference scenes;
- allow a high/hero quality path when its cost is spatially bounded and measurable;
- prefer eliminating unused data and redundant passes over reducing precision blindly.
- compare the complete visibility + material resolve + surface cache + lighting cost;
  a smaller visibility attachment is not a win if reconstruction, divergence, or a
  redundant full GBuffer moves more cost elsewhere;
- do not keep a full production GBuffer and visibility buffer live together except
  for controlled validation or measured downstream reuse.

## High-fidelity visibility and atmosphere guardrails

ADR-0010 adds quality tiers; it does not increase the 6.94 ms native-4K raster
budget. On the RTX 4090 reference, the ordinary dressed-scene target is 1.0 ms for
all shadow rendering/filtering and 1.2 ms for the complete non-RT
GI/AO/probe/reflection mix (these were 1.5 ms each under the former 10 ms
contract, and M7-era reports that cite 1.5 ms were judged against that value). Cinematic/hero overrides may exceed an individual row only when spatially
bounded and when the total frame, tail latency, and memory remain reported. RTX 5090
results are useful additional evidence but never replace the fixed 4090 comparison.

Future shadow reports must separate conventional-map raster, virtual page marking/
culling/raster/filtering, screen-space contact, temporal denoising, translucent RGB
visibility, and RT visibility. Required counters include shadowed lights per pixel,
requested/resident/rendered/cached/invalidated pages, blocker/filter samples, rays,
history rejection, owner omissions, update latency, and physical/transient VRAM.
Conventional directional reports also record configured coverage distance, cascade
near/far ranges, world units per texel, projected texel size, penumbra radius, filter
sample counts, receiver-plane bias, and bounded geometric-normal offset.

Future AO reports must separate authored material AO, GTAO/CACAO, bent-normal and
specular occlusion, temporal/spatial filtering, probe/distance-field visibility, and
RTAO. Ground-truth error, haloing, off-screen failure, thin-object loss, motion
stability, and double-darkening are acceptance criteria alongside pass time.

Sky/atmosphere reports separate environment cooking/startup publication from steady
background, IBL, atmosphere, aerial perspective, cloud lighting, and temporal work.
HDRI background and IBL controls must not add an extra full-screen buffer solely for
settings storage; simulated atmosphere/cloud resources require explicit persistent/
transient accounting and can share histories only with proven lifetime correctness.

## Benchmark scene set

- Material laboratory: dielectric, conductor, specular/glossiness, clearcoat, normal detail, transmission, absorption, and emissive range.
- Sample car: paint, windows, headlight ridges, emissive lamps, and material-default diagnostics.
- Transparency torture scene: intersecting surfaces, nested shells, particles, thin glass, rough refraction, and off-screen samples.
- Lighting scene: many local lights, sun/sky, shadow casters, probes, emissive surfaces, and mixed dynamic/static content.
- Geometry/CPU scene: many instances, many submeshes/materials, LOD transitions, animation, and frequent transform changes.
- Temporal scene: disocclusion, foliage/coverage, emissive motion, transparent motion, specular aliasing, and camera cuts.

Each benchmark needs a fixed camera path or deterministic state and a documented expected visual result.

## Baseline report format

For each run record:

1. build/commit or worktree state and configuration;
2. hardware, driver, display/output, and render settings;
3. scene and camera path;
4. CPU/GPU percentile timings and pass breakdown;
5. persistent/transient memory;
6. relevant content/counter totals;
7. screenshots or scene-linear captures;
8. validation errors, stutters, visual defects, and interpretation.

## M2 accepted material/surface baseline

Accepted 2026-07-25 on the reference RTX 4090 at 3840x2160. Five independent
Release runs used 500 warm-up and 10,000 measured frames on `material_lab_v1`.

| Metric | Five-run median |
|---|---:|
| CPU frame | 0.631800 ms |
| GPU frame | 0.455296 ms |
| canonical R GBuffer | 0.012288 ms |
| deferred lighting | 0.124928 ms |
| opaque complex forward | 0.015360 ms |
| output transform | 0.038912 ms |
| UI | 0.106496 ms |
| requested live / peak | 861.158 / 893.159 MiB |
| committed live / peak | 931.003 / 963.005 MiB |

The 36-byte/pixel R cache is production. Q and C reduce the matched
GBuffer+lighting pair from 0.137216 ms to 0.109568 ms and 0.065536 ms, but are
rejected because both omit scalar F90 and full metadata. C's extra format split also
prevents one graph alias, so it does not reduce requested memory below Q.

The proxy is intentionally small and does not claim dressed-scene performance.
Steady C++ allocation median and p99 are zero calls/bytes. The 6.497 MiB requested
peak increase over M1 is the accounted pair of persistent schema-2 GPU material
buffers. M5/M7 must measure their full clustered-lighting or
visibility+resolve+cache+lighting chains against this baseline.

## M3 accepted asset/runtime baseline

Accepted 2026-07-31 on the reference RTX 4090 at 3840x2160. Five independent
Release sample-car runs used 500 warm-up and 10,000 measured frames while loading a
self-contained cooked schema-3 artifact through the indexed production path.

| Metric | Five-run median |
|---|---:|
| wall frame average | 3.5397 ms |
| CPU frame median | 1.7080 ms |
| CPU frame p95 / p99 | 15.4599 / 16.2975 ms |
| asset runtime tick p99 | 0.0029 ms |
| editor build median | 0.0537 ms |
| GPU frame median | 0.8450 ms |
| GPU frame p95 / p99 | 2.2391 / 3.3213 ms |
| canonical GBuffer median | 0.1741 ms |
| deferred lighting median | 0.0573 ms |
| UI median | 0.1096 ms |
| requested live / peak | 897.558 / 972.257 MiB |
| committed live / peak | 967.567 / 1,042.265 MiB |

The CPU tail is swapchain-acquire/presentation waiting while the hidden benchmark
outruns presentation. Asset work and editor construction are not responsible for
that tail. The GPU median improves on the corrected M2 sample-car baseline of
0.955 ms; p99 remains well below the 10 ms base-frame contract.

A separate 10,000-frame allocation gate recorded 0.836768 / 2.128608 / 3.003328 ms
GPU median/p95/p99 and 1.5564 ms CPU median. Steady allocation calls and bytes are
zero at median and p99, with no dropped profiler counters. Startup submitted
78,327,200 bytes in one upload batch; startup work is excluded from the steady
frame contract and remains separately attributed.

M3 scale fixtures establish these non-frame gates:

| Asset/runtime gate | Accepted result |
|---|---:|
| 100,000-record catalog warm query p95 | 0.3034 ms |
| 100,000-record catalog incremental query p95 | 0.0878 ms |
| DDC lookup p95 | 0.043 ms |
| coalesced reimport schedule p99 | 0.0001 ms |
| runtime publisher schedule/publish p99 | 0.0005 ms |
| indexed material table | 65,536 resident records |
| indexed texture/sampler table | 8,192 views and 8,192 samplers |
| reverse dependency fan-out | 10,000 dependents |
| rapid reimport | 1,024 revisions; newest-only publication |

The full criterion evidence and deterministic artifact hashes are in
`docs/milestones/M3-acceptance-report-2026-07-31.md`. M4/M7 must compare future
scene serialization and persistent GPU-scene work against this cooked-only,
GUID-indexed baseline. M5/M6 must not attribute authored-material or
coverage-dependent lighting/transparency cost to M3 asset work without matched
captures and counters.

## M4.4 accepted cooked-scene baseline

Accepted 2026-08-02 in Release on the reference CPU. Independent 1k/10k/100k
processes used one warmup and five measured samples. The deterministic fixture has
one fixed-width backend-neutral component per entity; current-schema source staging
is outside the timed compiler work.

| Cooked-scene gate | 1k | 10k | 100k |
|---|---:|---:|---:|
| artifact bytes/entity | 56.720 | 56.072 | 56.007 |
| cold compile + serialize median | 1.497 ms | 16.489 ms | 180.587 ms |
| warm full-artifact DDC read median | 0.706 ms | 6.009 ms | 62.028 ms |
| artifact validate median | 0.543 ms | 5.380 ms | 56.624 ms |
| CPU-ready stage median | 1.112 ms | 12.193 ms | 148.105 ms |
| CPU-ready stage p95 | 1.146 ms | 13.120 ms | 254.994 ms |
| active-world commit median | 0.0000 ms | 0.0002 ms | 0.0007 ms |
| active-world commit allocations | 0 | 0 | 0 |

The 100k stage requests 96.29 MiB through 800,271 C++ allocations. This is
one-shot load work, not a steady-frame result, and its p95 is a recorded risk for
future scheduled/incremental loading. No renderer or GPU resource contract changed,
so the M3 4K GPU/VRAM baseline remains current. Full protocol and machine-readable
evidence are in `docs/performance/M4.4-cooked-runtime-scenes-2026-08-02.md`.

## M4.8 accepted ECS storage baseline

Accepted 2026-08-02 in Release on the reference CPU. Component pools retain dense
component/entity arrays and use demand-paged 32-bit sparse indices. One warmup and
30 measured fixed-seed samples establish these p95 gates:

| ECS/editor gate | Accepted result |
|---|---:|
| 100k entities, 1M random component hits | 10.911 ms |
| 100k entities, 1M random component misses | 1.257 ms |
| 100k Transform + Mesh view | 0.882 ms |
| 100k Transform + Relationship view | 0.717 ms |
| 100k depth hierarchy traversal | 1.061 ms |
| 100k breadth hierarchy traversal | 0.199 ms |
| 100k editor hierarchy snapshot/sort | 10.652 ms |
| 100k entities, 1M editor selection lookups | 5.059 ms |
| 100k dense Transform, 10M iterations | 23.737 ms |

Dense iteration is 1.0% faster than the sparse-map comparator; all selected lookup,
view, hierarchy, and editor paths exceed the 15% improvement gate. The accepted
five-run 4K sample-car check retains 1.2682 ms CPU median, 0.9481 ms GPU median,
byte-identical M3 VRAM, and zero steady C++ allocations at median/p99. Full protocol
and raw-data links are in
`docs/performance/M4.8-paged-sparse-index-2026-08-02.md`.

## M4 final accepted scene/editor baseline

Accepted 2026-08-03. The production serializer cutover preserves the M4.4 100k
cooked artifact size at 5,600,720 bytes. Current Release validation is 56.440 ms
median, CPU-ready staging is 136.526 ms median / 203.157 ms p95, and active-world
commit is 0.0007 ms with zero allocations. Source JSON remains editor/cook-host
only; the 10k strict parse is 165.123 ms median and verified atomic save is
419.593 ms median.

Five independent 3840x2160 sample-car runs with 500 warmups and 10,000 measured
frames each complete with zero drops. Cross-run medians are 2.8883 ms CPU and
1.5662 ms GPU; requested live/peak memory is 897.558/972.257 MiB and committed
live/peak is 967.567/1,042.265 MiB, byte-identical to M3. C++ allocation median and
p99 are zero. The validation capture is pixel-identical to M3.7. See
`docs/performance/M4.10-production-cutover-2026-08-03.md`.

## M5.0/M5.1 accepted lighting-contract baseline

Accepted 2026-08-08. M5.0 preserves the M4 sample-car final SDR byte-for-byte and
records 250,000 measured 4K frames across five fixtures with zero drops and clean
validation captures. It changes no production renderer path. A retained allocation
counter discrepancy of one call/eight requested bytes versus M4's accepted zero is
open and must be resolved or explained before M5 final acceptance.

M5.1 advances `iridium.component.light` source/cooked data to version 2 without GPU
consumption. A deterministic 1,000-light Release workload reads and visibly migrates
v1 in 26.190 ms median / 29.441 ms p95, reads current v2 in 21.357 / 22.604 ms, and
stages plus cooks v2 in 8.556 / 9.081 ms. The 10,000-target editor transaction gate
remains 0.1072 ms median / 0.1204 ms p95 with one 120-byte apply allocation and
allocation-free undo/redo. No frame-time or VRAM budget is charged until M5.2 adds
extraction. See `docs/performance/M5.1-light-component-v2-2026-08-08.md`.

## M5.2 accepted GPU light-record baseline

Accepted 2026-08-09. The common 256-light table adds 32 KiB of persistent Vulkan
storage across two frame contexts. A validated 4,096-light table uses 512 KiB and
publishes one 256 KiB range to each frame context before returning to zero upload.
Release steady extraction is allocation-free: 256 unchanged lights cost 0.0417 ms
median / 0.0559 ms p95; 4,096 unchanged lights cost 0.9112 / 1.4993 ms. The
65,536-light diagnostic ceiling costs 26.5381 / 30.4264 ms and is explicitly outside
the gameplay frame budget. No shading cost is charged yet. The matched Release 4K
capture is byte-identical to M5.0. See
`docs/performance/M5.2-gpu-light-records-2026-08-09.md`.

## M5.3 accepted clustered-assignment budget

Accepted 2026-08-09 on the reference RTX 4090. The selected 32x32x24 logarithmic
grid costs 0.241 ms median / 0.259 ms p95 for the final 512-light 4K fixture and
0.0189 / 0.0203 ms with zero lights. It requests 18.991 MiB per frame context and
adds 37.982 MiB across the two-context graph relative to M5.2. A 4,096-light dense
diagnostic intentionally exceeds normal capacity, switches wholly to a deterministic
top-64 fallback, and costs 1.911 / 1.921 ms. M5.4 must measure cluster consumption
inside the remaining 1.4 ms direct-light/IBL envelope. See
`docs/performance/M5.3-shared-clustered-assignment-2026-08-09.md`.

## M5.4 accepted clustered direct-light budget

Accepted 2026-08-09 on the reference RTX 4090. Authored lights are now the sole
production direct-light source. At 4K the canonical deferred direct plus current-
environment pass costs 0.123 ms median / 0.125 ms p95 with the 512-light spatial
stress distribution; the forced-forward standard contribution costs 0.061 / 0.065
ms. Cluster construction remains 0.196 / 0.210 ms in that run. The complete GPU
frame is 0.559 / 0.570 ms deferred and 0.698 / 1.048 ms with the forced-forward
surface. Direct-only deferred/forward parity passes at one maximum SDR code value
and 0.999996 mean luma SSIM. See
`docs/performance/M5.4-clustered-direct-lighting-2026-08-09.md`.

## M5.5 accepted cooked-environment and complete-IBL budget

Accepted 2026-08-09 on the reference RTX 4090. The 512/32/256/256 High cooked
environment adds 20.297 MiB requested / 20.362 MiB committed persistent memory and
20.297 MiB of startup upload over M5.4's neutral product. At 4K, deferred direct
plus complete irradiance/prefilter/BRDF IBL costs 0.110 ms median / 0.119 ms p95,
down from M5.4's 0.123 / 0.127 ms raw-environment approximation. The forced-forward
standard surface costs 0.055 / 0.059 ms and complex forward buckets remain 0.002-
0.005 ms median in the closure lab. These ranges remain well within the 1.4 ms
direct-light-plus-IBL allocation. Atomic editor replacement temporarily doubles the
environment category and pays a synchronous frame-context wait; steady residency
returns to one product. See
`docs/performance/M5.5-cooked-environment-ibl-2026-08-09.md`.

## M5.6 accepted directional-shadow budget

Accepted 2026-08-09 on the reference RTX 4090. The High directional product is one
persistent four-layer 2048x2048 D32 array: exactly 64 MiB requested and committed.
A static 4K fixture records four cache hits and no shadow pass. A moving caster
refreshes all four cascades at 0.0171 ms median / 0.0177 ms p95 GPU and 0.0216 /
0.0271 ms CPU recording. Deferred direct plus complete IBL and 5x5 tent shadow
sampling costs 0.272 ms median, remaining within the 1.4 ms lighting envelope.
The complete cache-hit/update GPU frames are 0.654 / 0.995 ms and 0.669 / 1.022 ms
median/p95 respectively, with zero dropped frames. See
`docs/performance/M5.6-directional-shadows-2026-08-09.md`.

## M5 final accepted dressed-lighting baseline

Accepted 2026-08-13 on the reference RTX 4090/Core i9-14900K. Five independent
Release processes use native 3840x2160, 500 warm-up frames, 10,000 measured frames,
validation off, Ultra PCSS, the cooked 118-primitive/87-material car, a cooked 4K
HDRI, and three independent shadow owners. All 50,000 frames and all 144 retained
per-frame counters complete without drop or overflow.

| Final M5 gate | Accepted result |
|---|---:|
| GPU median of medians | 4.238432 ms |
| GPU worst p95 / p99 | 4.484928 / 4.520544 ms |
| CPU median of medians | 4.5181 ms |
| CPU worst p95 / p99 | 4.7818 / 4.8439 ms |
| cluster assignment median | 1.929536 ms |
| deferred / complex-forward median | 0.334848 / 1.249280 ms |
| output transform median | 0.061440 ms |
| requested live / peak | 1,492.982 / 1,587.977 MiB |
| committed live / peak | 1,563.056 / 1,658.052 MiB |
| directional / local shadow reservation | 128 / 400 MiB |
| steady C++ calls / requested bytes | 39 / 5,288 |

The dressed GPU p99 consumes 45.2% of the 10 ms base-frame budget. The current
cluster-assignment cost exceeds the initial 1.4 ms direct-light/IBL hypothesis when
accounted alone, but the complete dressed frame—not additive row assumptions—still
has 5.479 ms of GPU margin. M7 visibility/GPU-scene work should target that 1.93 ms
cluster stage and restore the M4 zero-allocation steady-frame standard with
persistent frame-context scratch. See
`docs/performance/M5.11-production-qualification-2026-08-13.md`.

## M5.12 reflection-resolution checkpoint

Implemented 2026-08-13 as post-acceptance hardening. The high-end HDRI default is
now a 1024-face prefiltered specular cube with user-selectable lower and higher
recipes. Against the accepted 256-face Belfast product, one matched native-4K
Release process adds exactly 108 MiB of persistent environment residency:
29.276 to 137.276 MiB. CPU median is 6.461 versus 6.441 ms and GPU median is 6.010
versus 5.879 ms, so the larger product has no measured median frame-time charge in
this checkpoint. Environment creation rises from 0.483 to 2.855 seconds. These are
single-process stabilization measurements, not a replacement for the five-process
M5 gate. The general editor upload budget remains a 128 MiB per-tick scheduling
target. An explicit atomic HDRI publication may exceed it under a 640 MiB
per-environment cap; since M6, a single valid model may also publish atomically
under a 1 GiB per-model hard cap rather than being rejected solely for exceeding
128 MiB. See
`docs/performance/M5.12-reflection-resolution-stabilization-2026-08-13.md`.

## M6.8 steady-allocation checkpoint

Accepted 2026-08-26 on the reference RTX 4090/Core i9-14900K. The inherited M6.7
steady-frame sample was 17 C++ allocation calls and 5,216 requested bytes per frame.
Persistent transform and shadow-mapping scratch, allocation-free sorted owner
validation, and static deep tile-termination pass identities reduce this to exact
zero calls and zero requested bytes across 96 measured 1280x720 Release frames: 32
each for Ordinary2, Cinematic8, and WeightedOIT after 20 warmups. A separate Vulkan-
validation capture is byte-identical to the pre-hardening overflow reference and
retains the same 650 residual-tail pixels. Capture/readback output allocations are
deliberately excluded from the steady-frame claim. No graph resources, descriptors,
or rendered output changed. M6.9 must repeat allocation checks in its dressed
production qualification rather than generalizing this representative-fixture result
to every future scene composition.

## M6.10 procedural-grid checkpoint

Measured 2026-08-27 at 1280x720 Release after 20 warmups and across 500 retained
frames. The active infinite, depth-aware procedural editor grid raises the output-
transform median from 0.006144 ms to 0.013312 ms, approximately 0.007 ms. The active
run records a 0.129664 ms total GPU-frame median, 0.5206 ms CPU-frame median, and zero
dropped frames. This is a focused low-resolution editor-overlay check, not a
replacement for the frozen native-4K M6 qualification. Benchmarks, renderer captures,
and asset previews keep the grid disabled.

## M6.11 runtime display-transport checkpoint

Measured 2026-08-28 at 1280x720 Release with Vulkan validation enabled on the
reference system. One hidden-window process rebuilt SDR -> scRGB -> HDR10 -> SDR at
successive frame boundaries without validation output or scene reload. The three
cutovers measured 215.3441 ms, 179.2803 ms, and 223.7006 ms. SDR/HDR family changes
include replacement ACES LUT upload and binding; scRGB and HDR10 share the same
P3-D65 LUT and do not reupload it. This is deliberate display-mode latency, not a
steady-frame cost: the selector performs no polling, allocation, or graph mutation
until the user changes transport (or a normal swapchain recreation reevaluates Auto).

## M7.7 directional device-command checkpoint

Measured 2026-09-13 at 1280x720 Release with Vulkan validation enabled, 8 warmups,
and 16 measured frames. The 48-caster qualification refreshes two cascades and
retains 30 commands. Device compaction plus indirect graphics costs 0.022272 ms
median versus 0.015264 ms for the direct reference, while median CPU shadow
recording falls from 0.3004 to 0.1526 ms. All device bin counts match the CPU
oracle, overflow is zero, and paired final-SDR captures are byte-identical. This is
an architecture checkpoint rather than the M7.7 scale gate; the subsequent
spot/point correctness checkpoint is accepted, while the larger heterogeneous
quality matrix remains open. See
`docs/performance/M7.7-directional-shadow-device-commands-2026-09-13.md`.

## M7.7 local-shadow device-command checkpoint

Measured 2026-09-13 at 1280x720 Release with Vulkan validation enabled. Spot and
point automatic/direct-reference captures are byte-identical; 9 spot commands and
90 point-face commands match their CPU oracles with zero mismatch or overflow.
The retained Release samples contain only the first refreshed frame, so their
0.010656 ms spot and 0.010208 ms point compaction costs are diagnostics rather than
admission statistics. Initial double-buffered local command storage is 6.03 MiB;
the combined bounded growth ceiling is about 100 MiB. Warmed heterogeneous timing
and residency refinement remain required. The follow-up gates CPU view visibility
out of production device submission; a matched 35 m directional run records
0.0268 ms median / 0.0317 ms p95 on the CPU with the accepted image unchanged.
See `docs/performance/M7.7-local-shadow-device-commands-2026-09-13.md` and
`docs/performance/M7.7-shadow-command-oracle-gating-2026-09-13.md`.

## M7.7 independent probe-visibility checkpoint

Measured 2026-09-13 at 1280x720 Debug with Vulkan validation. A scheduled six-face
reflection capture over nine primitives submits 36 bytes of independent compact
GPU-scene references instead of 2,160 bytes of main-view packets. Eighteen owned
primitive-face pairs are self-excluded; the remaining 36 tests reject three and
draw 33. Invalid references are zero, and compact/direct final-SDR
captures are byte-identical. This is an architecture/correctness checkpoint, not a
4K performance admission: device-built capture commands, probe LOD, and warmed
heterogeneous update timing remain open. See
`docs/performance/M7.7-independent-probe-visibility-2026-09-13.md`.

## M7.7 GPU-scene probe-capture graphics checkpoint

Implemented 2026-09-14 and qualified at 1280x720 Debug with Vulkan validation.
All 33 non-owner visible face draws now use persistent GPU-scene transform and
material identity; the isolated direct-probe reference retains the same 18 owner
exclusions, 36 face tests, and three culls. The scene-linear A/B maximum absolute
difference is one R16 quantization step (`0.0009765625`) with a
`0.0000194135` mean. This is an architecture/correctness checkpoint, not a timing
admission; CPU visibility and draw emission remain until the next bounded device-
command slice. See
`docs/performance/M7.7-probe-gpu-scene-capture-pipeline-2026-09-14.md`.

## M7.7 reflection-probe device-command checkpoint

Implemented 2026-09-14 and qualified at 1280x720 Debug. A six-face capture over
nine compact probe primitives emits 33 commands across three bins; every count
matches the CPU oracle, overflow is zero, and 18 owner primitive-face pairs are
excluded on device. The deployable no-validation route records zero GPU-owned CPU
face tests while retaining delayed exact device counts. Its scene-linear capture
is byte-identical to the preceding GPU-scene direct-draw checkpoint, so the direct-
shader A/B remains inside the accepted one-R16-step envelope. Initial bounded
double-buffered storage is about 0.58 MiB. First-capture Debug timings are diagnostic
only; probe LOD and warmed heterogeneous admission remain open. See
`docs/performance/M7.7-probe-device-commands-2026-09-14.md`.

## M7.7 face-invariant probe-LOD checkpoint

Implemented 2026-09-14 and qualified at 1280x720 Debug plus a no-validation
Release deployment check. Independent radial selection reduces 7 of 226 visible
probe commands in the near/mid/far fixture, from 603,932 to 599,424 triangles.
Device and full CPU command oracles agree exactly with zero overflow, and the
scene-linear capture is byte-identical to LOD0. Production reports no CPU-owned
face tests or oracle commands. LOD0 remains the default until a reflection-
sensitive fixture and warmed threshold sweep establish visual/performance
admission. See `docs/performance/M7.7-probe-radial-lod-2026-09-14.md`.

## M7.7 directional shadow-LOD checkpoint

Implemented 2026-09-14 and qualified at 1280x720 Debug with Vulkan validation plus
a no-validation Release deployment check. A 2-shadow-texel density bound reduces
21 of 1,243 visible cascade commands in the dense near/mid/far fixture, from
3,321,626 to 3,308,102 shadow triangles. Device and complete command oracles agree
with zero mismatched bins/regions, and the scene-linear capture is byte-identical
to LOD0. Production reports zero CPU caster tests and oracle work. LOD0 remains the
default pending native-4K silhouette, cascade-transition, and warmed heterogeneous
admission. See
`docs/performance/M7.7-directional-shadow-lod-2026-09-14.md`.

## M7.7 face-invariant point-shadow LOD checkpoint

Implemented 2026-09-14 and qualified at 1280x720 Debug with Vulkan validation plus
a no-validation Release deployment check. A 2-shadow-texel radial bound reduces 16
of 565 visible cube-face commands, from 1,509,830 to 1,498,394 shadow triangles.
Device and complete command oracles agree with zero mismatch/overflow. The maximum
scene-linear AP1 difference from LOD0 is `5.96046448e-7`, and no pixel exceeds
`1e-6`. Production reports zero CPU point-caster tests and oracle work. LOD0 remains
default pending native-4K silhouette, motion, resolution, and warmed admission. See
`docs/performance/M7.7-point-shadow-radial-lod-2026-09-14.md`.

## M7.7 projected spotlight-shadow LOD checkpoint

Implemented 2026-09-14 and qualified at 1280x720 Debug with Vulkan validation plus
a no-validation Release deployment check. A 2-shadow-texel projected bound reduces
10 of 339 visible spotlight-tile commands, from 905,898 to 899,522 shadow triangles.
Device and complete command oracles agree with zero mismatch/overflow, and the
scene-linear capture is byte-identical to LOD0. Production reports zero CPU
spotlight-caster tests and oracle work. LOD0 remains default pending native-4K
silhouette, cone/tile-size, motion, atlas-cache, and warmed admission. See
`docs/performance/M7.7-spot-shadow-projected-lod-2026-09-14.md`.

## M7.7 heterogeneous shadow-command admission

Accepted 2026-09-19 at native 4K after two 120-warmup/600-frame A/B pairs in
opposite order. The moving 96-primitive fixture continuously refreshes one
directional cascade set, two spot tiles, and twelve point faces. Automatic device
submission reduces median combined CPU shadow recording from 0.4170/0.4176 ms to
0.1247/0.1214 ms (70.1-70.9%) and reduces combined GPU shadow work from
0.1071/0.1078 ms to 0.0986/0.0973 ms. Matched final-SDR captures are byte-identical;
validation reports exact device/oracle command agreement and zero mismatch,
overflow, or fallback. The isolated `--reference-direct-shadows` route keeps the
main GBuffer automatic. This admits production command construction, not the
default-off shadow-LOD threshold. See
`docs/performance/M7.7-heterogeneous-shadow-admission-2026-09-19.md`.

## M7.7 warmed reflection-probe LOD gate

Completed 2026-09-24 on a native-4K, 1024-pixel realtime capture fixture with
moving high-detail reflective content. The gate repaired multi-frame ticket
progress, retains exact device/oracle command agreement, and produces a byte-
identical LOD0/16-pixel final-SDR capture. The 16-pixel candidate removes only
0.9263% of retained capture triangles and shows no repeatable CPU, GPU, wall-time,
or memory win across reversed-order Release pairs. LOD0 therefore remains the
global probe default; the radial selector remains an experimental workload option.
See `docs/performance/M7.7-probe-lod-warmed-admission-2026-09-24.md`.

## M7.7 warmed conventional shadow-LOD gate

Completed 2026-09-29 without production admission. Eight shadow texels remove
1.2684% of combined directional/spot/point triangles and save only 0.007-0.011 ms
of combined shadow GPU time on the moving native-4K fixture, while CPU recording
regresses and whole-frame ordering reverses. The displayed frame changes seven of
8,294,400 pixels at mean luma SSIM 0.9999999999670409. LOD0 remains the default.
See `docs/performance/M7.7-shadow-lod-warmed-admission-2026-09-29.md`.

## M7.7 shadow consumer-membership cache

Implemented 2026-09-29 and Release-smoke qualified on the moving eight-instance
directional/spot/point fixture. GPU-scene publication now supplies stable shadow
and probe consumer lists, eliminating per-frame application scans. All three
conventional-shadow paths retain geometry/material bins and sorted candidate
layouts across transform-only frames while device visibility/LOD compaction still
runs for every refreshed light region. The 16 measured smoke frames report a cache
hit for every directional, spot, and point submission and zero fallback. This is
correctness/activation evidence, not a warmed timing admission. See
`docs/performance/M7.7-shadow-membership-cache-2026-09-29.md`.

## M7.7 opaque-shadow position-only fetch

Accepted 2026-09-29 for direct and GPU-scene directional, spot, and point shadows.
Opaque pipelines expose only the position attribute; alpha-masked pipelines retain
position/color/UV0/UV1 for opacity correctness. Two reversed-order native-4K pairs
reduce combined conventional-shadow raster median from 0.929824/0.932000 ms to
0.847584/0.843536 ms, a repeatable 0.082-0.088 ms saving, with byte-identical
final-SDR output. The canonical interleaved vertex store remains shared, so the win
adds no persistent geometry buffer. See
`docs/performance/M7.7-opaque-shadow-position-fetch-2026-09-29.md`.

## M7.7 conventional-shadow quality closure

The final moving-camera contact matrix uses nine primitives spanning thin opaque,
fully clipped alpha-mask, and double-sided receiver semantics. Across 120
validation frames, 18-36 directional device commands match the independent oracle
exactly with zero mismatch, overflow, or fallback. Native-4K automatic and direct
captures are byte-identical at both near/far sweep extremes. This closes M7.7
without changing bias or promoting default-off shadow LOD; M7.8 becomes active.
See `docs/performance/M7.7-shadow-quality-closure-2026-09-29.md`.

## M7.8 virtual-shadow residency contract

The first M7.8 checkpoint defines stable sparse-page identity,
separate static/dynamic validity, bounded priority allocation, deterministic
age/priority eviction, pending-raster publication, and safe missing-page fallback
and tests them through a CPU oracle. The second checkpoint adds a default-off Vulkan
allocation path. Its 1,024-page 4,352x4,352 D32 atlas commits 75,759,616 bytes and
its 65,536-entry flat page table commits 262,144 bytes. Two frame-owned aligned
compute working sets commit another 17,042,496 bytes (8,521,248 per frame), bringing
the exact enabled total to 93,064,256 bytes (88.753 MiB); the disabled run reports
zero for all three categories. A short 1280x720 Debug enabled run averaged 7.503475
ms, which is only a lifecycle smoke and not an admission measurement. No shaders
dispatch against or sample the resources yet. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`.

The next backend-neutral checkpoint adds no runtime allocation or frame work. Its
directional receiver-marking oracle bounds unique requests, reports dropped sample
coverage, selects guarded clip levels, and uses signed world-page addresses so
camera scrolling does not invalidate otherwise reusable pages. GPU cost remains
zero until the matching compute path is enabled explicitly.

The raw Vulkan marking shader and separate deterministic request compactor are now
hardware-qualified against the oracle in Debug and Release, but neither is
dispatched by runtime frames and therefore they add zero frame cost. The compactor
is intentionally a single-invocation correctness reference, not a performance
candidate. Its next admission gate must replace it with scalable parallel
deduplication/ordering and measure marking plus compaction separately before page
raster or sampling can be enabled.

The first parallel candidate fails that admission gate by orders of magnitude.
Although exact at 4,096 receivers and the full 65,536-unique-mark bound, its two
global bitonic networks issue 272 compare/exchange dispatches and measure 841.992 ms
Debug and 837.758-844.757 ms across repeated Release medians on the RTX 4090 (five
timestamped runs after one warm-up per measurement). It remains a default-off
hardware test and contributes zero runtime frame
cost. Do not integrate it; replace global sorting with a hierarchical/radix or
bounded top-K pipeline and remeasure the same capacity fixture.

The hierarchical replacement passes the standalone compute gate. Two exact
orderings now use 256-entry shared-memory block sorts plus eight global merge passes
each, and dropped coverage uses two shared reduction dispatches. At 65,536 unique
device-local marks and a 4,096-request output cap, five post-warm-up timestamps
measure 0.145792 ms Debug and 0.145984 ms Release median on the RTX 4090. Release
stage medians are 0.010016 ms projection/clear, 0.055296 ms address ordering,
0.006144 ms reduction/preparation, 0.069632 ms rank ordering, and 0.005120 ms output
and telemetry. This is a compute-kernel admission result, not a whole-frame result;
qualification upload/readback and host allocation are excluded. Persistent frame-
owned storage, immutable descriptors, and compute pipelines now belong to the
default-off runtime owner. The direct shared marking-plus-compaction chain measures
0.148512/0.147264 ms Debug/Release at 65,536 receivers (five post-warm-up runs) with
exact oracle parity and clean synchronization validation. These timestamps exclude
uploads/readback. Live receiver/clip publication and whole-frame measurement remain
open; tracked persistent buffer/image memory is still 88.753 MiB. See
`docs/performance/M7.8-persistent-mark-compact-chain-2026-09-30.md`.

The depth-region producer adds no tracked buffer/image storage. Sampled D32 depth
reconstruction plus the shared chain measures 0.067904/0.067872 ms Debug/Release
median over five warm runs for 4,095 region pixels with exact request parity and
clean synchronization validation. This differs from prior receiver distributions
and is standalone compute evidence. Full 4K coverage/accumulation and render-graph
scheduling remain open; normal runtime frames still do not dispatch the path. See
`docs/performance/M7.8-depth-receiver-producer-2026-09-30.md`.

The full-view continuation now accumulates every depth pixel directly into bounded
per-clip page cells and compacts once, without subsampling or added tracked GPU
buffer/image storage. Exact odd two-clip and native-4K request/overflow parity passes
under synchronization validation; the 4K chain measures 0.569824/0.569504 ms
Debug/Release on the RTX 4090. Stable live clips, render-graph scheduling, integrated
frame admission, page raster and sampling remain open. Conventional shadows stay
active and M7.8 remains in progress. See
`docs/performance/M7.8-full-view-page-marking-2026-09-30.md`.

The camera-driven clip prerequisite now constructs a fixed-capacity directional
stack with doubling world spans, page-snapped signed origins and per-level density
diagnostics. CPU/GPU checks preserve overlap identity through positive/negative
scroll and exact guard fallback. Fixed light-space depth avoids camera-driven depth
cache invalidation; changes to projection policy require a new explicit revision.
The four-level builder costs 0.186163 us Release with no heap allocation. Actual
scene/light revision ownership, conservative depth bounds, per-slot uploads and
render-graph dispatch remain the next live integration gate. See
`docs/performance/M7.8-camera-driven-directional-clips-2026-09-30.md`.

The live CPU publication continuation now derives the first selected directional
light's clip packet from the actual view and caster bounds under the experimental
resource switch, including conventional cache-hit frames. A backend-neutral
publisher retains conservative quantized depth envelopes, advances projection
revisions on expansion/light policy changes, and suppresses unknown-bound packets.
Both layers conservatively use the complete caster revision. Per-slot GPU uploads
and external-buffer graph hazard tracking remain open before live compute dispatch.
See `docs/performance/M7.8-live-clip-publication-2026-09-30.md`.

The frame-slot upload continuation adds non-owning imported-buffer graph bindings
with retirement-scoped rebinding, distinct-slot handles and persistent write-hazard
tracking. The experimental graph uploads 1,536 checked packed clip bytes from an
immutable slot snapshot into the existing working buffer each frame. Both slots
pass the twelve-frame validation smoke with unchanged 88.753 MiB GPU storage;
standalone GPU oracle parity survives the new command-embedded upload. Live sampled
depth accumulation, integrated per-slot request comparison and native-4K admission
remain next. See `docs/performance/M7.8-frame-slot-clip-uploads-2026-09-30.md`.


M7.8 live scene-depth demand is now recorded after opaque forward rendering, with
fence-retired request checks and an explicit full-depth CPU oracle. Native-4K
moving-camera and complex-forward coverage match exactly; empty packets and three
positive-extent resizes are validation-clean. Matched captures remain byte-identical.
Five paired runs show about 0.42 ms demand cost and increased complete-frame p99;
this is qualification overhead, not VSM admission. Request readbacks add 262,208
bytes; full-depth buffers are separately opt-in. GPU residency, page raster and
sampling remain next; M7.8 stays active and M7.9-M7.12 remain unfinished. See
`docs/performance/M7.8-live-depth-page-demand-2026-09-30.md`.

The 2026-10-01 raster prerequisite now shares checked physical-tile layout between
RHI and Vulkan and defines pending-page crops, rasterized borders/interior UVs,
strict identity/requested-layer revision rejection and conservative caster volumes.
CPU tests and independent GPU point projection cover border/depth/scroll behavior;
the 4K reference capture and allocation totals remain unchanged. GPU residency,
triangle page raster and sampling remain unfinished. See
`docs/performance/M7.8-page-raster-regions-2026-10-01.md`.

Residency ABI v1 now defines lossless 48-byte full-identity keys and 96-byte
resident records, preserving signed 64-bit coordinates/revisions/frame ages and
independent layer validity. The retired-slot collector uses checked request-to-
identity conversion and its native-4K CPU oracle compares full identities/revisions.
This is metadata qualification only: no GPU allocator/storage or new sampling is
enabled. M7.8 stays active. See
`docs/performance/M7.8-residency-identity-2026-10-01.md`.

A serial GPU residency reference now reproduces full-key CPU allocation, cache
hits, independent layer invalidation, protected eviction, overflow and fallback
semantics. Fourteen oracle dispatches and five atomic rejection cases pass under
Vulkan validation in Debug/Release; checked 80-byte requests, 96-byte mappings
and 48-byte telemetry freeze its boundary. The bounded 256-request reference
measures 10.5083/10.8429 ms GPU median and is explicitly rejected for live
production integration. It is private test infrastructure, not page raster,
completion publication or sampling. Engine allocations and conventional image
production remain unchanged. M7.8 stays active: scalable GPU residency, indirect
page raster, shared sampling/local lights and full-path qualification are still
required. See
`docs/performance/M7.8-gpu-residency-reference-2026-10-01.md`.
