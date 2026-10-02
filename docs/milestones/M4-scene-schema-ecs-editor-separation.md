# M4 Execution Plan: Scene Schema, ECS Identity, and Editor Separation

- Roadmap milestone: M4
- Status: Accepted on 2026-08-03; owner-approved on 2026-07-31
- Lead: milestone lead task; one integration owner for scene identity, source and
  cooked schemas, ECS handles, editor transactions, and production cutover
- Last updated: 2026-08-03
- Dependencies: M0, M1, M2, and M3 accepted
- Relevant ADRs: ADR-0004 (primary), ADR-0003 and ADR-0006 (future persistent
  GPU-scene identity), ADR-0001 (material/subasset provenance), ADR-0002 (viewport
  output behavior), and ADR-0005 (primitive/transparency boundaries)
- Performance contract: `docs/performance/FRAME_BUDGET.md`
- Source baseline: `30252593f8fdd2de5dffbb8da31bb570ff49a7c0`, with the
  intentionally dirty accepted M0-M3 worktree preserved
- Approval gate: passed on 2026-07-31. The owner delegated the remaining product
  choices to the milestone lead and authorized M4 implementation one buildable
  vertical slice at a time. `ROADMAP.md` is `Accepted`.

## Objective and user-visible outcome

Iridium scenes will become durable project data rather than snapshots of transient
ECS indices. A scene entity will have a persistent UUID that survives save/load,
asset moves, runtime handle recycling, and editor sessions. Source scenes will be
deterministic, human-readable JSON with explicit versions, component identities,
migrations, asset/subasset references, diagnostics, and unknown-data preservation.
Production runtime builds will consume a separately cooked, validated binary scene.

Opening a malformed or unsupported scene will never clear the scene already open.
Saving will use an atomic replacement and last-known-good backup. Save, Save As,
dirty state, failed saves, and recovery will be owned by an editor document service.
Component data will no longer include ImGui, native-dialog, or editor-service code.
An editor-only drawer and property registry will provide consistent transactions,
multi-selection, validation, asset assignment, undo, and redo.

The current dense component pools will be measured before storage changes. M4 will
add stale-safe generational runtime handles because that is a correctness
prerequisite, but it will not assume an archetype rewrite. Any lookup, query, or
layout change beyond that prerequisite must pass a measured large-scene gate.

At the core acceptance gate a reviewer can:

- create, name, duplicate, delete, reparent, reorder, save, reopen, and undo/redo
  entities without serializing a runtime index;
- move or rename an M3 asset without changing a scene reference;
- save a pending/nonresident model and material override before publication, then
  resolve it later without changing history or dirty state;
- round-trip an unknown component and unknown properties without semantic loss;
- reject a duplicate/invalid UUID, broken required reference, unsupported future
  schema, corrupt save, or failed migration while retaining the current scene;
- cook the same source scene twice to byte-identical M3 DDC artifacts and load the
  result in a runtime-only target with no editor UI, SQLite, or source parser;
- resize the actual scene viewport render target and camera aspect without stretching
  the prior render image.

The isolated asset-opening/viewer framework is staged only after these gates. It is
an optional final editor slice and cannot weaken or delay the core scene acceptance
decision.

## Audit method and M3-preservation baseline

The lead read the required roadmap, planning rules, project context, performance
contract, all six ADRs, the M0/M1/M2/M3 acceptance reports, and the complete M2 and
M3 execution plans. Current source, tests, local scenes, CMake ownership, and git
status were then inspected. No source, runtime, editor, test, asset, or roadmap file
was changed during the audit.

The prescribed builds require the Visual Studio developer environment so MSVC and
Ninja are on `PATH`. With Visual Studio 2026 18.8.2, MSVC 19.51.36252, and Vulkan SDK
1.4.335, the current baseline is:

| Gate | Result on 2026-07-31 |
|---|---:|
| Debug configure and build | Pass |
| Debug CTest | 39/39 pass; 5.17 s |
| Release configure and build | Pass |
| Release CTest | 39/39 pass; 5.06 s |
| Focused scene asset-reference tests | Included; 4 current cases pass |
| Focused ECS transform tests | Included; 2 current cases pass |
| Focused viewport/editor-action tests | Included; current cases pass |

This rerun proves preservation of the automated M3 baseline, not M4 correctness.
The accepted M3 visual and performance comparators remain:

- eleven cooked M0-M2 fixtures and the sample car validation-clean at 4K;
- five-run 4K car median 0.8450 ms GPU and 1.7080 ms CPU;
- editor construction median 0.0537 ms and asset-runtime tick p99 0.0029 ms;
- steady allocation median and p99 of zero;
- deterministic Sponza production artifact SHA-256
  `e96e46f4967d32f32449b7eebdc29ea70c68066cdd1041ae4ad8b51b5f0e38af`.

Each M4 slice must rerun the focused tests plus the complete suites. Renderer/editor
integration slices must also repeat the relevant accepted cooked fixtures and
Vulkan-validation workflows. M4 acceptance repeats the complete M3 gate.

## Current implementation and defects

### Runtime entities and component pools

`Entity` is currently a plain `uint32_t`. `Registry::createEntity()` increments one
counter; destruction does not recycle an index or carry a generation; `clear()`
resets the counter to zero. Stale runtime references therefore cannot be
distinguished from a newly created entity after a clear or future index reuse.

Each `ComponentPool<T>` owns a dense `std::vector<T>`, a parallel dense entity
vector, and an `std::unordered_map<Entity, size_t>` sparse lookup. Removal swaps the
last dense element. `Registry` keys pools by `std::type_index` and `typeid(T)`.
Calling `getPool<T>()` creates a missing pool, including from read-only editor and
system queries. There is no general view/query abstraction, const lookup, alive
check, pool reserve policy, or memory/large-scene benchmark.

The pool interface contains `DrawInspector`, and `ComponentPool<T>` invokes
`T::OnInspector()`. This forces every pooled runtime type to satisfy an editor UI
contract. It also makes a runtime-only component target impossible today.

### Central scene serializer

`SceneSerializer` directly knows Transform, Mesh, Light, Relationship, and Name. It
iterates only the Transform pool, so an entity without a Transform is omitted. It
writes the transient `Entity` value as `EntityID`, then uses those transient values
for hierarchy parent/children references. There is no top-level schema version,
component type ID, component version, registry, migration chain, or structured
diagnostic.

Deserialization parses only the `Entities` array before calling `Registry::clear()`.
Later malformed GUIDs, invalid overrides, JSON type errors, and other failures clear
the partially loaded registry and therefore destroy the previously open scene.
Duplicate `EntityID` values overwrite the saved-to-loaded map. Missing relationship
targets are silently ignored. Parent, children, depth, and sibling order can
contradict one another without a complete consistency/cycle validation.

Writing uses `std::ofstream` directly to the destination. There is no sibling
temporary file, flush/verification, atomic replace, backup, or recovery record.
Unknown top-level, entity, component, and property data is discarded. Unsupported
future data is generally ignored. Output entity order follows mutable Transform-pool
dense order; deletion can change it through swap-remove.

### Currently serialized field inventory

The transitional unversioned document writes `Scene` and `Entities`. For each entity
that has Transform it can write:

| Location | Current fields | Required M4 disposition |
|---|---|---|
| Entity | `EntityID` | Migration input only; never written again |
| Entity | `Name` | Migrate to `iridium.component.name` v1 |
| Transform | `position`, `rotation`, `scale` | Preserve; matrices and dirty flag remain derived/transient |
| Mesh | `enabled`, optional `assetGuid` | Preserve GUID assignment including requested/pending precedence |
| Mesh | `materialOverrides[]` with `sourceMaterialGuid`, `materialGuid` | Preserve both stable subasset identities; migrate to explicit subasset-reference objects |
| Light | `color`, `intensity`, `type`, `range`, `radius`, `innerCone`, `outerCone`, `castsShadows` | Preserve exactly; M5 may later evolve semantics through component migration |
| Relationship | `parent`, `children`, `depth`, `siblingOrder` | Preserve parent/order intent; children and depth become validated derived data |

Mesh runtime-only state is deliberately not source data: `model`,
`requestedAssetSourcePath`, `assetResolutionDiagnostic`, and
`requestedMaterialAssetRoots`. `requestedAssetGuid` is authoring intent and wins over
the currently resident `assetGuid` during save, which must remain true. Asset
publication/residency is not an authoring edit.

The removed `meshPath` field is explicitly rejected by current tests and remains a
hard migration diagnostic. M4 will additionally diagnose known predecessor path
shapes (`currentMeshPath`, `requestedMeshPath`, and `Mesh.FilePath`) rather than
silently ignoring them. No path-to-GUID compatibility lookup will be restored.

### Existing scene data and tests

`SceneAssetReferenceTests` currently covers GUID-only mesh/name/override round-trip,
explicit `meshPath` rejection, malformed GUID rejection, and sibling-order
round-trip. It creates temporary JSON; there is no committed current-schema scene
fixture, migration matrix, atomic-save failure injection, unknown component,
unknown-property, duplicate UUID, hierarchy cycle, or failed-load-retention case.

`assets/scenes/test_scene.json` is the one tracked historical scene. It uses the
older `Transform`/`Mesh.FilePath` shape and must become a tracked rejection fixture
with a precise pre-M3 path-identity diagnostic; it is not valid compatibility data.
The checkout also contains ignored local authoring scenes:

- `2cars.json`: current M3 narrow `assetGuid` shape with two entities;
- `scene.iridium`: empty transitional scene;
- `sponza.iridium`: `currentMeshPath`/`requestedMeshPath` predecessor shape.

These local files are user data and will not be modified. M4.0 will copy only
minimal, license-safe structures into new tracked test fixtures. The current M3
asset/cooked fixtures remain the GUID, material-override, pending-residency, and
move/rename integration inputs.

### Editor ownership and transactions

The Inspector enumerates `Registry::getPools()`, derives display and behavior from
compiler RTTI names, special-cases Transform and Mesh by strings, and falls back to
`pool->DrawInspector()`. Component addition is another central menu switch. Name,
transform, mesh, material override, add/remove, delete, and hierarchy order mutate
component data directly.

Undo and Redo menu items are empty. There is no document transaction manager,
coalescing, multi-selection contract, save checkpoint, or dirty state. Asset
residency changes and authoring changes are not mechanically separated for history.

The File menu already owns editor-facing modal/native-dialog calls, which is the
correct side of the boundary, but its path/current-document state lives inside
`MenuBarPanel`. It calls the unsafe serializer directly. Save As changes no asset
identity contract, failed-load recovery is not safe, and failed-save/dirty behavior
is undefined.

Runtime components still include `editor/Reflection.h` or ImGui directly.
`Components.h` includes the editor reflection header. Transform, Light, and
Relationship implement UI behavior; Name and Mesh retain no-op `OnInspector` hooks
only to satisfy the pool interface.

### Viewport behavior

`ViewportPanel` aspect-fits the existing scene image inside the panel, which avoids
visible stretching. However, Application camera projection and scene render targets
still use the swapchain `renderExtent_`. Resizing adjacent panels changes only the
display rectangle; it does not update the actual scene render extent or camera
aspect. M4 must preserve aspect fitting during transitions while making the active
offscreen render extent follow the panel's pixel extent.

## Invariants

- Asset and exported subasset identity remains M3 GUID identity. Scene source and
  runtime code never use source paths, catalog row IDs, DDC paths, or transient RHI
  indices as persistent references.
- Schema-1 `.iridium.meta` sidecars remain source asset identity/settings authority.
  The SQLite catalog remains rebuildable editor state and is never required by the
  runtime scene loader.
