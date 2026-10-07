# M7 completion phase 1: owner performance cases (2026-10-07)

This is the phase-1 report for `docs/milestones/M7-completion.md`: fixtures, attribution, fixes and
before/after numbers for the three owner-observed performance cases.

## Setup

- **Machine:** RTX 4090 (driver 617.42), i9-14900K, native 3840x2160, Release. The machine was quiet
  (owner away, editor and games closed). States were recorded before each session: CPU load 0%,
  GPU idle 0–9% from background apps.
- **Builds:**
  - **A, before phase-1 fixes:** `1758e0d`, which is `main` plus the M7.10.0 code motion and the
    M7.P1 harness.
  - **A, standard routes:** `main` `8a9a601`.
  - **B:** HEAD `5c87384`. The PC3 and PC2 rows below were measured at `13e6675`/`14480ee`;
    later commits change CPU work only.
- **Routes:**
  - Owner cases use the **product route**: TAA, auto-exposure and bloom on, which is what the
    owner sees.
  - Standard routes use the pinned M7R measurement route.
- **Protocol:**
  - `tools/m7r/Run-TimingPair.ps1`, A,B,B,A.
  - Owner cases: 500 warm-up and 2,000 measured frames. Standard routes: 500 + 10,000.
  - "Non-wait CPU" is the frame total minus fence, acquire and present waits.
  - These are refactor-tier pairs, not five-process admissions; every phase-1 fix is
    byte-identical.
- **Fixtures:** `assets/m7c-owner-cases-manifest.v1.json`, regenerated with
  `tools/m7r/Generate-OwnerCaseManifest.py`. Two models from the local asset library: the
  Porsche 911 Carrera 4S and the Porsche 911 930 Turbo (`porsche911930t`).

## Case 3: point light (owner: about 300 → 220 FPS when a point light is added, regardless of intensity)

**Root cause:** clustered-light assignment serialized large lights. `cluster_count` and
`cluster_fill` ran one 64-lane workgroup per light over the light's whole cluster AABB.

- A light whose sphere contains the camera covers every tile of its slices. At 10 m that is
  138,720 of 195,840 clusters, or about 2,170 serial iterations per lane, done twice.
- Cost therefore scaled with range, not intensity.
- The cube shadow was a cache hit: it was *not* re-rendering.
- Intensity changes do invalidate the shadow and all probe captures while you drag the slider.
  That is real edit-time waste, kept as an optional exact fix, but it is not this case.

**Fix, M7.10.2 (byte-identical):** a per-light bounds and reservation stage, then (light, chunk)
grid-strided count and fill.

GPU frame median, product route:

| PC3 variant (930 + 911, camera between) | A | B |
|---|---:|---:|
| no local light | 2.947 | 2.943 |
| point light, 5 m | 5.159 | 3.419 |
| point light, 10 m | 5.387 | **3.427** |
| point light, 40 m | 5.708 | **3.433** |
| point light, 10 m, unshadowed | 5.056 | 3.102 |
| point light, 10 m, Ultra (PCSS) | 6.263 | 4.297 |
| spot light, 10 m | 5.604 | 3.352 |

`gpu.lighting.cluster` fell from 1.8–2.4 ms to 0.07–0.09 ms.

- A point light now costs about +0.48 ms: deferred shadow sampling +0.20, forward +0.09,
  sorted glass +0.15, clusters +0.05.
- Its cost no longer depends on range.
- Ultra PCSS adds about 0.87 ms over High.

## Case 1: many objects

**Root causes (two):**

1. **Transparent work had no camera-frustum test.** There was only a near/far depth test, so
   off-screen cars were fully requested, sorted and recorded.
2. **The previous-transform cache was the largest main-thread cost.** This cost was hidden: it
   was unscoped until M7.10.1b added a scope, and with 16 cars visible it took 1.10 of 2.2 ms
   (binary search per packet plus a per-frame sort).

**Fixes (byte-identical):**
- **M7.10.1:** conservative per-view frustum culling of transparent work, per packet and as a
  whole model, with motion history kept for culled glass.
- **M7.10.1b:** that bookkeeping made O(owners). The first version regressed the all-off-screen
  case by 0.18 ms, which this timing caught.
- **M7.10.3:** an open-addressing previous-transform index.

| PC1 route | Wall frame A → B (ms) | Non-wait CPU A → B (ms) | Transparent visible / requested (B) | Recorded draws A → B |
|---|---|---|---|---|
| 16× 911, all visible | 2.352 → 2.350 (now GPU-bound) | 2.186 → **1.175** | 4,320 / 4,320 | 4,788 → 4,788 |
| 16× 911, half off-frustum | 2.557 → **1.762** | 2.436 → **0.868** | 1,911 / 4,320 | 4,553 → 2,144 |
| 16× 911, all off-frustum | 0.698 → 0.698 | 0.260 → 0.227 | 0 / 4,320 | 4 → 4 |
| 16× Alfa, all visible | 2.545 → 2.562 (GPU-bound) | 0.847 → **0.638** | 976 / 976 | 2,788 → 2,788 |
| 256-instance stress | 8.255 → **7.501** | 7.938 → **4.207** | 15,616 / 15,616 | 44,548 → 44,548 |

