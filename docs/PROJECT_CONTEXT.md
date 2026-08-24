# Iridium Engine Project Context

## Why this document exists

This is the compact handoff for new lead tasks. It records facts found during the post-RHI-refactor architecture review, accepted direction, and unresolved choices. It is not a substitute for reading current source, the roadmap, milestone plans, and ADRs.

## Product intent

Iridium is intended to be a visually ambitious, high-end engine rather than a lowest-common-denominator renderer. The reference PC is an RTX 4090, Core i9-14900K, 64 GB DDR5-6000, a 2 TB Samsung 990 Pro, and a 4K HDR display. The target is more than 100 FPS in fully dressed active gameplay scenes, with native rendering or high-quality temporal reconstruction chosen intentionally.

The engine should eventually support wide-gamut HDR, GPU-driven submission, mesh shaders, DLSS-class reconstruction, hybrid ray tracing, and a reference path tracer. Before those become dependencies, it needs a complete raster foundation: lights, shadow maps, image-based lighting, probes/cubemaps, baking, and credible non-RT GI.

## Current implementation facts from the architecture review

These facts must be revalidated as the refactor continues.

### Rendering and draw data

- The application currently builds draw packets per submesh. A full model matrix is repeated in each packet; the observed packet was approximately 112 bytes before container and alignment overhead.
- Transparent sorting uses a distance derived from the entity origin rather than a per-transparent-primitive bound or depth interval.
- The Vulkan transparency path treats the last packet as foreground and the remainder as background. This is not general depth peeling and cannot correctly model arbitrary nesting.
- Import-time merge-by-material can join disconnected transparent surfaces and discard the spatial boundaries required for useful sorting, culling, or layer classification.
- The production canonical surface cache R uses four `RGBA16F` targets plus
  `R32_UINT`, or 36 color bytes/pixel before depth. Q/C are selectable experiments,
  but are not production because they lose scalar F90 and full metadata.
- Canonical draw push constants contain a matrix and indexed material identity.
  Closure state lives in schema-2 GPU material buffers instead of growing per-draw
  authoring fields.

### Materials and the sample car

- M2 replaced the legacy combined material path with a provenance-preserving glTF
  source model, pure compiler, immutable material instance, schema-2 packed GPU
  record, and unconditional canonical deferred/forward GPU paths. Active clearcoat,
  sheen, anisotropy, iridescence, transmission, volume, diffuse-transmission, and
  unlit records are explicit rather than silently reduced.
- The shaders do consume per-material base color, metallic, and roughness factors; the car is not simply rendered with one universal hardcoded metallic/roughness value.
- In the inspected sample car, 25 of 87 materials omitted `metallicFactor`, which means the glTF default of `1.0`; 61 explicitly specified `0.0`. A red paint material omitted metallic, used roughness around `0.523`, and declared clearcoat. A glass material used transmission `1.0` and also omitted metallic. If clearcoat is ignored and omitted values are accepted without diagnostics, the result can look implausibly dark or metallic even while technically following individual glTF defaults.
- The current lighting is a major confounding factor: it includes demonstration/hardcoded behavior and the environment path does not yet implement a complete irradiance plus prefiltered-specular IBL solution with a BRDF integration term.
- The production graph composes deferred, opaque-complex forward, emissive, and
  transparent forward results in FP16 scene-linear AP1 before one output transform.
  Selection is a final-output boundary overlay and does not alter selected shading.

### ECS, editor, assets, and scenes

- Component pools use dense vectors with an `unordered_map` sparse lookup. This can be adequate for iteration, but entity-to-component lookup, allocation behavior, stable identity, and query construction should be measured before redesign.
- Components expose inspector behavior such as `DrawInspector`/`OnInspector`, pulling ImGui and editor concerns into runtime data types.
- Scene serialization is manual and centrally coordinated. The reviewed path lacks a durable top-level schema version, per-component versions, stable entity UUIDs, a component serializer registry, migrations, and robust unknown-component preservation.
- M3 removed source-path runtime identity. Schema-1 source-controlled sidecars own
  UUIDv7 root/subasset identity and deterministic settings; the rebuildable SQLite
  catalog is editor search state, not runtime authority.
- Import discovery/parsing, CPU cooking, DDC artifacts, runtime decode, GPU
  publication, and residency are separate. glTF runtime parsing, path-keyed model
  caches, merge-by-material cooking, and per-material descriptor sets are gone.
- The Asset Browser owns project import, physical folders, search/filter,
  thumbnails, reimport, and assignment. Viewport, hierarchy, and Mesh Inspector
  drops assign already registered GUID assets; the hierarchy no longer imports
  source files.
