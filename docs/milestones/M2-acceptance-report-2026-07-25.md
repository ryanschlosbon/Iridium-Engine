# M2 Acceptance Report - 2026-07-25

## Decision

M2 is accepted. The production renderer now consumes provenance-preserving compiled
materials and canonical closures through the render graph. Candidate R, the complete
36-byte/pixel canonical reference surface cache, remains the near-term production
layout because the faster Q/C experiments discard required F90 and metadata.

## Criterion review

| Acceptance criterion | Result and evidence |
|---|---|
| Source/compiled/instance/GPU separation | `SourceMaterial`, `CompiledMaterial`, immutable `MaterialInstance`, and schema-2 `PackedGpuMaterial` are distinct contracts with deterministic hashes and tests. |
| glTF factors/defaults/textures/channels/samplers/UV/color/alpha/two-sided | Imported with provenance and validated by source, compatibility, compiler, runtime, and tracked fixture tests. |
| Unsupported/approximated/conflicting diagnostics | Structured compiler/packer diagnostics and searchable editor snapshots expose severity, code, source default/authored state, and closure reason. Required/invalid conflicts fail. |
| Canonical standard closure | Diffuse albedo, RGB F0, scalar F90, perceptual roughness, shading normal, AO, scene-linear AP1 emissive, IDs, and flags are represented end to end. |
| MR and spec/gloss conversion | CPU reference vectors and shader/runtime tests validate equivalent single-lobe conversion; archived spec/gloss import is explicit. |
| Complex classification | Active coat, sheen, anisotropy, iridescence, thin/volume/diffuse transmission, dispersion, and unlit use explicit tagged forward records. Dormant zero-effect lobes may remain standard with diagnostics. |
| Shared BSDF | Deferred and forward shaders share GGX/Smith/Fresnel, normal, tangent, F0/F90, and numerical conventions suitable for later RT reuse. |
| GBuffer comparison | R/Q/C formats, 4K images, GPU time, graph memory, and semantic loss are recorded in M2.8/M2.9. R retained. |
| Material diagnostics | Editor displays source values and origins, textures/interpretation/UVs, compiled values, instance values, GPU packing, closure, and warnings. |
| Sample car | Paint, windows, lenses/headlights, normal/roughness sources, zero authored emissive, glTF defaults, extension conflicts, and queue classifications are recorded and validation-clean. |
| Production graph cutover | Graph/canonical path unconditional; manual renderer and deprecated flags/shaders removed. |
| M1 invariants | AP1 scene composition, one output transform, SDR/scRGB/HDR10, UI, capture provenance, output timing, and memory accounting pass. |

## Integrated verification

- Hardware: RTX 4090, i9-14900K, 64 GB; NVIDIA 610.74; Vulkan 1.4.341.
- Debug build + 20/20 CTest.
- Release build + 20/20 CTest.
- Eleven tracked M0/M1/M2 fixtures at 3840x2160 with Vulkan validation: clean.
- Optional sample car at 3840x2160 with validation: clean.
- R/Q/C tracked equivalence captures: bit-identical.
- SDR/scRGB/HDR10 scene captures: identical AP1 hash.
- Selected material-lab and selected-car captures: mesh shading retained; cyan mask
  boundary composited last.
- Nsight Graphics 2026.2 capture and 20-loop replay: successful.

## Final reference baseline

Five independent Release runs, each 500 warm-up + 10,000 measured frames:

| Metric | Five-run median |
|---|---:|
| wall frame | 0.659179 ms |
| CPU frame | 0.631800 ms |
| GPU frame | 0.455296 ms |
| GBuffer | 0.012288 ms |
| deferred lighting | 0.124928 ms |
| opaque complex forward | 0.015360 ms |
| output transform | 0.038912 ms |
| UI | 0.106496 ms |
| requested live / peak | 861.158 / 893.159 MiB |
| committed live / peak | 931.003 / 963.005 MiB |
| steady C++ allocation median / p99 | 0 / 0 calls, 0 / 0 bytes |

This proxy scene validates attribution; it is not a fully dressed gameplay claim.

## Regressions closed during acceptance

- Complex opaque materials no longer use transparent sorting or no-depth-write
  pipelines.
- Forward materials no longer disappear from material debug views.
- Selection no longer replaces/darkens mesh shading and is no longer covered by
  later forward passes.
- Roughness no longer incorrectly scales environment-reflection energy to zero.
- HDR controls clearly expose active transport, exposure, paper white, and peak;
  Windows HDR does not implicitly switch an SDR swapchain.

## Post-acceptance opaque-coverage correction

The M3 hard precondition exposed a separate M2 production regression after this
report was accepted. The selected sample car showed cyan selection coverage over
the wheels while the ordinary rendered view exposed the environment through the
same surfaces. Selected and unselected 4K captures reproduced the defect. Final,
depth, base-color, normal, material-ID, closure-class, and wireframe diagnostics
confirmed missing opaque depth rather than dark lighting or absent geometry.

The wheel-region material-ID crops contain IDs 9, 15, 29, 78, and 84. Every source
primitive behind those IDs was inspected. The current importer globally merges all
primitives sharing one material, so a grouped material may also contribute geometry
outside the crop; that loss of individual production primitive identity is an M3
cooker defect recorded in the proposed plan.

