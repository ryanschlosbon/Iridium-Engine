# M3 Acceptance Report - 2026-07-31

## Decision

M3 is accepted. Production asset identity is GUID-based, source-controlled sidecars
own deterministic import identity and settings, cooking produces validated
content-addressed artifacts, and runtime publication consumes cooked products rather
than source files. The editor Asset Browser is the project-owned import,
organization, reimport, thumbnail, drag/drop, and assignment workflow.

M3.7 removes the transitional source/runtime and per-material descriptor paths. The
Vulkan production renderer now requires indexed material, texture-view, and sampler
tables. Material storage grows at runtime within device limits instead of stopping at
4,096 records.

The owner's M3.6 interaction acceptance remains part of this decision: external
import, folders, rename/move/delete, Sponza placement and rendering, Browser
operations, scene persistence, and reload were repeated successfully before M3.7.

## Criterion review

| Acceptance criterion | Result and evidence |
|---|---|
| Stable identity | UUIDv7 asset and subasset GUIDs are stored in schema-1 `.iridium.meta` sidecars. Moves and renames retain identity when the source and sidecar move together. Runtime and new scene references use GUIDs, not paths. |
| Project ownership | `assets/` is the current physical project root. External imports transactionally copy the source and its relative glTF dependencies into a project package. Deleting the project asset does not delete the external original. |
| Importer/settings contract | A registered importer descriptor owns stable ID, implementation version, source extensions, settings schema, migration, validation, dependency discovery, and cook preparation. glTF uses importer version 3/settings schema 1; DirectXTex textures use version `20260508`/settings schema 1. |
| Deterministic cooking and DDC | Cook keys include source/dependency content, normalized settings, importer/cooker/target versions, and product schemas. Receipts validate cache hits. Two clean Sponza production cooks produced byte-identical artifacts and hashes. |
| Dependency and reimport | The dependency graph maintains reverse edges incrementally. Source monitoring schedules dependency-first reimport, coalesces revisions, publishes only the newest valid product, and retains last-known-good runtime state on failure. |
| Stage separation | Source discovery/parsing, prepared CPU cook, artifact serialization, runtime decode, scheduled GPU publication, and residency are separate contracts. Production startup and benchmarks require cooked model artifacts. |
| Texture semantics | Texture products and typed views retain image interpretation, color-space, swizzle/channel, UV, and sampler semantics. The indexed table has fallback records and fence-delayed index reuse. |
| Material indirection | Schema-2 `PackedGpuMaterial` records contain indexed texture views and samplers. The material table grows fence-safely to the device storage-buffer limit; the fixed 4,096-record limit and per-material descriptor allocation are removed. |
| Model/geometry products | Cooked model schema 3 preserves primitive identity, index ranges, bounds, material assignment, transparent bounds, vertices/indices, and ray-tracing position/index streams. Missing or degenerate normals/tangents regenerate deterministically per primitive. |
| Asset Browser workflow | Search, type/status filtering, physical folders, folder and asset context operations, nested model/material/texture associations, grid/list/zoom, real thumbnails, details, background import/reimport, hierarchy/viewport drops, Inspector assignment, entity naming/order, and scene persistence are integrated. |
| Scale and performance | 100,000 catalog records, 65,536 resident materials, 8,192 texture views/samplers, 10,000 dependency dependents, 1,024 rapid revisions, residency churn, and five long 4K car runs pass their gates. |
| M0-M2 preservation | All tracked M0-M2 fixtures, sample-car diagnostics, output transports, selected/unselected behavior, Vulkan validation, material provenance checks, and the 10 ms base-frame contract pass. |

## Final data and interface contracts

| Contract | Accepted version or rule |
|---|---|
| asset metadata sidecar | schema 1 |
| SQLite editor catalog | schema 1; rebuildable, never runtime authority |
| asset GUID | UUIDv7, serialized lowercase canonical form |
| import settings | schema 1 for production glTF and texture importers |
| glTF model importer | implementation version 3 |
| DirectXTex texture importer/codec | implementation version `20260508` |
| cooked artifact container | version 1, sectioned, checksummed |
| compiled-material product | version 1 |
| cooked texture product | schema 1 |
| cooked model product | schema 3 |
| compiled material | schema 1 |
| packed GPU material | schema 2 |
| material/texture identity in runtime | typed handles and GUID/subasset records; no path identity |
| scene mesh reference in the M3 transition | required asset GUID plus material overrides; removed `meshPath` is rejected with a migration error |
| Vulkan material resources | indexed storage-buffer table; dynamic capacity bounded by `maxStorageBufferRange` |
| Vulkan texture resources | indexed sampled-image and sampler tables with explicit view/sampler semantics |

