# Temporal image-quality metrics (G6d)

`IridiumTemporalMetrics` measures image quality over engine captures: error against a
reference, static-camera stability, ghosting and disocclusion recovery. It also
averages a set of PFMs into a mean reference.

- **Source:** `tools/TemporalMetrics.cpp` (CLI). The metric math lives in
  `tools/temporal/TemporalMetrics.{h,cpp}`, the static library
  `iridium_temporal_metrics`.
- **Dependencies:** standard C++, plus nlohmann/json (already a dependency) for the
  report. There is no new third-party library, and the tool is never linked into the
  engine.
- **Tests:** `tests/tools/TemporalMetricsTests.cpp` (ctest `TemporalMetricsTests`).
- **Build:** the target is part of the normal build. The binary is
  `out/build/<preset>/bin/IridiumTemporalMetrics.exe`. Use the Release build for 4K
  sequences.

## Inputs

| Domain | File | Contents | Row order |
|---|---|---|---|
| `scene` | `.pfm` | Scene-linear ACEScg (AP1) RGB float32, captured before exposure (`capture.exposure = "unapplied"`) | Bottom row first, per the PFM spec. This matches `writeFrameCapturePfm` in `src/capture/PfmImage.cpp` (`PF\n<w> <h>\n-1.0\n`, little-endian). |
| `sdr` | `.tga` | Final SDR, 8-bit display-encoded BGRA | Engine captures are top-left (descriptor `0x28`). 24-bit and bottom-left TGAs are accepted too. |

- **Reader leniency:** the PFM reader also accepts `Pf` (grayscale, replicated to RGB)
  and big-endian files (positive scale). Rasters are read in one bulk read and
  converted on all cores.
- **Domain:** inferred from the first input's extension. `--domain` forces it. Every
  input must match the domain.
- **Metadata:** the JSON sidecars (`iridium.frame_capture` v1) are not read. Pair
  inputs deliberately; `IridiumImageCompare` is the metadata-checked comparator.

### Path lists

- **Wildcards:** `--frames` and `--references` take one or more paths, up to the next
  `--option`. A `*` or `?` in the file name is expanded by the tool, since PowerShell
  does not glob for native executables. Matches are sorted in natural order
  (`f2 < f10`).
- **List files:** `--list FILE` and `--reference-list FILE` read one path per line.
  Blank lines and lines starting with `#` are ignored. Relative paths resolve against
  the list file's directory.
- **Pairing:** for `ghosting` and `recovery`, frames and references pair by position.
  The counts must match.

### Masks

- **Applying a mask:** `--mask M.tga|M.pfm` restricts every statistic to the included
  pixels.
- **TGA masks:** a pixel is included where `max(R, G, B) > 127`.
- **PFM masks:** a pixel is included where `R > 0.5`.

## Signals

| Name | Definition |
|---|---|
| AP1 luminance `Y` | `0.2722287 R + 0.6740818 G + 0.0536895 B` (ACEScg to XYZ, Y row) |
| Exposure | Scene domain only: RGB is multiplied by `2^EV` (`--exposure-ev`, default 0) before any metric |
| Tone-mapped luma, scene | `t = Y' / (1 + Y')` with `Y' = max(Y, 0)`. NaN becomes 0 and is counted as non-finite. `+Inf` becomes 1. |
| Tone-mapped luma, SDR | `t = 0.2126 R' + 0.7152 G' + 0.0722 B'` on display-encoded codes / 255 |
| Per-channel tone map | `c / (1 + c)` with `c = max(c, 0)` (simple Reinhard) |

Temporal metrics (stability, ghosting, recovery) operate on tone-mapped luma, which
lies in [0, 1). The `1/255` flicker threshold and the `0.02` trail threshold are on
that scale.

Reductions are summed in double per fixed 16-row chunk and combined in chunk order.
Results therefore do not depend on `--threads`.

## Commands

### `reference-error`

```powershell
IridiumTemporalMetrics reference-error --reference R --test T [--domain scene|sdr] [--mask M]
    [--exposure-ev EV] [--log2-epsilon 1e-4] [--tm-threshold 0.015625] [--changed-threshold 0]
```

**Scene domain** (`R` and `T` are PFM). `N` is the number of included pixels that are
finite in both images. Pixels with any non-finite channel are excluded and counted
in `non_finite_pixel_count`.

| Field | Formula |
|---|---|
| `luminance_rmse` | `sqrt(mean((Y_T - Y_R)^2))` |
| `log2_luminance_mae` | `mean(abs(log2(max(Y_T, eps)) - log2(max(Y_R, eps))))`, with `eps = --log2-epsilon` (scene-linear units after exposure) |
| `log2_luminance_rmse` | RMS of the same log2 difference |
| `tone_mapped_rmse` | `sqrt(mean over pixels and the 3 channels of (tm(T) - tm(R))^2)`, a perceptual-ish HDR error |
| `max_abs_linear` | Maximum absolute linear channel difference |
| `max_abs_tone_mapped` | Maximum absolute tone-mapped channel difference |
| `above_threshold_fraction` | Fraction of pixels whose maximum channel tone-mapped error exceeds `--tm-threshold` (default 1/64) |

