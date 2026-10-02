# M6 to M7 Lead Hand-off — 2026-08-28

## Status and purpose

M6 Hybrid Transparency is accepted and complete. M6.10 subsequently closed the
editor workflow issues discovered during final use, and M6.11 added live display-
transport switching. This document summarizes the work performed during the M6 lead
task, records its post-acceptance additions, and gives the M7 lead a practical
starting contract that does not depend on the original chat history.

M7 remains `Proposed`. Its lead must begin with a checked-in execution plan following
[`PLANS.md`](../../PLANS.md), then audit current source and establish the matched
three-high-fidelity-asset baseline described below before changing renderer behavior.

## Source of truth and recommended read order

Read current source before relying on prose. Some early historical paragraphs in
`PROJECT_CONTEXT.md` describe the pre-M6 two-bucket path; its later accepted-history
section, current source, ADR-0012, and the M6 acceptance report describe the current
production renderer.

Recommended order for the M7 lead:

1. [`AGENTS.md`](../../AGENTS.md), [`PROJECT_CONTEXT.md`](../PROJECT_CONTEXT.md),
   [`ROADMAP.md`](../../ROADMAP.md), [`PLANS.md`](../../PLANS.md), and
   [`FRAME_BUDGET.md`](../performance/FRAME_BUDGET.md).
2. [`M6 acceptance report`](M6-acceptance-report-2026-08-27.md) and the
   [`completed M6 plan`](M6-hybrid-transparency.md).
3. [`ADR-0012`](../architecture/ADR-0012-versioned-transparency-transport-and-bounded-execution.md)
   for production transparency and [`ADR-0013`](../architecture/ADR-0013-runtime-display-transport-switching.md)
   for live output transport.
4. [`ADR-0006`](../architecture/ADR-0006-hybrid-visibility-and-clustered-rendering.md)
   for M7's GPU-scene and visibility-buffer boundary, plus ADR-0001, ADR-0002,
   ADR-0007, ADR-0008, ADR-0009, and ADR-0010 for inherited material, HDR,
   lighting, shadow, and environment contracts.
5. The frozen M6 evidence under `out/benchmarks/m6.9-final/` and hashes in
   `assets/benchmarks/m6/run-manifest.v1.json`.

## Executive summary

The chat began with an M5-era compatibility renderer, fragile sample-asset startup,
slow monolithic model cooking, and a transparency path that treated the final packet
as a foreground bucket. It ended with:

- classified hybrid transparency as the only production architecture;
- stable per-primitive transparent identity, deterministic ordering, metric
  transport, scene-linear rough refraction, bounded 2/4/8-interface glass, and
  explicit non-refractive WeightedOIT;
- a corrected non-emissive Alfa Romeo headlamp result that exposes the authored
  chrome reflectors through the lenses;
- deterministic, observable model cooking with persistent derived-texture reuse,
  bounded parallel texture work, and safe oversized atomic model publication;
- an asset-independent editor startup scene containing separate Cube, Sun, and HDRI
  Sky entities, plus reusable entity-creation menus;
- restored viewport selection and a substantially improved transform, grid, light,
  inspector, and Asset Browser workflow;
- live Auto/SDR/scRGB/HDR10 selection without reloading the scene; and
- explicit roadmap ownership for the scaling and asset-streaming work now entering
  M7, with material and animation graph editors deferred to M12 and M13.

## Work completed during the M6 lead task

### Startup resilience and entity creation

Interactive startup no longer depends on the Alfa Romeo or any external model
metadata. When no explicit cooked model is supplied, `Application` loads the cached
built-in cube and constructs three independent editor entities:

- `Cube`, backed by hard-coded/cached built-in geometry;
- `Sun`, using the directional-light preset; and
- `HDRI Sky`, using the Sky component and the active cooked environment when one is
  available.

An explicit cooked model remains available for benchmarks, diagnostics, and captures.
The top menu and Scene Hierarchy context menu expose Empty Entity, Cube, Directional
Light, Point Light, Spot Light, and HDRI Sky presets. Creation runs through editor
scene commands so selection, undo/redo, stable scene identity, naming, and component
defaults remain coherent.

