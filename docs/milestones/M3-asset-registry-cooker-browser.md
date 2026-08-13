# M3 Execution Plan: Asset Registry, Cooker, and Browser

- Roadmap milestone: M3
- Status: Accepted on 2026-07-31; M3.1-M3.7 are accepted
- Lead: milestone lead task; one integration owner for asset identity, cooker,
  runtime loading, RHI indexing, and editor migration
- Last updated: 2026-07-31
- Dependencies: M0, M1, and M2 accepted; M3 hard precondition passed
- Relevant ADRs: ADR-0004 (primary), ADR-0001, ADR-0003, ADR-0005, and
  ADR-0006; ADR-0002 constrains output-domain behavior
- Performance contract: `docs/performance/FRAME_BUDGET.md`
- Source baseline: `30252593f8fdd2de5dffbb8da31bb570ff49a7c0`,
  intentionally dirty accepted M0-M2 worktree
- Approval: the owner approved all seven recommended decisions on 2026-07-25 and
  subsequently directed the lead to continue through the sequential slices.

## Objective and user-visible outcome

Iridium will treat source files as inputs, not runtime identities. Artists can import
or reimport models and textures through an Asset Browser, move or rename sources
without breaking references, inspect deterministic errors, search and filter assets,
see cached thumbnails, and drag assets onto supported assignment targets. Scenes and
runtime objects refer to stable asset GUIDs.

Headless and editor workflows will use the same importer registry, canonical settings,
dependency graph, cooker, and derived-data keys. A warm load consumes versioned
runtime blobs without reparsing source. GPU upload and residency are scheduled
separately from source parsing and CPU cooking.

Materials and textures become scalable indexed resources with explicit image-view,
sampler, transfer/color, and texture-use semantics. Cooked model data preserves
primitive identities and spatial bounds instead of destructively merging disconnected
surfaces by material. The same products remain suitable for M6 transparent ordering,
M7 indexed visibility/material resolve, M8 meshlet generation, and M10 acceleration
structures without a second asset database or semantic recook.

M3 acceptance requires that assets can be imported, reimported, referenced by GUID,
assigned from the browser, and rebuilt bit-deterministically from source plus metadata.
It does not require the M4 scene/ECS rewrite, M5 lighting, M6 transparency algorithm,
or M7 visibility renderer.

## Hard precondition

The sample-car wheel/tire regression is resolved before M3 implementation:

- the source material is ordinary opaque material 84, with two source primitives
  totaling 47,880 indices;
- the selection and production passes use the same geometry and index range;
- the M2 canonical-routing override incorrectly disabled depth writes for the
  standard deferred `Opaque` queue;
- depth writes now derive from the final render queue: `Opaque` and
  `ForwardOpaque` write depth, while `Transparent` does not;
- a deterministic material-ID/depth capture gate fails the old capture
  (545,427 of 545,427 material-84 pixels at background depth) and passes the
  corrected capture (516,502 pixels, zero at background depth);
- Debug and Release pass 20/20 CTest, tracked material fixtures validate at 4K,
  and the revision-2 car capture set is validation-clean.

The complete evidence, primitive ranges, hashes, and performance data are in
`docs/milestones/M2-acceptance-report-2026-07-25.md`. This was an owning M2
material-routing fix, not an M3 asset-architecture change. The owner subsequently
approved the seven M3 architecture decisions and authorized execution. M3.1-M3.7
are accepted; the criterion-level result is recorded in
`docs/milestones/M3-acceptance-report-2026-07-31.md`.

## M3-start context and audited implementation

Repository source was reinspected after the complete M0-M2 handoff and every accepted
ADR. The path at M3 start was:

```text
filesystem path
  -> AssetManager::getModel(path) string-keyed cache
  -> AssetManager::loadModelFromFile
       -> fastgltf source parsing
       -> SourceMaterialDocument + M2 material compilation
       -> texture decoding + CPU mip generation
       -> immediate RHI texture/material allocation
       -> vertex/index extraction, transform baking, merge-by-material
       -> immediate RHI geometry allocation/upload
  -> shared_ptr<ModelAsset>
  -> MeshComponent + path-based SceneSerializer
  -> per-submesh DrawPacket
  -> Vulkan material table and per-material descriptor sets
```

### Identity and registry

- `AssetManager::modelCache` is keyed by the caller's path string. Equivalent relative,
  absolute, or differently normalized paths can be separate identities.
- `ModelAsset::filePath`, `MeshComponent::requestedMeshPath`, and the scene
  serializer's `meshPath` field make filesystem location the persistent reference.
- There is no asset GUID, metadata sidecar, importer registry, source hash graph,
  subasset identity, catalog, or rebuildable derived-data index.
- M2 material local indices are stable only within one imported document. They are
  not durable asset identities across source edits.

### Importing, cooking, and failure behavior

- `AssetManager::loadModelFromFile` combines file I/O, source parsing, validation,
  material compilation, image decoding, mip generation, vertex conversion, transform
  baking, GPU allocation, cache insertion, and editor callback notification.
- The importer is glTF-specific and selected implicitly by the called function. There
  is no stable importer ID/version or canonical import-settings object.
- Source and CPU work occur synchronously on the caller. A warm process avoids a
  second import through the path map, but a fresh process reparses and recooks all
  sources.
- Errors primarily throw exceptions or become M2 material diagnostics. There is no
  persistent asset status, dependency-invalid state, retry policy, or deterministic
  error record for the browser.

### M2 material and texture path to preserve

- `SourceMaterialDocument`, `CompiledMaterial`, `MaterialInstance`, and schema-2
  `PackedGpuMaterial` are valid, separate M2 contracts. M3 must store or reproduce
  them without collapsing source values, defaults, compiler decisions, instance
  overrides, or GPU packing.
- A packed material is 832 bytes with 21 explicit texture uses. Texture operations
  preserve semantic, UV set, transform, scalar, sampler behavior, and sRGB/linear
  transfer intent.
- Texture storage/view/sampler identity is not yet separated. `TextureHandle` owns
  one Vulkan image view and one sampler, and `SamplerHandle` is currently synthesized
  from the texture handle.
- Each material allocates two Vulkan descriptor sets containing 21 combined image
  samplers plus the material storage buffer. The packed texture indices are not a
  global shader-visible texture table.
- The Vulkan material buffer has a fixed 4,096-record capacity. This is adequate for
  M2 fixtures but not the M7 material-resolve destination.

### Geometry path and future-consumer gaps

- `Vertex` is a 72-byte interleaved runtime vertex with position, color, normal,
  UV0, tangent, and UV1. Geometry is uploaded as one model-wide vertex/index pair.
- The importer initially records source primitives, but `mergeMaterials` bakes node
  transforms and combines every part sharing a material into one `SubMesh`.
- This erases source primitive identity, per-primitive local bounds, node association,
  and disconnected-surface boundaries. It is especially invalid for transparent
  sorting and must not become the cooker contract.
- There are no cooked sections for canonical positions/indices, primitive bounds,
  topology provenance, LODs, optional meshlets, or ray-tracing geometry inputs.
- `GeometryDesc` describes only vertex stride and index format; allocation immediately
  produces a backend handle. No CPU runtime geometry blob survives independently.

### GPU upload and residency

- RHI allocation accepts complete byte spans and returns a handle synchronously.
  Vulkan staging and submission are backend-owned, but source import directly triggers
  resource creation and the startup path waits for completion.
- `AssetManager` owns model-created material, texture, and geometry handles and frees
  them at shutdown. There are no asset load states, upload tickets, residency budgets,
  pins, evictions, nonresident fallback bindings, or dependency-aware hot swaps.
- Generation-stamped RHI handles correctly reject stale CPU references. Shader-visible
  raw indices still need delayed slot reuse because GPU records do not carry the CPU
  generation.

### Editor and scene workflow

- Scene Hierarchy owns an `Import Model...` file dialog, calls `getModel(path)`, and
  creates a scene entity.
- `MeshComponent` contains ImGui controls and native file-dialog behavior, exposes a
  typed path buffer, and schedules path-based swaps.
- `SceneSerializer` writes `meshPath` and restores it into
  `requestedMeshPath`. It has no versioned asset-reference migration.
- M3 must move import, search, thumbnail, drag/drop, and assignment responsibility to
  the Asset Browser. M4 still owns the general component-drawer and scene-schema
  architecture rewrite.

### Measured baseline

- The corrected revision-2 sample car imports 87 materials, 80 persistent texture
  allocations, one geometry allocation, 20.4 MB of vertex data, 3.65 MB of index
  data, and approximately 116.4 MB of requested texture storage.
- Its current fresh-process source import is approximately 4.9 seconds in the Debug
  diagnostic and submits approximately 140.5 MB through upload staging.
- The corrected 4K Release car run over 10,000 measured frames records 0.955 ms
  median / 1.514 ms p95 / 1.612 ms p99 GPU frame time, 5.050 ms average wall
  frame, and 1.186 ms median CPU frame time. Its 16.062 ms CPU p95 is dominated
  by a 15.562 ms swapchain-acquire wait in the hidden presentation loop, not
  render preparation.
- The accepted M2 material-lab baseline remains the authoritative matched renderer
  comparator: 0.455296 ms median GPU frame, 0.137216 ms for GBuffer plus deferred
  lighting, and zero steady C++ allocations.

## Invariants

- Asset identity is a stable opaque GUID. A path is discoverable source location,
  never persistent identity, runtime identity, or shader identity.
- Source-controlled metadata sidecars are authoritative for identity and import
  settings. The editor catalog and DDC are rebuildable derived state.
- Source parsing, validation, CPU cooking, runtime blob loading, GPU upload, and
  residency are distinct stages with explicit ownership and diagnostics.
- Import and cook output depends only on versioned importer/cooker code, canonical
  settings, declared platform/profile, source bytes, and transitive dependency
  hashes. Wall clock, enumeration order, thread scheduling, absolute paths, and
  machine-local state cannot affect output bytes.
- M2 source/compiled/instance/packed material distinctions remain intact. No cooker
  converts canonical closure data back into authoring-workflow shader parameters.
- Texture color interpretation belongs to a use/view. Image storage, image view,
  sampler, material use, and asset identity remain mechanically distinct.
- Lighting and material data remain scene-linear ACEScg/AP1 through the one M1 output
  boundary. Thumbnails may be display encoded only as explicitly tagged preview
  products.
- Catalog, dependency, cooker, runtime-blob, upload, and residency contracts are
  backend neutral. Vulkan descriptor layouts, image-format feature queries, barriers,
  staging implementation, queues, and timeline values remain in the Vulkan backend.
- Generation-stamped CPU handles and shader-visible indices have explicit retirement.
  A descriptor, material, or geometry index is never reused while an in-flight frame
  can observe the old resource.
- Cooked geometry preserves stable primitive records, spatial bounds, material
  references, winding/topology, and reconstructable position/index data. Packing
  buffers together may not semantically merge disconnected surfaces.
- Transparent primitives retain independent bounds and records even when they share
  a material. No M3 optimization may recreate destructive merge-by-material behavior.
- Model products preserve enough canonical geometry for later meshlet generation and
  BLAS construction. M3 does not select mesh-shader or ray-tracing execution policy.
- New scene assignments serialize GUIDs. Legacy paths are migration inputs only and
  do not survive as the authoritative reference after successful resolution.
- Asset Browser/UI code depends on asset/runtime services; core asset records and
  runtime components do not depend on ImGui or native file dialogs. M4 completes the
  broader component/editor separation.
- Existing M0-M2 fixtures, profiling, captures, the reference GBuffer, output
  transports, and sample-car material-84 depth gate remain valid.

## Scope

- Opaque 128-bit asset GUIDs, canonical text encoding, source-controlled metadata
  sidecars, duplicate/orphan detection, and stable subasset identity.
- Rebuildable editor asset catalog with asset type, location, importer, status,
  dependencies, tags, thumbnail, and searchable diagnostic fields.
- Explicit importer registry, importer/cooker versions, canonical settings schemas,
  deterministic source/dependency hashing, and structured import/cook diagnostics.
- Content-addressed local DDC with atomic writes, corruption detection, dependency
  invalidation, concurrent-request coalescing, and deterministic clean rebuild.
- Versioned cooked artifact container and typed model, material, texture, thumbnail,
  and dependency products.
- Separate runtime CPU loading, scheduled RHI upload, upload completion, residency,
  fallback, pinning, eviction, and retirement states.
- GPU-indexed material records, sampled-image views, and samplers with explicit
  capability reporting and compatibility behavior.
- Model and texture settings, validation, deterministic manual/automatic reimport,
  and clear failure states.
- Cooked primitive records, local/world-independent bounds, preserved source
  mappings, optional section points for later meshlets, and RT-compatible
  position/index/topology data.
- Asset Browser thumbnails, search, filtering, import, reimport, error inspection,
  drag/drop, and GUID-based model/material/texture assignment where a current target
  contract exists.
- Removal of source-file import responsibility from Scene Hierarchy and removal of
  new path-based scene asset references.
- Headless cook/validate commands that use exactly the same registry and cooker
  contracts as the editor.

## Non-goals

- General versioned scene schema, entity UUIDs, component serializer registry,
  unknown-component preservation, undo/transactions, or complete removal of ImGui
  from every runtime component (M4).
- Production direct lights, clustered assignment, IBL, shadows, probes, or baking
  systems (M5). M3 may cook environment/texture assets those systems later consume.
- Transparent classification, sorting, peeling/OIT, layered glass, or refraction
  algorithms (M6). M3 supplies primitive identities and bounds only.
- Persistent GPU instances, culling, indirect draw generation, visibility-buffer
  raster/material resolve, or final GPU-scene record design (M7).
- Production meshlet generation limits, mesh-shader pipelines, or mesh-task
  scheduling (M8). The cooked container reserves optional sections and preserves
  source data M8 needs.
- Motion vectors, temporal reconstruction, DLSS, or history management (M9).
- BLAS/TLAS construction, update policy, ray dispatch, denoising, or path tracing
  (M10+). M3 avoids discarding required geometry/material inputs.
- Remote/shared/cloud DDC service, source-control integration, package distribution,
  marketplace workflows, or multi-user asset locking. Interfaces may allow a later
  cache tier without implementing it.
- Silent repair of suspicious authored materials, car-specific material exceptions,
  display-referred shading, or lighting changes intended to hide asset defects.

## Design and data flow

### Identity types and references

`AssetGuid` is an opaque 128-bit value with canonical lower-case text formatting.
It is the identity of every root asset and exported subasset. The value contains no
path, type, importer, runtime index, or source-local index semantics.