- New scene mesh references are GUID-only and persist material overrides, entity
  names, and sibling order. The removed pre-M3 `meshPath` field is rejected with a
  migration diagnostic. M4 completed the scene/component schema rewrite; the
  central legacy serializer is deleted and logical v0 is migration-only.
- Production Vulkan rendering uses dynamic indexed material storage plus typed
  indexed texture-view and sampler tables. Descriptor indexing is a required
  high-end capability; material capacity is derived from device limits rather than
  fixed at 4,096.

## Accepted architecture

- Preserve rich source materials, but compile ordinary single-scattering surface materials to a canonical runtime closure: diffuse albedo, specular F0, perceptual roughness, normal, AO, and emissive.
- Converting glTF specular/glossiness to that closure can be physically faithful for a single dielectric/conductor microfacet model. Conversion is lossy when the source represents multiple lobes, coatings, volume transmission, sheen, anisotropy, or other closures; classify those into a forward complex-material path.
- Supporting F0 directly avoids treating metallic as the only possible specular control. Authoring workflows may remain metallic/roughness, specular/glossiness, or extension-based; runtime storage should follow the evaluated closure.
- Keep a high-precision reference GBuffer first. Introduce a near-term production
  packed canonical surface cache only after image-difference, frame-time, and
  bandwidth evidence. It is not a permanent requirement that geometry emit a full
  GBuffer; ADR-0006 defines the later visibility-buffer migration.
- Share BSDF code and material data semantics across deferred, visibility/material
  resolve, forward, and future ray-tracing paths.
- Use hybrid transparency classification. Default inexpensive paths should handle ordinary surfaces; hero/nested/refractive glass can request bounded extra layers or a complex forward path.
- Establish a render graph and linear HDR/color-managed pipeline before adding more major lighting features.
- Build stable asset, scene, and component identities before a persistent GPU scene depends on them.
- Add indexed indirect GPU-driven rendering before mesh shaders. Mesh shaders should reuse the same GPU scene, visibility data, and cooked meshlets rather than create a separate renderer.
- Use one clustered-light representation for deferred/material-resolve and complex-
  forward consumers. After stable GPU-scene identities exist, measure an indexed
  visibility-buffer material resolve into the M2 canonical surface cache; later mesh
  shaders emit the same visibility representation. Conventional packed deferred
  remains a workload/capability fallback.

## Important unresolved choices

- A future packed cache that preserves scalar F90 and full material/feature identity
  while retaining most of Q/C's measured bandwidth benefit.
- Visibility payload width, barycentric/attribute reconstruction policy, and whether
  a later fused material-plus-lighting resolve beats an explicit canonical surface
  cache on representative 4K scenes.
- Any future approximation threshold that would reduce a nonzero complex lobe to the
  standard closure. M2's initial policy is exact classification: dormant zero-effect
  lobes may compile standard with diagnostics; materially active coat, sheen,
  anisotropy, iridescence, or transport selects a complex closure.
- M1 accepted a backend-neutral graph compiler with a Vulkan-owned executor,
  graphics-queue scheduling, two-frame-context transient ownership, and compatible
  physical reuse. These are current architecture, not an unresolved proposal.
- Scene source format details, registry API, and migration policy limits.
- Progressive independently resident texture products, remote/shared DDC, and
  virtualized payloads for sub-second placement of arbitrarily large packages.
- Transparency algorithms and quality tiers after representative captures and timing data exist.
- Native TAA/reconstruction baseline and external SDK abstraction.

## Immediate next step

M0 was accepted on 2026-07-18 after closing the independent audit. The authoritative
decision and exact baseline are
`docs/milestones/M0-acceptance-report-2026-07-18.md`; the reopen audit remains useful
historical evidence. Full-run percentiles cover the complete 10,000-frame contract,
the frozen run header and cold/import scopes are populated, C++ allocation and
transparent-work counters exist, and nested-transparency plus opaque-emissive axes
are deterministic required fixtures.

M1 was accepted on 2026-07-22. Read
`docs/milestones/M1-acceptance-report-2026-07-22.md`, the completed
`docs/milestones/M1-render-graph-hdr-color.md`, ADR-0002, and current source. The
production graph path owns FP16 ACEScg/AP1 composition, one ACES 2 output boundary,
display-linear UI, SDR/scRGB/HDR10 transports, optional HDR metadata, and distinct
scene/final capture domains. Debug and Release pass 15/15 tests. All required 4K
fixtures and HDR resize runs are validation-clean. Output transforms cost
0.043008-0.049152 ms median; combined output/UI is 0.144384 ms SDR, 0.266240 ms
scRGB, and 0.205824 ms HDR10. Worst requested transport peak is 886.662 MiB.

