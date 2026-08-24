# Iridium Engine Performance Contract

## Reference target

- Hardware: RTX 4090, Core i9-14900K, 64 GB DDR5-6000, fast NVMe SSD.
- Display workload: 3840x2160, HDR where supported.
- Goal: more than 100 FPS in fully dressed, active gameplay scenes.
- Base-render frame budget at 100 FPS: 10.0 ms.
- Frame generation, if used, is reported separately and does not satisfy the 10.0 ms simulation/base-render target.

This is an engineering contract, not a promise that every pathological authoring case runs at 100 FPS. Quality tiers and explicit hero-material budgets should keep normal production content predictable.

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

Independent child products may run in a bounded job graph, with texture compression
and geometry/LOD/meshlet work parallelized only where profiles show useful CPU work.
Progressive GPU residency keeps the last complete revision or a semantic
texture/coarser-LOD/proxy fallback visible while child products upload within a
per-frame byte/time budget. The current one-shot atomic model path remains a
compatibility fallback, not the intended steady solution for very large assets.

## Initial 10 ms GPU budget

This table is a starting hypothesis for M0/M1, not a permanent allocation. Overlap means the row totals are not a scheduling model.

| Area | Initial budget | Notes |
|---|---:|---|
| Visibility, depth, and surface data | 1.8 ms | Includes geometry plus conventional GBuffer or visibility/material resolve; should improve with M7/M8. |
| Shadows | 1.5 ms | Requires caching and content-aware update policy. |
| Direct lighting and IBL | 1.4 ms | Deferred plus complex-forward contribution. |
| Transparency and refraction | 1.0 ms | Ordinary scene target; separately budget marked hero glass. |
| Non-RT GI, probes, and reflections | 1.5 ms | Technique mix will evolve in M5/M10. |
| Temporal reconstruction and AA | 0.9 ms | Native reference and external upscalers measured independently. |
| Post-processing, bloom, exposure, output | 0.8 ms | Includes HDR output transform, excludes UI if separately timed. |
| UI, particles, and miscellaneous | 0.5 ms | Content dependent. |
| Scheduling margin | 0.6 ms | Protects against spikes and features not represented above. |

CPU work should remain comfortably below the GPU target in representative scenes, with a provisional target of less than 4 ms for simulation plus render preparation on the reference CPU and low submission overhead. M0 establishes useful percentiles and thread-level budgets.

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

ADR-0010 adds quality tiers; it does not increase the 10.0 ms base-render budget.
On the RTX 4090 reference, the ordinary dressed-scene target remains 1.5 ms for all
shadow rendering/filtering and 1.5 ms for the complete non-RT GI/AO/probe/reflection
mix. Cinematic/hero overrides may exceed an individual row only when spatially
bounded and when the total frame, tail latency, and memory remain reported. RTX 5090
results are useful additional evidence but never replace the fixed 4090 comparison.

Future shadow reports must separate conventional-map raster, virtual page marking/
culling/raster/filtering, screen-space contact, temporal denoising, translucent RGB
visibility, and RT visibility. Required counters include shadowed lights per pixel,
requested/resident/rendered/cached/invalidated pages, blocker/filter samples, rays,
history rejection, owner omissions, update latency, and physical/transient VRAM.

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
