# Material authoring and asset viewer

Status: in progress, first implementation slice 2026-09-05. This does not close
the owner's full material-editor/viewer request or advance M7.6 acceptance.

## Viewer layout/camera follow-up

### Asset subcar steady-frame scaling repair (2026-09-10)

The reported drop from roughly 1,200 FPS to 400 FPS is approximately 0.83 ms to
2.50 ms per frame: about 1.67 ms of added CPU/UI work, despite the large-looking
FPS difference. Inspection found that the expanded horizontal subcar repeated a
catalog query, copied source-detail maps, reclassified and sorted every child, and
constructed an ImGui child window plus thumbnail draw for every material/primitive
on every frame. Cost therefore scaled with total imported child count, including
cards entirely outside the clipped horizontal view.

The subcar now caches catalog records and classification by root/source revision,
limits source-detail probes to once per 30 UI frames, and invalidates on successful
catalog work or drawer replacement. Offscreen cards retain layout with a dummy and
do not create child windows, query textures or emit image commands. Thumbnail demand
now follows only cards visible in the subcar (with one-frame scroll prefetch lag),
and the deep demand map is rebuilt only when that compact GUID set changes. Existing
material/model/transparent grouping, selection, drag/drop and double-click behavior
remain intact.

Debug/Release builds and the complete 77-test suites pass; focused asset-browser,
thumbnail and renderer regressions also pass after the final demand change. Exact
interactive before/after FPS still requires the owner to repeat the same asset and
window test because the automated benchmark cannot open and hold this native ImGui
drawer. No renderer, VRAM or ADR change is claimed for this editor-only repair.

### Project settings organization and layer-budget testing (2026-09-09)

Project Settings has left-side categories and a scrollable content pane: Display
and HDR, Lighting and shadows, Reflection probes, Transparency. Search filters
matching groups using category/control keywords; choosing a category clears search.
Existing controls and reset behavior are retained. Search is group-level, not
individual property highlighting.

Transparency exposes an explicit session-only override of Layered Glass interface
quality: authored (default), Ordinary2, Hero4, Cinematic8. The backend-neutral
`withLayeredInterfaceBudget` helper changes only quality on already-resolved
Layered Glass draw packets. Authored/cooked assets, GPU material identities,
classes, topology flags, optical parameters and priorities remain unchanged.
The override applies to scene and viewer, not benchmark execution; existing
atlas residency, bounds and fallback paths remain responsible for execution.
The UI explains that interfaces are not object counts and warns of increased
cost. This extends editor execution controls within ADR-0012's existing 2/4/8
bounds; it does not change accepted authoring semantics or add a new tier.

Persistence of this override in project settings is still outstanding; it resets
on restart and is explicitly labeled session-only. Tier-switching visual/GPU
qualification and the missing 930 bulbs are not closed by this control. The owner
reports the old 930 black-lens problem fixed in Blender and the material editor;
see the updated Porsche triage record. Debug/Release builds, all 77 Release tests
and targeted Debug editor tests pass, including bounded overrides, preservation
of author intent and non-Layered class immunity. A 240-frame current-930 viewer
validation launch exited 0 with no engine Vulkan errors (the loader skipped the
third-party Overwolf layer). This launch used authored quality; it does not qualify
interactive tier changes or the new layout visually. No 4K timing/VRAM claim is made.

M7 resume checkpoint remains M7.6 Hi-Z occlusion/shared depth pyramid (M7.0-M7.5
accepted); M7.7 retains independent-consumer and conventional-shadow work. These
side features do not advance acceptance or replace the milestone execution plan.

### Primitive override clarity and stable list interaction (2026-09-09)

The owner explicitly retained primitive transparency overrides. When enabled, the
primitive's transparency editor now displays a wrapped yellow notice that it takes
precedence over the material, with instructions to disable the override to inherit.
This shared editor covers both the viewer options panel and browser asset details.
Primitive row metadata lists the referenced material names and stable IDs, deduplicated
across runtime pieces; unavailable catalog names fall back to IDs. This does not
change geometry, material assignments or transparency precedence.

