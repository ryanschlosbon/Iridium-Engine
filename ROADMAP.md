# Iridium Engine Development Roadmap

## Purpose

This is the durable program-level roadmap for the engine after the RHI refactor. It records milestone order, dependencies, acceptance gates, and work intentionally deferred. Detailed execution plans follow `PLANS.md` and live under `docs/milestones/`.

Status values: `Proposed`, `Ready`, `In Progress`, `Blocked`, `Accepted`.

## Product and performance goals

- High-fidelity, future-facing Vulkan renderer for high-end PCs.
- Raster (no ray tracing): 144 FPS at native 3840x2160 HDR on the reference system in fully dressed gameplay scenes (6.94 ms base-render budget), with native temporal AA. Owner decision 2026-10-02; previously >100 FPS / 10 ms.
- With hybrid ray tracing: 144 FPS displayed, using temporal reconstruction (DLSS-class or native TAAU) where appropriate.
- Visual fidelity comparable to Unreal Engine 5, Frostbite, Anvil, and Northlight is the primary quality bar.
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

Status: `Complete`

Lead handover: `docs/milestones/M6-hybrid-transparency-handover-2026-08-13.md`
records the post-M5.12/M5.13 renderer state, temporary glass limitations, inherited
lighting/shadow/reflection contracts, resize risks, and recommended qualification
order. The completed execution plan is
`docs/milestones/M6-hybrid-transparency.md`.
M6.0-M6.10 are complete. The dated acceptance report is
`docs/milestones/M6-acceptance-report-2026-08-27.md`. M6.6's CPU stack-reduction and
deterministic per-tier atlas-preparation contracts are complete. GPU tier storage
topology is complete. Same-tier projected overlap now forms deterministic shared
optical islands, and the indexed peel contract supports nested cross-work interface
sequences while preserving Ordinary2's paired behavior. Deep capture/composition
frame targets, capture framebuffers, and previous-interface descriptor chains are
conditionally materialized with topology rollback. Stable bounded draw plans and
sequential 4/8 capture execution are active for explicitly authored deep tiers. Deep
local composition now rerasterizes exact captured entry slots deepest-to-nearest,
matches later same-work exits, and evaluates the shared measured-chord material path
into premultiplied AP1 tier atlases. Hero4 and Cinematic8 scene resolve are active with
interface-zero stable-identity ownership, and accepted deep packets no longer draw
through compatibility forward. A deterministic two-shell Hero4 fixture proves 10,922
paired pixels and 3,890 four-interface pixels. A four-shell Cinematic8 fixture proves
15,042 paired pixels and reaches all eight interfaces at 1,636 pixels. Both produce the
exact expected resolve draws, zero compatibility draws, and zero validation errors;
lit Belfast captures are visibly correct. Mixed Hero4/Cinematic8 content uses one
global-order scene-resolve pass with tier descriptor switches. These short Debug
readback runs are semantic rather than performance evidence. The bounded residual-tail
operator is also active without an additional graph resource or pass. A five-shell
Cinematic8 fixture requests ten interfaces, stores eight, and proves 650 saturated
prefix pixels and 1,300 estimated residual samples per measured frame versus zero in
the four-shell control, with zero fallback, semantic, or Vulkan errors. Its Belfast
capture remains finite and legible. A two-shell Hero4 crossing fixture additionally
proves 2,705 non-LIFO `Entry(A), Entry(B), Exit(A), Exit(B)` pixels within 14,211
valid paired/local-color pixels, with exact two-draw resolve and no fallback or
validation errors. Per-tile early termination is also active at the accepted 1/1024
threshold: a Q14 conservative transport/open-stack state reuses deep R32 identity
records, useful odd non-final interfaces reduce 16x16 masks, and later peels reject
terminated tiles before material/texture evaluation. A two-shell separated Hero4
fixture proves 7,522 suppressed deeper-interface pixels across 43 occupied tiles with
zero fallback or validation errors. Hero4 and Cinematic8 each pass two real
120-frame retire/reactivate cycles and a final GPU readback. Five native-4K
Cinematic8 Release processes cover 50,000 measured frames at 1.680448 ms GPU-frame
median-of-medians and 1.276928 ms complete-transparency median, with identical graph
and live memory and zero actual compatibility draws, rejects, topology events,
profiler errors, or dropped frames. M6.6 is complete.
M6.7 now freezes the backend-neutral WeightedOIT numerical contract and conditionally
materializes its Vulkan graph storage: explicit nonrefractive work uses premultiplied
AP1 input, bounded depth/coverage weights, a documented FP16 radiance/overdraw
envelope, weighted average plus revealage resolve, and exactly 82,944,000 logical
native-4K bytes per frame context with no owned depth. An opt-in test topology owns
one `RGBA16F` accumulation image and one `R16F` revealage image per frame context,
reads opaque depth without writing it, and orders active accumulation/resolve passes
after foreground transparency and before bloom. Frame targets acquire both products,
and the live backend now makes them conditionally resident through startup prewarm
or next-frame demand, delayed 120-inactive-frame retirement, and rollback-safe graph
rebuilds. Explicit WeightedOIT work bypasses refraction pyramids and the legacy glass
buckets. Once resident it now uses a fixed cull-none, opaque-depth-tested accumulation
pipeline with additive weighted color, multiplicative revealage, and a premultiplied
fullscreen resolve into scene HDR. The first dynamic-demand frame retains deterministic
SortedSurface fallback while the topology is enabled for the next frame.
This intentionally changes only explicitly classified WeightedOIT content; default
scene rendering and default graph memory remain unchanged. A tracked cooked
`weighted_oit_particles_v1` fixture now exercises 256 explicit emissive-16 particles
in an 8x8x4 overlapping grid. Its corrected 1280x720 Debug run is Vulkan-clean and
records exactly 256 accumulation draws, one resolve, zero sorted/compatibility draws,
zero refraction or layered residency, and zero profiler drops. That run exposed an
alias-layout conflict in dormant refraction descriptors; the explicit nonrefractive
OIT shader now compiles refraction transport and bindings out instead of activating
the pyramids. A separate scene-linear PFM readback at emissive 16 has 921,600 finite
pixels, zero nonfinite pixels, 142,884 active pixels, and a 10.21875 maximum AP1
component, confirming finite pre-output HDR composition. A separate tracked
`weighted_oit_particles_hdr256_v1` boundary fixture cooks through importer v6 in
54 ms without diagnostics. Its Vulkan-clean scene-linear readback again has 921,600
finite pixels and zero nonfinite pixels, reaches 163.5 AP1, and has zero pixels at the
FP16 finite limit; the emissive-256 numerical gate is therefore closed. A
single-process 300-frame native-4K Release baseline measures 4.005 ms CPU, 3.185 ms
total GPU, 2.764 ms OIT accumulation, and 0.020 ms resolve medians with
the same exact routing counters. It sustains roughly 314 GPU frames/s on the reference
RTX 4090, but the 256 independent draws are deliberately not yet a batched particle
system, so this is populated-path cost evidence rather than final acceptance.
A backend-neutral qualification seed now drives an allocation-free coprime
permutation while seed zero preserves production order. Sixty-four independent
Vulkan-validation processes produce 64 distinct scene-linear hashes with identical
coverage and exact routing; worst order-dependent error is 0.046875 absolute AP1,
0.001718 RMSE, and 0.4587% relative, inside the frozen 0.0625/0.002/0.5% limits.
The 4096/65536-particle high-overdraw gate is now closed by a bounded
backend-neutral instance stream: each tier is one ECS entity, one packet, and one
Vulkan draw, with exact 256 KiB/4 MiB uploads and zero fallback. At 65,536, Debug
validation reduces extraction/sort from 567.681/389.941 ms to 0.261/0.0009 ms while
the finite capture stays inside the frozen order-error envelope. Populated resize
restore and separate SDR/scRGB/HDR10 processes are also Vulkan-clean with exact
routing and finite output. Five native-4K Release processes then complete 50,000
measured frames with a 3.182048 ms GPU-frame median-of-medians, exact one-draw OIT
routing, identical memory, and no profiler loss. M6.7 is complete.
M6.8 now has one backend-neutral source/resolved/topology/fallback diagnostic contract
shared by headless snapshots and artist panels, plus validated Transparency Class and
Transparency Fallback viewport/CLI views spanning opaque, layered, and WeightedOIT
execution. Cooked opaque primitives now default to a disabled compact summary with
their policy controls collapsed, while one explicit stable-GUID override reveals the
full artist workflow. The Profiler now groups every transparency counter, highlights
active bounded fallback/rejection/overflow evidence, and dynamically lists all active
M6 GPU ranges rather than relying only on its pre-M6 fixed range table. Measured
interval, selected pyramid mip, retained layer count, and overflow/saturation views
are now Vulkan-validated across Ordinary2, Cinematic8, and WeightedOIT. Their exact
pixel evidence agrees with independent layered readback, and the diagnostic paths add
no images, descriptors, or steady allocations. The inherited 17-call/5,216-byte
steady-frame baseline is now zero across 96 measured Release frames spanning
Ordinary2, Cinematic8, and WeightedOIT. Persistent frame scratch, sorted owner
validation, and static deep-pass identities close M6.8 without changing graph
resources, descriptors, or rendered output. Transparency/import settings now use a
backend-neutral asset transaction
history: apply, undo, and redo each enqueue a real recook, history advances only after
the matching catalog job succeeds, divergence is rejected, and scene history remains
untouched. A headless apply/undo/redo test preserves stable root and primitive GUIDs
while verifying the sidecar after every cook.