```text
AssetRef<T>
  assetGuid          persistent, serializable identity
  expectedType       compile-time or validated runtime type

AssetHandle<T>
  slot + generation  process-local CPU runtime lookup

GpuMaterialIndex / GpuTextureViewIndex / GpuSamplerIndex / GpuGeometryIndex
  shader-visible residency slots
  never serialized
  reused only after timeline/frame retirement
```

An `AssetRef<T>` remains valid across rename, move, reimport, unload, or GPU eviction.
An `AssetHandle<T>` becomes stale when the runtime object version is retired.
Shader indices are resolved from resident runtime objects and deliberately have a
shorter lifetime than GUIDs.

### Metadata sidecar

The proposed source sidecar is `<source-filename>.iridium.meta`, deterministic JSON:

```text
schema_version
asset_guid
asset_type
importer_id
importer_version_selected
settings_schema_version
settings                         canonical typed values
subassets[]                      GUID + source key + structural fingerprint
user_tags[]
```

Source and dependency hashes are derived catalog/cook data, not hand-edited identity.
They may appear in diagnostic snapshots but are not required to be committed in the
sidecar. Importer implementation version is resolved from the registry; the selected
version field detects incompatible or intentionally pinned metadata.

Root and subasset GUIDs are generated once and persisted. Importers provide a stable
source key when the format has one. Reimport matching uses, in order:

1. an exact stable source key;
2. an exact prior importer fingerprint that is unique on both sides;
3. an explicit user resolution for ambiguity.

An ambiguous match never silently transfers a GUID. Removed subassets become
diagnostic orphans until references are migrated or the tombstone is explicitly
accepted. Duplicate root or subasset GUIDs are hard catalog errors.

### Catalog

`AssetCatalog` is editor/tool infrastructure, independent of the renderer. It stores
rebuildable records keyed by GUID:

```text
AssetRecord
  guid, type, source location, sidecar location
  importer ID and resolved version
  settings hash, source hash, cook key, current artifact hash
  status and structured diagnostics
  direct dependencies and reverse dependents
  subasset parent/source key
  thumbnail artifact key
  user tags and normalized search fields
```

The sidecar and source tree can rebuild the catalog from nothing. Runtime builds
consume a compact cooked catalog mapping GUID/type/platform to artifact keys; they
do not open the editor database.

Filesystem discovery normalizes locations relative to configured asset roots for
display and hashing, but never derives identity from that normalized path. Moving a
source and its sidecar updates location only. Moving a source without its sidecar is
reported as a missing-source/orphan condition rather than generating a replacement
GUID automatically.

### Importer and cooker contracts

Importer registration is explicit and test-enumerable:

```text
ImporterDescriptor
  stable importer ID
  implementation version
  supported asset types/extensions
  settings schema + migration functions
  probe(source)
  parse(source bytes, settings) -> SourceDocument + diagnostics + dependencies
  cook(SourceDocument, CookContext) -> CookProducts + diagnostics
```

Registration order cannot change selection. An explicit metadata importer wins;
automatic selection requires one unambiguous probe result. Importers cannot allocate
GPU resources or call editor UI.

Settings serialize to canonical typed bytes: stable field order, explicit integer
widths/enums, finite IEEE values, UTF-8 strings, and normalized asset-root-relative
dependency locations. Unknown settings fields are preserved for editor recovery but
must be migrated or rejected before a strict cook.

The cook key is SHA-256 over a domain-separated sequence containing:

- artifact/container schema and target platform/profile;
- importer ID and implementation version;
- settings schema version and canonical settings bytes;
- source content hash;
- ordered direct dependency GUIDs and artifact/content hashes;
- relevant compiler/cooker feature versions, including M2 material schema;
- explicit quality policy, never ambient machine state.

Dependencies are typed (`SourceFile`, `Asset`, `Tool`, `OptionalAsset`) and stored in
both directions. Dependency cycles produce a stable diagnostic with the complete
cycle. Independent cook requests for the same key coalesce.

### Derived-data cache and cooked artifacts

The local DDC is content addressed and rebuildable. Entries are written to a unique
temporary file, flushed, hash-verified, and atomically renamed. Readers verify magic,
container version, payload sizes, artifact hash, and section checksums. A corrupt or
partial entry is quarantined and rebuilt; it is never handed to runtime loading.

Each artifact uses a small versioned little-endian container:

```text
CookedArtifactHeader
  magic, container version, artifact type/schema
  asset GUID, target platform/profile
  cook key, payload hash
  dependency table location/count
  section table location/count

SectionRecord
  stable section ID, schema version
  offset, compressed size, decoded size, alignment, checksum
```

Sections permit a future schema to add meshlets, BLAS hints, thumbnails, debug
provenance, or alternative texture payloads without changing unrelated products.
Runtime code validates artifact and section versions before constructing a typed
view. Source formats and editor JSON layouts never become runtime blob layouts.

### Runtime load, upload, and residency

Runtime state is explicit:

```text
Unloaded
  -> LoadingCpu
  -> CpuReady
  -> UploadQueued
  -> Uploading
  -> Resident
  -> Evicting
  -> Unloaded

Any stage -> Failed(diagnostic, retry policy)
Reimport -> pending immutable revision -> atomic publish -> retired old revision
```

`AssetRuntime` resolves GUIDs to cooked artifacts, validates dependencies, maps or
loads CPU blobs, and exposes typed process handles. `GpuUploadScheduler` accepts
backend-neutral texture, geometry, and table-update requests under byte/time budgets.
The RHI returns upload tickets whose completion is polled through existing frame
progress; normal frames do not wait for the device.

`ResidencyManager` owns pins, reference counts, priority, last-use fences/timeline
values, CPU/GPU budgets, fallback policy, eviction, and statistics. It does not own
Vulkan objects directly. Evicted texture views point to typed fallback descriptors;
evicted models are non-drawable with an explicit state. Slot retirement waits until
all submitted frames that can observe the old descriptor or buffer have completed.

Reimport constructs an immutable new runtime revision. The GUID-to-current-revision
mapping changes only after dependencies and required uploads succeed. Existing frames
and handles may finish on the old revision, which is retired later. A failed reimport
leaves the last known-good revision resident and exposes the new diagnostics.

### Indexed material, texture-view, and sampler contracts

M3 retains `PackedGpuMaterial` schema-2 closure semantics while changing how texture
uses resolve:

```text
TextureAsset             cooked storage payload(s), dimensions, mips, encoding
TextureViewDesc          asset GUID, subresources, dimension, format/decode,
                         swizzle, semantic compatibility
SamplerDesc              filter, mip filter, addressing, anisotropy/LOD policy
MaterialTextureUse       semantic, TextureViewIndex, SamplerIndex, UV/transform/scalar
GpuMaterialIndex         index into a dynamically growable packed-material table
```

Image storage, image views, and samplers are independently deduplicated. Compatible
sRGB and linear interpretations may share physical storage while using distinct
views. Incompatible semantic cooks, such as BC5 tangent normals versus BC7 color,
remain distinct cooked variants with explicit keys. Shaders always receive the
linear values required by the M2 closure contract; no shader guesses color space from
a material name or image path.

The primary Vulkan implementation uses separate update-after-bind sampled-image and
sampler arrays plus a packed-material storage buffer. Capability reporting includes
non-uniform indexing features, update-after-bind support, maximum resident views and
samplers, and table growth limits. Descriptor slot zero is a typed fallback.

Material and descriptor tables grow by allocating a replacement buffer/table,
copying live entries, switching at a frame-safe boundary, and retiring the old
storage. A fixed 4,096-material ceiling is not retained. CPU generation checks guard
updates; shader indices are not reused before retirement. Residency can replace one
descriptor with a fallback without rewriting every material that references it.

### Texture settings and products

Texture metadata exposes deterministic settings for:

- semantic/profile (`Color`, `Normal`, `Scalar`, `HDR`, `Data`, or explicit);
- source transfer and primaries, runtime decode/view, and channel/swizzle mapping;
- maximum dimensions, mip policy, filter kernel, alpha-coverage preservation, and
  normal renormalization;
- target format/quality policy and optional uncompressed reference product;
- sampler defaults only when no material use supplies an authored sampler;
- thumbnail crop/background/exposure as a separate preview product.

The cooker validates dimensions, channels, finite HDR values, alpha behavior, normal
length, mip coverage, target format support, and per-use compatibility. Material
uses select a view/sampler explicitly. The same image referenced by color and data
uses cannot silently reuse the wrong transfer interpretation.

### Model, material, and geometry products

The glTF importer reuses M2 source-material ingestion and compiler behavior. It
emits model, primitive, material-subasset, texture dependency, and optional scene
hierarchy products. A cooked model refers to material/texture subassets by GUID.

Each cooked primitive record contains at least:

```text
primitive GUID/source key
vertex stream references and index range/format
material asset GUID
topology, winding, culling/coverage classification
local AABB and sphere; optional normal cone
source node/mesh/primitive mapping
attribute availability and decode layout
RT geometry flags and position/index section references
optional LOD and meshlet section references
```

The cooker may pack multiple primitive byte ranges into shared buffers, optimize
index locality, deduplicate compatible vertices, or quantize a GPU stream when
validated. The primitive records, material mapping, transparent bounds, and canonical
reconstructable geometry remain independent. Disconnected surfaces are not merged
into one semantic primitive merely because they share a material.

An RT-compatible section preserves triangle topology, object-space positions,
indices, winding, primitive-to-material mapping, and any decode metadata required to
build a BLAS later. M3 does not create acceleration structures. Meshlet sections are
versioned and optional; M8 selects generation limits and fills them.

### Asset Browser and assignment

The Asset Browser queries the catalog, never scans and parses the filesystem during
an ImGui frame. It provides:

- folder and flat/search views without making folders identity;
- normalized text search and filters for type, importer, status, tags, and errors;
- cached texture/material/model thumbnails with visible pending/failed states;
- import, settings edit, manual reimport, reveal source, duplicate GUID repair, and
  dependency/dependent inspection;
- typed drag payloads containing asset GUID and type only;
- assignment to a selected `MeshComponent` and supported material/texture targets;
- model drop into the viewport/hierarchy through an editor command that creates an
  entity referencing the GUID.

Scene Hierarchy no longer opens a source-file importer. A menu-level import command
may focus the Asset Browser and open its import dialog, consistent with ADR-0004.

M3 introduces a narrow transitional scene field for asset assignment:
`MeshComponent.asset` is an `AssetRef<ModelAsset>` plus transient runtime state.
New saves write `assetGuid`. Legacy `meshPath` loads through a catalog lookup,
produces a migration diagnostic, and writes only the GUID after successful save.
This is not the general M4 scene schema; M4 later supplies top-level versions,
registered component migrations, entity UUIDs, and editor drawer separation.

## Migration strategy

Migration is incremental and keeps the renderer buildable:

1. **Index without changing runtime.** Generate sidecars for tracked fixtures and
   existing project sources, build the catalog, and detect duplicate/orphan state.
   Current `AssetManager` remains the rendering path.
2. **Cook in parallel.** Run source imports through the importer/cooker and compare
   products, diagnostics, material snapshots, vertices, indices, textures, and
   captures against the current path. DDC misses may fall back to a synchronous
   compatibility load only in editor/developer mode.
3. **Introduce runtime blobs and upload scheduler.** Load one tracked fixture from
   cooked products through new typed runtime handles. Keep the old path selectable
   for A/B verification, not as a second permanent asset architecture.
4. **Migrate indexed material/texture access.** Preserve schema-2 material values and
   shader math while replacing per-material combined-image descriptor sets with
   global view/sampler indices. Run matched material and car captures.
5. **Cut models over.** Build model runtime state from cooked primitive records and
   shared packed buffers. Do not call `mergeMaterials`; preserve draw behavior through
   primitive records and compare exact source/production ranges.
6. **Migrate editor references.** Asset Browser imports and assigns GUIDs. Resolve
   legacy scene paths once through the catalog. Remove Scene Hierarchy import and
   typed path swap controls.
7. **Remove source-runtime coupling.** After all gates pass, production runtime no
   longer parses glTF/images, creates GPU objects from source import, or keys assets
   by path. `AssetManager` is deleted or reduced to a temporary façade over
   `AssetRuntime`, then removed before acceptance.

Every cutover records rollback artifact hashes. Source-control reversal and DDC
deletion are valid rollback mechanisms because source plus metadata can rebuild all
products. New metadata GUIDs must not be regenerated during rollback.

## Vertical slices

### M3.0 - M2 opaque-coverage precondition

- Status: Complete as an M2 correction; this does not put M3 in progress.
- Behavior: reproduce selected/unselected car failure, inspect wheel/rim source and
  draw ranges, fix queue-derived depth writes, add architecture and capture gates,
  rerun full verification, and update M2 acceptance evidence.
- Completion: satisfied as recorded in the M2 acceptance report.

### M3.1 - Asset GUID, metadata, and catalog foundation

- Status: Accepted on 2026-07-25.
- Preconditions: approve GUID/sidecar/subasset rules and catalog implementation.
- Affected systems: new asset-core/catalog modules, metadata schemas, fixture
  sidecars, build registration, tests, and a headless catalog inspection command.
- Behavior: add `AssetGuid`, `AssetRef<T>`, deterministic sidecar read/write,
  importer/type IDs, subasset mappings, duplicate/orphan diagnostics, asset-root
  discovery, rebuildable catalog, move/rename recognition, and typed query API.
- Tests: GUID parse/format, malformed metadata, duplicate GUIDs, move with/without
  sidecar, subasset exact/fingerprint/ambiguous matching, deterministic sidecar
  serialization, catalog rebuild equality, and 100,000-record search/filter scale.
- Performance: no catalog work on the render loop; 100,000-record warm text/type
  query p95 <=16 ms and incremental query p95 <=4 ms on the reference CPU; report
  cold rebuild wall time and peak memory.
- Rollback: catalog remains advisory and current runtime path remains active. Sidecars
  are durable source data and are not deleted by rollback.
- Completion: a source move with its sidecar preserves GUID and all catalog queries;
  a clean catalog rebuild produces equivalent records without renderer involvement.
