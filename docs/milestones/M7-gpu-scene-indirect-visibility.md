# M7 GPU Scene, Indirect Visibility, and Scalable Shadows Execution Plan

## Header

**2026-10-07 — M7 completion:** M7.9–M7.12 execute under the focused plan
`docs/milestones/M7-completion.md` (branch `m7-completion`). It supersedes this plan's
M7.9–M7.12 slice text and records the 2026-10-07 audit corrections. This plan keeps
the candidate register, invariants, M7.0–M7.8 history and decision log.

2026-09-05 owner-requested material authoring/viewer work is in progress alongside
M7. See `docs/milestones/Material-authoring-and-asset-viewer.md` for the implemented
sparse source-material overrides and primitive viewer slice, and the explicitly
open live-preview, independent-view, texture and material-instance requirements.

2026-09-04 Porsche transparency follow-up: two local lamp source materials now
author dielectric metallic factors; the 930 refraction lens also has a neutral
tint. Mixed SortedSurface/ThinGlass cover ordering and grazing-angle reflection
remain open fidelity cases, not solved by quality or atlas priority. See
`docs/performance/M7-porsche-transparency-triage-2026-09-04.md` for exact edits,
validation evidence, and the required mixed-class composition regression slice.

- **Milestone:** M7 — GPU Scene and Indirect Visibility
- **Status:** In Progress — **paused 2026-10-02 at the M7.8 checkpoint** for M7R
  consolidation and the M9 pull-forward; M7.0-M7.7 accepted; LOD and Hi-Z
  workload-selectable; M7.8 resumes after M8, M7.9-M7.12 after M9
- **Lead:** fresh M7 milestone-lead task; one integration owner for GPU-scene,
  visibility, geometry, residency, and shadow-path coherence
- **Last updated:** 2026-10-02
- **Reference system:** RTX 4090, Core i9-14900K, 64 GB DDR5-6000, fast NVMe,
  native or temporally reconstructed 4K HDR
- **Target:** more than 100 FPS in fully dressed active gameplay scenes while
  retaining excellent material, geometric, shadow, transparency, and HDR fidelity
- **Dependencies:** accepted M0-M6 and the post-M6.11 display/editor hardening
- **Entry handoff:** `docs/milestones/M6-to-M7-handoff-2026-08-28.md`
- **Performance contract:** `docs/performance/FRAME_BUDGET.md` and
  `docs/performance/M7-performance-fidelity-contract.md`
- **Governing architecture:** ADR-0001 through ADR-0014, with particular M7
  relevance in ADR-0003, ADR-0004, ADR-0006, ADR-0008 through ADR-0011, and
  ADR-0013's live output-lifetime requirements and ADR-0014's generation-safe
  GPU-scene publication contract
- **Source baseline:** `5a9f705688829f61a5b12eda2eb36d14a15bbf15` plus the
  deliberately dirty accepted-M6/post-M6 worktree classified in
  `docs/performance/M7.0-independent-audit-2026-08-28.md`

The owner has directed M7 to optimize for high-performance computers, pursue as
much useful performance as possible, reject meaningful visual-fidelity loss, remain
future-facing, and retain workload/capability choices instead of locking every scene
to one technique. This document translates that direction into evidence gates; it
does not pre-approve an experiment that has not been measured.

## Objective and user-visible outcome

M7 replaces per-frame, per-submesh CPU draw construction with a persistent,
identity-safe GPU scene. CPU work becomes compact changed-record publication. The
GPU conservatively determines visible instances and primitives, selects bounded-
error LODs, rejects off-frustum and occluded work, builds compact indirect commands,
and creates independent visibility products for main, shadow, and reflection views.

When M7 is accepted:

- adding dense assets no longer scales CPU submission linearly with all authored
  submeshes;
- unchanged static content generates no repeated transform/instance upload;
- off-screen, safely occluded, and sub-pixel detail stops consuming main-view
  geometry work;
- hero content retains explicit high-fidelity overrides;
- shadows use the hardened conventional path or a measured sparse Virtual Shadow
  Map path according to quality, workload, and capability;
- oversized assets cook and become resident progressively without one monolithic
  upload hitch or mixed revision;
- standard opaque surfaces may use a measured indexed visibility/material-resolve
  path, while conventional indexed packed deferred remains available;
- M8 meshlets, M9 motion/reconstruction, M10 GI, and M11 ray tracing can consume the
  same records rather than creating incompatible scene databases.

## Current context to revalidate

The M6 handoff and source inspected on 2026-08-28 indicate:

- every enabled opaque submesh becomes a CPU `DrawPacket`;
- `DrawPacket` is currently 240 bytes and mixes hot draw data with cold identity,
  transparency, selection, and diagnostic state;
- queues are sorted and most opaque submeshes produce individual
  `vkCmdDrawIndexed` calls;
- full packets are copied into shadow/selection-related work;
- material overrides are searched during per-frame model extraction;
- normal opaque work computes data such as camera distance that it does not need;
- there is no general production opaque frustum or Hi-Z culling;
- cooked LOD and meshlet sections are not populated;
- the standard vertex is 72 bytes and model publication uses UInt32 indices even
  when UInt16 would suffice;
- current main, eligible shadow, and probe work can receive full authored primitive
  detail;
- the cluster grid uses a multi-pass count/scan/fill/sort/finalize path and its M5
  dressed cost was about 1.93 ms before later renderer hardening;
- repeated view/projection inverse work and remaining general-scene allocations are
  visible cleanup candidates;
- M6 classified transparency itself reached zero steady allocations, while its
  final Alfa scene retained 16 general renderer/light/editor calls and 1,912 bytes;
- the owner observed approximately 180 FPS with three dense high-fidelity assets
  versus approximately 1,700 FPS empty, but no frozen attribution exists.

The lead must re-open current files and measure before treating any item as a
bottleneck. Documentation describes direction; current source and matched evidence
decide implementation.

## Invariants

### Identity, update, and publication

- Persistent entity identity is a scene UUID. Persistent asset, model, material,
  texture, and primitive identity is an asset/subasset GUID.
- `sourcePrimitiveGuid` and `primitiveGuid` remain distinct and survive GPU-scene,
  LOD, visibility, shadow, capture, diagnostics, and editor selection paths.
- GPU slots, device addresses, vector indices, indirect offsets, page indices, ECS
  indices, and pointers are transient implementation details, never persistent
  identity.
- Material-only edits do not mutate geometry identity or recook geometry. A
  one-primitive edit does not rebuild unrelated children.
- Publication remains newest-complete-revision only. The last complete usable
  revision stays active until its replacement is ready.
- Uncertain culling or incomplete residency fails visible or selects an explicit
  valid proxy/coarser product. It never silently drops the object.

### Rendering and fidelity

- All material, lighting, reflection, transmission, emissive, transparency, and
  probe work remains scene-linear ACEScg/AP1 until the single M1 output boundary.
- M2 packed materials and shared BSDF/normal conventions remain the shading truth.
- Deferred, material-resolve, complex-forward, and transparent consumers reuse the
  one M5 clustered-light representation and independent per-light shadows.
- M6 classified transparency remains in its existing forward/ordered/OIT paths.
  M7 visibility-buffer work starts with standard opaque surfaces only.
- Main-camera visibility is never reused as shadow or probe visibility. Consumers
  share records, not an incorrect culling decision.
- Geometric normals, shading normals, transport thickness/depth, and shadow bias
  remain separate.
- Optimizations may reduce invisible or sub-pixel work. They may not trade away a
  meaningful silhouette, stable material response, transparent ordering, shadow
  caster, reflection participant, HDR highlight, or temporal result.
- Fidelity-sensitive representations retain a high-precision or conventional
  fallback and explicit quality/content policy.

### Architecture and lifetime

- Backend-neutral scene, RHI, visibility, and shadow contracts remain outside the
  Vulkan implementation. Vulkan owns formats, descriptor layouts, page-table
  representation, barriers, queues, workgroup/subgroup choices, and device limits.
- Render-graph passes declare access, lifetime, and subresources. No second manual
  renderer or hidden shadow scheduler is introduced.
- Resize, zero extent, graph topology changes, capture insertion, frame-context
  retirement, and live SDR/scRGB/HDR10 switching remain safe.
- Runtime code does not parse source glTF/JSON/images, depend on SQLite/importers,
  or store editor/ImGui behavior.
- The M7 scene representation remains usable by M8 indexed and mesh-shader emission,
  M9 motion history, M11 TLAS updates, and later animation. No indexed-only identity
  model is allowed.

### Measurement

- Release performance decisions use matched native-4K scenes and report median,
  p95, and p99, with presentation waits separated.
- CPU/GPU time, memory, latency, temporal stability, and image quality are all
  acceptance results.
- The 10 ms GPU and less-than-4 ms simulation-plus-render-preparation targets remain
  program goals, not permission to consume unused margin without benefit.
- Steady C++ allocation should be zero in representative runtime paths.

## Scope

### Required production foundations

- frozen M7 benchmark fixtures and missing counters;
- persistent GPU scene with compact dirty updates;
- explicit Static, Movable, and Animated update policies;
- hot/cold data separation and stable CPU identity-to-GPU-handle mapping;
- geometry arenas compatible with indexed indirect-count submission;
- conservative instance/primitive frustum visibility;
- deterministic screen-space-error LOD with hysteresis and hero overrides;
- visible-list compaction and indirect command generation;
- temporally conservative Hi-Z occlusion;
- independent main, directional/spot/point shadow, and probe visibility;
- lossless geometry/index locality optimization and eligible UInt16 indices;
- progressive model-child residency and fine-grained deterministic cooking;
- conventional-shadow integration with GPU-scene visibility;
- production diagnostics, safe fallback, and capability/quality selection.

### Required measured candidates

- high-quality compact vertex formats;
- position-only/opacity-UV shadow streams and shadow-specific indices/LOD;
- sparse Virtual Shadow Maps for directional and local lights;
- cluster-assignment reduction and reuse;
- indexed visibility/material resolve for standard opaque surfaces;
- targeted CPU SIMD, including a DirectXMath comparison;
- Release LTCG/PGO and an explicit AVX2 high-end tier;
- depth-prepass, asynchronous compute, and other scheduling changes only where the
  GPU timeline provides a plausible win.

These candidates may conclude as production, workload-selectable, experimental, or
rejected. A documented rejection with sound evidence satisfies the experiment; a
silent omission does not.

## Non-goals

- M8 meshlet cooking, normal-cone culling, and `VK_EXT_mesh_shader` production;
- M9 native TAA/DLAA-quality reconstruction, DLSS/FSR/XeSS, temporal denoisers, or
  general variable-rate shading rollout;
- M10 non-RT GI/AO, simulated atmosphere/clouds, or colored transparent shadows;
- M11 acceleration structures, RT shadows/reflections/GI/transmission, or path
  tracing;
- M12 material graph authoring and M13 animation runtime;
- a wholesale ECS rewrite;
- a wholesale GLM-to-DirectXMath rewrite;
- global unsafe fast-math;
- a new transparency renderer or automatic promotion to hero glass tiers;
- making Virtual Shadow Maps mandatory for every light/workload;
- removing conventional indexed/deferred or conventional shadow fallbacks before a
  matched successor is accepted;
- speculative vendor-specific device-generated commands or descriptor-buffer
  rewrites unless the portable indirect path is complete and evidence warrants a
  separately logged candidate.

## Design and data flow

### Persistent scene and visibility flow