Wall frames are means of the A and B process averages.

- In the all-off-frustum case the cars were already outside the depth range, so the old depth
  cull removed them. Whole-model rejection now skips the per-submesh work instead: extraction
  worker time fell from 0.435 to 0.007 ms.
- The fully visible scenes are now GPU-bound, so the remaining cost of adding visible cars is GPU
  work (opaque and sorted transparency).

## Case 2: camera close to glass

| PC2 (911, glass coverage) | GPU frame A → B (ms) | `sorted.forward` B (ms) | Deferred B (ms) |
|---|---|---:|---:|
| far, ~4% | 4.246 → 2.328 | 0.900 | 0.308 |
| mid, ~24% | 5.429 → 3.297 | 1.743 | 0.524 |
| near, ~58% | 6.173 → 3.977 | 2.127 | 0.676 |
| fill, ~95% | 6.215 → 4.236 | 2.475 | 0.784 |

The A → B gain is the cluster fix: the PC2 fixture has a shadowed point light.

**What remains:** the sorted-transparency pass grows to 2.5 ms as glass fills the screen.
Attribution, with single-process diagnostics in `out/m7r/timing/m7c-glassdiag`:

- **No overdraw problem:** 1.01 transparent fragments per pixel at fill (one layer).
- **Shading is not the cost:**
  - removing all forward shadow evaluation, all IBL, or both saves ≤ 0.1 ms;
  - returning a constant at the top of the fragment shader, or `discard` at the top, still costs
    2.47–2.51 ms.
- **The cost scales with resolution:** 2.49 ms at 4K and 0.645 ms at 1080p.
- **One draw carries it:** bisecting the draw range shows a single draw, the windshield (240
  well-shaped triangles, longest edge ≈ 0.23 m, 0.16–0.89 m from the camera), costs 2.45 ms. The
  other 53 sorted draws together cost about 0.03 ms.

So a single full-screen 240-triangle draw costs about 2.4 ms in fixed-function work, independent of
its fragment shader. That is roughly 30× what a full-screen pass should cost on this GPU. Candidate
units are depth testing, rasterization, or an implicit depth/colour decompression triggered by the
pass's attachment and sampling combination. Identifying the unit needs **Nsight Graphics GPU
Trace**, which requires elevated performance-counter access (owner UAC). This is the next glass
step; no change is proposed until the unit is known.

## Standard routes (refactor tier, A = `main` `8a9a601`)

Pair 1 (B = `13e6675`, after M7.10.0–M7.10.2), GPU median:

| Route | A (ms) | B (ms) |
|---|---:|---:|
| T-F1-all | 1.161 | 1.157 |
| T-F7-stack | 1.994 | 1.991 |
| T-F5-hetero | 4.933 | **0.794** (cluster 4.45 → 0.10) |
| T-F6-probecap | 3.644 | **1.426** (cluster 2.25 → 0.08) |

Non-wait CPU stays within noise, and steady allocations are 0 on every run.

Pair 2 (B = HEAD `5c87384`; adds M7.10.1b and M7.10.3), `out/m7r/timing/m7c-pair2`:

| Route | GPU A → B (ms) | Non-wait CPU A → B (ms) | CPU frame A → B (ms) |
|---|---|---|---|
| T-F1-all | 1.062 → 1.072 | 0.314 → 0.293 | 1.045 → 1.053 |
| T-F7-stack | 1.904 → 1.910 | 2.215 → **1.448** | 2.290 → **1.885** |
| T-F5-hetero | 2.960 → **0.718** | 0.576 → 0.556 | 2.936 → 0.692 |
| T-F6-probecap | 3.593 → **1.415** | 0.465 → 0.387 | 3.575 → 1.390 |

Steady allocations are 0 on every run. The T-F1 GPU +0.010 ms (+0.9%) is within run-to-run
spread; T-F1 has no local lights or transparency and none of these changes touch its GPU work.

## Identity evidence (every phase-1 fix)

- Frozen set (`m9-g8`) with synchronization validation: every capture byte-identical, except
  F4-woit, which is within its envelope. Zero validation and synchronization messages.
- Owner cases: 50/50 captures identical to the pre-fix fixtures.
- TAA-on M9 composition set: 88/88 `scene-resolved` frames identical to `1758e0d`.
- A cull and re-entry TF-glass scratch fixture (whole-model rejection for about 57 frames, TAA on):
  16/16 frames identical.
- 512-light cluster stress: image and every cluster counter identical.

## Watch item: intermittent device loss

The baseline binary (`1758e0d`) lost the device on glass-fill twice in 76 seconds (13:00:16 and
13:01:32). Both coincide with NVIDIA driver event 153 ("Error occurred on GPUID: 100"), which also
appears three times in September. The same binary then ran glass-fill 4/4 clean, and HEAD ran it
8/8 clean. There is no evidence of a code cause; recorded so a recurrence can be correlated.