- Implemented contract:
  - `AssetGuid` is an opaque 128-bit RFC 9562 UUIDv7 with canonical lower-case
    serialization. Windows generation uses the system CSPRNG; deterministic UUIDv7
    construction is exposed for fixtures.
  - Metadata schema 1 lives at `<source>.iridium.meta` and owns the root GUID, stable
    type/importer IDs, selected importer version, settings schema and canonical
    settings JSON, persisted subasset GUIDs, exact source keys, structural
    fingerprints, and tags. Unknown settings keys round-trip.
  - Reimport identity matching uses an unused exact source key first, then one unique
    same-type structural fingerprint. Multiple fingerprint candidates are reported
    as ambiguous and never transfer identity silently.
  - `AssetCatalog` is backend-neutral. The M3 editor implementation uses SQLite
    catalog schema 1 with transactions and indexed GUID/type/status/source columns.
    SQLite row IDs are private implementation detail. The full-text index is rebuilt
    as a resident temporary table, so the database remains disposable and search
    avoids file-backed FTS stalls.
  - Discovery is metadata-driven, root-relative, and deterministic. Moving source
    and sidecar together preserves identity; moving only the source leaves an
    explicit missing-source record. Duplicate GUID claims remain queryable and are
    diagnosed rather than overwritten.
  - `IridiumAssetCatalogInspect` performs headless discovery, deterministic rebuild,
    typed search/filter, and JSON reporting. The existing path-based runtime loader
    remains unchanged as the M3.1 rollback path.
- Verification:
  - Debug and Release configure/build pass; both configurations pass 21/21 CTest
    tests. `AssetCatalogTests` covers UUID parsing/formatting, malformed and
    deterministic metadata, unknown-setting preservation, duplicate GUIDs,
    move-with/without-sidecar behavior, exact/unique/ambiguous subasset matching,
    catalog query/filter behavior, and rebuild equality.
  - The tracked M2 material provenance glTF now has a schema-1 sidecar. Headless
    inspection returns its root and two material subassets with zero diagnostics.
  - Release 100,000-record file-backed SQLite scale fixture: 978.086 ms cold
    transactional rebuild, 131.414 MiB peak process working set, 6.5246 ms warm
    text/type query p95 including exact total count, and 3.3513 ms incremental
    first-page query p95 without an unnecessary exact count. Gates are 16 ms and
    4 ms respectively.
  - No renderer, shader, runtime asset-loading, GPU upload, or residency path changed
    in M3.1, so new Vulkan capture/validation evidence is not required for this
    slice. The accepted M2 validation and capture baseline remains the comparator.

### M3.2 - Importer registry, dependency graph, artifact container, and local DDC

- Status: Accepted on 2026-07-25.
- Preconditions: M3.1 identity/catalog accepted.
- Affected systems: importer/cooker interfaces, settings schemas, hashing, dependency
  graph, artifact I/O, DDC, tool entry point, tests, and deterministic fixtures.
- Behavior: explicit importer registration/versioning, unambiguous probing, canonical
  settings bytes, typed dependency graph, cook-key calculation, coalesced cook jobs,
  atomic content-addressed storage, corruption recovery, and versioned artifact
  sections. Prove the framework with a small tracked test importer before glTF.
- Tests: registration order independence, settings migration/rejection, source and
  dependency invalidation, cycle diagnostics, concurrent same-key request
  coalescing, canceled cook cleanup, simulated truncated/corrupt entries, clean-cache
  rebuild, and byte-identical products across independent processes and thread counts.
- Performance: a DDC hit performs no source parse/cook; lookup + header validation
  p95 <=1 ms for a resident catalog and local NVMe entry, excluding payload I/O.
  Main-thread scheduling work p99 <=0.10 ms.
- Rollback: cooker/DDC remains opt-in; deleting the local DDC is safe.
- Completion: two clean cooks of every framework fixture produce identical artifact
  hashes, dependency edits invalidate exactly the affected reverse closure, and
  corrupt entries rebuild without being published.
- Implemented contract:
  - `ImporterRegistry` registration is explicit, enumerated in stable
    ID/version order, rejects duplicate ID/version pairs, honors an exact selected
    metadata importer, and fails automatic selection unless exactly one
    extension-filtered probe accepts the source.
  - Canonical settings encoding schema 1 is a typed little-endian byte stream with
    sorted object keys, explicit signed/unsigned 64-bit integers, finite IEEE-754
    doubles, length-delimited UTF-8 strings, arrays, and objects. Importers own
    version migration and strict unknown-field rejection while non-strict editor
    recovery preserves unknown JSON.
  - `AssetDependencyGraph` stores sorted typed direct edges and derived reverse
    edges, returns deterministic invalidation closures, and reports stable complete
    cycle chains. `SourceFile`, `Asset`, `Tool`, and `OptionalAsset` remain distinct.
  - Cook-key domain `IridiumCookKey/v1` hashes asset/product identity, artifact
    container and target platform/profile/quality, importer ID and implementation
    version, settings schema and canonical bytes, source hash, sorted dependency
    GUID/location/content/artifact hashes, material schema, and explicit cooker
    feature version.
  - Cooked artifact container 1 uses a 240-byte checksummed little-endian header,
    explicit asset/type/target/cook-key fields, typed dependency table, extensible
    section table, per-section schema/alignment/size/SHA-256, payload SHA-256, and
    optional whole-artifact hash verification. Stored and decoded sizes are separate
    fields even though M3.2 framework sections are currently uncompressed.
  - `DerivedDataCache` is a tierable backend-neutral interface. The implemented
    local tier is keyed by canonical cook hash, shards entries by hash prefix,
    validates headers before hits, fully validates payloads before loading, writes a
    unique same-directory temporary file, flushes it, and publishes without
    replacing a valid competing entry. Truncated/corrupt entries move to quarantine
    and rebuild; canceled jobs publish nothing.
  - One worker queue keeps request scheduling off the cook path and coalesces every
    in-process request for the same key into one shared future. A hit invokes no
    parse/cook builder.
  - `TextFixtureImporter` and the tracked `.irtest` source prove schema migration,
    source dependency hashing, parsing, cooking, artifact sections, clean-cache
    determinism, and the headless `IridiumCookAsset` path before glTF production
    integration in M3.4. It has no renderer, RHI, Vulkan, editor, or GPU dependency.
- Verification:
  - Debug and Release configure/build pass; both configurations pass 22/22 CTest
    tests. `AssetCookerTests` covers canonical order and non-finite rejection,
    settings migration/strict rejection, registration-order independence, explicit
    and ambiguous selection, duplicate registration, dependency reverse closure and
    cycles, source/dependency cook-key invalidation, section/header/payload
    corruption, same-key coalescing, cache-hit builder suppression, truncation and
    corruption quarantine/rebuild, cancellation cleanup, clean-cache equality, and
    byte-identical fixture products across eight concurrent threads and two
    independent processes.
  - Two clean tracked fixture cooks produce cook key
    `ea2b25defe0dae9fc4b32e6af1e728522c90fcbc965b774372d4e2ae8268e308`
    and artifact hash
    `2dd32959a2f54dea55b7eb0d6c7fede4446d1b8b7e7d16d5e11150d0f16ee245`.
    The 460-byte artifact records source dependency hash
    `af7edcc1f38f38c4c84ade504d38a36ee67cc2673f3d10b740f2f096fa153260`;
    the next headless request is a validated cache hit with no builder invocation.
  - Release local-NVMe benchmark over 1,000 resident header probes records
    0.0359 ms p95 against the 1.0 ms gate. Ten thousand coalesced request
    submissions record 0.0001 ms p99 against the 0.10 ms main-thread scheduling
    gate.
  - The current runtime/source glTF path remains unchanged and the new cooker is
    opt-in. Deleting the local DDC is safe. No renderer, shader, RHI, upload, or
    residency behavior changed, so no new Vulkan capture is required for M3.2; the
    accepted M2 visual/validation baseline remains authoritative.

### M3.3 - Cooked textures and indexed view/sampler residency

- Status: Accepted on 2026-07-26.
- Preconditions: M3.2 accepted; approve texture container/codec and Vulkan capability
  policy.
- Affected systems: texture importer/settings/cooker, runtime texture products,
  RHI capabilities/upload requests, Vulkan images/views/samplers/descriptors,
  packed-material texture binding, shaders, profiling, fixtures, and thumbnails.
- Behavior: deterministic decode/mips/normal/alpha handling, target texture products,
  separate storage/view/sampler identities, global shader-visible view and sampler
  tables, fallback slot, delayed reuse, dynamic capacity, scheduled uploads,
  eviction/reload, and texture thumbnail product.
- Tests: sRGB versus linear use of the same source, BC/color/normal/scalar reference
  error, mip determinism, alpha coverage, normal renormalization, sampler
  deduplication, stale handle rejection, descriptor retirement, table growth,
  nonresident fallback, capability rejection/fallback, and device-loss-safe cleanup.
- Captures: tracked texture grid, standard deferred/forced-forward twins, all M2
  material debug views, and sample-car normal/base/material IDs.
- Performance: five matched Release runs; material-lab GBuffer+lighting may not
  regress by more than 0.02 ms or 10%, whichever is larger, without owner approval.
  No device wait in a normal frame. Upload scheduling CPU p99 <=0.25 ms under the
  declared per-frame byte budget. Report descriptor, image, staging, CPU resident,
  and DDC bytes separately.
- Rollback: current per-material descriptors remain an A/B path only until M3.7.
  The approved bounded compatibility adapter may remain after cutover, but it consumes
  the same cooked products and is never the production/reference mode. Nonresident
  assets use typed fallback views.
- Completion: material shaders consume global view/sampler indices with identical
  accepted closure values and captures; eviction/reload is validation-clean and
  in-flight-safe.

#### M3.3 acceptance - 2026-07-26

- The approved codec decision is frozen in
  `docs/performance/M3.3-texture-codec-bakeoff-2026-07-25.md`. DirectXTex May 2026
  CPU-only output was bit-identical across repeated runs. Texture product schema 1
  selects BC7 color/data, BC5 normal, BC4 scalar, and BC6H HDR products; BC7 quick is
  the default iteration tier and production quality remains a distinct cook-key
  setting.
- Backend-neutral schema 1 now makes semantic, view color space, compression quality,
  mip policy, alpha mode, codec identity/version, and exact per-mip block layout
  explicit. Canonical settings reject sRGB normal/scalar/data products.
- Deterministic CPU mip generation now averages color in linear light, renormalizes
  tangent normals, reconstructs normal Z when requested, and preserves alpha-test
  coverage. Tests cover identical re-runs and semantic failures.
- Storage, view, and sampler descriptors are separate RHI contracts. BC4/5/6H/7
  storage sizes and Vulkan upload regions use block geometry rather than synthetic
  bytes-per-texel.
- The backend-neutral indexed residency table reserves fallback slot zero, grows
  within a declared bound, returns fallback for nonresident/stale handles, and delays
  slot reuse until a completed serial. Immutable sampler descriptions deduplicate by
  their full filter/address/LOD/anisotropy semantics.
- Vulkan 1.2 descriptor-indexing features and update-after-bind limits are now queried,
  enabled only as a complete required set, and reported through backend capabilities.
  The production shaders now consume two per-frame update-after-bind descriptor sets
  containing separate sampled-image and sampler arrays plus the material storage
  buffer. Unsupported devices fail explicitly; `--material-descriptors compatibility`
  retains the bounded per-material A/B path.
- Identical immutable sampler descriptions now share Vulkan sampler objects. The
  material-lab reference has five resident textures but only three unique live
  samplers. Freeing a texture writes the typed fallback before deferring image
  destruction, and sampler objects remain owned by the cache until device-idle
  cleanup.
- The first indexed/compatibility texture-stress comparison exposed a retained M2
  compatibility-shader binding-order defect. The compatibility shader now maps all
  canonical semantics (including normal versus metallic/roughness, occlusion versus
  emissive, and transmission) to the same schema-2 slots as the indexed path.
- Debug and Release builds pass 23/23 tests. The texture-product suite covers
  canonical settings, same-source sRGB/linear behavior, exact BC mip sizes, mip
  determinism, normal renormalization, alpha coverage, manifest/artifact validation,
  all selected BC7/BC5/BC4/BC6H production compression paths, source-mip
  preservation, sampler deduplication, nonresident fallback, growth, stale handles,
  and serial-delayed slot reuse.
  Indexed and compatibility scene-linear captures are byte-identical for all four
  M2 GPU material fixtures at 3840x2160. All M0/M2 renderer fixtures and SDR/scRGB/
  HDR10 output paths are clean under Release Vulkan validation.
- The production `TextureImporter` separates DirectXTex source decode from semantic
  CPU cooking, rejects untracked settings, pins the codec commit into importer/tool
  identity and the cook key, and emits texture container schema 1. A full source to
  sectioned artifact to local-DDC test is deterministic and the second request is a
  validated cache hit without rebuilding.
- Vulkan material views and samplers now occupy separate variable-count descriptor
  sets. Their layouts declare a capability/packing bound of 65,535 entries while
  per-frame pools begin at 64 entries and grow geometrically only after that frame
  slot's fence completes. The 81-texture sample car grows both physical tables to
  128 entries without rebuilding a pipeline layout or waiting the device.
- `--validate-texture-residency-churn` provides a bounded runtime acceptance path.
  Its declared M3.3 upload budget is 1 MiB per frame; Release scheduling p99 is
  0.2041 ms against the 0.25 ms gate. The 4K sample-car run wrote fallback before
  destruction, rejected immediate reuse of index 81, reclaimed it after the owning
  frame fence, and completed with no Vulkan validation message. Production texture
  handles are not returned to the resource pool until deferred image destruction
  is safe.
- The tracked embedded-image texture grid and optional sample car were captured at
  3840x2160 in Final, Depth, Base Color, Normal, Material ID, Closure Class, and
  wireframe views. Indexed and compatibility sample-car outputs are byte-identical
  in all seven views. Material-84 depth and material-ID hashes remain the accepted
  M2 values. The corrected canonical semantic mapping intentionally supersedes the
  old M2 normal/wireframe images; both descriptor modes agree exactly on the
  corrected output.
- The refreshed five-run Release gate passes: median GBuffer plus lighting is
  0.135168 ms indexed versus 0.134144 ms compatibility, a 0.001024 ms / 0.76%
  indexed cost and well below the allowed 0.02 ms. Exact
  descriptor, sampler, image, staging, material-buffer, and steady-allocation
  counters are recorded in
  `docs/performance/M3.3-indexed-texture-cutover-2026-07-25.md`.

### M3.4 - Cooked glTF model/material/geometry path

- Status: Accepted on 2026-07-27.
- Preconditions: M3.2 and indexed resource contracts accepted.
- Affected systems: glTF importer, M2 material compiler adapters, model/geometry
  cooker, cooked schemas, runtime loaders, geometry upload, extraction bridge,
  material diagnostics, fixtures, and optional car metadata.
- Behavior: cook models, material subassets, dependencies, canonical vertex/index
  streams, primitive records/bounds, RT sections, and optional future-section
  references. Reuse M2 source/compiled/packed contracts. Pack buffers without
  semantic merge-by-material. Load and render from cooked products.