```text
Scene UUID / asset GUID intent
        |
        v
CPU identity map and revision tracking
        |
        +--> compact dirty instance/transform/material/geometry patches
        |                         |
        |                         v
        |                  Persistent GPU scene
        |                         |
        |             +-----------+-----------+
        |             |           |           |
        |             v           v           v
        |          Main view   Shadow views  Probe views
        |             |           |           |
        |             +--> frustum / LOD / optional Hi-Z
        |                         |
        |                         v
        |              compact visible identities
        |                         |
        |                         v
        +--------------- indirect draw/dispatch bins
                                  |
                   +--------------+--------------+
                   |                             |
            packed deferred /              visibility IDs /
            complex-forward                 material resolve
```

### GPU-scene ownership

Use stable generational GPU-scene handles or an equivalent validated indirection.
The CPU maps persistent UUID/GUID identity to a transient handle. Compaction may
change physical storage but not logical identity or revision ownership.

Separate shader-hot records from cold data. The exact ABI is an M7.1 audit decision,
but the intended decomposition is:

- **Instance:** transform/bounds/primitive-range indices, mobility, visibility and
  revision flags;
- **Transforms:** current affine data and previous data only where required;
- **Primitive:** geometry range, material index, LOD chain/error, coverage/culling,
  source and cooked identity indirection;
- **Geometry:** arena/buffer identity, offsets, index type, vertex layout, bounds;
- **Material:** existing M2 schema/index, not a duplicate material representation;
- **Cold side tables:** UUID/GUID, editor/debug names, provenance, diagnostic detail;
- **Visibility output:** compact instance/primitive identity and indirect bins, not
  copied full draw packets.

Affine 3x4 transforms are preferred for GPU storage when exact for the supported
transform and verified against the existing shader convention. Nonuniform scale,
mirroring, normal transforms, and previous-state behavior must be tested explicitly.

### Mobility and updates

- **Static:** persistent transform/geometry record, no unchanged-frame upload,
  strongest cache eligibility.
- **Movable:** sparse current/previous transform and bounds patch when changed.
- **Animated:** reserves explicit current/previous deformation ownership for M13;
  M7 must not pretend rigid mobility is sufficient.

Mobility is authored or derived through a documented durable rule. It is never
guessed from a short interval without movement. Immutable mesh geometry and movable
instances are independent concepts.

Update publication must be bounded and coalesced. GPU records become visible only
at a valid frame-context boundary. Deleted/reused handles cannot expose stale scene
identity. Deferred resource/descriptor retirement follows the existing fence rules.

### Geometry representation

Group compatible indexed geometry into device-local arenas by vertex layout and
index type so indirect commands can vary `firstIndex`, `vertexOffset`, instance
identity, and material without rebinding one source-model buffer per primitive.

Production geometry cooking should apply lossless cache/fetch reordering and use
UInt16 indices where legal. A high-quality compact standard vertex is a measured
candidate: float32 position by default, high-precision packed normal/tangent,
half-precision UVs when error permits, and normalized 8-bit color. Preserve a
high-precision product and version the cooked/runtime layout explicitly.

LOD products record deterministic child identity, parent primitive/material
association, source/cook provenance, bounds, geometric error, format, and residency.
M8 meshlet and M11 RT children later attach to the same primitive/LOD identity.

### Visibility and indirect submission

The portable production path is indexed indirect-count rendering. Compute work:

1. reads a view descriptor and persistent scene records;
2. performs conservative frustum rejection;
3. selects LOD using projected geometric error and hysteresis state;
4. optionally performs conservative Hi-Z rejection;
5. compacts stable visible identities;
6. emits commands/bins for compatible pipelines/layouts/index types;
7. records deterministic overflow/fallback diagnostics.

Small workloads may select a direct path when measurement proves the dispatch and
compaction overhead is worse. This is workload flexibility, not permission to keep
per-submesh CPU scaling for dense scenes.

Hi-Z uses previous-frame depth first unless a measured current-frame two-phase path
wins. Camera cuts, rapid motion, near-plane intersections, newly resident objects,
large transform changes, and uncertain projection fail visible. Reuse one depth
pyramid contract for later cluster, contact, AO, reflection, and virtual-shadow
consumers rather than creating incompatible pyramids.

### Shadows

Conventional cascades, spot atlas, and point cubes remain the robust reference and
fallback. M7 first makes their caster extraction consume independent GPU-scene
visibility and shadow-specific LOD/geometry products.

`VSM` in M7 means **Virtual Shadow Maps**, not Variance Shadow Maps. The candidate
flow is:

```text
Visible receivers / light projection
        -> mark virtual pages
        -> resolve cached residency
        -> allocate/evict bounded physical pages
        -> cull casters per dirty page
        -> indirect raster into physical pages
        -> sample through page table with defined filtering/fallback
```

Directional lights use clip levels. Local lights use suitable virtual projections
and mip/residency policy. Cache identity includes stable light, projection, caster,
geometry/LOD/material-alpha, page-table, and pipeline revisions. Static and dynamic
content should be separable where that prevents a small moving object from forcing
expensive static rerasterization.

High/Ultra may prefer virtual shadows when matched evidence wins. Conventional maps
remain valid for Low/Medium, simple scenes, unsupported capability, debugging, and
workloads where they are faster. M8 meshlets feed the same page requests and cache
ownership. M9 may stabilize stochastic soft filtering; M11 may supersede selected
hero-light visibility with rays.

### Fine-grained cooking and residency

The source model parent becomes a deterministic manifest over independently cooked
children. Child recipes cover semantic texture views, primitive geometry, LODs, and
later meshlet/RT products. Worker completion order never affects output slots or
parent bytes.

Runtime publication is prioritized by visible demand and bounded per-frame bytes/
time. Missing detail uses semantic textures and bounds/proxy or resident coarser LOD.
Cancellation is newest-revision only and never publishes a mixed parent/child set.

### Visibility/material resolve

The indexed visibility experiment emits stable instance/primitive/triangle identity
and sufficient interpolation data for derivative-correct standard-material resolve
into the M2 canonical packed surface cache. Compare the complete depth + visibility
+ reconstruction + material resolve + cache + lighting chain against packed
deferred. Do not accept a smaller attachment if reconstruction, divergence, or
bandwidth makes the complete frame worse.

Complex-forward and M6 transparent surfaces remain outside the first experiment.
M8 mesh shaders must be able to emit the same payload.

## Optimization candidate register

| Candidate | Initial disposition | Required decision evidence |
|---|---|---|
| Persistent GPU scene and dirty updates | Required production | Correct identity/lifetime; CPU and upload scaling |
| Hot/cold draw-data split | Required production | Reduced copied/iterated bytes without lost diagnostics |
| Indexed indirect-count and geometry arenas | Required production | Dense-scene CPU/draw reduction; direct fallback |
| Frustum culling | Required production | Off-screen work removed; fail-visible correctness |
| Screen-error LOD and hysteresis | Required production | Triangle reduction with bounded error/no popping |
| Previous-frame Hi-Z | Expected production, measured | Occluded-work reduction; camera-cut/disocclusion safety |
| Independent main/shadow/probe visibility | Required production | No missing casters/reflections |
| UInt16 eligible indices | Expected production | Exact topology and reduced bytes |
| Cache/fetch reorder and degenerate filtering | Expected production | Exact/approved topology and measured locality |
| Compact standard vertex | Fidelity-gated selectable | Attribute/image error and vertex bandwidth/memory |
| Position-only shadow stream/index | Workload-gated production | Shadow raster saving versus extra residency |
| Shadow-specific LOD/caster thresholds | Fidelity-gated selectable | Stable shadow detail and update reduction |
| Virtual Shadow Maps | Workload/quality-selectable candidate | Matched CSM/atlas/cube quality, cost, memory, p99 |
| Cluster active-depth/reference optimization | Measured candidate | Complete lighting agreement and cost reduction |
| Persistent scratch/allocation removal | Required production | Zero steady allocation |
| Cached per-view derived math | Expected production | Exact matrices/frusta and reduced redundant work |
| DirectXMath/SoA SIMD | Benchmark-gated internal kernel | Kernel and representative full-frame savings |
| LTCG/PGO/AVX2 tier | Benchmark-gated build option | Reproducible performance, compatibility, determinism |
| Hybrid-core job priority/placement | Benchmark-gated scheduler policy | Render latency and editor responsiveness under cook/load pressure |
| Async compute | Timeline-gated | Genuine overlap without latency/barrier regression |
| Async I/O/decompression or DirectStorage-style delivery | Defer unless M7 residency traces prove an I/O/decode bottleneck | Load latency, CPU cost, hitching, portability, and fallback |
| Conditional depth prepass | Workload-selectable | Complete depth+shading win, not pass-only win |
| Indexed visibility/material resolve | Workload/capability candidate | Full-chain 4K image/perf/memory win |
| Variable-rate shading | Defer to M9 unless required plumbing only | Temporal/perceptual reconstruction evidence |
| Mesh shaders/meshlet normal cones | Defer to M8 | Consume M7 records |
| RT shadows and SER-like work | Defer to M11 | Consume M7 records |

## Vertical slices

Only one slice is `In Progress`. Each slice leaves the engine buildable, preserves
the direct/conventional fallback, updates this plan with evidence, and records any
changed decision. Slice numbering may be refined after M7.0, but dependencies and
acceptance behavior must remain explicit.

### M7.0 audit corrections — 2026-08-28

The fresh-lead audit confirms the central diagnosis and changes no accepted ADR.
It also narrows several implementation boundaries:

- M7.1-M7.2 consume the current validated whole-model product unchanged. They add
  a backend-neutral GPU-scene ABI and sparse publication beside `DrawPacket`; M6
  transparency remains packet based until its own later integration seam.
- M7.3 first establishes high-precision geometry arenas and explicit index/base-
  vertex semantics. Mixed UInt16/UInt32 products follow in the same versioned
  geometry slice. Compact vertices, 3x4 transforms, and shadow-only streams remain
  separately measured candidates rather than prerequisites.
- M7.5 may initially keep deterministic LOD children inside the parent model product.
  M7.9 externalizes the already-proven primitive/LOD definitions into child CookKeys
  and progressive physical residency. This avoids changing layout, LOD policy,
  hashing, cancellation, and publication atomicity in one rewrite.
- M7.4 must add explicit `multiDrawIndirect`, `drawIndirectFirstInstance`,
  `drawIndirectCount`, and `maxDrawIndirectCount` discovery/policy. Vulkan 1.3 alone
  does not make the current logical-device feature chain sufficient.
- Main, directional/spot/point shadow, reflection-probe, and selection visibility
  remain independent consumers. Existing spot/point/probe CPU sphere rejection is
  retained as an oracle/fallback; directional cascades currently have no caster
  rejection and are the first conventional-shadow scaling gap.
- The existing transparency depth pyramid is reusable only for render-graph,
  per-mip-view, resize, and retirement patterns. Its nearest metric-depth reduction
  and frame-local lifetime are not a conservative previous-frame occlusion Hi-Z.
- M7.0 fixture work must extend the current one-model benchmark schema or freeze
  source/cooked scene documents: it cannot presently express three distinct model
  assets, explicit occluder/occludee roles, shadow-only/probe-only participants, or
  near/mid/far groups.
- Asset-publisher eviction currently releases accounting/state but the model
  publication callback supplies no physical GPU-resource retirement. M7.9 must
  close that gap before claiming model-child residency or eviction.

M7.0 closed with a frozen 16-role fixture matrix, a six-case runnable dense-asset
manifest, exact/unavailable entry counters, five independent native-4K Release
profiles, and a validation-clean scene-linear capture. The median displayed run
average was 4.641494 ms, the median GPU-frame median was 1.214784 ms, and the median
CPU-frame median was 1.7757 ms. Full hashes, per-run results, draw/triangle/memory
ownership, and the clean 71/71 Debug and 71/71 Release post-change gate are recorded
in the audit report and `assets/benchmarks/m7/m7.0-run-manifest.v1.json`.