The catalog is an editor/search acceleration structure derived from project files and
sidecars. It is not a renderer-specific shipping database. Cooked artifacts are
rebuildable DDC values, not identity authorities. Source files and metadata remain
the source-controlled inputs.

## M3.7 production cutover

The following transitional paths are removed:

- path-keyed runtime model lookup and source-file model loading;
- runtime source parsing and merge-by-material geometry construction;
- `MeshComponent::requestedMeshPath` and the load-only scene `meshPath` adapter;
- selectable `--material-descriptors` behavior;
- fixed-size 4,096 material storage;
- legacy non-indexed canonical and complex material shaders;
- per-material Vulkan descriptor sets and allocations.

Editor startup resolves the default model through the catalog and GUID, prepares or
finds its cooked artifact asynchronously, and publishes the cooked product. Explicit
benchmark startup accepts only `--cooked-model-artifact`. Profile schema 1 retains
the historical `source_import_ns` field at zero and records the cooked operation as
`model_load_ns`.

Descriptor indexing is a production capability requirement on the current high-end
Vulkan target. Initialization fails clearly on unsupported devices; M3 does not keep
a low-scale renderer that would block M7 material resolve.

## Determinism, provenance, and scale evidence

### Clean and warm cooking

Two independent Sponza production cooks began from separate empty DDC roots:

| Result | Value |
|---|---|
| cook key | `f12cbf30b5fbbefe34e4b4d8dcc005fe604518dfb018ccfdd97bb7ca1357af56` |
| artifact bytes | 117,659,836 |
| artifact/container SHA-256 | `e96e46f4967d32f32449b7eebdc29ea70c68066cdd1041ae4ad8b51b5f0e38af` |
| clean-cook comparison | exact byte match |
| warm receipt/DDC result | receipt hit/cache hit |
| warm preparation time | 1,583.856 ms |

The clean production path performs high-quality texture compression and is
intentionally much slower than metadata registration or the bounded editor-preview
path. It is asynchronous and observable. Independent texture streaming/publication
is still required before arbitrarily large full-resolution packages can have
sub-second first placement.

### Source-to-cooked material provenance

`InspectCookedModel --verify-source` recompiles source materials in strict mode and
compares every cooked compiled-material content hash:

| Model | Exact compiled/cooked matches |
|---|---:|
| Alfa Romeo | 87 / 87 |
| Bugatti | 23 / 23 |
| Sponza | 25 / 25 |

Material 84 on the Alfa remains source-default opaque, double-sided,
standard-deferred, metallic `0.9798125`, roughness `0.540954`, with its Dunlop
base-color view interpreted as sRGB and normal view as linear. Its two source
primitives remain distinct in the cooked model. M3 found no source-to-compiled or
compiled-to-cooked material mutation.

The owner's report that some material appearance remains implausible is therefore
not addressed with an import heuristic. The Alfa authors many omitted or explicitly
metallic values, including its tire, and the current renderer still lacks M5's
production direct lighting and split-sum IBL. M5 owns that lighting correction; M6
owns advanced glass. Those milestones must preserve M2/M3 provenance.

### Scale gates

| Gate | Result |
|---|---:|
| catalog rebuild, 100,000 records | 1,150.6 ms |
| warm catalog query p95 | 0.3034 ms (16 ms gate) |
| incremental query p95 | 0.0878 ms (4 ms gate) |
| catalog working set after / peak | 131.797 / 131.797 MiB |
| runtime publisher, 100,000 records, idle p99 | 0.0001 ms |
| runtime publisher schedule/publish p99 | 0.0005 ms |
| DDC lookup p95 | 0.043 ms (1 ms gate) |
| coalesced schedule p99 | 0.0001 ms (0.10 ms gate) |
| reverse dependency fan-out | 10,000 dependents, correct dependency-first order |
| rapid reimport | 1,024 revisions; newest published, 1,023 coalesced |
| material table | 65,536 real resident records, validation-clean |
| texture/sampler table | 8,192 real views and 8,192 samplers, validation-clean |