Viewer list centering no longer observes camera framing revisions. Double-click
isolation and Show whole therefore do not recenter the list; initial document
selection centering is retained. The unused list framing cache was removed.
Material-association tests cover repeated, multiple and missing material IDs.
Both Debug/Release builds, all 77 Release tests and targeted Debug editor tests
pass. A 240-frame 930 viewer validation launch exited 0 with no engine Vulkan
errors (the loader skipped the third-party Overwolf layer).
Interactive yellow-note readability and double-click scroll behavior still
need visual qualification; no before/after GPU or VRAM measurement was taken.
No shader/RHI changes or ADR/M7 acceptance changes are introduced.

### Current integrated viewer (2026-09-08)

Interaction/cadence follow-up: `EditorViewScheduler` now arbitrates the two
retained outputs. An overdue background view cannot take consecutive submissions
while both views are visible, preventing foreground starvation below 30 total
submissions/second. The background rate remains a cap, not a guaranteed rate;
under load it yields alternate turns to focus. Only successful submissions consume
a turn. Tests cover slow cadence, the cap, focus changes, hidden views and reset.
When both views are hidden, the existing scene-render fallback is retained; this
is not a whole-application idle/simulation policy. Hidden preview windows no longer
submit image/camera controls or replace their last useful requested extent.
Collapsed list/material/scene panels still contribute focus for scene-input blocking.
Changes are confined to editor scheduling, Application integration, viewer UI and
editor tests; no renderer allocation or shader change is needed. Both builds,
77/77 Release tests and targeted Debug editor tests pass. A 240-frame current-930
viewer Release validation launch exited 0 with no engine Vulkan errors (the loader
skipped the third-party Overwolf layer). Interactive docked-tab,
collapsed-title-bar and drag input qualification remains open. No measured
before/after GPU/VRAM comparison or ADR/M7 acceptance change is claimed.

Hover-feedback follow-up: viewer row hover now adds an 18% amber display-overlay
tint across the referenced primitive/material pieces, independently of the cyan
selection outline. A selected-and-hovered piece retains both cues. Isolation still
suppresses both. Selection packets encode feedback in the previously reserved byte
of `DrawPacket` (size remains 240 bytes), and the existing 80-byte mask push block
uses its first padding word. Negative mask alpha carries the feedback bits; no
additional images, passes or material edits are introduced. Legacy scene selection
queues retain the outline default. This retains the existing x-ray mask semantics,
not transparency-aware surface picking; overlapping selected/hovered geometry
still shares one rasterized mask. Interactive glass/overlap and HDR appearance
qualification remains open. Debug/Release builds, all 77 Release tests and the two
targeted Debug editor/color tests pass. The current 930 viewer completed a
240-frame validation-enabled Release launch (exit 0, no engine Vulkan errors;
the loader skipped the third-party Overwolf layer). This is launch evidence,
not interactive hover appearance acceptance. No before/after GPU timing or VRAM
measurement has been made; M7 acceptance and accepted ADRs are unchanged.

Session-lifecycle follow-up: viewer caches now follow both asset GUID and opening
session serial. Closing/reopening the same GUID between UI frames retires its
camera, bounds, expanded groups, preview lighting and lighting undo history.
Tab activation and reuse of an already-open document retain those caches. Camera
framing also checks the opening session, and pre-UI camera/lighting queries reject
stale sessions. The collapsed viewer submits its private dockspace as KeepAliveOnly
so hiding child windows does not discard their dock ownership. Regression tests
cover first opening, activation, reuse, unobserved close/reopen, and final closure.
Debug/Release builds, Debug document tests and all 77 Release tests passed.
A 240-frame current-930 viewer launch exited 0 with validation enabled and no
engine validation errors. This does not replace manual collapse/reopen interaction
or visual/performance qualification; no accepted ADR or M7 status changed.