### M7.0 — Independent audit, fixtures, and baseline

**Work**

- Read all required context, ADRs, M6 acceptance/handoff, and current source.
- Inspect and preserve the dirty worktree; record revision and status manifest.
- Freeze the fixture matrix from the performance contract.
- Add missing CPU/GPU/memory/visibility/upload/shadow counters before optimizing.
- Attribute the three-asset delta and remeasure the post-M6 cluster/shadow costs.
- Inventory device capabilities and limits required by indirect count, subgroup,
  descriptor indexing, page tables, and candidate formats.

**Completion**

- Reproducible five-process native-4K baseline and validation captures exist.
- The lead can explain which CPU/GPU stages, draws, triangles, bytes, and waits own
  the observed scaling.
- This plan is updated where source evidence differs; lasting new choices receive an
  ADR rather than silent contradiction.

### M7.1 — GPU-scene ABI, identity, mobility, and lifetime

**Work**

- Define backend-neutral records, handles, revisions, capacity/overflow, and safe
  missing-record behavior.
- Separate hot runtime data from identity/editor/provenance side tables.
- Define Static/Movable/Animated update semantics and current/previous ownership.
- Define frame-boundary publication, deletion, compaction, and retirement.
- Freeze RHI manifests and CPU/reference packing tests before Vulkan consumption.

**Completion**

- Stable UUID/GUID intent survives slot reuse and compaction.
- Records are reconstructable for main, shadow, probe, selection, M8, M9, and M11.
- ABI sizes/alignment and memory capacity are measured, not accidental.

**Accepted result — 2026-08-29**

- ADR-0014 freezes 64-bit non-wrapping logical handles with fence-serial retirement,
  cold identity/provenance tables, explicit mobility and independent consumer masks.
- The std430-friendly reference ABI measures 48 bytes per affine transform, 80 per
  instance, 48 per instantiated primitive binding, and 96 per shared geometry.
- Deterministic reference packing preserves owner/model/source/exact/material
  identities across dense reordering, keeps entity material overrides separate from
  shared geometry, and routes malformed/missing/capacity-omitted instances to direct
  fallback instead of partial disappearance.
- Parallel 64-bit revision tables and exact coalesced upload planning are defined per
  fence-owned frame context. Debug and Release builds pass 72/72 tests. This ABI-only
  slice changes no Vulkan rendering, image, timing, or VRAM behavior; those gates
  begin when M7.2 publishes the records.

### M7.2 — Persistent publication and compact updates

**Work**

- Publish existing scene instances/primitives/material indices/bounds/transforms to
  persistent GPU buffers.
- Replace repeated full extraction with coalesced dirty patches.
- Pre-resolve material override lookup when authoring state changes.
- Add persistent frame-context scratch and eliminate steady allocations.
- Keep current direct draws consuming the persistent records as a parity stage.

**Completion**

- Static fixture uploads zero unchanged instance/transform bytes.
- One movable object updates only its required current/previous/bounds ranges.
- Direct-path images and M6 transparency remain matched and validation-clean.

**Accepted result — 2026-08-29**

- Stable scene observations now publish 3x4 current/previous transforms, instance
  records, instantiated primitive bindings, and shared geometry records into four
  fence-owned Vulkan storage tables with exact per-context revision-range uploads.
- Fence-derived monotonic submission/completion serials govern retirement; atomic
  capacity preflight and non-wrapping handles preserve direct fail-visible behavior.
- Ordinary opaque and forward-opaque direct draws consume the persistent packed
  records. M6 transparency and explicit runtime-batch/failure fallback remain on the
  unchanged packet path.
- Cached primitive/material-override extraction avoids rebuilding unchanged source
  primitive records. The frozen fixture records zero changed records, zero upload
  bytes/ranges, and zero steady C++ allocations after 500 warm-up frames.
- The final validation capture is byte-identical to M7.0 (SHA-256
  `2073f5d985ce3632a1e0fb438fa441e06617ce76623534705f170c5bf423a297`).
  Debug and Release pass 72/72 tests; focused GPU-scene coverage passes 8/8.
- Full evidence and deliberately deferred M7.3/M7.4 work are recorded in
  `docs/performance/M7.2-persistent-gpu-scene-publication-2026-08-29.md`.

### M7.3 — Geometry arenas and cooked data efficiency

**Work**

- Add versioned geometry arenas and stable primitive ranges.
- Produce UInt16 indices for eligible primitives and UInt32 otherwise.
- Apply deterministic cache/fetch reordering and validated redundant-triangle
  filtering.
- Evaluate 3x4 GPU transforms, compact standard vertices, and position-only shadow
  streams under the performance/fidelity gates.
- Preserve high-precision/legacy geometry products where needed.

**Completion**

- Geometry/index bytes and fetch counters improve without topology, silhouette,
  material, tangent, or UV regressions.
- Products are deterministic and old/incompatible artifacts fail safely or migrate
  through an explicit version.

**Accepted result — 2026-08-29**

- A versioned backend-neutral geometry arena now provides stable primitive ranges,
  explicit base vertices, and split UInt16/UInt32 index streams. Cooked schema 6
  writes index-section schema 2 and explicitly migrates schema-5 UInt32 products.
- Cooked model Vulkan publication uses one shared high-precision vertex buffer and
  one index buffer per required width. All indexed draw consumers and persistent
  GPU-scene geometry records preserve the arena's base vertex.
- The importer removes exact same-winding opaque duplicates and adopts a
  deterministic 32-entry-cache reorder only on strict simulated-fetch improvement;
  transparent order and shared raster/RT triangle order remain invariant.
- The production fixture removes one duplicate triangle and stores all 913,620
  remaining indices as UInt16. Requested index bytes fall exactly 50%, from
  3,654,480 to 1,827,240; the complete artifact falls 1,818,864 bytes (3.4970%).
- A warm repeat returns the identical cook key and artifact hash. Unknown arena ABI
  rejects, schema 5 migrates, and Debug/Release pass 73/73 tests.
- The Release validation capture is byte-identical to M7.0 (SHA-256
  `2073f5d985ce3632a1e0fb438fa441e06617ce76623534705f170c5bf423a297`).
  Full evidence and measured compact/shadow-stream deferrals are recorded in
  `docs/performance/M7.3-geometry-arena-cooked-efficiency-2026-08-29.md`.

### M7.4 — Frustum visibility and indexed indirect-count submission

**Work**

- Implement conservative instance then primitive frustum tests.
- Compact visible identities and generate deterministic indirect bins/commands.
- Submit compatible geometry through indirect count; retain a direct fallback.
- Define overflow prioritization and fail-visible fallback.
- Reuse visibility identity for selection where correctness permits.

**Completion**

- Off-frustum fixtures stop producing main-view raster work.
- CPU record/submission time and draw calls scale with compacted bins rather than
  source submeshes.
- Tiny-workload policy selects direct or indirect from evidence, not assumption.

**Accepted result — 2026-08-30**

- Backend-neutral visibility and 20-byte indexed-indirect ABIs now drive a Vulkan
  device-compute compaction pass and `vkCmdDrawIndexedIndirectCount`, with direct
  fallback for unsupported, malformed, capacity-limited, tiny, wireframe,
  selection, and complex-forward work.
- All-visible, one-visible, and 256-instance stress fixtures prove exact
  device/oracle command parity with zero mismatched bins and zero overflow. The
  one-visible fixture rejects 206 of 339 main-view primitives and 471,210 of
  905,898 opaque triangles without changing independent consumers.
- The final validation capture is byte-identical to M7.0 (SHA-256
  `2073f5d985ce3632a1e0fb438fa441e06617ce76623534705f170c5bf423a297`),
  Debug and Release pass 76/76 tests, and the canonical launcher opens a visible,
  responsive Release window.
- The accepted five-run native-4K set records 1.834385 ms median wall average,
  1.7689 ms CPU median, 1.149856 ms GPU median, 0.007584 ms compaction median,
  zero steady allocations, and 303/303 parity on every run. Relative to M7.0,
  wall time falls 60.48%, GPU median falls 5.35%, and live/peak requested memory
  falls 1.632/3.446 MiB.
- Exact evidence, hashes, the rejected externally contended run, and remaining
  tiny-threshold/view-scaling follow-up are recorded in
  `docs/performance/M7.4-frustum-indirect-parity-2026-08-29.md`. No ADR change is
  required.

### M7.5 — LOD cooking and screen-space selection

**Status:** Accepted 2026-09-02 as a workload-selectable route; LOD0 remains the
global and hero-quality fallback.

**Work**

- Add deterministic authored/generated LOD child products and geometric error.
- Select per view with hysteresis, stable identity, residency awareness, and hero
  overrides.
- Publish the shared per-content ceiling needed by every consumer, but keep
  shadow/probe-specific thresholds and independent visibility in M7.7.
- Add LOD transition and fallback diagnostics.

**Completion**

- Near/mid/far fixtures reduce triangles according to bounded error.
- Motion captures show no accepted popping, oscillation, missing material, or broken
  silhouette.
- Missing fine LOD uses a valid coarser resident product.

### M7.6 — Hi-Z occlusion and shared depth-pyramid contract

**Status:** Accepted — workload-selectable; production default remains off.

2026-09-12 large-occluded admission: the frozen 64-instance native-4K depth
stack enlarges only its nearest instance and retains ordinary-scale occludees.
The fused route removes 5,133 of 6,464 opaque commands (79.4090%), reduces median
GBuffer time from 0.741376 ms to 0.081920 ms and complete GPU time from 2.133760
ms to 1.471840 ms. Median CPU time improves from 5.455600 ms to 4.828700 ms after
qualification-only projection, standalone query dispatch, per-result host
readback, result writes, and full-capacity oracle buffers are removed from the
deployable route. The independent oracle remains automatic under validation and
reports exact command/bin parity with zero invalid or unsafe results; final
baseline/rejection captures are byte-identical. M7.6 is accepted as a
workload-selectable path and M7.7 becomes active. See
`docs/performance/M7.6-large-occluded-admission-2026-09-12.md`.

2026-09-11 in-place view history: the experimental build now writes directly into
the persistent image owned by the active retained view after fused compaction has
sampled that image's preceding contents. This removes the two per-frame R32 chains,
the full-mip publication copy, two graph passes, one logical/physical graph
resource, 9.374718 MiB requested at 1280x720, and 84.373894 MiB requested at native
4K. Scene/asset history isolation, fence-safe publication, exact live D32 oracle
parity, temporal/cut parity, byte-identical query/rejection captures, and the
three-step resize recovery all remain valid under synchronization validation. The
focused pyramid range falls from 0.054272 ms to 0.032768 ms in matching 32-frame
temporal runs. Only the controlled large-occluded-workload performance gate remains
before the production admission decision; the feature stays default-off. See
`docs/performance/M7.6-in-place-view-history-2026-09-11.md`.

2026-09-11 resize recovery: `--validate-depth-pyramid-resize` now reallocates the
scene target at 960x540, 1600x900, and restored 1280x720. Every resize resets
history and retains all 404 commands; the following stable frames recover with
exact device/oracle counts of 340, 346, and 331. Three requests, successes, and
graph rebuilds complete with zero validation, unsafe, invalid, bin, overflow, or
profiler-drop errors. The subsequent in-place-history slice closes the duplicate
storage item; the controlled large-occluded-workload performance gate remains. See
`docs/performance/M7.6-resize-recovery-2026-09-11.md`.

