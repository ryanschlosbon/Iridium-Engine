# Iridium Engine Project Context

## Current direction (2026-10-06, read first)

Claude Code took over from Codex on 2026-10-02.

- **M9 temporal rendering and core post-processing is accepted** (2026-10-06,
  branch `m9-temporal`, PR #8; completion report in
  `docs/milestones/M9-temporal-and-post.md`; hand-off
  `docs/milestones/M9-to-M7.9-handoff.md`).
- **Product defaults are now** native TAA, GPU auto-exposure (the manual EV is
  compensation) and subtle bloom (4% scatter, no threshold). Measurement tools pin
  the M7R route (`--anti-aliasing none --exposure manual --bloom off`) through
  `Get-M7REngineBaseArgs`, so the frozen set (`out/m7r/captures/m9-g8` hashes) and
  older baselines stay comparable.
- **The engine now has:**
  - per-view, reset-policy graph History;
  - matrix jitter, kept out of culling, Hi-Z, shadows, probes and LOD;
  - an RG16F velocity target with previous transforms;
  - a native TAA tuned against 64-phase references in motion;
  - a revealage-alpha reactive mask;
  - histogram auto-exposure;
  - a dual-filter bloom;
  - the vendor-neutral SR input contract (`TemporalUpscaleInputs`).
- **Cost** (five-process native 4K): TAA about 0.25 ms of its 0.40 ms row; post
  (bloom, exposure, output) about 0.22 ms of its 0.50 ms row. The heaviest timing
  route is 3.95 ms GPU with everything on.
- **Next:** M7.9–M7.12 under one lead (`docs/milestones/M7-completion-task-lead-prompt.md`),
  starting with the owner performance cases (many objects, close to glass, bright
  point light), then M9c. Then M8, resumed M7.8 Virtual
  Shadow Maps, M10, **M9b** (DLSS/FSR/XeSS providers and dynamic resolution, before
  M11) and M11.
- **Watch:** `VulkanVertexBackend.cpp` is at 2,474 of its 2,500-line cap, so move code
  into feature owners before adding to it.
- Evidence tiers, the 6.94 ms budget, the M7R CPU baseline and the M9 admission are in
  `docs/performance/FRAME_BUDGET.md`.
- Third-party content is never committed; it lives in the local asset library
  (below).
- The history below is a dated record; prefer ROADMAP.md and the active plan for
  status.

## Local asset library (third-party content)

Licensed third-party test content (the Alfa and other models, HDRIs, Sponza,
`ship_in_a_bottle`, the owner's scene documents in `scenes/`) lives outside the
repository in a **local asset root**, for example `D:/IridiumAssets`, with the same
relative layout the repository's `assets/` used (`models/alfa_romeo/alfa_romeo.gltf`,
`hdri/...`, `scenes/...`). Licences allow local testing only, so it is never
committed and never copied, hard-linked or junctioned into a checkout or worktree;
every checkout reads it in place.

- **Configuration.** A gitignored `iridium.local.json` at the repository root,
  `{ "localAssetRoot": "D:/IridiumAssets" }` (format in
  `iridium.local.example.json`). A git worktree without its own file uses the main
  checkout's. The `IRIDIUM_LOCAL_ASSET_ROOT` environment variable overrides both. A
  configured root that does not exist is ignored with a diagnostic. With nothing
  configured, only `<repo>/assets` is used, exactly as before.
- **Engine.** `core/ProjectAssetRoots.h` lists the roots (`project`, plus `local`
  when configured) and resolves a root-relative path: the project copy, else the
  local copy, else the project path. Benchmark manifests stay in `assets/`; content
  they name that is missing beside the manifest resolves at the same relative
  location in the local root, with the escape check applied to that root. The
  catalog, the preparation services and the qualification background cook register
  both roots, and each record resolves under its own root.
- **Cook keys** use root-relative source and dependency locations plus content
  hashes, never absolute paths, so moving content between roots changes no cook key
  or artifact hash (verified with `Verify-CookedArtifacts.ps1 -SourceRoot`).
- **Editor.** The Asset Browser shows "Project" and "Local library" sections. Items
  keep their root for rename, delete and folder operations. Moves between roots are
  refused. Imports with nothing selected go to the local library. New scene
  documents default to `<local>/scenes`. Baked reflection probes go to the root
  that holds their scene.
- **Tools.** `tools/m7r/M7RFixtures.ps1` provides `Resolve-IridiumAssetPath` and
  `Get-IridiumLocalAssetRoot` over the same configuration. A worktree reuses the
  main checkout's cooked `out/m7r/ddc` and frozen-set baselines.