The list, preview, Scene options and Material options now use a private ImGui
dockspace inside the Asset Viewer. Default placement is list left, preview center,
Scene options top right, Material options bottom right. Panels can float, resize,
close and redock; toolbar buttons reopen them. Settings windows have explicitly
opaque backgrounds. This replaces the earlier manual child-panel drag grips.
The redundant wireframe/unlit/lit selector and its temporary neutral materials
were removed at the owner's request; the existing viewport diagnostic selector
remains, with independent scene/viewer mode state.

Scene options includes the horizontal project HDRI picker, asynchronous preparation
status/errors, HDRI rotation, lighting/background EV, output exposure, and a separate
preview sun with yaw/pitch, linear color, enable and EV controls. Lighting history
keeps individual control gestures separate. The sun lives in an editor-only world;
the scene's authored lights, HDRI identity and exposure are not edited. Scene probe
captures pause on preview renders, and scene local-probe influence is excluded from
the preview. Scene resources remain resident.

Both views are live. A serial renderer reuses frame-slot transient depth/color and
lighting work; two retained display-linear output images preserve independent UI
results. The background refresh is fixed at 30 FPS by the latest owner request;
remaining render turns go to the focused view. This is not two camera uploads into
one command submission and not a permanently frozen screenshot. Each completed
render acknowledges its own cadence. Current view-history identity changes on view
switches, conservatively invalidating shared temporal work rather than blending
different cameras. Independent persistent temporal histories remain future work.
Retained outputs are recreated behind fences on size/format changes and retired
on viewer close. HDRI descriptors update only the fence-completed frame set rather
than waiting for both frames on each view switch.

The preview fills its exact panel area without distortion or bars: centered UV
cropping is paired with a projection correction preserving target-aspect geometry
and vertical FOV. Shared scratch resolution follows the maximum requested view
dimensions. Tests cover wide/tall/square source and target combinations. Scene
mouse-look/fly/scroll input is blocked whenever a viewer panel has focus; closing
the last viewer clears that input ownership. Additional output storage is two
images (64 MiB at 4K SDR, 128 MiB at 4K FP16 HDR), not a second GBuffer/asset pool.

Verification so far: Debug/Release builds, Debug document tests and all 77 Release
tests passed. The current 930 GUID completed a 6,000-frame validation-enabled run
with no engine Vulkan errors. Earlier GUIDs in this log no longer identify the
current imported cars: the old 930 was rejected as unavailable and old Carrera
preparation timed out. PrintWindow produced a blank GPU surface, so it is not
visual evidence. Interactive docking/input/resize and different-HDRI switching
still need screenshot-based qualification. No before/after GPU/VRAM performance
admission or M7/accepted-ADR status change is claimed.
Final follow-up builds and the 77-test Release suite passed; a further 240-frame
930 run at 1280x720 completed validation-clean after adding reachable-window bounds
and per-document history identity. Saved dock settings confirm separate left,
center, top-right and bottom-right nodes. Native screenshot capture remained
unavailable (foreground acquisition was denied); that is not visual acceptance.

The dated entries below describe intermediate slices; this section supersedes
their mutually-exclusive-rendering and missing-HDRI/sun statements.

2026-09-08 interaction follow-up: Deselect clears the current part and exits
isolation without closing the document. List text hit areas now match the 28px
thumbnail height; thumbnails select on click and focus/isolate on double-click,
including all pieces using a material. The material-options title is a drag grip;
the panel remains constrained to the preview and supports collapse and close.
The Material options toolbar button reopens/expands it. Resize edges remain
available while expanded. Tests cover material isolation and deselection lifetime.
Both configurations built successfully; the Debug document tests and all 77
Release tests passed. A 120-frame Carrera material-viewer run exited 0 with
validation enabled and no engine validation errors (the loader skipped the
incompatible third-party Overwolf layer). Pointer interaction and screenshots
still need visual qualification; no performance/VRAM comparison is claimed.
No accepted ADR or milestone acceptance status changed in this UI slice.