Primary implementation areas are `src/core/Application.cpp`,
`src/editor/EditorSceneActions.*`, `src/editor/EditorSceneCommandService.*`,
`src/editor/panels/menus/MenuBarPanel.cpp`, and
`src/editor/panels/core/SceneHierarchyPanel.cpp`.

### Asset identity, import, cooking, and publication

The Alfa import failure and later Sponza import stall exposed several independent
problems. The resulting behavior is:

- Root, material, texture-view, source-primitive, and connected-piece GUIDs remain
  stable across reimport and policy edits. The Alfa regression preserves its root
  plus all 281 subasset GUIDs.
- Transparent source primitives are split into deterministic connected runtime
  pieces instead of merging disconnected surfaces. Each piece retains both
  `sourcePrimitiveGuid` and its exact cooked `primitiveGuid`.
- Asset-catalog duplicate-GUID diagnostics remain strict. Six benchmark metadata
  snapshots that accidentally used the live `.iridium.meta` suffix were renamed to
  a non-discoverable fixture form; the catalog then reported zero duplicate matches.
  Do not weaken duplicate detection to hide bad sidecars.
- Material and primitive policy edits use an independent asynchronous asset-settings
  transaction history. Apply, undo, and redo each enqueue a real recook. History
  advances only after the matching catalog job succeeds; failure or external
  divergence cannot silently publish stale settings.
- Model cooking exposes an optional backend-neutral progress callback. CLI cooks
  emit flushed `IRIDIUM_COOK_PROGRESS` records; editor cooks publish the same stages
  to Engine Console. Stages include preparation/metadata, parent DDC request,
  importer, materials, each texture-view built/cache-hit completion, geometry,
  model sections, serialization, and artifact publication with elapsed time and
  source identity.
- Embedded texture views use persistent deterministic DDC child products. A policy-
  only Alfa recook fell from 39.558 seconds cold to 6.583 seconds, an 83.4% reduction.
- Unique texture-view jobs run in deterministic output slots on a bounded pool of at
  most 16 workers and no more than half the hardware threads. On the measured cold
  Sponza workload, eight workers completed in 56.966 seconds and 16 workers in
  49.055 seconds; a warm parent/DDC hit took 1.460 seconds. Geometry took only about
  119 ms after texture completion, so indiscriminate geometry parallelism was not
  justified by that profile.
- The 128 MiB upload value is now a per-tick scheduling target, not a hard rejection
  limit. One oversized model may publish atomically and alone in a tick up to a
  separate 1 GiB per-model safety cap. Ordinary uploads still share the 128 MiB
  budget, and the independent HDRI cap remains 640 MiB.
- Thumbnail lookup understands connected cooked pieces via `sourcePrimitiveGuid`.
  False `Thumbnail failed` tiles no longer imply failed model publication.
- Asset Browser cooked-result panels expose requested/resolved transparency class,
  quality, fallback, connected-piece count, active/pending CookKeys, runtime state,
  and published revision so artists can tell whether a live edit was actually
  applied.

The current path is materially better but remains a complete-parent cook and an
atomic model publication path. Fine-grained geometry products, cross-product
dependency reuse, cancellation, and progressive residency are deliberately M7 work.

### Classified hybrid transparency

ADR-0012 is the authoritative policy. Transparency class, coverage, execution phase,
and quality are independent concepts. `TransparencyPolicyV1`, keyed by stable
material or primitive GUID, carries:

- Auto or explicit `AlphaClip`, `SortedSurface`, `ThinGlass`, `LayeredGlass`, or
  `WeightedOIT`;
- `Ordinary2`, `Hero4`, or `Cinematic8` quality;
- signed author priority; and
- nonnegative thin-sheet thickness in metres.

The policy survives source settings, compiled material, cooked product, runtime
submesh, `DrawPacket`, diagnostics, and the packed GPU material ABI. Invalid or
unknown input is normalized with a diagnostic. Policy bytes participate in CookKeys.
Do not collapse this policy into Vulkan pipeline enums or expand `RenderQueue` to
replace it.

Auto intentionally never selects WeightedOIT or expensive hero tiers. Important
fallbacks include positive-volume content without valid closed topology to
ThinGlass, incompatible transmissive SortedSurface to ThinGlass, and refractive or
dispersive WeightedOIT to SortedSurface. Serialized author intent is retained while
requested and resolved behavior remain inspectable.