- Runtime rendering loads cooked products and never parses glTF or source images.
- `meshPath` and all known predecessor path fields remain rejection/migration
  diagnostics. No source-path repair adapter is reintroduced.
- M2 source/compiled/instance/packed material separation, canonical closure values,
  stable material subasset identity, and material overrides remain intact.
- Scene, ECS, property, serializer, and component contracts are backend neutral.
  Vulkan objects and synchronization remain behind the RHI/backend.
- Component data and runtime scene targets have no ImGui, native file dialog,
  editor service, SQLite, or source JSON parser dependency.
- Unknown source component/property payloads are preserved semantically and are
  never silently discarded. Unknown components are not silently cooked.
- No transient ECS index, generation packing detail, pointer, RTTI name, unordered
  iteration order, C++ object representation, padding, or memory address is
  serialized.
- Scene load and save are transactional. A failure leaves the current scene,
  current path, save checkpoint, and undo history in a defined state.
- Asset pending/resident/failed/later-resident transitions do not create history,
  rewrite authoring GUIDs, or change dirty state.
- Relationship parent and sibling order are authoring data. Children, depth, and
  world transforms are derived and validated.
- ECS storage changes beyond generational handle correctness require Release
  measurements. M4 is not an archetype rewrite.
- Scene viewport panel resizing updates actual render extent and camera aspect;
  while a fence-safe resize is pending, the last valid image remains aspect-fitted
  and never stretched.
- M4 does not add production lighting, transparency algorithms, progressive texture
  publication, persistent GPU scenes, visibility buffering, or mesh shaders.

## Scope

- Typed persistent scene entity UUIDs and 64-bit generational runtime entity handles.
- Explicit UUID-to-handle and handle-to-UUID mapping, stale checks, deterministic
  create/duplicate/copy/paste remapping, and hierarchy reference validation.
- Source scene schema 1, canonical JSON, top-level and per-component versions,
  component/property identity, migration registry, diagnostics, and unknown data.
- Staged source load, atomic active-scene commit, atomic save/backup/recovery, editor
  Open/Save/Save As, dirty state, and failed-operation behavior.
- M3 importer/cooker/DDC integration and a versioned binary runtime scene product.
- Runtime-only scene loading and a build/link boundary proving editor independence.
- Editor property metadata, component drawer registry, selection/edit contexts,
  transactions, undo/redo, and coalescing.
- Property and structural workflows named in the milestone request, including asset
  and material assignment while pending/nonresident.
- Current ECS benchmark suite, before/after evidence, and only justified targeted
  lookup/view/data improvements.
- Actual scene-viewport render-target resize and camera-aspect integration.
- A late reusable asset-opening framework and isolated model/material viewer slice
  that reuses accepted editor services without placing assets into the active scene.

## Non-goals

- Direct/clustered lighting, IBL, shadows, probes, or baking (M5).
- General transparent sorting, OIT, peeling, layered glass, or production refraction
  (M6).
- Progressive independent texture publication, persistent GPU-scene records,
  indirect visibility, changed-data submission, or visibility-buffer rendering
  (M7).
- Meshlet construction or mesh-shader execution (M8).
- Source-path asset compatibility, runtime glTF parsing, per-material descriptor
  sets, or authoring workflow fields in GPU data.
- A prefab format or multi-scene streaming system. M4 defines identity/remap rules
  that those systems can reuse, but does not invent their authoring product.
- General scripting/reflection, network collaboration, or a complete project/level
  packaging system.
- Wholesale ECS archetype/chunk conversion without evidence.

## Design and data flow

### Identity model

M4 introduces two strongly typed identities:

```cpp
struct SceneEntityUuid { std::array<std::uint8_t, 16> bytes; };

struct EntityHandle {
    std::uint32_t index;
    std::uint32_t generation;
};
```

`SceneEntityUuid` and `AssetGuid` are different C++ types even though both use
canonical RFC UUID text. New entities receive UUIDv7 values from an injected
`SceneUuidGenerator`. Deterministic legacy migration and future namespace-derived
instances use UUIDv5 values in the scene-entity type. Nil, non-RFC variant, and
unsupported UUID versions fail validation. Tests use a deterministic generator; no
test relies on wall-clock or random ordering.

`EntityHandle` is packed to 64 bits only at ABI/storage boundaries. Source code uses
accessors, not shifts or masks. The Registry owns an index free list, alive bits, and
per-index generations. Destroy increments generation before returning an index to
the free list. Generation wrap is a hard diagnostic; generation zero and the all-one
null handle are reserved. Every component lookup first validates the handle.

`SceneIdentityMap` is owned by an active `SceneWorld`, not by a global catalog:

```cpp
class SceneIdentityMap {
public:
    expected<void, SceneDiagnostic> bind(SceneEntityUuid, EntityHandle);
    std::optional<EntityHandle> resolve(SceneEntityUuid) const;
    std::optional<SceneEntityUuid> persistentId(EntityHandle) const;
    bool containsAlive(SceneEntityUuid, const Registry&) const;
    void unbind(EntityHandle);
    expected<void, SceneDiagnostic> validate(const Registry&) const;
};
```

Bindings are one-to-one. Duplicate UUID, duplicate live handle, stale reverse map,
missing identity for a serializable entity, and identity for a dead handle are
errors. Cross-scene references, if later added, are keyed by scene asset GUID plus
entity UUID; an ECS handle is never cross-scene identity.

Identity allocation rules are fixed:

- create: allocate one UUIDv7 and bind only after entity construction succeeds;
- duplicate subtree: traverse hierarchy pre-order by `(siblingOrder, UUID)`, allocate
  new UUIDs in that order, clone known/opaque payloads, then remap all registered
  internal entity-reference properties through the old-to-new table;
- copy/paste: the clipboard stores persistent source UUIDs and component payloads,
  but paste always allocates new UUIDs using the same traversal/remap rule;
- external references remain external; nullable references remain null;
- an opaque unknown component is copied byte-semantically because its reference
  fields are unknowable. The transaction emits a visible diagnostic that opaque
  references were not remapped; strict prefab/cook flows reject that ambiguity;
- Save As preserves entity UUIDs because it is the same document state at a new
  scene asset location. The new source file receives a new M3 sidecar asset GUID.
  A future explicit Duplicate Scene Asset operation must remap scene/entity identity.

### Stable component and property identity

`ComponentTypeId` is a validated, permanent UTF-8 identifier, not RTTI and not a
hash. The grammar is lowercase reverse-domain-style ASCII segments:
`[a-z][a-z0-9]*(\.[a-z][a-z0-9_-]*)+`. Once shipped, an ID is never renamed or
reused. A replacement component receives a new ID plus an explicit migration.

Core IDs and initial source versions are:

| Component | Stable ID | Source schema | Cooked section |
|---|---|---:|---|
| Name | `iridium.component.name` | 1 | `NAM1` |
| Transform | `iridium.component.transform` | 1 | `TRN1` |
| Relationship | `iridium.component.relationship` | 1 | `REL1` |
| Mesh | `iridium.component.mesh` | 1 | `MSH1` |
| Light | `iridium.component.light` | 1 | `LGT1` |

The explicit four-byte cooked section code is also permanent and registry-unique; it
is not derived by hashing the string. Human display names are localized editor
metadata and never identity.

Property IDs are permanent component-local lowercase ASCII names such as
`position`, `rotation`, `scale`, `model`, and `material_overrides`. UI labels,
source member names, and widget choices can change without changing property IDs.
Metadata defines type, nullability, default, numeric/domain constraints, reference
kind, multi-edit policy, serialization order, and transaction/coalescing policy. It
contains no ImGui callback.

### Source scene JSON schema 1

The canonical source suffix is `.iridium.scene.json`. The source is a project asset
and therefore has a normal `<scene>.iridium.meta` sidecar with an M3 asset GUID and
scene importer settings. The sidecar owns asset identity; entity UUIDs live in the
scene document. Existing `.json` and `.iridium` files are migration inputs only.

Representative schema-1 source:

```json
{
  "format": "iridium.scene",
  "schemaVersion": 1,
  "name": "Vehicle Lab",
  "entities": [
    {
      "uuid": "019fb7d3-0100-7000-8000-000000000001",
      "components": [
        {
          "id": "iridium.component.name",
          "version": 1,
          "data": { "value": "Hero Vehicle" }
        },
        {
          "id": "iridium.component.transform",
          "version": 1,
          "data": {
            "position": [0.0, -1.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0]
          }
        },
        {
          "id": "iridium.component.relationship",
          "version": 1,
          "data": { "parent": null, "siblingOrder": 0 }
        },
        {
          "id": "iridium.component.mesh",
          "version": 1,
          "data": {
            "enabled": true,
            "model": {
              "assetGuid": "019f9cf0-378e-718d-84d6-3eb7b3d602fe"
            },
            "materialOverrides": [
              {
                "source": {
                  "subassetGuid": "019f9cf0-378f-7e9d-8482-2909403b4928"
                },
                "replacement": {
                  "subassetGuid": "019fa7a3-a854-7001-9000-000000000002"
                }
              }
            ]
          }
        },
        {
          "id": "studio.vendor.wind",
          "version": 7,
          "data": {
            "strength": 2.5,
            "futureProperty": { "mode": "gust" }
          }
        }
      ],
      "extensions": {
        "studio.note": "preserved entity extension"
      }
    },
    {
      "uuid": "019fb7d3-0100-7000-8000-000000000002",
      "components": [
        {
          "id": "iridium.component.name",
          "version": 1,
          "data": { "value": "Wheel Marker" }
        },
        {
          "id": "iridium.component.transform",
          "version": 1,
          "data": {
            "position": [1.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0]
          }
        },
        {
          "id": "iridium.component.relationship",
          "version": 1,
          "data": {
            "parent": "019fb7d3-0100-7000-8000-000000000001",
            "siblingOrder": 0
          }
        }
      ]
    }
  ],
  "extensions": {
    "studio.review": { "status": "draft" }
  }
}
```

The example UUID values are illustrative; production UUID creation follows the
rules above. The JSON representation obeys these canonical rules:

- UTF-8 without BOM, LF line endings, two-space indentation, final newline;
- duplicate JSON object keys are parse errors before DOM construction;
- top-level known keys are written in the order shown; entity known fields are
  `uuid`, `components`, then `extensions` when nonempty. Unexpected direct fields
  from a valid current-version document are retained with their original names and
  values and emitted lexicographically after known fields;
- entities are hierarchy pre-order; roots and siblings sort by nonnegative
  `siblingOrder`, with UUID as a deterministic tie-break used only for diagnostics;
  duplicate sibling orders are invalid in strict schema 1;
- known component entries sort by explicit permanent descriptor `sourceOrder`, then
  stable component ID as a tie-break. Core order is Name, Transform, Relationship,
  Mesh, then Light; unknown components follow in lexicographic ID order. There is at
  most one component of a given ID on an entity unless its descriptor explicitly
  declares multiplicity in a later top-level schema;
- known properties use descriptor order. Unknown properties are retained and then
  emitted lexicographically after known properties. Unknown component `data` and
  envelope fields are retained as a semantic JSON tree and canonically emitted;
- finite JSON numbers only. Transform values are JSON numbers decoded to finite
  IEEE-754 float32 under round-trip-safe formatting. NaN/Inf are errors;
- absent means use the versioned schema default. `null` is legal only when metadata
  declares nullability. Required non-null fields cannot be omitted;
- Relationship `parent` is nullable. `children`, `depth`, matrices, dirty flags, ECS
  handles, and asset residency are never source fields;