Pre-frame indexed-texture publication now batches descriptor synchronization until
`beginFrame`; mid-frame changes still synchronize the fence-owned set immediately.
The 8,192-record Release validation run averaged 1.47135 ms/frame.

## Integrated verification

- Full Debug build and 39/39 CTest: passed.
- Full Release build and 39/39 CTest: passed.
- Eleven source-controlled M0/M1/M2 fixtures at 3840x2160 using cooked artifacts and
  Vulkan validation: passed with no validation messages.
- Eight unique M0-M2 glTF benchmark inputs now have tracked schema-1 sidecars with
  unique root/subasset UUIDv7 identities and deterministic structural fingerprints.
- Sample car at 3840x2160: selected, unselected, scene Final, final output, Depth,
  Base Color, Normal, Material ID, Closure Class, and wireframe captured with
  validation enabled.
- SDR, scRGB, and HDR10: 60-frame 4K Vulkan-validation runs passed.
- Resized 1600x900 path: validation-clean; viewport rendering retains its aspect
  ratio while editor panel dimensions change.
- Texture-residency churn: fallback, immediate publication, no premature reuse, and
  post-fence reuse passed.
- Live 65,536-material and 8,192-view/sampler growth: validation-clean.
- Startup cutover profile: cooked artifact mode, `source_import_ns = 0`,
  `model_load_ns = 1,253,546,900` for that run.
- `git diff --check`: no whitespace errors; only existing Windows line-ending
  conversion warnings.

No Nsight capture was required for M3.7 acceptance: Vulkan validation, explicit
descriptor-table scale modes, upload counters, fence-reuse tests, GPU timestamps,
and zero-allocation evidence directly attribute the changed ownership.

## Sample-car capture contract

The accepted M2 opaque-coverage hashes remain stable for Depth and Material ID:

| Capture | SHA-256 |
|---|---|
| unselected final output | `fb0a37e71ab557a12e7a42609e6544b9f8d9c0de1b9d304ff3f689ea9b1f14d4` |
| selected final output | `8ba46efc6317927d4569d16145ebab6c2c1319c9c14e665437ed6edf35418668` |
| scene-linear Final | `79487ea6b47ab4c00c943d7970637d37ef4b2bce9df9d14b1fb79ccde9209fe4` |
| Depth | `686e0408ab608446cf5a740c1fb05268fdfa9eb7255465a9d4d5c8ae146f16db` |
| Base Color | `4e0703417e20420271273cc3f629aa997f191caa46c230043ae2ba03dd973fd6` |
| Normal | `c5e7202db3ace190984c35f8f6a2f0079e597b2db7fdbd03b5b3dcbb560a7c63` |
| Material ID | `496d487c83664d3f8b9dd3a20f8f5c210237155726e3e34f72432402880df996` |
| Closure Class | `a7785072cdaa0fb65c56f9cc91f22f440ed3a4ace9b672a59bc1546a0b57370f` |
| wireframe final output | `5c86e1291d987cae5b64a25e9406ced2396ed2d875506ff962d12c0416a69cc5` |

Selection is a final-output overlay, so the selected and unselected scene-linear
capture is identical. Depth and Material ID match the M2 material-84 coverage
acceptance, which proved zero background-depth pixels on identified tire coverage.

## Final 4K sample-car baseline

Five independent Release runs used 500 warm-up and 10,000 measured frames:

| Metric | Five-run median |
|---|---:|
| wall frame average | 3.5397 ms |
| CPU frame median | 1.7080 ms |
| CPU frame p95 / p99 | 15.4599 / 16.2975 ms |
| asset runtime tick p99 | 0.0029 ms |
| editor build median | 0.0537 ms |
| GPU frame median | 0.8450 ms |
| GPU frame p95 / p99 | 2.2391 / 3.3213 ms |
| canonical GBuffer median | 0.1741 ms |
| deferred lighting median | 0.0573 ms |
| UI median | 0.1096 ms |
| requested live / peak | 897.558 / 972.257 MiB |
| committed live / peak | 967.567 / 1,042.265 MiB |

