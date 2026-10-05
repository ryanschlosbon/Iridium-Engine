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