- Mesh `model` may be null for an unassigned component. Root asset references use
  `assetGuid`; exported child references use `subassetGuid`. All are canonical M3
  lowercase UUIDv7 values. Material override sources are unique and output sorted by
  source subasset GUID;
- unknown top-level/entity fields are retained in an extension store and re-emitted
  at their original object level. Reserved core field collisions are errors, not
  last-writer-wins behavior.

Unknown payload preservation is semantic, not whitespace/comment preservation. JSON
has no comments in this schema. Loading and immediately saving an unknown component
must reproduce the same values, arrays, object members, number values, ID, and
version in canonical formatting. A strict source-scene cook rejects unknown
components and unknown known-component properties unless a registered descriptor
marks the field editor-only/forward-preservable.

### Registry and linker-safe composition

Registration is explicit and startup-composed. Runtime/cooked behavior and source
JSON codecs are separate so the runtime library does not acquire a JSON parser:

```cpp
struct ComponentMigration {
    std::uint32_t fromVersion;
    std::uint32_t toVersion; // must equal fromVersion + 1
    MigrationFn migrate;     // pure JSON-to-JSON transformation
};

struct RuntimeComponentDescriptor {
    ComponentTypeId id;
    std::uint32_t cookedSectionId;
    std::uint32_t currentCookedVersion;
    std::span<const PropertyDescriptor> properties;
    ResolveFn resolveReferences;
    PostLoadFn postLoadValidate;
    EncodeCookedFn encodeCooked;
    DecodeCookedFn decodeCooked;
};

struct SourceComponentCodec {
    ComponentTypeId componentId;
    std::uint32_t currentSourceVersion;
    std::uint32_t sourceOrder;
    SerializeSourceFn serializeSource;
    DeserializeSourceFn deserializeLocal;
    ValidateSourceFn validateLocal;
    std::span<const ComponentMigration> migrations;
};

class RuntimeComponentRegistry {
public:
    expected<void, RegistryError> add(RuntimeComponentDescriptor);
    const RuntimeComponentDescriptor* find(ComponentTypeId) const;
    std::span<const RuntimeComponentDescriptor> descriptors() const;
    expected<void, RegistryError> freezeAndValidate();
};

class ComponentSerializerRegistry {
public:
    expected<void, RegistryError> add(SourceComponentCodec);
    const SourceComponentCodec* find(ComponentTypeId) const;
    std::span<const SourceComponentCodec> codecs() const;
    expected<void, RegistryError> freezeAndValidate(
        const RuntimeComponentRegistry&);
};

RuntimeComponentRegistry createRuntimeSceneComponentRegistry();
ComponentSerializerRegistry createSourceComponentSerializerRegistry(
    const RuntimeComponentRegistry&);
```

Each component module exposes ordinary runtime-registration and, in the authoring
module, source-codec-registration functions. The two composition roots call those
functions explicitly and freeze their registries. Source codecs use the authoring
module's canonical JSON value; runtime descriptors never mention JSON. There are no
translation-unit static registrars, constructor attributes, linker sections, RTTI
enumeration, or whole-archive requirements. Build files list the module translation
units, and a registry enumeration test asserts the exact core IDs, source/cooked
versions, section IDs, property IDs, and migration continuity. Adding a component
adds its module registrations; it never edits a central serializer switch.

Duplicate component ID, property ID, cooked section ID, version gap, unordered or
branching migration, missing callback, invalid default, and illegal editor type in a
runtime descriptor are startup/test failures.

Editor drawers use a third explicit registry:

```cpp
struct ComponentDrawerDescriptor {
    ComponentTypeId componentId;
    DrawComponentFn draw;
    MultiSelectionPolicy multiSelection;
};

EditorComponentDrawerRegistry createEditorComponentDrawerRegistry(
    const RuntimeComponentRegistry& runtimeRegistry);
```

The editor registry must have at most one drawer per runtime ID. A component without
a custom drawer receives a metadata-driven generic drawer; runtime registration does
not depend on editor registration.

### Migration policy

There are two ordered layers:

1. `SourceSceneEnvelopeMigrator` migrates a supported top-level schema one version at
   a time to the current envelope.
2. The component registry migrates each known component one version at a time before
   local deserialization.

M4 supports the unversioned M3 transitional shape as logical source version 0 and
schema 1 as current. Version-0 migration requires a scene asset GUID from its M3
sidecar. It validates unique integer `EntityID` values and derives stable UUIDv5
entity identities from the scene asset GUID plus `legacy-entity/<EntityID>`. This is
deterministic across process, thread count, and source location. A migrated save
writes those UUIDs permanently; subsequent edits never derive them again.

Version-0 migration rules are exact:

- `Scene` becomes `name`; missing name uses `Untitled Scene`;
- every entity is migrated, including one without Transform;
- missing `Name` becomes `Entity <legacy EntityID>`;
- known Transform, Mesh, Light, and Relationship fields map to the schema-1
  descriptors above;
- `requestedAssetGuid` intent is represented by the saved version-0 `assetGuid` and
  remains pending after load until M3 runtime publication succeeds;
- material override GUID pairs become source/replacement subasset references;
- Relationship parent IDs resolve through the complete validated ID map. `children`
  and `depth` are checked against the derived hierarchy, then discarded as redundant;
- unknown version-0 entity fields are retained beneath a named legacy extension and
  produce a warning because no stable component ID can be inferred safely;
- `meshPath`, `currentMeshPath`, `requestedMeshPath`, `Mesh.FilePath`, malformed or
  nil GUIDs, duplicate EntityIDs, broken required parents, hierarchy cycles, and
  contradictory redundant hierarchy fields fail migration with structured context.

Top-level schema versions greater than current are rejected before active-scene
mutation. A known component version greater than its registered current version is
also rejected in normal open/cook. An explicit read-only recovery tool may preserve
such a component as opaque, but cannot save over the source or cook it. A wholly
unknown component type at any version is preserved and shown as unavailable in the
editor; strict cooking rejects it. Older versions below the supported floor fail
with a named minimum-supported-version diagnostic.

Migration functions are pure and independently testable. They receive a canonical
JSON value and migration context, not Registry, renderer, editor, wall clock, file
dialog, or asset catalog services. Asset existence and residency are resolution
concerns, not migration inputs.

### Diagnostics

All scene operations return ordered diagnostics:

```cpp
enum class SceneDiagnosticSeverity { Info, Warning, Error };
enum class ScenePhase {
    Read, Parse, EnvelopeMigration, Identity, ComponentMigration,
    Deserialize, ReferenceResolution, AssetResolution, Hierarchy,
    PostLoad, Save, Cook, RuntimeLoad, Transaction
};

struct SceneDiagnostic {
    SceneDiagnosticSeverity severity;
    std::string code;
    ScenePhase phase;
    std::optional<SceneEntityUuid> entity;
    std::optional<ComponentTypeId> component;
    std::optional<std::uint32_t> componentVersion;
    std::string propertyPath; // JSON Pointer syntax
    std::optional<std::uint32_t> migrationFrom;
    std::optional<std::uint32_t> migrationTo;
    std::string message;
};
```

Diagnostics sort by phase, entity UUID, component ID, property path, then code. Tests
assert codes and context rather than compiler-dependent exception text. Errors block
commit/cook. Warnings never authorize data loss or repair.

### Multi-phase source loading and atomic commit

The active scene is an aggregate:

```text
SceneWorld
  Registry                    runtime component storage
  SceneIdentityMap            persistent UUID <-> live handle
  SceneReferenceState         unresolved stable entity/asset references

EditorSceneDocument
  scene asset/path metadata
  unknown top/entity/component/property JSON
  transaction history and save checkpoint
```

`SceneLoadTransaction` owns a staging `SceneWorld` and source document. It cannot
mutate the active world. The required phases are:

1. read bounded bytes; detect BOM/encoding, duplicate keys, I/O and size errors;
2. parse and validate format/top-level envelope; reject unsupported future versions;
3. apply ordered top-level and component migrations in a temporary DOM;
4. validate all entity UUIDs, then create staging runtime entities and the complete
   bidirectional UUID/handle map;
5. deserialize known component-local scalar/value data; store unknown payloads in the
   editor document without instantiating fake runtime components;
6. validate and resolve registered entity/component references through the complete
   map. Missing required references are errors; optional unresolved references retain
   their UUID and explicit state according to property metadata;
7. validate M3 asset/subasset GUID types and dependency intent. Missing,
   nonresident, or failed cooked assets remain stable pending/failed references with
   diagnostics; source paths and SQLite rows are not consulted for identity;
8. request/associate already prepared cooked products through the M3 runtime service
   without blocking source parsing. Publication completion is not required for a
   valid authoring scene;
9. validate hierarchy parents, cycles, sibling uniqueness/order, then reconstruct
   children/depth and derived transform state. Schema 1 performs no silent hierarchy
   repair;
10. run component and whole-scene post-load validation. Only recomputation of
    explicitly derived data is automatic; any authoring repair requires a named
    policy and diagnostic;
11. if every required phase succeeds, swap the complete staging world/document into
    the editor at a frame-safe boundary, reset selection by UUID, establish a clean
    save checkpoint, and retire the old world normally. Otherwise discard staging
    and retain the current scene, path, selection, dirty state, and history.

Asset publication after commit updates transient component runtime state only. It
does not modify the source document or create an editor transaction.

### Save, backup, recovery, and document state

`EditorSceneDocumentService` owns New, Open Scene, Save, Save As, current path,
sidecar/adoption, diagnostics, recovery prompts, and dirty state. Panels send
requests; they do not instantiate serializers.

A save performs:

1. validate the live Registry/identity/reference/unknown stores without mutation;
2. build complete canonical JSON in memory;
3. write a unique sibling temporary file with create-new semantics;
4. flush file contents, close, reopen, parse, and verify the canonical content hash;
5. atomically replace the destination while retaining the prior valid file as one
   `.bak` last-known-good sibling; use `ReplaceFileW` on Windows and equivalent
   atomic same-filesystem replacement elsewhere;
6. remove only the operation's verified temporary file and update the save checkpoint
   after replacement succeeds.

Failure before step 5 leaves the destination and backup unchanged. Failure during
replacement is reported and remains dirty. Save As creates/adopts the new source
scene and sidecar transactionally, and changes current path/asset GUID only after
both succeed. It never deletes or rewrites the old file. New scenes begin dirty until
the first successful save.

Open checks primary and backup independently. A corrupt primary with a valid backup
offers explicit recovery; it does not silently overwrite either file. Orphaned temp
files are listed with hashes/timestamps for recovery and never auto-promoted. Loading
a backup still uses the full staging transaction.

Dirty state follows history state identity, not a lossy boolean. Every successful
transaction creates a unique state token. Save records the current token. Undo can
return exactly to the save token and become clean; branching after undo receives a
new token. Failed/no-op operations create no token. Asset runtime state, selection,
panel state, and thumbnail/viewer camera do not affect scene dirty state.

### Cooked runtime scene and M3 DDC integration

The scene importer/cooker is registered explicitly with M3. A source scene remains
JSON; runtime never reads that JSON. Cooking is strict and begins only after the same
current-schema validation used by editor load.

The cook key extends `IridiumCookKey/v1` with:

- scene asset GUID, source scene content hash, source envelope schema, and canonical
  migrated content hash;
- scene importer/compiler implementation version and registry manifest hash;
- every included component source/cooked schema and explicit cooker feature version;
- target platform/profile/quality and artifact container version;
- sorted required/optional M3 asset and subasset dependencies with content/artifact
  hashes;
- explicit cook policy; no editor recovery or silent repair policy is allowed.