- Tests: source-default/material snapshot equality, subasset identity over reorder,
  primitive and material mapping, transformed/mirrored tangents, indexed/non-indexed
  inputs, malformed accessors, bounds, disconnected same-material surfaces,
  transparent primitive preservation, RT position/index reconstruction, and clean
  artifact determinism.
- Captures: all M2 GPU material fixtures, transparency layers, emissive range, Final/
  Depth/Base Color/Normal/Material ID/Closure ID/wireframe, selected/unselected car,
  and the material-84 depth gate.
- Performance: report cold parse/cook per stage; warm DDC load must avoid source parse
  and improve fresh-process car CPU-ready time by at least 4x versus the audited
  approximately 4.9-second Debug source path. Geometry/image upload bytes may not
  grow without attributed fidelity/runtime benefit. Renderer GPU time remains within
  the M3.3 matched gate.
- Rollback: old source importer remains selectable for A/B until this slice is
  accepted. Failed cooked load retains a clear editor-only compatibility option and
  never silently substitutes different material semantics.
- Completion: tracked models and the optional car render from cooked runtime blobs;
  every source primitive has a stable record and valid bounds, and no runtime glTF
  parsing occurs on the cooked path.

Implementation checkpoint, 2026-07-26:

- Cooked model schema 1 now defines typed manifest, canonical 72-byte vertex,
  uint32 raster-index, RT position, and RT index sections. Every primitive record
  carries its persisted GUID and source key, material GUID, source node/mesh/primitive,
  attribute availability, topology/winding/coverage/culling flags, vertex/index
  ranges, conservative AABB/sphere, RT flags/ranges, and reserved optional LOD and
  meshlet section references.
- Validation rejects nil or duplicate primitive identity, duplicate source keys,
  missing material identity, invalid/overflowing stream ranges, bad triangle counts,
  non-conservative or non-finite bounds, raster or RT indices outside their primitive,
  and RT flags that contradict opaque/masked/transparent coverage. The typed runtime
  reader rejects the wrong artifact type/schema, missing required sections, unsupported
  section schemas, corrupt layouts, and products that fail the same semantic checks.
- `iridium.gltf-model@1` is registered in the headless cook tool. The importer uses
  pinned vendored fastgltf 0.9.0 for source parsing and the accepted M2
  `SourceMaterialDocument`/strict material compiler for material semantics. Importer
  source context now contains both relative and resolved paths so external buffers and
  images can be discovered, loaded, hashed, and keyed without making their paths
  runtime identity. The fastgltf tool identity is an explicit hashed cook dependency.
- Source parsing and CPU cooking are separate calls. Parsing emits a deterministic
  renderer-independent intermediate plus discovered material/primitive source keys and
  structural fingerprints. Cooking resolves persisted subasset GUIDs, canonicalizes
  indexed and non-indexed triangle input, generates missing Mikk-compatible tangents,
  bakes node transforms, corrects mirrored tangent handedness/winding, computes bounds,
  packs without merge-by-material, and emits reconstructable RT streams.
- A transitional cooked runtime bridge now converts the validated canonical streams
  to the current RHI `Vertex`/uint32 upload layout without reparsing glTF and carries
  primitive/material GUIDs, source mapping, coverage, flags, and bounds into each
  runtime `SubMesh`. Material bindings resolve by GUID with deterministic first-use
  indices; duplicate, nil, or missing bindings fail before upload. `AssetManager`
  caches cooked models by root GUID plus cook key, uploads one packed geometry
  allocation, never calls `mergeMaterials`, and leaves material ownership with the
  asset-runtime publisher in preparation for M3.5 revision retirement.
- The tracked M3.4 fixture contains opaque and transparent primitives, an indexed and
  generated-index input, two disconnected node instances sharing the same materials,
  and a mirrored instance. It cooks to four distinct primitive records and preserves
  two opaque plus two double-sided transparent records. Reordering metadata subasset
  entries does not change section bytes; a missing persisted primitive GUID is a hard
  deterministic cook error.
- The optional sample car now has a local ignored sidecar with root GUID
  `019f9cf0-378e-718d-84d6-3eb7b3d602fe`, 87 material identities, and 118
  primitive identities. Production inspection reports 78 external dependencies and
  zero diagnostics; a second metadata import preserves all 205 subasset GUIDs.
  This local sidecar remains outside source control with the commercially sourced
  model, while the tracked fixture proves the same serialized contract.
- The sample car Release cook produced 118 non-merged primitive records in a
  31,149,816-byte artifact, cook key
  `0320d0d276a7e553f67b307769fede61cd49542058dd0baa020514c693f99f36`,
  and artifact hash
  `f4ba55bc6b0a1d2e03daf2eb3b8286d4d52db3e361434df2233fb41f43e04d84`
  with zero diagnostics. Source inspection was 2.614 seconds and the first full
  parse/cook/publish was 7.274 seconds; the deterministic intermediate is currently
  95.5 MB and remains an explicit cold-cook memory/serialization optimization target.
- A versioned DDC cook receipt now records the request identity, source/dependency
  hashes, and cook key. A warm request validates the source plus all 78 known dependency
  hashes and the artifact header before returning the DDC entry, without parsing glTF.
  The measured car warm path fell from the misleading 3.022-second parse-before-hit
  behavior to 0.506 seconds, a 9.7x improvement against the audited approximately
  4.9-second Debug source path and above the required 4x gate. Receipt corruption,
  changed hashes, missing/corrupt artifacts, asset dependencies not yet validated by
  the catalog, or changed importer/settings/target versions fall back to a full parse.
- Two clean Debug headless requests produced `built` then `cache-hit` for cook key
  `7b27dbfb688e05d74730a528f5bfffd487917d8e2bf9ec388b6accbf12bf7a9e`
  and identical artifact hash
  `ae8b3b35af39bf449fbe7b5585c05ea6a26deda00eae676f29f6d08c48de53bf`.
  Debug and Release pass all 25 CTests, including model schema corruption/range/bounds
  tests and end-to-end deterministic glTF artifact, material coverage, mirrored tangent,
  disconnected primitive, RT reconstruction, subasset identity, and malformed accessor
  gates. Both main engine configurations remain buildable.
- M3.4 is not accepted yet. The current production selection still enters through the
  legacy source loader and destructive `mergeMaterials`; the cooked bridge is present
  but not selected by scene/editor loading. The next checkpoint is cooked material/
  texture subasset publication, source/cooked A/B captures, sample-car metadata/cook,
  cold-versus-warm load measurements, and the full M2 visual/validation gate.

Compiled-material publication checkpoint, 2026-07-27:

- The glTF importer is now `iridium.gltf-model@2`, and the cooked model artifact
  schema is version 2. Both bumps are deliberate cache barriers: a schema-1 artifact
  that only carried material hashes cannot be mistaken for a complete runtime
  product.
- A checksummed, deterministic compiled-material product now serializes the complete
  immutable M2 closure: provenance index/name, workflow and closure class, every
  standard closure value, alpha/culling state, all texture operations including
  image/semantic/channel/transfer/UV-transform/sampler origins, and all eight complex
  lobe variants. Its stored closure hash is recalculated through the same canonical
  M2 hash function on read. Truncation, checksum corruption, unsupported enums,
  trailing data, closure-hash mismatch, and lobe type/variant disagreement fail
  deterministically.
- Models now require an `MTL1` material section in addition to the manifest and
  geometry streams. Each material record carries its stable material GUID/source key,
  the full compiled closure product, and an operation-indexed binding from source
  image index to stable texture GUID. Validation rejects missing/duplicate material
  identity, incomplete or mismatched texture bindings, primitive references absent
  from the material table, and primitive coverage/culling flags that disagree with
  the compiled closure. This directly protects the material-84 opaque-depth routing
  contract.
- glTF image subassets are discovered as `iridium.texture` identities under
  `images/{index}`. Their structural fingerprints hash encoded image content rather
  than source paths, including external, data-URI, and buffer-view image sources.
  The metadata reimport tool now permits a same-importer version upgrade, preserves
  existing settings and subasset GUIDs, advances the pinned importer version only
  after a successful parse/match, and still rejects a different or newer importer.
- The CPU runtime bridge reconstructs `CanonicalMaterialAsset` records from the
  cooked closure plus exact material/operation/texture-GUID view bindings. It packs
  the accepted M2 GPU schema and derives deferred versus complex-forward pipeline,
  blend, culling, depth-test, and depth-write state without glTF parsing.
  `AssetManager::loadCompleteModelFromCookedArtifact` is the explicit GPU publication
  boundary: it allocates canonical materials and geometry only after typed artifact,
  closure, view-binding, and GUID resolution succeed, with rollback of partial
  material allocation on failure. Texture view upload/residency remains external so
  model loading does not become a renderer-specific asset database.
- The tracked model fixture now includes an embedded image and proves content-based
  texture identity, complete closure bytes, stable image GUID binding, missing-image
  failure, opaque/deferred depth-write routing, transparent/forward no-depth-write
  routing, mismatched runtime texture GUID rejection, and source-free packed material
  reconstruction. A separate exhaustive product fixture round-trips all complex
  lobes and authored/default texture-transform and sampler origins byte-for-byte.
- The local sample-car sidecar migrated from importer version 1 to 2 while preserving
  all 205 existing material/primitive GUIDs and creating 76 image GUIDs. A second
  import preserved all 281 identities with zero creations and zero diagnostics.
  The schema-2 Release cook contains 87 complete materials, 118 non-merged
  primitives, 283,343 vertices, and 913,623 indices in a 31,211,320-byte artifact.
  Its cook key is
  `2505ee6639a1e35d1d9fed03dc51d9fc3f081635cb0b3f86dc1efc09aeb0c9b9`
  and artifact hash is
  `5d18211505cc75c2072d222495ae5a11ab39cb079dedd07f83d7b280d616dc6f`.
  The following request was a receipt-only cache hit with the same hashes.
- Typed artifact inspection proves source material 84 remains
  `standard-deferred`, `OPAQUE`, and double-sided, with metallic
  `0.979812502861023`, roughness `0.540954053401947`, two referencing primitives,
  and stable GUID bindings for image 72 base color (sRGB) and image 73 normal
  (linear). This verifies that the cooker no longer has a path where this tire
  closure can silently become transparent or lose material identity.
- Debug and Release engine builds succeeded and both configurations passed all 26
  CTests. At this schema-2 checkpoint, texture view products still needed to be
  cooked/resolved for the complete sample car and the renderer gates remained open.

#### M3.4 acceptance - self-contained runtime publication, 2026-07-27

- `iridium.gltf-model@3` and cooked model schema 3 are the accepted cache barriers.
  Every model artifact now requires `MTL1` compiled-material and `MTX1` typed
  texture-view sections in addition to the canonical geometry and RT streams.
  Version 2 remains a historical complete-closure checkpoint but cannot be loaded
  as a self-contained runtime product.
- Source parsing decodes each discovered image once into a deterministic texture
  intermediate. Cooking maps color operations to BC7 sRGB, normal operations to
  BC5 linear, and packed/data operations to BC7 linear, applies closure-derived
  alpha semantics, and deduplicates canonical views. DirectXTex is an explicit
  hashed tool dependency. Its WIC apartment now has thread lifetime so repeated
  imports in one process cannot retain decoder state beyond COM teardown.
- Each `MTX1` record carries stable texture GUID and source-image identity, canonical
  view key, typed texture manifest, complete mip layout, and GPU-ready payload.
  Each compiled texture operation resolves through material GUID, operation index,
  texture GUID, and view index. Validation rejects unused, duplicate, missing,
  mismatched, corrupt, or semantically incompatible view records before allocation.
- `AssetManager::loadSelfContainedModelFromCookedArtifactFile` is a production RHI
  publication path selected explicitly with `--cooked-model-artifact`. It validates
  the container and typed model once, uploads GPU-ready texture blocks, reconstructs
  the exact M2 packed materials and pipeline states, uploads non-merged geometry,
  and records load mode, asset GUID, cook key, and input location in capture/profile
  evidence. It does not read glTF, source images, metadata, or a renderer-specific
  asset database. Source loading remains the explicit A/B rollback until M3.7.
- BC5 A/B inspection found that the original shader sampled the nonexistent blue
  channel rather than reconstructing positive tangent-space Z. A backend-neutral
  per-texture-use bit now marks two-channel normal products in the existing packed
  material reserve, and both deferred and complex-forward shaders reconstruct Z.
  Source RGB normal views leave the bit clear. This corrected a cooked-publication
  defect rather than accepting an accidentally brighter but physically invalid car.
- The local sidecar migrated from importer 2 to 3 with all 281 GUIDs preserved and
  zero creations or diagnostics. The accepted Release artifact contains 87 complete
  materials, 76 typed texture views, 118 preserved primitives, 283,343 vertices,
  and 913,623 indices. It is 51,992,911 bytes, with cook key
  `cd169f757da5f3d591b825bf3ae279cf309b4fe4236757b2d55683024096f825`
  and artifact hash
  `fd4c52148be3b9fec37bcd90e3c03dce1f7e19e89ad0701952c16a61889d4bb2`.
  The cold parse/decode/cook was approximately 39.4 seconds. The next validated
  receipt/DDC request was a source-free cache hit in 727.968 ms.
- Typed inspection of source material 84 records stable material GUID
  `019f9cf0-378f-7e9d-8482-2909403b4928`, `standard-deferred`, `OPAQUE`,
  double-sided, metallic `0.979812502861023`, roughness `0.540954053401947`,
  two referencing primitives, sRGB base-color image 72, and linear normal image 73.
  Runtime reconstruction selects the GBuffer pipeline, opaque blending, no culling,
  depth test, and depth write for those two primitive ranges.
- At 1280x720, source and cooked Depth, Material ID, and Closure Class captures are
  byte-identical, with respective SHA-256 values
  `f5032441d7da5133d869b505c243c68f654a73ef46aba49da23639371ba6e522`,
  `9afa526ba6ce2e65b712644412e0ac4f02e4eff5bcf1a1c63db7c8a05fee09e8`,
  and `ca383629a8d57eb7beb70912fd8d0420d02f5b37cc04980177ac4779850c275e`.
  Base Color differs at only 302 of 921,600 pixels with 0.0177/255 RMSE. Normal
  differs by 0.354/255 RMSE after valid BC5 reconstruction. These exact identity
  and depth results prove that preserving 118 primitive records instead of the
  legacy material merge did not change visible coverage or material routing.
