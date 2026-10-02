# M7 Lit Requalification — 2026-08-30

## Outcome and limits

The benchmark's declared constant illumination is now applied, and populated
scene-linear captures match an independent unculled direct GBuffer reference.
The earlier black-image equality is withdrawn. This is current-implementation
requalification, **not a reconstruction of the pre-M7.1 executable**.

Five-process native-4K testing does **not** show a complete-path indirect win in
the three-car fixtures. Relative to current unculled direct submission, automatic
indirect is 2.84% slower in median run-average wall time for all-visible and 1.00%
slower for one-visible. The indirect candidate needs workload admission and
further optimization before blanket production cutover. Correct rejection alone
does not establish a performance win. M7.5 stays active; M7 is not accepted.

See [M7.5 implementation evidence](M7.5-lod-selection-oracle-2026-08-30.md) for the
lighting repair, capture-signal gate, LOD contracts, importer compatibility,
publication revision regression, and pending obligations.

## Frozen identity

- Worktree HEAD: `5a9f705688829f61a5b12eda2eb36d14a15bbf15`, branch
  `Render-Refactor-for-Modularity`; deliberately dirty inherited M6/M7 work retained.
- Release executable: `3d9f24d8825e039aaaefed20e0d43d51d935f9be149285aecf632279547e6eff`.
- Debug executable: `a931880395b140c1725c6ce9ef473c539c6d7202afebe38cbcebfa1724d8aa0d`.
- Compaction shader: `2ee788781e71fdf381e17d3e262e41e865d7dd7eda5d4b4b6893a70031ebf240`.
- Manifest `assets/m7-three-dense-assets-manifest.v1.json`:
  `083b4470063989830199926134db3b9ce2debe64d47062647b5eaf3c48c944b7`.
- Frozen schema-6 model at `out/benchmarks/m7.3/schema6-ddc/d2/4ad2669934d4bbe79d4cb88de5f17d93a92bd21b70bd9803762d96dec8b597.irartifact`:
  `72ef91c3c213c78aa7286d799100e2433afd2d15273fc85cf3b2073ba9d87ca4`.
- RTX 4090 UUID `3e45ec9c96fc398d79613146f37d826d`, NVIDIA 610.74;
  i9-14900K, 68,466,892,800 bytes physical memory, Windows 10.0.26220.
- MSVC 19.51.36256.0, shaderc v2023.8 v2025.5, Vulkan SDK 1.4.335.

The frozen manifest and asset sidecar were not regenerated. The repaired runtime
uses the declared environment RGB; no external environment artifact is supplied.

## Visual and structural checks

Validation-enabled captures use 12 warmup / six measured frames, capture frame 4,
and `--require-capture-signal`. Automatic and `--reference-direct-gbuffer` captures
are byte-identical in each case:

| Fixture / capture | Matching SHA-256 |
|---|---|
| All-visible AP1, 3840x2160 | `a500fbd6991e2f3131783fccbd8ec096e81e30892bed95eb273761d79dd43401` |
| One-visible AP1, 3840x2160 | `088ac21fe82c12aac368cd4aca5f5307322cd84eba3462bda0c1f49e7a7526b2` |
| 256-instance AP1, 3840x2160 | `d992a2d78bdfc5c59f157a740fd5ae984b0b2ec55a1db6e7e6e36d7623d8f369` |
| All-visible final SDR, 1280x720, +4 EV companion | `abaff96353b105e4bd350b67523b0a833fe53fd50a4a3cccf48dcb33988f88e2` |
| One-visible final SDR, 1280x720, +4 EV companion | `e4b405cfa414a3fe3659fc787956c3842be93b3e49e8190b606c78a79a2ab2ed` |

All ten captures contain finite nonconstant RGB, and their metadata correctly
identifies the procedural constant environment. All runs exit successfully with
no Vulkan validation messages. The all-visible +4 EV companion was inspected:
three complete cars and their materials are visible. The exposure override is
explicitly for inspection, not a change to the frozen timing/scene-linear input.

Automatic device/oracle GBuffer command counts are 303/303, 114/114, and
25,856/25,856 respectively, with 101 bins and no mismatch/overflow. Direct reference
emits no indirect commands; absent device counters are **not** interpreted as
measured zeros. Forward/transparency consumers keep their existing route.

Artifacts: `out/benchmarks/m7.5/lit-qualified-r2`; capture logs:
`out/benchmarks/m7.5/lit-*-validation-r2.log`.

## Twenty-process native-4K timing

Each camera/route pair has five fresh processes. Routes alternate order on each
repeat. Every run uses 3840x2160, 500 warmup / 10,000 measured frames, hidden
borderless window, validation off, CPU/GPU profiling on, native reconstruction,
default ACES2 SDR at 0 EV, mailbox present, three swapchain images, and
`fresh-process-os-driver-cache-uncontrolled`. No capture occurs in timing runs.

Table medians are the median of five per-run medians (wall: median of run means).
Tails are the worst per-run p95/p99, not percentiles of only five values.