The output reuses M3 `CookedArtifact` container version 1 with artifact type
`iridium.scene.runtime` and runtime scene schema 1. Its dependency table is the
runtime authority for referenced cooked assets; SQLite is absent. Proposed sections:

| Section | Contents |
|---|---|
| `SCN1` | fixed little-endian header, scene asset GUID, counts, directory offsets, registry manifest hash |
| `STR1` | length-prefixed UTF-8 string table for names and stable type IDs |
| `ENT1` | entity UUID table in canonical hierarchy order, parent entity index/null, sibling order, component-binding ranges |
| `NAM1` | name string indices |
| `TRN1` | finite float32 local position/rotation/scale only |
| `REL1` | validated parent entity index/null and sibling order; no children/depth duplication |
| `MSH1` | enabled bit, model dependency index/null, sorted source/replacement material dependency index pairs |
| `LGT1` | explicit enum/integer/float fields in fixed little-endian encoding |

`SCN1` includes magic, schema/endian marker, total decoded size, section/type/entity
counts, and checksums already complemented by the outer container checks. Offsets and
sizes are 64-bit and alignment is explicit. Component records are grouped by stable
type section for validated contiguous loading; they are not C++ object dumps. The
type directory maps the full stable component ID string to its explicit section ID
and cooked version. Unknown components and unsupported properties fail strict cook.

Runtime loading validates outer container identity/schema/target/dependencies, all
section versions/checksums/alignments/ranges, unique entity UUIDs, parent indices,
component counts, finite values, asset dependency types, and the registry manifest
before allocating a staging world. It then follows the same entity-map,
component-local, reference, hierarchy, post-load, and atomic-commit phases. It does
not instantiate nlohmann JSON, ImGui, file dialogs, SQLite, source importers, or glTF.

Two clean cooks in independent processes and at least two worker schedules must
produce identical artifact bytes. Warm DDC load performs no source parse or scene
compile. Corrupt/truncated products quarantine/rebuild in tools and fail safely at
runtime.

### Runtime/editor module boundary

M4 creates explicit build targets instead of continuing one monolithic dependency
accident:

```text
IridiumEcs
  EntityHandle, Registry, component pools/views

IridiumSceneRuntime
  scene UUID/map, runtime components, component registry,
  cooked scene validation/load; depends on IridiumEcs and backend-neutral asset APIs

IridiumSceneAuthoring
  source JSON DOM/schema/migrations/save/cook; no ImGui or native dialogs

IridiumEditor
  drawers, property widgets, document dialogs, transactions, panels, asset viewers
```

Runtime components become data/runtime behavior only. `Components.h` no longer
includes `editor/Reflection.h`; `IComponentPool` loses `DrawInspector`; all
`OnInspector` methods and ImGui includes leave `src/scene/components`. Property
metadata is backend-neutral and may be consumed by editor, cook tools, diagnostics,
or future scripting without widget code.

A dedicated `SceneRuntimeBoundaryTests` target links `IridiumSceneRuntime` and loads
a cooked fixture without linking ImGui, ImGuizmo, native file-dialog sources,
SQLite, Asset Browser, or editor panels. CMake link/interface tests and include scans
enforce this boundary; a passing monolithic engine executable is not sufficient.

### Property editing, drawers, and transactions

Editor drawers receive an `EditorComponentContext`, not unrestricted globals:

```text
selection UUIDs and resolved live handles
runtime component/property descriptors
EditorTransactionManager
EditorAssetSelectionService
SceneValidationService
multi-selection helpers
presentation/thumbnail services
```

The generic drawer renders properties from metadata. Custom Transform, Mesh, Light,
and Relationship drawers reuse the same property/transaction API for specialized
presentation. Asset pickers return stable GUID/subasset values; they never write a
component directly. The existing resizable component bodies and Mesh grid/list/zoom
collection UI remain editor-only services and are preserved.

Every edit is prepare/validate/apply/commit:

- `PropertyTransaction` stores stable target UUIDs, component/property IDs, and
  canonical before/after values;
- structural commands store validated component payloads or entity-subtree snapshots,
  including unknown source payloads where applicable;
- a failed validation or apply rolls back all targets and creates no history entry;
- undo/redo resolves UUIDs to current handles at execution time and fails atomically
  if preconditions no longer match;
- history stores asset GUIDs, not resident handles/resources, so publication and
  eviction cannot invalidate it;
- multi-selection applies one atomic transaction to the sorted UUID set. Mixed values
  are explicit; unsupported multi-edit metadata disables the control.

Required history commands cover property changes, component add/remove, entity
create/delete, subtree duplicate/copy/paste, rename, reparent, sibling reorder,
model assignment, material override add/change/reset, and supported multi-edit.
Deletion snapshots the complete subtree, known component source representation,
unknown payloads, UUIDs, parent/order, and asset references so undo is exact.

Continuous editing coalesces by
`(interaction token, sorted entity UUIDs, component ID, property ID)`. A widget
begins on activation, updates one pending after-value while active, and commits once
on deactivation-after-edit; Escape/cancel restores before-values. Gizmo manipulation
uses one token from mouse-down to release. Text entry commits once on accept/focus
completion. Discrete toggles, asset drops, and structural actions commit separately.
No-op before/after equality creates no entry.

### ECS measurement and change policy

M4.0 adds `IridiumEcsSceneBenchmark` around the current dense-vector plus
`unordered_map` implementation before behavior changes. Release runs use fixed
seeds, warmed memory, at least 30 measured repetitions, and report median/p95/p99,
allocations, resident/peak bytes, and bytes/entity for these workloads:

- create 10k, 100k, and 1M entities; add Transform-only and mixed
  Transform/Mesh/Relationship/Name occupancy;
- 1M randomized component lookup hits, misses, and stale-handle probes;
- dense Transform iteration and changed-transform update at 0%, 1%, 10%, and 100%;
- construction of Transform+Mesh and Transform+Relationship views plus full
  iteration;
- batch deletion/recreation at 1%, 10%, and 50%;
- breadth/depth hierarchy traversal at 10k and 100k entities;
- persistent UUID map insert/resolve/reverse/stale validation;
- source serialization and staged load at 1k, 10k, and 100k entities;
- cooked serialization/load at the same scales;
- editor hierarchy snapshot/sort, selection lookup, and Inspector descriptor
  construction without ImGui rendering.

The benchmark emits machine-readable JSON with compiler, configuration, CPU, counts,
seed, schema/registry hash, and sample counts. M4.1 reruns it after generational
handles/identity; later slices append source/cooked/editor results.

The default recommendation is to retain dense component arrays and the existing
sparse-map approach initially. Required correctness changes may add overhead but
must be attributed. Further changes require one of:

- at least 15% p95 improvement in a representative gated lookup/query/traversal
  workload with no more than 5% regression in dense iteration;
- removal of a measured allocation or memory spike with no material latency loss;
- a correctness need that cannot be met at the existing boundary.

A sparse-vector or paged sparse set is the first candidate if random lookup/map
allocation is measured as the problem. Cached explicit views are the first candidate
if query construction is measured as the problem. Archetypes are not an M4 fallback.
Unchanged storage is an acceptable, documented result. Regardless of storage,
M7-facing interfaces expose stable entity UUID/handle identity and changed-component
signals without defining GPU-scene record layout.

### Viewport extent and camera aspect

`ViewportPanel` will publish a desired content pixel extent after DPI scaling. An
editor viewport service clamps it to device/configuration bounds, ignores zero or
minimized panels, and debounces interactive resizing. Application/RHI request a
fence-safe rebuild of scene offscreen graph targets independently of swapchain size.
The active camera projection uses the active scene render extent, not the panel or
swapchain guess.

Until the new targets are active, the last completed image is aspect-fitted in the
panel. After activation, picking and gizmo rectangles use the exact displayed image
rectangle and projection. Tests cover wide/tall panels, DPI, rapid resize,
minimize/restore, camera aspect, render-target extent, and no resampling stretch.
Vulkan validation covers repeated resize with pending asset publication and normal
close. Release measurements report rebuild count, CPU spike, GPU idle/stall, and
requested/committed graph-memory changes; steady resize-inactive frames add no work.

### Reusable asset-opening framework

After the core scene, drawer, transaction, and viewport services are stable,
`EditorAssetDocumentRegistry` may register a viewer by stable asset type. Opening a
model or material creates an editor document/tab with its own selection, orbit/pan/
zoom camera, render target, debug mode, pins, and lifecycle. It never creates an
entity in the active scene and never changes scene dirty state.

The model viewer frames cooked bounds and exposes primitive/material/debug views.
The material viewer uses the accepted M2 closure/provenance diagnostics on a standard
preview scene. Both reuse editor property presentation, asset selection, transaction
interfaces where an asset override is actually editable, and the viewport extent
service. This slice cannot change source material provenance, renderer lighting, or
M6 transparency to improve appearance.

## Vertical slices

Only one slice is active at a time. Each slice is independently buildable and keeps
the old accepted path available until its replacement evidence is complete.

### M4.0 - Freeze fixtures, boundaries, and ECS/scene baseline

- Status: Accepted on 2026-07-31. Criterion-level evidence is in
  `docs/milestones/M4.0-fixtures-and-baseline.md`; performance evidence is in
  `docs/performance/M4.0-ecs-scene-baseline-2026-07-31.md` and its linked JSON.
- Preconditions: approve the schema/identity/registry decisions in this plan.
- Affected systems: new test fixtures and benchmark target only; no production
  behavior change.
- Work: add tracked copies of current M3 GUID scenes and pre-M3 rejection shapes;
  freeze current field inventory/diagnostics; add the Release benchmark described
  above around the unchanged Registry/serializer/editor construction; add a build
  dependency inventory that proves current UI coupling before removal.
- Tests: existing 39/39; characterize transform-only omission, duplicate saved ID,
  broken relationship, unknown-field loss, partial-load clear, and direct-save
  failure as expected current defects without accepting them.
- Measurements: all current ECS workloads, current v0 save/load at 1k/10k/100k,
  memory/allocations, and current editor hierarchy/Inspector construction.
- Captures/runtime: rerun representative cooked fixture startup and current viewport
  layout tests; no renderer image change expected.
- Fallback: fixture/benchmark-only; production remains untouched.
- Completion: versioned machine-readable baselines and migration/rejection fixtures
  exist for every current field and defect. The next slice does not infer performance.

### M4.1 - Generational handles and persistent scene identity

- Status: Accepted on 2026-07-31. Evidence is in
  `docs/milestones/M4.1-generational-handles-and-scene-identity.md`; performance
  evidence is in `docs/performance/M4.1-generational-identity-2026-07-31.md` and
  its linked JSON.

- Affected systems: `ecs/Entity.h`, `ecs/Registry.h`, component pools,
  TransformSystem, scene world/identity types, editor selection adapters, focused
  tests; no new source format selected by production yet.
- Work: introduce stale-safe 64-bit handles/free list/generations, non-mutating pool
  lookup, alive validation, `SceneEntityUuid`, injected generator,
  `SceneIdentityMap`, and creation/destruction binding. Keep dense component storage.
- Tests: handle reuse/staleness, generation increment/wrap policy, double destroy,
  clear/recreate, UUID parse/generation, duplicate/nil/invalid UUID, map forward/
  reverse consistency, missing identity, deterministic generator, create/destroy
  failure rollback, and TransformSystem hierarchy behavior with recycled indices.
- Measurements: repeat all baseline ECS lookup/iteration/create/delete/hierarchy and
  memory cases. Attribute handle/map overhead; no storage optimization is bundled.