Preview-lighting follow-up (2026-09-08): Scene options now opens a movable,
collapsible, closable preview drawer. HDRI rotation, HDRI lighting EV, background
EV and final-output exposure are document-local, immediately applied, and have
independent per-gesture undo/redo and reset. EV converts to linear intensity with
exp2; exposure is not also multiplied into environment radiance. Scene environment
settings and output exposure are restored when returning to the scene; no scene
component or saved project exposure is edited. Output metadata is only updated
when exposure changes, not repeatedly on unchanged frames. The current preview
still uses the scene's environment asset and direct lights/reflection probes.
This does not implement independent simultaneous view rendering.
Verification: both builds passed, Debug document tests passed, and Release passed
77/77. A validation-enabled Carrera material viewer completed 120 frames and
exited 0, with only the previously noted third-party loader warning. Tests cover
EV-to-linear conversion and independent preview values. Interactive scene-return,
exposure/rotation screenshots, and frame-cost/VRAM comparisons remain unqualified.

The horizontal project-HDRI picker and viewer-only sun direction/color are now
implemented as described above. These controls must not edit scene
components or replace the scene's environment publication. Add explicit wireframe,
unlit neutral, lit neutral and lit material display modes; do not substitute
G-buffer debug views for true material-free geometry rendering. Qualify these
with the independent-view resource/state work below, including environment
preparation failures, switching/closing, and restoration of scene render state.

The material/primitive parameter drawer now occupies a separate scrollable panel
anchored at the bottom-right of the preview. Drag its top/left edges to resize;
drag the list's right edge to resize that panel. List groups are collapsible and
retain state per open document. Model documents start fully collapsed; material
documents expand Materials; primitive documents expand the relevant opaque/clip
or transparent group once the model is resident. Selected-item scrolling also
waits for residency rather than losing its first-open request while loading.

The orbit camera now explicitly uses right-handed Vulkan zero-to-one projection,
instead of GLM's configuration-dependent default. Frame previously selected a
large near plane, then the first dolly reduced it, explaining a discontinuity
between Frame/Show whole model and subsequent wheel input. Framing and dolly now
use consistent small, clearance-aware near planes. Dolly approaches the ray exit
of the framed AABB exponentially, slowing as clearance shrinks; orbit and pan
keep the eye outside that bound too. This is a conservative bounds guard, not
triangle-level collision: concave surfaces and empty corners may stop zoom early;
isolating a primitive gives tighter bounds for close inspection. It is not proof
that all Porsche missing-surface/transparent-ordering artifacts are fixed.

Tests assert near maps to NDC 0, far to 1, all framed corners stay inside depth
range, progressive dolly slows and remains outside a cube, and orbit preserves
that clearance. Debug/Release targeted tests and builds pass.
The full suites also pass 77/77 in both configurations. A 120-frame 930 viewer
run with Vulkan validation exits cleanly; its final-SDR capture shows intact front
bodywork at the tested framing, but does not reproduce the owner's exact angle.
That inspection also exposed the default orbit pitch placing the eye below the
target; the default pitch is now negative so a new view starts above the model.
No matched performance/VRAM comparison or full UI screenshot admission is claimed.

## Private parameter previews (2026-09-05)

Viewer documents now own separate settings drafts, independent of the Asset
Browser selection. A Revert button reloads published settings. Closing discards
the document's unapplied edits. Divergent published settings no longer silently
erase a dirty draft; the existing optimistic settings transaction rejects stale
Apply requests. Apply still performs the durable source-sidecar/reimport transaction.

The thumbnail/source worker retains full SourceMaterial snapshots and their cook
revision. AssetManager retains material/primitive metadata and resident texture
bindings, not duplicate vertex/index payloads. Pending preview requests are consumed
before beginFrame. They compile source-copy patches, remap texture operations by
semantic, pack through the canonical material runtime, and publish private material
bindings on a geometry-sharing preview model. Compatible parameter edits update
only changed packed records; shader/pipeline changes allocate replacement private
bindings. Shared scene models/materials are never edited by the preview request.
Closing retires private handles, and reimport invalidates the old preview until a
matching source revision is available. Compilation failures retain the last valid
preview and expose a diagnostic.

