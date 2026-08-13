# Iridium Engine Development Roadmap

## Purpose

This is the durable program-level roadmap for the engine after the RHI refactor. It records milestone order, dependencies, acceptance gates, and work intentionally deferred. Detailed execution plans follow `PLANS.md` and live under `docs/milestones/`.

Status values: `Proposed`, `Ready`, `In Progress`, `Blocked`, `Accepted`.

## Product and performance goals

- High-fidelity, future-facing Vulkan renderer for high-end PCs.
- 4K HDR gameplay at more than 100 FPS on the reference system, using native rendering, DLAA, or high-quality temporal reconstruction as appropriate.
- Linear scene-referred HDR lighting and transparency with SDR, scRGB, HDR10, and useful wide-gamut output paths.
- A complete high-quality raster path before ray tracing becomes required for baseline lighting.
- Scalable CPU/GPU architecture: GPU scene, indirect visibility, mesh shaders, and later ray tracing.
- Modular assets, scenes, ECS, editor, and serialization suitable for an expanding engine rather than one demonstration model.

Frame generation is not counted as meeting the engine's simulation or base-rendering frame-time target.

## Accepted architectural direction

The ADRs contain the full rationale. The current direction is:

1. Use a render graph for pass dependencies, resource state, transient allocation, history, and external SDK integration.
2. Keep opaque lighting, emissive, transparency, bloom, and temporal effects in linear HDR. Tone-map and gamut-map once at output.
3. Preserve rich source material workflows. Compile ordinary single-closure materials to a canonical standard closure containing diffuse albedo, F0/F90, and roughness, independent of whether conventional deferred, visibility-resolved, forward, or ray-tracing execution consumes it.
4. Render genuinely multi-lobe, transmissive, or otherwise uncommon closures through a forward complex-material path using the same BSDF library.
5. Use stable GUID-based source assets, metadata sidecars, dependency tracking, and a derived-data cache. Separate import, cooking, and GPU upload.
6. Use a versioned JSON editor scene format and a cooked binary runtime format. Register component serializers by stable component IDs.
7. Move component drawers and editor interaction out of ECS component types.
8. Build a persistent GPU scene and indexed indirect path before adding a mesh-shader emission path.
9. Preserve meshlet, bounds, material, and triangle data needed by classic raster, mesh shaders, and BLAS construction.
10. Use classified transparency paths rather than one universal sorting or OIT algorithm.
11. Evolve from the measured packed deferred path toward a hybrid renderer: standard
    opaque visibility/material resolve feeding a canonical surface cache and clustered
    deferred lighting, with clustered forward for complex and transparent classes.
    Retain classic indexed and packed-deferred fallbacks; do not run redundant full
    GBuffer and visibility representations without measured benefit.

## Milestones

### M0 - Reference scenes and profiling foundation

Status: `Accepted` on 2026-07-18

Establish trustworthy baselines before changing renderer architecture.

Deliverables:

- CPU frame-stage timers and Vulkan GPU timestamps.
- Per-frame draw, triangle, material, pipeline, transparent-overdraw, and VRAM statistics.
- Reference scenes for material spheres, the sample car, transparent nesting, emissive range, lighting, and large-scene CPU stress.
- Deterministic screenshot capture and reference-image comparison workflow.
- Debug views for base color/diffuse, F0 or metallic, roughness, normals, emissive, depth, material ID, and motion vectors as they become available.
- Recorded baseline on the reference PC at 4K in Debug and Release where meaningful.

Acceptance gate: satisfied. The criterion-level decision and final 4K baseline are
recorded in `docs/milestones/M0-acceptance-report-2026-07-18.md`.

### M1 - Render graph, linear HDR, and color management

Status: `Accepted` on 2026-07-22

Dependencies: M0.

Execution plan: `docs/milestones/M1-render-graph-hdr-color.md`. Its color/output
choices and M1.0 preflight were accepted on 2026-07-19. M1.1 through M1.4 are also
accepted; M1.5 final SDR and M1.6 scRGB/HDR10/PQ transport, metadata, capture, and
color-managed UI are accepted. The criterion-level report is
`docs/milestones/M1-acceptance-report-2026-07-22.md`.

Deliverables:

- Backend-neutral render-graph declarations with Vulkan execution and barriers.
- Transient, persistent, and history resource classes.
- FP16 or equivalently suitable linear scene-color target.
- Opaque and transparent composition before tone mapping.
- Exposure, bloom integration point, final tone mapping, and gamut mapping.
- SDR output plus negotiated scRGB/HDR10 paths and HDR metadata where supported.
- Display/paper-white/peak-nits configuration and color-managed UI composition.

Acceptance gate: satisfied. Opaque, emissive, sky/environment, and glass compose in
one scene-linear HDR pipeline; SDR, scRGB, and HDR10 validate on the reference
hardware. The next milestone is M2; its execution plan was approved on 2026-07-22.

### M2 - Material compiler and canonical shading closure

Status: `Accepted` on 2026-07-25

Dependencies: M0, M1.

Execution plan: `docs/milestones/M2-material-compiler-and-closures.md`. Acceptance:
`docs/milestones/M2-acceptance-report-2026-07-25.md`. Rich glTF inputs now compile
through distinct source, compiled, instance, and schema-2 GPU contracts. Standard
closures use the canonical deferred cache; active complex lobes use explicit opaque
or transparent forward queues with shared BSDF conventions. Material diagnostics,
HDR/output controls, uniform transform editing, text-enterable numeric controls, and
non-destructive final-output selection outlines are integrated.

The measured 36-byte/pixel reference cache R remains production. Faster Q/C
experiments saved 20.1%/52.2% of matched GBuffer+lighting time but lost scalar F90
and full metadata, so they remain experiments. The production graph is unconditional
and the manual renderer/deprecated flags are removed. ADR-0006 records how these
contracts migrate to clustered lighting and visibility-buffer rendering in M5/M7/M8.

Deliverables:

- `SourceMaterial`, compiled material, material instance, and packed GPU material separation.
- Rich glTF material parsing with support policy and diagnostics for relevant Khronos extensions.
- Canonical standard closure: diffuse albedo, F0, perceptual roughness, normal, AO, emissive, and model/feature flags.
- Shared BSDF library used by deferred/material-resolve, forward, and future RT rendering.
- Reference and near-term production GBuffer/surface-cache layouts with image-difference validation and an explicit visibility-resolve compatibility contract.
- Clear classification between standard deferred and complex forward closures.
- Material inspector/debug reporting showing source values, defaults, compiled values, textures, color spaces, and warnings.

Acceptance gate: satisfied. glTF metallic/roughness and specular/gloss reference
assets are consistent and explainable; the sample car's authored/default/extension
behavior is visible and validation-clean. Debug/Release pass 20/20 tests, eleven
tracked 4K fixtures and SDR/scRGB/HDR10 validate, scene captures remain
transport-independent, and Nsight frame capture/replay succeeds.

### M3 - Asset registry, cooker, and browser

Status: `Accepted` on 2026-07-31

Dependencies: M0; coordinate with M2 and M6 data needs.

Execution plan: `docs/milestones/M3-asset-registry-cooker-browser.md`. Acceptance:
`docs/milestones/M3-acceptance-report-2026-07-31.md`. Stable source-controlled
identity, deterministic import/cook/DDC, dependency-driven reimport, cooked-only
runtime publication, scalable indexed material/texture resources, and the
project-owned Asset Browser workflow are production.

Deliverables:

- Stable asset GUIDs and source-controlled metadata sidecars.
- Importer registry, importer versions, settings hashing, dependency graph, and derived-data cache.
- Separation of source parsing, CPU cooking, runtime blobs, and scheduled GPU upload.
- Scalable GPU-indexed material/texture indirection with per-use color-space, sampler,
  and view semantics plus an explicit capability/fallback policy for later material
  resolve.
- Model and texture import settings, reimport, validation, thumbnails, search, filtering, and drag/drop.
- Asset browser as the primary import/assignment workflow; remove import responsibility from the scene hierarchy.
- Cooked geometry retains spatial primitive boundaries, transparent-surface bounds, optional meshlets, and RT-compatible source data.

Acceptance gate: satisfied. Assets import and reimport through the project-owned
Browser, retain GUID identity across moves/renames, persist in scenes by GUID, and
rebuild byte-deterministically from source plus metadata. Debug/Release pass 39/39;
all tracked M0-M2 cooked fixtures and the 4K car pass Vulkan validation. Scale gates
cover 100,000 catalog records, 65,536 resident materials, 8,192 texture
views/samplers, dependency fan-out, rapid reimport, and residency churn.