| Camera / route | Wall ms | CPU median / worst p95 / worst p99 ms | GPU median / worst p95 / worst p99 ms |
|---|---:|---|---|
| All / automatic indirect | 1.426540 | 1.3981 / 1.4750 / 1.6807 | 1.101536 / 1.111680 / 1.350880 |
| All / unculled direct | 1.387138 | 1.3593 / 1.4311 / 1.6433 | 1.091584 / 1.099392 / 1.340576 |
| One / automatic indirect | 2.297612 | 2.2656 / 2.4651 / 2.5924 | 1.964128 / 1.979072 / 2.217632 |
| One / unculled direct | 2.274937 | 2.2358 / 2.4238 / 2.5981 | 1.962368 / 1.983424 / 2.224128 |

The narrow one-visible camera covers more pixels with the car than the all-visible
camera. Its total cost is not an isolated before/after culling comparison; compare
routes **within** each camera.

| Camera / route | GBuffer CPU median us | GBuffer GPU median us | Compaction GPU median us |
|---|---:|---:|---:|
| All / indirect | 30.5 | 79.872 | 7.520 |
| All / direct | 28.7 | 78.848 | not dispatched |
| One / indirect | 33.7 | 175.104 | 7.456 |
| One / direct | 30.0 | 177.152 | not dispatched |

Every run completes 10,000 frames with zero drops. Each run's 512 retained counter
samples has zero C++ allocation calls/bytes, zero GPU-scene upload bytes, and the
unchanged-publication fast path. These counter claims cover the retained window,
not all 10,000 frames. In all automatic runs, every retained sample has exact
device/oracle parity and zero mismatched bins/overflow commands.

Both routes report the same engine allocation totals: requested live/peak
2152.772 / 2225.908 MiB; committed live/peak 2315.611 / 2388.748 MiB.
This is not total system/driver VRAM usage. The direct reference currently still
owns indirect capacity; it does not demonstrate a lower-memory direct backend.

Profiles and logs: `out/benchmarks/m7.5/native-4k-lit/{camera}-{route}-{1..5}`.
`tools/ReportM7RunEvidence.ps1` verifies completed profile structure and derives
all per-run medians/tails, hashes, counter min/max/exactness, and memory fields.
Its 20-run report is `out/benchmarks/m7.5/native-4k-lit-evidence.json`, SHA-256
`59acf666fee965a34e8572f4d118a4cd9202bcae9c051283e706fcc0bddaebd5`.

## Bounded 256-instance pilot

One fresh process per route, native 4K, validation off, 100 warmup / 1,000 measured
frames. This is a diagnostic pilot, **not** the five-process acceptance protocol.

| Route | Wall average ms | CPU median / p95 / p99 ms | GPU median / p95 / p99 ms | GBuffer CPU ms |
|---|---:|---|---|---:|
| Automatic indirect | 18.027287 | 17.9479 / 18.4375 / 18.7940 | 6.917760 / 7.221760 / 7.394336 | 0.5882 |
| Unculled direct | 18.097624 | 18.0215 / 18.4894 / 18.7471 | 6.685440 / 6.957472 / 7.142400 | 0.9724 |

Indirect saves 0.3842 ms GBuffer CPU recording, but adds GPU compaction and yields
only 0.39% lower wall time in this single pair. The complete-path/scaling admission
gate therefore remains open. CPU frame timing includes waits: indirect's median
frame-fence wait is 6.78 ms, transparent sorting 2.46 ms, extraction 1.80 ms, and
opaque sorting 0.92 ms; those are not additive independent frame totals.

Profiles in `out/benchmarks/m7.5/native-4k-lit-stress-pilot`:

- `automatic.jsonl`: `b241b51921e143ff5622fdbb0b7778dcb7b08fe0afa930f2607b9729839cc297`.
- `direct.jsonl`: `6cc53f7f00bd9b3358aa0272de0556901dbe69f33e72f1837fbdb00c4449128e`.

The generated companion report retains timing/counter/memory details. No result
from this pilot is promoted to a production win.

## Launcher and regression checks

The final canonical Release build was started through `launch-engine.bat` with
validation enabled and the caller's working directory set to the repository's
`out` directory. PID 21732 had a nonzero native window handle, `Responding=true`,
and an updating engine window title. Normal window close succeeded and the
launcher returned exit code 0. No unrelated process was stopped. Logs:
`out/benchmarks/m7.5/launch-lit-final.{stdout,stderr}.log`; stderr is empty.
The observed title-bar FPS is launch proof only, not a performance measurement.

Full Debug/Release builds and 76/76 tests pass in each configuration. Strict SDR
image comparisons pass for both camera pairs: maximum code delta 0, zero changed
pixels, luma SSIM 1. Their comparison reports are in `lit-qualified-r2`.

Continue device LOD integration, workload admission, and broader M2/M5/M6
correctness/lifecycle qualification. This checkpoint does not complete M7.5.

No ADR or frame-budget target is changed. The older unlit comparisons remain
historical, and workload-selectable indirect admission remains open.
