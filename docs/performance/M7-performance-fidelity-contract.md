# M7 Performance and Fidelity Contract

## Purpose

This document defines how M7 optimization decisions are measured and accepted. It
supplements `docs/performance/FRAME_BUDGET.md`; that document remains authoritative
for the engine-wide 10 ms native-4K base-render target and reporting rules.

M7 targets the reference RTX 4090, Core i9-14900K, 64 GB DDR5-6000 system and other
high-end PCs. It should use the available hardware aggressively, but additional
work, memory, or complexity is accepted only when it produces measurable fidelity,
performance, latency, or scalability value. A faster result that loses meaningful
silhouette, material, shadow, transparency, temporal, or HDR fidelity is not an
optimization.

## Decision principles

1. Remove work before making individual operations faster.
2. Measure complete production paths, not isolated attachment or dispatch costs.
3. Preserve a high-fidelity path and explicit quality controls when a compact or
   approximate representation is introduced.
4. Keep conventional indexed/deferred and conventional shadow fallbacks until a
   successor has won matched tests.
5. Select representations by workload, quality profile, and capability rather than
   forcing one technique on every scene.
6. Do not use title-bar FPS as bottleneck attribution. Separate CPU work, GPU work,
   acquire/present waits, asynchronous jobs, and upload stalls.
7. Do not accept a steady median improvement that introduces p95/p99 spikes,
   temporal instability, page thrash, streaming hitches, or unbounded memory.
8. Keep experiments behind backend-neutral capability/policy boundaries. Vulkan
   owns their implementation details.

## Frozen entry baseline

M7.0 must create a new baseline from the current source. Do not reuse the owner-
observed approximately 180 FPS three-asset result as qualification. Preserve the
exact asset GUIDs and revisions, scene/camera data, output transport, quality
profile, resolution, driver, executable revision, and shader/DDC state.

Use native 3840x2160 Release builds, validation disabled for timing, at least five
fresh processes, and the established warmup/measured-frame protocol. Run separate
validation-enabled correctness captures. Record displayed, CPU, and GPU frame times
independently.

The lead must freeze these camera/workload cases:

| Fixture | Required purpose |
|---|---|
| Three dense assets, all visible | Main scaling and production visual reference |
| One and two assets off-frustum | Frustum rejection and CPU submission scaling |
| Large occluder | Hi-Z rejection and temporal conservatism |
| Near/mid/far placements | LOD selection, hysteresis, and shadow/probe LOD |
| Explicitly lit oblique near/mid/far placements | LOD silhouette/material error under spot, point, directional, clustered-lighting, and shadow work |
| Entirely static transforms | Zero unchanged-instance upload and cache reuse |
| One moving asset | Sparse transform/history update and shadow invalidation |
| Shadow-only caster | Independent light-view visibility |
| Probe-only/off-camera object | Independent reflection-capture visibility |
| Representative M6 transparency | Prove classified transparency is unchanged |
| Many-instance stress | GPU culling, compaction, and indirect scaling |
| Many-material stress | Pipeline/material bin behavior and resolve coherence |
| Many-local-light stress | Cluster assignment and shadow-owner scaling |
| Directional outdoor shadow | CSM versus virtual directional clip levels, with fixed project coverage and per-level texel density |
| Close/grazing directional shadow | Penumbra reconstruction, acne, contact detachment, and peter-panning |
| Local-light shadow stress | Conventional atlas/cubes versus virtual residency |
| Dynamic invalidation stress | Virtual-page cache behavior under movement |
| Oversized model publication | Fine-grained cook, cancellation, and residency |

## Required metrics

### CPU

- simulation, transform update, render extraction, visibility preparation, sorting,
  command construction/recording, upload preparation, acquire, present, and waits;
- source/requested/changed/visible/rejected instance and primitive counts;
- direct and indirect draw/dispatch counts and material/pipeline bins;
- bytes and ranges uploaded for instances, transforms, geometry, materials, and
  shadow metadata;
- allocation count/bytes, worker occupancy, job latency, and scratch high-water
  marks;
- cooker wall time, aggregate CPU time, cache hits/misses, cancellation latency,
  peak memory, and publication latency.

### GPU

- GPU-scene patching, frustum/LOD, Hi-Z construction, occlusion, compaction, and
  indirect-command generation;