2026-09-11 small-object/depth-content safeguards: a 0.001-scale fixture reports
all 101 opaque candidates as exact `SmallBounds` fail-visible results and retains
all commands. A selected-occluder visibility step changes the active set from 404
to 303 commands, rejects old history once as `DepthContentChanged`, retains all 303
transition commands, and recovers to 251 commands after 52 safe rejections. Both
routes have exact oracle parity and zero invalid/unsafe/bin/overflow/profiler-drop
counts; transition/recovered image pairs are byte-identical. Per-reason diagnostics
and resize qualification now fit in a bounded 272-counter frame capacity. Resize
evidence is complete; the later in-place-history slice closes duplicate storage,
leaving controlled large-workload performance evidence. See
`docs/performance/M7.6-small-object-depth-content-2026-09-11.md`.

2026-09-11 moving geometry/disocclusion: a new manifest-owned selected-instance
step moves either the nearest occluder or farthest occludee at frame 12. Both
Release synchronization-validation runs invalidate history exactly once as
`DepthContentChanged`, retain the transition geometry, recover eligibility on the
next frame, and maintain exact device/oracle command parity with zero invalid,
unsafe, bin, or overflow results. Query-only/rejection captures are byte-identical
at the transition and first recovered frame, while the pre-step image is distinct.
Small-object and explicit depth-content invalidation are now also complete. True-
resize and in-place history are now complete; controlled large-workload performance
evidence remains. See
`docs/performance/M7.6-moving-occluder-disocclusion-2026-09-11.md`.

2026-09-11 fused query/compaction: the rejection variant now projects, queries,
writes its qualification record, and conditionally emits each indirect command in
one invocation. A shared GLSL implementation keeps standalone and fused verdicts
identical. Unavailable/cut/rebuilt history uses a compatible base pipeline so an
undefined image is never statically accessed; frustum-rejected candidates write an
explicit auditable `OutsideView` record. Temporal, off-frustum, transport-rebuild,
and image checks are exact, with zero invalid/unsafe/bin/overflow results. The
separate device-owned query dispatch and barrier are gone. Keep this experimental:
fused local work remains about 0.0025–0.0047 ms above base compaction plus query in
the current small fixture, and reverse-ordered 4K processes remain state-noisy. See
`docs/performance/M7.6-fused-query-compaction-2026-09-11.md`.

2026-09-11 experimental command rejection: the separately explicit
`--experimental-depth-occlusion-rejection` route now consumes validated
device-owned Hi-Z verdicts during opaque indirect compaction. The frozen 32-frame
overlap/cut run removes exactly 73 of 404 pre-cut commands and 42 of 404 post-cut
commands, retains all work on unavailable/cut frames, preserves byte-identical
current-build captures, and has exact per-bin oracle parity with zero invalid or
unsafe results. Off-frustum and scRGB/HDR10/SDR rebuild runs are also exact. This is
not production admission: the 1280x720 pair adds 0.008608 ms to complete GPU median,
and both 4K pairs contain unrelated forward/UI variance. Fuse query with compaction,
remove duplicate history/readback storage, and add small-object/depth-invalidation/
true-resize evidence before repeating the native-4K gate. Production remains off.
See `docs/performance/M7.6-experimental-command-rejection-2026-09-11.md`.

2026-09-11 device-owned projection qualification: a second default-off compute
oracle now constructs Hi-Z queries directly from the live GPU-scene tables and
validates them against the independently CPU-projected/device-tested safety oracle
after the frame fence. It matches the CPU oracle's conservative world AABB, adds an
extra guard pixel and camera-ward depth margin, and explicitly fails visible on all
uncertainty. The frozen 32-frame overlap/camera-cut run, 16-frame off-frustum run,
and scRGB/HDR10/SDR rebuild run have zero invalid records and zero unsafe mismatches.
The GPU route intentionally reports fewer marginal rejections: 73 versus 88 before
the cut and 42 versus 48 after it. Its isolated dispatch median is approximately
0.001024 ms at 1280x720. The subsequent separate rejection experiment consumes
these verdicts, but production fusion and duplicate-history removal remain.
See `docs/performance/M7.6-device-owned-projection-qualification-2026-09-11.md`.

2026-09-10 live-query qualification: complete deferred-opaque plus depth-writing-
forward ownership is now evaluated before opaque compaction. The additional
default-off `--experimental-depth-occlusion-query` path projects frustum-visible
bounds, consumes exactly-adjacent same-queue history, dispatches the proven query
shader, and validates fence-delayed records without rejecting any draw. A Release
1280x720 synchronization-validation run reports 303 requested, 288 tested, 15
projection-fail-visible, 34 `would_reject`, and zero invalid records on every
measured frame. Indicative overhead is +0.1886 ms CPU-frame median and 0.0625 MiB;
GPU-frame median changes by +0.001312 ms in one short pair. Device-owned projection/
compaction and temporal fixture evidence remain before admission. See
`docs/performance/M7.6-live-query-qualification-2026-09-10.md`.

The same record also freezes and runs `m7_occlusion_depth_stack_v1`: four dense
depth-overlapping assets followed by a deterministic oblique camera cut. Stable
pre-cut frames report 88 `would_reject`; the cut frame rejects history as
`CameraCut` and remains fully visible; stable post-cut frames report 48
`would_reject`. All device records are valid, and pyramid-only/query cut-frame
captures are byte-identical. The fixture matrix now marks `large_occluder` runnable.

2026-09-10 device-query parity: a versioned 32-byte query/result ABI and Vulkan
compute shader now match the CPU oracle exactly for 3,640 queries across 28
forward/reverse, D32/R32, odd/1D/native-4K configurations under synchronization
validation. History camera identity now hashes both view and projection, closing
a pose-reuse hole. Queue-ordered metadata is now distinct from fence-published CPU
metadata, allowing an exactly-adjacent same-queue consumer without pretending the
image is CPU-complete; alternating views remain stale. Complete depth-content
identity must move before opaque compaction before live binding. No rejection is
enabled. See
`docs/performance/M7.6-device-query-parity-2026-09-10.md`.

2026-09-10 view-history publication: the opt-in live chain now publishes into
independent persistent scene and asset-preview images. Producer metadata carries
actual view/scene/depth/projection/reset revisions, global submission serial and
image generation, and becomes available only after the producing frame fence.
Newer same-view submissions suppress stale completion; resize/rebuild invalidates
all generations. The live oracle also verifies the copied history exactly. At 4K,
the temporary build-plus-copy bridge costs 0.162816 ms median and adds 87.3515625
MiB committed over build-only storage; eliminating that duplicate topology is an
explicit pre-admission task. Rejection remains off. See
`docs/performance/M7.6-view-history-publication-2026-09-10.md`.

2026-09-10 live-source parity: the opt-in validation path now copies the actual D32
opaque depth attachment and every R32 pyramid mip after reduction, then compares
all texels against the CPU oracle after the frame fence completes. Empty 1280x720
and populated odd-extent 3840x2139 runs pass exactly under synchronization
validation; the populated run checks 10,949,495 pyramid texels over 12 mips with
zero invalid source depths and zero mismatches. Readback is one-shot qualification
only. See `docs/performance/M7.6-live-depth-readback-parity-2026-09-10.md`.

2026-09-09 live build prerequisite: `--experimental-depth-pyramid` now samples
completed opaque scene depth into a graph-owned R32 farthest-depth mip chain.
Default-off, build-only: no object rejection or history consumption. Debug/Release
tests pass; live synchronization validation, identical 4K captures and scRGB/HDR10/
SDR rebuilds pass. Initial 4K build cost is approximately 0.11 ms and 87.35 MiB
committed across two frame contexts, not a production admission measurement.
Next: GPU query/compaction and temporal fail-visible qualification. See
`docs/performance/M7.6-live-depth-build-2026-09-09.md`.

2026-09-09 history ownership prerequisite: depth-pyramid contract v2 requires
explicit view/scene/depth-content/projection/reset identities, depth convention and
adjacent global submission serials. Missing, cross-view, changed-occluder and stale
history fail visible. This is CPU metadata qualification, not live resource or
temporal occlusion acceptance. See
`docs/performance/M7.6-history-ownership-2026-09-09.md`.

2026-09-09 device reduction follow-up: the farthest-depth compute shader passes
exact all-mip readback parity in 28 D32/R32, forward/reverse, odd/1D/4K cases on the
RTX 4090 in Debug and Release with synchronization validation. This is a headless
hardware test, not live scene-depth consumption. Live build/readback and persistent
view-owned publication are now complete; GPU query/compaction is next, and
occlusion remains off.
See `docs/performance/M7.6-device-reduction-parity-2026-09-09.md`.

2026-09-09 resumed after viewer side features. Conservative world-bounds projection
now produces padded history-view query rectangles and near-rounded device depth,
with explicit fail-visible reasons. CPU tests cover forward/reverse perspective and
orthographic projections, jitter, Y orientation, odd/4K extents and visible holes.
No runtime rejection is enabled; device reduction/readback parity and history
ownership remain next. See
`docs/performance/M7.6-conservative-bounds-projection-2026-09-09.md`.

2026-09-04 prerequisite correction: the CPU oracle now maps query endpoints through
each mip's integer partition. A direct base-to-mip ratio caused false rejection on
odd extents. Debug/Release regression tests cover every rectangle and hole position
on three odd/one-dimensional extents in both depth conventions. Vulkan integration
must match this corrected mapping. See
`docs/performance/M7.6-odd-extent-query-correction-2026-09-04.md`.

**Work**

- Build an extent-aware depth pyramid and conservative previous-frame tests.
- Handle camera cuts, rapid motion, disocclusion, near-plane bounds, new residency,
  and small objects.
- Record requested/tested/rejected/fail-visible reasons.
- Expose a backend-neutral depth-pyramid consumer contract without pulling M9/M10
  features into M7.

**Completion**

- The occluder fixture removes hidden geometry work with no missing-object captures
  or temporal flicker beyond frozen thresholds.
- Resize, zero extent, capture insertion, and live transport remain safe.

### M7.7 — Independent consumers and conventional-shadow optimization

**Status:** Accepted 2026-09-29.

The first accepted implementation slice decouples project/profile directional
coverage from distant camera far planes, preserves engine-derived practical/log
splits, reports exact split and world-units-per-texel density, reconstructs
bilinear comparisons for PCSS filter taps while retaining raw blocker depth, and
makes the Cinematic project tier reachable. Bias constants remain unchanged until
the receiver-plane/grazing fixture gate in the following slice. See
`docs/performance/M7.7-directional-shadow-density-and-filter-reconstruction-2026-09-12.md`.

The second accepted slice keeps raster bias unchanged and adds a project-owned
1.25-texel receiver depth bias, bounded 2.0-texel receiver-plane correction, and
0.5-texel geometric-normal offset. The matched grazing fixture removes severe
self-shadow striping with identical deferred-lighting median cost. See
`docs/performance/M7.7-directional-shadow-receiver-bias-2026-09-12.md`.

The third accepted slice derives a conservative caster set for each refreshed
directional cascade while preserving the pre-main-view shadow source queue and
unknown-bounds fail-visible behavior. The moving-grid fixture rejects 162/192
caster-cascade pairs before Vulkan resource lookup and records 30 draws; the
accepted contact image remains byte-identical. See
`docs/performance/M7.7-directional-shadow-caster-visibility-2026-09-12.md`.
The affected-bounds follow-up in the same report replaces the global caster cache
key with four conservative revisions; a live enter-volume transition refreshes two
cascades while the other two remain cached and sampleable.

The fourth accepted slice replaces copied persistent caster packets with a
backend-neutral submission containing compact GPU-scene primitive references and
an explicit direct fallback. Directional, spot, and point passes now share the
camera-independent shadow consumer set. The 48-primitive moving-grid fixture uses
192 bytes of application submission payload instead of 11,520 bytes of copied
packets, reports zero fallback packets, and keeps the grazing contact capture
byte-identical. See
`docs/performance/M7.7-gpu-scene-shadow-submission-2026-09-13.md`.