- The 3840x2160 Release final-output A/B uses source hash
  `9836233f431a4f0cb4bb036d81beeedb5a8e16ca6bf2d7ceecf01892d3c1d0b6`
  and cooked hash
  `fb0a37e71ab557a12e7a42609e6544b9f8d9c0de1b9d304ff3f689ea9b1f14d4`.
  Expected block-compression differences affect 0.1333% of pixels, with
  0.00332/255 mean absolute error and 0.1262/255 RMSE. The same cooked hash is
  produced by Debug validation and Release.
- Five independent cooked Release runs at 3840x2160, 500 warm-up frames, and 10,000
  measured frames record per-run median GPU-frame times of 0.903552, 0.918464,
  0.946368, 0.949280, and 0.951520 ms. The five-run median is 0.946368 ms versus
  the accepted 0.955 ms source-car baseline. Median GBuffer, deferred-lighting,
  and forward-opaque times are 0.187392, 0.126976, and 0.190464 ms. Median CPU
  frame time is 1.5013 ms, below the 4 ms budget; its 16.0312 ms p95 remains
  dominated by hidden-window acquire wait.
- The optimized Release cooked publication is 1.014 seconds versus 1.348 seconds
  for fresh-process source publication. It submits 74.699 MiB instead of 133.964
  MiB and records 1005.504 MiB committed live instead of 1064.731 MiB, a
  59.227 MiB reduction. Debug deliberately performs expensive full cryptographic
  validation and is not the shipping load-performance comparator.
- Debug and Release pass all 26 CTests. A 4K Debug cooked-car capture is Vulkan
  validation-clean, as are fresh 4K validation runs of the four M2 GPU material
  fixtures, transparency, and emissive range. No ADR changes are required.
  M3.4 is accepted; M3.5 becomes the sole active implementation slice.

### M3.5 - Dependency-driven reimport, hot publish, and residency budgets

- Status: Accepted on 2026-07-27.
- Preconditions: cooked texture and model paths accepted.
- Affected systems: file change service, dependency graph, job scheduling, asset
  runtime revisions, upload scheduler, residency manager, profiler counters, and
  browser status model.
- Behavior: debounce and hash source changes, invalidate reverse dependencies,
  schedule deterministic reimport, preserve last known-good revision on failure,
  atomically publish successful CPU/GPU revisions, retire old resources after frame
  completion, enforce CPU/GPU/upload budgets, and expose pins/evictions/errors.
- Tests: rapid edits, same-content timestamp changes, dependency fan-out, cycle/error,
  failure retention, cancellation, edit during cook, reimport during in-flight draw,
  descriptor/material/geometry retirement, eviction priority, reload, and shutdown.
- Performance: no normal-frame device wait; asset-runtime tick p99 <=0.25 ms when
  idle and <=0.50 ms while scheduling within budget. Record upload bytes, queue
  latency, staging peak, residency changes, and eviction counts. A background reimport
  may not push the representative 4K GPU frame above 10 ms p95.
- Rollback: disable automatic watching while retaining manual reimport; keep last
  known-good products and fallbacks.
- Completion: editing one tracked texture updates its material/model dependents
  deterministically without a renderer restart or stale-index validation error;
  failed edits leave the old image active and show the new diagnostic.

Implementation checkpoint, 2026-07-27:

- `AssetRuntimePublisher` is a renderer-independent revision/publication
  coordinator keyed only by stable asset GUID. It coalesces rapid unpublished
  revisions to the newest cook key, rejects unchanged requests, executes only
  already-prepared publication callbacks, and enforces an exact per-tick upload-byte
  budget. Source parsing and cooking are deliberately outside its tick.
- A successful publish advances a monotonic runtime revision and atomically replaces
  resident byte/retirement ownership. A failed or throwing publish retains the last
  known-good revision and exposes `ReadyWithError`; a first failure exposes `Failed`.
  Superseded queued work never executes. Shutdown and rejected partial outcomes run
  non-throwing cleanup.
- Runtime snapshots expose state, current and pending cook keys, revision, diagnostic,
  CPU/GPU resident bytes, pin state, and last-use serial without any ImGui type.
  Residency eviction is deterministic LRU with GUID tie-breaking, respects pins,
  reports an unsatisfied budget when only pinned assets remain, and preserves
  monotonic revision history across eviction/reload.
- `AssetManager::replaceSelfContainedModelFromCookedArtifact` builds and validates a
  complete replacement before mutation, preserves the existing `shared_ptr<ModelAsset>`
  identity held by scene components, swaps the new resources in one main-thread
  publication step, and frees the old textures, materials, and geometry through the
  Vulkan backend's existing fence-deferred retirement paths. A failed replacement
  restores the prior cache entry unchanged.
- The tracked publisher tests cover last-known-good retention, failure diagnostics,
  rapid-revision coalescing, unchanged suppression, strict upload deferral, successful
  replacement retirement, monotonic revisions, pin-aware LRU eviction, reload,
  invalid requests, thrown callbacks, and shutdown.
- `SourceFileWatcher` observes only explicitly registered source and dependency
  paths on a background thread. One path may invalidate multiple owning GUIDs;
  size/write-time transitions, deletion, and recreation emit compact events without
  filesystem queries on the application frame. Registration captures a baseline and
  never generates a synthetic first change.
- `SourceChangeTracker` debounces those events, hashes content only after the quiet
  window, suppresses same-content timestamp changes, computes the reverse-dependency
  closure, rejects cyclic closures, and produces a deterministic dependency-first
  rebuild order. Hash failures are diagnostic and retryable.
- `AssetReimportScheduler` executes preparation callbacks on a dedicated worker in
  dependency-first enqueue order. Newer edits request cancellation and supersede
  stale completions even when a callback is not cooperative. Only prepared
  CPU/runtime products cross to the frame thread; their GPU publication callbacks
  execute when drained into `AssetRuntimePublisher`. Preparation failure updates
  runtime error state without replacing or retiring a last-known-good revision.
- A Release benchmark with 100,000 runtime records and 10,000 samples records
  0.0001 ms idle tick p99 and 0.0006 ms enqueue-plus-publish p99, against 0.25 ms
  and 0.50 ms gates. New deterministic tests
  cover multi-owner file events, edit/delete/recreate transitions, debounce and
  same-content suppression, dependency fan-out/order, cycle blocking, preparation
  failure retention, edit-during-cook cancellation/supersession, main-thread-only
  publication, and shutdown cancellation.
- `AssetRuntimeService` composes the watcher, dependency invalidation, asynchronous
  preparation, frame-thread publication, upload budgeting, and residency management.
  `Application` ticks it under `cpu.asset_runtime.tick` and exports change, rebuild,
  cancellation, publication, failure, upload-byte, queue, residency, eviction, and
  retirement counters. Source parsing, hashing, dependency walking, cooking, and
  cooked-model decoding remain off the application frame.
- Editor startup with `--cooked-model-artifact` adopts the current cooked revision,
  discovers its sidecar and source by stable GUID, watches the primary source,
  metadata, and external dependencies, and performs a real `prepareAssetCook` into a
  local DDC before publishing. Cooked-artifact changes use the same revision path.
  Benchmark startup deliberately disables workers and watching.
- The end-to-end runtime-service fixture edits a real external PNG referenced by a
  glTF asset. The watcher detects and hashes the changed dependency, the glTF importer
  reparses it, the cooker emits a different valid cook key, and the runtime publishes
  revision 2. The failure fixture preserves revision 1 and exposes
  `ReadyWithError`; rapid and non-cooperative stale preparations cannot publish.
- A live Release Vulkan-validation run replaced the loaded sample-car artifact with a
  second valid artifact carrying the same GUID while draws were in flight. It
  recorded one source event, rebuild, publication, and retirement; uploaded
  44,845,012 bytes; retained one resident asset; and reported no publication failure
  or validation message. A separate invalid-revision run recorded one publication
  failure, zero upload/retirement, and retained the original resident CPU/GPU bytes.
- The representative 3840x2160 validation/profile run completed 2,500 measured
  frames with no missing GPU samples. `gpu.frame` was 1.709888 ms median,
  2.612992 ms p95, 2.644256 ms p99, and 2.820384 ms maximum, well below the
  10 ms p95 gate. The composed asset-runtime tick was 0.4142 ms p99, below its
  0.50 ms active-work gate. The accepted artifact was restored byte-for-byte after
  the replacement run.
- Debug and Release pass all 32 CTests. The live valid and invalid replacement runs
  are Vulkan-validation clean. M3.5 is accepted without an ADR change; M3.6 becomes
  the sole active implementation slice.

### M3.6 - Asset Browser and GUID assignment migration

- Status: Accepted on 2026-07-31.
- Preconditions: catalog, reimport, runtime, and thumbnail APIs accepted.
- Affected systems: new Asset Browser panel, editor commands/drop targets, Mesh
  assignment, narrow scene compatibility serialization, Scene Hierarchy,
  `MeshComponent`, and tests.
- Behavior: thumbnail grid/list, search, filtering, status/errors, settings/reimport,
  dependencies, typed drag/drop, model entity creation, model/material/texture
  assignment where supported, legacy `meshPath` resolution, GUID-only new saves,
  and removal of Scene Hierarchy/source-path import responsibility.
- Tests: query/filter/status UI model without ImGui dependency, drag payload type
  rejection, assignment to existing/new mesh entity, rename/move preservation,
  legacy path migration, missing GUID, failed thumbnail, reimport diagnostics,
  deterministic save, and editor/runtime dependency boundaries.
- Interaction gate: no source scan/import/cook on the ImGui frame; cached 100,000-item
  queries remain within M3.1 gates; thumbnail upload is budgeted and cancellable.
- Rollback: Browser can show read-only catalog state while current GUID assignments
  continue to load. Legacy path resolution remains load-only until M4 migrations
  supersede it.
- Completion: a user imports, finds, filters, drags, assigns, renames, reimports, and
  reloads a model without Scene Hierarchy importing a source file or the saved scene
  depending on its path.

Implementation checkpoint, 2026-07-27:

- `AssetBrowserModel` is an ImGui-independent cached-catalog view model with bounded
  paging, full-text search, exact asset-type/status filters, grid/list state,
  selection, catalog/runtime diagnostics, and thumbnail-state decoration. Its
  versioned fixed-size drag payload carries only a stable GUID plus semantic asset
  kind; wrong payload type, schema, size, nil/non-v7 GUID, and assignment kind are
  rejected before any component mutation.
- The editor owns an Asset Browser window backed by the rebuildable SQLite catalog.
  It shows cached model/material/texture records in grid or list form, exposes
  search/filter/status/runtime diagnostics, supports typed drag sources, queues a
  tracked asset's manual reimport through the M3.5 background scheduler, and creates
  a mesh entity from a double-clicked model. Placeholder cards are deliberately not
  counted as accepted thumbnails.
- The Inspector accepts only model-kind GUID payloads. `MeshComponent` now stores the
  assigned and pending GUIDs without any ImGui/file-dialog dependency; its temporary
  no-op inspector hook exists only for the pre-M4 component-pool interface. Source
  path entry and browsing were removed from the component and Scene Hierarchy.
- New scene saves emit `MeshComponent.assetGuid` and never emit `meshPath`. The narrow
  serializer compatibility path reads old `meshPath` fields but does not write them
  back. Valid unresolved GUIDs survive loading for later catalog resolution;
  malformed GUIDs reject the load without leaving a partial scene.
- `Application` builds the project catalog once at editor startup, resolves pending
  model GUIDs outside the ImGui frame, preserves the already loaded cooked model when
  its GUID matches, and reports missing/ambiguous/non-model records without replacing
  the prior assignment. A nonresident GUID now enters `AssetModelPreparationService`:
  metadata validation, source parse, deterministic cook/DDC lookup, container decode,
  and typed model-product validation complete on its worker. Only the prepared CPU
  artifact/product crosses to the existing byte-budgeted runtime publisher, and only
  that publisher invokes `AssetManager` GPU allocation on the frame thread. The GUID
  path no longer calls the source-model loader. The old `meshPath` load branch remains
  the explicit load-only compatibility adapter to remove in M3.7.
- `AssetCatalogService` now owns background import and catalog refresh jobs. The
  Asset Browser's Import and Refresh actions enqueue work and only drain compact
  results on the ImGui frame. Import uses the same shared importer registry and
  metadata/subasset matching contract as `IridiumImportAsset`, rejects sources
  outside registered roots before writing, preserves root and subasset GUIDs on
  reimport, and atomically rebuilds the SQLite catalog after success. Moving a source
  together with its sidecar and refreshing preserves identity while updating the
  catalog path.
- `AssetThumbnailService` produces real deterministic 96x96 previews from validated
  cooked products. Model and primitive cards CPU-rasterize their preserved cooked
  triangles; material cards shade a sphere from the compiled closure; embedded and
  standalone texture cards decode the exact cooked texture view, including normal
  reconstruction and bounded HDR display mapping. The service coalesces work by root
  GUID, cancels queued roots that leave the visible page, discards active results
  that are no longer demanded, and shares one `LocalDerivedDataCache` instance with
  nonresident model preparation. A cold thumbnail request and assignment for the
  same cook key therefore share one in-flight build instead of merely racing on the
  same disk path. None of this touches the ImGui frame. Fixed hashes pin the model,
  material, and texture fixture previews.
- Prepared RGBA8 previews enter a strict 512 KiB-per-frame upload queue. The editor
  GPU cache is capped at 512 entries with deterministic LRU eviction; evicted visible
  records regenerate, while no-longer-visible prepared uploads cancel before GPU
  allocation. The Browser shows real image cards plus explicit pending/failed states.
  It presents importer identity, tags/source keys, canonical import settings, cooked
  dependencies, and runtime/thumbnail diagnostics from compact cached worker results.
- Sequential background texture-import tests exposed a DirectXTex WIC-factory
  lifetime defect: the process-global factory could outlive the short worker COM
  apartment that created it and crash a later worker. A dedicated process-lifetime
  MTA now owns that factory while each importer worker retains its own balanced COM
  scope.
- Debug and Release build cleanly and pass all 38 CTests. Focused tests cover cached
  query/filter/paging/selection/decoration behavior, strict typed payload rejection,
  GUID-only deterministic scene round trips, legacy path load compatibility,
  malformed GUID rejection, manual reimport through the tracked preparer, background
  import/reimport, outside-root rejection, GUID-preserving move/refresh, nonresident
  model prepare/coalescing/identity failure, external prepared publication, real
  model/material/embedded/standalone texture previews, visibility cancellation,
  eviction regeneration, source-detail caching, and exact upload deferral.