M6.9 completes the cutover by making classified hybrid execution the production default
for fresh schema-2 glTF imports and all uninitialized runtime transport records. The
frozen schema-1 migration and historical current-schema legacy values remain readable,
but serialized execution mode no longer selects the production renderer. Artist-facing
architecture selection is removed; `--developer-legacy-transparency` is the sole
explicit A/B override and is applied coherently while publishing runtime geometry,
material variants, shaders, passes, and fixed-function state. The same classified
Ordinary2 artifact is Vulkan-validation clean in both modes: production records one
entry, exit, local-composition, and scene-resolve draw with 5,888 paired pixels and no
compatibility draws, while the override records one legacy depth and forward draw and
no Ordinary2 work. Distinct final-SDR hashes prove the switch is real. The default
production graph now omits `depth.glass` and all four background/foreground bucket
passes, reducing its base contract from 22/27/20 to 19/26/19 passes/logical resources/
physical slots. On the active 1280x720 Ordinary2 control this removes exactly 7,372,800
requested and 7,864,320 committed bytes while preserving the prior capture byte for
byte. A rejected open-shell volume uses one classified compatibility-forward draw,
zero glass-depth draws, and zero legacy buckets. The legacy graph is materialized only
by the developer override. Final production qualification passes 71/71 tests in both
Debug and Release and is Vulkan-validation clean across Ordinary2, Cinematic8,
WeightedOIT, fallback, resize, lifecycle, selection, capture, and SDR/scRGB/HDR10.
Five native-4K Ordinary2 Release processes cover 50,000 measured frames at
0.720192 ms GPU median-of-medians and 0.403456 ms complete-transparency median with
identical memory/counters, zero fallback or legacy work, zero profiler loss, and zero
retained-frame C++ allocations. Separate Cinematic8 and WeightedOIT profiles measure
1.612576 and 2.657440 ms. The matched Alfa result is byte-identical to its accepted
pre-cutover capture and measures 5.820864 ms GPU versus 6.092096 ms at M6.0. M6 is
complete; retained raster limitations and future GPU-scene/RT work are explicit in
the acceptance report.

M6.10 closes the editor workflow around the accepted renderer. Viewport left-click
picking once again selects the nearest mesh entity. A default adaptive metric ground
grid, one world SI display unit, independent translation/rotation/scale snapping,
Ctrl fine-control bypass, and world/local gizmo space are available from the viewport
toolbar without modifying existing transforms. The Inspector adds component/property
search, world-unit position/range presentation, and uncapped nonnegative physical
light fields with lux/kilolux and candela/lumen views. Asset Browser folders support
physical nesting from both tree and content views plus persistent custom tree order;
model subassets use an attached horizontal thumbnail strip, and the Up affordance is
wider. A benchmark metadata snapshot no longer masquerades as a live asset sidecar,
eliminating the reported duplicate-GUID catalog records. The grid is now a
derivative-filtered procedural plane in the output transform rather than a finite CPU
line overlay. It has no geometry extent, fades toward grazing/horizon views, and
samples the opaque depth attachment so nearer scene geometry occludes it. The
backend-neutral RHI carries only editor plane/spacing state; benchmarks, asset
previews, and renderer captures disable the overlay. This adds one existing-depth read
to the output pass but no image or render pass, and leaves accepted M6 benchmark
images grid-free.
Viewport model drops now enforce the Y-up world ground plane (`Y = 0`) even when the
cursor ray is parallel to or aimed away from that plane. The first contextual
shortcut policy reserves Alt-prefixed chords for viewport presentation/tools and
adds **Alt+G** for grid visibility; it routes only through a focused viewport and is
suppressed while typing, editing a widget, using the gizmo, or displaying a popup.

M6.11 post-acceptance display hardening adds a live **Display transport** selector to
Project Settings. Auto prefers scRGB, then HDR10, then SDR; unsupported concrete modes
are identified from the active Vulkan surface. A frame-boundary RHI operation rebuilds
only the swapchain-dependent output, HDR encode, UI, graph-target, framebuffer, and
descriptor state while keeping scene assets resident. The ACES LUT follows the
effective rather than merely requested transport, including SDR fallback. The startup
CLI also accepts `auto`, and a single-process validation sequence exercises
scRGB -> HDR10 -> SDR. ADR-0013 supersedes ADR-0002's restart-only restriction.

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