Production paths now are:

- **AlphaClip:** conventional coverage rejection for masked material.
- **SortedSurface:** stable, premultiplied, depth-read-only forward blending using
  author priority, camera-space primitive depth intervals, and stable identity ties.
- **ThinGlass:** active-camera, metric, scene-linear dielectric transport with
  separate geometric and shading normals, bounded Beer-Lambert absorption, Fresnel,
  total internal reflection, and explicit off-screen fallback.
- **LayeredGlass / Ordinary2:** genuine paired entry/exit capture and local AP1
  composition for ordinary validated closed volumes.
- **LayeredGlass / Hero4 and Cinematic8:** explicit-only, bounded four/eight-interface
  peeling in packed optical islands, with area limits, deterministic residual tails,
  per-tile early termination, lifecycle hysteresis, and visible diagnostics.
- **WeightedOIT:** explicit-only approximate non-refractive content using FP16
  premultiplied AP1 accumulation and revealage. It is appropriate for particles and
  high-overdraw effects, not hero glass.

Rough refraction uses one conditional graph-owned scene-linear `RGBA16F` color
pyramid plus `R32F` conservative depth pyramid. Off-screen or unavailable samples
fall back through the accepted probe/environment transport rather than stretching
screen-edge pixels. Transparent work reads main opaque depth but does not write it.
Transparent surfaces remain excluded from scene reflection-probe capture to prevent
screen-space recursion.

The legacy two-bucket/glass-depth renderer is removed from the production graph. It
exists only behind `--developer-legacy-transparency` for explicit regression A/B.
Rejected specialist work uses one classified compatibility-forward pass; it never
silently revives the legacy glass-depth approximation.

### Alfa Romeo investigation and final result

The initially edited `nodes/40/meshes/10/primitives/0` is unrelated opaque geometry.
The actual transmitting headlamp surfaces are:

- `nodes/22/meshes/4/primitives/0`; and
- `nodes/25/meshes/5/primitives/0`.

Those materials omitted glTF `metallicFactor`, whose glTF default of 1.0 suppressed
the shared shader's dielectric transmission. Their source settings were corrected to
metallic 0.0. A short-lived emissive workaround was added while investigating the
dark interior, then removed at the owner's direction; the final lenses are
non-emissive dielectric ThinGlass.

The final renderer correction was not an asset fake. Zero-distance ThinGlass had
sampled an earlier opaque-only pyramid and composited opaquely over a transparent
chrome reflector that had already rendered. Classified ThinGlass also lacked the
execution feature bit needed for local composition. All classified transparent
runtime variants now carry that bit and use premultiplied blending. Zero-distance
ThinGlass evaluates macro transmission from the geometric normal, preserves
normal-mapped Fresnel detail, and composites locally over the current destination;
metric/thick transport continues through the refraction pyramids.

The accepted Alfa final-SDR capture is non-emissive, shows the authored chrome bowls
and central optics through the ribbed lenses, is Vulkan-validation clean, and has
SHA-256 `4165a6b05d4baab2560b5a08069f3be4bf4d7c214ecee1a7edcf36683531111d`.
This remains raster screen-space transport, not ray tracing.

### Artist-facing transparency and diagnostics

Model materials and source primitives can be selected from an attached horizontally
scrollable thumbnail strip under the parent model card. Each stable primitive may
inherit material policy or explicitly override class, priority, metric thin
thickness, and 2/4/8-interface quality. Tooltips explain the intended content and
cost of each tier.

Opaque cooked primitives collapse and disable transparency controls by default. An
explicit stable-GUID override checkbox reveals them when an artist has a legitimate
reason to force a policy. Missing or pending cook evidence never hides the controls.

Material Diagnostics, Asset Browser details, headless snapshots, viewport debug
views, and Profiler presentation consume one backend-neutral resolved diagnostic
contract. Available views include Transparency Class, fallback, measured interval,
selected pyramid mip, retained layer count, and overflow/saturation. The Profiler
groups all `gpu.transparency.*` ranges and highlights rejection, fallback, overflow,
and profiler-capacity problems.