**SDR domain** (`R` and `T` are TGA):

| Field | Formula |
|---|---|
| `mse_code`, `psnr_db` | MSE over RGB codes, and `10 log10(255^2 / MSE)`. Identical images report `"inf"`. |
| `max_abs_code` | Maximum absolute RGB code difference |
| `changed_pixel_fraction` | Pixels where any RGB code differs by more than `--changed-threshold` |
| `luma_ssim_mean` | Mean SSIM (Wang et al. 2004) on encoded luma, with dynamic range L = 1, K1 = 0.01, K2 = 0.03 and an 11x11 Gaussian window (sigma 1.5). Evaluated over the valid region with no padding. With a mask, only window centres on included pixels count. Moments are accumulated in double. |

`IridiumImageCompare` reports a different SSIM: 8x8 non-overlapping windows on
decoded linear Rec.709 luma. The two values are not interchangeable.

### `stability`

```powershell
IridiumTemporalMetrics stability --frames F1 F2 ... | --list FILE [--domain D] [--mask M]
    [--flicker-threshold 0.0039216] [--heatmap OUT.pfm|OUT.tga] [--heatmap-scale S]
```

The input is a static-camera sequence of K >= 2 frames, and `t_k(p)` is the
tone-mapped luma of pixel `p` in frame `k`.

| Field | Formula |
|---|---|
| `mean_frame_delta` | Mean of `abs(t_k(p) - t_{k-1}(p))`, pooled over every included pixel and every consecutive pair |
| `p99_frame_delta` | Nearest-rank 99th percentile of the same pooled deltas. A 65,536-bin histogram is used: exact zeros have their own bin, and the resolution is 1/65535. |
| `max_frame_delta` | Exact maximum delta |
| `pair_mean_delta[]` | Mean delta per consecutive pair (K - 1 entries) |
| `mean_temporal_std`, `p99_temporal_std`, `max_temporal_std` | Statistics over pixels of `sigma(p)`, the population standard deviation of `t_k(p)` over the K frames (divide by K). It is accumulated in double, shifted by the first frame. |
| `flicker_pixel_fraction` | Fraction of pixels with `sigma(p) > --flicker-threshold` (default 1/255) |
| `flicker_energy` | Mean `sigma(p)` over those flickering pixels. Removing the per-pixel temporal mean is implicit in `sigma`. |

`--heatmap` writes the `sigma(p)` map as grayscale. The scale defaults to
`1 / max_temporal_std`, so the brightest pixel reaches 1. The scale is recorded in
the report.
- **TGA:** values are clamped to [0, 1] and quantized to 8 bits.
- **PFM:** values are written as float.

Expected values:
- Identical frames give 0 for every field.
- An alternation between `a` and `b` with an even K gives a mean delta of `|a - b|`
  and `sigma = |a - b| / 2`.

### `ghosting`

```powershell
IridiumTemporalMetrics ghosting --frames F... --references R... [--mask-dir DIR | --trail-mask auto]
    [--trail-k 4] [--trail-threshold 0.02] [--mask M]
```

Each frame `F_t` is compared with its own reference `R_t`, for example a
per-frame accumulation reference.

- **Per frame:** `mean_abs`, `rmse` and `max_abs` of `abs(t(F_t) - t(R_t))`, over
  `--mask` when one is given.
- **Summary:** `mean_abs_mean` and `mean_abs_max` over all frames.

Trail masks:
- **`--trail-mask auto`:** the trail of frame `t` covers the pixels where
  `abs(t(R_t) - t(R_{t-k})) > --trail-threshold`, where content moved within the last
  k frames. Frames `t < k` have no trail (`null`).
- **`--mask-dir DIR`:** reads one TGA or PFM mask per frame. The files are sorted in
  natural order, and their count must equal the frame count.

For each trail mask, the report adds per-frame fields:
- `trail_area`: the number of trail pixels.
- `trail_energy`: the sum of tone-mapped luma error over the trail divided by
  `trail_area`, which approximates ghosting energy.

The summary adds `trail_energy_mean` and `trail_energy_max` over frames with a
nonempty trail, and `trail_energy_pooled`: the total trail error divided by the total
trail area.

### `recovery`

```powershell
IridiumTemporalMetrics recovery --frames F... --references R... --region X0 Y0 X1 Y1
    [--threshold T] [--steady-multiplier 2]
```

