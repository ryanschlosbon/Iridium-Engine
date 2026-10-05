# M9 tools

## Temporal image-quality metrics (G6d)

`IridiumTemporalMetrics` (C++, `tools/TemporalMetrics.cpp`) reports the following
from scene-linear PFM and final-SDR TGA captures:
- reference error;
- static-camera stability and flicker;
- ghosting trail energy;
- disocclusion recovery;
- mean accumulation.

See [Temporal-Metrics.md](Temporal-Metrics.md) for the formulas and the CLI.

## Motion evaluation (M9.2d)

`Run-MotionEvaluation.ps1` captures a fixed frame plan of each moving fixture: pan cut and
recovery, disocclusion, teleport, moving glass, moving transparency over a still backdrop
(TF-reactive, M9.3), and one moving pair each for the others. In a git worktree, both scripts
also use references found under the main checkout's `out/m9/motion/ref64`.
For every planned frame it also captures a 64-phase accumulation reference held at that
frame, under `out/m9/motion/ref64` (shared by all candidates and reused). The candidate
set goes under `out/m9/motion/<Label>`. Runs resume: existing references and ranges are
skipped. Each 4K reference writes 64 PFM frames (about 6.4 GB) before averaging them, so
leave room on the disk.

`Evaluate-TemporalMotion.py --candidate L [--baseline noaa]` scores every frame against its
held reference. It reports the tone-mapped RMSE, the share of pixels above 1/64, and the
trail energy over the pixels whose reference changed since the previous frame.

## Feature-admission timing (G6a)

`Run-FeatureAdmission.ps1` runs the feature-admission protocol from
`docs/performance/FRAME_BUDGET.md` ("Evidence tiers"): for each timing route, five
fresh native-4K Release processes per side, interleaved `A,B,B,A,A,B,B,A,A,B`, with
500 warm-up and 10,000 measured frames by default. Engine flags match
`tools/m7r/Run-TimingPair.ps1` (CPU and GPU profiling, hidden borderless window, SDR,
no validation, pipeline cache off). The refactor tier still uses `Run-TimingPair.ps1`.

```powershell
# Same build, a feature flag on side B only (the flag is illustrative)
powershell -ExecutionPolicy Bypass -File tools/m9/Run-FeatureAdmission.ps1 -Label taa-admission `
    -ArgsB '--anti-aliasing taa' -RequireQuiet

# Two builds (baseline worktree vs this checkout), two routes
powershell -ExecutionPolicy Bypass -File tools/m9/Run-FeatureAdmission.ps1 -Label g5-vs-base `
    -RootA out/m7r/worktrees/base -Routes T-F1-all,T-F6-probecap
```

| Parameter | Meaning |
|---|---|
| `-Label` | Output directory `out/m9/timing/<Label>` (`-OutRoot` overrides the parent). An existing label is never overwritten. |
| `-Routes` | Subset of the M7R timing routes (`T-F1-all`, `T-F7-stack`, `T-F5-hetero`, `T-F6-probecap`; from `tools/m7r/M7RFixtures.ps1`). Default: all four. |
| `-RootA`, `-RootB` | Checkout per side; the exe is `<root>/out/build/x64-release/bin/IridiumEngine.exe` and the engine runs with that checkout as its working directory. Default: this checkout. |
| `-ExeA`, `-ExeB` | Explicit executable per side; the working directory is derived from `<checkout>/out/build/<preset>/bin`, or given by `-RootX`. |
| `-ArgsA`, `-ArgsB`, `-ExtraArgs` | Extra engine flags for side A, side B, or both. Space-separated when passed through `-File`. |
| `-Warmup`, `-Frames` | 500 and 10,000 by default. Use tiny values only for smoke tests. |
| `-RepoDataRoot` | Where manifests and cooked artifacts (`out/m7r/ddc`, from `tools/m7r/Cook-FrozenModels.ps1`) come from. Default: the main checkout (parent of the git common directory), so a git worktree uses the main checkout's cooked data and local-only assets. |
| `-PipelineCache` | `off` (default) or a cache directory, as in the M7R scripts. |
| `-RequireQuiet` | Before the first run, GPU utilization and per-process CPU are sampled for `-QuietSampleSeconds` (3). Above `-QuietThresholdPercent` (5%, CPU as a share of all logical processors, engine and this script excluded), the runner warns; with `-RequireQuiet` it aborts before creating the label directory. |
| `-StateSampleSeconds` | CPU sample window for the per-process top-10 record (default 1 s). |
| `-SkipSummary` | Run only; summarize later with `Summarize-Admission.py`. |

Outputs in the label directory:

- `<route>__<n>-<side>.jsonl` and `.log`: per-process profile and engine output.
- `runs.json`: the configuration (sides, args, order, frame counts) and per-run records
  (exe, working directory, arguments, exit code, wall time). Written even if a run fails.
- `machine-state.json`: the quiet preflight, and for every process the timestamp,
  `nvidia-smi` GPU state before launch (clocks, power, temperature, utilization,
  P-state, clock-event reasons, driver) and after exit, the top 10 CPU processes, and
  the active power plan.