| Source primitive | Material | Source index count | Production `firstIndex` / `indexCount` |
|---:|---:|---:|---:|
| mesh 11, primitive 0 | 9 | 19,542 | 214,680 / 19,542 |
| mesh 17, primitive 0 | 15 | 5,952 | 268,521 / 69,342 |
| mesh 56, primitive 0 | 15 | 35,544 | 268,521 / 69,342 |
| mesh 57, primitive 0 | 15 | 27,846 | 268,521 / 69,342 |
| mesh 34, primitive 0 | 29 | 23,544 | 375,735 / 91,905 |
| mesh 37, primitive 0 | 29 | 27,609 | 375,735 / 91,905 |
| mesh 39, primitive 0 | 29 | 27,210 | 375,735 / 91,905 |
| mesh 41, primitive 0 | 29 | 13,542 | 375,735 / 91,905 |
| mesh 104, primitive 0 | 78 | 25,326 | 816,879 / 47,832 |
| mesh 105, primitive 0 | 78 | 22,506 | 816,879 / 47,832 |
| mesh 113, primitive 0 | 84 | 15,960 | 865,617 / 47,880 |
| mesh 115, primitive 0 | 84 | 31,920 | 865,617 / 47,880 |

All five wheel-region materials are source-default `OPAQUE`, explicitly double-sided,
have no authored material extension that changes routing, and compile to
`StandardDeferred`. They therefore share the same required GBuffer/depth behavior.

Material 84 is source-default `OPAQUE`, explicitly double-sided, metallic
`0.9798125099`, roughness `0.5409540296`, and has base-color and normal textures.
It compiles to `StandardDeferred` and must use the opaque queue,
`CanonicalPbrGBuffer`, the GBuffer pass, no culling, opaque blending, depth test
`Less`, and depth writes. Its production material-table index is 84. Extraction
copies the same geometry handle, `firstIndex`, and `indexCount` packet into the
selection queue, so the selection and production draws were proven to use identical
geometry ranges.

The root cause was the canonical-routing override in `AssetManager`: it set
`depthWrite = canonicalForward && !coverageBlend`. That correctly enabled depth for
forward-opaque closures but disabled it for every standard deferred material.
Fragments still executed and wrote GBuffer color/material data, but depth remained
at the clear value. Deferred lighting therefore classified those pixels as
background and returned the environment. The selection-mask pipeline did not
expose the error because it intentionally draws the copied packet without ordinary
depth testing.

The correction derives depth-write behavior from the final render queue:
`Opaque` and `ForwardOpaque` write depth; `Transparent` does not. A shared
`renderQueueWritesDepth` contract and architecture test prevent the queue/pipeline
state from diverging again. The optional sample-car manifest is revision 2 and
requires material-84 depth coverage. `tools/ValidateMaterialCoverage.py` compares
scene-linear material-ID and depth PFM captures and fails if an identified material
pixel retains background depth.

The deterministic 3840x2160 comparison was decisive:

| Capture | Material-84 pixels | Background-depth pixels | Valid depth range |
|---|---:|---:|---:|
| Before correction | 545,427 | 545,427 | none |
| After correction | 516,502 | 0 | 0.9189453125-0.94970703125 |

The lower corrected pixel count is expected: restored depth allows ordinary
visibility and occlusion instead of retaining every overwritten GBuffer fragment.
The verified revision-2 capture hashes are:

- selected Final: `61e39830566fe6ddd93a42cc415e8591d1b5d407e5a39ded9c4c85ab746909ed`;
- unselected Final: `2d826a3abff60097fd223b696c609fa038be3416c59fdf69d7831e870709c4f4`;
- Depth: `686e0408ab608446cf5a740c1fb05268fdfa9eb7255465a9d4d5c8ae146f16db`;
- Base Color: `c2cb4b1f35bbb057aa64158a355262b829dae795241007248ca44ff5cc73fdd8`;
- Normal: `a3b407ac9ad9d018c03396dadb0a8804c78d613db1eefe54aad7cb77c4439943`;
- Material ID: `496d487c83664d3f8b9dd3a20f8f5c210237155726e3e34f72432402880df996`;
- Closure Class: `83d65ba21f0d83b7571cd0e03e9b024d16bc01f0a39bdf1af2d421506c422516`;
- wireframe: `b2f93da5f50b3efd582e1639cde21df7928a24609519d3a71179c266643e1cbd`.

Debug and Release remain 20/20 CTest. The tracked standard, forward, and complex M2
GPU material fixtures plus the legacy material, transparency, and emissive fixtures
are validation-clean at 4K. All revision-2 car captures above are validation-clean.
A 10,000-frame Release car run at 4K recorded 0.955 ms median / 1.514 ms p95 GPU
frame time, 1.612 ms GPU p99, 5.050 ms average wall frame, and 1.186 ms median CPU
frame time. CPU frame p95 was 16.062 ms, dominated by a 15.562 ms swapchain-acquire
wait while the hidden benchmark outran presentation rather than render preparation.
The directly comparable one-frame Debug diagnostic changed the unselected GBuffer
range from 0.694 ms to 0.190 ms; that single sample is diagnostic rather than an
acceptance statistic, but confirms that restoring early depth coverage did not
introduce a performance regression.

M2 remains accepted. No ADR changed. M3 remains Proposed pending approval of its
execution plan.

## Deferred work

M5 owns production clustered direct lighting, IBL, shadows, and probes. M6 owns
general transparency ordering and advanced glass/refraction. M7 owns the measured
visibility-buffer path after persistent GPU-scene identities exist. Those milestones
must consume the M2 closure and surface contracts rather than reintroduce authoring
workflow or display-referred material math.

## Fresh-lead handoff

Read `AGENTS.md`, `ROADMAP.md`, `PLANS.md`, `docs/PROJECT_CONTEXT.md`,
`docs/performance/FRAME_BUDGET.md`, every ADR, the M0/M1 acceptance reports, the M2
milestone plan, and this report. Current source and the dirty worktree are
authoritative. Reproduce the current acceptance contract with
`assets/benchmarks/m2/run-manifest.v1.json`; do not use the removed graph/material
flags. No information from the originating conversation is required.
