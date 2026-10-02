# M4 Acceptance Report - 2026-08-03

- Milestone: M4 - Scene schema, ECS identity, and editor separation
- Status: Accepted
- Reference system: Core i9-14900K, RTX 4090, 64 GB DDR5, Windows, Vulkan 1.4
- Acceptance build: MSVC 19.51.36252.0, Debug and Release
- Detailed plan: `docs/milestones/M4-scene-schema-ecs-editor-separation.md`
- Performance record: `docs/performance/M4.10-production-cutover-2026-08-03.md`

## Verdict

M4 is accepted. Scene entities now have persistent UUID identity distinct from
64-bit generational runtime handles; source schema 1 is deterministic, versioned,
unknown-data preserving, GUID-only for asset references, and saved atomically;
runtime scenes are deterministic source-free cooked artifacts. Runtime component
registration and scene loading do not link editor UI, source JSON, importers, native
dialogs, or SQLite. Editor property and structural changes are transaction-only and
undoable. Scene viewport sizing follows the panel target, and isolated model and
material viewers reuse production presentation without mutating scene state.

The legacy `SceneSerializer` implementation and characterization target are deleted.
Its v0 behavior survives only in the pure `SourceSceneEnvelopeMigrator`; no runtime
or editor production path can fall back to it.

## Frozen production contract

The tracked acceptance fixture is
`tests/scene/fixtures/m4_acceptance_schema1.iridium.scene.json` with sidecar GUID
`01890f4c-0000-7000-8000-000000000040`.

| Contract | Frozen value |
|---|---|
| source bytes SHA-256 | `8053f5b8b38190e89d2588df328b6a2e1afd31e22e37a289c95137cf55536944` |
| canonical scene SHA-256 | `497a4499ff5533d2595ae175582936b8d4aed80750a6a0e05133b0118789b24e` |
| runtime registry manifest SHA-256 | `ddcf859e4d422acda824e84c6bd44bfd8b9a404c3a8d81197d50f9ddcc0fa18a` |
| Release/high Windows x64 CookKey | `f27b18a39dd3efa7140caea45a74ee2f7bb55e00e0b2b8288c70625bd1bb58a6` |
| cooked artifact SHA-256 | `515e26d7e118e022d797b52496fae493ce33d304ce62400ddeb54d1a63a55f31` |
| cooked artifact size | 1,568 bytes; 2 entities |

`M4ProductionCutoverTests` regenerates and verifies every value, stages the cooked
artifact without source code or source JSON, and scans the production tree for the
removed serializer and direct editor fallbacks.

## Criterion-level acceptance

| Criterion | Result |
|---|---|
| Component addition avoids a central serializer | Pass: stable runtime/source registries and adapters own component behavior; legacy serializer files and CMake entries are absent. |
| Old scenes migrate without transient identity authority | Pass: v0-to-v1 migration derives UUIDv5 from scene sidecar GUID plus legacy ID, validates redundant hierarchy, and discards `EntityID`, `children`, and `depth`. |
| Unknown source data survives | Pass: top, entity, component, property, and extension payloads survive canonical repeated round trips. |
| UUID/reference failure handling | Pass: nil/invalid/duplicate/missing/stale UUID and broken-reference cases fail staging without mutating the active world. |
| Asset identity and overrides | Pass: model/material intent serializes stable GUIDs and subasset overrides only; move/rename and pending/failed/later-resident behavior do not dirty the document. |
| Atomic lifecycle | Pass: temp, flush, verify, replace, backup, corrupt-primary, orphan-temp, Save As, sidecar rollback, and failed-open retention are covered. |
| Hierarchy and structural editing | Pass: cycle rejection, parent/order reconstruction, iterative traversal, UUID remap, delete/duplicate/reparent/reorder, and undo/redo are covered. |
| Transaction-only editor mutation | Pass: property, mesh/asset, component, rename, multi-edit, gizmo, and structural persistence require the transaction/command services; preview state is restored on failure. |
| Runtime/editor separation | Pass: the runtime-only target links cooked runtime/artifact libraries and excludes ImGui, dialogs, importers, SQLite, and source JSON. |
| Deterministic cooked runtime | Pass: cross-process byte identity, strict envelope/section/manifest/target/CookKey validation, corruption cases, source-free stage, and allocation-free active commit. |
| ECS scale and identity | Pass: demand-paged sparse lookup, 100k-operation property testing, 30-sample workloads through one million entities, and stale-handle safety. |
| Viewport and viewers | Pass: panel render extent/aspect/picking/resizing, Vulkan-validation resize captures, GUID-keyed reusable viewers, bounds framing, pin lifetime, and scene/history isolation. |
| M0-M3 preservation | Pass: current 60-test matrix includes the full prior baseline; sample-car memory is byte-identical and final SDR is pixel-identical to M3.7. |