- Fallback: source-control reversal before later schema depends on handles; old
  serializer remains operational through an explicit legacy-handle adapter only in
  this slice.
- Completion: no stale handle can access a new entity and every serializable entity
  has one persistent UUID, with both builds and all tests green.

### M4.2 - Source schema, component registry, migrations, and unknown data

- Status: Accepted on 2026-07-31.

- Affected systems: new `IridiumSceneAuthoring`/`IridiumSceneRuntime` foundations,
  component descriptors, canonical JSON, v0 migrator, diagnostics, fixtures. The
  current editor serializer remains the production fallback.
- Work: implement schema 1 exactly as specified; explicit runtime registry and core
  descriptors; pure v0-to-v1 envelope migration; per-component migration API;
  duplicate-key detection; unknown component/property/extension store; canonical
  writer; structured diagnostics.
- Tests: registry enumeration/order independence/duplicate rejection/version gaps;
  deterministic schema-1 output; all current fields; entities without Transform;
  defaults/nulls; unknown components/properties/top/entity extensions; top/component
  future versions; v0 GUID/name/mesh/override/light/hierarchy migration; every path
  predecessor rejection; duplicate IDs/UUIDs; broken/cyclic/inconsistent hierarchy;
  independent component migration vectors and exact diagnostic context.
- Determinism: repeated processes and randomized registration-call input produce the
  same frozen registry and canonical JSON; two round trips are byte-identical after
  first canonicalization.
- Measurements: parse/migrate/serialize 1k/10k/100k and allocation/memory results;
  compare to M4.0 without imposing premature shipping gates.
- Fallback: new authoring path remains opt-in/test-only; current serializer still
  owns editor Save/Load.
- Completion: adding a test component requires no central switch, v0 supported
  fixtures migrate, rejected path fixtures remain rejected, and unknown payloads
  survive semantic/canonical round trips.

### M4.3 - Staged load, atomic save/recovery, and editor document lifecycle

- Affected systems: `SceneWorld`, load transaction, editor document service, File
  menu/actions, selection-by-UUID, sidecar scene adoption, failure injection, current
  serializer call sites.
- Work: implement all load phases and swap-on-success; atomic save/backup/temp
  verification; Open Scene/Save/Save As; path/sidecar ownership; state-token dirty
  model; explicit backup/temp recovery UI. Switch editor source operations to the new
  path while retaining v0 migration code.
- Tests: save/open/Save As; first-save; failed temp write/flush/verify/replace/sidecar;
  failed parse/migration/reference/post-load retaining active scene/path/history;
  primary corruption with valid/invalid backup; orphan temp listing; dirty/clean
  transitions; Save As identity/path behavior; selection recovery by UUID; missing,
  pending, failed, and later-resident assets; GUID move/rename; material overrides;
  hierarchy order and unknown payload after editor round-trip.
- Runtime/editor verification: asset publication after load changes no document
  state; no source scan/cook blocks the ImGui frame; 4K representative scene load and
  editor workflow validation-clean.
- Measurements: save/load stage timings, bytes, allocations, peak staging memory,
  editor construction, and active-scene commit time at scale.
- Fallback: editor can reopen the last-known-good backup or continue the current
  scene. The previous serializer remains compiled only as a test comparator, not a
  save path.
- Completion: every failed load/save is non-destructive, current M3 authoring state
  round-trips, and the new source path is the sole editor writer.

### M4.4 - Deterministic cooked runtime scenes

- Affected systems: scene importer/compiler, M3 cooker/DDC registration, runtime
  scene sections/loader, headless cook/inspect tools, runtime-only build target.
- Work: implement `iridium.scene.runtime` schema 1 and the exact sections above;
  derive sorted dependencies; strict cook; source-free staged runtime load; DDC
  receipts/corruption behavior; registry manifest validation.
- Tests: bit-exact clean cooks across processes/schedules; warm hit without JSON
  parse/compile; header/section/version/range/checksum corruption; duplicate UUID;
  invalid parent/dependency/component stream; source-to-cooked semantic equality;
  missing required/optional/pending asset policy; runtime load/commit failure
  retention; unknown-source component cook rejection.
- Boundary gate: `SceneRuntimeBoundaryTests` links and loads without editor, ImGui,
  dialogs, SQLite, glTF, or source JSON libraries.
- Measurements: cold source compile, DDC lookup, artifact read/validate, CPU-ready,
  entity/component construction, peak memory, allocations, and bytes/entity for
  1k/10k/100k fixtures.
- Fallback: source scene and sidecar rebuild the artifact; DDC deletion is safe.
  Runtime reports failure and retains its prior world rather than parsing source.
- Completion: two clean outputs are byte-identical and a production-style runtime
  target loads only the validated cooked representation.

### M4.5 - Runtime component cleanup and editor drawer/property registries

- Affected systems: component headers, pool interface, reflection/property metadata,
  Inspector, component add/remove menus, CMake targets, editor services.
- Work: remove all `OnInspector`/`DrawInspector`, ImGui, Reflection, native-dialog,
  and editor-service dependencies from runtime components/pools; add generic/custom
  drawers keyed by stable ID; route Inspector enumeration and Add Component through
  descriptors; preserve resizable bodies and collection chrome.
- Tests: runtime header/link boundary; exact drawer-to-component registration;
  generic property validation/default/null behavior; custom Transform/Mesh/Light/
  Relationship presentation models; no RTTI name dependency; multi-selection policy;
  component addition without serializer or Inspector switch edits.
- Verification: interactive Inspector, Mesh thumbnails/material grid/list/zoom,
  asset pickers, component add/remove, compact layouts, and no UI regression under
  Vulkan validation.
- Measurements: closed/open Inspector construction median/p95/p99 and allocations
  versus M4.0/M3 0.0537 ms editor baseline.
- Fallback: metadata-driven generic drawer is the fallback for a missing custom
  drawer. Runtime never falls back to component-owned UI.
- Completion: runtime scene/components compile independently and all component UI is
  editor-registered by stable ID.

### M4.6 - Transaction service, property undo/redo, and asset assignment

- Affected systems: transaction/history service, editor document state, property
  drawers, gizmo, Mesh asset/material controls, Edit menu/shortcuts.
- Work: implement atomic property commands, history state tokens, coalescing,
  undo/redo, multi-edit, Transform/gizmo changes, enabled/light/name edits, model
  assignment, material override add/change/reset, and component add/remove.
- Tests: exact before/after/undo/redo, no-op/failure, validation rollback,
  continuous-widget and gizmo one-command coalescing, text entry, branch-after-undo,
  savepoint dirty behavior, multi-selection atomicity, asset GUID history across
  pending/failure/publication/eviction, component add/remove with unknown properties.
- Interactive verification: shortcuts/menu enablement and error presentation;
  pending model/material edits save immediately and later residence does not alter
  history. Vulkan validation covers undo/redo during asset publication.
- Measurements: transaction apply/undo/redo p95/p99 for 1/100/10k targets, history
  memory per command, editor-frame allocations, and gizmo steady cost.
- Fallback: a failed command restores all targets and produces no entry. History may
  be cleared only on a successful scene-open commit, never on failed open.
- Completion: every property/asset/component edit in scope is undoable and dirty
  state follows committed history rather than widget activity.

### M4.7 - Structural editing, hierarchy reconstruction, and viewport extent

- Affected systems: editor scene commands, Hierarchy, clipboard, duplicate/delete/
  reparent/reorder, Relationship runtime derivation, Viewport service, Application/
  RHI offscreen extent integration.
- Work: route create/delete/duplicate/copy/paste/rename/reparent/sibling order through
  atomic commands; reconstruct hierarchy from parent/order; block cycles; preserve
  unknown payload snapshots; make panel extent drive actual scene render targets and
  camera aspect with debounce/fence-safe replacement.
- Tests: subtree operations and exact undo/redo; deterministic UUID allocation/remap;
  internal/external/nullable references; opaque-reference warning; cycle and invalid
  reparent rollback; root/child sibling order; selection after structural changes;
  viewport wide/tall/DPI/minimize/rapid resize, target extent, projection aspect,
  picking/gizmo rectangle, and no stretch.
- Captures: deterministic hierarchy/model placement scene before/after save/load;
  resized scene-linear/final screenshots with geometry aspect oracle; repeated 4K and
  1600x900 resize with validation, pending assets, and normal close.
- Measurements: hierarchy snapshot/traversal and structural command costs at scale;
  render-target rebuild CPU/GPU/memory/stall counts; steady no-resize overhead zero.
- Fallback: last completed target is aspect-fitted while replacement is pending; a
  failed resize keeps the prior target/aspect and reports the error.
- Completion: structural workflows are exact/undoable and the rendered viewport,
  projection, picking, and displayed rectangle agree after panel resize.

### M4.8 - Evidence-driven ECS/query tightening

- Preconditions: M4.0-M4.7 measurements available; no assumed storage change.
- Affected systems: only the measured ECS lookup/view/data paths selected by evidence,
  plus benchmark/tests. Scene schemas and editor contracts are frozen.
- Work: choose no change, sparse-vector/paged sparse lookup, cached explicit views,
  reserve/layout tightening, or another narrowly demonstrated improvement. Record the
  decision and raw comparable evidence. Do not add archetypes.
- Tests: randomized property tests for add/get/remove/destroy/reuse/views, ordering
  independence, stale handles, allocation bounds, and all scene/editor workflows.
- Performance gate: selected change meets the 15%/5% policy above or a documented
  correctness need. Repeat every baseline workload and M3 representative frame gate;
  report CPU memory and allocations.
- Fallback: retain the existing dense plus sparse-map implementation if candidates
  fail. A no-change result completes this slice when evidence is documented.
- Completion: current storage is either retained with justification or narrowly
  improved without schema/runtime/editor regression and while keeping M7-compatible
  identity/change signals.

### M4.9 - Reusable asset documents and isolated viewers (optional, non-core)

- Preconditions: core M4.0-M4.8 gates are green and interfaces stable.
- Affected systems: editor-only asset document/viewer registry, viewport layout,
  orbit camera, model/material presentation, pins/lifecycle.
- Work: double-click/open model/material into a dedicated viewer independent of scene
  placement; orbit/pan/zoom/frame bounds; material/debug presentation; reuse drawer,
  property, transaction, asset, and viewport services.
- Tests: type registration/open/close/reopen, no active-scene entity/dirty/history
  mutation, camera framing, resize/aspect, asset failure/reload, pins/retirement.
- Captures/measurements: representative model and material viewer images, validation,
  open/idle editor CPU/GPU/memory and resource cleanup.
- Fallback: Browser thumbnails/details remain available. Deferral of this optional
  slice must be explicit in the completion report and does not reopen a passed core
  scene gate.
- Completion: viewers are isolated editor documents and cannot place or mutate scene
  data implicitly.

### M4.10 - Production cutover and criterion-level acceptance

- Preconditions: M4.0-M4.8 accepted; optional M4.9 complete or explicitly deferred.
- Affected systems: remove old serializer/UI hooks/RTTI paths and temporary adapters;
  finalize fixtures, documentation, roadmap/context, and acceptance report.
- Work: delete the central serializer as a production path while retaining its v0
  behavior only as pure migration code; remove transient-ID serialization, direct
  editor mutations, component UI hooks, and obsolete filters/extensions. Freeze
  source/cooked schema hashes and last-known-good artifacts.
- Automated gate: Debug and Release builds and all CTests; deterministic source
  round-trip; top/component migrations; unknown preservation; duplicate/invalid UUID;
  broken refs; atomic save/recovery; GUID/model/material override; hierarchy/order;
  pending/residency; undo/redo; runtime-only link; cooked determinism/load; ECS scale;
  viewport extent/aspect.