M2 was accepted on 2026-07-25. Read
`docs/milestones/M2-acceptance-report-2026-07-25.md`, the completed execution plan,
ADR-0001, ADR-0006, and current source. The graph and canonical material path are
unconditional; deprecated graph/material switches and the manual renderer are gone.
R is the complete production reference cache, while Q/C remain measured experiments.
Debug/Release pass 20/20 tests; eleven tracked 4K fixtures, the optional car,
SDR/scRGB/HDR10, selected-object captures, and Nsight frame replay pass. The final
proxy baseline is 0.455296 ms GPU and 0.631800 ms CPU with 893.159 MiB requested
peak. The Project Settings editor exposes live exposure, paper white, and peak;
`--output-transport scrgb` is the recommended Windows HDR editor startup mode and
does not require exclusive fullscreen.

M3 was accepted on 2026-07-31. Read
`docs/milestones/M3-acceptance-report-2026-07-31.md`, the completed execution plan,
ADR-0004, and current source. Production assets have source-controlled UUIDv7
identity, deterministic importer/settings/dependency/cook/DDC contracts, cooked-only
runtime publication, and dynamic indexed material/view/sampler resources. The
project-owned Asset Browser provides physical folders, import/reimport,
search/filter, thumbnails, nested associations, drag/drop assignment, and scene
persistence by GUID. Debug/Release pass 39/39; eleven cooked M0-M2 fixtures, the 4K
sample car, output transports, resize, residency churn, 65,536 materials, and 8,192
views/samplers are Vulkan-validation clean. Two clean Sponza production cooks are
byte-identical. The five-run 4K car baseline is 0.8450 ms median GPU and 1.7080 ms
median CPU; asset-runtime-tick p99 is 0.0029 ms.

M4 is accepted. M4.0-M4.2 established generational
runtime handles, stable scene UUIDv5/v7 identity maps, frozen runtime/source
component registries, strict schema-1 source scenes, canonical unknown-preserving
round trips, and deterministic logical-v0 migration with path-identity rejection.
M4.3 staged load, atomic save/recovery, and editor document lifecycle is accepted.
It now has an address-stable swap-on-success staging world, metadata-driven stable-
reference collection, production core component adapters, unknown-preserving live-
world capture, and a verified same-directory `ReplaceFileW` save primitive with
`.bak` retention and failure injection. The editor document service owns canonical
`.iridium.scene.json` Open/Save/Save As, scene metadata sidecars, logical-v0
migration, state-token dirty checkpoints, explicit backup recovery, and UUID-based
selection recovery. The legacy serializer is deleted.
Debug and Release pass 52/52. Orphan-temp recovery is asynchronous and explicit,
and pending/failed/later-resident asset coverage proves runtime residence does not
change document state or serialized GUID intent. Release lifecycle evidence at
1k/10k/100k records an allocation-free sub-microsecond staged-world commit at 100k,
while strict source parsing and verified save remain deliberately expensive and are
assigned to the later cooked-runtime boundary. The menu uses a testable document-
command layer for Open/Save paths/recovery/selection, a 1,000-frame Release 4K
validation run is clean, editor initialization is 0.0211 ms median versus M3's
0.0537 ms, and the comparable 100k inspector case remains allocation-free.