The fifth accepted slice builds directional-cascade commands on the device from
that compact set. One candidate table is shared across refreshed cascades, compute
performs the accepted light-view sphere test, and GPU-scene shadow shaders resolve
transform and material through indirect `firstInstance`. All 30 commands in the
48-caster/two-cascade qualification match the independent oracle with zero overflow;
paired direct/indirect captures are byte-identical. See
`docs/performance/M7.7-directional-shadow-device-commands-2026-09-13.md`.

The sixth accepted correctness slice extends the same bounded device-command ABI
to refreshed spot-atlas tiles and point-cube faces. Nine spot commands and 90 point-
face commands match their independent CPU oracles with zero mismatch or overflow,
and paired direct/indirect final-SDR captures are byte-identical. The single
refreshed-frame timing samples are diagnostic only; warmed heterogeneous local-
light admission remains open. The follow-up removes per-view CPU visibility from
production device submission while retaining exact delayed device counts and
explicit/validation-enabled oracle comparison. See
`docs/performance/M7.7-local-shadow-device-commands-2026-09-13.md` and
`docs/performance/M7.7-shadow-command-oracle-gating-2026-09-13.md`.

The seventh accepted architecture/correctness slice gives reflection capture a
backend-neutral compact submission selected with `GpuSceneConsumerProbe` instead
of reusing main-view queues. Nine primitives require 36 bytes rather than 2,160
bytes of packets, with no invalid references, and the direct/compact captures are
byte-identical. Stable owner identity excludes 18 owned primitive-face pairs; the
remaining 36 tests conservatively reject three and draw 33. See
`docs/performance/M7.7-independent-probe-visibility-2026-09-13.md`.

The eighth accepted implementation checkpoint moves compact probe-capture shading
onto a GPU-scene graphics pipeline: `firstInstance` selects the persistent
primitive, while direct packets retain an interleavable fallback/reference path.
An isolated probe-only A/B preserves all 33 draws and bounds the scene-linear
difference to one R16 quantization step (`0.0009765625` maximum,
`0.0000194135` mean). Device face visibility and command construction remain open.
See `docs/performance/M7.7-probe-gpu-scene-capture-pipeline-2026-09-14.md`.

The ninth accepted architecture/correctness checkpoint gives reflection capture
its own bounded candidate, indexed-command, and count allocation. Device cube-face
tests use the accepted conservative sphere/clip rule, exclude the stable capture-
owner instance, and submit three compatible bins through indirect-count draws. All
33 device commands match the qualification oracle with zero mismatch or overflow;
the deployable route performs no GPU-owned CPU face tests and retains the prior
scene-linear capture hash. See
`docs/performance/M7.7-probe-device-commands-2026-09-14.md`.

The tenth accepted architecture/correctness checkpoint adds independent radial
probe LOD to device command construction. All six cube faces share one selection
per primitive, the host admits only resident buffer-compatible chain prefixes, and
validation compares complete command multisets. At a 2-pixel threshold the 339-
primitive near/mid/far fixture reduces 7 of 226 visible commands and removes 4,508
capture triangles with zero mismatch or overflow; the final scene capture remains
byte-identical to LOD0. The deployable route performs no CPU visibility/LOD oracle
work. See `docs/performance/M7.7-probe-radial-lod-2026-09-14.md`.

The eleventh accepted architecture/correctness checkpoint makes directional-shadow
LOD depend on each cascade's measured world-units-per-texel rather than main-camera
distance. The host admits only resident buffer-compatible chain prefixes and the
device chooses geometry per visible cascade command. At a 2-texel bound, the dense
near/mid/far fixture reduces 21 of 1,243 commands and removes 13,524 shadow
triangles with exact command-oracle agreement; its scene-linear capture is byte-
identical to LOD0. Production performs no CPU caster visibility or shadow-LOD
oracle work. See
`docs/performance/M7.7-directional-shadow-lod-2026-09-14.md`.

The twelfth accepted architecture/correctness checkpoint adds face-invariant radial
LOD to point-cube device commands. A point light, cube resolution, and conservative
bounds produce one selection shared by all six faces. At a 2-texel bound, the dense
near/mid/far fixture reduces 16 of 565 commands and removes 11,436 shadow triangles
with exact device/oracle agreement and no overflow. Scene-linear output differs by
at most `5.96046448e-7`; production performs no CPU point-face visibility or LOD
oracle work. See
`docs/performance/M7.7-point-shadow-radial-lod-2026-09-14.md`.

The thirteenth accepted architecture/correctness checkpoint adds projected LOD to
spot-atlas device commands. The authored light clip transform and tile resolution
drive the conservative perspective error bound. At a 2-texel bound, the dense
near/mid/far fixture reduces 10 of 339 commands and removes 6,376 shadow triangles
with exact device/oracle agreement and no overflow. The scene-linear capture is
byte-identical to LOD0; production performs no CPU spotlight visibility or LOD
oracle work. See
`docs/performance/M7.7-spot-shadow-projected-lod-2026-09-14.md`.

The fourteenth checkpoint completes the reflection-sensitive warmed probe-LOD
gate without admitting a production threshold. A standalone non-renderable
1024-pixel realtime probe captures all three moving high-detail cars and publishes
into their visible glossy reflections. It exposed and repaired partial-cube ticket
replacement: admitted multi-frame cubes now finish and publish before newer scene
revisions queue another cadence-bounded refresh. At 16 pixels, exact device/oracle
selection removes only 380,392 of 41,067,376 retained-face triangles (0.9263%);
the native-4K final-SDR capture is byte-identical to LOD0, but reversed-order
Release pairs show no repeatable CPU, GPU, wall-time, or memory win. LOD0 therefore
remains the global probe default and the experimental selector remains available.
See `docs/performance/M7.7-probe-lod-warmed-admission-2026-09-24.md`.

The fifteenth checkpoint completes the conventional shadow-LOD admission gate
without promoting a threshold. Native-4K directional/spot/point sweeps remain
visually bounded through eight texels, and the moving dense-geometry oracle is
exact with zero overflow. The candidate removes 1.2684% of combined shadow
triangles and saves 0.007-0.011 ms of combined shadow GPU time, but CPU shadow
recording regresses and reversed-order whole-frame results do not repeat. LOD0
therefore remains the global default. See
`docs/performance/M7.7-shadow-lod-warmed-admission-2026-09-29.md`.

The warmed heterogeneous conventional-shadow command gate is separately accepted:
one directional, two spot, and two point lights reduce median CPU shadow recording
by 70.1-70.9% at native 4K with byte-identical output and exact validation/oracle
agreement. This admits device command construction, not default-off shadow LOD.
See `docs/performance/M7.7-heterogeneous-shadow-admission-2026-09-19.md`.

The final quality-closure fixture sweeps three copies of the authored thin,
fully-clipped alpha-mask, double-sided contact scene through near/far cascade
density regions. Validation matches 18-36 device commands exactly with zero
mismatch, overflow, or fallback. Conventional-direct and automatic native-4K
captures are byte-identical at both sweep extremes, and visual inspection confirms
attached finite contact shadows with no clipped-caster residue or penumbra
staircase. See
`docs/performance/M7.7-shadow-quality-closure-2026-09-29.md`.

**Work**

- Position/opacity fetch is accepted without duplicating geometry storage: opaque
  direct and GPU-scene shadow pipelines fetch position only, while alpha masks keep
  color/UV inputs. Native-4K reversed-order pairs save 0.082-0.088 ms of combined
  shadow raster with byte-identical output; a separate compact position buffer is
  rejected pending an additional memory-inclusive benefit. See
  `docs/performance/M7.7-opaque-shadow-position-fetch-2026-09-29.md`. Static consumer-membership publication
  and directional/spot/point bin/candidate caching are implemented and Release-
  smoke qualified with all caches hitting and zero fallback; see
  `docs/performance/M7.7-shadow-membership-cache-2026-09-29.md`. Directional
  density-based, face-invariant point-cube, and projected spotlight LOD are
  implemented; their visual/performance admission gate is complete without
  promotion. Virtual-page variants remain M7.8 work.
- Decouple project/profile directional-shadow coverage from camera far distance and
  derive stable practical/logarithmic cascade splits from a reported world-units-
  per-texel density policy. Do not expose manual cascade planes; reserve bounded
  camera/volume overrides for demonstrated use cases.
- Remove PCSS penumbra stair-stepping by reconstructing bilinear depth comparisons
  for filter taps while retaining raw-depth blocker search, then measure the complete
  hard-to-soft path at every quality tier. Make the configured Cinematic tier
  reachable instead of silently capping per-light selection to Ultra.
- Retain world-shadow-texel bias scaling and add receiver-plane depth-gradient bias
  plus a bounded geometric-normal world offset. Tune from close-contact and grazing-
  receiver fixtures; do not substitute larger global bias constants.
- Preserve conventional CSM/atlas/cube ownership, sampling, cache, and fallback.

**Completion**

- Camera-invisible objects still cast required shadows and appear in probes.
- Conventional shadow output matches accepted quality while submission/update cost
  scales with relevant casters.
- Close penumbrae have no visible nearest-depth staircase, nearby cascades receive
  measurably finer texel density than distant cascades, and camera motion/cascade
  transitions remain stable without M9 temporal accumulation.
- Close-contact, grazing-receiver, thin/alpha-caster, and peter-panning fixtures pass
  with recorded coverage distance, cascade near/far ranges, world units per texel,
  projected texel size, penumbra radius/sample counts, and active bias terms.
- Alpha-clip and two-sided caster semantics remain correct.

### M7.8 — Sparse Virtual Shadow Map production candidate

**Status:** Active.