The CPU p95/p99 tail is swapchain acquire/presentation waiting while the hidden run
outruns presentation, not simulation, render preparation, asset work, or UI
construction. The GPU median improves on the corrected M2 car baseline
(`0.955 ms`), while the measured tail remains below the 10 ms base-frame contract.
The fuller cooked texture residency and 4K graph account for the memory delta.

The final 10,000-frame allocation gate recorded:

| Metric | Result |
|---|---:|
| wall frame average | 3.189607 ms |
| CPU frame median | 1.5564 ms |
| GPU median / p95 / p99 | 0.836768 / 2.128608 / 3.003328 ms |
| steady allocation calls median / p99 | 0 / 0 |
| steady allocation bytes median / p99 | 0 / 0 |
| dropped profiler counters | 0 |
| startup upload | 78,327,200 bytes in one batch |

`CpuProfiler` now retains 128 counters per frame so M3 asset counters do not hide
the allocation gate. The retained steady ring's maximum was two calls and 80 bytes;
its median and p99 are zero.

## User-visible and architectural outcome

- Assets can be imported from inside or outside the project, organized in physical
  folders, renamed, moved, reimported, deleted, searched, filtered, previewed, and
  assigned without making source paths scene identity.
- Imported model associations remain collapsed until opened; model drawers expose
  materials and nested texture associations.
- Model placement works by dragging into the viewport or hierarchy. Mesh and
  material slots accept typed drops and persist GUID assignments and overrides.
- Large work is asynchronous, cancellable, phase-reported, and visible in the
  in-engine Console. Failed or cancelled work cannot replace last-known-good state.
- Viewport extent follows its actual panel without stretching the rendered image.
- Runtime rendering does not parse glTF, create authoring materials, merge source
  primitives, or allocate descriptors per material.

Primary interfaces affected are under `src/assets/`, `src/assets/cooker/`,
`src/assets/model/`, `src/assets/texture/`, `src/assets/runtime/`,
`src/assets/thumbnail/`, the Asset Browser/Console/editor scene actions, scene mesh
references, RHI texture/material contracts, and the Vulkan indexed tables.

## Deferred work and risks

- M4 owns stable scene entity UUIDs, serializer registration/migration, unknown
  component preservation, editor/runtime component separation, undo, and cooked
  runtime scenes. M3's GUID mesh field is deliberately narrow.
- M5 owns production clustered direct lighting, shadows, probes, and split-sum IBL.
  Current dark/metallic appearance must be evaluated there without destroying
  authored material provenance.
- M6 owns general transparent sorting, layered/refraction algorithms, and close-up
  glass cost.
- M7 owns persistent GPU-scene instances, indirect visibility, visibility-buffer
  material resolve, and changed-data submission. It should also consume
  independently resident texture products rather than require monolithic
  full-package publication for arbitrarily large content.
- M8 owns meshlet generation and mesh-shader execution. M3 preserves the primitive,
  bounds, and RT streams needed by it.
- Remote/shared DDC, virtualized texture payloads, progressive texture publication,
  and an interactive isolated model viewer remain future production/editor work.
- The local DDC and editor catalog are rebuildable and may be deleted safely, but
  project sources and sidecars are source-controlled identity inputs.

## Architecture and roadmap result

ADR-0004's accepted direction is implemented for assets. No accepted ADR changed and
no superseding ADR is required. M4 still owns the scene/component half of ADR-0004.
ADR-0001 material provenance, ADR-0003 future geometry consumers, ADR-0005
transparency boundaries, and ADR-0006 visibility compatibility remain intact.

`ROADMAP.md`, `docs/PROJECT_CONTEXT.md`, this plan, and
`docs/performance/FRAME_BUDGET.md` advance M3 to Accepted on 2026-07-31. M4 is the
next proposed milestone; it does not begin automatically with this acceptance.

## Fresh-lead handoff

Read `AGENTS.md`, `ROADMAP.md`, `PLANS.md`, `docs/PROJECT_CONTEXT.md`,
`docs/performance/FRAME_BUDGET.md`, every ADR, the M0-M2 acceptance reports, the M3
execution plan, and this report. Current source and the intentionally dirty worktree
are authoritative. Reproduce asset/runtime gates with the tracked benchmark
sidecars, cooked artifacts, scale validation modes, and M3.7 run manifest. No
information from the originating conversation is required.