### Editor workflow and quality-of-life additions

The post-acceptance editor closeout added:

- nearest-mesh left-click selection in the viewport;
- a default infinite procedural grid on Iridium's Y-up XZ plane at `Y = 0`;
- derivative-filtered decade spacing, a smooth horizon/grazing fade, and rejection
  behind the existing opaque depth so foreground objects occlude the grid;
- an optional grid-follow mode that aligns through the selected pivot and active
  world/local translation axis or plane during manipulation, with fixed `Y = 0` as
  the alternative;
- world SI display units in millimetres, centimetres, metres, or kilometres;
- independent translation, rotation, and scale snapping. Enabling snapping affects
  only new manipulations; existing transforms are not quantized. Holding Ctrl during
  a manipulation bypasses snapping for fine control;
- world/local gizmo space, with local space using the selected entity pivot;
- uncapped nonnegative physical light intensity and range controls, including
  lux/kilolux and candela/lumen presentation;
- Inspector component/property search with visual separation below the search
  header;
- physical folder nesting by drag/drop from both Asset Browser content and tree
  views, plus persistent custom tree ordering in
  `out/editor/asset-browser-folder-order.json`;
- a wider Up affordance in the Asset Browser;
- viewport asset/model drops that always set the entity transform's vertical
  coordinate to world `Y = 0`, including a parallel or above-horizon ray fallback;
  this grounds the entity pivot, not the mesh's lowest vertex; and
- the beginning of a contextual shortcut policy. Ctrl owns document/transaction
  commands, Alt owns viewport presentation/tools, and Shift owns additive/alternate
  behavior. `Alt+G` toggles the grid only while the viewport is focused and yields
  to text input, active widgets, gizmo use, and popups.

The shortcut policy is a foundation, not yet a complete user-remappable keymap.
Future editor work must reuse the semantic groups and context routing instead of
adding globally greedy chords.

The procedural grid is an editor overlay in the output transform. It adds no graph
image or render pass and is disabled for benchmarks, asset previews, and renderer
captures so it cannot contaminate frozen image evidence.

### Live HDR display transport

Project Settings now exposes `Auto`, `SDR`, `scRGB`, and `HDR10`. Requested and
effective transport are separate. Auto prefers scRGB, then HDR10, then SDR;
unsupported explicit HDR requests retain the requested preference but fall back to
SDR with a visible diagnostic. Concrete choices unsupported by the active Vulkan
surface are disabled in the editor.

Switching occurs at a frame boundary. Vulkan creates and validates a replacement
swapchain, waits for active GPU frames once, and rebuilds only presentation-dependent
output, optional HDR10 encode, UI, graph-target, framebuffer, pipeline, and descriptor
state. Scene entities and resident geometry, materials, textures, and environments
remain loaded. The ACES LUT follows the effective transport: Rec.709/100 nit for SDR
and P3-D65/1000 nit for scRGB/HDR10.

This is a deliberate display-mode stall, not steady work. The validation sequence
measured approximately 179-223 ms per change with no Vulkan validation messages and
no scene reload. Windows HDR and a compatible display must still be enabled; Iridium
does not enable the operating system's HDR mode. ADR-0013 supersedes only ADR-0002's
old restart requirement.

## Accepted M6 evidence

The final production qualification and post-acceptance checks report:

| Workload | Native-4K GPU median | Transparency median | Important result |
|---|---:|---:|---|
| Ordinary2 populated grid | 0.720192 ms | 0.403456 ms | Five processes, 50,000 frames, zero fallback/legacy work |
| Cinematic8 four-shell control | 1.612576 ms | 1.276928 ms | All eight interfaces reached, zero compatibility draws |
| WeightedOIT 256-particle field | 2.657440 ms | 2.362368 ms | One accumulation draw and one resolve |
| Alfa three-light Ultra/Belfast | 5.820864 ms total | 0.466368 ms | 4.45% faster than M6.0 total GPU median |

Ordinary transparency remains below its 1.0 ms native-4K budget. Hero and OIT paths
are explicit quality costs and are reported separately. The representative
classified transport domains reached zero retained-frame C++ allocations. The more
complex Alfa scene retains 16 calls/1,912 bytes per frame in general
renderer/light/editor work; this is M7 submission/update debt, not M6 transparency
allocation.

