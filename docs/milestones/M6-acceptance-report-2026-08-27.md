# M6 Hybrid Transparency Acceptance Report — 2026-08-27

## Decision

M6 Hybrid Transparency is accepted and complete. Classified hybrid transparency is
the production renderer. The former `LegacyTwoBucket` architecture is no longer
artist-selectable or serialized as a production choice; it remains available only
through the explicit `--developer-legacy-transparency` comparison option.

The production graph omits `depth.glass` and the four background/foreground legacy
passes. Unsupported classified specialist work uses one direct
`transparent.compatibility.forward` fallback without reviving the legacy glass-depth
approximation. No accepted Ordinary2, Hero4/Cinematic8, or WeightedOIT packet enters
that fallback.

## Changed behavior and architecture

- Fresh schema-2 glTF imports and uninitialized cooked/runtime records default to
  classified execution. Frozen schema-1 migration and historical metadata remain
  readable so stable identities and CookKeys are not silently rewritten.
- Complete cooked-model publication resolves one runtime execution mode across
  geometry routing, material flags and variants, shaders, graph passes, blend, and
  depth state.
- Artists author transparency class, coverage, priority, metric thin thickness, and
  explicit 2/4/8 interface quality. Opaque primitives collapse and disable these
  controls until a stable-GUID override is checked.
- Production supports deterministic SortedSurface and ThinGlass, packed Ordinary2,
  explicit Hero4/Cinematic8 deep peeling with residual overflow, and explicit
  non-refractive WeightedOIT.
- Import settings use backend-neutral asynchronous apply/undo/redo transactions.
  Import/cook stage timing, persistent derived texture reuse, bounded parallel texture
  cooking, and atomic large-model upload admission address the failures found during
  the Alfa and Sponza investigations.
- The FPS title formatter now uses fixed stack storage. A 10,000-frame qualification
  exposed two 40-byte allocations at each one-second title update that the earlier
  32-frame allocation sample could not see; the final long profiles record zero.

The principal interfaces affected are the model import/product/runtime contracts,
material policy/runtime records, draw-packet routing, RHI backend configuration and
runtime diagnostics, Vulkan production graph/frame targets/passes, editor asset and
material diagnostics, profiler presentation, and deterministic M6 fixture manifests.

## Verification

The final source passes the complete 71-test Debug suite and the complete 71-test
Release suite. Vulkan-validation-enabled classified, forced-legacy, invalid-topology,
resize, lifecycle, output-transport, selection, and capture processes complete with
zero validation messages. The inherited M6.4 evidence supplies eleven real OS
resize/maximize/restore/minimize/zero-extent cycles; M6.9 does not alter the accepted
swapchain zero-extent wait path.

Current classified validation evidence includes:

- Ordinary2: 5,888 exact paired entry/exit/local-color pixels in the one-shell control;
  the populated resize run restores 47,070 pairs after 960x540 -> 1600x900 ->
  1280x720 and keeps the selected-entity overlay active.
- Cinematic8: two 120-frame retire/reactivate cycles complete at measured frame 246;
  final readback reaches all eight interfaces with 15,042 paired/local-color pixels
  and zero compatibility draws.
- WeightedOIT: the 65,536-instance topology survives the same three resize extents;
  the representative 256-particle performance fixture records one accumulation draw,
  one resolve, and zero fallback.
- SDR, scRGB, and HDR10 select their requested transports without fallback. The scRGB
  and HDR10 readbacks contain 2,764,800 finite values each and zero nonfinite values.
  Their final capture hashes remain byte-identical to the accepted M6.7 output gate.
- The current Alfa final-SDR capture is byte-identical to the accepted pre-cutover
  non-emissive headlamp result (`4165a6b...31111d`). Its ribbed lenses, bright chrome
  reflector bowls, body clearcoat, glass, and authored texture detail remain visible.

## Native-4K performance and memory

Measurements use the reference RTX 4090/i9-14900K system, Release, hidden uncapped
3840x2160 rendering, 500 warmups, GPU timestamps, transparent pipeline statistics,
and no Vulkan validation in percentile runs.

| Workload | Processes / frames | GPU median | Worst p95 | Complete transparency median |
|---|---:|---:|---:|---:|
| Ordinary2 populated grid | 5 / 50,000 | 0.720192 ms median-of-medians | 0.725824 ms | 0.403456 ms median-of-medians |
| Cinematic8 four-shell control | 1 / 10,000 | 1.612576 ms | 1.622624 ms | 1.276928 ms |
| WeightedOIT 256-particle field | 1 / 10,000 | 2.657440 ms | 2.879584 ms | 2.362368 ms |
| Alfa three-light Ultra/Belfast scene | 1 / 10,000 | 5.820864 ms | 6.343168 ms | 0.466368 ms |

