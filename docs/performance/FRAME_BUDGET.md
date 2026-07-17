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

## Initial 10 ms GPU budget

This table is a starting hypothesis for M0/M1, not a permanent allocation. Overlap means the row totals are not a scheduling model.

| Area | Initial budget | Notes |
|---|---:|---|
| Visibility, depth, and GBuffer | 1.8 ms | Includes instance/geometry work; should improve with M7/M8. |
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