## Production cutover

- Deleted `src/scene/SceneSerializer.h/.cpp` and the expected-defect
  `SceneSerializerCharacterizationTests` target.
- Removed legacy v0 reader/writer cases from `IridiumEcsSceneBenchmark`; the pure
  migrator is the only v0 implementation.
- Replaced the scene asset-reference test with schema-1 read/stage/capture/write
  coverage and explicit rejection of transient/path fields.
- Removed component-owned inspector hooks, RTTI/property macros used as serializer
  authority, direct property/component/name/gizmo mutation fallbacks, the generic
  all-files scene filter, and the obsolete `.json` default extension.
- Panels now require selection, transaction, and scene-command services. A missing
  service cannot silently persist a change outside history.

## Verification

- Debug build: pass.
- Debug CTest: 60/60 pass.
- Release build: pass.
- Release CTest: 60/60 pass.
- Full Release ECS matrix: pass, 30 samples per case through one million entities.
- Source 10k, cooked 100k, transaction, and structural benchmarks: pass.
- Five Release 4K sample-car launches: 50,000 measured frames, zero drops,
  source-free cooked load, exact accepted live/peak requested and committed memory,
  and zero steady-frame allocation median/p99.
- Release 4K validation smoke: 300/300 frames, no Vulkan validation message, normal
  cleanup, and zero profiler overflow/drop.
- M3.7 final-SDR comparison: exact SHA-256 match, maximum delta 0, zero changed
  pixels, SSIM 1.0.

## Performance and memory decision

The current 100k cooked artifact remains 5,600,720 bytes and loads/stages faster
than M4.4: validation is 56.440 ms median, CPU-ready staging is 136.526 ms median /
203.157 ms p95, and active commit is 0.0007 ms with zero allocations. Five-run 4K
sample-car medians are 2.888 ms CPU and 1.566 ms GPU; GPU remains 6.4x below the
10 ms base-frame budget. Exact image, command/content contract, and VRAM preservation
rule out a scene-cutover fidelity or memory regression.

Strict 100k source JSON remains deliberately expensive. A new five-sample rerun hit
the five-minute process ceiling, consistent with the accepted M4.3 per-operation
results. The 10k confirmation is stable, runtime has no source parser, and this is a
documented editor/cook-host scalability risk rather than an acceptance blocker.

## Visual and output decision

The final 4K sample-car image is byte-identical to M3.7. M4 changes no BSDF,
lighting, transparency, output-transform, or transport ownership. Existing
selection, debug-view, wireframe, resized viewport, SDR, scRGB, and HDR10 evidence
therefore remains authoritative, supplemented by the new validation-clean exact
final-SDR comparison and M4.9 model/material viewer captures.

## Remaining risks and deferrals

- Streaming or more allocation-efficient construction may be warranted for very
  large cooked scenes; it must not weaken rollback-safe staging or validation.
- Streaming source parsing is optional future editor tooling. Runtime source parsing
  remains forbidden.
- Deep Duplicate Scene Asset identity remap and read-only future-schema recovery are
  separately planned product features, not missing M4 correctness paths.
- M5 owns clustered lights, shadows, probes, and baking; M7 owns persistent GPU scene
  and visibility-buffer work. Both consume M4 UUID/GUID/cooked contracts.

## Architecture and review status

No ADR is superseded or amended. M4 implements the accepted identities and
boundaries in ADR-0001 through ADR-0006. Criterion review was performed against the
frozen plan, enforced independently by the production-cutover executable plus the
60-test Debug/Release matrix, and audited by the milestone lead. No separate human
reviewer was available in this task; the complete evidence is retained for one
without requiring the implementation to remain open.