## Evidence retention and disk use (owner decision, 2026-10-07)

Qualification evidence under `out/` is gitignored working data, never the durable
record. Accepted conclusions, numbers and hashes belong in `docs/` under git.

- **While a slice is active**, its raw evidence (images, profile JSONL, logs, held
  references) may live under `out/`.
- **When a slice is accepted**, prune its evidence with `tools/Prune-Evidence.ps1
  -Summarize <dirs> -Execute`. The tool keeps every `.md` and `.txt` and every small
  `.json` (summaries, `runs.json`, `machine-state.json`, `hashes.json`, capture
  sidecars) and deletes the payloads. It refuses to run on any link and never deletes
  protected baselines, caches, builds or tools. Run it without `-Execute` first.
- **Always kept:** the current frozen baseline (`out/m7r/captures/m9-g8` until a
  successor is frozen), `out/build`, cook caches (`out/editor/model-ddc`,
  `out/m7r/ddc`, `out/m9/ddc`, `out/ddc`), the pipeline cache and `out/tools`.
- **64-sample temporal references** (`out/m9/motion/ref64`, about 6.4 GB each at 4K)
  are deleted right after the evaluation that needed them.
- **A/B baselines:** use at most one Release-only baseline worktree, created for the
  comparison and removed afterwards (`git worktree remove`, after a link scan).
  Never keep long-lived baseline worktrees; any commit can be rebuilt.
- **On 2026-10-07** the first prune removed 152.7 GB of pre-M7-completion raw
  evidence and four old baseline worktrees (11 GB), with owner approval.

## Why this document exists

This is the compact handoff for new lead tasks. It records facts found during the post-RHI-refactor architecture review, accepted direction, and unresolved choices. It is not a substitute for reading current source, the roadmap, milestone plans, and ADRs.

## Product intent

Iridium is intended to be a visually ambitious, high-end engine rather than a lowest-common-denominator renderer. The reference PC is an RTX 4090, Core i9-14900K, 64 GB DDR5-6000, a 2 TB Samsung 990 Pro, and a 4K HDR display. Since 2026-10-02 the raster target is 144 FPS at native 4K without ray tracing (6.94 ms base render, native temporal AA); with hybrid ray tracing, temporal reconstruction may be used while holding 144 FPS displayed. Visual fidelity comparable to UE5/Frostbite/Anvil/Northlight is the primary bar.

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
- M7.6 accepts a workload-selectable previous-frame Hi-Z route. Per-view persistent
  R32 histories are built in place after fused GPU-scene compaction consumes the
  preceding contents. Camera/depth-content/resize changes and small or uncertain
  bounds fail visible. A controlled native-4K depth stack removes 79.4090% of
  opaque commands and reduces complete GPU median by 0.661920 ms with identical
  scene bytes. Qualification-only CPU/device oracles remain available under
  validation but are absent from the deployable path; global default remains off.