The infinite grid costs approximately 0.007 ms at 1280x720 in its focused editor
checkpoint. It is excluded from the native-4K benchmark path. Runtime display
transport selection has no steady-frame polling or graph mutation.

Debug and Release both pass the complete 71-test suite. Validation-enabled
classified, forced-legacy, invalid-topology, resize, topology lifecycle, output-
transport, selection, capture, and grid runs complete without Vulkan messages.
Post-M6.11 verification repeated 71/71 in both configurations.

## Contracts M7 must preserve

### Identity and asset contracts

- Persistent identity is a scene entity UUID or asset/subasset GUID. ECS indices,
  GPU slots, vector indices, pointers, queue positions, and indirect-command offsets
  are never persistent identity.
- `sourcePrimitiveGuid` identifies authored source work; `primitiveGuid` identifies
  an exact deterministic connected cooked piece. Do not discard either when moving
  data into persistent GPU-scene records.
- A material-only change must not mutate geometry identity. A one-primitive change
  must not rebuild unrelated primitive children.
- Publication is revision-safe and retains the last complete usable revision until
  its replacement is complete. Do not expose partially mixed parent/child revisions.

### Rendering contracts

- Keep material, lighting, reflection, transmission, emissive, OIT, and composition
  scene-linear ACEScg/AP1 until the single output transform.
- Reuse the M2 packed material and shared BSDF libraries. A visibility-buffer resolve
  may emit the canonical packed surface cache, but it must not create a new shading
  truth.
- Reuse the single M5 clustered-light assignment for deferred, forward-complex, and
  transparent consumers. Do not introduce a GPU-scene-only light list.
- Preserve independent per-light shadow visibility and local `+Z` light emission.
- Keep geometric normals, shading normals, transport thickness/depth, and shadow
  receiver bias separate.
- Transparent work stays in M6's classified forward/ordered paths. The M7
  visibility-buffer experiment is for standard opaque surfaces first.
- Main-view visibility is not shadow-view or probe-view visibility. Each view needs
  conservative independent culling while sharing stable instance records.
- Do not merge disconnected transparent pieces, reorder transparent stable ties, or
  make hero tiers automatic as part of batching.
- Render-graph resources and descriptors remain extent-aware and fence-retired.
  Resize, zero extent, capture insertion, topology changes, and live display
  transport must continue to coexist.

### Performance and measurement contracts

- Optimize for the 10.0 ms native-4K base-render target on the reference RTX
  4090/i9-14900K system. High-end hardware is not permission for unmeasured work.
- Use Release for performance decisions and report median, p95, and p99 over matched
  scenes. Separate simulation, preparation, command recording, GPU execution,
  acquire/present waits, asynchronous cooking, and upload stalls.
- Keep image quality, temporal stability, memory, and latency alongside timing.
  An FPS title value is evidence of a symptom, not bottleneck attribution.
- Preserve zero-steady-allocation behavior in the representative M6 transport paths
  and drive the remaining general scene submission allocations toward zero.

## M7 starting problem and recommended first execution order

During M6 the owner observed roughly 180 FPS with three dense high-fidelity assets
versus roughly 1,700 FPS in an empty scene on the same 240 Hz display. These values
are about 5.56 ms and 0.59 ms per completed frame, a credible 4.97 ms scene-dependent
delta. The scene already exceeds the greater-than-100-FPS product target, but the
observation is not a qualification: exact resolution, camera, asset revisions,
triangle/draw/material counts, shadow work, and CPU/GPU pass timings were not frozen.

Current source predicts the scaling:

- every enabled opaque submesh becomes a CPU `DrawPacket`;
- packets are sorted and most source submeshes produce individual direct
  `vkCmdDrawIndexed` calls;
- there is no general opaque main-view frustum or Hi-Z occlusion culling;
- cooked LOD and meshlet sections are not populated;
- dense assets remain at full authored detail in main, eligible shadow, and capture
  views; and
- only limited classified-transparent and local-shadow bounds rejection exists.

The M7 lead should prefer the following vertical order, adjusting only after the
source audit and baseline provide contrary evidence:

1. **M7.0 — Audit and freeze the three-asset baseline.** Preserve the exact source
   revisions and fixed cameras for all-visible, one/two off-frustum, occluded,
   near/mid/far, static, moving, shadow-only, and representative-transparent cases.
   Use at least five fresh native-4K Release processes and record the complete CPU,
   GPU, memory, residency, draw, triangle, material, and wait breakdown required by
   `FRAME_BUDGET.md`.
2. **Persistent GPU-scene records and compact updates.** Establish stable
   instance/primitive/material/bounds records, current/previous transforms, and
   explicit Static, Movable, and Animated update policy. Mobility may be authored or
   derived by a documented rule; never infer permanence merely because an object has
   not moved recently.
3. **Conservative frustum visibility and compact indexed indirect submission.** Make
   CPU extraction and command recording scale with changed data and visible batches,
   while keeping classic indexed direct submission as an A/B fallback.
4. **LOD data and screen-space-error selection.** Add deterministic cooked/authored
   LOD chains with stable primitive/material identity, hysteresis, diagnostics, and
   hero overrides before claiming distant-scene savings.
5. **Hi-Z occlusion with temporal conservatism.** Handle camera cuts,
   disocclusion, near-plane/camera-intersecting bounds, and small objects. Missing or
   uncertain visibility must fail visible, not disappear.
6. **Independent visibility consumers.** Share instance records but build
   conservative main, directional/spot/point shadow, and reflection-capture visible
   sets. Prove that camera-culled objects can still cast shadows or appear in probes.
7. **Progressive GPU residency and fine-grained cooking.** Replace monolithic model
   publication with deterministic texture, geometry/LOD, later meshlet, and RT child
   products. Use per-frame byte/time budgets, cancellation, visible-demand priority,
   last-known-good publication, semantic texture fallback, and bounds/proxy or
   coarser-LOD geometry fallback.
8. **Indexed visibility-buffer experiment.** Reconstruct correct attributes and
   derivatives and resolve standard opaque surfaces into the M2 canonical packed
   cache. Compare the complete visibility + material resolve + cache + lighting
   chain against conventional packed deferred; do not accept an attachment-only win.
9. **Virtual-shadow experiment.** Add sparse paging only after persistent scene
   visibility exists, and retain it only if matched 4K timing, memory, cache, and
   quality beat the hardened conventional-map baseline.

The roadmap combines these under M7, but the lead should keep only one slice in
progress and leave the engine buildable after each. M8 consumes the same GPU scene
for meshlet/normal-cone culling and mesh shaders; M7 must not create an indexed-only
identity model that M8 has to replace.

## Import/cooking acceptance cases M7 must add

The M7 incremental-cook and residency work is not complete until it proves:

- material/policy-only edit: no texture recompression and no geometry/LOD/meshlet
  rebuild;
- one source primitive edit: only that primitive and dependent children rebuild;
- one texture edit: only dependent semantic views/material parents rebuild;
- unchanged source: preparation plus complete parent/child DDC hits;
- superseded edit: bounded cancellation and newest-revision-only publication;
- oversized model: progressive visibility within frame upload budgets, with no
  single monolithic hitch and no mixed-revision corruption; and
- deterministic parent bytes and output slots regardless of worker completion
  order.

Record wall and aggregate CPU time, worker occupancy, per-stage cache hits, cancel
latency, peak CPU memory, derived bytes, upload bytes, per-frame upload time, and
fallback/residency state. Parallelism is justified by measured work, not worker count.

## Future roadmap commitments established during the chat

These are durable planned features, not additions to active M6 scope:

- **M7:** persistent GPU scene; static/movable/animated update policy; main/shadow/
  probe visibility; frustum culling; screen-error LOD; Hi-Z occlusion; compact
  indirect submission; progressive GPU residency; fine-grained per-primitive cooking
  and deterministic child DDC; indexed visibility-buffer experiment; and measured
  virtual-shadow work.
- **M8:** meshlet cooking, per-LOD bounds and normal-cone rejection, and optional
  `VK_EXT_mesh_shader` submission over the same M7 GPU-scene/visibility contracts.