The Ordinary2 five-process wall averages cluster at 0.890441-0.893460 ms. All five
processes have identical graph/live memory, exact eight-packet routing, 3,387,680
transparent fragment invocations per frame, zero compatibility or legacy work, zero
topology churn, zero dropped profiler data, and zero C++ allocations in every retained
frame. The production cutover removes 63.281 MiB requested and 63.750 MiB committed
native-4K graph memory from the prior M6.5 Ordinary2 topology.

Against the frozen M6.0 Alfa fixture, total GPU median improves 4.45%, worst p95
improves 19.73%, and worst p99 improves 23.01%. Transparency rises 3.98% from
0.448512 to 0.466368 ms but remains below the 1 ms ordinary-transparency budget. The
completed M6 transport graph is 105.467 MiB requested and 198.286 MiB committed above
the M6.0 graph because M6 added the accepted conditional refraction/layer products;
the M6.9 cutover already removes the obsolete two-context 4K glass-depth cost.
The representative classified domains are zero-allocation. The heavier Alfa scene
improves from M6.0's 39 calls/5,288 bytes to 16 calls/1,912 bytes per retained frame;
that residual general renderer/light/editor work is outside the M6 transport path and
is deferred to the broader GPU-scene/submission work.

## M6.10 editor workflow closeout

The accepted rendering result is unchanged. A final editor-only slice restores
left-click mesh selection and adds an adaptive metric ground grid, shared world SI
display unit, independent translation/rotation/scale snapping, Ctrl snap bypass, and
world/local gizmo space. Inspector transform and light-distance displays follow that
unit; light controls are nonnegative but otherwise uncapped, with lux/kilolux and
candela/lumen views. Component and property names are searchable.
The grid rests flat on Iridium's Y-up world XZ plane at `Y=0`; an artist setting optionally aligns it through the
selected pivot with the active world/local translation axis or plane during a drag.
It is an unbounded procedural plane rather than a finite CPU line set: per-pixel
derivatives select stable decade spacing, lines reach the horizon, and their opacity
falls toward grazing/horizon views. The output transform samples the production opaque
depth attachment and suppresses grid fragments behind nearer scene geometry. The
search header is visually separated from component bodies.

Asset Browser folders now support physical drag nesting in both thumbnail and tree
views, with a persistent custom order for the tree. Model expansion is an attached,
horizontally scrollable strip of selectable material/primitive thumbnail cards. The
Up affordance is widened. A diagnostic benchmark metadata snapshot was renamed away
from the live `.iridium.meta` suffix and its manifest updated, so catalog inspection
reports zero duplicate GUIDs; the fixture contract guards the filename distinction.

These changes add no render-graph resources or passes. The procedural grid adds one
read of the existing opaque depth attachment to the output transform and is disabled
for benchmarks, asset previews, and renderer captures. Debug and Release each pass the
complete 71-test suite. A validation-enabled hidden 20-frame Ordinary2 run exits
cleanly with the grid active, classified execution, and no Vulkan messages. A
500-frame 1280x720 Release profile measures the output transform at 0.013312 ms median
with the grid active versus 0.006144 ms disabled (roughly 0.007 ms incremental), a
0.129664 ms total GPU-frame median, and zero dropped frames.

## Remaining risks and deferrals

- Raster refraction is bounded screen-space transport. It does not provide ray-traced
  off-screen geometry, colored transparent shadows, caustics, or path-traced multiple
  scattering.
- Hero4/Cinematic8 are explicit quality costs; Ordinary2 remains the automatic closed-
  volume tier. WeightedOIT remains explicit and non-refractive.
- The developer legacy option is retained only for regression diagnosis and should be
  removed after a suitable post-M6 support window.
- Progressive GPU residency and finer-grained geometry recooking remain planned asset
  pipeline work. Material editing and animation graphs remain later roadmap items.
- General opaque visibility, LOD, static/movable update policy, indirect submission,
  and remaining scene-level CPU allocations belong to M7/M8 rather than M6.

ADR-0012 remains accepted and now records the completed cutover; no superseding ADR is
required. `ROADMAP.md` marks M6 complete. The evidence is frozen under
`out/benchmarks/m6.9-final/` and hashed in
`assets/benchmarks/m6/run-manifest.v1.json`.
