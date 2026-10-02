# M1 Acceptance Report: Render Graph, Scene-Linear HDR, and Color Management

- Decision: Accepted
- Date: 2026-07-22
- Plan: `docs/milestones/M1-render-graph-hdr-color.md`
- Primary ADR: `docs/architecture/ADR-0002-render-graph-hdr-color.md`
- Baseline commit: `30252593f8fdd2de5dffbb8da31bb570ff49a7c0`
- Worktree: intentionally dirty; all pre-existing work preserved

## Criterion decision

| Acceptance criterion | Decision | Evidence |
|---|---|---|
| Backend-neutral compiled render graph | Pass | Deterministic validation/topology tests; Vulkan executor owns graph images, transitions, reuse, and frame-context retirement. |
| No per-frame compile/allocation | Pass | Cached graph plan has zero misses after rebuild; steady C++ allocation median is zero in all five-run Release sweeps. |
| Graph parity and overhead | Pass | M1.3 bit-exact SDR comparisons; graph-only CPU/GPU deltas remain within 0.05 ms gates. |
| One scene-linear HDR composition domain | Pass | Opaque, environment, emissive, refraction, and transparency share FP16 ACEScg/AP1 before output; scene captures retain values above white. |
| One explicit output transform | Pass | ACES 2 is default; legacy fitted and identity are explicit SDR compatibility choices; no intermediate shader tone maps. |
| SDR color/UI gate | Pass | M1.5 acceptance and Nsight evidence prove exact SDR patches and exposure-independent UI. |
| HDR transport/color/UI gate | Pass | scRGB and HDR10 use exact negotiated pairs; inspected linear references and target-window captures pass grayscale, saturation, highlight, transparency, and UI gates. |
| HDR metadata/fallback | Pass | Optional extension is reported independently, metadata is applied only to HDR10, and unsupported requested HDR falls back explicitly to known SDR. |
| Capture provenance | Pass | Scene, final SDR, and final HDR artifacts identify domain, primaries, transfer, operator, ACES ID, exposure, paper white, peak, and exact content hashes. |
| 4K output performance | Pass | Output transform 0.043008–0.049152 ms median and <=0.050176 ms p99; combined output/UI 0.144384–0.266240 ms. |
| Memory reconciliation | Pass | Worst transport total requested peak is scRGB at 886.662 MiB, below M0's 949.220 MiB; HDR10 composition aliases a graph slot. |
| Debug validation/resize/cleanup | Pass | All required fixtures and transports are clean at 4K; targeted HDR resize/rebuild/normal-close runs are clean. |
| Automated suites | Pass | Debug 15/15 and Release 15/15 CTests. |

## Final architecture

The accepted frame is:

```text
source decode and Rec.709/sRGB -> ACEScg/AP1
  -> graph-owned FP16 opaque/environment/emissive/transparency composition
  -> optional bloom hook
  -> manual EV
  -> pinned ACES 2 target rendering/gamut transform
  -> display-linear target-gamut UI at paper white
  -> SDR fixed-function sRGB, scRGB 80-nit scale, or HDR10 ST.2084
  -> negotiated swapchain and present
```

The compiled SDR/scRGB graph has 12 passes and 9 logical resources. HDR10 has 13
passes and 10 logical resources because `ui-compose` writes a reusable FP16 target
and `hdr10-encode-present` performs the only PQ encoding. The graph uses seven SDR
physical slots and six compatible FP16 HDR slots across two fenced frame contexts.

The legacy manual path remains available only as a deprecated diagnostic rollback.
Graph execution is the accepted production path. Its owner is the renderer lead and
its removal target is M2 after material-compiler integration has retained the same
image, validation, and timing evidence.

## Baseline evidence for later milestones

- Required 4K M0 fixtures: material lab, nested transparency, opaque/emissive,
  legacy lighting, geometry CPU grid, and temporal proxy all validation-clean.
- Comprehensive fixture content SHA-256:
  `76e4fd4952101c1bf54b28c5d48001851c0708b7b845cade869aa060c98c2072`.
- Scene-linear 4K SHA-256 across all transports:
  `fbda36388bac4884ca3394494d812b50de412f9d8ac268608863633193eb9515`.
- Final SDR 4K SHA-256:
  `3457b6cc4b05fff502c3c62dc1d545cccfd5d84b4160308e2afd5321cf869316`.
- Final scRGB linear-reference SHA-256:
  `2cb67abb8b75c1a590cd13616973ba1c4ac054dae534d2131658c8f4f8d53eca`.
- Final HDR10 linear Rec.2020 reference SHA-256:
  `e126218a93b6fccf4c95faa27dcd19429e6042b7fc1ea73504efaa42a5fd50af`.
- Performance JSON Lines: `out/evidence/m1/m1.7-performance/`.
- Validation, resize, capture, and visual artifacts:
  `out/evidence/m1/m1.7-validation/`, `m1.7-resize/`, `m1.7-captures/`, and
  `m1.7-target-display/`.

## Residual risks and deferrals

- Automatic exposure, TAA/reconstruction, and real history consumers remain M9.
- Bloom is a graph hook, not an implemented effect.
- Current classified transparency remains intentionally limited; M6 owns general
  ordering/refraction quality.
- The 1000-nit ACES transform is the frozen M1 HDR mastering preset. Peak and paper
  white are user-configurable and serialized; adding other ACES mastering peaks
  requires additional validated transform assets rather than rescaling scene light.
- Nsight 2026.2 CLI retries produced no new artifact in the final environment. The
  retained M0/M1.5 captures continue to validate the unchanged timestamp/debug-marker
  mechanism; new HDR ranges are internally timed and Vulkan-validation clean.

## Fresh-lead handoff

M1 is complete. A fresh lead should read `AGENTS.md`, `ROADMAP.md`, `PLANS.md`,
`docs/PROJECT_CONTEXT.md`, `docs/performance/FRAME_BUDGET.md`, all ADRs, this report,
and the current source/status. The next roadmap milestone is M2, but its execution
plan requires owner approval before implementation.