M4.4 deterministic cooked runtime scenes is accepted. Production-style runtime
loads now consume validated `iridium.scene.runtime` schema-1 artifacts with exact
section, registry-manifest, target/CookKey, dependency, component-stream, and stable-
reference validation before an allocation-free active-world swap. Headless cooking
uses explicit `iridium.scene@1` metadata, the M3 DDC and atomic receipt contract;
valid warm hits skip JSON parse and scene compile. Cross-process outputs are byte-
identical and the source-free runtime boundary links no editor, ImGui, importer, or
JSON library. Debug/Release pass 56/56. The 100k artifact is 5.34 MiB, stages in
148.105 ms median, and commits in 0.0007 ms with zero allocations; its 254.994 ms
p95 and roughly eight allocations/entity remain a visible load-path risk. M4.5
editor/runtime component separation and M4.6 editor transactions are accepted.
M4.6 provides atomic apply/rollback, exact undo/redo and branching, savepoint-aware
dirty state, coalescing, stable GUID Mesh edits, metadata-driven properties,
hierarchy multi-selection, atomic multi-target edits, component snapshots, global
shortcuts, and visible failure diagnostics. Debug/Release pass 58/58; 10k-target
apply is 0.112 ms median, selected-entity gizmo processing is 4.1 us median, and a
like-for-like 1280x720 editor run retains the M4.5 allocation median. M4.7 structural
editing and actual viewport-extent integration is accepted. M4.8 evidence-driven
ECS tightening is also accepted: component pools retain dense vectors but replace
per-pool sparse hash maps with demand-paged 32-bit sparse indices. At 100k entities,
random lookup p95 improves 72.6-98.6%, representative view p95 improves 57.6-75.0%,
dense iteration improves 1.0%, and Debug/Release pass 59/59 including a deterministic
100k-operation property test. Five repeated 4K sample-car runs preserve M3 CPU,
VRAM, and zero-allocation gates; final validation is clean. M4.9 isolated asset
viewers are accepted. M4 is accepted as of 2026-08-03. M4.10 deleted the central
legacy serializer and characterization target, retained v0 only as pure migration,
removed editor mutation fallbacks, and froze exact schema-1, canonical, runtime-
manifest, CookKey, and cooked-artifact hashes. Debug and Release pass 60/60. The
final five-run 4K car preserves exact M3 live/peak requested and committed memory,
zero allocation median/p99, and a validation-clean final-SDR image byte-identical to
M3.7. The 100k cooked path stages in 136.526 ms median and commits in 0.0007 ms
allocation-free. Strict very-large source JSON remains a documented editor/cook-host
risk and is never a runtime fallback. Full evidence is
`docs/milestones/M4-acceptance-report-2026-08-03.md`.
M5.0 and M5.1 are accepted as of 2026-08-08. M5.0 froze deterministic lighting
reference math, fixtures, captures, and a 250,000-frame 4K baseline without changing
production rendering. M5.1 advances `iridium.component.light` source/cooked data to
version 2 while retaining its stable ID and `LGT1`: linear Rec.709/D65 color,
lux/candela, metre/degree shape fields, shadow quality, and priority are explicit;
v1 and legacy-v0 migration is visible and deterministic; legacy Area remains
readable but strict cooking rejects it. Accepted ADR-0007 owns the photometric and
single shared clustered-assignment direction. Debug and Release pass 63/63 tests.
M5.2 is accepted as of 2026-08-09. Backend-neutral, UUID-owned 64-byte light records
now extract directional/point/spot components with scale-independent hierarchy
orientation, deterministic slots/tombstones/overflow diagnostics, and allocation-
free steady processing. Vulkan owns geometrically grown per-frame storage buffers,
revision-exact range uploads, a permanently valid zero-light fallback, capabilities,
profiles, and dedicated memory accounting. Debug/Release pass 64/64; a 4,096-light
validation run proves two-frame publication followed by zero uploads, and the 4K
sample-car image remains byte-identical to M5.0. Full evidence is
`docs/performance/M5.2-gpu-light-records-2026-08-09.md`.
M5.3 is accepted as of 2026-08-09. One graph-declared GPU cluster product now
supplies both deferred and complex-forward descriptor contracts. The measured
production grid is 32x32x24 logarithmic: a final 512-light 4K run costs 0.241 ms
median / 0.259 ms p95 and 18.991 MiB per frame context. Normal lists are bounded and
deterministically sorted; capacity overflow publishes no partial list and selects
one UUID-stable top-64 fallback. Debug/Release pass 65/65, normal and overflow Vulkan
runs are validation-clean, and final SDR remains byte-identical to M5.2. Full
evidence is `docs/performance/M5.3-shared-clustered-assignment-2026-08-09.md`.
M5.4 is accepted as of 2026-08-09. Authored directional, point, and spot records
are now the sole production direct-light source. Deferred and complex forward share
one descriptor-free physical light evaluator and consume the same global/local/
fallback assignment. Unlit, emissive, AO, and complex-lobe layering retain their
defined semantics; a direct-only debug view isolates the result. Debug/Release pass
65/65, normal and fallback Vulkan runs are validation-clean, and equivalent 4K
standard surfaces differ by at most one SDR code with 0.999996 mean luma SSIM. Full
evidence is `docs/performance/M5.4-clustered-direct-lighting-2026-08-09.md`;
M5.5 is accepted as of 2026-08-09. Production now loads a deterministic,
source-free `iridium.environment` artifact containing AP1 radiance, exact source-
texel SH9 diffuse irradiance, GGX-prefiltered specular radiance, and an F0/F90 BRDF
LUT. Deferred and complex forward share one IBL include and equivalent 4K standard
surfaces differ by at most 0.000001188 scene-linear luma. The High product adds
20.297 MiB requested persistent memory; deferred direct-plus-complete-IBL costs
0.110 ms median / 0.119 ms p95. Missing products use semantic neutral cubes/LUT,
and atomic cooked hot replacement is Vulkan-validation clean. Debug and Release
pass 65/65. Full evidence is
`docs/performance/M5.5-cooked-environment-ibl-2026-08-09.md`.
M5.6 is accepted as of 2026-08-09. One priority/UUID-selected directional light now
owns four stabilized 2048 D32 cascades in a persistent imported graph resource.
Deferred and complex forward share 10% cascade blending, 5x5 tent compare PCF, and
the same bias/fallback contract. Static measured frames update no layers; a moving
caster refreshes all four at 0.0171 ms median / 0.0177 ms p95. The product costs
exactly 64 MiB requested/committed, Debug/Release pass 66/66, and alpha-mask,
one/two-sided, cache, capture, and Vulkan-validation gates are clean. Accepted
ADR-0008 owns the lasting raster-shadow history/cache contract. Full evidence is
`docs/performance/M5.6-directional-shadows-2026-08-09.md`. M5.7 is underway with
deterministic local-shadow request ranking, stable guarded spot-atlas and tiered
point-pool allocators, frozen spot/cube projections, and a rendered-texel cache
scheduler. Only caster-compatible history may remain stale for its bounded two-frame
diagnostic window; incompatible missed updates are unshadowed. Vulkan local-shadow
work now includes the first complete spot GPU slice: a persistent guarded D32 atlas,
constant-time packed-light slot publication, shared deferred/forward 5x5 sampling,
cached alpha-mask rendering, and conservative transformed-sphere caster culling.
The default 4096 atlas is exactly 64 MiB and a two-owner cold update measured
0.016640 ms in Debug validation. Tiered persistent 256/512/1024 D32 cube arrays now
provide 56 stable point slots, whole-cube cache publication, six-face caster
culling, seam-safe shared 5x5 sampling, and independent overlapping visibility. The
default point reservation is 336 MiB; a cold two-owner 512 update measured
0.031744 ms and static frames reuse both cubes without raster work. Final
complex-forward and multi-process M5.7 acceptance remain.
During M5.7, ADR-0009 superseded only ADR-0008's single-directional-owner capacity:
two independent four-cascade owners now compose visibility per light. The default
eight-layer 2048 D32 allocation is 128 MiB; a validated 1024 policy is exactly
32 MiB. Backend-neutral project settings own resolution/owner/update/stabilization
policy, while persisted Light components continue to own per-light shadow enable,
quality, and priority. Bloom is still a disabled graph hook with no effect workload.
Spot atlas resolution is startup-configurable at 2048/4096/8192; Project Settings
owns its live update/stale policy. Point lights select 256/512/1024 quality tiers;
project policy owns tier capacities, a default 12 MiTexel whole-cube update budget,
and the bounded compatible-stale window.
M5.8 is complete with `iridium.component.sky`, a stable three-mode component with
separate Skybox, HDRI, and Simulated settings. HDRI is the only implemented render
mode in this slice: first-class `.hdr` environment assets cook through DDC, produce
Asset Browser thumbnails, support typed Inspector drag/drop, persist source/cooked
GUID intent, and drive shared deferred/complex-forward background and IBL intensity,
rotation, camera visibility, lighting participation, and priority. Selection uses
priority then stable scene UUID; missing or failed products retain authored intent
and diagnose a safe black/neutral fallback. Skybox rendering remains future work;
the Simulated mode's physical sky/atmosphere/cloud path belongs to M10.
The same slice now provides stable sphere/box local reflection probes, bounded
four-candidate/two-sample clustered specular blending shared by deferred and
complex-forward paths, box projection, six-face AP1 scene capture, direct-light and
raster-shadow inclusion, recursive/local-owner exclusion, configurable Hammersley
GGX filtering, last-known-good publication, and project-wide capture budgets.
Baked mode creates complete reusable `.irprobe` environment assets with exact
cube-texel SH9 irradiance, the shared BRDF LUT, UUIDv7 sidecars, scene/shader
dependencies, atomic replacement, and Asset Browser refresh. Debug/Release pass
68/68; a 512 capture/filter costs 1.234144 ms GPU and one steady local probe adds
0.008448 ms median GPU plus 16.03125 MiB committed live VRAM. Evidence is
`docs/performance/M5.8-reflection-probe-capture-2026-08-11.md`.
Accepted ADR-0010 extends the renderer toward high-end-first contact-hardening soft
shadows, sparse virtual shadow pages, optional diagnosed screen-space contact detail,
M6/M10 RGB translucent shadow transmission, GTAO/CACAO plus specular occlusion in
M10, and RT area shadows/RTAO/transmission in M11. “Virtual Shadow Maps” and
“Variance Shadow Maps” remain explicitly different techniques. Conventional cached
maps and fixed PCF remain robust fallbacks until matched 4K evidence justifies each
successor. Project profiles and bounded per-light/per-volume overrides own quality,
samples, resolution/page pools, update/owner budgets, and VRAM limits.
M5.9 is complete. Directional, spot, and point conventional maps now share a
bounded raw-depth blocker search and source-size-driven PCSS filter with point-source
hard limits. Directional softness uses an explicit angular diameter (0.535 degrees
by default); local lights use their persisted meter source radius. Low or explicit
fixed mode retains deterministic 5x5 PCF. Project Settings exposes filter mode,
Low-through-Cinematic ceiling, directional source diameter, and maximum penumbra;
capture metadata records the active samples and physical extent. The shared RHI
contracts reserve conventional/virtual/RT representations, scalar/RGB visibility,
virtual pages, stochastic temporal inputs, GTAO/CACAO/bent-normal/specular
occlusion, and deterministic raster fallbacks. Five-process 4K Release spot-light
measurement puts Ultra PCSS at 0.449536 ms median deferred lighting versus
0.240640 ms fixed PCF, a 0.208896 ms delta. Debug/Release pass 68/68 and validation
is clean for deferred and complex-forward directional/spot/point fixtures. Evidence
is `docs/performance/M5.9-contact-hardening-shadows-2026-08-13.md`.
M5.10 is complete. `iridium.component.baked_lighting_set` / `BLS1` is the stable
scene owner for a cooked lighting GUID, diffuse/specular intensity, and independent
lightmap, irradiance-volume, and visibility contributions. The versioned
`iridium.baked-lighting` product uses scene entity UUID plus mesh primitive GUID
associations, typed optional sections, scene-linear ACEScg/metre semantics, and
separate SHA-256 scene/geometry/material/light/settings/tool fingerprints. Unknown,
truncated, mismatched, or future sections fail closed; bad publication retains the
last complete revision and missing data contributes neutral lighting. Accepted
ADR-0011 owns this M10-compatible boundary. Debug/Release pass 69/69. A five-process
55,934,252-byte Release contract benchmark loads/validates in 8.9649 ms median and
publishes in 8.9994 ms median. No solver, streaming, GPU sampling, or GI quality
claim is part of M5.10. Evidence is
`docs/performance/M5.10-baked-lighting-contracts-2026-08-13.md`.
M5 was accepted on 2026-08-13. Read
`docs/milestones/M5-acceptance-report-2026-08-13.md`, the completed execution plan,
ADR-0007 through ADR-0011, and current source. The production renderer now has
physical authored clustered lights shared by deferred and complex forward, cooked
AP1 IBL, independent cached directional/spot/point visibility, fixed PCF and
physical-source PCSS, production HDRI Sky, clustered local reflection probes with
capture/baking, and typed future-GI baked-lighting contracts. Debug/Release pass
69/69. A final cooked-HDRI dressed car measures 4.238432 ms GPU median of medians,
4.484928 ms worst p95, and 4.520544 ms worst p99 across five native-4K processes;
opposing shadow owners, car final/normal detail, SDR/scRGB/HDR10, and resize
lifecycle evidence are Vulkan-validation clean. The truthful retained steady frame
has 39 C++ allocations / 5,288 requested bytes; this bounded scheduling churn is a
documented later optimization target rather than a leak or hidden zero-allocation
claim.