The first accepted architecture checkpoint defines the backend-neutral sparse page
identity and deterministic CPU residency oracle. Static and dynamic caster layers
invalidate independently; priority/update budgets, requested-page protection,
age/priority eviction, pending-raster publication, pool/update overflow, and safe
conventional or fully-lit missing-page behavior are explicit. A default-off Vulkan
qualification path now allocates the bounded D32 physical atlas, flat page table,
and two aligned frame-owned compute working sets. It validates the atlas against
device limits and reports all three persistent categories separately. The measured
engine-owned enabled total is 88.753 MiB; the disabled path remains zero. No virtual
sampling path is enabled yet. The directional receiver oracle freezes finest-level selection,
guarded coarse fallback, bounded deduplication, and signed world-page identity that
survives snapped clipmap scrolling. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`.

**Work**

- Define page-table, physical-pool, page border/filter, residency, and overflow
  contracts through RHI/Vulkan boundaries.
- Implement receiver-driven marking, directional clip levels, local-light pages,
  GPU caster culling, indirect raster, sampling, and safe missing-page behavior.
- Implement stable cache identity, age/eviction, update budgets, and static/dynamic
  invalidation separation.
- Add page/cache/caster/memory visualizations and profiling.
- Compare conventional and virtual paths under the performance contract.

**Completion**

- Classify VSM as production for selected profiles/workloads, experimental, or
  rejected based on matched 4K evidence.
- Conventional maps remain selectable and validation-clean.
- No page thrash, stale visibility, or p99 spike is hidden by a favorable static
  median.

### M7.9 — Fine-grained cooking and progressive residency

> **Superseded 2026-10-07:** the M7.9–M7.12 slices below are the original
> descriptions. `docs/milestones/M7-completion.md` is the authoritative execution plan
> for them.

**Work**

- Create deterministic child CookKeys/products for primitive geometry and LOD,
  retaining future meshlet/RT attachment points.
- Build a bounded deterministic job graph with cancellation and editor priority.
- Publish within per-frame byte/time budgets by visible demand.
- Retain last-known-good revisions and semantic/proxy/coarse fallbacks.

**Completion**

- Material-only, one-primitive, one-texture, unchanged, superseded-edit, and
  oversized-model cases pass the handoff's acceptance matrix.
- Worker completion order does not change parent bytes/output slots.
- No monolithic upload hitch or mixed revision occurs.

### M7.10 — Cluster, CPU math, compiler, and scheduling optimization

**Work**

- Reprofile cluster assignment and reduce repeated light bounds, empty clusters,
  references, scans/sorts, clears, or barriers only where evidence identifies cost.
- Cache per-view derived matrices/frusta and remove opaque-only unnecessary work.
- Benchmark GLM, DirectXMath, and custom SoA kernels behind an internal boundary.
- Evaluate LTCG/PGO and a controlled AVX2 tier.
- Measure latency-sensitive render work versus bounded background cook/import jobs
  on the reference hybrid-core CPU; prefer job priorities and evidence over broad
  manual affinity.
- Evaluate conditional depth prepass and async compute only from full-frame evidence.
- Audit shader register pressure, divergence, precision, material/tile
  classification, and render-graph synchronization.

**Completion**

- Every retained optimization has a recorded full-frame or scalability win.
- No SIMD/compiler type leaks into stable engine ABI.
- The single cluster/light representation and shader numerical contracts remain
  correct.

### M7.11 — Indexed visibility/material-resolve experiment

**Work**

- Define a payload usable by indexed and later mesh-shader emission.
- Reconstruct attributes and derivatives for standard opaque materials.
- Resolve into the M2 canonical packed cache and reuse existing lighting.
- Compare the entire chain against conventional packed deferred.
- Add material coherence, motion/history readiness, debug, and selection coverage.

**Completion**

- Classify the path as production/workload-selectable, experimental, or rejected.
- It becomes production only with matched image/material correctness and a complete-
  chain performance/memory benefit.
- Conventional packed deferred remains a fallback.

### M7.12 — Production qualification, cutover, and handoff

**Work**

- Run Debug/Release builds, all tests, Vulkan validation, fixture matrix, five-run
  4K performance, memory, residency, cancellation, resize, transport, and temporal
  comparisons.
- Verify optional/capability fallbacks and clean diagnostics.
- Update ADRs, roadmap status, frame budget, plan decision log, and acceptance
  report deliberately.
- Write the M7-to-M8 handoff with exact records and remaining risks.

**Completion**

- All mandatory outcomes and roadmap acceptance gates pass.
- Results distinguish production, selectable, experimental, rejected, and deferred
  candidates.
- M8 can add meshlets/mesh shaders without replacing GPU-scene identity,
  visibility, LOD, indirect, or virtual-page ownership.

## Delegation and integration

The M7 lead retains ownership of architecture, shared records, shader/RHI ABI,
integration, and final acceptance. Subagents may inspect or implement bounded,
disjoint slices. Prefer parallel read-only audits, tests, fixtures, cooker work, or
isolated benchmark kernels. Do not allow concurrent write-heavy changes to shared
GPU-scene headers, central render-graph/Vulkan submission, common shader includes,
model schemas, or CMake manifests.

Every delegated result is reviewed against current source, this plan, governing
ADRs, and matched evidence before integration. The lead updates durable documents so
context compression or a fresh task does not make chat history authoritative.

## Verification

At minimum:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug --output-on-failure

cmake --preset x64-release
cmake --build out/build/x64-release
ctest --test-dir out/build/x64-release --output-on-failure
```

Renderer slices also require representative validation-enabled Vulkan runs, native-
4K Release measurements, scene-linear and final-output captures, resource/counter
evidence, resize/zero-extent checks, and common plus feature-heavy material paths.
Use `docs/performance/M7-performance-fidelity-contract.md` for the full fixture and
decision protocol.

## Risks, fallback, and rollback

| Risk | Mitigation/fallback |
|---|---|
| GPU-scene slot reuse corrupts identity | Generational handles, revision checks, side-table mapping, delayed reuse |
| GPU culling drops visible content | Conservative bounds, fail visible, reason diagnostics, direct fallback |
| LOD visibly pops or damages hero content | Error bounds, hysteresis, residency fallback, hero/high-precision override |
| Compact attributes lose fidelity | Matched image/angular/UV tests and high-precision product |
| Indirect overhead hurts tiny scenes | Workload-selectable direct path |
| Geometry arena fragmentation | Bounded suballocation, metrics, safe relocation/retirement policy |
| Hi-Z flicker after motion/cuts | History invalidation, expanded bounds, grace/fail-visible policy |
| Shadow/probe caster disappears | Independent view culling tests and conservative fallback |
| Virtual pages thrash | Fixed pool/update budgets, age diagnostics, static/dynamic separation, conventional fallback |
| VSM costs more before M8 meshlets | Retain conventional production; do not force rollout |
| Visibility resolve increases divergence/bandwidth | Compare full chain; retain packed deferred |
| DirectXMath migration churn | Internal benchmark-only boundary; no wholesale migration |
| AVX2/compiler optimization changes numerics | Explicit tier, deterministic tests, portable baseline |
| Async compute serializes the frame | GPU timeline proof; graphics-queue fallback |
| Progressive publication mixes revisions | Atomic manifest/revision selection and last-known-good product |
| Dirty inherited work is overwritten | Status/path classification; no reset/clean/bulk checkout |

## Decision log

Record decisions here during implementation. Each entry includes date, slice,
alternatives, evidence, selected production/workload policy, fallback, and ADR impact.

- **2026-08-28 — Planning direction:** prioritize persistent GPU scene, removal of
  source-submesh CPU scaling, conservative visibility, LOD, indirect submission,
  fidelity-preserving data efficiency, and flexible conventional/advanced paths.
  Treat compact vertices, VSM, visibility resolve, DirectXMath, async compute, and
  compiler tiers as measured candidates rather than assumptions.
- **2026-08-28 — Shadow interpretation:** M7 VSM means Virtual Shadow Maps. Existing
  cascades/atlases/cubes remain reference and fallbacks. Promotion is per
  workload/quality profile and requires matched 4K quality/performance/memory proof.
- **2026-08-31 — M7.7 conventional fidelity order:** harden the conventional
  reference before M7.8 comparison. Use project/profile-owned directional coverage,
  engine-derived stable cascade splits and world-units-per-texel diagnostics; use
  bilinear reconstructed PCSS filter comparisons; retain raw blocker depth; combine
  texel-scaled, receiver-plane, and bounded geometric-normal bias; and make Cinematic
  reachable. Keep conventional and virtual shadows selectable rather than rendering
  redundant full coverage. Temporal sampling/reprojection remains M9.
- **2026-09-12 — M7.7 density/filter slice accepted:** project/profile coverage
  now caps camera-relative directional reach, practical/log splits expose exact
  density counters, PCSS final taps bilinearly reconstruct comparison results, and
  Cinematic is reachable through Ultra-authored lights. The 35 m validation
  fixture resolves 0.000865/0.001955/0.004999/0.016406 m per texel across its four
  cascades with clean Vulkan validation. Ultra PCSS measures 1.131520 ms median
  deferred lighting at native 4K versus 0.456704 ms for fixed PCF. Existing bias
  remains unchanged; receiver-plane bias and independent consumers remain open.
  No ADR changes.
- **2026-09-12 — M7.7 receiver bias slice accepted:** retain the existing raster
  bias and add bounded per-sample receiver-plane correction plus a front-facing
  geometric-normal offset, all scaled by cascade world texels. The grazing A/B
  changes only 0/0/0 versus 1.25/2.0/0.5 texels: the default removes dense acne
  while preserving the projected shadow. Deferred lighting is 0.125952 ms median
  in both 1280x720 validation runs. Capture metadata and profiler counters record
  every term. This implements ADR-0010 decision 4; no new ADR is required.
- **2026-09-24 — M7.7 warmed probe-LOD gate completed without admission:** add a
  fixture-owned standalone 1024-pixel realtime capture so all moving high-detail
  primitives contribute to visible glossy reflections. Repair partially recorded
  cube lifetime/progress by finishing a ticket before observing newer dependency
  revisions. A 16-pixel bound removes 0.9263% of retained capture triangles with
  exact device/oracle agreement and byte-identical 4K output, but reversed-order
  Release pairs show no repeatable performance or memory win. Retain LOD0 globally
  and keep radial probe LOD experimental. ADR-0010 already governs the decision;
  no ADR change.
- **2026-08-28 — CPU math interpretation:** do not replace GLM globally. Benchmark
  isolated bulk kernels and adopt only behind an internal boundary when full-frame
  or scaling evidence is material.
- **2026-08-28 — Fresh-lead M7.0 audit:** preserve the existing whole-model schema
  through M7.2, introduce GPU-scene records beside rather than inside the 240-byte
  `DrawPacket`, establish high-precision arenas before mixed-width/compact products,
  and defer child-product externalization until parent-contained LOD behavior is
  proven. Existing spot/point/probe culling remains an independent fallback/oracle;
  main and directional visibility are confirmed gaps. No ADR change is required.
- **2026-08-28 — Baseline status:** the inherited source builds and passes 71/71
  tests in both Debug and Release on the RTX 4090 target. The owner-observed
  three-asset scene cannot yet be reproduced from a durable scene/camera artifact;
  M7.0 will freeze an explicit three-asset fixture and record any difference from
  that observation rather than relabeling the observation as measured evidence.
- **2026-08-28 — M7.0 accepted:** the frozen replacement fixture measures materially
  faster than the unsaved owner observation, so it is the only reproducible entry
  baseline. Future comparisons use its exact five-run manifest and hashes. Pending
  heterogeneous fixture roles remain named obligations of their owning slices.
- **2026-08-29 — M7.1 ABI:** accept ADR-0014. Use non-wrapping 64-bit scene handles,
  cold UUID/GUID identity tables, explicit affine transforms, separate instance,
  instantiated-primitive binding, and shared-geometry records, independent consumer
  masks, per-frame-context revision comparison, and direct fail-visible fallback.
- **2026-08-29 — M7.2 publication:** accept persistent per-frame-context Vulkan
  tables and revision-range uploads. Keep M6 transparency and runtime instance
  batches direct, use packed records for ordinary opaque direct parity, and retain
  the legacy RHI geometry-handle bridge only until M7.3 arenas replace it.
- **2026-08-29 — M7.3 geometry efficiency:** accept schema-6 model products with a
  versioned stable-identity arena, explicit base vertex, and split UInt16/UInt32
  streams. Adopt duplicate filtering and cache reordering only for opaque/masked
  work and only under exact/strict proof; preserve transparent order. Retain the
  72-byte high-precision vertex, defer position-only shadow streams to M7.7, and
  require no ADR change because ADR-0003 and ADR-0006 already govern the result.
- **2026-08-29 — M7.4 indirect parity stage:** retain M7.4 as active. Conservative
  instance/primitive frustum rejection, explicit indirect capabilities, a versioned
  20-byte command ABI, deterministic physical/material bins, indexed indirect-count
  consumption, bounded capacity, tiny/direct policy, and exact image parity are
  proven. Do not accept the slice until device compute replaces dense-scene host
  visibility/command compaction and the frozen native-4K multi-run gate passes.
- **2026-08-30 — M7.4 device compaction:** device compute now performs conservative
  primitive-frustum rejection and atomically generates per-bin indexed commands
  and counts. Fence-delayed telemetry proves exact device/oracle parity with zero
  mismatched bins and zero overflow for one-visible, all-visible, and 256-instance
  stress fixtures. Retain M7.4 as active only for clean native-4K qualification:
  the first five-run attempt was stopped after external GPU contention produced a
  non-comparable whole-frame distribution despite a 0.008512 ms compaction median.
