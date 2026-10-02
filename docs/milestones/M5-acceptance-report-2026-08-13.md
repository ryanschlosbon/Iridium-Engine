# M5 Acceptance Report - 2026-08-13

- Milestone: M5 - Raster lighting, shadows, probes, and baking foundation
- Status: Accepted
- Reference system: Core i9-14900K, RTX 4090, 64 GB DDR5, Windows, Vulkan 1.4
- Acceptance build: MSVC 19.51.36256.0, Debug and Release
- Detailed plan: `docs/milestones/M5-raster-lighting-shadows-probes-baking.md`
- Final qualification: `docs/performance/M5.11-production-qualification-2026-08-13.md`

## Verdict

M5 is accepted. Authored directional, point, and spot lights now feed one bounded
cluster product shared by deferred and complex-forward shading. Scene-linear AP1
direct lighting, complete cooked environment IBL, independent cached per-light
directional/spot/point shadows, contact-hardening PCSS, HDRI Sky, local reflection
probes, six-face probe capture/baking, and future-GI baked-product contracts are
production paths with explicit user/project policy and deterministic fallbacks.

The original user-facing regressions are closed: swapchain/cluster descriptor
lifetimes survive resize/maximize/restore; multiple shadow owners compose instead
of replacing each other; cooked HDRI renders rather than a black abyss; and the
dressed sample car visibly retains clearcoat/specular and normal-map detail through
the shared clustered forward path.

## Criterion-level acceptance

| Criterion | Result |
|---|---|
| Physical authored lights | Pass: Light v2 stores Rec.709/D65 color, lux/candela, metre/degree shape, source extent, shadow enable/quality/priority, deterministic migration, and scale-independent extraction. |
| Shared clustered representation | Pass: one graph-owned 32x32x24 list supplies deferred and complex-forward consumers; deterministic top-64 fallback replaces partial overflow. |
| Direct lighting and IBL | Pass: fixed demo lights and raw runtime HDR sampling are gone; physical evaluator and cooked AP1 irradiance/GGX/BRDF products are shared by both consumers. |
| Multiple shadows | Pass: two directional owners, guarded spot atlas, and tiered point cubes retain independent ownership and compose visibility per light; stale incompatible products become unshadowed. |
| Hard/soft quality | Pass: fixed 5x5 PCF and bounded physical-source PCSS are selectable by project/per-light quality with resolution, sample, owner, update, and VRAM policies. |
| Sky | Pass: stable Skybox/HDRI/Simulated component; HDRI assignment, thumbnail, persistence, background/IBL settings, and safe missing-product behavior are implemented. Skybox/simulated rendering is explicitly future work. |
| Reflection probes | Pass: sphere/box influence, bounded overlap blending, box projection, clustered selection, complete-only capture publication, GGX filtering, and reusable `.irprobe` baking. |
| Baked-lighting foundation | Pass: stable BLS1 scene owner plus typed/versioned lightmap, irradiance-volume, and visibility sections with exact invalidation and neutral fail-closed publication. No GI solver claim. |
| Scalability and future techniques | Pass: backend-neutral contracts reserve conventional/virtual/RT shadow representations, scalar/RGB visibility, GTAO/CACAO/bent normals/specular occlusion, and temporal inputs without prematurely claiming their implementation. |
| Editor/runtime separation | Pass: components remain ImGui/dialog independent; project policy and per-component controls are exposed through registries/editor drawers; runtime uses cooked products and stable GUID/UUID identity. |
| Resize and output | Pass: clustered descriptors are retired/rebound with graph resources; resize/maximize/restore, SDR, scRGB, and HDR10 are validation-clean. |
| Prior milestone preservation | Pass: final Debug and Release suites pass 69/69, including M0-M4 contracts, scene/cook determinism, materials, color/output, assets, and editor separation. |

## Frozen production contracts and ADRs

`m5_11Acceptance` in `assets/benchmarks/m5/run-manifest.v1.json` freezes the final
Light, Sky, reflection-probe, baked-set/product, shadow/project-policy, and
qualification-fixture hashes. `M5FixtureContractTests` regenerates/validates the
scene/cooked contract and every frozen file hash in both configurations.