This is **not yet full live authoring**: coverage/transport-class changes and new
texture bindings explicitly require Apply/reimport. Route-stable policy quality,
priority and thin-sheet thickness now preview privately with material defaults and
primitive override precedence. CPU source compilation/packing is asynchronous and bounded to
resident material metadata; GPU pipeline allocation remains on the renderer thread.
Draft gesture undo/redo and published settings undo/redo are available. Independent scene-under-viewer
rendering, texture assignment and persistent material instances remain open.

The parameter schema additionally exposes supported sheen, anisotropy, iridescence,
dispersion, diffuse transmission and imported specular/glossiness factors. Workflow
controls hide irrelevant metallic/roughness versus specular/glossiness fields.
Reopening an already-open primitive now requests fresh framing independently of
tab activation, and the parameter panel stays within small preview bounds.

Tests cover source-copy isolation, inherited extension preservation, texture
identity/normal-scale preservation and explicit reframe revisions. A Debug Carrera
command-line viewer attempt hit the existing 30-second preparation timeout before
rendering; that is not visual admission. See subsequent verification notes below.

Follow-up verification (2026-09-07): Release 930 model and Carrera material viewer
runs completed 120 and 180 frames respectively with Vulkan validation enabled and
exit code 0; neither reported validation errors. The earlier Debug preparation
timeout did not reproduce in these Release runs. These are launch checks, not
interactive slider/Apply/Revert image comparisons. Both configurations build and
their full 77-test suites pass. Explicit document session identity now prevents
restoring a discarded draft after closing all viewers and reopening the same GUID.
Preview route checks compare against the published engine override layer, not
only the untouched external source, so already-applied material edits remain a
valid baseline for subsequent private previews. No M7 acceptance/ADR status or
performance/VRAM admission changes are claimed.

## Implemented

- Material Asset Details and selected viewer rows share a sparse parameter drawer.
  Base color/opacity, metallic, roughness, emissive color/strength, IOR, specular
  factor/color, transmission, volume thickness/attenuation, clearcoat, normal/AO
  texture strengths, alpha coverage/cutoff and double-sidedness are editable.
  Normal/AO strengths appear only when the imported material has those textures.
- Each field has an explicit override checkbox; uncheck to inherit the source.
  Reset-all removes the material patch. This is shared-material scope, not a
  per-entity parameter instance. Existing entity material-slot assignments remain.
- `Apply and reimport` uses the existing transactional settings cook, undo/redo,
  source-sidecar persistence and last-good runtime publication path. It is not an
  immediate slider preview and may recook geometry. The active/pending cook and
  requested/resolved policy feedback remain available in Asset Details.
- Importer 8 accepts optional GUID-keyed `material_overrides`. Each entry is
  `{ "schema_version": 1, "values": { "/material/property/path": value } }`.
  No patch means no change to historical normalized settings. The frozen version-7
  importer rejects this new setting and the UI disables material authoring there.
- Source input is preserved; edits are applied to a copy of the material ingestion
  document before recompilation. Alpha coverage, double-sidedness, closure routing
  and texture recipes consume the effective compiled material. Invalid edits fail
  the cook. Missing material GUIDs produce an orphan diagnostic, retaining edits
  without assigning them to a different material. Identity guarantees still depend
  on preserved sidecars and the importer's existing subasset reconciliation.
- Viewer orbit/pan input is owned by its image interaction item. Only title bars
  move windows. The viewer is non-dockable and has a default floating size.
- Primitive thumbnail double-click opens a parent-backed document, isolates all
  runtime pieces of that source primitive and frames their combined bounds.