- `summary.json`, `summary.md`: from `Summarize-Admission.py`.

## Summaries

```powershell
python tools/m9/Summarize-Admission.py out/m9/timing/<Label>
```

Standard library only. Per-run parsing is imported from
`tools/m7r/Summarize-Profiles.py` (unchanged). The report contains:

- Per run: CPU frame, non-wait CPU and GPU frame median/p95/p99, allocation
  calls/bytes per frame, drain frames, and the GPU state before launch.
- Per side: the median of the five run medians (and of the run p95s and p99s), with
  the min-max spread.
- Per route: B-A deltas (absolute and %) for median, p95 and p99, and how many of the
  five matched pairs (consecutive processes in the interleaved order) favour B.
- Per-pass GPU ranges and CPU scopes per side, with B-A deltas sorted by absolute
  delta. Bisect a regression by pass before attributing it.
- Committed engine memory (live and peak, and by lifetime class), and the counters
  whose retained-frame medians differ between the sides.

It also accepts a `Run-TimingPair.ps1` directory (a list-form `runs.json`).

## Temporal captures (G6c)

`Run-TemporalCaptures.ps1` captures sequences of frames from the M9 temporal fixtures
(`M9Fixtures.ps1`; `-IncludeFrozen` adds the M7R frozen set) at native 4K, hidden,
SDR transport, one process per fixture and capture point. Cook the fixture models
first (`Cook-TemporalModels.ps1`, into `out/m9/ddc` of this checkout or of the main
checkout).

```powershell
# Six jittered scene-linear frames across TF-pan's cut at benchmark frame 180
powershell -ExecutionPolicy Bypass -File tools/m9/Run-TemporalCaptures.ps1 -Label pan-jitter `
    -Only TF-pan -Frames 57:62 -Points scene -ExtraArgs '--temporal-jitter on'

# A 64-sample accumulation reference of TF-static held at benchmark frame 130
powershell -ExecutionPolicy Bypass -File tools/m9/Run-TemporalCaptures.ps1 -Label ref64 `
    -Reference -Only TF-static -HoldFrame 130 -Samples 64
```

| Parameter | Meaning |
|---|---|
| `-Label` | Output directory `out/m9/captures/<Label>/<fixture>/<point>/` (`-OutRoot` overrides the parent). An existing label is never overwritten. |
| `-Only` | Fixture keys (`TF-pan`, ...; frozen keys such as `F1-all` with `-IncludeFrozen`). |
| `-Frames` | `FIRST:LAST[:STEP]` measured frames, inclusive (engine `--capture-frames`). The frame limit is `LAST + 1`. |
| `-Points` | `scene`, `final-sdr` and/or `final-output`. |
| `-Warmup` | Warmup frames; by default each fixture's manifest `warmup_frames`. Measured frame m is benchmark frame warmup + m. |
| `-ExtraArgs` | Extra engine flags, e.g. `--temporal-jitter on`. |
| `-Reference`, `-HoldFrame`, `-Samples`, `-Settle` | Accumulation reference (below). |
| `-MetricsExe` | The G6d metrics tool, `out/build/x64-release/bin/IridiumTemporalMetrics.exe` by default. |
| `-RepoDataRoot`, `-M9DataRoot` | Where the frozen-set data (`out/m7r/ddc`, local-only assets) and the M9 cooked models come from. Default: the main checkout, and this checkout for M9 when it has `out/m9/ddc`. |

Each frame is one `iridium.frame_capture` artifact (`..__mf<N>.pfm|.tga` plus `.json`),
written as its readback completes, so a long 4K sequence never sits in memory. The
sidecar's `render_configuration.temporal_jitter` records that frame's jitter
(`enabled`, `sequence_length`, `sequence_index`, `offset_pixels`, `offset_ndc`,
`turns_since_cut`, `history_reset`), and `run.benchmark_state_frame_index` the
benchmark frame its state came from. `runs.json` lists each process's arguments, exit
code, wall time, peak working set and peak private bytes, and every frame's image hash
and jitter; `machine.json` holds the commit, executable hash and GPU state.

**Reference mode.** `-Reference -HoldFrame F -Samples N` runs
`--benchmark-hold-frame F --temporal-jitter on --temporal-jitter-sequence N` and captures
N consecutive scene-linear frames from the first measured frame at or after `F + Settle`.
The scene is frozen at benchmark frame F, so the N frames differ only by the N Halton
phases. `IridiumTemporalMetrics accumulate` averages them into
`<fixture>/reference/<stem>__hold<F>__ref<N>.pfm`; the per-frame PFMs are then deleted,
and `frames.json` plus the per-frame sidecars are kept. The metrics tool is the G6d
lane's target: when it is not built, the script keeps the frames and says how to
accumulate them later.

Engine flags (qualification builds): `--capture-frames FIRST:LAST[:STEP]` (exclusive with
`--capture-frame`; same `--capture-directory`, `--capture-point` and
`--require-capture-signal` rules; the run fails unless exactly the requested frames were
captured, in order) and `--benchmark-hold-frame F` (benchmark camera, motion,
history-reset cuts and visibility steps evaluated at `min(frame, F)`; frames are
application frames, warmup included).
