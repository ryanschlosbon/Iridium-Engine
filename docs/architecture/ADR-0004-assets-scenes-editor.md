# ADR-0004: Stable Asset and Scene Identity with Editor Separation

- Status: Accepted direction; API details belong to M3 and M4
- Date: 2026-07-17
- Owners: Asset system, scene/ECS, and editor

## Context

The reviewed asset path uses filesystem paths as identity and combines importing, cooking, caching, and GPU upload. Scene serialization is centrally hand-written without robust versions or migrations. ECS components contain ImGui inspector behavior and file-selection responsibilities. These choices couple runtime data to tools and make deterministic reimport, headless cooking, stable references, undo, schema evolution, and unknown-data preservation difficult.

## Decision

### Assets

Source assets receive stable GUIDs recorded in source-controlled metadata sidecars. Metadata records importer type/version, import settings, source/dependency hashes, subasset identities, and other deterministic inputs. Derived runtime data is stored in a rebuildable cache keyed by those inputs.

Separate stages own:

1. source discovery and registry;
2. import and validation;
3. CPU cooking into runtime-oriented blobs;
4. scheduled GPU resource creation/upload;
5. residency and lifetime.

The asset browser is the primary workflow for import, reimport, thumbnails, search, drag/drop, and assignment. A menu-level import command may open a file browser. The scene hierarchy should create scene entities, not import source files.

### Scenes and components

Editor scene files use deterministic, human-inspectable JSON with a top-level schema version. Entities have persistent UUIDs distinct from runtime generational handles. Asset references use GUIDs plus subasset IDs where necessary.

Component serializers are registered by explicit stable component ID and carry per-component versions and migrations. Loading occurs in phases: create entities, deserialize components, resolve references, then run post-load validation. Unknown component payloads are preserved in editor round trips where safe. Runtime builds consume a cooked binary scene representation optimized for loading, not the source JSON layout.

### Editor separation

Runtime component types contain data and runtime behavior only. The editor owns a drawer registry keyed by component type ID. Drawers use editor services for asset selection, file dialogs, transactions/undo, validation, and multi-selection.

## Consequences

- Renames and moves do not break references when GUID metadata remains intact.
- Import and cook can run headlessly and asynchronously.
- Adding a component serializer does not require editing one central switch statement.
- Explicit registrations should be linker-safe and test-enumerable; static initialization order must not define correctness.
- Editor modules depend on runtime component definitions, not the reverse.
- JSON source scenes prioritize diffs and recovery; cooked binary scenes prioritize load performance and layout.

## Rejected alternatives

- Paths as permanent asset identity: simple initially, fragile under move/rename and ambiguous for subassets.
- Serialize ECS memory directly: fast to write but unstable across compiler, layout, version, and platform changes.
- Keep ImGui methods on components: convenient for small prototypes, but forces tool dependencies into runtime types and blocks modular drawers.