The region uses pixel coordinates with the origin at top left. `X1` and `Y1` are
exclusive, and the region is clipped to the image. Each frame reports
`region_mean_abs` and `region_rmse` of the tone-mapped luma error inside the region.

| Field | Meaning |
|---|---|
| `threshold` | `--threshold`, or by default `--steady-multiplier` times `steady_state_median`, the median region error of the last quarter of the frames (at least one) |
| `recovery_frames` | Index of the first frame whose error is <= `threshold`. Frame 0 is the first frame, so this is the frame count from the first frame until the error falls below the threshold. `null` if the error never does. |
| `settled_frame` | First index from which every later frame stays <= `threshold`. `null` if the last frame is above it. |

Start the sequence at the disocclusion event (the first frame after the cut or
reveal). The default threshold assumes that the sequence ends converged.

### `error-flicker`

```powershell
IridiumTemporalMetrics error-flicker --frames F... --references R... [--flicker-threshold T] [--mask M]
```

For consecutive frames, this measures the change of the tone-mapped luma error `e_t = t(F_t) - t(R_t)`,
where each frame has its own reference (for example a held 64-phase accumulation reference, as in
`Run-MotionEvaluation.ps1`). It reports, per pair and pooled:
- `mean_error_delta`: the mean of `abs(e_t - e_{t-1})`;
- `p99_error_delta`: the 99th percentile of the same;
- `shimmer_pixel_fraction`: the share of pixels where it exceeds `--flicker-threshold` (default 1/255).

**Limitation:** errors are compared at the same pixel, not along motion. Under camera or object motion,
any error that travels with the content (blur, lag) also counts. More history weight therefore scores
*worse* here, even when it shimmers less. Use it on held or near-still sequences, or alongside the
reference error, and do not tune against it alone. A motion-compensated variant is future work.

### `accumulate`

```powershell
IridiumTemporalMetrics accumulate --frames F1.pfm F2.pfm ... --out MEAN.pfm [--report FILE]
```

Averages N scene-linear PFMs per channel. The sums are kept in double, and the mean
is written as float in the engine's PFM layout. Use it as an offline supersampled
reference when the engine-side accumulation reference (G6c) is not available, for
example over N jittered TAA-off captures of a held frame. Non-finite input samples
propagate and are counted in `non_finite_sample_count`.

## Output

- **Report:** JSON with `schema: "iridium.temporal_metrics.v1"`, `command`, the
  inputs, `settings`, `metrics` (or `summary` and `frames`), `timing_ms` and
  `threads`.
- **Destination:** the report goes to stdout, or to `--out FILE` (`--report FILE` for
  `accumulate`). A one-line human summary goes to stderr.
- **Existing outputs** are refused unless `--force` is given.
- **Exit codes:** 0 on success, 2 on an error. There are no pass/fail thresholds;
  gating belongs to the caller.

## Examples

```powershell
$tm = 'out/build/x64-release/bin/IridiumTemporalMetrics.exe'
$cap = 'out/m7r/captures'

# Jitter-on vs jitter-off, scene-linear
& $tm reference-error --reference "$cap/r0/F1-all/scene/*.pfm" `
    --test "$cap/m9-g5b-jitter-on/F1-all/scene/*.pfm" --out out/m9/metrics/f1-jitter.json

# Final SDR with a mask
& $tm reference-error --reference ref.tga --test taa.tga --mask edges.tga

# Static-camera flicker over a capture sequence, with a heatmap
& $tm stability --frames "out/m9/seq/static/scene/*.pfm" --heatmap out/m9/metrics/flicker.tga

# Ghosting against per-frame accumulation references with an automatic trail mask
& $tm ghosting --frames "seq/taa/*.pfm" --references "seq/ref/*.pfm" --trail-mask auto

# Disocclusion recovery inside a region
& $tm recovery --frames "seq/taa/*.pfm" --references "seq/ref/*.pfm" --region 1600 900 2200 1300
```

## Performance

Measured on the reference machine (i9-14900K, 32 threads, Release build), on
3840x2160 captures:

| Command and input | Load | Compute |
|---|---|---|
| Scene `reference-error`, PFM pair (2 x 99.5 MB), OS file cache warm | about 105 ms | 11-21 ms |
| Scene `reference-error`, PFM pair, cold first read | about 1.1 s | |
| SDR `reference-error`, TGA pair (2 x 33 MB) | 40-180 ms | about 100 ms (mostly the Gaussian SSIM) |

For `stability`, `ghosting` and `recovery`, the next frame is prefetched on a separate
thread while the current one is processed. Throughput is about 100 ms per 4K scene
frame, or about 10 s per 100 frames, with warm files. Memory is bounded by a few
full-resolution planes, plus `--trail-k` reference planes for `ghosting`. It does not
grow with the frame count.