Accepted deferrals: remote/shared DDC and virtualized payloads remain future
production infrastructure and require a separately approved plan. Progressive
independent texture publication is carried into M7; the isolated interactive asset
viewer is carried into M4.

### M4 - Scene schema, ECS identity, and editor separation

Status: `Accepted` on 2026-08-03

Dependencies: M3 for asset references.

Execution plan: `docs/milestones/M4-scene-schema-ecs-editor-separation.md`.
The owner approved execution on 2026-07-31 and delegated the remaining product
choices to the milestone lead. M4.0 fixture/baseline, M4.1 generational
handle/persistent identity, and M4.2 source-schema/registry/migration slices are
accepted. M4.3 staged load, atomic save/recovery, document lifecycle, menu-command
workflow, scale evidence, and 4K validation are accepted. M4.4 deterministic cooked
runtime scenes is accepted with byte-identical cross-process cooks, strict M3
DDC/receipt behavior, source-free staged runtime loading, a clean runtime-only link
boundary, and 1k/10k/100k scale evidence. M4.5 runtime component/editor drawer
separation is accepted with stable-ID generic/custom registration, runtime/UI link
separation, Debug/Release 57/57 coverage, measured registry/dispatch evidence, and
maximized/compact Vulkan-validation interaction checks. M4.6 transaction service,
property undo/redo, asset assignment, multi-selection, history-aware dirty state,
and failure presentation is accepted with Debug/Release 58/58 coverage, measured
1/100/10k-target costs, like-for-like whole-editor allocation evidence, a 4.1 us
median selected-entity gizmo path, and live Vulkan-validation shortcut/multi-edit
checks. M4.7 structural editing, hierarchy reconstruction, and viewport extent is
accepted with UUID-backed atomic commands, iterative hierarchy reconstruction,
panel-driven scene targets independent of presentation extent, repeated live
4K/1600x900 Vulkan-validation resizing, deterministic resized captures, Debug/Release
58/58 coverage, and measured 100k-hierarchy/10k-command evidence. M4.8 evidence-
driven ECS/query tightening is accepted with a demand-paged sparse component index,
Debug/Release 59/59 coverage, 100k-operation randomized property testing, a repeated
30-sample scale matrix, and preserved five-run M3 4K frame/VRAM/allocation gates.
Random lookup p95 improved 72.6-98.6%, representative view p95 improved 57.6-75.0%,
and dense iteration improved 1.0%. M4.9 reusable isolated model/material asset
documents are accepted with stable GUID tabs, shared-root runtime pins, orbit/
pan/dolly/bounds framing, production debug presentation, scene/history isolation,
Debug/Release 60/60 coverage, distinct validation captures, and sub-0.05 ms median
GPU overhead for the five-draw fixture. M4.10 completed the production cutover:
the legacy central serializer and expected-defect target are deleted, v0 survives
only as a pure migration, editor changes cannot persist outside transactions, and
frozen source/canonical/registry/CookKey/artifact hashes guard the production
contract. Debug and Release pass 60/60; the full 30-sample ECS matrix, 100k cooked
load, five-run 4K car, and validation-clean exact M3.7 image comparison pass. The
criterion record is `docs/milestones/M4-acceptance-report-2026-08-03.md`.

Deliverables:

- Stable scene entity UUIDs distinct from runtime generational entity handles.
- Versioned top-level schema and per-component versions.
- Explicit component serializer/migration registry and multi-phase reference fixup.
- Deterministic JSON source scenes, unknown-component preservation, atomic saves, and cooked binary scenes.
- Cache-friendly sparse component lookup and query/view improvements based on measured needs.
- Editor-only component drawer registry with undo-aware property editing.
- Reusable asset-opening/editor framework with an isolated model/material viewer,
  orbit controls, and material/debug presentation independent of scene placement.
- Runtime components no longer depend on ImGui or native file dialogs.

Acceptance gate: satisfied. Stable registries replace central serialization;
sidecar-namespaced v0 migration preserves identity; unknown payloads round-trip;
runtime scene loading is source-free and editor-independent; and all persistent
editor mutations are transaction/command-service owned.

### M5 - Raster lighting, shadows, probes, and baking foundation

Status: `Accepted` (M5.0-M5.11 complete on 2026-08-13; M5.12 reflection and
M5.13 shadow post-acceptance hardening complete on 2026-08-13)