M5.12 post-acceptance hardening replaces the low-resolution HDRI reflection default
with an explicit, user-configurable quality ladder. Importer/cooker v3 defaults to a
1024-face GGX product, removes the editor's hidden 128-sample clamp, builds true
radiance mips, and uses PDF-aware source-mip sampling to avoid bright-texel
"bokeh" artifacts. Parallel cooking remains byte-deterministic. Existing HDRIs keep
their authored settings until an explicit Upgrade to Ultra plus reimport, and the
editor shows the resulting memory estimate. The general 128 MiB publication budget
remains the per-tick scheduling target. HDRI publication may use one atomic upload
under its 640 MiB per-environment cap, and M6 model publication may likewise use
one atomic upload when a valid model exceeds the scheduling target, under a 1 GiB
per-model hard cap. One
matched native-4K checkpoint adds exactly 108 MiB environment residency with no
median CPU/GPU frame regression. Debug/Release remain 69/69 and the Ultra dressed-
car path is Vulkan-validation clean. See
`docs/performance/M5.12-reflection-resolution-stabilization-2026-08-13.md`.

M5.13 post-acceptance corrective hardening unifies editor and renderer direction:
transformed local `+Z` is the authored emission axis, and shading negates it only
when a surface-to-light vector is required. High-end conventional-shadow defaults
are now 4096 directional cascades, an 8192 spot atlas with 4096 Ultra tiles, denser
Ultra/Cinematic PCSS, antialiased zero-radius hard shadows, and nearest raw-depth
point sampling before explicit PCF/PCSS. The Light v2 schema and ADR-0007's physical
calibration remain unchanged. See
`docs/performance/M5.13-shadow-direction-and-quality-hardening-2026-08-13.md`.