- M7.7 is accepted and M7.8 is active. M7.7's first conventional-shadow slice caps directional coverage by
  project/profile policy instead of a distant camera far plane, retains stable
  practical/log splits, reports per-cascade world units per texel, reconstructs
  PCSS filter comparisons bilinearly, and makes the Cinematic project tier
  reachable. Its second accepted slice adds bounded receiver-plane correction and
  a geometric-normal offset in cascade world-texel units. A matched grazing fixture
  removes severe self-shadow striping without increasing global raster bias or
  changing measured deferred-lighting median cost. Its third accepted slice builds
  a conservative caster set per refreshed directional cascade before Vulkan draw
  recording; a moving-grid fixture rejects 162/192 caster-cascade pairs while the
  established contact capture remains byte-identical. A follow-up replaces the
  global directional caster cache key with four conservative revisions; an
  enter-volume transition refreshes only two affected cascades and leaves two
  cached/sampleable. Its fourth accepted slice replaces copied persistent caster
  packets with compact, camera-independent GPU-scene primitive references for
  directional, spot, and point shadows. The 48-primitive qualification scene uses
  192 bytes of submission references, zero fallback packets, and preserves the
  accepted contact image byte-for-byte. Its fifth accepted slice builds
  directional-cascade commands on the device; all 30 qualification commands match
  the CPU oracle with zero overflow, and paired direct/indirect captures are byte-
  identical. Its sixth accepted correctness slice extends bounded device commands
  to spot-atlas tiles and point-cube faces; 9 spot and 90 point-face commands match
  their independent oracles with zero mismatch/overflow, and paired captures are
  byte-identical. The follow-up completes oracle gating: production retains exact
  delayed device counts/overflow checks but performs no CPU light-view tests for
  wholly GPU-owned casters; validation or the explicit qualification switch
  restores exact CPU comparison. Reflection capture now has its own compact probe-
  consumer submission: the nine-primitive qualification uses 36 bytes instead of
  2,160, self-excludes 18 owned primitive-face pairs, and processes the remaining
  36 tests with zero invalid references. It matches the direct capture byte-for-
  byte. Compact capture shading now resolves persistent transforms and materials
  through a dedicated GPU-scene graphics pipeline; its isolated probe-only A/B
  retains all 33 draws within one R16 scene-linear quantization step of the direct
  path. Reflection capture now owns bounded device command storage; compute performs
  conservative cube-face tests and stable-owner exclusion before indirect-count
  submission. All 33 device commands match the CPU oracle with zero overflow, the
  deployable route performs no GPU-owned CPU face tests, and its capture is unchanged.
  The later reflection-sensitive warmed gate repairs multi-frame realtime capture
  ticket progress and proves byte-identical 4K output at a 16-pixel probe bound,
  but removes only 0.9263% of retained capture triangles with no repeatable
  performance or memory win. That gate is complete without production admission:
  probe LOD remains experimental/default-off and LOD0 remains global. Conventional directional/spot/point
  device-command submission has separately passed its warmed heterogeneous gate.
  Directional-shadow command construction now also owns an independent density-
  based LOD selector driven by each cascade's measured world-units-per-texel. The
  dense near/mid/far correctness fixture reduces 21 of 1,243 commands and 13,524
  triangles at a 2-texel bound with exact command-oracle agreement and a byte-
  identical LOD0 capture. Production performs no CPU caster tests or shadow-LOD
  oracle work. The later native-4K warmed gate rejects production admission after
  only a 1.2684% combined triangle reduction and no repeatable complete-path win,
  so the global default remains LOD0. See
  `docs/performance/M7.7-directional-shadow-lod-2026-09-14.md`.
  Point-cube commands now also use a face-invariant radial shadow-texel selector.
  A 2-texel dense-car checkpoint reduces 16 of 565 commands and 11,436 triangles
  with exact command agreement, zero overflow, and a maximum scene-linear
  difference of `5.96046448e-7`. The Release route records zero CPU point-caster
  tests and oracle work. See
  `docs/performance/M7.7-point-shadow-radial-lod-2026-09-14.md`.
  Spotlight commands now evaluate the cooked error bound in the authored light
  projection and atlas-tile texel density. The 2-texel checkpoint reduces 10 of
  339 commands and 6,376 triangles with exact command agreement and a byte-
  identical LOD0 capture. Production records zero CPU spotlight visibility/LOD
  work. See
  `docs/performance/M7.7-spot-shadow-projected-lod-2026-09-14.md`.

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
does not require exclusive fullscreen. ADR-0013 post-M6 hardening now also exposes
Auto/SDR/scRGB/HDR10 as a live Project Settings selector. It preserves resident scene
assets while rebuilding only presentation-dependent resources at a frame boundary;
Auto prefers scRGB, then HDR10, then SDR, and the ACES LUT follows the effective
transport after negotiation or fallback.

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
M6.7 subsequently completed its backend-neutral WeightedOIT reference contract and conditional
Vulkan graph storage. It is explicit-only and nonrefractive, consumes premultiplied
scene-linear AP1 radiance, uses bounded depth/coverage weights and a documented FP16
numerical envelope, and resolves weighted average through multiplicative revealage.
Its opt-in graph products are one full-resolution `RGBA16F` accumulation image and
one `R16F` revealage image per frame context, exactly 82,944,000 logical native-4K
bytes with no owned depth. Active passes read opaque depth without writing it and sit
after foreground transparency but before bloom; frame targets acquire both images.
The live backend now prewarms or demand-enables this topology and retires it after
120 inactive frames through the existing rollback-safe transparency rebuild. Explicit
WeightedOIT packets bypass refraction pyramids and legacy glass. When resident they
use additive FP16 weighted-color accumulation, multiplicative FP16 revealage, and a
premultiplied fullscreen resolve into scene HDR; only the first dynamic-demand frame
uses deterministic sorted fallback while topology activates. Default scene output
and default graph memory remain unchanged. The tracked cooked
`weighted_oit_particles_v1` fixture now covers 256 explicit emissive-16 particles in
an 8x8x4 overlap grid. A corrected 1280x720 Debug run is Vulkan-clean with exactly
256 accumulation draws, one resolve, zero sorted/compatibility draws, zero refraction
or layered residency, and no profiler drops. The fixture exposed a graph-alias layout
conflict in unreachable refraction descriptors, so WeightedOIT now compiles the
non-applicable refraction transport and bindings out. Its pre-output scene-linear PFM
at emissive 16 contains 921,600 finite pixels, zero nonfinite pixels, 142,884 active
pixels, and a maximum AP1 component of 10.21875. A boundary fixture at emissive 256
also cooks cleanly through importer v6 and produces
921,600 finite pixels, zero nonfinite or FP16-limit pixels, identical active coverage,
and a maximum AP1 component of 163.5. A single-process 300-frame native-4K Release
baseline measures 4.005 ms CPU, 3.185 ms total GPU, 2.764 ms OIT
accumulation, and 0.020 ms resolve medians. This is populated-path evidence, not final
particle-system acceptance. An allocation-free seeded permutation control preserves
production order at seed zero and exports the active seed in profiler evidence.
Sixty-four independent validation processes have identical coverage and exact
256/1/0 accumulation/resolve/fallback routing; all distinct scene-linear hashes stay
within 0.046875 absolute AP1, 0.001718 RMSE, and 0.4587% relative error versus seed
zero, closing the draw-order gate. The 4096/65536 tiers now publish one runtime
entity and one backend-neutral transform-range packet each. At the 65,536 production
bound, Debug validation uploads exactly 4 MiB, records one instanced draw, reports
zero fallback, and reduces extraction/sort from 567.681/389.941 ms to
0.261/0.0009 ms. Its finite scene-linear result remains within the frozen order-error
envelope versus the explicit-packet baseline. This closes the high-overdraw gate;
the populated path also survives three deterministic scene-target resizes with a
byte-identical restored capture, and separate SDR/scRGB/HDR10 processes preserve
exact routing with fully finite HDR output. Multi-process native-4K Release
qualification passes across five processes and 50,000 measured frames:
GPU-frame/accumulation/resolve median-of-medians are 3.182048/2.742272/0.020480 ms,
with exact one-draw routing, identical memory, and no profiler loss. M6.7 is complete.
M6.8 editor controls, diagnostics, and allocation hardening are complete. A
backend-neutral compiled-policy diagnostic now exposes resolved route, topology
state, fallback cause, and sanitization in headless snapshots, Material Diagnostics,
and Asset Browser. Viewport/CLI Transparency Class and Transparency Fallback modes
use per-view transport across opaque, Ordinary2/deep layered, and WeightedOIT paths;
validation captures prove the expected class/fallback palettes without changing the
final render path. Opaque cooked model primitives present a disabled compact result
with transparency controls collapsed until an explicit stable-GUID override is
checked; missing cook evidence never hides controls. The Profiler groups the complete
transparency counter family, highlights nonzero fallback/rejection/overflow signals,
and enumerates active `gpu.transparency.*` ranges dynamically. Measured interval,
selected pyramid mip, retained layer count, and overflow/saturation views are now
Vulkan-validated across Ordinary2, Cinematic8, and WeightedOIT. Exact pixel evidence
matches independent deep-layer readback, and the diagnostic paths add no images,
descriptors, or steady allocations. Persistent transform and shadow-mapping scratch,
sorted owner validation, and static deep-pass identities reduce the inherited
17-call/5,216-byte baseline to exact zero across 96 measured Release frames covering
Ordinary2, Cinematic8, and WeightedOIT. The Vulkan validation capture is byte-identical
to its pre-hardening reference, with the same 650 residual-tail pixels and no validation
messages. M6.8 is complete. Asset import and
transparency-policy edits now have an independent backend-neutral transaction history:
apply/undo/redo each recook, history advances only after successful catalog completion,
failed jobs do not move its cursor, divergence is rejected, and scene document state
is unchanged. A real headless glTF apply/undo/redo test verifies sidecar persistence
and stable root/primitive identity.
M6.9 cutover now makes classified hybrid execution the production default for fresh
schema-2 glTF imports and uninitialized cooked/runtime/draw records. Schema-1 retains
its frozen legacy migration and historical settings/CookKeys remain readable, but the
production runtime ignores serialized architecture selection. Artists see Classified
hybrid as a fixed production renderer; `--developer-legacy-transparency` is the sole
developer A/B override. It is applied before complete cooked-model publication so
geometry routing, material feature flags and variants, shaders, passes, blend, and
depth state change together. Vulkan validation of the same classified Ordinary2
artifact proves the default exact one-draw entry/exit/composition/resolve path with
5,888 paired pixels and a forced legacy one-depth/one-forward control with zero
Ordinary2 work. Their final-SDR hashes differ. The production graph now omits the
legacy `depth.glass` image and all four two-bucket passes; its base topology is
19 passes, 26 logical resources, and 19 physical slots versus 22/27/20 under the
developer override. The active 1280x720 Ordinary2 graph saves exactly 7,372,800
requested and 7,864,320 committed bytes with byte-identical output. Rejected
classified topology retains one direct compatibility-forward fallback with no depth
prepass or legacy bucket. M6.9 final production qualification passes 71/71 tests in
both Debug and Release. Five native-4K Ordinary2 processes cover 50,000 measured
frames at 0.720192 ms GPU median-of-medians with zero retained-frame allocations or
profiler loss; separate Cinematic8 and WeightedOIT profiles measure 1.612576 and
2.657440 ms. Resize/lifecycle and SDR/scRGB/HDR10 validation are clean, and the Alfa
capture is byte-identical to the accepted pre-cutover result. M6 is complete; see
`docs/milestones/M6-acceptance-report-2026-08-27.md`.
M6.10 then closes the editor workflow while keeping accepted benchmark output
grid-free:
viewport mesh picking is restored; the default adaptive metric grid, world SI unit,
independent transform snaps with Ctrl bypass, and world/local gizmo space live in an
editor-only settings record. Inspector positions and light distances follow the world
unit, light intensity is uncapped with lux/kilolux and candela/lumen presentation, and
components/properties are searchable. Asset folders support grid/tree nesting and a
persistent custom tree order, while model subassets expand into an attached horizontal
thumbnail strip. A benchmark metadata fixture was renamed away from `.iridium.meta`,
removing six false live sidecars and leaving the asset catalog with zero duplicate
GUID diagnostics. The world grid subsequently moved from finite ImGui lines to a
procedural output-overlay contract: it is unbounded, derivative-filtered, fades at
grazing/horizon angles, and uses the production opaque depth attachment so foreground
geometry occludes it. Benchmark, asset-preview, and capture output keep the grid off.
Asset Browser model drops into the viewport are grounded at world `Y = 0`, including
parallel/above-horizon ray fallback. Contextual shortcuts now use semantic modifier
groups: Alt owns viewport presentation/tools, beginning with **Alt+G** for grid
visibility. The binding is focused-viewport-only and yields to text input, active
widgets, gizmo manipulation, and popups.
Earlier M6.7 default-topology qualification measured
0.922 ms CPU and 0.460 ms GPU medians with zero OIT work. That qualification run
published 207 counters; the always-emitted order-seed evidence counter brings the
current default to 208. The profiler's bounded capacity is now 256, removing the
observed 15-counter loss without adding steady-frame allocation.
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
Current source publishes persistent generation-safe GPU-scene records, performs
device-compute main-view frustum compaction into indirect commands, and can populate
parent-contained bounded-error LOD chains behind explicit experimental importer and
runtime settings. The representative topology-transactional cook covers 289,619 of
301,966 opaque triangles and reduces native-4K lit main-view triangles by 7.7677% at
the 2-pixel candidate ceiling with exact device/oracle selection. Default rendering
remains LOD0. The expanded near/mid/far and 240-frame motion/cut gates are
validation-clean and temporally stable. Exact CPU LOD command reconstruction now runs
only under validation or an explicit qualification switch, while the GPU selector
reuses invariant bounds projection across each chain. This repairs the initial 1.5206%
native-4K wall regression to effective parity (+0.0378%) at 2.8282% triangle
reduction, but complete GPU median remains 0.5642% higher and candidate tails regress
on that small workload. A qualification-only physical residency floor now
removes fine index ranges before upload and rebinds the canonical primitive identity
to the first retained coarser range. The native-4K floor-one gate withholds 1,737,714
bytes across 70 chains, retains 303 commands with zero fallback or mismatch, and
changes 0.143856% of the LOD0 image at 0.9999913623 mean luma SSIM. The frozen
256-instance stress workload admits generated main-view LOD as workload-selectable:
2-pixel selection removes 24.5102% of 51,612,672 triangles, improves median complete-
frame wall time by 0.310995 ms (1.4487%), improves CPU/GPU medians and tails, and
retains identical memory plus exact validation/oracle results. One of five matched
pairs reverses, and the small-workload control does not win, so global rendering
remains LOD0 rather than forcing generated LOD on every scene. M7.5, M7.6, and M7.7
are accepted; M7.8 is active. The parent vertex stream remains resident; independent
vertex/index child CookKeys, asynchronous streaming, and shadow/probe visibility
remain later M7 work. M8 meshlet sections are not yet populated. See
`docs/performance/M7.5-lod-oracle-separation-2026-09-02.md`,
`docs/performance/M7.5-physical-coarser-residency-2026-09-02.md`, and
`docs/performance/M7.5-material-workload-admission-2026-09-02.md`.
M7.6 now has a backend-neutral conservative depth-pyramid oracle, an opt-in live
Vulkan farthest-depth build, persistent scene/asset-view histories, exact live
source/history readback, and versioned device query shaders. Forward/reverse
reduction, odd-extent coverage and bounded 2x2 sampling match across 3,640 hardware
queries. Complete deferred-opaque plus depth-writing-forward ownership is computed
before opaque compaction. The additional default-off live-query path records
requested/projected/tested/would-reject/invalid/fail-visible diagnostics but still
submits every draw. Its first synchronized dense run tests 288 candidates, reports
34 `would_reject`, retains 15 projection failures, and has zero invalid results per
frame. A second qualification shader now projects directly from live GPU-scene
tables and checks every fence-delayed verdict against the independently CPU-projected
safety route. Overlap/cut, off-frustum, and output-transport rebuild runs have zero
invalid records and zero unsafe mismatches; conservative GPU margins intentionally
retain more marginal candidates. A separately explicit rejection variant now consumes
only valid matching verdicts during opaque compaction. It removes 73 of 404 pre-cut
commands and 42 of 404 post-cut commands in the frozen temporal fixture, fails visible
on the cut, preserves current-build image bytes, and maintains exact per-bin oracle
parity. Off-frustum and output-transport rebuild runs are exact. A follow-up fuses
projection/query/result emission with compaction, shares one implementation with the
standalone oracle, and uses a sampler-free base pipeline whenever history is invalid.
The second dispatch and barrier are gone. This is still not production admission:
fused local work is about 0.0025–0.0047 ms above base compaction plus query in the
current small fixture, and the 4K pairs retain unrelated pass/GPU-state variance.
Selected moving-occluder, moving-occludee, and disocclusion fixtures now reject
history exactly once as `DepthContentChanged`, recover on the next frame, preserve
byte-identical query/rejection captures, and maintain exact device/oracle parity.
The subpixel fixture now retains 101/101 commands as exact `SmallBounds` fail-visible
results, and a selected visibility step rejects stale ownership as
`DepthContentChanged`, retains the full
transition set, and recovers on the next frame. Three scene-target resize/rebuilds
also invalidate history, retain all transition work, recover on their following
frames, and restore 1280x720. The later in-place-history refactor removes the two
frame-local build chains and full-mip publication copy while retaining independent
scene/asset histories, exact capture/oracle parity, and resize recovery. It saves
9.374718 MiB requested at 1280x720 and 84.373894 MiB requested at native 4K. The
final controlled native-4K depth stack removes 5,133 of 6,464 commands and improves
complete GPU median by 0.661920 ms with byte-identical captures. M7.6 is accepted
as workload-selectable; global production rejection remains off. See
`docs/performance/M7.6-depth-pyramid-contract-2026-09-02.md`,
`docs/performance/M7.6-live-query-qualification-2026-09-10.md`,
`docs/performance/M7.6-device-owned-projection-qualification-2026-09-11.md`, and
`docs/performance/M7.6-experimental-command-rejection-2026-09-11.md`, followed by
`docs/performance/M7.6-fused-query-compaction-2026-09-11.md`,
`docs/performance/M7.6-moving-occluder-disocclusion-2026-09-11.md`,
`docs/performance/M7.6-small-object-depth-content-2026-09-11.md` and
`docs/performance/M7.6-resize-recovery-2026-09-11.md`, followed by
`docs/performance/M7.6-in-place-view-history-2026-09-11.md`, and
`docs/performance/M7.6-large-occluded-admission-2026-09-12.md`.
M7.7's first conventional-shadow slice adds project/profile-owned directional
coverage, exact practical/log split and world-density diagnostics, bilinearly
reconstructed PCSS filter comparisons, and a reachable Cinematic project tier.
The second slice adds bounded receiver-plane/geometric-normal bias with exact
profile and capture provenance. The third adds camera-independent, conservative
per-cascade caster visibility with exact tested/culled telemetry. See
`docs/performance/M7.7-directional-shadow-density-and-filter-reconstruction-2026-09-12.md`
and
`docs/performance/M7.7-directional-shadow-receiver-bias-2026-09-12.md` and
`docs/performance/M7.7-directional-shadow-caster-visibility-2026-09-12.md`.
The fourth slice publishes per-primitive shadow/probe consumer masks and feeds
directional, spot, and point passes through compact GPU-scene references with an
explicit direct fallback. See
`docs/performance/M7.7-gpu-scene-shadow-submission-2026-09-13.md`.
The fifth slice uses those references to build and submit directional-cascade
indexed commands on the device. Fence-delayed device counts match the independent
CPU oracle and direct/indirect final images are byte-identical. The sixth slice
extends the same bounded ABI to spot-atlas tiles and point-cube faces: 9 spot
commands and 90 point-face commands match the CPU oracle with zero mismatch or
overflow, and paired captures are byte-identical. A follow-up makes CPU comparison
qualification-only while retaining
exact delayed device counts and overflow checks. See
`docs/performance/M7.7-directional-shadow-device-commands-2026-09-13.md` and
`docs/performance/M7.7-local-shadow-device-commands-2026-09-13.md` and
`docs/performance/M7.7-shadow-command-oracle-gating-2026-09-13.md`, followed by
`docs/performance/M7.7-independent-probe-visibility-2026-09-13.md`.
Reflection capture also has an independent, default-off radial LOD selector whose
decision is identical across all six faces. Host validation limits chains to
resident buffer-compatible prefixes; a 2-pixel correctness run reduces 7 of 226
commands and 4,508 triangles with exact device/oracle agreement and a byte-
identical LOD0 scene capture. See
`docs/performance/M7.7-probe-radial-lod-2026-09-14.md`.
Directional cascade LOD now independently converts cooked error into shadow texels
using per-cascade world density. A 2-texel correctness run reduces 21 of 1,243
commands and 13,524 triangles with exact device/oracle agreement and a byte-
identical LOD0 capture; production records no CPU caster tests or LOD oracle work.
See `docs/performance/M7.7-directional-shadow-lod-2026-09-14.md`.
Point-cube LOD uses a face-invariant radial projection bound shared by all six
faces. The 2-texel correctness fixture reduces 16 of 565 commands and 11,436
triangles with exact device/oracle agreement and no pixel exceeding `1e-6` AP1
difference from LOD0; production records zero CPU point-face visibility/LOD work.
See `docs/performance/M7.7-point-shadow-radial-lod-2026-09-14.md`.
Spotlight LOD uses the authored light clip transform and atlas-tile resolution to
evaluate the same conservative perspective error bound used by main-view LOD. The
2-texel correctness fixture reduces 10 of 339 commands and 6,376 triangles with
exact device/oracle agreement and a byte-identical LOD0 capture; production records
zero CPU spotlight visibility/LOD work. See
`docs/performance/M7.7-spot-shadow-projected-lod-2026-09-14.md`.
The subsequent native-4K moving heterogeneous gate admits conventional
directional/spot/point device-command submission. Across reversed-order pairs it
reduces median combined CPU shadow recording by 70.1-70.9%, slightly lowers
combined GPU shadow work, and preserves byte-identical final-SDR output. See
`docs/performance/M7.7-heterogeneous-shadow-admission-2026-09-19.md`. Shadow and
probe LOD remain default-off. The probe gate is complete without admission after
its reflection-sensitive warmed run; see
`docs/performance/M7.7-probe-lod-warmed-admission-2026-09-24.md`. The conventional
shadow-LOD gate is likewise complete without admission: eight texels remove only
1.2684% of combined triangles, save 0.007-0.011 ms of shadow GPU work, regress CPU
recording, and do not produce repeatable whole-frame ordering. LOD0 remains default;
see `docs/performance/M7.7-shadow-lod-warmed-admission-2026-09-29.md`.
The next host-overhead checkpoint publishes stable shadow/probe consumer membership
with the GPU scene and caches directional/spot/point geometry-material bins across
transform-only frames. Per-light visibility and LOD remain device-owned. The
Release smoke hits all three caches without fallback; see
`docs/performance/M7.7-shadow-membership-cache-2026-09-29.md`.
Opaque conventional-shadow raster now uses position-only vertex shaders and a
single-attribute Vulkan layout; alpha masks keep the full opacity input set. This
retains the canonical interleaved geometry store while saving 0.082-0.088 ms of
combined shadow raster time in reversed-order native-4K pairs with byte-identical
output. See
`docs/performance/M7.7-opaque-shadow-position-fetch-2026-09-29.md`.
The final moving-camera contact fixture closes the remaining M7.7 quality matrix:
thin, fully clipped alpha-mask, double-sided, close-contact, grazing, and cascade-
density transition coverage has exact device/oracle commands, zero overflow or
fallback, and byte-identical direct/automatic native-4K captures at both sweep
extremes. See
`docs/performance/M7.7-shadow-quality-closure-2026-09-29.md`. M7.8 now owns the
sparse Virtual Shadow Map production candidate while conventional maps remain the
selectable reference and fallback.
M7.8 now begins with a backend-neutral sparse residency ABI and deterministic CPU
planner covering stable page identity, separate static/dynamic cache validity,
bounded priority allocation, requested-page protection, age/priority eviction,
pending-raster publication, and safe missing-page fallback. Vulkan virtual storage
is now available behind a default-off qualification switch: a 1,024-page D32 atlas
and 65,536-entry flat table use separate persistent memory categories. Two aligned
device-local frame-owned compute working sets now add 17,042,496 bytes, bringing the
validated enabled total to exactly 88.753 MiB; the disabled path remains zero.
Sampling remains disabled; conventional
shadows are unchanged. ABI v2 also defines deterministic directional receiver
marking and signed world-page coordinates with snapped per-level origins, preserving
cache identity across camera-relative clipmap scrolling. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`.
The raw Vulkan receiver-marking compute stage now matches this oracle in Debug and
Release RTX 4090 hardware qualification. It emits explicit mapped, unmapped,
invalid, and address-overflow states. The following single-invocation GPU reference
compactor matches CPU deduplication, deterministic ranking, bounded overflow,
dropped coverage, saturation, and malformed-mark telemetry in both configurations.
It freezes behavior rather than scalability; runtime parallel compaction, page
raster, and sampling remain disabled.
The first parallel sort/reduce/sort prototype is correctness-clean at 4,096
receivers and the 65,536-mark capacity, but is rejected: its 272 full-capacity
bitonic compare/exchange dispatches measure 841.992 ms Debug and 837.758-844.757 ms
across repeated Release medians on the RTX 4090. It is qualification evidence only,
and its scratch layout
is deliberately not public RHI ABI. M7.8 next requires a hierarchical/radix or
bounded top-K replacement before runtime integration.
The hierarchical replacement now passes that standalone gate. It uses 256-entry
shared-memory block sorts, eight global merges per 65,536-entry ordering, and a
two-stage saturated dropped-coverage reduction. Exact oracle parity is retained,
while device-local compute falls to 0.145792/0.145984 ms Debug/Release median at
65,536 unique marks and a 4,096-request cap on the RTX 4090. Upload/readback and host
allocation are outside the timestamps. Persistent buffers now exist behind the
default-off resource switch. That owner now also creates immutable per-frame
descriptors and both compute pipelines. The shared direct marker-to-compactor
operation passes exact two-slot populated/empty reuse with synchronization
validation at 0.148512/0.147264 ms Debug/Release for 65,536 receivers. Live depth-
driven receiver/clip publication and integrated frame evidence remain next. See
`docs/performance/M7.8-persistent-mark-compact-chain-2026-09-30.md`.
The persistent owner now also initializes a sampled-depth receiver producer. Its
explicit pixel-region contract reconstructs every pixel, rejects over-capacity
regions, and suppresses clear/invalid depth and unsafe homogeneous division. Real
D32 reconstruction plus marking/compaction matches the oracle for odd offset
regions, forward/reverse depth and perspective/singular cameras. Full-view page
demand accumulation, live clip publication and render-graph scheduling remain open.
See `docs/performance/M7.8-depth-receiver-producer-2026-09-30.md`.

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

The 2026-09-04 review corrected false occlusion at odd extents: queries now invert
each mip's integer reduction partition recursively. Exhaustive hole/rectangle tests
pass for forward and reverse depth. Vulkan must match the corrected oracle; see
`docs/performance/M7.6-odd-extent-query-correction-2026-09-04.md`.

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