- **M9:** anti-aliasing and temporal reconstruction. The intended order is a native
  TAA/DLAA-quality reference, stable jitter/motion/reactive/history data, then DLSS
  with room for FSR/XeSS. General AA was explicitly not pulled into the grid work.
- **M10:** production non-RT GI/AO, physically based simulated sky/atmosphere/clouds,
  project quality profiles, and bounded raster RGB/optical-depth transparent shadows
  consuming M6 material semantics. This does not imply caustics.
- **M11:** optional hybrid ray tracing, ray-traced transmission/reflection/GI,
  acceleration-structure support, denoising, and a reference/photo-mode path tracer.
- **M12:** production material graph/editor with deterministic GUID-addressed graphs,
  live material-instance parameters, isolated HDR preview, incremental compilation,
  diagnostics, and shared raster/RT closure output. It is scheduled after the current
  M0-M11 renderer program.
- **M13:** versioned animation assets and controllers, multithreaded pose evaluation,
  GPU skinning, current/previous skinned data, and a production animation graph
  editor. It depends on M7 visibility/update policy and M9 motion history.

A complete user-remappable shortcut system is not yet assigned a numbered milestone.
The implemented contextual-prefix policy and `Alt+G` are the current contract; any
expansion should be planned explicitly rather than accumulated as unrelated global
shortcuts.

## Known limitations and deliberate deferrals

- Raster refraction remains bounded and screen-space. It cannot reveal off-screen
  geometry and does not provide path-traced multiple scattering.
- Transparent surfaces receive existing scalar per-light shadows but do not cast
  production colored/transmissive shadows. M10 owns the bounded raster product and
  M11 the ray-traced alternative. Caustics remain unsupported.
- Ordinary2 is the automatic closed-volume tier. Hero4/Cinematic8 and WeightedOIT
  are explicit artist choices with real memory and fragment costs.
- WeightedOIT is approximate and non-refractive; it must not become a hero-glass
  shortcut.
- The developer legacy transparency path remains only for a bounded post-M6 support
  window and should eventually be removed after regression value expires.
- Current model publication can admit one large asset safely but is not streaming.
  A 1 GiB model still risks a visible one-shot upload stall.
- Tree-folder custom order currently lives in an editor-output file, not a shared
  project asset.
- Asset-drop grounding uses the entity pivot. Meshes authored with an offset pivot
  can still extend below or float above the world plane.
- The contextual shortcut layer is semantic and context-aware but not yet a
  configurable binding registry.
- scRGB/HDR10 selection depends on the active OS/display/Vulkan surface. The editor
  cannot force Windows HDR on.

## Worktree and integration warning

At hand-off, the repository contains the accumulated M6 implementation and evidence
as a large dirty worktree, including tracked modifications and new untracked source,
shader, fixture, ADR, report, and test files. It also contains ordinary user/editor
state such as `imgui.ini`. Do not run `git reset --hard`, `git clean`, bulk checkout,
or regenerate hashes blindly. Inspect `git status`, classify each path, preserve
unrelated user changes, and integrate deliberately. The deleted old
`cinematic8_nested_tetrahedra.mixed-deep.iridium.meta` path is intentional: its
replacement is a metadata fixture whose filename cannot be discovered as a live
asset sidecar.

## M7 entry checklist

Before the first M7 implementation change, the new lead should be able to answer:

- Which exact CPU and GPU stages own the three-asset 4.97 ms wall-time delta?
- How many source, requested, visible, shadow-only, probe-only, and rejected
  instances/primitives/triangles exist in each frozen camera?
- What stable identity and revision fields survive into every proposed GPU record?
- Which changes cause instance, geometry, material, texture, shadow, or probe uploads?
- What is the conservative fallback when visibility, LOD, residency, or child
  publication is uncertain?
- How do main, shadow, probe, transparent, and future RT consumers share records
  without sharing an incorrect visibility decision?
- What conventional indexed/deferred fallback remains available for every
  experimental indirect or visibility-buffer stage?
- Which M6 captures, hashes, transparency counters, allocation gates, and lifecycle
  tests will detect a regression?

Once those questions are answered in the M7 execution plan and the matched baseline
is frozen, M7 implementation can begin without reopening M6 architecture.