- **2026-08-30 — M7.5 selection oracle:** begin M7.5 without relaxing the pending
  M7.4 qualification gate. `GpuSceneLodAbiVersion` 1 defines deterministic
  projected-geometric-error selection, directional hysteresis, explicit hero
  maximum-LOD override, coarser-resident-first fallback, finer-resident fallback,
  and invalid-input fail-visible behavior. This CPU oracle is accepted only as the
  reference policy for later cooked LOD chains and device selection; no generated
  LOD product or production triangle reduction is claimed yet.
- **2026-08-30 — M7.4 accepted:** accept the device-compute indirect path after a
  clean five-run native-4K replacement set. Every run proves 303/303 exact
  device/oracle parity with zero mismatch, overflow, dropped frames, or steady C++
  allocations. The median run-average wall time improves 60.48% from M7.0, GPU
  median improves 5.35%, and CPU median remains flat after replacing deep
  observation comparison and capacity-wide retirement scans with exact caller
  revisions and sparse retired-slot collection. Preserve direct/tiny and
  conventional consumer fallbacks; advance M7.5. No ADR change is required.

- **2026-08-30 — qualification repair within M7.5:** the old `2073f5...` capture
  contains zero nonzero RGB samples. Benchmark startup parsed but did not apply
  `constant_linear_rgb`, selecting the neutral-black environment instead. Preserve
  the old timings as unlit-workload evidence and reopen lit visual/performance
  qualification; do not credit black-image equality as fidelity acceptance.
  Implement the declared constant fixture through the existing environment product,
  AP1 conversion, shared BRDF integration, and asset-owned upload path. No second
  lighting system or ADR change is introduced. M7.5 remains the sole active slice.
- **2026-09-02 — M7.5 production admission held:** the topology-transactional
  artifact passes strict static near/mid/far comparison and 240-frame motion/cut
  validation. The 15% hysteresis margin reduces recorded LOD changes by 45.7933%
  with exact device/oracle selection. Do not promote the current path: the
  alternating five-process native-4K gate reduces main-view triangles by only
  2.8282% and regresses median wall time by 1.5206%. GBuffer GPU raster time is
  unchanged; CPU selection and GPU compaction account for the regression. Preserve
  LOD0 default, separate qualification-oracle overhead from the deployable path,
  implement physical coarser-resident fallback, and repeat the frozen gate. No ADR
  change is required.
- **2026-09-02 — M7.5 deployable/oracle split accepted:** exact CPU LOD command
  reconstruction now runs automatically with Vulkan validation or through
  `--gpu-lod-qualification-oracle`; quality provenance records the effective mode.
  The deployable path retains visibility parity but omits LOD expected-command
  construction, command readback, and CPU history/oracle work. GPU selection also
  reuses invariant bounds projection across a chain. Strict device/oracle and image
  equality remain exact. The repeated native-4K set improves the 2-pixel route from
  +1.5206% wall time to effective parity at +0.0378%, but GPU median is still +0.5642%
  and tails regress. Accept the overhead repair, keep LOD0 default, and retain M7.5
  for physical fallback and a materially beneficial workload. No ADR change.
- **2026-09-02 — M7.5 physical coarse fallback accepted:** the qualification-only
  `--gpu-lod-minimum-resident-level` floor compacts fine index ranges out of the
  parent arena before Vulkan allocation and rebinds the stable canonical primitive
  to its first retained coarse range. The native-4K floor-one gate withholds
  1,737,714 bytes across 70 chains, reduces the forced base from 604,836 to 482,328
  triangles, and retains 303 exact commands with zero direct fallback, LOD mismatch,
  or visibility-bin mismatch. The complete image changes 0.143856% of pixels at
  0.9999913623 mean luma SSIM. Accept this as physical fallback proof, not progressive
  streaming: M7.9 still owns independent child CookKeys and vertex/index residency.
  Floor zero and LOD0 remain default; M7.5 remains active for a materially beneficial
  production workload. No ADR change.
- **2026-09-02 — M7.5 workload-selectable admission accepted:** the existing frozen
  `m7_many_instance_stress_v1` workload supplies 256 dense instances without
  inventing a favorable replacement fixture. The alternating five-process-per-route
  native-4K gate removes 12,650,344 of 51,612,672 main-view triangles (24.5102%) and
  improves median complete-frame wall time by 0.310995 ms (1.4487%), CPU median by
  0.4550 ms, GPU median by 0.871328 ms, and CPU/GPU p95 and p99. Four of five matched
  pairs favor the candidate; the retained reversal prevents a global-default claim.
  Validation records exact device/oracle selection, zero fallback/mismatch/overflow,
  unchanged memory, and 0.178494% changed pixels at 0.9999969265 mean luma SSIM.
  Accept M7.5 as workload-selectable, retain LOD0 globally, and advance M7.6. M7.7
  still owns shadow/probe LOD and M7.9 owns independent child residency. No ADR
  change.
- **2026-09-02 — M7.6 conservative pyramid contract accepted:** the RHI now defines
  full extent/mip behavior, forward/reverse farthest-depth reduction, proportional
  odd-resolution footprints, a bounded four-texel CPU query oracle, and stable
  fail-visible history reasons for resize, stale data, projection uncertainty, cuts,
  rapid motion, near-plane intersection, new residency, and transform changes. Full
  Debug/Release suites pass 77/77 and the visible validation launch remains clean.
  The live build and query resources are available only under explicit experimental
  flags; no occlusion rejection is enabled. M7.6 remains active for device-owned
  compaction integration and temporal fixture admission. No ADR change.
- **2026-09-12 — M7.6 workload-selectable admission accepted:** a frozen
  64-instance depth stack with one enlarged nearest occluder removes 5,133 of
  6,464 opaque commands at native 4K. The final matched deployable pair reduces
  GBuffer median from 0.741376 ms to 0.081920 ms, complete GPU median from
  2.133760 ms to 1.471840 ms, and CPU median from 5.455600 ms to 4.828700 ms.
  Independent validation reports exact per-bin parity and zero invalid, unsafe,
  or overflow results; the scene captures are byte-identical. Qualification-only
  projection, query, readback, writes, and full-capacity buffers are separated
  from the deployable route. Accept M7.6 as workload-selectable, retain the global
  default-off policy, and advance M7.7. No ADR change.