- The viewer has grouped material, opaque/clip primitive, and transparent primitive
  lists, selected details, material names, primitive IDs and piece triangle counts.
  Show whole model restores context and frames it. Selected and hovered parts use
  the existing selection outline path; isolation suppresses both outlines. Opening
  a primitive scrolls its selected row into the center. Material documents now
  retain original model material assignments rather than painting the whole model
  with the chosen material. Material graph UI is not implemented.
- The subcar has a Close subcar button. Root geometry-import controls and root
  dependency lists are hidden on subassets. Material defaults and primitive
  transport overrides remain distinct: ADR-0012 explicitly supports both.

## Required remaining work

### Owner-approved background view cadence (2026-09-07)

Use 30 FPS by default for the visible, unfocused view, with a 60 FPS option.
The focused view follows the normal renderer/display frame budget. If neither
view is focused, both may run at their background cadence; hidden/minimized views
should not render. This is a per-view render policy, not a sleep in the application
loop: UI input, scene simulation, asset workers and publication must remain responsive.
Each view retains its last completed image between scheduled renders. Focus gains,
resizes, camera framing, material publication and explicit invalidation request an
immediate render. Do not accumulate catch-up frames after stalls.

`EditorViewCadence` is now wired to serial view submissions with explicit successful
render acknowledgement and independent retained outputs. The latest owner request
fixes background rendering to 30 FPS rather than exposing the earlier 30/60 choice.
See the integrated-viewer section for shared transient resources and conservative
history invalidation. Persistent separate temporal histories still need a later
renderer extension; no camera uniforms are overwritten within one submission.

Viewer draft history now coalesces slider/color-picker gestures into local undo/redo
entries (bounded to 128), independently of the published settings history. Revert
resets the draft history; source publication establishes a new history baseline.
These commands do not enqueue reimports and feed the existing private preview path.
Each individual control activation/drag is a separate entry, including repeated
drags of the same slider. History tracks the active control identity and activation
boundary, not just whether any UI item is active. Moving directly from roughness
to metallic must not combine their changes. Undo and redo traverse the same single
gesture boundaries; regression tests cover this without requiring an idle frame
between edits, and ensure unrelated UI focus creates no extra history entry.

### Remaining implementation

Async-preview follow-up: one reusable CPU worker compiles immutable material/source
snapshots. It permits one in-flight job, while newer document requests coalesce on
the main thread. Request serials increase across edits and source revisions; a
completion publishes only if it still matches the live document's latest serial
and matching cook revision. Closed documents and obsolete edits therefore cannot
publish stale texture handles or resurrect a discarded draft. Closing does not
join the worker. Shutdown joins before destroying its synchronization state.
Failures remain diagnostics alongside the last valid preview. Tests cover off-thread
execution, bounded submission, non-waiting polling, failure delivery and recovery.
This does not implement topology-changing live transport or asynchronous Vulkan
pipeline creation, and no measured latency improvement is claimed yet.

Route-stable policy preview follow-up: policy quality, priority and thin-sheet
thickness are resolved using source -> material override -> source-primitive override
precedence. Existing cooked requested/resolved classes and topology/fallback flags
are retained; a class change is rejected with an Apply/reimport diagnostic. Private
draw-packet policy metadata is rebound together with private material records,
including when variant ordering/count changes. Ordinary scalar edits avoid copying
the preview model when its primitive policies are unchanged. The parameter panel
shows the actual published private-preview policy separately from the cooked shared
result, and reports mixed policies across pieces. Tests cover primitive precedence,
inheritance after removing an override, topology fallback preservation and rejection
of class changes. Interactive quality-tier switching still needs GPU/visual admission.

Async/policy follow-up verification (2026-09-07): both Debug and Release builds
passed, and the full Release suite passed 77/77. The Carrera material viewer ran
600 frames with validation enabled and exited 0, with no reported engine Vulkan
validation errors. The loader skipped an incompatible third-party Overwolf layer.
This checks startup/render-loop stability, not interactive slider/policy fidelity
or performance acceptance; no before/after GPU timing or VRAM comparison was made.