- main depth/visibility/GBuffer/material-resolve/lighting costs;
- requested, visible, LOD-selected, occluded, and rasterized triangle counts;
- cluster count/scan/fill/sort/finalize and complete lighting cost;
- conventional shadow culling, raster, cache update, and sampling, including per-
  cascade near/far ranges, world units per texel, projected texel size, filter radius/
  sample counts, receiver-plane/geometric bias terms, and exact tested/culled
  caster-cascade pairs;
- virtual-shadow page marking, allocation, caster culling, raster, and sampling;
- reflection/probe visibility and capture work;
- transparency counters and timings inherited from M6;
- queue overlap and barrier stalls when asynchronous work is evaluated.

### Memory and residency

- requested and committed live/peak memory by category;
- GPU-scene, geometry, index, LOD, visibility, indirect, Hi-Z, shadow page pool,
  page-table, and visibility-buffer bytes;
- internal arena fragmentation and high-water marks;
- physical/virtual shadow pages requested, resident, rendered, cached, invalidated,
  evicted, and missing;
- upload, staging, readback, and retirement peaks;
- resident/nonresident model child products and fallback state.

### Fidelity and stability

- scene-linear AP1 and final-output comparisons;
- silhouettes, thin geometry, normal/tangent response, UV seams, material response,
  emissive energy, and transparency classification;
- LOD transition captures in motion and under temporal jitter when available;
- occlusion camera cuts, rapid turns, disocclusion, near-plane intersection, and
  small-object behavior;
- shadow contact detail, close-penumbra reconstruction without binary-texel stepping,
  bias/leak/peter-panning on grazing receivers, clip/page transitions, camera motion,
  cache invalidation, thin/alpha-clip casters, and local-light overlap;
- SDR, scRGB, and HDR10 output-boundary preservation where relevant;
- Vulkan validation and resize/zero-extent/live-transport behavior.

Freeze numerical image thresholds per fixture before changing the path. A broad
global threshold must not hide a localized missing object, shadow, reflection,
transparent layer, or high-energy HDR error.

For populated visual fixtures, run captures with `--require-capture-signal` and
inspect RGB signal statistics plus representative object content before accepting
hash equality. Alpha, a clear color, or a spatially varying background cannot
establish that geometry and illumination are correct. Compare against an
independent direct/high-fidelity path where available. Intentional black/constant
diagnostics must be explicitly identified. The 2026-08-30 M7.5 audit withdrew
earlier black-image fidelity evidence; see the M7.5 and lit requalification reports.

## Mandatory production outcomes

M7 is not complete unless all of these are true:

- unchanged static instances produce no steady transform/instance upload;
- off-frustum and conservatively occluded content stops producing main-view
  geometry work;
- CPU preparation/submission scales primarily with changed records and compacted
  visible batches rather than source submesh count;
- LOD uses bounded screen-space error, hysteresis, deterministic selection, and
  hero-quality overrides without visible popping in accepted fixtures;
- main, shadow, and probe visibility are independently correct;
- uncertain culling, residency, and LOD state fails visible or uses an explicit
  valid proxy/coarser product rather than disappearing;
- conventional indexed/deferred and conventional shadows remain usable fallbacks;
- conventional directional coverage is project/profile owned rather than coupled to
  camera far, nearby cascades receive finer reported texel density, and Cinematic is
  reachable when selected;
- M2 materials, M5 lighting, M6 transparency, AP1 HDR composition, selection,
  capture, resize, and live display transport retain their accepted behavior;
- the representative steady paths have zero C++ allocation or a documented,
  measured exception approved by the lead;
- the complete base-render path remains inside the 10 ms native-4K target with
  sufficient margin for later gameplay, M9 reconstruction, M10 GI, and M11 RT.

## Optimization admission gates

### Lossless or structurally equivalent candidates

These should normally ship when verification confirms correctness and a measurable
benefit or material memory saving:

- indexed indirect-count batching;
- stable geometry arenas;
- eligible 16-bit index buffers;
- vertex-cache and vertex-fetch reordering;
- duplicate/degenerate triangle removal with strict topology tests;
- affine 3x4 transform storage where the represented transform remains exact;
- hot/cold record separation;
- sparse dirty uploads;
- position-only or position/opacity-UV shadow streams;
- render-graph lifetime aliasing and narrower synchronization;
- persistent scratch and steady-allocation removal.

### Fidelity-sensitive candidates

These require matched visual error bounds, a high-quality fallback, and explicit
content/quality policy:

- packed normals/tangents, half UVs, vertex colors, or quantized positions;
- generated LODs and shadow-specific LOD;
- visibility-buffer reconstruction and derivative policy;
- variable-rate shading or lower-rate effects;
- approximate occlusion, temporal reuse, and soft-shadow filtering;
- reduced-precision shader arithmetic;
- reconstructed rather than native output resolution.

Position remains float32 in the default high-fidelity vertex candidate unless a
separate quantized product proves asset-relative and image-space error bounds. A
hero/high-precision product must remain selectable when compact attributes lose
meaningful fidelity.

### Workload-sensitive candidates

These are retained only where their complete path wins and may coexist with another
production route:

- conventional packed deferred versus visibility/material resolve;
- conventional CSM/atlas/cubes versus virtual shadow maps;
- depth prepass versus single-pass depth testing;
- CPU/direct submission versus GPU indirect submission for tiny workloads;
- asynchronous compute versus graphics-queue execution;
- alternative cluster tile/slice/assignment algorithms;
- DirectXMath, GLM SIMD, or custom SoA CPU math kernels.

## Virtual-shadow decision gate

Here `VSM` means **Virtual Shadow Maps**, not Variance Shadow Maps.

M7's virtual path is a serious production candidate, but not a mandatory global
replacement. It must implement bounded sparse page tables, a physical page pool,
receiver-driven marking, GPU caster culling, directional clip levels, local-light
residency, cache validity, age/eviction policy, static/dynamic invalidation, safe
missing-page behavior, filtering, and diagnostics.

Promote it for a workload/quality profile only when matched 4K evidence shows a
Pareto improvement over the hardened conventional path:

- lower cost at matched quality; or
- materially higher, more stable detail at comparable cost and within the 1.5 ms
  shadow budget; and
- bounded memory and p95/p99 invalidation behavior.

Conventional cascades, spot atlases, and point cubes remain capability, quality,
debug, and workload fallbacks. High/Ultra may prefer virtual shadows when they win;
Low/Medium and simple single-sun scenes may prefer conventional maps. M8 meshlets
must feed the same virtual pages without replacing M7 identity or page ownership.
The comparison baseline includes M7.7's reconstructed PCSS filter, coverage/cascade-
density policy, receiver-plane/geometric bias hardening, and independent conservative
directional-cascade caster sets. Conventional directional, spot, and point passes
consume the persistent camera-independent GPU-scene shadow set through compact
references; the remaining direct draw recording is not a second identity system.
VirSM is not credited
for defects that belong to the shared filter or receiver-bias contract. Conventional
and virtual methods remain selectable unless a separately measured hybrid assigns
non-overlapping coverage or a genuinely complementary effect.

Suggested physical pool sizes are hypotheses, not contracts. Report the actual D32
page data, metadata, page tables, static/dynamic layers, and transient peaks rather
than describing virtual resolution as allocated memory.

## DirectXMath and CPU-SIMD gate

Do not migrate the engine wholesale from GLM. M7 may add an internal CPU math-kernel
boundary and benchmark:

- current GLM Release behavior;
- safely configured GLM SIMD/alignment variants;
- DirectXMath SSE2 and AVX2;
- a purpose-built SoA AVX2 implementation where justified.

Required kernels include bulk sphere/frustum tests, AABB transforms, affine
hierarchy composition, view/frustum derivation, and visibility-input preparation.
Record cycles or time per item, cache/branch behavior, code size, numerical error,
and representative full-frame savings.

Adopt a new kernel only when it provides at least a clear repeatable kernel win and
either approximately 0.1 ms representative CPU-frame savings or a demonstrated
scaling benefit that the baseline scene cannot yet expose. Fifteen to twenty percent
kernel improvement is a useful initial threshold, not a substitute for full-frame
measurement. Keep `XMVECTOR`, `XMMATRIX`, compiler-specific SIMD types, and alignment
requirements out of ECS, serialization, RHI, GPU records, and public asset ABI.

An AVX2 high-end execution tier may coexist with a portable baseline. Runtime
capability dispatch or separately controlled builds must be explicit. Global unsafe
fast-math is not an M7 optimization.

## Experiment result template

Every gated candidate records:

```text
Candidate:
Hypothesis:
Compared paths and capability/quality policy:
Fixture revisions and cameras:
CPU median/p95/p99:
GPU median/p95/p99:
Requested/committed live/peak memory:
Image and temporal result:
Validation result:
Scalability counters:
Decision: production / workload-selectable / experimental / rejected
Fallback retained:
Risks and later milestone interaction:
```

Rejected experiments remain documented so later milestones do not repeat them
without new evidence.