M6 owns general transparency/refraction and colored translucent transmission; M7
owns persistent GPU-scene and visibility-buffer work; M10 owns AO, atmosphere/clouds
and GI solvers; M11 owns hybrid RT. These milestones must consume M2-M5 closure,
identity, primitive, lighting, visibility, and product contracts rather than
reintroduce paths, authoring-workflow material storage, or display-referred shading.
M6.5 Ordinary2 is complete as of 2026-08-23. Five independent native-4K Release
processes over 50,000 measured populated-fixture frames report a 0.806944 ms GPU
frame median-of-medians and a 0.414720 ms full transparency-chain
median-of-medians, with identical graph/memory/work counters and no atlas rejects,
fallbacks, topology events, or profiler drops. M6.6 is complete and M6.7 WeightedOIT
approximate workloads are the active transparency slice.
Its backend-neutral CPU foundation now includes bounded 2/4/8 stack reduction and
independent deterministic per-tier atlas preparation. Transitively overlapping
same-tier work now shares one optical-island rectangle while keeping stable per-work
identities. Hero4/Cinematic8 GPU storage is conditionally represented by independent
4/8-interface Vulkan graph products with explicit residency, resize, and restoration.
The indexed peel contract supports nested cross-work sequences and preserves the live
Ordinary2 paired-entry/exit path through an explicit compatibility flag. Deep
frame targets now materialize 4/8 capture framebuffer chains, local-color targets, and
one previous-interface descriptor set per peel when explicitly resident. Incomplete
chains fail rebuild and restore the prior topology. Explicitly authored Hero4 and
Cinematic8 content now prewarms its tier and records a bounded stable draw plan through
four/eight sequential interface captures. Deep local composition is now active: it
rerasterizes captured entry slots deepest-to-nearest, validates identity/orientation/
depth, pairs each entry with a later same-work exit, and evaluates the shared measured-
chord material path into premultiplied AP1 local atlases. Hero4 and Cinematic8 scene
resolve are active: interface-zero identity gives exactly one nearest captured work
item ownership of each composed pixel, and accepted deep packets are suppressed from
compatibility forward. When both tiers coexist, one graph pass consumes their packets
in global transparent order and switches tier descriptors without merging the atlases.
Rejected work retains compatibility fallback. Bounded overflow residual evaluation
is now active inside the existing tier-local composition pass: semantic entries behind
the eighth stored interface, plus exits for work left open at capacity, receive a
finite authored-thickness non-refractive Beer-Lambert operator before the exact prefix.
It adds no graph image, graph pass, or steady-frame allocation. The single-alpha exact
and residual operators reduce colored transmittance to AP1 luminance. The deterministic
deep-tier semantic GPU qualification is complete. A
validation-enabled 1280x720 Debug run of two nested closed Hero4 shells
published 10,922 valid paired pixels, including 3,890 pixels with all four ordered
interfaces, and 10,922 finite premultiplied local-color pixels. Stable identity,
orientation, depth ordering, interface continuity, pairing, and local-color checks
reported zero errors and Vulkan validation reported zero messages. This is not
performance evidence: the four-frame Debug run deliberately includes a one-shot atlas
readback. End-to-end validation additionally proves two Hero4 scene-resolve draws and
zero compatibility-forward draws. A four-shell Cinematic8 fixture additionally proves
15,042 paired pixels, all eight ordered interfaces at 1,636 pixels, four scene-resolve
draws, zero fallback draws, and zero Vulkan messages. Belfast Ultra final-SDR captures
visibly retain the nested shells without duplicate-resolve silhouettes. A mixed
Hero4/Cinematic8 run records one global-order resolve range, four resolve draws, and
zero compatibility draws. A five-shell Cinematic8 overflow fixture requests ten
interfaces while storing eight; validation reports 650 saturated-prefix pixels,
1,300 estimated residual samples per measured frame versus zero in the four-shell
control, five resolve draws, zero fallback draws, zero semantic errors, and zero Vulkan
messages. Its Belfast capture remains finite and legible. These short Debug/readback
runs are semantic rather than performance evidence. A separate offset-shell Hero4
fixture now proves genuine crossing rather than LIFO nesting: 2,705 pixels capture
`Entry(A), Entry(B), Exit(A), Exit(B)` within 14,211 valid paired/local-color pixels,
with two resolves, zero fallback, and zero semantic/Vulkan errors. Its Belfast capture
is finite and retains both silhouettes. Deep tiers now carry a conservative Q14
remaining-transmission/open-volume state in the existing R32 identity records and
reduce only useful 16x16 tile boundaries (one Hero4 dispatch, three Cinematic8).
Later peels reject terminated tiles before material/texture evaluation. A separated
two-shell Hero4 fixture proves 7,522 early-terminated pixels and 43 occupied terminated
tiles after interface one, with interfaces two/three empty, finite local color, zero
fallback, and zero semantic/Vulkan errors. The final lifecycle gate drives both
Hero4 and Cinematic8 through two real 120-frame retirement/reactivation cycles and a
post-recovery semantic readback. It exposed and corrected a full-resolution R32 graph
alias/descriptor-placeholder layout conflict; both corrected Debug runs are
Vulkan-clean. Five independent native-4K Cinematic8 Release processes cover 50,000
measured frames at 1.680448 ms GPU-frame median-of-medians and 1.276928 ms summed
transparency-range median-of-medians. Graph and live memory are identical across
processes, with zero actual compatibility draws, rejects, preparation fallbacks,
measured topology events, profiler errors, or dropped frames. M6.6 is complete.
M6.7 is active with a backend-neutral WeightedOIT reference contract. It is
explicit-only and nonrefractive, consumes premultiplied scene-linear AP1 radiance,
uses bounded depth/coverage weights and a documented FP16 numerical envelope, and
resolves weighted average through multiplicative revealage. Its exact native-4K
logical target cost is 82,944,000 bytes per frame context with no owned depth. The
foundation changes no visible rendering or default graph memory; Vulkan execution
and particle/high-overdraw qualification remain open.
The durable post-M5 lead context for M6 is
`docs/milestones/M6-hybrid-transparency-handover-2026-08-13.md`. It records the
current two-bucket/depth-copy glass bridge, the headlamp-alpha corrective behavior,
M5.13 shadow-bias/filter changes, M5.12 reflection quality, resize/descriptor
lifetime requirements, and the recommended first-slice/qualification order.