M5.7 correction: ADR-0009 replaces the M5.6 single-sun capacity with two
independently cached directional owners and per-light visibility composition.
Shadow project policy is backend-neutral and user-configurable. The persistent,
guarded spot atlas and tiered point-cube pools now render, cache, and sample
independent per-light shadows through both lighting consumers. Matched opaque
complex-forward spot/point visibility now passes at SSIM 0.999987/0.999947;
the final 20-process on/off gate covers 200,000 measured native-4K frames. Two
spots add 0.081248 ms median and two point cubes add 0.406368 ms median.
M5.8 is complete. It has the stable source/cooked/editor reflection-probe component,
deterministic scale-independent world extraction and residency gating, sphere/box
influence, four-candidate/two-probe blending, smooth global fallback, and box-
parallax contracts. A backend-neutral 112-byte GPU record ABI, stable incremental
publication, bounded capacity diagnostics, and coherent four-probe-per-cluster CPU
oracle and Vulkan compute assignment are implemented. A bounded indexed local-
environment table is consumed by the shared deferred and complex-forward specular
IBL path with per-pixel selection. The backend-neutral six-face orientation,
priority/update-budget scheduler, revision invalidation, private partial-capture
staging, and complete-only publication handoff are implemented. Vulkan owns a
bounded raw-radiance/depth/full-mip-prefilter capture-target pool whose in-flight
storage is physically separate from last-known-good published cubes. Opaque and
opaque-complex scene radiance, authored lights/shadows, optional global sky, owner
exclusion, recursive-probe suppression, configurable GGX filtering, and indexed
publication are live. Baked mode reads back a layer-major cube, derives SH9 diffuse
irradiance, writes an atomic versioned `.irprobe` with UUIDv7 metadata and scene/
shader CookKey provenance, and refreshes the Asset Browser. Debug/Release pass
68/68; a full 512 capture/filter costs 1.234144 ms GPU, steady one-probe overhead is
0.008448 ms median GPU, and final 4K plus baked-readback runs are validation-clean.
M5.9 is complete with bounded source-size-driven PCSS for directional, spot, and
point maps plus fixed 5x5 fallback and frozen future visibility/AO handoffs. M5.10
is complete with the stable `iridium.component.baked_lighting_set` scene owner and
versioned `iridium.baked-lighting` product: typed lightmap/entity/primitive,
irradiance-volume, and visibility sections; exact provenance/invalidation hashes;
strict corruption/version rejection; and last-known-good neutral-safe publication.
Debug/Release pass 69/69. A five-process 55,934,252-byte Release contract benchmark
loads and validates in 8.9649 ms median and publishes in 8.9994 ms median. This is a
foundation measurement, not a production GI quality or frame-time claim.
M5.11 removes the last stale fixed-light diagnostic, freezes the production
contracts, and qualifies the cooked-HDRI dressed sample car. Five independent
native-4K Ultra-PCSS processes measure 4.238432 ms GPU median of medians,
4.484928 ms worst p95, and 4.520544 ms worst p99 with zero frame/counter drops.
The final car visibly retains clearcoat/specular and wheel/tire normal detail;
SDR, scRGB, HDR10, opposing spot/point shadows, and resize lifecycle evidence are
Vulkan-validation clean. See
`docs/milestones/M5-acceptance-report-2026-08-13.md`.
M5.12 hardens cooked HDRI reflection fidelity without changing the accepted RHI or
product schema. Importer/cooker v3 removes the editor sample clamp, adds a true
radiance mip pyramid and PDF-aware deterministic GGX filtering, and makes Ultra
1024-face reflections the high-end import default. The Asset Browser exposes
Iteration/High/Ultra/Cinematic and independent custom controls with memory estimates
and explicit upgrade/reimport. One bounded oversized atomic HDRI publication may
cross the unchanged 128 MiB general asset budget, subject to a 640 MiB environment
cap. A repeated Ultra cook is byte-deterministic, Debug/Release remain 69/69, a
validation-enabled car run is clean, and a matched native-4K checkpoint measures an
exact 108 MiB residency increase with no median frame-time regression. See
`docs/performance/M5.12-reflection-resolution-stabilization-2026-08-13.md`.
M5.13 aligns the light gizmo and renderer on local `+Z` emission, increases the
high-end directional/spot density, replaces scale-dependent receiver displacement
with world-shadow-texel bias, and reconstructs hard/contact edges without exposing
one nearest raw depth texel. See
`docs/performance/M5.13-shadow-direction-and-quality-hardening-2026-08-13.md`.