- Visual/runtime gate: all relevant M0-M3 cooked fixtures, sample car, scene/editor
  workflows, SDR/scRGB/HDR10 where ownership changed, 4K and resized validation,
  selection/debug/wireframe behavior, and normal cleanup.
- Performance/memory gate: repeat M3 five-run car and allocation contract; report
  scene load/save/cook, ECS/editor construction, runtime tick, CPU/GPU frame,
  requested/committed memory, and source/cooked artifact bytes. No unexplained
  regression or normal-frame wait.
- Fallback: last accepted source/cooked hashes and source-control reversal. Runtime
  never falls back to source parsing or old path identity.
- Completion: write `docs/milestones/M4-acceptance-report-<date>.md`, update this
  completion report, `ROADMAP.md`, `docs/PROJECT_CONTEXT.md`, performance baselines,
  and ADR status. An independent reviewer passes every acceptance criterion.

## Old-path removal matrix

| Transitional path | Retained through | Removal condition |
|---|---|---|
| Current `SceneSerializer` writer | M4.2 | New editor writer plus deterministic/unknown-data tests pass in M4.3 |
| Current `SceneSerializer` reader | M4.3 as comparator | Its supported behavior is represented by pure v0 migration fixtures; file/class removed in M4.10 |
| Transient `EntityID` source field | v0 migrator only | UUID derivation/mapping and all hierarchy fixtures pass |
| Serialized `children` and `depth` | v0 validation only | parent/order reconstruction and cycle/inconsistency tests pass |
| Component `OnInspector`/pool `DrawInspector` | M4.4 | Runtime/editor build split and drawer registry pass M4.5 |
| Inspector RTTI/string special cases | M4.4 | stable-ID generic/custom drawers enumerate all components |
| Direct editor property mutations | M4.5 | property/asset/component transaction coverage passes M4.6 |
| Direct structural mutations | M4.6 | structural transaction/UUID/hierarchy coverage passes M4.7 |
| Swapchain-sized scene viewport assumption | M4.6 | active offscreen extent/aspect/resize validation passes M4.7 |
| Any pre-M3 path field | never a compatibility path | remains a named rejection fixture permanently |

## Delegation and integration

The default is one lead working sequentially. No subagent is authorized during this
initial audit/plan. After approval and after interfaces for a slice are frozen,
bounded disjoint work may be delegated only for items such as fixture/property tests,
read-only benchmark analysis, binary corruption vectors, or viewer-only UI.

One owner at a time controls Entity/Registry public headers, persistent UUID types,
component registry and property descriptors, source/cooked schemas, central CMake
targets, document transactions, and production cutover. Do not overlap write-heavy
work in those areas. The lead inspects source, builds both configurations, reruns
evidence, and integrates every result before advancing a slice.

## Verification and acceptance evidence

Every implementation slice runs the prescribed Debug and Release configure/build/
CTest commands. Performance decisions use Release on the reference i9-14900K/RTX
4090 system. Scene/ECS microbenchmarks report fixed workload/seed and at least 30
samples; renderer decisions use 3840x2160, fixed camera/content/output, 500 warm-up
and 10,000 measured frames over five independent runs unless a fixture defines a
different contract.

M4 acceptance evidence includes at minimum:

- deterministic source scene canonicalization and repeated round trips;
- top-level v0-to-v1 and every component migration fixture;
- unknown component/property/top/entity extension preservation;
- duplicate, nil, invalid, missing, stale, and broken-reference tests;
- atomic save, backup, temp, corrupt-primary, and failed-load retention tests;
- GUID asset/subasset/material override and move/rename tests;
- pending, missing, failed, nonresident, later-resident, reimport, and eviction tests;
- hierarchy cycle/parent/sibling order plus duplication/copy/paste remap tests;
- property/component/entity/rename/reparent/reorder/asset/multi-edit undo/redo;
- runtime-only component/cooked-scene build with no editor dependencies;
- byte-identical cooked artifacts and source-free runtime loads;
- before/after ECS lookup, iteration, view, creation, deletion, hierarchy,
  serialization, editor construction, allocation, and memory results;
- viewport render-extent, camera-aspect, picking, resize capture, and Vulkan
  validation evidence;
- full M3 39/39 baseline preservation (or higher current total), representative
  cooked fixtures, sample-car material/coverage contracts, output transport behavior,
  frame/memory/allocation measurements, and no path/source-runtime regression.

## Risks, fallback, and rollback

- **UUID migration accidentally depends on a path:** namespace v0 migration with the
  scene sidecar asset GUID and legacy ID only. Moving source plus sidecar cannot
  change entity UUIDs.
- **Generational handles cause broad churn or regression:** land them alone, keep
  component storage unchanged, and measure every baseline. Correctness is required;
  unrelated query redesign is not bundled.
- **Unknown data appears preserved but is reordered/lost:** retain a semantic DOM,
  duplicate-key reject, canonicalize once, and assert values/types/envelope fields
  across repeated round trips. Do not promise comments/whitespace preservation.
- **Future version recovery corrupts newer data:** normal open/save rejects supported
  future versions. Explicit recovery is read-only and cannot overwrite/cook.
- **M3 v0 path data is silently accepted:** maintain permanent rejection fixtures for
  every known path field. Never consult the catalog to guess a GUID from a path.
- **Atomic save is only nominally atomic:** use same-directory temp files, flush and
  reparse, platform atomic replace, fault injection at every step, and preserve a
  verified backup.
- **Staging doubles large-scene memory:** measure peak and allow immutable parsed
  buffers/arena ownership transfer, but never trade away rollback safety. Cooked
  loading may reserve exact validated counts.
- **Undo snapshots grow without bound:** structural snapshots are canonical and
  measured; expose a budget and checkpoint policy only after behavior is correct.
  Never drop the sole ability to undo a partially applied operation because partial
  operations are forbidden.
- **Asset runtime state leaks into authoring history:** transaction values contain
  GUIDs only; publication callbacks update transient resolution state outside the
  document service.
- **Runtime/editor split is cosmetic:** enforce separate link targets and a runtime
  load test, not only include cleanup.
- **Cooked format mirrors C++ memory:** explicit little-endian encoders, type streams,
  offsets, versions, checksums, and corruption tests prohibit raw dumps.
- **ECS benchmark favors a synthetic case:** include mixed occupancy, editor,
  hierarchy, serialization, and changed-data workloads; a small proxy cannot justify
  an archetype rewrite.
- **Panel resize causes allocation/stall churn:** debounce interactive changes,
  rebuild fence-safely, retain last image, and report rebuild/stall/memory counters.
- **Viewer expands into renderer work:** stage last, reuse cooked assets and existing
  debug presentation, and permit explicit deferral without weakening core M4.

## Decisions recommended for approval

1. Use strongly typed scene UUIDs: UUIDv7 for new entities and UUIDv5 only for
   deterministic namespace-derived migration/future instancing. Keep them distinct
   from M3 `AssetGuid` and runtime handles.
2. Replace `Entity = uint32_t` with a 32-bit index plus 32-bit generation handle and
   retain dense component pools initially.
3. Use permanent lowercase namespaced string component IDs and explicit cooked
   four-byte section IDs; do not serialize RTTI or hash values as authority.
4. Adopt source schema 1 and `.iridium.scene.json` exactly as specified, with scene
   asset identity in a normal M3 sidecar.
5. Preserve unknown source payloads semantically in canonical JSON, reject unknowns
   from strict cook, and make future-version recovery read-only.
6. Compose registries through explicit ordinary module functions and freeze/test
   them; do not use static registrars or linker discovery.
7. Treat the current unversioned M3 GUID scene as version 0, derive entity UUIDs from
   sidecar scene GUID plus legacy ID, and permanently reject all path-based shapes.
8. Make staging-world swap-on-success the only load commit model; missing/nonresident
   assets can remain pending, but broken required entity/hierarchy references fail.
9. Reuse M3 CookedArtifact/DDC infrastructure with `iridium.scene.runtime` schema 1
   and explicit per-component streams rather than inventing a second cache/container.
10. Use history state tokens for dirty state and UUID/property-ID transactions for
    undo/redo; asset residence is not authoring state.
11. Keep the current dense+sparse-map pools unless M4.0/M4.1 evidence passes the
    stated threshold for a targeted alternative. A no-storage-change result is valid.
12. Drive a separately resizable scene offscreen extent/camera aspect from the editor
    viewport, with aspect-fit fallback during fence-safe rebuild.

## Owner-delegated decisions

The owner delegated these choices to the milestone lead on 2026-07-31. They are
resolved as follows:

- Save As preserves entity UUIDs because it saves the same document state at a new
  scene asset path. The destination receives a new scene-asset sidecar GUID. A
  separate future Duplicate Scene Asset command performs a deep identity remap.
- A known component version newer than the editor understands is a hard failure for
  normal writable open. A separate read-only recovery path may preserve and inspect
  the document, but cannot overwrite or cook it.
- The optional M4.9 isolated viewer remains post-core and non-blocking. It may be
  included in the M4 completion report if ready; otherwise it is an explicit accepted
  deferral after the core M4.0-M4.8 gates pass.

These choices follow familiar editor document semantics, prioritize lossless project
data, and keep the core identity/schema milestone from depending on a presentation
feature.

## Decision log

- 2026-07-31: M0-M3 acceptance, all accepted ADRs, roadmap/planning/context, and the
  full current source/test inventory were read before design.
- 2026-07-31: Debug and Release both configure/build and pass 39/39 tests in the
  Visual Studio developer environment. No M4 implementation began.
- 2026-07-31: Current entities were confirmed as non-generational `uint32_t` values;
  the roadmap/prompt description of generational handles is a required M4 result,
  not existing behavior.
- 2026-07-31: Current deserialization clears the Registry before complete validation,
  so late failures destroy the open scene. Staging-world commit is mandatory.
- 2026-07-31: Current writer omits entities without Transform and uses mutable dense
  Transform order. Current hierarchy persists redundant transient IDs/children/depth.
- 2026-07-31: The tracked historical scene and ignored local Sponza predecessor can
  be silently reduced by the current loader because their field names are unknown.
  M4 will turn all known path shapes into explicit rejection fixtures.
- 2026-07-31: M3 GUID mesh intent, pending assignment precedence, names, sibling
  order, material subasset overrides, and later asset publication are frozen as
  migration/preservation contracts.
- 2026-07-31: Current viewport letterboxing avoids stretch but does not resize the
  actual scene render target or camera aspect when panels change. M4.7 owns the full
  correction.
- 2026-07-31: Owner approved the plan and delegated the three open product choices.
  Save As preserves entity UUIDs while assigning a new scene asset GUID; future
  known component versions require a separate read-only recovery flow; optional
  M4.9 viewer work does not block core M4 acceptance.
- 2026-07-31: M4.0 began. This slice is limited to tracked compatibility fixtures,
  expected-defect characterization, dependency inventory, and an unchanged-code
  Release baseline.
- 2026-07-31: M4.0 accepted. Debug/Release pass 40/40, all 34 Release benchmark
  cases contain 30 samples, the hidden cooked `material_lab_v1` validation startup
  passes, dense component arrays remain selected, and M4.1 becomes active.
- 2026-07-31: M4.1 accepted. Strong 64-bit generational handles, stale-safe
  Registry operations, UUIDv5/v7 scene identity, bidirectional binding, and
  application-owned `SceneWorld` pass 41/41 in Debug and Release. The same-compiler
  41-case benchmark retains dense pools, attributes identity cost, and records the
  remaining sparse/structural candidates for M4.8. M4.2 becomes active.