- **2026-10-02 — Program pause and reschedule (owner decision):** after the
  takeover audit, pause M7 at the committed M7.8 checkpoint (GPU residency reference
  rejected; scalable residency, page raster, and sampling not started). The raster
  target becomes 144 FPS at native 4K (6.94 ms); the shadow row drops to 1.0 ms.
  Order: M7R architecture consolidation, M9 temporal AA/motion vectors/bloom/auto-
  exposure, then M7.9-M7.12, M8, and M7.8 Virtual Shadow Maps resumed on meshlet
  caster submission (the plan's own "VSM costs more before M8" risk). All M7.8 code
  stays default-off and must keep compiling; conventional shadows remain production.
  No ADR is superseded; ADR-0010's VSM direction stands with revised timing.

## Completion report requirements

Latest 2026-09-29 continuation: the moving-camera contact matrix closes M7.7.
Thin, fully clipped alpha-mask, double-sided, grazing, close-contact, bias, and
cascade-density transition coverage is validation-clean; 18-36 device commands
match the independent oracle with zero mismatch, overflow, or fallback. Direct and
automatic native-4K captures are byte-identical at both sweep extremes. M7.7 is
accepted and M7.8 is active. See
`docs/performance/M7.7-shadow-quality-closure-2026-09-29.md`. No ADR is superseded.

The 2026-09-29 M7.8 storage continuation maps the accepted residency contract to a
default-off Vulkan allocation: 1,024 bordered D32 pages and a 65,536-entry table.
Validation is clean, both allocations have dedicated persistent memory categories,
and the exact enabled/disabled engine-owned delta is 72.500 MiB. Conventional
shadows remain the only sampled representation. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`. No ADR is
superseded.

The following M7.8 receiver-marking continuation advances the ABI to v2 with signed
world-page addresses and snapped per-level origins. The CPU oracle deterministically
selects the finest containing directional clip, falls back at guarded edges,
deduplicates marks, and reports invalid, unmapped, and capacity-dropped coverage.
This preserves cache identity during ordinary camera clipmap scrolling. GPU marking
and sampling remain disabled. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`.

The raw Vulkan directional-marking stage now passes Debug/Release RTX 4090 parity
against the CPU oracle. Its std430 records expose mapped, unmapped, invalid, and
address-overflow states and preserve signed world-page identity across a snapped
clip scroll. Its separate deterministic GPU reference compactor now passes the same
hardware gate for duplicate merging, priority/coverage/address ordering, capacity
overflow, dropped coverage, saturation, and ABI/level rejection. This is hardware
qualification only: the reference kernel is intentionally single-invocation, while
scalable parallel compaction, page raster, and sampling remain outside runtime
frames.

The first multi-workgroup compaction candidate is rejected despite exact oracle
parity. Its forward-progress-safe address sort, segmented reduction, and rank sort
pass both a 4,096-receiver fixture and a 65,536-unique-mark capacity fixture, but
the two global bitonic networks require 272 compare/exchange dispatches. Five
timestamped full-capacity runs after warm-up measure 841.992 ms Debug and
837.758-844.757 ms across repeated Release medians on the RTX 4090. The shader
remains qualification-only, its scratch
record is test-local, and runtime integration waits for a hierarchical/radix or
bounded top-K replacement.

The replacement hierarchical candidate is now admitted for standalone compute
qualification. Shared-memory 256-entry block sorts and eight global merges per
full-capacity ordering retain exact oracle results; a two-stage saturated reduction
replaces the rejected contended telemetry atomic. Five device-local timestamped
runs after warm-up measure 0.145792 ms Debug and 0.145984 ms Release median for
65,536 unique marks with a 4,096-request cap on the RTX 4090. Persistent device-
local storage now exists independently for both frames in flight behind the default-
off resource switch. The 2026-09-30 continuation also gives this owner immutable
per-frame descriptors and both compute pipelines. The shared direct receiver-marker
and compactor chain passes exact populated/empty two-slot reuse with synchronization
validation, measuring 0.148512/0.147264 ms Debug/Release at 65,536 receivers. Runtime
promotion still requires live depth-driven receiver/clip publication and whole-frame
evidence; page raster and sampling remain disabled. See
`docs/performance/M7.8-persistent-mark-compact-chain-2026-09-30.md`.

The subsequent depth receiver checkpoint adds an explicit source extent/pixel-
region RHI contract, CPU reconstruction oracle and sampled-D32 Vulkan producer.
It chains every region pixel into the admitted marker/compactor and exactly matches
requests through forward/reverse depth, invalid values, odd offset regions and
perspective/singular cameras. Current work is full-view page-demand accumulation
and live directional clip publication before render-graph scheduling; the current
65,536-receiver cap cannot justify implicit pixel subsampling. See
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


The 2026-09-12 continuation: the controlled large-occluded-workload gate closes
M7.6. At native 4K, fused Hi-Z rejection removes 79.4090% of opaque commands and
improves median complete GPU/CPU frame time by 0.661920/0.626900 ms with identical
scene bytes. Validation retains the complete independent oracle and passes exactly;
the deployable route avoids its per-candidate CPU projection, standalone dispatch,
readback, result write, and full-capacity buffers. M7.6 is accepted as
workload-selectable, remains globally default-off, and advanced work to M7.7. See
`docs/performance/M7.6-large-occluded-admission-2026-09-12.md`. No ADR is superseded.

Latest 2026-09-02 continuation: M7.6 begins with the shared conservative depth-
pyramid contract and deterministic CPU oracle. Farthest-depth reduction is explicit
for forward/reverse Z, odd extents cannot drop edge samples, queries touch at most
2x2 texels, and every unsafe previous-frame state fails visible with a stable reason.
Full Debug/Release builds and 77/77 tests pass; a visible 30-frame Vulkan-validation
launch through `launch-engine.bat` exits cleanly. No runtime Hi-Z image or culling is
enabled yet. See `docs/performance/M7.6-depth-pyramid-contract-2026-09-02.md`. M7.6
remains active and no ADR is superseded.

Previous 2026-09-02 continuation: the frozen 256-instance stress fixture closes M7.5's
remaining complete-path admission gate. Across five fresh processes per LOD0 and
2-pixel route, 100,000 measured native-4K frames reduce median wall time from
21.466823 to 21.155828 ms (-0.310995 ms / -1.4487%), CPU median from 21.5387 to
21.0837 ms, and GPU median from 7.829536 to 6.958208 ms. CPU/GPU p95 and p99 improve;
memory is identical. The candidate removes 24.5102% of 51,612,672 triangles with
exact validation/oracle parity and changes 0.178494% of pixels at 0.9999969265 mean
luma SSIM. Four of five pairs win and one reverses, so accept generated LOD as
workload-selectable while retaining LOD0 globally. M7.5 is accepted and M7.6 is
active. See `docs/performance/M7.5-material-workload-admission-2026-09-02.md`. No ADR
is superseded.

Previous 2026-09-02 continuation: a qualification-only physical residency floor removes
fine index ranges before GPU upload while retaining canonical primitive/material
identity and rebinding its draw to the first coarse resident payload. Floor one on the
frozen native-4K fixture applies to 70 chains and saves exactly 1,737,714 live index
bytes. Device and CPU oracle both draw 482,328 triangles through all 303 commands,
with zero direct fallback or mismatch. The complete image changes 0.143856% of pixels
from physical floor zero at mean luma SSIM 0.9999913623. Full Debug/Release suites pass
76/76. Accept physical coarse fallback proof while retaining parent vertex residency
and fixed load-time policy only; M7.9 owns independent child streaming. See
`docs/performance/M7.5-physical-coarser-residency-2026-09-02.md`. Default remains
floor zero/LOD0; M7.5 stays active for a materially beneficial workload. No ADR is
superseded.

Previous 2026-09-02 continuation: qualification-only CPU LOD command reconstruction is
separated from the deployable GPU selector, with automatic validation enablement, an
explicit no-validation evidence switch, `_oracle_0/1` provenance, and invariant
base-bounds projection reuse. The alternating native-4K set measures 3.127328 ms at
maximum LOD 0 and 3.128511 ms at 2 pixels (+0.0378%); complete GPU median remains
+0.5642% and candidate tails regress. See
`docs/performance/M7.5-lod-oracle-separation-2026-09-02.md`.

Previous 2026-09-02 continuation: expanded native-4K admission uses the current
topology-transactional artifact in the three-asset near/mid/far and 240-frame
oscillation/camera-cut fixtures. Static LOD0 and 2-pixel captures are byte-identical;
the motion frame with maximum reduction changes 251 of 8,294,400 pixels with mean
luma SSIM 0.9999979771 and no inspected topology/material defect. Fifteen-percent
hysteresis reduces history changes from 832 to 451, with exact device/oracle results,
full cut reset, and zero overflow. The alternating five-process native-4K performance
set rejects production admission: 2.8282% fewer triangles costs 1.5206% more median
wall time, median GBuffer GPU time is unchanged, and CPU selection plus GPU
compaction account for the overhead. See
`docs/performance/M7.5-topology-admission-2026-09-02.md`. Default rendering remains
LOD0; M7.5 stays active for deployable-path overhead reduction and physical
coarser-resident fallback. No ADR is superseded.

Previous 2026-09-01 continuation: a separately opt-in transactional topology gate now
applies the mesh link condition before each collapse while retaining the final
manifold/component/Euler/boundary proof. A valid closed-torus regression proves
topology-changing merges are rejected without preventing a disconnected reducible
component from producing deterministic levels; canonical raster prefixes, materials,
and RT streams are explicitly frozen by the product test. The representative Alfa
Romeo artifact grows from 57 to 70 chains and from 180,628 to 289,619 covered base
triangles. Native-4K lit main-view reduction improves to
7.7677/16.5342/25.3730% at 2/4/8 pixels with exact device/oracle selection and no
overflow. The 2-pixel result changes 896 of 8,294,400 pixels with mean luma SSIM
0.9999728463; the deliberately loose 8-pixel result reaches 0.9998812261 and remains
diagnostic. Only 12 eligible primitives / 6,838 triangles now fail to produce a
level. A geometry-policy edit nevertheless forced 76 texture recooks (457.241 of
462.547 cook seconds) and 35 texture payload hashes changed, confirming the M7.9
fine-grained child-CookKey requirement. See
`docs/performance/M7.5-topology-transactional-lod-cooking-2026-09-01.md`. No ADR is
superseded; default rendering remains LOD0 and M7.5 stays active pending multi-asset
temporal/fresh-process admission and physical residency fallback.

Latest 2026-09-01 continuation: permanent primitive/triangle-weighted LOD rejection
analysis identified boundary-degree topology as the dominant blocker rather than
small primitive fragmentation. New independently opt-in boundary-fan repair and
transactional per-merge orientation/correspondence validation preserve canonical
LOD0, RT geometry, default settings, legacy rejection, and all final topology and
error guards. Combined with bounded boundary collapse, the Alfa Romeo artifact grows
from 10 to 57 chains and native-4K lit main-view triangle reduction improves from
0.4560/1.3504/2.4549% to 6.9988/13.4023/20.1350% at 2/4/8 pixels. Device/oracle
selection remains exact; LOD0 is byte-identical and 8-pixel mean luma SSIM is
0.9999392204. The stronger cook costs approximately 3.365 seconds of LOD work and
remains experimental. Twenty-five sufficiently large opaque primitives containing
115,829 triangles still produce no level, so broader temporal/performance admission
remains open. See
`docs/performance/M7.5-transactional-lod-cooking-2026-09-01.md`. Debug/Release pass
76/76, and the normal launcher opens a visible validation-enabled Release window
from outside the repo. No ADR is superseded; M7.5 stays active.

Latest 2026-09-01 continuation: importer version 8 now offers explicitly opt-in,
bounded source-boundary edge collapse while preserving locked-boundary defaults,
canonical LOD0/RT geometry, legacy rejection, and historical CookKeys. The Alfa
Romeo artifact grows from 5 to 10 generated chains; native-4K lit reduction improves
from 0.2275/1.2367/2.0369% to 0.4560/1.3504/2.4549% at 2/4/8 pixels with exact
device/oracle parity. LOD0 remains byte-identical, and the 8-pixel result changes
803 of 8,294,400 pixels with mean luma SSIM 0.9999989552. This is a conservative
experimental improvement, not production admission: only 9.17% of canonical
triangles participate in chains. See
`docs/performance/M7.5-boundary-aware-lod-cooking-2026-09-01.md`. M7.5 stays active
and no ADR is superseded. Full Debug/Release suites pass 76/76, and the normal
launcher starts a visible validation-enabled Release window from outside the repo.

Later 2026-08-31 continuation: bounded stable-handle main-view device history now
adds a 15% coarsening margin without relaxing the requested error ceiling. The
240-frame motion fixture reduces recorded LOD changes from 58 to 19, with exact
GPU/CPU command parity and camera-cut reset. Nine final synchronization-validation
runs cover 536 retained frames, including 504 LOD parity frames, populated
transparency, display transport switching and a separate Ordinary2 resize check.
A pre-existing read-only forward-depth STORE hazard was reproduced with LOD off
and repaired using Vulkan 1.3 STORE_OP_NONE; depth-writing forward remains STORE.
See `docs/performance/M7.5-lod-history-2026-08-31.md`. Both builds pass 76/76 tests.
Default stays LOD0; streaming, hero controls and visual/performance admission remain
open. No ADR is superseded and M7.5 remains the only active slice.
The final five-process-per-route static comparison measures 1.394311 ms default
versus 1.432656 ms experimental wall time (2.75% slower including diagnostics).
The normal launcher also opens a responding Release window from a non-root
directory and closes cleanly; detailed hashes and provenance are in that report.

Latest 2026-08-31 continuation: an additive typed-light benchmark contract and
explicitly lit oblique LOD fixture now exercise warm spot, cool point, directional,
clustered-lighting, and shadow paths without rewriting frozen manifests. Native-4K
2/4/8-pixel captures retain exact device/oracle parity and mean luma SSIM above
0.999999, but remove only 0.2275/1.2367/2.0369% of main-view triangles. This is
diagnostic fidelity evidence, not production admission; strengthen deterministic
bounded-error cooking before considering a default threshold. See
`docs/performance/M7.5-lit-lod-admission-fixture-2026-08-31.md`. Both builds pass
76/76 and all Vulkan captures are validation-clean; M7.5 stays active.

Earlier 2026-08-31 continuation: Mesh source/cooked schema 2 and GPU-scene ABI 2 now
carry a transactional per-content maximum LOD ordinal. LOD0 pins hero content;
LOD15 preserves the existing project/device policy, and the Vulkan candidate uses
the minimum authored, global, and compatible-resident limit. Source v1 migrates to
the unchanged default, frozen M4/M5 hashes remain historical with explicit M7.5
supersessions, Debug/Release pass 76/76, and a visible 30-frame Release validation
launch exits cleanly. See
`docs/performance/M7.5-hero-lod-policy-2026-08-31.md`. Physical streaming,
independent shadow/probe LOD, and production visual/performance admission remain
open; M7.5 stays active.

Earlier 2026-08-31 checkpoint: shared LOD links and arena-relative indirect addressing
now feed an opt-in **stateless experimental** device selector with exact command
readback parity. Default launches retain LOD0. The frozen frontal depth fixture
self-occludes, so an additive oblique fixture supplies visible-silhouette evidence.
See `docs/performance/M7.5-device-lod-2026-08-31.md`. Per-view device hysteresis,
streaming fallback and quality/performance admission remain open; M7.5 stays active.
The five-process final oblique comparison is 1.282390 ms LOD0 versus 1.367980 ms
experimental LOD including diagnostics: no production performance admission.
Debug/Release pass 76/76; the normal launcher and live scRGB/HDR10/SDR switching
are verified. The rejected uncached-upload-memory CPU read path and its hot-mirror
repair remain documented with their original measurements.

Earlier M7.5 cooking/oracle evidence is in
`docs/performance/M7.5-lod-selection-oracle-2026-08-30.md`: typed schema-7 LOD
children, opt-in deterministic cooking, explicit importer-v7 compatibility,
dense-reference upload revision repair, and populated direct/indirect capture
parity. The 2026-08-31 report advances device selection experimentally. The Debug/Release texture codec
produces different payload bytes under the same legacy CookKey; M7.9 must address
that determinism/toolchain-identity issue without rewriting frozen products.
Small-scene lit timing does not support a blanket indirect performance claim;
retain the direct reference and measure workload admission before cutover.

The final M7 acceptance report records:

- changed user-visible behavior and architecture;
- exact source/schema/RHI/shader interfaces affected;
- production, selectable, experimental, rejected, and deferred optimizations;
- Debug/Release/test/validation results;
- before/after CPU, GPU, memory, draw, triangle, upload, residency, and p95/p99 data;
- image and temporal comparisons;
- conventional versus virtual shadow results;
- conventional deferred versus visibility-resolve results;
- DirectXMath/SIMD/compiler experiment outcome;
- remaining risks and later M8-M11 integration requirements;
- ADR, roadmap, and frame-budget changes;
- a durable M7-to-M8 handoff.