Dependencies: M1, M2; asset integration from M3 as available.

Deliverables:

- ECS lights uploaded to GPU light buffers.
- One clustered light-assignment representation shared by deferred/material-resolve
  and forward paths.
- Physically consistent direct lighting and image-based lighting with irradiance, prefiltered specular environment, and BRDF integration.
- Directional, spot, and point shadow-map foundations with an explicit quality/caching policy.
- Independent per-light shadow visibility, user-configurable high-resolution tiers,
  and the first physically driven contact-hardening/soft-shadow quality slice.
- Stable three-mode Sky component (`Skybox`, `Hdri`, `Simulated`), with cooked HDRI
  assignment/background/IBL implemented first and mode-specific future settings.
- Reflection/environment probe system and cubemap capture/cooking.
- Interfaces for lightmaps, irradiance volumes/probes, and other non-RT baked GI data.
- Backend-neutral contracts and scalability controls for later virtual shadow maps,
  translucent colored shadows, contact shadows, and high-quality AO.

Acceptance gate: no hardcoded demonstration light is required; deferred/material-resolved and forward materials consume the same cluster/light records and agree under direct lights and IBL; shadow and probe costs are measured at 4K.

### M6 - Hybrid transparency

Status: `Proposed`

Lead handover: `docs/milestones/M6-hybrid-transparency-handover-2026-08-13.md`
records the post-M5.12/M5.13 renderer state, temporary glass limitations, inherited
lighting/shadow/reflection contracts, resize risks, and recommended qualification
order. Read it before producing the M6 execution plan.

Dependencies: M1, M2, M3, M5.

Deliverables:

- Per-transparent-primitive bounds and deterministic depth/priority keys.
- `AlphaClip`, `SortedSurface`, `ThinGlass`, `LayeredGlass`, and weighted-OIT material modes.
- Correct front/back or thickness data for thin closed glass.
- Two-layer baseline and scalable additional peels for nested hero glass.
- Rough refraction using the HDR scene-color pyramid and defined off-screen fallbacks.
- Tile/bounds restriction, early termination, and quality-tier controls.

Acceptance gate: the sample car windows and headlights render predictably, normal/roughness detail remains visible, nested reference scenes are stable, and costs fit the transparency budget.

### M7 - GPU scene and indirect visibility

Status: `Proposed`

Dependencies: stable M2/M3/M4 identities, M5 clustered-light records, and M0 profiling.

Deliverables:

- Persistent GPU buffers for instances, current/previous transforms, geometry, materials, bounds, and visibility metadata.
- Compact CPU update streams rather than repeated full draw packets.
- Compute frustum/LOD culling, visibility compaction, and indexed indirect-count submission.
- Hi-Z occlusion path with temporal conservatism and diagnostics.
- Shared visible-instance representation for raster, shadows, and future RT updates.
- Sparse virtual-shadow page tables, GPU page marking/caster culling, physical-page
  pools, cache invalidation/age diagnostics, directional clip levels, and local-light
  residency, retained only if they beat conventional maps in matched 4K tests.
- Progressive, budgeted publication of independently resident texture products with
  semantic fallback views instead of monolithic full-model texture admission.
- Stable instance/primitive/material identities and reconstructable triangle data for
  the ADR-0006 visibility payload.
- Indexed visibility-buffer experiment for standard opaque surfaces, including
  derivative-correct material resolve into the M2 canonical packed surface cache.
- Matched conventional packed-deferred versus visibility-resolved 4K image, timing,
  bandwidth, memory, and material-coherence evidence.

Acceptance gate: CPU render preparation and submission scale primarily with changed data rather than total draws; the indexed visibility path resolves correct surface/motion data and demonstrates a representative-scene benefit before becoming production; classic indexed packed-deferred remains available as a fallback.

### M8 - Meshlet cooker and mesh-shader path

Status: `Proposed`

Dependencies: M3, M7.

Deliverables:

- Offline meshlet construction with tunable vendor-neutral limits, local indices, bounds, and normal cones.
- Mesh-shader capability detection and pipeline path using `VK_EXT_mesh_shader`.
- Meshlet culling and indirect mesh-task dispatch consuming the M7 visibility architecture.
- Meshlet-driven shadow-caster submission feeding the same conventional/virtual
  visibility ownership and cached page requests as the indexed path.