- ADR-0007: photometric lights and shared clustered assignment.
- ADR-0008: raster-shadow ownership, caching, and stale-product safety.
- ADR-0009: multi-owner per-light visibility and user project policy.
- ADR-0010: high-fidelity sky/shadow/AO evolution and explicit milestone handoffs.
- ADR-0011: baked-lighting product and scene ownership for future GI.

No accepted ADR was silently contradicted or superseded during final cutover.

## Verification

- Debug build: pass; CTest 69/69.
- Release build: pass; CTest 69/69.
- Contract test: pass in Debug and Release after final hash freeze.
- Vulkan validation: clean for the cooked-HDRI sample car, opposing spot/point PCSS
  fixtures, final/normal captures, scRGB, and HDR10.
- Visual: finite cooked-HDRI scene radiance; final-SDR car SHA-256
  `5cc47a3e51e6aa873fa3a930f2c2b53c2757ae3ec51068938fbab1140768b9ca`;
  normal diagnostic SHA-256
  `90fea82a5d3df4a06d1b1964e9d023eb6e7876848d0f7405709fc5a5a6d2fea5`.
- M5.7: 20 independent on/off processes, 200,000 measured native-4K frames,
  zero frame/GPU-range drops. Two spots add 0.081248 ms median; two points add
  0.406368 ms. Final allocation evidence comes from the profiler-fixed car gate.
- Final dressed car: five processes, 50,000 measured native-4K frames, zero frame
  and counter drops; 4.238432 ms GPU median of medians, 4.484928 ms worst p95,
  4.520544 ms worst p99.

## Performance and memory decision

The dressed car remains well inside the 10 ms base-render contract. Its median pass
costs include 1.929536 ms cluster assignment, 0.334848 ms deferred lighting,
1.249280 ms complex forward, 0.123904 ms GBuffer, and 0.061440 ms output transform.
Requested live/peak memory is 1,492.982/1,587.977 MiB and committed live/peak is
1,563.056/1,658.052 MiB. The explicit shadow reservation is 128 MiB directional and
400 MiB local; texture/environment residency is 51.758/29.276 MiB.

This is intentionally not called an M4 regression comparison: M4's 1.5662 ms GPU
car was undressed and preserved old output, while the M5 car adds cooked 4K HDRI,
three shadow owners, PCSS, clustered assignment, and complex-forward lighting.

Final audit fixed a profiler-capacity defect that had dropped 12 M5 counters. The
truthful steady result is a constant 39 C++ allocations / 5,288 requested bytes per
retained car frame. It is bounded and shows no memory growth, but restoring M4's
zero-allocation standard by replacing temporary frame vectors with persistent
scratch is a documented optimization carry-forward.

## Visual and material decision

The final car does not need a separate forward-only renderer to be reflective.
Standard surfaces remain deferred; active clearcoat and other complex lobes use the
complex-forward queue. Both consume the same clustered lights, per-light shadows,
environment/probe IBL, material records, normal maps, and shared BSDF functions.
The final and normal captures confirm reflective paint/chrome/lenses plus detailed
wheel, tire, grille, lamp, suspension, and body normals.

## Remaining risks and deliberate deferrals

- Per-frame C++ scheduling/diagnostic allocations are bounded but should return to
  zero in later renderer work.
- Default conventional shadow capacity consumes 528 MiB at Ultra defaults. Project
  resolution/tier/capacity settings are explicit; later sparse virtual pages must
  earn adoption with matched 4K quality, timing, and residency evidence.
- Bloom is not implemented. The graph hook is skipped and owns no settings or cost.
- Colored translucent shadows/refraction are M6; GTAO/CACAO/specular occlusion,
  physical atmosphere/clouds, and GI solvers are M10; hybrid RT visibility is M11.
- Screen-space shadows may later add diagnosed contact detail but never replace
  off-screen/per-light visibility. VSM and virtual shadow maps remain distinct.

## Architecture and review status

M5.0-M5.11 are complete, ROADMAP and PROJECT_CONTEXT are updated, and the
completion report is finalized. The work was performed sequentially by the
milestone lead; no subagent or separate human reviewer was available. All raw
profiles, capture sidecars, cooked products, and validation outputs are retained
under `out/acceptance/m5/` for independent audit.