- A warm-DDC 1280x720 Release validation/profile run produced and uploaded 100 visible
  previews with zero failures. It uploaded 3,686,400 bytes in eight scheduling events
  (seven 516,096-byte batches and one 73,728-byte batch), never exceeding the
  524,288-byte frame budget. `cpu.scene.asset_swaps` was 0.0034 ms p99; its one-time
  4.3905 ms maximum includes preview allocation. `gpu.frame` was 0.342272 ms p95 and
  1.137952 ms p99. The Khronos validation layer reported no messages. The cold run
  correctly cancelled its incomplete preview batch at shutdown after first populating
  the 51,992,383-byte editor DDC artifact.
- M3.6 is not accepted yet. The remaining gate is the owner-observed interactive
  import -> find/filter -> drag/create -> GUID-only save -> source+sidecar rename ->
  refresh/reimport -> reload workflow against the 100,000-record scale catalog.
  Headless query, cook, cancellation, upload, validation, and both-preset gates pass,
  but they do not substitute for that UI/interaction observation.

Corrective checkpoint, 2026-07-28:

- The first owner interaction gate failed and is not counted as approval. Scene
  save/load did not present a usable window, the Browser layout overflowed and exposed
  details below the result grid, folder organization and meaningful editable import
  settings were absent, rename/refresh could strand an assigned model, and resizing
  editor panels stretched the rendered viewport. M3.6 remains In Progress until the
  owner repeats and accepts the corrected workflow.
- Scene Save As and Load now open an in-editor modal that is independent of the native
  common-dialog result. It provides a project-relative path field, lists scenes under
  `assets/scenes`, supports an optional native Browse action, keeps diagnostics
  visible, and saves directly to an established current path. GUID-only serialization,
  legacy path load-only compatibility, malformed-GUID rejection, and deterministic
  round trips remain unchanged.
- The Browser is now a bounded three-pane layout: a cached source-folder hierarchy on
  the left, the searchable/filterable paged grid or list in the center, and nested
  asset details on the right. Fixed-size grid cards remove variable wrapped-text row
  gaps, the result child supports horizontal scrolling, and the toolbar uses bounded
  table columns. Folder construction and filtering operate on catalog snapshots; no
  source scan or filesystem traversal was moved onto the ImGui frame.
- Root importer settings can now be edited and reimported through the background
  catalog service. glTF exposes the optional tangent-generation setting while showing
  required transform/RT-geometry contracts; textures expose semantic, compression
  quality, mip/alpha policy, view color space, normal reconstruction, and coverage
  controls; the text fixture exposes transform/repeat settings. Settings are
  canonicalized and validated before one atomic sidecar replacement, and root and
  subasset GUIDs remain stable. Subasset details explicitly identify inherited root
  settings instead of presenting nonfunctional raw JSON.
- Catalog rebuild/query access is atomic. A reader can observe the complete old or
  complete new snapshot, never the transient delete/insert state. Pending scene GUIDs
  now survive a temporarily busy or missing catalog, retain a visible resolution
  diagnostic, and retry when a renamed source appears at a new current catalog path.
  Manual reimport also resolves the current root record instead of relying on a stale
  runtime source path.
- The viewport image, picking bounds, and gizmo rectangle now use a centered
  aspect-fit rectangle derived from the render extent. Resizing adjacent editor panels
  letterboxes unused space instead of resampling the scene to the panel's arbitrary
  shape.
- The 100,000-record Release catalog gate records a 1,081.47 ms cold rebuild,
  131.367 MiB peak working set, 0.4112 ms warm-query p95, and 0.1597 ms incremental
  query p95 against the accepted 16/4 ms query limits. The full-text query forces its
  bounded match set to drive primary-key catalog lookups so SQLite cannot choose the
  prior full-catalog join order.
- Debug and Release build cleanly and pass all 38 CTests after the correction.
  Added coverage exercises atomic concurrent rebuild/read snapshots, folder-tree and
  directory filtering, atomic GUID-preserving settings updates and invalid-setting
  rejection, and wide/tall/invalid viewport fitting. A bounded 1280x720 Debug run
  completed 120 measured frames with `VK_LAYER_KHRONOS_validation` active and no
  validation message. The evidence is recorded in
  `out/m3.6/corrective/debug-validation.jsonl`; generated output remains untracked.

Workflow corrective checkpoint, 2026-07-28:

- The repository's `assets/` directory is the current physical project-content root.
  This is the deliberately narrow pre-project-schema policy: Browser folders are real
  directories beneath that root, empty folders carry a small `.iridium-folder`
  source-control marker, and SQLite remains a rebuildable index rather than folder
  authority. A future project descriptor may register multiple named roots without
  changing persisted asset GUIDs or scene references.
- The Browser can create, rename, and explicitly delete folders; rename and explicitly
  delete root source assets; and drag root assets between folders. Source files and
  `<source>.iridium.meta` sidecars move together. GUIDs remain unchanged. A moved
  `.gltf` rewrites relative buffer/image URIs so dependencies continue resolving.
  A folder rename rejects a contained glTF that points outside the renamed folder
  instead of silently breaking it. Imported material/texture subassets remain nested
  under their source model and cannot be physically moved independently. Recursive
  folder and asset deletion is confirmed in a modal and is deliberately not presented
  as undoable before M4 editor transaction ownership.
- Top-level catalog queries hide imported subassets. A small model-card arrow opens a
  drawer whose model-to-material and material-to-texture edges come from the validated
  cooked product. Materials and textures remain typed drag sources for assignment.
  Models, standalone materials, and standalone textures retain exact type filters;
  imported children are revealed only through their owning drawers. The fixed-card
  grid, paged list, bounded center child, horizontal scrollbar, physical folder tree,
  and right-side detail/setting panel remain the Browser layout.
- glTF import settings now include a finite logarithmic uniform import scale in
  `[0.000001, 1000000]`. The canonical cook key includes it, and cooking applies it
  to baked positions, node translations, raster/RT geometry, and bounds while
  renormalizing normals. The default is exactly `1.0`, so accepted importer version 3
  sidecars and old outputs remain compatible; changing scale deterministically
  reimports the asset without changing any GUID.
- Model cards no longer create scene entities on double click. Models are dragged to
  the Scene Hierarchy, Mesh Inspector, or aspect-fitted Scene Viewport, and the main
  `Create` menu offers Empty Entity and the currently selected model. Viewport drag
  hover draws a screen-space thumbnail/crosshair preview and unprojects the cursor to
  the world ground plane, with a forward-ray fallback. A full double-click model
  viewer and live rendered ghost preview remain later editor work rather than hidden
  M3 scene-schema or renderer ownership.
- Scene entities now have editor names with unique-name creation, inline Hierarchy
  rename, F2/context rename, and context deletion. The Mesh Inspector accepts model
  drops or a cached-catalog browser, exposes source material slots, accepts material
  drops or a material browser per slot, shows pending/ready override state, and can
  reset each override. Cross-model material overrides prepare the owning cooked model
  on the existing background/runtime publication path and render through its compiled
  binding without introducing path identity or authoring shader parameters.
- Scene saves persist entity names, transforms, the latest assigned or still-pending
  model GUID, and deterministic source-material-to-replacement-material GUID
  overrides. Loads restore those values and queue model/material resolution outside
  the ImGui frame. Saving immediately after a model drop therefore records the new
  GUID even if its asynchronous cook/upload has not completed. `meshPath` remains
  load-only compatibility and is never emitted.
- The physical catalog jobs share one worker, rebuild the complete directory/catalog
  snapshot after a successful mutation, and keep all filesystem traversal off the
  ImGui frame. A rejected settings job exposed that MSVC exception unwinding had not
  been enabled despite exception-based error handling throughout the engine.
  C++ targets now compile with `/EHsc`; the rejection path shuts down deterministically
  instead of stranding the worker during object cleanup.
- Debug and Release build cleanly and pass all 39 CTests. New/extended tests cover
  physical folder discovery and markers, GUID/sidecar preservation, relative glTF
  dependency rewriting, escape/subasset/root-operation rejection, the complete
  asynchronous create/rename/move/delete workflow, hidden top-level subassets,
  cooked model/material/texture associations, import-scale geometry/normal behavior,
  unique entity creation and viewport placement, entity-name/material-override scene
  round trips, and pending-model save precedence.
- Release Vulkan validation completed without messages for 30 measured frames at
  1280x720 after 8 warm-up frames (`6.991596 ms` average wall time) and for 12
  measured sample-car frames at 3840x2160 after 8 warm-up frames (`8.428141 ms`
  average wall time). No renderer format, output, material-closure, GBuffer, ADR, or
  roadmap status changed.
- M3.6 remains In Progress. The owner must still repeat and accept the interactive
  physical folder/rename/move, nested drawer, import-scale, viewport/hierarchy drop,
  model/material swap, entity rename, save, reload, panel resize, refresh, and
  reimport workflow. M3.7 does not start from this implementation checkpoint alone.

Performance and interaction corrective checkpoint, 2026-07-28:

- The reported approximately 140 FPS Release and 10 FPS Debug behavior reproduced as
  an editor CPU defect rather than renderer/GPU load. The Browser expanded the visible
  sample-car root into all 1,768 catalog/subasset records and replaced thumbnail demand
  every ImGui frame. The pre-correction 1280x720 Release run measured
  `cpu.frame.total` at 9.0661/10.1600/10.9295 ms median/p95/p99,
  `cpu.editor.build` at 8.5159/9.5470/10.3073 ms, and `gpu.frame` at only
  0.333280/0.336416/0.339296 ms. The GPU and present path were not the limiter.
- Browser thumbnail demand is now rebuilt only when the visible root set or inspected
  root changes. A separate pinned demand source keeps the selected Mesh Inspector
  model's material thumbnails alive without fighting Browser paging or repeating
  catalog work. The final like-for-like 600-frame Release run records 0.7534/0.8052/
  0.8425 ms CPU-frame median/p95/p99, 0.3007/0.3300/0.3774 ms editor-build, and
  0.319840/0.330912/0.332064 ms GPU-frame. Its measured wall average is 0.769837 ms,
  approximately 1,299 FPS, versus 9.022869 ms before the correction.
- A 300-frame Debug run with Vulkan validation enabled records 2.9594/3.7596/
  6.0037 ms CPU-frame median/p95/p99 (approximately 338 FPS at the median) and
  0.9678/1.3030/2.3477 ms editor-build, replacing the owner-observed approximately
  10 FPS behavior. Debug command recording, assertions, and validation remain
  intentionally more expensive than Release.
- Asset Details now renders the selected model, material, or texture thumbnail.
  Thumbnail demand includes the inspected record even when its tile is paged away.
  The grid drawer arrow uses the captured thumbnail rectangle rather than the mutable
  last-ImGui-item rectangle, so drag-source construction can no longer move its hit
  target outside the card. Five discrete Browser thumbnail sizes provide explicit
  zoom controls.
- Physical immediate-child folders are rendered as first-class items in the main
  grid/list in addition to the source hierarchy. A selected folder queries only its
  direct assets while deeper content remains behind its visible child folders. They
  support direct traversal, parent navigation, asset drop targets, and
  context-sensitive Open, Import,
  New Subfolder, Rename, Delete, and Refresh actions. Asset context menus expose
  Reimport, Rename, and Delete only for eligible root assets; the empty Asset View
  exposes Import, New Folder, and Refresh. Imported child materials/textures remain
  protected under their owning model.
- The Mesh Inspector presents at most four source-material slots per page in a
  bounded scrolling grid. Every card shows the cooked thumbnail (with an explicit
  pending fallback), display name, slot, stable material GUID, override state,
  drop target, browser action, and reset where applicable. Scene serialization and
  runtime material-override behavior are unchanged.
- The Browser toolbar now reflows search and equal-width controls, its Sources and
  Details panes collapse to popups below their usable width, and pane widths are
  capped as fractions of the window. Editor style metrics and font scale reduce
  progressively below a 1600x900 work area. The existing aspect-fit Scene Viewport
  remains authoritative, so responsive panes never stretch the rendered image.
- Debug and Release build cleanly and pass all 39 CTests, including new pinned-demand
  coverage. A 3840x2160 Release Vulkan-validation run completed 180 measured frames
  without validation messages; `gpu.frame` was 2.166048/2.386400/2.394656 ms
  median/p95/p99. No material, GBuffer, output, RHI, accepted ADR, roadmap, or M3
  status changed.
- M3.6 remains In Progress. These automated corrections and measurements do not
  substitute for the owner repeating and accepting the requested Browser,
  Inspector, folder/content-operation, scene-save/load, and resized-window workflow.
  M3.7 still does not start without that acceptance.

Owner acceptance checkpoint, 2026-07-31:

- The owner repeated the corrected interaction workflow after the final Sponza
  runtime-publication fix and confirmed that import, placement, rendering, Browser
  operations, scene persistence, and reload now work as intended. This satisfies the
  remaining owner-observed M3.6 interaction gate; M3.6 is accepted and M3.7 becomes
  the sole active implementation slice.
- The owner separately reported that current material appearance remains visibly
  incorrect. That observation is not silently counted as correct material output by
  this interaction acceptance. M3.7 must distinguish absent production lighting/IBL,
  which remains M5 scope, from any source-to-compiled-closure or cooked-binding
  mismatch, which is an M2/M3 regression and must be corrected before final M3
  acceptance. No provenance-destroying metallic/specular heuristic is authorized.

### M3.7 - Production cutover, scale gate, and acceptance

- Status: Accepted on 2026-07-31.
- Preconditions: M3.1-M3.6 accepted individually.
- Affected systems: removal of source-runtime branches and production selection of
  the old descriptor path, isolation of the approved compatibility adapter,
  production configuration, all fixtures/manifests, profiling docs, roadmap/context,
  ADR clarification if approved decisions require it, and acceptance report.
- Behavior: production loads cooked catalog/artifacts, uses indexed materials/views/
  samplers, schedules uploads/residency, and uses Browser/GUID assignment. Remove
  path-keyed model cache, runtime source parsing, merge-by-material cooking, fixed
  4,096 material table, and legacy source-coupled per-material descriptor allocation.
- Scale fixtures: at least 100,000 catalog records; 65,536 resident material records
  exercised through dynamic table growth; capability-bounded large texture-view and
  sampler tables; dependency fan-out; rapid reimport; and residency pressure.
- Verification: both presets and all CTests, headless clean cook twice, empty-DDC and
  warm-DDC runs, all M0-M2 fixtures, sample car, selected/unselected/debug/wireframe
  captures, Vulkan validation, resize/output transports, eviction/reload, normal
  cleanup, five-run Release timing/memory, and Nsight when descriptor/upload ownership
  changes require external attribution.