- 2026-07-31: M4.2 began with separate `IridiumSceneRuntime` and
  `IridiumSceneAuthoring` libraries. Stable component/property/section identities,
  deterministic runtime/source registries, one-version component migration chains,
  structured scene diagnostics, bounded strict JSON parsing, and nested duplicate-
  key rejection pass 44/44 in Debug and Release. Canonical schema-1 document and
  unknown-payload work remains active; production Save/Load is unchanged.
- 2026-07-31: M4.2 added the schema-1 semantic document and canonical writer.
  UUIDs and hierarchy are validated before hierarchy-preorder output; registered
  components/properties use explicit source order and source-field bindings;
  unknown components, properties, envelope members, and extensions survive
  canonical two-pass round trips. Scene UUID generation no longer borrows the
  asset/JSON module. The pure v0 envelope migrator derives UUIDv5 identities from
  scene asset identity, migrates M3 GUID/name/transform/relationship/mesh/override/
  light data, preserves unknown legacy entity fields, validates redundant hierarchy
  data, and rejects every tracked path-identity predecessor. Debug and Release pass
  46/46. Exact production core descriptors and scale measurements remain before
  M4.2 acceptance; editor Save/Load still uses the accepted fallback.
- 2026-07-31: The exact five-component manifest is now explicit and
  test-enumerable: stable type/property IDs, `NAM1`/`TRN1`/`REL1`/`MSH1`/`LGT1`,
  source/cooked version 1, source/property order, nullability/reference kinds, and
  camelCase source-field bindings. Runtime and source callbacks are injected into
  the composition roots, so current editor-coupled component headers do not leak
  into `IridiumSceneRuntime`; missing callbacks fail composition. Debug and Release
  pass 47/47. M4.2 scale measurements and their report remain.
- 2026-07-31: M4.2 accepted. Property metadata now includes validated defaults and
  deterministic collection policy; all core fields, null/default behavior,
  material-override sorting, future envelopes, duplicate components, missing
  parents, cycles, and legacy contradictions have direct coverage. Release scale
  evidence measures strict parse, canonical serialize, and v0 migration at
  1k/10k/100k with allocation and live-memory sampling. Canonical 100k serialize is
  481.971 ms p95; strict parse is a documented M4.3 risk at 8,610.990 ms p95.
  Debug/Release pass 47/47, the runtime boundary scan is clean, the production
  fallback remains unchanged, and M4.3 becomes active.
- 2026-07-31: M4.3 began with an address-stable staging commit. `SceneWorld` swaps
  Registry storage, persistent identities, and stable-reference state only after
  every staging phase succeeds, while keeping the application-held `Registry&`
  valid. The loader creates the complete UUID map before component-local decode,
  records entity/asset/subasset references from property metadata, distinguishes
  required failures from optional unresolved references, leaves asset references
  pending, invokes registered fixup/post-load callbacks, and retains opaque source
  components outside runtime storage. Focused failure tests prove late validation
  cannot mutate the active world.
- 2026-07-31: M4.3 added generic live-world source capture and the verified atomic
  file primitive. Capture enumerates frozen codecs rather than a component switch,
  writes persistent UUIDs rather than ECS handles, merges unknown top/entity/
  component/property payload by UUID and stable component ID, and removes known
  components that no longer exist. Atomic save uses a unique same-directory
  create-new temporary, write-through flush, reopen plus SHA-256 and semantic
  verification, `ReplaceFileW` replacement, and a single `.bak` last-known-good
  sibling. Fault injection at create/write/flush/verify/replace proves the existing
  destination and backup remain byte-exact before adoption. Debug and Release pass
  50/50 and the JSON/editor-free runtime boundary scan remains clean. Production
  core component adapters, editor document/state-token ownership, recovery UI, and
  the menu cutover remain active M4.3 work; the old serializer is still the editor
  fallback/comparator.
- 2026-08-01: M4.3 added the production adapters for Name, Transform,
  Relationship, Mesh, and Light. The exact frozen registries now drive local
  decode, hierarchy fixup, post-load validation, live source capture, GUID-only
  model/material overrides, default handling, and permanent removed-path rejection.
  Core adapter round trips retain opaque/unknown data and pending stable asset
  references without serializing runtime handles.
- 2026-08-01: `EditorSceneDocumentService` now owns current path, scene-asset
  sidecar GUID, semantic document, diagnostics, staged Open, Save/Save As, explicit
  backup recovery, and history-compatible state tokens. Canonical sources require
  `.iridium.scene.json` plus an `iridium.scene` metadata sidecar. Logical-v0 input
  migrates using that sidecar identity, stays dirty, and requires Save As; entity
  UUIDs remain stable while the new destination receives a new scene-asset UUIDv7.
  A failed first Save As removes its newly created sidecar, and all failed opens or
  saves retain world/path/token state.
- 2026-08-01: The editor File menu no longer calls or constructs
  `SceneSerializer`. It discovers only canonical scene sources, uses the document
  service for Save/Open, exposes explicit `.bak` recovery, and restores selection
  by persistent entity UUID after a successful world swap. The legacy serializer
  remains compiled only for characterization/comparison until M4.10 removal.
  Debug and Release pass 52/52; the runtime boundary scan and production call-site
  scan are clean. Crash-left temporaries can now be enumerated read-only for one
  exact destination with deterministic path order, byte size, write time, and
  SHA-256; tests prove they are never auto-promoted. Non-blocking recovery
  presentation, asset-residency workflow coverage, scale timings, and interactive/
  Vulkan validation remain before M4.3 acceptance.
- 2026-08-01: Crash-left scene temporaries are now discovered by an asynchronous,
  destination-specific read-only scan and presented with path, byte size, and
  SHA-256. Recovery requires an explicit candidate action, re-enumerates the exact
  valid set before staging, never promotes or deletes a temporary automatically,
  adopts the primary document path as dirty, and restores selection by persistent
  UUID only after a successful swap.
- 2026-08-01: Asset-residency workflow coverage proves pending, failed/nonresident,
  and later-resident model/material state never changes the document state token or
  serialized GUID intent. The save remains clean and omits transient resolution
  diagnostics while preserving all requested model and material-override GUIDs.
- 2026-08-01: Release lifecycle evidence now covers strict parse, v0 migration,
  staging, live capture, address-stable commit, and verified atomic save at
  1k/10k/100k. The 100k commit is 0.0008 ms median with zero allocations; stage is
  367.181 ms median and verified atomic save is 20.019 s median. Inline duplicate-
  key tracking reduces strict-parse allocation calls 17.6% and requested traffic
  22.0% versus M4.2, but 100k source JSON latency remains a documented risk owned by
  the later cooked path rather than weakened validation.
- 2026-08-01: Debug and Release both build and pass 52/52 after the recovery,
  residency, parser, and benchmark additions. Runtime/editor and production legacy-
  serializer call-site scans are clean. A hidden Debug material-lab run at 4K with
  the Khronos validation layer and accepted cooked artifact completes without a
  VUID or validation message; source import remains zero. M4.3 remains active only
  for the visible Open/Save/Save As/backup/temp recovery walkthrough and comparable
  multi-sample editor construction/frame evidence.
- 2026-08-01: M4.3 accepted. Visible File-menu operations now call a dedicated,
  testable editor document-command layer for canonical Save As paths, Open, backup/
  temporary recovery, failure retention, and UUID selection remapping. A 1,000-frame
  Release 4K validation run drops zero frames and reports editor-build median/p99 of
  0.0699/0.3241 ms with steady allocation median zero. Five Release launches measure
  editor initialization at 0.0211 ms median versus M3's 0.0537 ms. The unchanged
  100k inspector construction case is 84.992 ns/op median with zero allocations.
  Debug/Release remain 52/52, dependency/call-site scans are clean, no ADR changed,
  and M4.4 deterministic cooked runtime scenes becomes active.
- 2026-08-02: M4.4 accepted. `iridium.scene.runtime` schema 1 now has a
  deterministic little-endian sectioned representation, registry-manifest and
  CookKey validation, exact dependency policy, source-free staged runtime load,
  allocation-free active-world commit, dedicated headless M3 DDC/receipt cooking,
  and a source-free inspector. Cross-process cooks are byte-identical; cold/warm
  CLI coverage proves a receipt/DDC hit skips JSON parse and scene compile; corrupt
  artifacts and every staged-load failure retain the prior active world. The
  production boundary links only scene-runtime and cooked-artifact libraries.
  Debug and Release pass 56/56. At 100k, the 5.34 MiB artifact stages in 148.105 ms
  median but has a visible 254.994 ms p95 and 800,271 allocations; commit is 0.0007
  ms median with zero allocations. This one-shot staging tail remains follow-up
  work, while M4.5 becomes active to remove component-owned UI from runtime types.
- 2026-08-02: M4.5 began with the runtime-side cut. `IComponentPool` no longer has
  a drawing virtual, no component is required to implement `OnInspector`, and Name,
  Transform, Relationship, Mesh, Light, and the aggregate component header contain
  no ImGui or editor reflection dependency. Existing Transform, Mesh, Light, and
  Relationship presentation was retained in `InspectorPanel`. The headless scene
  cooker and core adapter/document tests no longer compile or link ImGui merely to
  instantiate runtime components. Debug and Release build and pass 56/56. The
  Inspector still enumerates RTTI pool names and contains core presentation/add
  branches; the next M4.5 cut replaces those with frozen stable-ID editor drawer
  and property registrations before this slice can be accepted.
- 2026-08-02: M4.5 added a frozen `EditorComponentRegistry` and a core editor
  registration factory keyed by the same shared stable component IDs as the runtime
  registry. Inspector enumeration, display order, visibility, body size, required
  policy, typed pool access, removal, and Add Component entries now come from those
  descriptors; it no longer reads RTTI names or contains type-specific add/remove
  branches. Name, Transform, and Relationship are protected core state, while Mesh
  and Light are descriptor-addable/removable. Tests cover invalid/duplicate/frozen
  registration, typed add/get/remove bindings, deterministic display order, policy
  validation, and exact editor/runtime core ID coverage. Debug and Release pass
  57/57. The actual Transform/Mesh/Light/Relationship drawing bodies remain in the
  Inspector and generic property metadata/drawers remain active M4.5 work.
- 2026-08-02: M4.5 extended frozen editor component descriptors with ordered,
  stable-ID typed property bindings. Registration rejects invalid/duplicate IDs or
  order, missing and size-incompatible bindings, non-reference nullability,
  type-mismatched defaults, invalid numeric ranges, and enum-label misuse. Core
  Name, Transform, Relationship, Mesh, and Light editor properties exactly cover
  the corresponding runtime registry IDs, types, and nullable policy. A generic
  editor drawer now handles Boolean, Int32, Float32, String, Float32x3, and labeled
  Enum values; it presents stable references safely and requires a custom drawer
  for reference assignment and collections. Debug and Release remain 57/57. Core
  Transform/Mesh/Light/Relationship custom drawing is still located in
  `InspectorPanel`; moving those bodies behind stable-ID drawer callbacks is the
  next cut.
- 2026-08-02: M4.5 added a frozen `EditorComponentDrawerRegistry` keyed by stable
  component ID. Registration rejects invalid IDs, missing callbacks, duplicates,
  unknown components, hidden components, and use before the component registry is
  frozen. Inspector dispatch for Transform, Light, and Relationship now resolves
  through registered callbacks; an absent callback continues through the generic
  property drawer. Focused tests exercise registration, freeze policy, lookup, and
  callback invocation. Debug and Release build and pass 57/57. The asset-aware,
  collection-heavy Mesh body remains the final inline core custom drawer to move.
