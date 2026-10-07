# M9 to M7.9 hand-off

Status: **final** (M9 accepted 2026-10-06). Branch `m9-temporal` (PR #8). Authoritative detail lives in the M9 plan's decision log
(`docs/milestones/M9-temporal-and-post.md`). This document is the brief for the next lead (M7.9–M7.12) and
for M9b.

## 1. What M9 delivered

| Area | Delivered | Where |
|---|---|---|
| History | View-keyed graph History: up to two physical sets per view (`HistoryViewSetCount`) and a per-pair `HistoryReset{OnCut, SurviveCut}`. History is valid when written on the view's previous turn. | `renderer/graph/ViewHistory.h`, RenderGraph, executor (`beginViewExecution`) |
| View motion | `ViewMotionTracker` detects cuts: requested revision, explicit, projection kind or extent, 10 m / 45° discontinuity. A FOV change is not a cut. It also produces the per-view time delta. | `renderer/rhi/ViewMotion.*` |
| Jitter | Halton(2,3) in *matrices* (`jitteredProjection`, `jitteredInverseProjection`), indexed by turns since the last cut; zero jitter is bit-identical by construction. Culling, clustering, Hi-Z, shadows, probes, LOD and layered atlas rects never see jitter. | `Mesh.h` (view record and UBO), `include/view_uniforms.glsl` |
| Previous transforms | GPU-scene settle: previous = current on the pass after an object stops. `PreviousTransformCache` keys on (owner UUID, primitive GUID). Direct packets carry previous matrices (`CanonicalMotionPushConstants`). | GpuScenePublisher, `extraction/PreviousTransformCache.*` |
| Velocity | RG16F `gbuffer.velocity` holds current − previous *unjittered* UV; 4.0 is the "no history" sentinel. GBuffer, forward-opaque and direct draws write it. Motion-vector debug view. | `include/motion_vectors.glsl`, production graph |
| Native TAA | `VulkanTemporalAntiAliasingFeature`, one compute pass: Gaussian reconstruction (sharpness 6) of the jittered 3x3; YCoCg variance clip in a pre-exposed, Karis-compressed space; closest-depth motion and camera reprojection; Catmull-Rom history; still-pixel trust gated by a range test; velocity-disagreement disocclusion (history alpha holds content motion); reactive handling from scene-colour revealage. | `taa_resolve.comp` |
| Reactive | Scene-colour alpha is **revealage**: opaque writers output 1, and blended passes multiply alpha by 1 − coverage. TAA's reactive = 1 − alpha. | Pipeline blend states, layered and WeightedOIT resolves |
| Auto-exposure | `VulkanExposureFeature`: a 128-bin log-luminance histogram (per-tile rows, fixed-order reduce, deterministic), percentile 10–90%, EV100 limits, 3 / 1 EV/s adaptation on the per-view delta. A 16 B History buffer (`SurviveCut`) feeds the output (current) and TAA (previous). | `exposure_*.comp` |
| Bloom | `VulkanBloomFeature`: dual filter, 13-tap down with Karis on level 0, tent up, 6 levels from half resolution, one transient mipped image (aliases). Energy-conserving composite `mix(scene, bloom, k)`, no threshold. | `bloom.comp`, output binding 6 |
| SR contract | `TemporalUpscaleInputs`: provider, render and output extents, jitter (px) and sequence length, exposure and previous exposure, reset, camera, frame time, reactive availability, native tuning. Native TAA consumes it today. | `renderer/rhi/TemporalUpscaleInputs.h` |
| Settings | `--anti-aliasing`, `--temporal-jitter[-sequence]`, `--taa-settings`, `--exposure`, `--auto-exposure-settings`, `--bloom`, `--bloom-settings`. Editor Project Settings > Anti-aliasing and Post-processing switch live (graph rebuild at a frame boundary). | RendererOptions, ProjectSettingsPanel |
| Gaps closed | G1–G8: view-keyed History, per-view sets and reset policy, settle publication, previous matrices for direct packets, the jitter ABI, the admission runner, temporal fixtures and metrics, the probe-promotion race (qualification drain), deterministic opaque compaction. | Plan decision log |

**Product defaults since M9.7** (owner decisions, 2026-10-05 and 2026-10-06): TAA on, auto-exposure
on (manual EV becomes compensation), and bloom on at a subtle 4% with no threshold; the Karis
prefilter is Auto, so off with TAA. Measurement tools pin TAA off, Manual exposure and bloom off through
`Get-M7REngineBaseArgs` (`tools/m7r/M7RFixtures.ps1`); a later feature flag wins.

**Admitted costs** (five-process native 4K; FRAME_BUDGET "M9 temporal and post-processing admission"):
- TAA: +0.18–0.21 ms GPU (pass 0.20 ms; the 0.40 ms row is at about 0.25 ms with velocity).
- Exposure: +0.01–0.03 ms. Bloom: +0.13–0.14 ms. The post row is at about 0.22 ms of 0.50.
- All three: +0.35–0.37 ms; the heaviest route is 3.95 ms GPU.
- Memory: TAA History is +127.5 MB at 4K.

## 2. Contracts the next milestones must keep

1. **Scene-colour alpha is revealage.** Any new opaque writer, including M7.11's material resolve, outputs
   alpha 1, or TAA reads it as reactive.
2. **Velocity.** Every opaque writer of scene colour also writes velocity: current minus previous unjittered
   UV. New geometry paths (meshlets in M8, M7.11 visibility resolve, M13 skinning) need previous positions or
   previous matrices. A new instance or a teleport writes previous = current, which TAA rejects locally.
3. **Residency and LOD swaps (M7.9).** A progressive residency or LOD change must not invent motion. Keep the
   settle semantics: previous transforms stay valid across content swaps of the same instance. For a topology
   change, the per-pixel sentinel or zero motion is correct, and TAA's neighbourhood clip handles it.
4. **Jitter.** Never feed `jitteredProjection` to culling, Hi-Z, clustering, LOD selection, shadows, probes or
   layered atlas rects. Raster stages and reconstruction read the jittered fields; everything else reads the
   unjittered ones.
5. **History.** Declare temporal state through `createHistory` (never aliased) with the right reset policy.
   Per-view sets are lazy, so set 1 exists only after a second view renders.
6. **Single output transform.** Bloom and exposure act in scene-linear AP1 before it.
7. **TAA-off identity.** The TAA-off, Manual, bloom-off route must reproduce the frozen set
   (`out/m7r/captures/m9-g8` hashes, F4-woit within its envelope) at refactor tier.

## 3. Evidence tooling (reuse it)

- **Feature admission:** `tools/m9/Run-FeatureAdmission.ps1` and `Summarize-Admission.py`. Five
  native-4K processes per side, interleaved A,B,B,A,A,B,B,A,A,B. Use `-RequireQuiet`, which refuses to run
  while other GPU or CPU load is present.
- **Captures:** `tools/m9/Run-TemporalCaptures.ps1` for sequences, holds and held 64-phase references, and
  `IridiumTemporalMetrics` (reference-error, stability, ghosting, recovery, accumulate).
- **Motion evaluation:** `tools/m9/Run-MotionEvaluation.ps1` and `Evaluate-TemporalMotion.py`. Per-frame
  held references live in `out/m9/motion/ref64`, which takes about 6.4 GB transient per 4K reference.
- **Frozen set:** `tools/m7r/Run-FrozenCaptures.ps1 -SyncValidation` with `Compare-FrozenCaptures.ps1`
  against `m9-g8`.
- **Qualification extras:** `--qualification-probe-finalize-drain` and `--qualification-exposure-trace`.
- **Disk:** C: fills quickly. Lane worktrees cost about 25 GB each with builds; delete build directories
  when a lane ends.

## 4. Known limits and open items

| Item | State | Owner |
|---|---|---|
| Sub-pixel wires (thinner than a pixel at 4K) | Thinner ones break up or vanish in some jitter phases. Candidates: minimum-width wire AA or TSR-style rejection. | Later TAA work |
| Shadow ghosting on high-contrast texture | A moving shadow's trail lasts a few frames (the range gate cannot see it there). The mean-statistics gate was rejected on cost. | Later TAA work |
| WeightedOIT soft-edge halo | The largest remaining TF-reactive error. | Later TAA work |
| Jitter-phase pulse under motion | About 0.018 / 0.024 RMSE alternation on TF-pan; a softer motion kernel was worse. | Later TAA work |
| Motion softness | On TF-pan, 17–21% of pixels exceed 1/64 against 11% without AA, though RMSE is better. A sharpening pass is not done. | Later TAA work |
| Bloom Karis prefilter | Resolved: Auto, so off with TAA (owner decision, 2026-10-06). | Closed |
| `VulkanVertexBackend.cpp` | **2,474 / 2,500 lines.** The next backend addition must first move code into a feature owner. | M7.10 |
| TF-glass fixture | `thicknessFactor 0`, so only Fresnel is visible. TF-reactive covers tinted glass. | Fixture debt |
| Layered-tier reactive | No fixture yet. Additive blend has no users. | M7.11 or a later reactive pass |
| F6 placement watch item | Closed: +1.6% against M7R final, attributed to the velocity targets and G8; no placement policy. | Closed |

## 5. What M9b needs (DLSS / FSR / XeSS and dynamic resolution)

- **Third-party SDKs require the owner's approval** (AGENTS.md): explain each SDK, its licence and its
  benefit first, and record the decision in the M9b plan.
- **Contract.** Providers implement a temporal-resolve provider interface (native TAA is provider 0) and
  consume `TemporalUpscaleInputs` unchanged:
  - colour, depth and velocity at render extent;
  - jitter in pixels plus sequence length;
  - exposure (the M9.5 state buffer, or a value);
  - reactive (1 − scene-colour alpha); a transparency-composition mask is not yet provided;
  - reset (view cut);
  - camera near/far/FOV and frame time.
- **Render versus output extent.** Today both extents are equal.
  - M9b splits them: the TAA/SR resolve writes output-extent history, and bloom, exposure and output run at
    output extent.
  - The jitter sequence length should scale with the upscale ratio squared (at least 8).
  - Texture LOD bias becomes log2(render/output), applied through the material sampler set.
  - The velocity convention (UV units) is extent-independent.
- **Dynamic resolution.** The render extent changes without rebuilding History when only the viewport rect
  changes. This needs a viewport-rect path through the graph's fixed-size targets, which is not built.
- **Evidence.** Reuse the motion evaluation against 64-phase references at output resolution, the admission
  runner, and the TAA-off identity.

## 6. Notes for M7.9–M7.12

- **First: the owner's performance cases (2026-10-07, ROADMAP M7).** Before other work, capture native-4K
  fixtures for (1) many model instances, (2) the camera close to glass and (3) a very bright point light, and
  report pass times, non-waiting CPU stages and requested/visible/recorded work. Known lead: sorted and
  compatibility transparency is not frustum-culled (an out-of-view Porsche still records every transparent
  packet and sorts 26,910 ambiguous intervals).
- **Since this hand-off (M9.8d-e):** ADR-0017 keeps history across compatible rebuilds; transparent queues now
  carry previous transforms and write motion-aware reactive coverage (dual-source alpha, `dualSrcBlend`
  required); `VulkanVertexBackend.cpp` is at 2,496 / 2,500 lines.
- **M7.9 (fine-grained cooking, progressive residency):** child streaming swaps must keep previous transforms
  (contract 3). Use the TF-teleport and TF-disocclude motion evaluation to check that no streaming swap
  ghosts.
- **M7.10 (cluster, CPU, compiler and scheduling):** TAA, exposure and bloom are three compute passes after
  scene colour. The exposure histogram reads full-resolution colour (quarter resolution is the measured
  fallback if the post budget tightens). Free headroom in `VulkanVertexBackend.cpp` first.
- **M7.11 (indexed visibility and material resolve):** the resolve must write velocity and alpha 1, and the
  material-resolve output must stay bit-compatible with the TAA-off identity route at refactor tier.
- **M7.12 (production qualification):** add TAA-on fixtures (`scene-resolved` domain) to the qualification
  sweep with measured envelopes.