- Performance acceptance: preserve the 10 ms base-frame contract and the M3.3 matched
  renderer gate; CPU simulation + render preparation remains below 4 ms in the
  representative scale scene; asset tick and upload scheduling satisfy prior p99
  gates; no unexplained persistent/transient/staging/DDC growth; steady non-streaming
  allocation median/p99 remains zero.
- Rollback: preserve last known-good artifact hashes and source-control reversal.
  DDC deletion recovers to a clean recook; source files are never the shipping
  runtime fallback after cutover.
- Completion: write the M3 acceptance report, update roadmap/context and any accepted
  ADR clarifications, and leave a fresh-lead handoff independent of chat history.

## Delegation and integration

Default execution is one lead working sequentially. No subagent is authorized by
this plan. If the owner later approves delegation, safe bounded work includes:

- read-only sidecar/catalog scale experiments;
- deterministic artifact/container property tests after schemas freeze;
- texture quality/error analysis against immutable source/reference images;
- read-only geometry primitive/bounds audits;
- Asset Browser query-model tests after runtime interfaces freeze.

One owner at a time controls `AssetGuid`/metadata public types, importer/cooker
interfaces, artifact schemas, central catalog/DDC, runtime load/residency state,
RHI upload/capability contracts, Vulkan descriptor/material tables, cooked model
schemas, `MeshComponent`/serializer migration, central CMake, and durable architecture
documents. Do not run overlapping write-heavy work across these boundaries.

Every slice is reviewed against current source and this plan, built in both presets,
and integrated before the next dependent slice begins. Returned summaries never
substitute for source review and rerun evidence.

## Verification and performance evidence

Every slice runs:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug --output-on-failure