- 2026-08-02: M4.5 moved the asset-aware Mesh editor behind the same stable-ID
  callback path without changing model selection, thumbnails, material overrides,
  filtering, layout, or paging. Production registration is centralized and tests
  prove the exact Transform, Relationship, Mesh, and Light custom set; Name has no
  custom registration. The Inspector presentation loop now has no RTTI-name or
  component-ID switch. The Release benchmark no longer links ImGui or measures RTTI
  traversal: registry construction is 0.0057 ms median / 0.0116 ms p99 with 46
  one-time allocations, while descriptor/access/drawer dispatch is allocation-free
  at 44.735 ns/op for 10k and 168.911 ns/op for 100k. Raw and interpreted evidence
  is under `docs/performance/`. A visible benchmark-output Release run completed
  300 measured 4K frames with validation enabled and zero drops, while a separate
  normal-editor pass verified maximized and 1280x720 compact Inspector layouts,
  Mesh thumbnail/material paging, grid/list/zoom, model and material picker
  open/cancel, and descriptor-driven Light add/custom presentation/remove. The
  original component set was restored. A final 300-frame normal-editor validation
  profile had zero drops or validation messages, 0.1021 ms median editor build,
  and 0.0174 ms median GPU UI. M4.5 is accepted; M4.6 transaction work is active.
- 2026-08-02: M4.6 established the editor-owned transaction/history service with
  atomic multi-operation apply and rollback, exact undo/redo rollback, unique
  document state tokens, savepoint-aware dirty state, redo-branch invalidation,
  continuous-edit coalescing, logical history-memory accounting, and exception-safe
  diagnostics. Successful scene-open commits clear history through the document
  lifecycle observer; failed opens retain it, and out-of-history mutations invalidate
  unsafe entries. Edit menu labels, enablement, Ctrl+Z, Ctrl+Y, and Ctrl+Shift+Z are
  functional. Entity name, Transform and Light Inspector edits, component add/remove
  snapshots, and viewport gizmo release commits now use transactions; Transform and
  Light continuous controls coalesce by widget activation and each gizmo gesture is
  one command. Focused tests cover exact apply/undo/redo, no-op behavior, atomic
  failure rollback, failed-undo recovery, coalescing, branch-after-undo, savepoint
  dirtiness, open lifecycle, external-state invalidation, and typed component snapshot
  restore. A headless Mesh authoring adapter now limits commands to enabled state,
  effective requested model GUID, and material-override GUID pairs. Picker and
  drag/drop model assignment, material add/change/reset, and Mesh enabled edits use
  it. Tests prove pending assignments save immediately, publication failure does not
  dirty the scene, publication/eviction do not change history, undo/redo survives
  those runtime transitions, and undoing an initial assignment clears the published
  asset. The generic metadata fallback now captures and validates Boolean, Int32,
  Float32, String, Float32x3, and enum values through stable component/property
  bindings; text and continuous controls coalesce by widget activation. Its reusable
  multi-target construction is atomic, with a test proving one invalid target restores
  every earlier target and produces no state. Release measurements over 30 samples
  show 10,000-target apply at 0.112 ms median / 0.128 ms p95 / 0.163 ms p99,
  allocation-free undo/redo below 0.068 ms p99, one 120-byte apply allocation, and
  247 bytes retained per target. In-place operation compaction removed a measured
  1.68 MB temporary copy and improved 10,000-target median apply by 4.2x. Hierarchy
  Ctrl-click multi-selection now maintains one primary entity for viewport/legacy
  consumers and atomically fans changed Transform, Light, Mesh, generic-property,
  add-component, and remove-component fields across applicable selected entities.
  Stale selections are reconciled after destruction. A focused source-scene test
  removes and undoes a Light component, edits it, saves, and proves the component's
  unknown `studioHint` property survives the transaction round trip.
- 2026-08-02: Final M4.6 interaction and performance gates passed. Live validation
  exposed that focused-window shortcut routing was unreliable because commands are
  registered before the menu window begins; Save/Open/Undo/Redo shortcuts now use
  ImGui's global routed policy, which still yields to higher-priority active text
  controls. Ctrl+Z, Ctrl+Y, and Ctrl+Shift+Z were then exercised successfully.
  A two-entity Transform edit applied, undid, and redid exactly on both targets;
  menu labels/enablement matched history. Deleting one target outside history forced
  an atomic undo failure, restored the earlier target, retained the command, and
  displayed `Undo failed: entity/transform is no longer available` in the new
  command-failure modal. Three long-running interactive validation profiles reported
  zero frame drops and zero profiler overflow. A like-for-like 1280x720 Release run
  measured 0.1032 ms median editor build versus M4.5's 0.1021 ms, with unchanged
  126-call/14,890-byte median whole-editor allocations and unchanged 0.0174 ms
  median GPU UI. A dedicated 300-frame 4K Release range measured selected-entity
  gizmo processing at 4.1 / 5.9 / 6.8 us median/p95/p99, with zero drops, profiler
  overflow, or validation warning/error output. Debug and Release build and pass
  58/58. M4.6 is accepted; M4.7 is active. No ADR changed.
- 2026-08-02: M4.7 structural editing now uses stable UUID/component snapshots
  for create, subtree delete, duplicate, copy/paste, sibling reorder, and
  reparent. Runtime `children`/`depth` are reconstructed from authoritative
  parent/order, invalid cycles roll back, selection is restored by UUID, and
  opaque source payload survives delete-save-undo-save. Duplicate/paste remap
  known internal entity references, retain external/nullable references, and
  report that opaque references cannot be remapped without their schema. Menu,
  Hierarchy, Inspector, and viewport model drops use the command boundary; the
  hierarchy renders parent-before-child preorder. Full Debug and Release builds
  and 58/58 tests pass. The viewport side now also has a deterministic DPI-aware
  pixel-extent/debounce policy covering wide, tall, minimized, rapid-resize, clamp,
  retry, and zero-steady-request behavior. M4.7 remains active for wiring that
  policy to scene render-target replacement, resize evidence, and scale
  measurements. No ADR changed.
- 2026-08-02: M4.7 completed the panel-driven viewport path. The RHI now replaces
  scene/offscreen extent independently of the presentation swapchain; Vulkan uses
  candidate graph compilation, in-flight frame-fence cutover, last-valid rollback,
  scene-sized GBuffer/depth/color/output resources, and presentation-sized SDR/HDR10
  UI targets. The panel requests DPI-scaled pixel dimensions after a 100 ms debounce,
  continues to aspect-fit the prior target while pending, and projection, picking,
  gizmos, and display use one committed extent/fitted rectangle. A visible Release
  validation session repeatedly changed the real window between 1600x900 and 4K with
  the cooked reference car resident, completed 825 frames with zero drops, and closed
  normally; retained replacement stalls were 20.805 and 30.911 ms CPU and peak graph
  memory during the 4K phase was 758.62 MiB. Final 1600x900 and scene-linear 4K
  captures passed visual aspect review. Scale measurement exposed quadratic bulk
  duplicate naming; a per-base suffix cursor reduced 10k duplicate median from
  10.524 s to 71.116 ms and temporary allocation from 1.56 GiB to 37.84 MiB. The
  accepted 30-sample matrix records 100k breadth rebuild at 15.852 ms median and
  100k depth collection at 36.364 ms; hierarchy traversal is now iterative and a
  20k-depth test guards against stack overflow. Final visible validation committed
  exactly one target replacement, then ran 400 frames with zero drops. Debug and
  Release build and pass 58/58; focused graph extent, policy, deep hierarchy, atomic
  failure, UUID remap, and bulk naming tests pass. Evidence is in
  `docs/performance/M4.7-editor-structure-viewport-2026-08-02.md`. M4.7 is accepted;
  M4.8 is next. No ADR changed.
- 2026-08-02: M4.8 replaced each component pool's sparse hash map with a demand-
  allocated 1,024-slot paged 32-bit sparse index while retaining contiguous dense
  component/entity arrays, the generational handle authority, current views, and
  every scene/editor contract. Empty pages are released and add rollback is
  exception-safe. A fixed-seed, 30-sample before/after matrix shows random hit/miss
  p95 improving 72.6/98.6%, Transform+Mesh and Transform+Relationship views
  improving 57.6/75.0%, 100k hierarchy traversal improving 90.8-91.8%, and dense
  Transform iteration improving 1.0%. One-million mixed creation improves 66.1%
  and requests 203.4 MiB less temporary allocation. Five isolated identity reruns
  disproved two non-causal full-suite regressions: their median p95 instead improves
  28.2% for 10k create/bind and 7.4% for reverse lookup. A deterministic 100k-
  operation reference-model property test covers add/get/update/remove/destroy/
  reuse, stale handles, erased lookup, dense-order independence, views, sparse page
  demand, and empty-page release. Full Debug and Release builds pass 59/59. Five
  repeated M3 4K sample-car runs preserve CPU, exact VRAM, and zero-allocation
  gates; a final 300-frame 4K Vulkan-validation run has zero warnings/errors, drops,
  or profiler overflow. Evidence is in
  `docs/performance/M4.8-paged-sparse-index-2026-08-02.md`. M4.8 is accepted;
  optional M4.9 is next. No ADR changed.
- 2026-08-03: M4.9 added a frozen stable-type asset viewer registry, GUID-keyed
  reusable model/material document tabs, shared presentation-root pin reference
  counting, an isolated orbit camera, exact cooked-bounds framing, and one shared
  production 3D target for the active document. Asset Browser double-click/context
  opening never places an entity or mutates scene selection, dirty state, or
  history. Material documents apply their selected binding to the owner model;
  production debug views, DPI extent requests, asynchronous preparation,
  last-known-good/failure diagnostics, and close-time pin release are preserved.
  A deterministic GUID-open path produced visually distinct model/material
  scene-linear captures under Vulkan validation. Release model-viewer overhead is
  0.0317 ms median editor CPU, 0.3108 ms median whole-frame CPU, 0.0339 ms median
  GPU, 1.1 KiB requested and 2.6 KiB committed backend memory, and five backend
  allocations for the five-draw fixture. Debug and Release build and pass 60/60;
  evidence is in `docs/performance/M4.9-isolated-asset-viewers-2026-08-03.md`.
  M4.9 was accepted and M4.10 became next. No ADR changed.
- 2026-08-03: M4.10 deleted the central `SceneSerializer` production reader/writer
  and its expected-defect characterization target. Logical v0 survives only in the
  pure envelope migrator. Direct editor property/component/name/gizmo fallbacks and
  obsolete scene filters/extensions are removed; panels require transaction,
  selection, and command services. A tracked two-entity acceptance fixture freezes
  exact source, canonical, registry-manifest, CookKey, and cooked-artifact hashes,
  and source-free staging verifies the artifact. Debug and Release pass 60/60. The
  Release ECS/scene/editor matrix, five independent 4K 10,000-frame car runs, and a
  300-frame validation capture pass. VRAM is byte-identical to M3 and final SDR is
  pixel-identical to M3.7. Evidence is in
  `docs/milestones/M4-acceptance-report-2026-08-03.md` and
  `docs/performance/M4.10-production-cutover-2026-08-03.md`. M4 is accepted; M5 is
  next. No ADR changed.

## Completion report

Complete and accepted on 2026-08-03. M4.0 through M4.10 are accepted. The central
serializer, transient/path identity, runtime UI coupling, and direct editor mutation
paths have been cut over; exact source/cooked contracts and the full criterion audit
are recorded in `docs/milestones/M4-acceptance-report-2026-08-03.md`. M5 is next.
No ADR changed.