- Visibility-buffer emission consumes the same payload/material-resolve contract as
  the indexed M7 path; no mesh-shader-only scene representation is introduced.
- Benchmarks against indexed indirect rendering for both packed-deferred and
  visibility-buffer workloads; select the faster path per workload/capability.

Acceptance gate: mesh shaders produce matching images and visibility identities and a measured benefit on appropriate high-geometry scenes without becoming a mandatory regression for small meshes.

### M9 - Temporal rendering and reconstruction

Status: `Proposed`

Dependencies: M1, M2, current/previous data from M7.

Deliverables:

- Stable jitter, current/previous matrices, skinned motion, depth, exposure, reactive data, and history invalidation.
- Native TAA/DLAA-quality reference path.
- Vendor-neutral super-resolution interface and Vulkan SDK/plugin requirement negotiation.
- DLSS integration first, with room for FSR/XeSS providers.
- Dynamic-resolution policy and objective ghosting/disocclusion tests.
- Temporal accumulation, disocclusion rejection, and denoiser inputs for stochastic
  soft shadows, screen-space contact shadows, GTAO, and later hybrid visibility.

Acceptance gate: high-quality reconstruction is stable in motion, transparencies provide appropriate reactive behavior, and displayed versus base-render frame rates are reported separately.

### M10 - Non-ray-traced GI production paths

Status: `Proposed`

Dependencies: M3, M5, M9 as appropriate.

Deliverables:

- Production-ready selection of baked lightmaps, irradiance probes/volumes, screen-space techniques, and probe updates.
- GTAO-class high-quality horizon AO with depth/normal pyramids, optional bent
  normals, temporal/spatial filtering, multi-bounce compensation, and bounded
  diffuse/specular occlusion; measure FidelityFX CACAO as a Vulkan fallback.
- Bounded screen-space contact shadows as a diagnosed complement, never a replacement
  for independent per-light shadow ownership.
- Raster RGB/optical-depth transmittance shadows for M6 colored translucent closures,
  with explicit layer, memory, and update budgets and no implied caustics.
- Production Skybox mode and physically based Simulated sky, atmosphere, sun disk,
  aerial perspective, and cloud-lighting integration using the M5 Sky contract.
- Streaming, invalidation, and authoring workflows.
- Project-wide and per-volume quality profiles covering AO method/resolution,
  shadow method/resolution/page pools, samples/rays, owner/update budgets, contact
  detail, colored transmission, denoising, and VRAM limits.
- Quality/performance comparisons and fallbacks for dynamic objects.

Acceptance gate: fully dressed scenes have a credible non-RT indirect-lighting solution within the 4K frame budget.

### M11 - Hybrid ray tracing and reference path tracing

Status: `Proposed`

Dependencies: M2, M3, M5, M7, M9; baseline raster and non-RT solutions accepted.

Deliverables:

- RHI and Vulkan acceleration-structure/resource support.
- BLAS cooking/build policy and TLAS update system using stable GPU-scene instances.
- Progressive hybrid effects: physically sized area/contact-hardening shadows, RTAO,
  reflections, GI, colored transmission, and participating-media visibility as
  justified, sharing raster light/material semantics and quality controls.
- Denoiser integration using correct motion/depth/normal/material demodulation inputs.
- Reference/photo-mode path tracer for visual validation.

Acceptance gate: RT features are optional capabilities, share material/light/scene data with raster, and have measured quality and performance fallbacks.

## Program controls

M5 post-acceptance hardening is tracked in M5.12 (reflection resolution) and M5.13
(shadow direction/quality). M5.13 establishes local `+Z` emission, 4096 directional
maps, an 8192 spot atlas with 4096 Ultra tiles, denser PCSS, and an antialiased
point-like hard-shadow limit. This conventional-map baseline is inherited by M7
virtual shadows and M9 temporal filtering; it does not replace those successors.

- Do not start a milestone before its dependencies and acceptance criteria are understood.
- Every milestone begins with a checked-in execution plan following `PLANS.md`.
- Architectural changes require an ADR or explicit update to an existing proposed ADR.
- Every milestone ends with a review, verification report, and updated baseline.
- New features discovered during implementation enter the roadmap explicitly; they must not disappear into chat-only follow-up notes.