cmake --preset x64-release
cmake --build out/build/x64-release
ctest --test-dir out/build/x64-release --output-on-failure
```

Renderer/RHI slices additionally run 3840x2160 Vulkan validation on:

- `material_lab_v1`, `transparency_v1`, and `opaque_emissive_v1`;
- `material_gpu_lab_v1`, `standard_shading_opaque_v1`,
  `standard_shading_forward_v1`, and `complex_closure_lab_v1`;
- the optional revision-2 sample car when locally available;
- selected and unselected Final, Depth, Base Color, Normal, Material ID,
  Closure ID, and wireframe captures;
- the material-84 material-ID/depth coverage gate;
- SDR, scRGB, and HDR10 output transports when renderer ownership changes.

Determinism evidence includes:

- two empty-DDC cooks in independent processes;
- at least two worker-thread counts and randomized job scheduling;
- exact artifact/container and diagnostic hashes;
- source timestamp changes without byte changes;
- dependency edits with exact reverse invalidation;
- catalog rebuild from sidecars only;
- DDC corruption/truncation recovery and atomic publication;
- rename/move with GUID preservation and ambiguous subasset rejection.

Release performance decisions use the reference RTX 4090/i9-14900K at 3840x2160,
validation off, fixed content/camera/output/cache state, at least 500 warm-up and
10,000 measured frames, and five independent runs. Report median, p95, and p99:

- source scan, metadata parse, catalog rebuild/query, dependency scheduling;
- cold import/parse, each cook stage, DDC lookup, artifact read/decode, CPU-ready;
- upload queue latency, submitted bytes/batches, staging peak, completion, and
  residency changes/evictions;
- CPU asset tick, render extraction/record/submission, allocation calls/bytes;
- GPU GBuffer, lighting, forward, output, UI, transfer interference, and total;
- persistent CPU blob, image, buffer, descriptor/table, staging, graph transient,
  driver budget, DDC, and thumbnail memory/storage.

The current sample-car 10,000-frame data and accepted M2 five-run material baseline
are comparators, not automatic M3 acceptance after content or representation changes.
Every comparison labels cold/warm DDC and resident/streaming state.

## Risks, fallback, and rollback

- **GUID creation causes noisy or unstable source control:** sidecars are written
  atomically only through explicit import/adopt operations. Scanning never regenerates
  missing identity silently. Duplicate/ambiguous state blocks cook with repair tools.
- **Subasset reorder transfers the wrong identity:** use exact keys and unique
  fingerprints, retain tombstones, and require explicit resolution for ambiguity.
- **Catalog becomes source of truth:** make clean rebuild a required test and keep
  runtime catalogs as cooked products. Never serialize database row IDs.
- **Nondeterministic third-party decoders/compressors:** pin tool versions and settings,
  isolate output in reference tests, and reject codecs that vary across process/thread
  counts or record platform-specific products explicitly.
- **DDC key omits a semantic input:** domain-separate and snapshot every key field;
  mutation tests must prove source, settings, importer, compiler, dependency, and
  target changes invalidate the product.
- **Async work introduces races or stale GPU indices:** immutable revisions, explicit
  tickets, generation checks, frame/timeline retirement, stress tests, and typed
  fallback slot zero are mandatory.
- **Descriptor indexing limits vary:** query all required features/limits before
  renderer creation, expose the active mode, and reject visibility/material-resolve
  use when scalable indexing is unavailable.
- **Global tables consume large fixed memory:** grow dynamically, measure live versus
  capacity bytes, compact only at safe boundaries, and never preallocate the maximum
  synthetic scale in every normal run.
- **Texture variants duplicate storage:** separate view from storage and share
  compatible payloads. Distinct compression is allowed only when semantic/fidelity
  requirements differ and the DDC/runtime delta is reported.
- **Geometry optimization destroys future inputs:** validate primitive/source mapping,
  bounds, winding, canonical positions/indices, and RT reconstruction before accepting
  packing or quantization. Keep an unoptimized reference cook for comparison.
- **Browser pulls M4 forward:** limit M3 scene work to GUID asset assignment and one
  legacy field migration. M4 retains general serializers, entity UUIDs, drawers,
  undo, and unknown-data preservation.
- **Warm cooked load appears fast by deferring a first-use hitch:** measure CPU-ready,
  upload-ready, and actually resident/drawable separately; record frame-time spikes
  during streaming.
- **Licensed car becomes mandatory:** duplicate all required behaviors in tracked
  redistributable fixtures. The car remains supplemental, except as the locally
  requested precondition diagnostic.
- **Production cutover hides a regression:** old and new paths remain A/B comparators
  until matched tests/captures/timings pass. Remove the old path only in M3.7.

## Approved owner decisions

The owner approved the following seven recommendations together on 2026-07-25:

1. **Catalog storage:** use SQLite for the rebuildable editor catalog, with an
   `AssetCatalog` interface and a compact cooker-produced runtime catalog. SQLite
   provides indexed search, transactions, and scale without making database rows
   persistent identity. A custom flat catalog is simpler to vendor but recreates
   query/index/transaction behavior.
2. **GUID and sidecar policy:** use opaque RFC 9562 UUIDv7 values in canonical
   lower-case text, persisted in `<source>.iridium.meta`; exported subassets also
   receive persisted GUIDs. Exact source keys and unique structural fingerprints
   assist reimport, but ambiguous identity transfer fails for user resolution.
3. **DDC scope:** ship a local content-addressed DDC only in M3. Keep the cache
   interface tierable, but defer shared/remote service, authentication, eviction
   coordination, and network failure behavior.
4. **Indexed-resource capability policy:** make Vulkan descriptor indexing the
   production/reference mode. Retain the current per-material descriptor path only
   as a bounded editor compatibility mode through M3 acceptance; mark scalable M7
   material resolve unavailable in that mode. Do not optimize M3 around low-end
   hardware.
5. **Texture product/tool choice:** select the deterministic codec/container in M3.3
   from a measured bake-off. The recommended destination is GPU-native BC7 color,
   BC5 normal, BC4 scalar, and suitable HDR products in a versioned sectioned
   container, with uncompressed references for fidelity tests. Do not freeze a
   third-party compressor before determinism, quality, throughput, licensing, and
   Vulkan-format/view compatibility are measured.
6. **Reimport publication:** publish immutable new runtime revisions atomically only
   after required uploads succeed; retain the last known-good revision on failure and
   retire old GPU slots after frame completion. Do not mutate a live material in
   place when its closure class or dependencies change.
7. **Scene transition:** allow the narrow M3 `assetGuid` field plus load-only
   `meshPath` migration described above. Defer general schema/version/serializer
   architecture to M4 rather than blocking GUID-based M3 assignment.

Any later change that contradicts an accepted ADR requires a new superseding ADR
before implementation; none is required by the approved set.

## Decision log

- 2026-07-25: M0-M2 and all accepted ADRs were read completely; the dirty worktree
  and current source were reinspected before planning.
- 2026-07-25: The sample-car wheel/tire precondition reproduced as real missing
  opaque coverage. Standard deferred materials wrote GBuffer identity without depth
  because the canonical override enabled depth only for forward-opaque closures.
- 2026-07-25: Queue-derived depth writes, architecture coverage, revision-2 car
  metadata, and material-ID/depth capture validation corrected and froze the M2
  behavior before M3 architecture work.
- 2026-07-25: Current M2 material concepts are accepted inputs to M3, not temporary
  types to replace. M3 changes asset identity, cooking, resource indirection, upload,
  and residency around them.
- 2026-07-25: Current `mergeMaterials` is explicitly rejected as the cooker contract
  because it destroys disconnected primitive boundaries and bounds needed by
  ADR-0003, M6, M7, M8, and M10.
- 2026-07-25: The owner approved all seven recommended decisions. ROADMAP M3 changed
  to In Progress and M3.1 became the sole active implementation slice. No accepted
  ADR requires amendment.
- 2026-07-25: M3.1 implementation completed with metadata schema 1, catalog schema 1,
  a tracked fixture sidecar, headless inspection, deterministic move/rebuild tests,
  Debug/Release 21/21, and passing 100,000-record search gates. M3.2 remains not
  started pending owner acceptance of M3.1.
- 2026-07-25: The owner accepted M3.1 and authorized the next sequential slice.
  M3.2 became the sole active implementation slice.
- 2026-07-25: M3.2 implementation completed with importer/settings/dependency
  contracts, artifact container 1, the tierable local DDC, tracked headless fixture,
  Debug/Release 22/22, independent-process determinism, corruption recovery, and
  passing lookup/scheduling gates. M3.3 remains not started pending owner acceptance
  of M3.2.
- 2026-07-25: The owner accepted M3.2 and authorized the next sequential slice.
  M3.3 became the sole active implementation slice.
- 2026-07-25: The M3.3 codec bake-off selected pinned DirectXTex May 2026 CPU
  products and BC7 quick iteration quality after repeated deterministic hashes and
  measured error/throughput. Texture product schema 1, deterministic semantic mip
  generation, exact BC upload layouts, indexed residency/sampler contracts, and
  Vulkan descriptor-indexing capability reporting landed. Production shader cutover
  and M3.3 acceptance gates remain open.
- 2026-07-26: M3.3 was accepted after production DirectXTex import/cook/DDC
  integration, fence-scoped physical descriptor-table growth, delayed texture-index
  reuse, 4K churn validation, tracked texture-grid and sample-car captures, full
  Debug/Release 23/23 tests, and a passing refreshed five-run renderer gate.
  M3.4 became the sole active implementation slice.
- 2026-07-26: M3.4 schema/importer checkpoint landed with persisted primitive and
  material GUID references, non-destructive primitive records and bounds, separate
  canonical raster/RT streams, stable discovered-subasset fingerprints, typed runtime
  artifact validation, pinned fastgltf/M2 material ingestion, and deterministic
  headless build/cache-hit evidence. M3.4 remains active until cooked runtime rendering,
  source/cooked parity, sample-car, validation, and performance gates pass.
- 2026-07-26: The optional sample car imported 205 stable subasset identities,
  cooked 118 preserved primitives with 78 dependencies and zero diagnostics, and
  passed the warm-load CPU gate through validated cook receipts: 0.506 seconds with
  no glTF parse versus the audited approximately 4.9-second Debug source path.
  Cooked material/texture publication and renderer A/B parity remain open.
- 2026-07-27: M3.4 was accepted with importer/model schema 3, complete compiled
  closures, 76 embedded typed texture views, source-free RHI publication, stable
  material/primitive/image GUID resolution, and corrected BC5 normal-Z
  reconstruction. The sample-car Depth, Material ID, and Closure Class A/B captures
  are byte-identical; the 4K final A/B is 0.1262/255 RMSE; Debug Vulkan validation
  and Debug/Release 26/26 pass; and the five-run 10,000-frame median GPU time is
  0.946368 ms versus the accepted 0.955 ms source baseline. M3.5 became the sole
  active implementation slice.
- 2026-07-27: M3.5 began with the stable-GUID runtime publisher, strict per-tick
  upload budgets, last-known-good failure retention, monotonic revisions, pin-aware
  LRU residency, and in-place cooked-model hot replacement through fence-deferred
  RHI frees. The 100,000-record publisher benchmark passes both CPU p99 gates and
  Debug/Release pass 27/27; automatic file/dependency/cook integration remains open.
- 2026-07-27: M3.5 was accepted after composing the source monitor, reimport
  scheduler, runtime publisher, residency manager, and live application counters.
  A real external-texture edit recooks and publishes a new model revision; a failed
  revision retains the last-known-good asset. Valid and invalid in-flight sample-car
  replacement runs are Vulkan-validation clean, Debug/Release pass 32/32, and the
  2,500-frame 4K run records 2.612992 ms GPU-frame p95 and 0.4142 ms
  asset-runtime-tick p99. M3.6 became the sole active implementation slice.
- 2026-07-27: M3.6 established the cached Asset Browser model and editor window,
  strict GUID/type drag payloads, Inspector and double-click model assignment,
  manual background reimport, GUID-only new scene saves, load-only legacy
  `meshPath`, and removal of source-file import from Scene Hierarchy. Debug/Release
  pass 34/34. Background browser import/catalog refresh, real thumbnails, cooked
  nonresident assignment, and the complete interaction/scale gate remain open.
- 2026-07-28: Asset Browser import and catalog refresh moved to
  `AssetCatalogService` background jobs and share the headless importer's metadata
  and subasset identity implementation. Reimport preserves GUIDs, outside-root
  imports cannot write sidecars, source-plus-sidecar moves retain identity after a
  refresh, and Debug/Release pass 35/35. Thumbnail, cooked nonresident assignment,
  dependency/settings UI, and interaction/scale evidence remain open.
- 2026-07-28: M3.6 added background cooked preparation for nonresident GUID
  assignment, removed source parsing from the GUID resolution path, produced real
  cooked model/material/texture thumbnails, added cancellable visible-page demand,
  a strict upload queue and bounded LRU GPU cache, and exposed cached settings and
  dependencies in the Browser. A process-lifetime DirectXTex WIC owner fixes
  sequential worker imports. Debug/Release pass 37/37; a 100-preview live
  validation run stays within its 512 KiB upload budget with zero failures. The
  owner-observed 100,000-record interaction workflow remains the only M3.6 gate.
- 2026-07-28: The second M3.6 workflow correction made `assets/` the current physical
  project root, added source-controlled folder/asset operations, nested cooked
  model-material-texture drawers, deterministic glTF import scale, viewport and
  hierarchy model drops, Mesh Inspector model/material assignment, entity naming,
  and scene persistence for pending model GUIDs and material overrides. It also
  corrected the main Create/first-save menu integration and enabled MSVC C++
  exception unwinding for deterministic rejected-job cleanup. Debug/Release pass
  39/39 and 1280x720 plus 3840x2160 Vulkan-validation runs report no messages.
  M3.6 remains In Progress pending the repeated owner interaction gate.
- 2026-07-28: The third M3.6 correction removed a per-frame 1,768-record thumbnail
  demand rebuild, reducing the sample-car Release wall average from 9.022869 ms to
  0.769837 ms (approximately 1,299 FPS) and validated Debug at a 2.9594 ms median
  CPU frame with validation enabled. It also added selected-asset detail thumbnails,
  reliable grid drawers, five-level Browser zoom, main-view physical folders,
  context-sensitive content menus, a paged thumbnail material grid, and responsive
  compact-window panes. Debug/Release remain 39/39 and 4K validation is clean.
  M3.6 still awaits the repeated owner interaction gate.
- 2026-07-28: The fourth M3.6 interaction correction introduced reusable
  editor-only collection chrome for vertically resizable content, zoom, and
  grid/list presentation; indented every component body; and applied it to the
  Mesh material-slot view while retaining four-slot paging. The Mesh component now
  shows its current model thumbnail and accepts replacement model drops directly
  on that preview. Model drawer activation moved out of clipped grid cards into one
  Browser-root popup, card width now reserves a real arrow hit target, and Browser
  status text is pinned below the folder tree or included in the content header when
  the tree is hidden. Scene Hierarchy accepts model drops over its entire surface
  and row dragging writes a serialized `siblingOrder` rather than depending on ECS
  dense-array order. Asset Details keeps 96x96 catalog tiles but requests only the
  selected record through a separate 256x256 CPU-generation lane and one replaceable
  GPU preview slot (256 KiB RGBA8); an interactive isolated model viewer remains a
  deliberate future editor slice because it requires its own render target, camera,
  input, material-debug modes, and residency lifecycle. The new high-resolution lane
  and hierarchy-order round trip have focused coverage; Debug/Release remain 39/39.
  A 600-frame Release validation run at 1280x720 emitted no validation messages and
  recorded a 0.341152 ms median GPU frame. Concurrent game/launcher workloads made
  CPU wall data unsuitable as a replacement performance baseline, so the prior clean
  0.7562 ms median CPU-frame checkpoint remains authoritative. M3.6 still awaits the
  repeated owner interaction gate.
- 2026-07-29: The fifth M3.6 interaction/performance correction made catalog queries
  explicitly dirty-driven, retained the selected asset's dependency/association
  detail instead of copying it every frame, batched thumbnail-state reads, clipped
  off-screen Browser grid/list rows, and lowered the opportunistic CPU-thumbnail
  worker priority. The clean Release checkpoint improved from 0.3016 to 0.1605 ms
  median `cpu.editor.build` and from 0.7562 to 0.6272 ms median
  `cpu.frame.total`; median GPU UI work remained only 0.0174 ms, so replacing ImGui
  is not justified by the measured bottleneck. Mesh material list rows now keep
  thumbnail, identity, and actions together without nested scrolling, while every
  Inspector component body is independently height-resizable with one outer
  overflow scrollbar. The default scene and benchmark manifests now follow the
  physical `alfa_romeo.gltf` rename instead of retaining stale `scene.gltf`
  references. Source inspection also confirmed that the Alfa's dark plastics and
  metallic tires originate in its glTF data: omitted `metallicFactor` values legally
  default to 1.0 and tire material 84 explicitly authors approximately 0.98, whereas
  the Bugatti explicitly authors most core materials nonmetal despite using the
  specular extension much more broadly. No provenance-destroying import heuristic
  was introduced. The present transparent path still performs two bounded
  scene-color copies plus depth/complex-forward work; the 1280x720 checkpoint records
  0.0164 ms median copy work and 0.0369 ms depth/forward work at the reference
  camera. At 4K, median GPU-frame time is 1.7040 ms, GPU UI is 0.1444 ms,
  editor construction is 0.3128 ms, and the CPU waits 1.5854 ms at the renderer
  fence; this confirms that the maximized-window limit is dominated by the
  resolution-scaled render workload rather than ImGui drawing. Close-up glass cost
  remains coverage-dependent and belongs to the measured M6 transparency work
  rather than an unmeasured M3 rewrite. Debug/Release pass 39/39, and a 600-frame
  Release Vulkan-validation run emitted no validation messages. M3.6 still awaits
  the repeated owner interaction gate.
- 2026-07-29: The sixth M3.6 import correction fixed three coupled large-model
  failures exposed by Sponza. The native file dialog allowed external selection while
  the worker silently rejected every source outside `assets/`; the import job also
  generated the full vertex-heavy cook intermediate merely to create a metadata
  sidecar; and shutdown requested worker cancellation without propagating it into
  the active source parse, cook, thumbnail, or model-preparation work. External glTF
  import now transactionally stages the source and its relative buffer/image
  dependencies in a physical package folder beneath the currently viewed project
  folder, never writes a sidecar outside the project, rolls back incomplete packages,
  and then runs lightweight deterministic metadata discovery. File reads, JSON
  discovery, full importer preparation/cooking, DDC work, thumbnails, and runtime
  model preparation now share cooperative stop tokens; a deliberately blocked
  importer shuts down in under 500 ms. A reusable bounded, thread-safe engine log and
  cached Console panel expose queued, active, copied, completed, failed, and cancelled
  asset jobs through `Window > Console`, with severity and text filters, copy, clear,
  and auto-scroll. Debug/Release remain 39/39, a validation-enabled 300-frame Release
  run emitted no Vulkan messages, and the open idle Console preserves the clean
  1280x720 checkpoint at 0.6245 ms median CPU frame, 0.1656 ms editor build, and
  0.0184 ms GPU UI. A post-change native-4K checkpoint with the Alfa Romeo scene
  measured 2.2875 ms median CPU frame, 0.3367 ms editor build, 0.0291 ms CPU UI
  recording, 1.7048 ms GPU frame, and 0.1475 ms GPU UI. M3.6 still awaits the
  repeated owner interaction gate.
- 2026-07-30: The seventh M3.6 interaction correction reproduced the owner's exact
  imported Sponza package. Import staging was correct, but image 68, `white.png`,
  was a 128-byte unresolved Git LFS pointer rather than PNG data; all other 68
  images were present. glTF metadata and full-cook preflight now reject unresolved
  LFS buffer/image dependencies before decoding other images and report the exact
  index, URI, and recovery action in the Console. The Asset Browser no longer
  demands every hidden primitive, material, and texture for every visible model:
  ordinary pages demand only displayed/inspected records, an open association
  drawer temporarily adds its materials and textures, and an inactive dock tab
  clears Browser and selected-preview demand. This removes the observed
  1,966-record demand against the 512-entry GPU thumbnail cache that repeatedly
  evicted and recooked the Alfa Romeo and Bugatti roots. Folder-tree popup IDs are
  now physical-path scoped, eliminating the repeated context-menu contents and
  Dear ImGui ID collision. External imports remain project-owned physical copies;
  deleting an asset removes its project source and sidecar while preserving the
  original external source. The catalog remains derived from project files, and
  sidecars store identity/settings rather than fragile machine-local references.
  Debug/Release remain 39/39 and a validation-enabled 300-frame Release run
  emitted no Vulkan messages. No ADR or roadmap status changed. M3.6 remains In
  Progress pending the repeated owner interaction gate with the real `white.png`
  payload restored.
- 2026-07-31: The eighth M3.6 import correction separated catalog registration,
  model preparation, and thumbnail production after the restored Sponza package
  exposed a pathological cold-cook path. A visible uncooked model previously caused
  the thumbnail worker and scene-preparation worker to independently parse every
  image before DDC coalescing, and `requestPreparedCook` copied the resulting
  `PreparedAssetCook`; Sponza's full source inspection took 4.2501 seconds and held
  1,140,852,600 bytes of decoded float image intermediates before its BC iteration
  cook then exceeded 120 seconds without publishing an artifact. Model source
  intermediates now retain the 42,991,228 bytes of encoded image payloads and decode
  one texture recipe at a time during cooking; the same Release inspection takes
  1.805 seconds, retains zero decoded image intermediates, and still discovers the
  same 197 subassets and 72 dependencies. Prepared cooks move through shared
  ownership instead of gigabyte-scale lambda copies. A newly registered model's
  browser tile no longer triggers a cold runtime cook: it remains a bounded
  placeholder until first scene use publishes a validated cook receipt, after which
  thumbnail generation consumes that same DDC artifact. Editor targets use
  deterministic RGBA8/RGBA16 preview texture products while production/iteration
  targets retain the accepted BC products, and the model worker emits phase and
  five-second progress messages instead of appearing frozen. The corrected Sponza
  editor cook completes successfully with cook key
  `a92dbf8b585b09858c68f9caf165d899d2ef9961a6ecf3cbde67a26b8941eb6a`
  and artifact hash
  `53c39755ffbec60a393c1d4310c1a18280af850dc7f771cde0e48b84b4548654`;
  the measured cold path is 29.435 seconds and the receipt/DDC path is 5.329
  seconds. This first-use cost is now asynchronous and visible, not part of import
  registration; progressive independently resident texture products remain the
  longer-term route to sub-second placement of packages this large. The final
  Sponza blocker was not metadata: one authored tangent became degenerate after its
  node transform. Missing or degenerate normals/tangents are now regenerated
  deterministically per primitive, preserve primitive identity and bounds, and have
  a targeted regression fixture. No ADR, schema, or roadmap status changed. M3.6
  remains In Progress pending the repeated owner interaction gate. Debug/Release
  pass 39/39, and a validation-enabled hidden 300-frame Release run at 1280x720
  emitted no Vulkan validation messages.
- 2026-07-31: The ninth M3.6 runtime-publication correction fixed the owner's
  non-rendering Sponza assignment. The successfully cooked editor artifact contained
  approximately 384 MiB of uncompressed preview texture data, while the runtime
  publisher enforces a strict 128 MiB single-frame admission budget. Because the
  publication callback is currently monolithic, the request was deferred every
  frame and could never become resident. Editor preview texture products now begin
  at the first semantic-correct mip no larger than 512x512; full-resolution
  production and iteration BC products are unchanged. The corrected Sponza artifact
  is 117,653,137 bytes total, estimates 112,098,600 GPU upload bytes (including
  95,070,884 texture bytes), and therefore fits the existing bounded publication
  contract. It contains 103 preserved primitives, 25 materials, 69 texture views,
  192,496 vertices, 786,801 indices, and bounds from approximately
  `(-15.368, -1.012, -9.462)` to `(14.399, 11.435, 8.843)`. Its clean Release cold
  cook completed in 12.833 seconds with cook key
  `380ff6d8062f3a4cfa579570dbf568092a7f39d33d1a8590bd5953d1b933135e`
  and artifact hash
  `76d3ecb00bc3decbe8c9b9e14889fd785f7f23df87bbfee85371ce3daa5a1582`.
  Loading that exact artifact for 60 hidden frames with Vulkan validation enabled
  emitted no validation messages. The application now reports an explicit Console
  and runtime-asset error if another monolithic model product exceeds the editor
  admission budget instead of leaving it silently queued. Progressive texture
  publication remains the required future solution for full-resolution sub-second
  placement of arbitrarily large packages; the bounded preview is the M3 editor
  path and does not alter production product quality. Debug/Release remain 39/39.
- 2026-07-31: The owner repeated the corrected workflow after the Sponza
  runtime-publication fix and accepted M3.6. Import, placement, rendering, Browser
  operations, scene persistence, and reload satisfy the owner-observed interaction
  gate. M3.7 became the sole active slice. The owner's separate report that material
  appearance remains visibly incorrect is carried into M3.7 verification: missing
  production lighting/IBL remains M5 scope, while any glTF-to-compiled-closure or
  cooked-binding mismatch remains an M2/M3 defect and blocks final M3 acceptance.
  No ADR or schema changed.
- 2026-07-31: M3.7 removed source-runtime model loading, path-keyed model identity,
  merge-by-material cooking, the load-only `meshPath` scene adapter, per-material
  descriptor allocation, non-indexed production shaders, and the fixed 4,096
  material table. Production now requires cooked GUID assets and indexed
  material/view/sampler resources. Dynamic material growth validated 65,536 real
  records; texture/sampler growth validated 8,192 records; 100,000 catalog records,
  10,000 reverse dependents, 1,024 rapid revisions, DDC lookup, and residency churn
  passed their gates.
- 2026-07-31: Two empty-DDC Sponza production cooks produced the same 117,659,836
  artifact bytes and SHA-256
  `e96e46f4967d32f32449b7eebdc29ea70c68066cdd1041ae4ad8b51b5f0e38af`.
  Strict source recompilation matched 87/87 Alfa, 23/23 Bugatti, and 25/25 Sponza
  cooked material hashes. The remaining appearance concern is therefore carried to
  M5 lighting/IBL without an import-time metallic/specular heuristic.
- 2026-07-31: Debug and Release pass 39/39, all eleven tracked M0-M2 cooked fixtures
  and the selected/unselected/debug/wireframe 4K car are Vulkan-validation clean,
  SDR/scRGB/HDR10 and resize paths pass, and the final five-run car median is
  0.8450 ms GPU / 1.7080 ms CPU. Steady allocation median/p99 remains zero.
  `docs/milestones/M3-acceptance-report-2026-07-31.md` records the full evidence.
  M3 is accepted. No ADR changed; M4 is the next proposed milestone.

## Completion report

M3 is accepted on 2026-07-31. The complete criterion review, final schema and
interface versions, cutover removals, deterministic hashes, material provenance,
scale results, 4K captures, Debug/Release/Vulkan evidence, performance and memory
baseline, deferred work, roadmap changes, and fresh-lead handoff are in
`docs/milestones/M3-acceptance-report-2026-07-31.md`.