Status: `In Progress` — **paused 2026-10-02 at the M7.8 checkpoint** for M7R
architecture consolidation and the M9 temporal/post pull-forward (see Program
schedule). M7.0-M7.7 accepted; M7.8 Virtual Shadow Map work is retained default-off
and resumes after M8 meshlets. M7.9-M7.12 resume after M9. Historical status:
M7.8 active; M7.0-M7.7 accepted after repaired lit
qualification. LOD cooking, opt-in device selection, and bounded generation-safe
main-view hysteresis are accepted as a workload-selectable route; default remains
LOD0. A
persisted transactional per-content maximum LOD ceiling provides the hero override.
Opt-in boundary-fan repair plus per-merge transactional
orientation/correspondence/topology-link validation increases the representative
artifact from 10 to 70 generated chains. The native-4K oblique sweep reduces
main-view triangles by 7.768/16.534/25.373% at 2/4/8 pixels; the 2-pixel image changes
896 of 8,294,400 pixels with mean luma SSIM 0.9999728463, while the loose 8-pixel
diagnostic falls below the 0.9999 admission line. Expanded multi-asset validation is
exact and the 240-frame motion/cut fixture shows a 45.7933% reduction in LOD changes
from 15% hysteresis with no mismatch or overflow. The first alternating five-process
native-4K near/mid/far set rejected production admission: 2.8282% fewer triangles cost
1.5206% more median wall time. Exact CPU LOD command reconstruction is now separated
into validation/explicit qualification mode, and invariant base-bounds projection is
reused across each GPU LOD chain. The final deployable-path set improves to effective
wall parity (+0.001183 ms / +0.0378%), but complete GPU median remains 0.5642% higher
and candidate tails are worse. A qualification-only physical residency floor now
compacts away fine index ranges and rebinds the stable canonical primitive to its
first retained coarse range. The matched floor-one gate withholds 1,737,714 bytes
across 70 chains, retains 303 exact commands with zero direct fallback/mismatch, and
reduces the forced base from 604,836 to 482,328 triangles; the complete image changes
0.143856% of pixels at 0.9999913623 mean luma SSIM. The frozen 256-instance stress
workload closes the remaining admission gate: 2-pixel LOD removes 12,650,344 of
51,612,672 main-view triangles (24.5102%) and improves median complete-frame wall
time by 0.310995 ms (1.4487%), CPU median by 0.4550 ms, and GPU median by
0.871328 ms. CPU/GPU p95 and p99 also improve, while memory remains identical and
validation/oracle selection stays exact. Four of five matched pairs favor the
candidate; one reversal is retained, so LOD0/floor zero remain the global default
and generated LOD is workload-selected rather than unconditional. Twelve eligible
opaque primitives containing 6,838 triangles still have no accepted level.
A geometry-policy edit also forced all 76 embedded textures to recook, directly
confirming the M7.9 child-CookKey granularity requirement.
Independent child streaming and shadow/probe LOD also remain open. Current evidence:
`docs/performance/M7.5-lod-history-2026-08-31.md` and
`docs/performance/M7.5-hero-lod-policy-2026-08-31.md`, plus
`docs/performance/M7.5-lit-lod-admission-fixture-2026-08-31.md` and
`docs/performance/M7.5-boundary-aware-lod-cooking-2026-09-01.md`, superseded by
`docs/performance/M7.5-transactional-lod-cooking-2026-09-01.md`, then
`docs/performance/M7.5-topology-transactional-lod-cooking-2026-09-01.md` and
`docs/performance/M7.5-topology-admission-2026-09-02.md`, followed by
`docs/performance/M7.5-lod-oracle-separation-2026-09-02.md` for the deployable-path
overhead repair and held production-admission decision, and
`docs/performance/M7.5-physical-coarser-residency-2026-09-02.md` for the physical
fallback seam, and
`docs/performance/M7.5-material-workload-admission-2026-09-02.md` for the final
workload-selectable admission. M7.9 still owns independently resident vertex/index
child products; M7.6 now owns the active Hi-Z slice. Its first accepted slice defines
a backend-neutral farthest-depth pyramid/reference oracle, preserves odd-resolution
edge coverage, bounds queries to at most four texels, and enumerates camera-cut,
motion, near-plane, residency, transform, stale-history, and resize fail-visible
reasons. The opt-in Vulkan live-depth build now passes synchronized execution,
4K image identity, exact live D32/all-mip CPU-oracle readback, and output-transport
rebuild checks. The build remains frame-context storage and now publishes
completion-safe, generation-stamped persistent histories for the
scene and asset-preview views. The live oracle verifies both the build and copied
history exactly. The temporary 4K build-plus-copy bridge measures 0.162816 ms median
and adds 87.3515625 MiB committed over build-only storage; it must be collapsed
before production admission. The new device query ABI/shader matches 3,640 CPU
oracle queries exactly across forward/reverse, D32/R32, odd/1D and native-4K cases,
and history identity now includes camera pose. The complete deferred-opaque plus
depth-writing-forward owner is computed before compaction. A second default-off
flag now runs live queue-ordered queries and fence-delayed validation without
removing draws: the first synchronized dense run tests 288 candidates per frame,
reports 34 `would_reject`, keeps 15 projection failures visible, and returns zero
invalid records. Queue-ordered and fence-published metadata remain separate,
preserving exact serial adjacency without misreporting CPU completion. A second
device-owned projection oracle now consumes the live GPU-scene tables and compares
every candidate against the CPU-projected safety route after the frame fence. The
32-frame overlap/cut, off-frustum, and output-transport rebuild runs report zero
invalid results and zero unsafe mismatches; extra pixel/depth margins intentionally
reduce marginal would-reject verdicts. A separately explicit rejection variant now
consumes only valid matching verdicts during opaque compaction. The frozen temporal
run removes 73 of 404 pre-cut commands and 42 of 404 post-cut commands, retains all
work when history is unavailable or cut, preserves current-build image bytes, and
has exact per-bin oracle parity. Off-frustum and transport-rebuild runs are exact.
The follow-up fuses projection/query/result emission with compaction, removes the
second dispatch and barrier, shares one shader implementation with the standalone
oracle, and selects a sampler-free base pipeline whenever history is invalid. The
same temporal/off-frustum/rebuild/image evidence remains exact. This is still not
production admission: fused local work is about 0.0025–0.0047 ms above base
compaction plus query in the current small fixture, and reverse-ordered native-4K
pairs remain contaminated by unrelated forward/UI and GPU-state variance. Selected
moving-occluder, moving-occludee, and disocclusion fixtures now reject history once
as `DepthContentChanged`, recover on the next frame, preserve byte-identical
query/rejection captures, and maintain exact device/oracle parity. A subpixel
fixture retains all 101 commands as exact `SmallBounds` fail-visible results, while
a visibility-step fixture rejects
history as `DepthContentChanged`, retains all remaining transition work, and recovers
on the next frame. Three scene-target resizes invalidate history, retain all 404
transition commands, recover exact device/oracle parity on the following frames,
and restore 1280x720. The build now writes directly into the persistent per-view
history after that view's earlier contents are sampled, removing two frame-local
chains, the full-mip copy, 9.374718 MiB requested at 1280x720, and 84.373894 MiB
requested at native 4K. The final controlled native-4K depth-stack gate removes
5,133 of 6,464 opaque commands, reduces median GBuffer time from 0.741376 ms to
0.081920 ms, complete GPU time from 2.133760 ms to 1.471840 ms, and median CPU
frame time from 5.455600 ms to 4.828700 ms while preserving byte-identical scene
captures. Qualification-only CPU projection, standalone queries, result readback,
and full-capacity result buffers are now absent from the deployable route. M7.6 is
accepted as workload-selectable; default allocation/rejection remain off while
M7.7 owns independent consumers and conventional-shadow optimization. Its first
accepted slice adds project/profile-owned directional coverage, retains engine-
derived practical/log cascade splits with exact world-units-per-texel diagnostics,
reconstructs PCSS filter comparisons bilinearly, and makes the Cinematic project
tier reachable without weakening lower per-light caps. Bias constants remain
unchanged globally; a second accepted slice adds bounded receiver-plane correction
and a half-texel geometric-normal offset. A matched grazing fixture removes severe
self-shadow striping with identical 0.125952 ms deferred-lighting median time.
A third accepted slice preserves camera-invisible caster eligibility while deriving
a conservative set per refreshed directional cascade. Its moving-grid qualification
rejects 162/192 caster-cascade pairs before resource lookup and draw recording; the
accepted grazing contact capture remains byte-identical. A follow-up replaces the
global caster cache key with per-cascade membership/content revisions; its
enter-volume transition refreshes two affected cascades rather than all four.
A fourth accepted slice replaces the application-owned copy of persistent caster
packets with camera-independent GPU-scene primitive references shared by
directional, spot, and point shadows. The 48-primitive qualification scene carries
192 bytes of reference payload instead of 11,520 bytes of copied packets, uses zero
fallback packets, and preserves the accepted contact image byte-for-byte. A fifth
accepted slice now builds directional-cascade commands on the device, consumes the
primitive index as indirect `firstInstance`, and retains direct mixed-submission,
capability, tiny-workload, and qualification-reference paths. In the 48-caster
qualification, all 30 device commands match the CPU oracle with zero overflow and
the paired direct/indirect captures are byte-identical. Spot and point device-built
commands now follow the same bounded ABI: 9 spot commands and 90 point-face
commands match their independent oracles with zero mismatch or overflow, and both
paired Release captures are byte-identical. Their retained single-frame timings
are diagnostics only. The following slice makes the CPU count oracle
qualification-only while retaining delayed exact device counts and overflow
checks; the matched 35 m directional capture stays byte-identical and production
records no GPU-owned caster/view tests. The next accepted architecture slice gives
reflection captures their own `GpuSceneConsumerProbe` submission instead of the
main-view queues. Nine primitives use 36 bytes of compact references rather than
2,160 bytes of packets; 18 owned primitive-face pairs are self-excluded, and the
remaining 36 tests produce 33 draws with no stale references. The direct/compact
captures are byte-identical. Compact capture shading now also resolves transform
and material identity through a dedicated GPU-scene graphics pipeline while direct
packets remain interleavable. An isolated 33-draw probe A/B is numerically
equivalent within one R16 quantization step. Reflection capture now owns bounded
device candidate/command/count storage, performs stable-owner exclusion and cube-
face tests in compute, and submits indirect-count graphics. All 33 commands match
the CPU oracle with zero mismatch/overflow; production performs no GPU-owned CPU
face tests and retains the preceding capture hash. Probe LOD and warmed
heterogeneous admission remain active M7.7 work. A face-invariant radial probe-LOD
checkpoint now independently selects resident, buffer-compatible geometry: 7 of
226 visible commands reduce the near/mid/far capture from 603,932 to 599,424
triangles with exact device/oracle command agreement and a byte-identical LOD0
scene capture. The later reflection-sensitive warmed gate uses a standalone
1024-pixel realtime probe over moving glossy high-detail content. A 16-pixel bound
removes only 0.9263% of retained capture triangles and preserves a byte-identical
native-4K final-SDR capture, but reversed-order Release pairs show no repeatable
performance or memory win. The gate is complete without production admission:
probe LOD remains experimental/default-off and LOD0 remains global. Directional
shadow-map LOD now independently evaluates cooked geometry
against each cascade's measured world-units-per-texel. A 2-texel correctness run
reduces 21 of 1,243 commands and 13,524 shadow triangles with exact device/oracle
agreement and a byte-identical LOD0 scene capture. The deployable route performs no
CPU caster visibility or shadow-LOD oracle work. The later native-4K warmed gate
rejects production admission, so LOD0 remains default. Point-cube commands
now use the same shadow-texel policy with a face-invariant radial projection bound.
The 512-pixel dense-car checkpoint reduces 16 of 565 commands and 11,436 triangles
with exact command agreement, zero overflow, and a maximum scene-linear difference
of `5.96046448e-7`. Production again records no CPU visibility/LOD oracle work.
Spotlight commands now use their authored clip projection and atlas-tile resolution
for the conservative error bound. The dense-car checkpoint reduces 10 of 339
commands and 6,376 triangles with exact command agreement, zero overflow, and a
byte-identical scene-linear LOD0 capture. Production records no CPU spotlight
visibility/LOD work. A native-4K moving 96-primitive heterogeneous gate now admits
directional/spot/point device-command submission: two reversed-order 600-frame
pairs reduce combined median CPU shadow recording by 70.1-70.9%, slightly reduce
combined GPU shadow work, and preserve a byte-identical final-SDR capture. The
isolated direct-shadow reference leaves the main GBuffer automatic. Shadow LOD
remains default-off pending its separate visual threshold gate.
See `docs/performance/M7.6-live-depth-build-2026-09-09.md`,
`docs/performance/M7.6-view-history-publication-2026-09-10.md`, and
`docs/performance/M7.6-device-query-parity-2026-09-10.md`,
`docs/performance/M7.6-live-query-qualification-2026-09-10.md`, and
`docs/performance/M7.6-device-owned-projection-qualification-2026-09-11.md`, and
`docs/performance/M7.6-experimental-command-rejection-2026-09-11.md`, followed by
`docs/performance/M7.6-fused-query-compaction-2026-09-11.md`,
`docs/performance/M7.6-moving-occluder-disocclusion-2026-09-11.md`,
`docs/performance/M7.6-small-object-depth-content-2026-09-11.md` and
`docs/performance/M7.6-resize-recovery-2026-09-11.md`, followed by
`docs/performance/M7.6-in-place-view-history-2026-09-11.md`, and
`docs/performance/M7.6-large-occluded-admission-2026-09-12.md`. The active shadow
slice is recorded in
`docs/performance/M7.7-directional-shadow-density-and-filter-reconstruction-2026-09-12.md`;
receiver-bias evidence is in
`docs/performance/M7.7-directional-shadow-receiver-bias-2026-09-12.md`; directional
caster-visibility evidence is in
`docs/performance/M7.7-directional-shadow-caster-visibility-2026-09-12.md`; GPU-scene
shadow-submission evidence is in
`docs/performance/M7.7-gpu-scene-shadow-submission-2026-09-13.md`; directional
device-command evidence is in
`docs/performance/M7.7-directional-shadow-device-commands-2026-09-13.md`; local
device-command evidence is in
`docs/performance/M7.7-local-shadow-device-commands-2026-09-13.md`.
Oracle-gating evidence is in
`docs/performance/M7.7-shadow-command-oracle-gating-2026-09-13.md`. Independent
probe-submission evidence is in
`docs/performance/M7.7-independent-probe-visibility-2026-09-13.md`. Probe and
directional LOD evidence is in
`docs/performance/M7.7-probe-radial-lod-2026-09-14.md` and
`docs/performance/M7.7-directional-shadow-lod-2026-09-14.md`, followed by
`docs/performance/M7.7-point-shadow-radial-lod-2026-09-14.md` and
`docs/performance/M7.7-spot-shadow-projected-lod-2026-09-14.md`.
Heterogeneous command admission is recorded in
`docs/performance/M7.7-heterogeneous-shadow-admission-2026-09-19.md`.
The completed probe-LOD decision is recorded in
`docs/performance/M7.7-probe-lod-warmed-admission-2026-09-24.md`.
The conventional shadow-LOD gate is also complete without promotion. A native-4K
8-texel moving dense-geometry fixture removes 1.2684% of combined shadow triangles
and saves only 0.007-0.011 ms of shadow GPU work; CPU shadow recording regresses
and reversed-order whole-frame timing does not repeat. LOD0 remains the production
default. See
`docs/performance/M7.7-shadow-lod-warmed-admission-2026-09-29.md`.
GPU-scene publication now also owns stable camera-independent shadow/probe consumer
lists. Application extraction no longer rescans packed primitives per consumer,
and directional, spot, and point Vulkan paths retain geometry/material bins and
candidate layouts across transform-only frames while continuing device-owned
per-light visibility/LOD compaction. A Release moving-fixture smoke reports cache
hits for all three paths on every measured frame with zero fallback. See
`docs/performance/M7.7-shadow-membership-cache-2026-09-29.md`.
Opaque conventional-shadow pipelines now fetch only position for both direct and
GPU-scene submission; alpha-masked casters retain color and both UV sets. Reversed-
order native-4K pairs reduce combined directional/spot/point shadow raster time by
0.082-0.088 ms (about 9%) with byte-identical output and no additional geometry
memory. A duplicated compact position buffer is not admitted. See
`docs/performance/M7.7-opaque-shadow-position-fetch-2026-09-29.md`.
The final moving-camera contact matrix closes M7.7: thin, fully clipped alpha-mask,
double-sided, grazing, and close-contact cases retain exact device/oracle agreement
with zero fallback, while direct and automatic native-4K captures match byte for
byte at both cascade-density sweep extremes. See
`docs/performance/M7.7-shadow-quality-closure-2026-09-29.md`. M7.8 is active for
the sparse Virtual Shadow Map production candidate; conventional maps remain the
selectable reference and fallback.
M7.8's first architecture checkpoint now freezes stable virtual-page identity,
separate static/dynamic validity, bounded priority allocation, deterministic
age/priority eviction, pending-raster publication, and safe missing-page fallback.
The second checkpoint adds a default-off Vulkan physical atlas and flat page-table
allocation with device-limit validation and separate persistent memory accounting.
An opt-in Debug validation launch now measures an exact 88.753 MiB engine-owned
total: the 72.500 MiB pool/table plus two aligned 8,521,248-byte frame-owned compute
working sets. The disabled run reports zero bytes in all three categories; no shader
or lighting path consumes these resources yet. The backend-neutral
directional receiver oracle now selects the finest containing clip level, falls
back across a guarded clip edge, bounds/deduplicates requests, and retains signed
world-page identity across snapped camera scrolling. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`.
The matching raw Vulkan compute pass now has Debug/Release RTX 4090 parity against
that oracle for finest/guarded selection, invalid/outside receivers, ABI output, and
snapped-scroll identity. A separate deterministic GPU reference compactor now also
matches the CPU oracle for deduplication, priority/coverage/address ordering,
bounded overflow, dropped coverage, saturation, and malformed-mark rejection in
Debug and Release. It remains an explicit single-invocation qualification path;
scalable parallel compaction and virtual sampling are not enabled in runtime frames.
The first multi-workgroup candidate is now measured and rejected. It preserves the
oracle exactly at 4,096 receivers and 65,536 unique marks without cross-workgroup
spin waits, but two global bitonic networks require 272 compare/exchange dispatches
and cost 841.992 ms Debug and 837.758-844.757 ms across repeated Release medians on
the RTX 4090. The qualification
shader remains outside runtime; the next candidate must use hierarchical/radix or
bounded top-K ordering with dramatically fewer global dispatches.
That replacement is now qualified. Shared-memory 256-entry block sorts plus eight
global merges per full-capacity ordering preserve exact 4,096-receiver and
65,536-unique-mark parity. A two-stage saturated coverage reduction removes the
rejected atomic bottleneck. Device-local compute measures 0.145792/0.145984 ms
Debug/Release median on the RTX 4090 at 65,536 marks and a 4,096-request output cap.
The candidate now owns persistent storage, immutable per-frame descriptor sets,
and both compute pipelines behind the same default-off switch. The shared direct
marker-to-compactor chain matches its oracle through populated/empty reuse in
Debug/Release with synchronization validation, measuring 0.148512/0.147264 ms at
65,536 receivers. Live depth-driven receiver/clip publication and native-4K whole-
frame admission remain next. See
`docs/performance/M7.8-persistent-mark-compact-chain-2026-09-30.md`.

The following depth-producer checkpoint reconstructs every pixel in an explicit
bounded source region from sampled D32 depth and chains it directly into marking
and compaction. Odd offset regions, forward/reverse depth, invalid values and
perspective/singular cameras match the CPU oracle under synchronization validation.
The 4,095-pixel chain measures 0.067904/0.067872 ms Debug/Release on the RTX 4090.
Full-view demand accumulation and live clip/render-graph publication remain open;
full 4K cannot silently be subsampled to the current 65,536-receiver cap. See
`docs/performance/M7.8-depth-receiver-producer-2026-09-30.md`.

The full-view continuation now accumulates every depth pixel directly into bounded
per-clip page cells and compacts once, without subsampling or added tracked GPU
buffer/image storage. Exact odd two-clip and native-4K request/overflow parity passes
under synchronization validation; the 4K chain measures 0.569824/0.569504 ms
Debug/Release on the RTX 4090. Stable live clips, render-graph scheduling, integrated
frame admission, page raster and sampling remain open. Conventional shadows stay
active and M7.8 remains in progress. See
`docs/performance/M7.8-full-view-page-marking-2026-09-30.md`.

The camera-driven clip prerequisite now constructs a fixed-capacity directional
stack with doubling world spans, page-snapped signed origins and per-level density
diagnostics. CPU/GPU checks preserve overlap identity through positive/negative
scroll and exact guard fallback. Fixed light-space depth avoids camera-driven depth
cache invalidation; changes to projection policy require a new explicit revision.
The four-level builder costs 0.186163 us Release with no heap allocation. Actual
scene/light revision ownership, conservative depth bounds, per-slot uploads and
render-graph dispatch remain the next live integration gate. See
`docs/performance/M7.8-camera-driven-directional-clips-2026-09-30.md`.

The live CPU publication continuation now derives the first selected directional
light's clip packet from the actual view and caster bounds under the experimental
resource switch, including conventional cache-hit frames. A backend-neutral
publisher retains conservative quantized depth envelopes, advances projection
revisions on expansion/light policy changes, and suppresses unknown-bound packets.
Both layers conservatively use the complete caster revision. Per-slot GPU uploads
and external-buffer graph hazard tracking remain open before live compute dispatch.
See `docs/performance/M7.8-live-clip-publication-2026-09-30.md`.

The frame-slot upload continuation adds non-owning imported-buffer graph bindings
with retirement-scoped rebinding, distinct-slot handles and persistent write-hazard
tracking. The experimental graph uploads 1,536 checked packed clip bytes from an
immutable slot snapshot into the existing working buffer each frame. Both slots
pass the twelve-frame validation smoke with unchanged 88.753 MiB GPU storage;
standalone GPU oracle parity survives the new command-embedded upload. Live sampled
depth accumulation, integrated per-slot request comparison and native-4K admission
remain next. See `docs/performance/M7.8-frame-slot-clip-uploads-2026-09-30.md`.


M7.8 live scene-depth demand is now recorded after opaque forward rendering, with
fence-retired request checks and an explicit full-depth CPU oracle. Native-4K
moving-camera and complex-forward coverage match exactly; empty packets and three
positive-extent resizes are validation-clean. Matched captures remain byte-identical.
Five paired runs show about 0.42 ms demand cost and increased complete-frame p99;
this is qualification overhead, not VSM admission. Request readbacks add 262,208
bytes; full-depth buffers are separately opt-in. GPU residency, page raster and
sampling remain next; M7.8 stays active and M7.9-M7.12 remain unfinished. See
`docs/performance/M7.8-live-depth-page-demand-2026-09-30.md`.

The 2026-10-01 raster prerequisite now shares checked physical-tile layout between
RHI and Vulkan and defines pending-page crops, rasterized borders/interior UVs,
strict identity/requested-layer revision rejection and conservative caster volumes.
CPU tests and independent GPU point projection cover border/depth/scroll behavior;
the 4K reference capture and allocation totals remain unchanged. GPU residency,
triangle page raster and sampling remain unfinished. See
`docs/performance/M7.8-page-raster-regions-2026-10-01.md`.

Residency ABI v1 now defines lossless 48-byte full-identity keys and 96-byte
resident records, preserving signed 64-bit coordinates/revisions/frame ages and
independent layer validity. The retired-slot collector uses checked request-to-
identity conversion and its native-4K CPU oracle compares full identities/revisions.
This is metadata qualification only: no GPU allocator/storage or new sampling is
enabled. M7.8 stays active. See
`docs/performance/M7.8-residency-identity-2026-10-01.md`.

A serial GPU residency reference now reproduces full-key CPU allocation, cache
hits, independent layer invalidation, protected eviction, overflow and fallback
semantics. Fourteen oracle dispatches and five atomic rejection cases pass under
Vulkan validation in Debug/Release; checked 80-byte requests, 96-byte mappings
and 48-byte telemetry freeze its boundary. The bounded 256-request reference
measures 10.5083/10.8429 ms GPU median and is explicitly rejected for live
production integration. It is private test infrastructure, not page raster,
completion publication or sampling. Engine allocations and conventional image
production remain unchanged. M7.8 stays active: scalable GPU residency, indirect
page raster, shared sampling/local lights and full-path qualification are still
required. See
`docs/performance/M7.8-gpu-residency-reference-2026-10-01.md`.


Entry hand-off: `docs/milestones/M6-to-M7-handoff-2026-08-28.md` summarizes the
completed M6 architecture, post-acceptance editor/display work, frozen evidence,
inherited contracts, known limitations, and the required three-high-fidelity-asset
baseline before M7 implementation begins.

Dependencies: stable M2/M3/M4 identities, M5 clustered-light records, and M0 profiling.

Deliverables:

- Persistent GPU buffers for instances, current/previous transforms, geometry, materials, bounds, and visibility metadata.
- Compact CPU update streams rather than repeated full draw packets.
- Explicit static, movable, and animated instance update policies. Unchanged static
  instances retain GPU records and shadow-cache identity; movable/animated instances
  upload only changed current/previous data. Mobility is authored or derived by a
  documented policy, never silently guessed from a short period without motion.
- Conservative instance and primitive frustum culling, screen-space-error LOD
  selection with hysteresis, visibility compaction, and indexed indirect-count
  submission. Cooked or authored LOD chains retain stable primitive/material
  identity and expose quality overrides for hero content.
- Hi-Z occlusion culling with temporal conservatism, camera-cut/disocclusion
  handling, small-object safeguards, and requested/visible/rejected diagnostics.
- Separate conservative visibility for the main view, conventional/virtual shadow
  views, and reflection capture views; an object invisible to the camera is not
  incorrectly removed from a light or probe view.
- Before virtual-shadow promotion, harden the conventional reference with project-
  owned directional coverage, engine-derived stable cascade density, bilinear PCSS
  filter reconstruction, receiver-plane/geometric bias, and reachable Cinematic
  quality; retain conventional and virtual methods as project-selectable policies.
- Draw/dispatch construction that batches compatible geometry/material work and
  scales with visible work rather than issuing one CPU `vkCmdDrawIndexed` per source
  submesh.
- Shared visible-instance representation for raster, shadows, and future RT updates.
- Sparse virtual-shadow page tables, GPU page marking/caster culling, physical-page
  pools, cache invalidation/age diagnostics, directional clip levels, and local-light
  residency, retained only if they beat conventional maps in matched 4K tests.
- Progressive, budgeted publication of independently resident model products:
  texture views, geometry/LOD sections, and later meshlet/RT sections use semantic
  fallback textures and a bounds/proxy or coarser-LOD fallback instead of requiring
  one monolithic full-model upload. Publication is revision-safe, cancelable,
  prioritized by visible demand, and retains the last complete usable revision.
- Fine-grained geometry cooking and DDC reuse. Each source primitive and its
  dependent geometry, LOD, meshlet, and RT products receive deterministic child
  recipes/CookKeys; a material-only edit reuses geometry and texture products, and a
  one-primitive edit rebuilds only that primitive and its dependent children before
  a deterministic parent-manifest update.
- A bounded cook job graph for independent texture and geometry products with
  deterministic output slots, memory/worker limits, cancellation, editor priority,
  and stage/worker/cache-hit telemetry. Parallelism is selected from measured work;
  parsing, geometry, or serialization are not parallelized merely to increase thread
  count.
- Stable instance/primitive/material identities and reconstructable triangle data for
  the ADR-0006 visibility payload.
- Indexed visibility-buffer experiment for standard opaque surfaces, including
  derivative-correct material resolve into the M2 canonical packed surface cache.
- Matched conventional packed-deferred versus visibility-resolved 4K image, timing,
  bandwidth, memory, and material-coherence evidence.

Acceptance gate: CPU render preparation and submission scale primarily with changed
data and visible batches rather than total source draws. A fixed native-4K scene with
three high-fidelity assets records presentation wait separately from CPU and GPU
work, covers static/moving/off-screen/occluded/LOD cases, and demonstrates a measured
frame-time and submission reduction without visible popping or missing shadow/probe
casters. Material-only and one-primitive recooks prove unaffected child-product
cache hits, deterministic parent bytes, bounded peak memory, and responsive
cancel/reimport behavior. Oversized models become visible progressively without a
single upload hitch or incomplete-revision corruption. The indexed visibility path
resolves correct surface/motion data and demonstrates a representative-scene benefit
before becoming production; classic indexed packed-deferred remains available as a
fallback.

### M7R - Architecture consolidation

Status: `Accepted` 2026-10-04 (execution plan
`docs/milestones/M7R-architecture-consolidation.md`; completion report there; R0-R6
accepted 2026-10-02/04; M9 hand-off `docs/milestones/M7R-to-M9-handoff.md`; lead prompt `docs/milestones/M7R-task-lead-prompt.md`)

Dependencies: accepted M0-M7.7 and the committed M7.8 checkpoint.

The takeover audit of 2026-10-02 found that feature work had outpaced structure:
`VulkanVertexBackend.cpp` is 13.3k lines with about 385 members, `Application.cpp`
6.7k lines, qualification/oracle code is interleaved with production paths (about
20-30% of both), render-graph passes are declared but executed through imperative
string-matched calls, Vulkan 1.3 features (synchronization2, dynamic rendering,
timeline semaphores) are unused, memory is one `vkAllocateMemory` per resource with no
transient aliasing or pipeline cache, structural changes drain the GPU, the frame
loop is single-threaded, and the build has no module libraries or precompiled
headers. M7R fixes these without changing rendered output so M9-M11 can add heavy
techniques inside the 6.94 ms raster budget.

Deliverables:

- Per-module CMake libraries, tests linking libraries rather than recompiling
  sources, precompiled headers, and measured build-time improvement.
- Qualification harness separated from production (backend, application, and
  `IRenderBackend` test hooks), data-driven CLI registration, and behavioral tests
  replacing source-text assertions.
- Render-graph-driven execution: passes register execute callbacks, resources and
  passes are index-addressed, barriers are batched with synchronization2, and
  rendering uses dynamic rendering.
- Backend decomposition into pass/feature owners, including one shared indirect
  view culler for main/shadow/probe consumers.
- Vulkan memory modernization: suballocation (VMA), aliased transient graph memory,
  fence-keyed deferred deletion instead of all-frame waits, a persisted pipeline
  cache, and asynchronous uploads on a transfer queue with timeline semaphores.
- Application decomposition (frame orchestration, render extraction, editor host,
  asset integration), an engine task system replacing per-service threads,
  parallel/change-driven extraction, and retirement of the M7.2 parity packet path.

Acceptance gate: behavior-preserving evidence tier throughout (byte-identical
captures on the frozen fixture set, validation clean, Debug/Release tests, one matched
native-4K timing pair per slice with no median/p99 regression); measured build-time,
CPU-frame, and hitch improvements; no source file above an agreed size guideline
without justification; and a written M7R-to-M9 handoff.

### M8 - Meshlet cooker and mesh-shader path

Status: `Proposed`

Dependencies: M3, M7.

Deliverables:

- Offline meshlet construction with tunable vendor-neutral limits, local indices, bounds, and normal cones.
- Mesh-shader capability detection and pipeline path using `VK_EXT_mesh_shader`.
- Meshlet culling and indirect mesh-task dispatch consuming the M7 visibility architecture.
- Per-LOD meshlets use screen-space error, bounds, and normal-cone rejection so very
  dense assets can discard invisible clusters after M7 instance/primitive culling.
- Meshlet-driven shadow-caster submission feeding the same conventional/virtual
  visibility ownership and cached page requests as the indexed path.
- Visibility-buffer emission consumes the same payload/material-resolve contract as
  the indexed M7 path; no mesh-shader-only scene representation is introduced.
- Benchmarks against indexed indirect rendering for both packed-deferred and
  visibility-buffer workloads; select the faster path per workload/capability.

Acceptance gate: mesh shaders produce matching images and visibility identities and a measured benefit on appropriate high-geometry scenes without becoming a mandatory regression for small meshes.

### M9 - Temporal rendering and reconstruction

Status: `Ready` (lead prompt `docs/milestones/M9-task-lead-prompt.md`; execution plan
`docs/milestones/M9-temporal-and-post.md`, draft awaiting owner approval 2026-10-05)

Director decisions (2026-10-04):

- **Branching:** PR #7 (M7R) merges into `Render-Refactor-for-Modularity`; M9 branches
  from there.
- **F6 +1.8% GPU (VMA placement):** carried as a watch item, not a pre-slice. The
  delta (~0.06 ms on a non-gating probe route) is smaller than the observed
  environmental swing, and M9's new alias-eligible images will reshuffle placement
  anyway. M9 bisects any regression by pass GPU ranges, and re-measures F6 at
  acceptance; a placement policy (dedicated/aligned allocations for the refraction
  pyramids and cluster buffers) is applied only if a placement-attributed regression
  above 1% remains.
- **Capture point under TAA:** the scene-linear (`scene`) capture stays **before**
  TAA as the single-frame radiance domain. A TAA-off, jitter-off route is mandatory
  and must reproduce the M7R frozen set byte-for-byte (refactor tier). TAA-on adds a
  new post-TAA scene-linear capture domain (before bloom/exposure) plus the final
  output; its fixtures use deterministic per-view jitter sequences and measured
  envelopes (feature tier).
- **Scope split:** M9 delivers native-resolution motion vectors, jitter, TAA,
  reactive handling, bloom, and auto-exposure, plus the vendor-neutral
  super-resolution input contract. DLSS/FSR/XeSS integration and dynamic resolution
  move to **M9b**, scheduled before M11 (the RT tier is their consumer). Generic
  reprojection/history utilities land in M9; their stochastic-shadow/GTAO consumers
  land in M10.

Dependencies: M1, M2, M7R, and the current/previous transform data already provided
by the accepted M7.1/M7.2 GPU scene. Scheduled immediately after M7R (2026-10-02). M7R is accepted (2026-10-04). Its hand-off
(`docs/milestones/M7R-to-M9-handoff.md`) notes that the GPU scene's previous transform
changes only when the transform changes (a stopped object keeps stale motion), that
direct-packet draws carry no previous matrix, and that graph History is not yet keyed
per view. M9 closes these first.
As the raster target is native 4K, M9's first production deliverable is native-
resolution temporal AA plus motion vectors; sub-native reconstruction serves the RT
tier.

Deliverables:

- Stable jitter, current/previous matrices, skinned motion, depth, exposure, reactive data, and history invalidation.
- Native TAA/DLAA-quality reference path.
- Core HDR post-processing pulled forward with M9: physically based bloom on the
  existing disabled graph hook and automatic exposure/eye adaptation (deferred to
  M9 by ADR-0002), both scene-linear before the single output transform.
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

### M12 - Material authoring and graph editor

Status: `Proposed` (scheduled after M0-M11)

Dependencies: M2 material/closure contracts, M3 asset/DDC infrastructure, M4 editor
transactions and asset documents, M6 transparency policy, and the M11 shared
raster/RT material semantics.

Deliverables:

- A versioned, GUID-addressed source material graph with deterministic serialization,
  explicit migrations, copy/paste, comments/groups, search, validation, undo/redo,
  and recoverable autosave.
- Nodes that compile into the existing standard, complex-forward, transparent, and
  RT-compatible closure contracts rather than introducing an editor-only shading
  representation.
- Live material-instance parameters, texture/sampler selection, transparency/layer
  policy, compile diagnostics, generated-code/closure inspection, and an isolated
  HDR model/sphere preview using the production renderer.
- Asynchronous incremental graph compilation and DDC publication. Unaffected
  functions, textures, geometry, and material instances remain resident; failed
  compiles retain the last-known-good product.
- Artist-facing cost diagnostics for closure class, texture sampling, transparency
  tier, shader permutations, and expected forward/RT consequences.

Acceptance gate: artists can build and debug representative opaque, coated,
transmissive, and emissive materials without editing glTF metadata or source code;
saved graphs round-trip deterministically, live preview and scene results agree, bad
graphs fail safely, and interactive edits do not stall the editor or recook geometry.

### M13 - Animation runtime and animation graph editor

Status: `Proposed` (scheduled after M12 and the current M0-M11 renderer program)

Dependencies: M3 assets/cooking, M4 scene/editor identity, M7 static/dynamic GPU
scene updates and visibility, M9 current/previous motion data and temporal
invalidation, and M12's mature graph-editor interaction patterns where reusable.

Deliverables:

- Versioned skeleton, skin, animation-clip, retargeting, compression, root-motion,
  curve, event, and animation-controller products with stable bone/clip identities.
- A runtime animation graph supporting states, transitions, parameters, blend trees,
  additive layers, masks, sync groups, cached poses, events, and deterministic
  evaluation/fallback behavior.
- A transaction-safe graph editor with searchable nodes, live state/transition
  debugging, pose inspection, timeline/scrubbing, validation, and isolated preview.
- Multithreaded pose evaluation and bounded GPU skinning integrated with M7 visible
  instance/LOD selection. Off-screen and distant animation uses measured update-rate,
  pose-cache, or skip policies while preserving root motion and gameplay events.
- Correct current/previous skinned positions for motion vectors, temporal
  reconstruction, shadows, reflection captures, and future BLAS update policy.

Acceptance gate: representative characters blend and transition deterministically,
motion vectors and shadows remain correct, graph edits hot-reload without corrupting
runtime state, and CPU/GPU animation cost scales with visible animated work and
quality policy rather than every loaded clip or character.

## Program controls

M5 post-acceptance hardening is tracked in M5.12 (reflection resolution) and M5.13
(shadow direction/quality). M5.13 establishes local `+Z` emission, 4096 directional
maps, an 8192 spot atlas with 4096 Ultra tiles, denser PCSS, and an antialiased
point-like hard-shadow limit. This conventional-map baseline is inherited by M7
virtual shadows and M9 temporal filtering; it does not replace those successors.

The owner-observed approximately 180 FPS with three high-fidelity assets versus
approximately 1,700 FPS empty on the same 240 Hz display during M6 is a useful
workload signal, not a frozen baseline. It rules out a 180 Hz refresh ceiling and
indicates roughly 4.97 ms of scene-dependent wall work, while the coarse title
window and mailbox acquire/present behavior still prevent bottleneck attribution.
Before M7 work, capture the exact assets/camera/settings as a native-4K Release
fixture and report GPU pass times, non-waiting CPU stages, acquire/present waits,
requested/visible geometry, draws, transparency, shadow work, and residency. M7/M8
optimization is accepted from those counters and matched imagery, not title FPS
alone.

Program schedule (owner decision 2026-10-02): **M7R architecture consolidation ->
M9 temporal AA, motion vectors, bloom, and auto-exposure -> M7.9-M7.12 -> M8 ->
M7.8 Virtual Shadow Maps resumed on meshlet caster submission -> M10 -> M11.** The
material editor follow-ups and the Porsche mixed-class glass ordering defect are
deferred until after M7R/M9. Previously the order was M6 through M11. M12 material authoring and M13 animation
graph work are intentionally placed afterward and must not expand active renderer
milestones.

- Do not start a milestone before its dependencies and acceptance criteria are understood.
- Every milestone begins with a checked-in execution plan following `PLANS.md`.
- Architectural changes require an ADR or explicit update to an existing proposed ADR.
- Every milestone ends with a review, verification report, and updated baseline.
- New features discovered during implementation enter the roadmap explicitly; they must not disappear into chat-only follow-up notes.