Viewer-list follow-up: rows now use catalog display names and available thumbnails,
with independently collapsible metadata beneath each row. Primitive totals and
bounds aggregate every runtime piece belonging to the source primitive; this UI
aggregation does not merge geometry. Material rows report their used runtime pieces
and triangles. Mixed-policy source primitives appear once in the transparent group.
The viewer has its own thumbnail demand lane. Browser, Inspector and viewer demand
are unioned per source root rather than replacing each other's requested subassets;
closing/collapsing the viewer releases only its own requests. Unit tests cover row
aggregation and independent thumbnail demand lifetime.

1. **Independent viewer qualification:** serial live views and separate retained
   outputs now render the scene underneath the viewer. Complete mixed-transparency,
   HDR, resizing/closing and GPU-safe retirement comparisons; quantify view-switch
   shadow/history rework and output copy cost. Do not claim independent persistent
   temporal histories: shared histories are conservatively invalidated on switches.
2. **Complete immediate material preview:** private compatible parameter publication
   and coalesced draft undo are implemented. CPU compilation is asynchronous. Finish
   structural pipeline preparation,
   topology-aware coverage/transport changes, and qualification of failure/reimport
   lifecycle. Retain Apply/Save as durable boundaries.
3. **Texture authoring:** stable texture asset/view assignment, compatible semantic
   and channel selection, UV/sampler controls, dependency recook and clear errors.
   Current texture support is strength editing only, not reassignment.
4. **Material instances and scope:** persistent parent material references and
   per-field inheritance; explicit shared asset / independent instance / entity
   assignment UI. Rebase inherited values on parent change, preserve overrides,
   handle missing slots and parent cycles, and test save/reopen and reimport.
5. **Viewer polish/verification:** collapsible rows with thumbnails and catalog
   names and distinct hover tint/selection outlines are implemented. Complete
   interactive pointer/keyboard/drag/drop and overlapping-surface feedback
   regression coverage. The current
   parameter drafts are now document-local; the thumbnail detail worker still has
   one active detail demand, prioritized to the viewer.
6. **Full feature coverage:** extended scalar/lobe controls are now exposed. Finish
   explicit unavailable-field handling, texture controls, cross-field validation
   presentation and human-readable conversion warnings.
7. **Graph-compatible output adjustment:** the sparse source-factor patches are
   not yet arbitrary evaluated-graph output modifiers. Define typed replace versus
   multiply/add semantics and a graph-independent material authoring IR before
   exposing the future graph editor. Do not conflate material outputs with the
   display/HDR output transform.
8. **Qualification:** representative 4K shared/instance edits, imported car
   source changes, missing materials, texture reassignment, compare images and
   CPU/GPU/upload/VRAM costs. The outstanding Porsche mixed-class compositing bug
   is independent and remains open.

## Verification for this slice

Debug and Release full builds and 77/77 tests passed. Added cooked material-override
routing, scalar/vector validation, source inheritance and parent-backed primitive
isolation tests. The GUI received a 120-frame Release validation-enabled launch
with the Carrera material GUID; it exited 0 with no reported Vulkan errors.
This is launch evidence, not screenshot-based visual acceptance or proof of the
full interactive workflow. No before/after performance or VRAM claim is made.
No shader/RHI ABI change or independent secondary-view allocation was introduced.

New primary interfaces: `MaterialAuthoringPatch.h`,
`importGltfSourceMaterialsJson`, `MaterialParameterEditor.h`, thumbnail-source
parameter snapshots, `EditorAssetDocument::selectedPart/isolateSelectedPart`, and
`selectPreviewPart`. Existing browser transactions, importer cooking, viewer and
Application selection extraction are the integration points.

Architecture follows ADR-0001 source-to-closure compilation, ADR-0004 stable
identity and editor separation, and ADR-0012 per-material/per-primitive transport
policy. No accepted ADR is superseded. Reference workflows: [Unreal parameter
overrides](https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-and-using-material-instances-in-unreal-engine)
and [Unity shared materials](https://docs.unity3d.com/ScriptReference/Renderer-sharedMaterial.html).