The 2026-08-23 M6 high-fidelity asset check reported approximately 180 title-bar
FPS with three dense assets active versus approximately 1,700 FPS empty on the same
240 Hz display. This is about 5.56 versus 0.59 ms per completed frame, so the roughly
4.97 ms delta is real scene-dependent wall work rather than a 180 Hz presentation
ceiling. It remains unqualified CPU/GPU evidence because the title uses a coarse
window and mailbox acquire/present waits can still contribute. Freeze and profile
the exact three-asset scene at native 4K Release before assigning the delta to
geometry, materials, shadows, transparency, or CPU submission.
Current source creates and sorts one CPU draw packet per enabled opaque submesh,
issues direct indexed draws, has no general opaque frustum/Hi-Z path, and does not
populate the cooked LOD/meshlet section slots. Near-linear scaling with authored
primitive, triangle, complex-forward, transparent, and shadow-caster work is
therefore expected until M7/M8.

M7 now explicitly owns static/movable/animated GPU-scene update policy,
frustum/screen-error-LOD/Hi-Z visibility, compact indirect submission, a shared
main/shadow/probe visible representation, progressive texture and geometry
residency, and deterministic per-primitive child cooking/DDC reuse. Material-only
edits must not rebuild geometry or textures; one-primitive edits rebuild only that
primitive and dependent LOD/meshlet/RT children. Publication uses bounded frame
budgets and semantic texture/coarser-LOD/proxy fallbacks rather than requiring one
monolithic model upload. M8 adds per-LOD meshlet and normal-cone culling for dense
visible geometry on top of the same scene.

After the existing M0-M11 renderer program, M12 is reserved for a production
material graph/editor compiling into the shared raster/RT closure contracts, and
M13 for versioned animation assets, multithreaded/GPU skinning, and an animation
graph editor integrated with M7 visibility and M9 motion history. These are durable
roadmap commitments, not scope additions to M6.
