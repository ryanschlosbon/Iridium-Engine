# M1 Execution Plan: Render Graph, Scene-Linear HDR, and Color Management

- Roadmap milestone: M1
- Status: Accepted on 2026-07-22
- Lead: milestone lead task; one owner for shared renderer/RHI/Vulkan integration
- Last updated: 2026-07-22
- Dependencies: M0 accepted by `M0-acceptance-report-2026-07-18.md`
- Relevant ADRs: ADR-0002 (primary), ADR-0001 material closures, ADR-0005 transparency
- Performance contract: `docs/performance/FRAME_BUDGET.md`

## Objective and user-visible outcome

Iridium will render opaque lighting, sky/environment, emissive, and current forward
transparency into one scene-linear high-dynamic-range image. Exposure, tone mapping,
gamut mapping, display transfer, and color-managed UI will occur through one explicit
final-output path. The same frame will execute from backend-neutral render-graph
declarations whose Vulkan executor owns resource lifetime, barriers, and reuse.

At acceptance, the reference PC must reproduce the M0 fixtures through SDR and at
least one validated HDR transport, capture scene-linear and final-output artifacts as
distinct color domains, and show measured CPU, GPU, and memory costs.

## Current context

Repository source is authoritative and must be reinspected before each slice. The
accepted M0 state currently has these relevant facts:

- `VulkanVertexBackend` manually sequences GBuffer, lighting, two-bucket legacy
  transparency, capture, UI, submit, and present.
- `VulkanFrameTargets` allocates every intermediate per swapchain image. At 4K and
  three images, each `RGBA16F` GBuffer category requests 199,065,600 bytes; each
  4-byte scene/depth/swapchain-like category requests 99,532,800 bytes.
- Static fixtures use about 949.220 MiB requested and 956.253 MiB committed engine
  memory. Three high-precision GBuffer targets dominate the footprint.
- `litScene` and `opaqueCopy` use the swapchain format rather than a scene-linear HDR
  format. `lighting.frag` applies `ACESFilm` before transparency; the environment is
  also mapped inside lighting. `forward.frag` samples this display-referred copy and
  separately maps its environment reflection.
- The final M0 4K proxy GPU medians are 0.280-0.426 ms. Nested transparency reports
  0.247839 fullscreen-equivalent fragment work. These scenes are attribution cases,
  not dressed-gameplay evidence.
- Release steady-state C++ allocations are normally zero. A graph implementation
  that allocates or recompiles topology every frame would be a measurable regression.
- M0 captures are bit-exact 8-bit legacy display-referred artifacts. They are valid
  before images but cannot serve as scene-linear references.
- The active reference output is native 3840x2160,
  `VK_FORMAT_B8G8R8A8_SRGB`, sRGB nonlinear, Mailbox, no reconstruction. The RTX
  4090/driver exposes Vulkan 1.4.341; platform capability checks still must govern
  HDR transports.

## Invariants

- The render graph is backend-neutral at its declaration/compiled-plan boundary.
  Vulkan handles, layouts, stages, access masks, barriers, query pools, and swapchain
  negotiation remain in the Vulkan executor/backend.
- Lighting and transparency consume scene-linear values. No display transfer or
  tone mapper may feed another lighting pass.
- There is exactly one final output transform per view. Capture and UI paths must
  declare whether their values are scene-linear, display-linear, or encoded output.
- The graph must not introduce a normal-frame global stall, query wait, device idle,
  or unbounded allocation. Resize/output-mode changes may use a labelled rebuild
  stall.
- Transient graph images are owned by frame contexts, not multiplied by swapchain
  image count without a proven lifetime requirement.
- Existing M0 fixture identity, camera state, provenance, profiler names, and capture
  compatibility remain stable. Intentional image changes require new references and
  interpretation, never silent replacement.
- UI color constants are interpreted as sRGB display colors and composed through a
  documented paper-white policy; they are not treated as scene radiance.
- The high-precision GBuffer remains the M2 reference. M1 does not opportunistically
  repack material data.

## Scope

- Pure backend-neutral graph resource, pass, usage, lifetime, and compiled-plan
  contracts with deterministic cycle/error diagnostics.
- Vulkan execution of compiled ordering, state transitions, and frame-context
  physical-resource reuse.
- Transient, persistent, history, imported, and exported resource classes. History
  invalidation is implemented even though M1 does not add a temporal algorithm.
- Migration of the current raster passes to graph execution with a temporary legacy
  fallback during integration.
- `RGBA16F` scene-linear color and scene copy; current transparency samples and
  writes the HDR domain before output conversion.
- User-adjustable manual EV exposure and a stable exposure resource/interface for
  later automatic exposure.
- One configurable final-output contract with explicit working-to-display gamut
  conversion, tone mapping, transfer, and output metadata. ACES 2 is the production
  operator; the existing fitted ACES curve remains a selectable compatibility mode.
- Color-managed UI composition.
- Capability-negotiated scRGB and HDR10/PQ paths, including HDR metadata where the
  platform and `VK_EXT_hdr_metadata` support it.
- Separate high-precision scene-linear capture plus final-output capture and
  metadata-aware comparison.
- A color-volume/output fixture containing grayscale/near-black ramps, saturated
  source/display primaries, out-of-gamut colors, emissive highlights above white,
  transparent highlights, and UI patches.

## Non-goals

- GBuffer packing or the M2 material compiler/BSDF rewrite.
- Correct general nested transparency, rough-refraction pyramids, or OIT; M6 owns
  those algorithms. M1 only moves the existing classified work into HDR composition.
- Production lights, shadows, IBL/probes, baking, or GI; M5/M10 own those systems.
- Bloom implementation. M1 declares the scene-linear hook/resource contract and
  proves no output transform occurs before it.
- Temporal AA, reconstruction, motion vectors, dynamic resolution, or automatic
  history consumers; M9 owns them.
- Async compute scheduling. Queue ownership is represented, but M1 executes the
  current graphics-queue workload.
- Asset/texture color-management metadata beyond the conversions needed for the M1
  fixture and current sRGB/linear inputs; M2/M3 expand authoring/import policy.
- Shipping or certifying Display-P3 or cinema DCI-P3 output in M1. Output-profile
  descriptors must nevertheless carry primaries, white point, transfer, and luminance
  explicitly so either P3 target can be added later without changing scene shading or
  graph contracts.

## Owner decisions resolved on 2026-07-19

The owner delegated the working-space choice to the lead, requested a configurable
production output operator while retaining the existing ACES curve, approved
user-adjustable HDR defaults, and accepted manual exposure for M1. These decisions
close the four product choices. They did not by themselves authorize source
implementation; the owner subsequently approved this execution plan on 2026-07-19.

1. **Scene working gamut: linear ACEScg/AP1.** Standard Windows SDR remains the
   initial/default output through an explicit AP1-to-Rec.709/sRGB transform. Source
   sRGB/Rec.709 values are decoded and transformed into AP1 before lighting. The
   output-profile contract is not tied to sRGB: Display-P3 (D65), cinema DCI-P3, and
   Rec.2020/HDR targets remain explicit future profiles rather than being conflated.
2. **Production output operator: configurable ACES 2.** The production path uses the
   documented ACES 2 Output Transform, split at its rendering/display-encoding
   boundary so display-linear UI can be inserted correctly. It accepts target gamut,
   white point, peak luminance, and viewing/output parameters. The current small
   `ACESFilm` fitted curve remains selectable as `AcesFittedLegacy`; an identity/clamp
   path is diagnostic only. Additional operators can be registered without changing
   graph resources or lighting shaders.
3. **HDR order and defaults: scRGB, then HDR10/PQ.** UI paper white defaults to 203
   nits and target/mastering peak defaults to 1,000 nits. Both are user-editable,
   persisted settings, constrained to valid positive values and the selected
   transport/display capability. Changing them updates output constants and HDR
   metadata without changing the scene-linear image.
4. **Exposure scope: manual EV in M1.** Manual exposure is user-editable, persisted,
   serialized in captures, and represented through the history-capable interface.
   Histogram metering and temporal adaptation remain M9 work.

The ACES 2 architecture follows the Academy's documented separation between the
rendering transform and display encoding:
`https://docs.acescentral.com/system-components/output-transforms/`. The scRGB-first
choice follows the Windows Advanced Color recommendation for general-purpose
composited applications:
`https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range`.

No product choice is presently blocked on owner input. Graph ownership, resource
lifetime, `RGBA16F` scene color, final-transform ordering, and separate capture
domains are resolved by ADR-0002 plus M0 evidence. M1 execution approval was granted
on 2026-07-19.

## Design and data flow

### Graph ownership and compilation

Add a renderer graph module, independent of Vulkan, with opaque generation-checked
resource/pass handles and immutable descriptors. A builder declares resources and
passes; each pass declares reads/writes, usage class, load/store intent, and queue
class. The compiler validates handles, single-writer/ordering rules, cycles,
read-before-write, incompatible imports/exports, and history use. It produces:

- stable topological pass order;
- logical-resource first/last use;
- abstract transitions and queue ownership intents;
- compatible physical-image reuse slots using the union of declared usages;
- imported initial and exported final states;
- persistent/history allocation and invalidation records;
- diagnostic names and memory-lifetime reports.

Graph topology is cached by a hash of pass/resource descriptors, output mode,
resolution, and quality. Per-frame execution updates only external handles, dynamic
constants, and work spans. No normal frame rebuilds topology or allocates containers.

Pass recording remains backend-owned in M1: compiled pass IDs dispatch registered
Vulkan recording functions. This avoids leaking a `VkCommandBuffer` through the RHI
while the engine still lacks a complete backend-neutral command-list API. A later RHI
may generalize recording without changing declarations or the compiler.

Every logical write creates a new generation-checked resource version with one
producer. A load-preserving attachment write explicitly consumes the prior version;
the compiler orders all prior consumers before the new producer. This makes
multi-pass attachment updates unambiguous without exposing backend barriers or
allowing multiple writers to one version. Dynamic history validity is tracked outside
the topology hash so resize/camera-cut invalidation cannot force per-frame compile.

### Vulkan execution and physical resources

`VulkanRenderGraphExecutor` maps abstract usages to Vulkan image/buffer state and
records barriers immediately before the consuming pass. It uses the existing frame
fences and command buffers; it never waits for graph resources inside a normal frame.

The transient pool reuses one physical `VkImage` for compatible logical images with
nonoverlapping lifetimes, created with the union usage flags. This is physical-image
reuse, not unsafe simultaneous aliasing. Persistent resources survive graph rebuilds;
history resources are frame-indexed and expose explicit valid/invalid state; imported
resources carry caller-supplied initial/final states; exported resources keep their
physical allocation until the external consumer releases or the frame fence retires.

M0 memory data resolves the first allocation policy: raster intermediates use the
two frames-in-flight, not the three swapchain images. Merely moving the three
GBuffer and two depth families from three to two copies offsets the cost of moving
scene color/copy from 4 bytes to 8 bytes per pixel. Reusing the HDR scene-copy slot
with a dead compatible `RGBA16F` GBuffer image is a measurable further opportunity,
subject to declared lifetimes and image-usage compatibility.

### Color pipeline

The scene pipeline is:

```text
sRGB/linear source inputs
        -> ACEScg/AP1 scene-linear GBuffer and lighting
        -> scene-linear emissive/environment
        -> scene-linear transparency/refraction composition
        -> optional bloom input/output hook
        -> exposure
        -> ACES 2 rendering/gamut transform or selected compatibility operator
        -> display-linear UI composition at configured paper white
        -> SDR sRGB encode, scRGB linear scale, or HDR10 PQ encode
        -> swapchain/present and final-output capture
```

The scene target and transparency copy use `VK_FORMAT_R16G16B16A16_SFLOAT` and carry
linear ACEScg/AP1 values. Intermediate shaders may contain no output transfer or
display tone mapping. Exposure is expressed in stops/EV as a named user setting and
serialized in captures.

`OutputTransformConfig` selects the operator and records target primaries, white
point, transfer function, paper white, peak luminance, exposure, and operator-specific
parameters. The production ACES 2 path converts AP1 through its specified input and
emits display-linear target-gamut values before UI; display encoding remains a
separate final step. `AcesFittedLegacy` obeys the same display-linear contract so
operator selection cannot change pass ordering or feed display values back into the
scene.

SDR writes display-linear Rec.709/sRGB values to an sRGB swapchain attachment so the
fixed-function conversion performs the final sRGB encode exactly once. scRGB uses an
FP16 extended-linear-sRGB swapchain and a documented nits-to-scRGB scale. HDR10 uses
a 10-bit swapchain/color space when supported and writes PQ-encoded Rec.2020 values;
the backend sets mastering/content-light metadata only when the extension is active.

UI is built in its normal sRGB authoring space, decoded to display-linear, scaled to
paper white, and composed after scene tone/gamut mapping but before the selected
output encoding. Scene exposure never changes UI brightness.

### Capture and failure behavior

M1 adds a diagnostic RGB floating-point PFM scene-linear artifact with a JSON
sidecar, while retaining TGA for 8-bit final SDR. HDR final captures store either a
linear reference or decoded luminance-domain comparison artifact plus exact transport
metadata; raw swapchain code values are optional diagnostic data, not the sole image
quality oracle.

Unsupported HDR formats/color spaces/extensions fall back explicitly to SDR and
serialize the rejected capability/reason. A graph compile failure aborts before
recording and reports the offending pass/resource. During integration, a runtime
configuration switch retains the accepted legacy manual path. The fallback may be
removed only after graph parity and the final M1 gate.

## Vertical slices

### M1.0 - Decision recording, approval, and preflight

Status: `Accepted` on 2026-07-19. The durable capability inventory, frozen ACES
reference/version, parameter bounds, output profiles, and discovered M1.6 prerequisites
are recorded in `docs/milestones/M1.0-color-and-output-preflight.md`.

- Preconditions: the four owner choices above are closed; final M0 report is intact;
  the owner explicitly approves this execution plan before source work begins.
- Work: retain the decisions in this plan and ADR-0002. Reinspect swapchain/HDR
  capabilities and target-display settings. Freeze `OutputTransformConfig` parameter
  bounds and the exact ACES 2 reference/version used by tests.
- Verification: emit a read-only capability inventory; confirm M0 Release artifacts
  still identify the accepted baseline.
- Completion: no open aesthetic/output decision can silently change shader math, and
  the approved parameter/version contract is sufficient for deterministic tests.

### M1.1 - Backend-neutral graph contract and compiler

Status: `Accepted` on 2026-07-19. Contracts, tests, benchmark evidence, allocation
scope, and remaining executor risks are recorded in
`docs/milestones/M1.1-render-graph-contract-and-compiler.md`.

- Likely systems: new `src/renderer/graph/` module and isolated graph tests. Do not
  change `IRenderBackend`, Vulkan frame lifecycle, or shaders in this slice.
- Behavior: generation-checked handles, resource classes, pass usages, deterministic
  compile/order/lifetime/reuse plan, cycle and invalid-use diagnostics, topology hash.
- Tests: DAG ordering, stable ties, cycles, read-before-write, imported/exported
  states, history invalidation, nonoverlap reuse, incompatible descriptors, stale
  handles, and fixed-capacity/repeated compile behavior.
- Performance: 1,000-pass synthetic compile benchmark; cached per-frame lookup must
  allocate zero bytes and cost below 0.01 ms median.
- Fallback: module is unused by the renderer.
- Completion: pure unit tests and both builds pass; public contracts are reviewed and
  frozen before Vulkan work begins.

### M1.2 - Vulkan executor in shadow mode

Status: `Accepted` on 2026-07-19. The opt-in Vulkan shadow executor, corrected
nine-pass legacy topology, state mapping, fence-scoped pool, validation/image gates,
and exact performance/memory evidence are recorded in
`docs/milestones/M1.2-vulkan-executor-shadow-mode.md`.

- Likely systems: `VulkanRenderGraphExecutor`, state mapping, frame-context transient
  pool, allocator categories, and backend capability reporting. One owner controls
  scheduler/allocator/executor integration.
- Behavior: compile the current fixed topology, create frame-owned physical resources,
  generate barrier plans and memory reports, but continue recording through the
  legacy path. Compare generated intent with observed legacy transitions.
- Tests: abstract-to-Vulkan state mapping, resource-slot retirement after fences,
  resize/rebuild, imported swapchain state, allocation failure cleanup.
- Runtime: Debug validation at 4K, zero new warnings; graph shadow build occurs only
  on topology/resolution/output change.
- Performance/memory: no normal-frame allocations; shadow CPU delta below 0.05 ms or
  1%, whichever is larger. No duplicate live render-target set beyond the explicitly
  measured integration window.
- Fallback: disable shadow graph without changing images.
- Completion: shadow plan and allocator accounting are deterministic and match the
  current pass/resource dependencies.

### M1.3 - Graph-executed SDR parity and transient ownership

Status: **Accepted** on 2026-07-19. Evidence:
`docs/milestones/M1.3-graph-executed-sdr-parity.md`.

- Behavior: graph executor owns current GBuffer, lighting, transparency, capture,
  UI, and present ordering plus transitions. Existing pass recorders are split into
  backend-owned callbacks without changing shader semantics. Targets move from
  swapchain-image ownership to frame-context graph ownership where lifetimes allow.
- Tests: pass-order/counter names, frame IDs, resize, out-of-date/suboptimal recovery,
  capture retirement, descriptor selection, and fallback selection.
- Images: M0 opaque/final compatibility mode targets maximum code delta 1, changed
  fraction below 0.1%, and luma SSIM at least 0.9999; any larger delta requires a
  localized explanation. Exact equality remains preferred.
- Performance: CPU median delta below 0.05 ms/1%; GPU median delta below 0.05 ms/2%.
- Memory: committed/requested live memory must not exceed M0 by more than 32 MiB after
  removing temporary shadow allocations; report transient peak and reuse efficiency.
- Fallback: legacy manual path remains selectable.
- Completion: graph mode is validation-clean and stable across every M0 fixture.

### M1.4 - Scene-linear HDR and transparent composition

Status: **Accepted** on 2026-07-19. Consolidated evidence:
`docs/milestones/M1.4-scene-linear-hdr-acceptance.md`. The first sub-slice adds the scene-linear PFM
capture/domain contract without changing rendering. Evidence:
`docs/milestones/M1.4a-scene-linear-capture-contract.md`. The second freezes the CPU
source-transfer and Rec.709/sRGB-to-AP1 reference math:
`docs/milestones/M1.4b-scene-color-reference-math.md`. The third migrates scene and
copy storage to FP16 with measured transient reuse:
`docs/milestones/M1.4c-fp16-scene-target.md`. The fourth installs the explicit
compatibility-output pass before UI/present:
`docs/milestones/M1.4d-explicit-compatibility-output-pass.md`.

- Behavior: `RGBA16F` scene color/copy, lighting/environment/emissive output in linear
  ACEScg/AP1, removal of in-pass output mapping, HDR sampling/composition
  in current transparency, bloom hook, and PFM scene-linear capture.
- Tests: CPU shader-reference math for input transfer/gamut matrices; shader tests for
  no hidden output operator; half-float capture round trip; exposure-independent scene
  capture; monotonic emissive values above 1.0; transparent highlights retain range.
- Images: new color-volume fixture and existing opaque-emissive/nested-transparency
  captures. Old final images are interpretation references, not strict targets after
  intentional correction.
- Performance: lighting plus transparency remains within its existing proxy cost plus
  0.10 ms median; current transparency remains below the 1.0 ms ordinary-scene budget.
- Memory: target at or below the M0 956.253 MiB committed scope by frame ownership and
  compatible transient reuse; any increase requires a category/lifetime proof.
- Fallback: compatibility output can map HDR scene through the legacy final curve;
  legacy pre-transparent mapping is not accepted as the new path.
- Completion: captures prove opaque, emissive, environment, and transparency coexist
  in one unclipped scene-linear domain before output.

### M1.5 - Final SDR transform, exposure, gamut map, and UI

Status: **Accepted**. M1.5a accepted the versioned backend-neutral output
configuration, profile derivation, frozen bounds, and persisted-data fallback.
M1.5b accepted bounded manual EV at the sole final-output boundary and mechanically
proved exposure-independent scene capture. M1.5c accepted the BGRA8 sRGB output
target, explicit output/UI split, optional final-capture graph hook, and versioned
direct final-SDR artifact. M1.5d accepted the pinned production ACES 2 output LUT,
Academy vectors and provenance, selectable legacy/diagnostic branches, exact output
metadata, and measured cost. M1.5e accepted display-linear ImGui composition and
external final-present proof of exposure-independent UI. The final acceptance record
also closes the frozen comprehensive color-volume fixture. Evidence:
`docs/milestones/M1.5a-output-configuration-contract.md` and
`docs/milestones/M1.5b-manual-exposure-output-boundary.md` and
`docs/milestones/M1.5c-direct-final-sdr-capture.md` and
`docs/milestones/M1.5d-production-aces2-output.md`,
`docs/milestones/M1.5e-color-managed-ui.md`, and
`docs/milestones/M1.5-final-sdr-acceptance.md`.

- Behavior: configurable ACES 2 production output, selectable `AcesFittedLegacy`,
  explicit user-adjustable manual exposure, AP1-to-Rec.709 gamut mapping, SDR output
  encoding, and display-linear UI at user-adjustable paper white. Remove all other
  display transforms from lighting/forward shaders.
- Tests: Academy reference vectors/version, legacy compatibility vectors, operator
  selection, reference ramps, negative/NaN/Inf handling, out-of-gamut policy,
  tone-curve monotonicity, neutral/hue preservation, user-setting persistence and
  bounds, UI exposure independence, and output metadata.
- Images: final SDR references for all M0 fixtures and color-volume fixture; PFM and
  TGA sidecars cross-reference source state and transform parameters.
- Performance: final transform at 4K below 0.25 ms median and 0.35 ms p99 on the
  reference GPU; CPU submission delta below 0.02 ms.
- Fallback: SDR is the mandatory fallback for every HDR negotiation failure.
- Completion: a single output transform is mechanically and visually proven.

### M1.6 - scRGB, HDR10/PQ, and HDR metadata

Status: **Accepted on 2026-07-22**. M1.6a accepted the exact Vulkan format/color-space
negotiation table, independent HDR-metadata capability, explicit SDR fallback, and
fatal unknown-SDR behavior without changing runtime output. Evidence:
`docs/milestones/M1.6a-output-transport-negotiation-contract.md`. M1.6b enabled and
serialized optional live output capabilities without selecting a new transport. The
reference surface exposes SDR, scRGB, HDR10/PQ, and HDR metadata. Evidence:
`docs/milestones/M1.6b-live-output-capabilities.md`. M1.6c accepted the shared pinned
ACES 2 1000-nit P3-D65-limited Rec.2100-PQ transform asset for scRGB and HDR10.
Evidence: `docs/milestones/M1.6c-aces2-hdr-transform.md`. M1.6d accepted the
independent ST.2084, Rec.2020, scRGB 80-nit scale, and UI paper-white CPU reference.
Evidence: `docs/milestones/M1.6d-hdr-reference-math.md`.
Runtime scRGB, HDR10/PQ, display-linear UI composition, HDR metadata, final-HDR
capture, validation, timing, and memory evidence is
`docs/milestones/M1.6-hdr-transports-acceptance.md`.

- Behavior: enumerate and select supported format/color-space pairs, rebuild graph
  output resources on mode change, scRGB scaling, Rec.2020/PQ encode, metadata, and
  explicit SDR fallback. Never infer HDR from format alone.
- Tests: negotiation tables, unsupported-extension paths, metadata clamping, nits/PQ
  reference vectors, mode-switch history invalidation, and sidecar compatibility.
- Visual gate: validate SDR and approved first HDR path on the reference display;
  verify grayscale, paper white, saturated colors, highlights, UI, and screenshots.
  HDR10 is also implemented/validated where the current platform exposes it.
- Performance: HDR transport/output overhead versus SDR below 0.05 ms median; combined
  exposure/output/UI remains within the 0.8 ms post/output budget.
- Memory: output-mode rebuild has no leaked swapchains/descriptors; steady HDR delta
  is fully categorized.
- Fallback: failed negotiation returns to the known SDR graph and records why.
- Completion: SDR plus at least one HDR path pass target-display validation; all
  supported paths serialize exact transport and luminance parameters.

### M1.7 - Acceptance hardening and legacy-path disposition

Status: **Accepted on 2026-07-22**. The full criterion decision and fresh-lead
handoff are `docs/milestones/M1-acceptance-report-2026-07-22.md`.

- Work: rerun every M0 fixture, color-volume fixture, five-run overhead/spread,
  Debug validation, scene/final captures, memory reconciliation, resize/mode changes,
  and external attribution if marker boundaries changed.
- Compare CPU/GPU/memory/image results with the M0 acceptance report and explain all
  intentional image changes.
- Remove the legacy manual path only if graph mode satisfies every gate and rollback
  no longer needs it; otherwise retain it as an explicitly deprecated diagnostic path
  with an owner/removal milestone.
- Completion: write the M1 completion report, update ROADMAP/project context/ADRs,
  and leave a fresh lead handoff independent of chat history.

## Delegation and integration

The milestone lead owns graph/RHI contracts, topology decisions, shared shader
interfaces, Vulkan frame lifecycle, swapchain/output negotiation, integration, and
final acceptance. These systems have one write owner at a time:

- `IRenderBackend`, RHI resource/capability types, and application render flow;
- `VulkanVertexBackend`, frame scheduler, command list, allocator, graph executor,
  frame targets, descriptors, and swapchain;
- scene-color/lighting/forward/final-output shader interfaces;
- CMake and durable roadmap/ADR/milestone documents.

After M1.1 contracts are frozen, bounded parallel work may safely cover:

- pure graph compiler unit/fuzz tests and synthetic benchmarks;
- read-only Vulkan HDR capability/platform investigation;
- CPU color-science reference vectors and fixture generation;
- PFM/capture/comparison support in disjoint files;
- image sampling/heatmap reports and performance-data analysis.

Do not run overlapping write-heavy agents in shared renderer headers or executor/
swapchain code. Every returned change is inspected and integrated by the lead; a
subagent build or summary cannot accept a slice.

## Verification and performance gates

Every source slice runs both presets and all CTests. Renderer slices additionally run
4K Debug validation and Release captures on the reference RTX 4090. Performance runs
record the exact M0 header fields, validation off, fixed camera/output/cache state,
at least 500 warm-up and 10,000 measured frames unless a fixture explicitly differs,
plus five-run spread.

Acceptance requires:

- zero Vulkan validation errors in graph execution, resize, mode switch, capture,
  and cleanup;
- no per-frame graph compile or unbounded allocation; Release steady-state C++
  allocation counters remain at the M0 zero median;
- graph-only overhead within 0.05 ms CPU and 0.05 ms GPU proxy gates;
- final 4K output pass below 0.25 ms median/0.35 ms p99 and total post/output/UI below
  the 0.8 ms budget;
- HDR output overhead below 0.05 ms median versus matched SDR;
- requested/committed/transient peak by category, lifetime, and reuse slot, with no
  unexplained increase over 949.220/956.253 MiB;
- scene-linear captures preserve values above display white and remain independent of
  output transport; final captures identify exact operator/gamut/transfer/nits;
- SDR and one target-display HDR path pass the color-volume, emissive, transparency,
  grayscale, and UI visual gate;
- marker boundaries are externally recrossed if their ownership or command scope
  changes.

## Risks, fallback, and rollback

- **Graph barrier error:** keep legacy mode during integration, validate every slice,
  and fail compilation before command recording on invalid declarations.
- **Descriptor/frame ownership mismatch:** graph exports carry frame-context lifetime;
  editor descriptors are selected after frame acquisition and retired after its fence.
- **FP16 memory/bandwidth increase:** use frame-in-flight ownership and compatible
  physical reuse; reject extra targets without category timings and image benefit.
- **HDR display variability:** retain SDR as mandatory fallback, serialize display and
  transform parameters, and separate transport validation from scene-linear truth.
- **Tone-map aesthetic or version regression:** pin the ACES 2 reference/version and
  vectors, retain `AcesFittedLegacy`, serialize the selected operator/settings, and
  require new references for intentional operator changes.
- **UI brightness/exposure coupling:** compose UI in display-linear space after scene
  tone mapping and test exposure independence numerically and visually.
- **Capture size/tooling:** keep scene-linear PFM diagnostic-only, hash artifacts, and
  avoid embedding large payloads in JSONL.
- **Scope expansion into M2/M5/M6/M9:** preserve explicit non-goals and route newly
  discovered material, lighting, transparency, or temporal work to its owning
  milestone.

## Decision log

- 2026-07-18: M0 acceptance satisfies M1's dependency but does not authorize source
  implementation.
- 2026-07-18: Graph declarations/compiler are backend-neutral; M1 pass recording
  remains Vulkan-owned to avoid inventing an incomplete generic command API.
- 2026-07-18: Graph topology is cached and normal-frame execution is allocation-free,
  based on M0's zero steady-state allocation evidence.
- 2026-07-18: Scene color and transparency copy use `RGBA16F`; frame-in-flight
  ownership and compatible physical reuse offset the current per-swapchain footprint.
- 2026-07-18: Current GBuffer precision/layout is retained until M2 supplies image and
  bandwidth evidence.
- 2026-07-18: M0 TGA artifacts remain legacy final references; M1 adds a separate PFM
  scene-linear artifact and never changes their declared domain.
- 2026-07-18: Manual exposure was the recommended M1 scope; automatic adaptation
  remained an owner choice and otherwise belonged with M9 temporal history. This
  pending item was resolved on 2026-07-19.
- 2026-07-18: Working gamut, production output operator, HDR defaults, and exposure
  scope were pending owner approval. This pending item was resolved on 2026-07-19.
- 2026-07-19: The owner delegated the working-space choice to the lead. Linear
  ACEScg/AP1 is selected while Windows SDR remains the default Rec.709/sRGB output;
  explicit output-profile descriptors preserve future Display-P3, DCI-P3, and
  Rec.2020 paths.
- 2026-07-19: ACES 2 is the configurable production Output Transform;
  `AcesFittedLegacy` remains selectable. Paper white defaults to 203 nits and peak to
  1,000 nits, both user-adjustable and persisted. M1 uses manual EV exposure; automatic
  metering/adaptation remains M9.
- 2026-07-19: These choices close M1's product-decision inputs but do not authorize
  implementation. Explicit execution-plan approval is still required.
- 2026-07-19: The owner approved M1 execution. M1.0 froze ACES release
  `v2.0.0+2025.04.04`, configuration bounds, SDR/scRGB/HDR10 profiles, and the
  reference multi-monitor validation policy. The current engine does not enable
  `VK_EXT_swapchain_colorspace` or `VK_EXT_hdr_metadata`; exact extended surface pairs
  remain a truthful M1.6 prerequisite. M1.0 is accepted and M1.1 is next.
- 2026-07-19: M1.1 accepted versioned single-writer resources, stable topology
  compilation, abstract transitions, conservative transient reuse, dynamic history
  validity, and a fixed compiled-plan cache. Debug/Release 10/10 tests pass. The
  1,000-pass Release benchmark compiled in 1.8513 ms; cached lookup measured 1.8324 ns
  with zero allocations. The module remains unused by the renderer; M1.2 is next.
- 2026-07-19: M1.2 accepted the opt-in Vulkan shadow executor. Source comparison
  corrected the topology to include one scene-color copy for each transparent layer.
  The final maximum path has nine passes, eight logical resources, seven physical
  slots per frame context, and 25 barrier intents. Debug/Release 11/11 tests pass;
  4K validation is clean and shadow/legacy captures are bit-exact. Direct Release
  lookup/validation costs 0.0002-0.0003 ms with zero median C++ allocations. The
  temporary two-frame shadow set is 663,552,000 B requested and 668,467,200 B
  committed. M1.3 must replace legacy target ownership rather than retain it.
- 2026-07-19: M1.3 accepted opt-in graph execution and two-frame-context graph
  ownership while retaining the selectable legacy fallback. Debug/Release 11/11
  tests pass and all eight 4K M0 comparisons are bit-exact. Three-run Release medians
  show -0.0077 ms CPU and -0.0029 ms GPU deltas; normal-frame allocations and graph
  cache misses are zero. Live committed memory fell by 334,233,600 B (33.33%) to
  668,470,976 B. M1.4 scene-linear HDR composition is next.
- 2026-07-19: M1.4a accepted the isolated scene-linear capture contract. Canonical
  little-endian RGB float PFM round trips negative and above-one ACEScg/AP1 values;
  sidecars identify linear transfer, AP1 primaries, unapplied exposure, and no output
  operator. Debug/Release focused tests pass and renderer behavior is unchanged.
- 2026-07-19: M1.4b froze the CPU source-color oracle: IEC sRGB transfer, linear
  Rec.709/sRGB-D65 to ACEScg/AP1-D60 with Bradford adaptation, and its inverse.
  Debug/Release reference-vector tests pass; no frame path changed.
- 2026-07-19: M1.4c accepted FP16 scene/copy targets while retaining legacy shader
  interpretation. Compatible reuse reduces seven physical slots to six, keeping
  steady committed memory unchanged at 668,470,976 B. Debug/Release 12/12 tests and
  4K graph/legacy validation pass. The 4K transparency proxy changed by +0.0159 ms
  CPU and +0.0440 ms GPU, within the M1.4 gate; FP16 removes one 8-bit intermediate
  quantization step.
- 2026-07-19: M1.4d accepted an explicit identity compatibility-output pass and made
  UI sample its output instead of scene color. The ten-pass/nine-resource graph still
  uses six physical slots and unchanged graph memory through FP16 lifetime reuse.
  Debug/Release 12/12 tests and graph/legacy 4K validation pass. The output pass costs
  0.0399 ms GPU and 0.0021-0.0022 ms CPU recording; cumulative M1.4 GPU delta remains
  within +0.10 ms.
- 2026-07-19: M1.4 accepted FP16 linear ACEScg/AP1 composition across opaque,
  environment, emissive, refraction, and transparency; the fitted curve now exists
  only in the explicit compatibility-output pass. PFM evidence proves monotonic
  emissive values to 2.0332 and transparent values to AP1 [2.8613, 1.6963, 1.1689].
  Debug/Release 12/12 tests and 4K graph/legacy validation pass. Final three-run
  medians are 0.5516 ms CPU and 0.4244 ms GPU, +0.0294/+0.0260 ms from M1.3, with
  unchanged graph memory and zero median allocations. M1.5 is next.
- 2026-07-19: M1.5a accepted `OutputTransformConfig` schema version 1 with ACES 2,
  legacy, and diagnostic operators; SDR/scRGB/HDR10 profiles; requested/effective
  luminance; manual EV and metadata fields; the frozen ACES package identity; strict
  user bounds; and diagnostic persisted-data fallback. Debug/Release focused tests
  pass; runtime rendering is unchanged.
- 2026-07-19: M1.5b accepted bounded manual EV exclusively at the final output pass;
  identical -2/+2 EV scene captures mechanically prove scene-domain independence.
  M1.5c then accepted direct pre-UI BGRA8 sRGB capture, the explicit output/UI split,
  and complete transform sidecars.
- 2026-07-19: M1.5d accepted the default pinned Academy ACES 2 100-nit Rec.709/sRGB
  output as a deterministic 128-cubed log-shaper LUT with tetrahedral sampling.
  Debug/Release 14/14 tests and three validation operator captures pass. At 4K it
  costs 0.043008 ms median / at most 0.044032 ms p99, about +0.0143 ms versus the
  matched legacy operator, and adds 33,555,968 B committed persistent texture memory.
- 2026-07-22: M1.5e and all of M1.5 were accepted. Matched Nsight -4/+4 EV captures
  retain exact 64/128/192 and RGB UI codes while the scene changes, and externally
  prove output-before-UI ordering with no validation errors. The content-locked
  color-volume fixture now covers 0.01/0.18/1/8 neutrals, saturated and gamut-stress
  primaries, above-white emissive, transparent overlap, and UI patches. Debug and
  Release pass 14/14 tests; all seven fixtures are validation-clean and have inspected
  4K ACES 2 final references. Output is 0.043008 ms median / at most 0.044032 ms p99;
  combined output plus UI is 0.144384 ms median. This closed M1.5.
- 2026-07-22: M1.6a accepted the pure Vulkan output-transport selector. Debug and
  Release focused tests cover exact SDR/scRGB/HDR10 pairs, both HDR10 packings,
  metadata independence, explicit SDR fallback, and missing-SDR failure. Runtime,
  images, timings, and memory are unchanged because swapchain wiring is M1.6b.
- 2026-07-22: M1.6b accepted live optional-extension and surface-pair discovery.
  The RTX 4090/Windows surface exposes exact SDR, scRGB, and HDR10 pairs plus
  `VK_EXT_hdr_metadata`; the capability record is validation-clean. SDR remains the
  only selected mode, so visual, timing, and memory behavior is unchanged. M1.6c is
  the first scRGB transport slice.
- 2026-07-22: M1.6c accepted the common pinned ACES 2 HDR transform. The generalized
  cooker reproduces the SDR LUT bit-for-bit; the 1000-nit P3-D65/Rec.2100-PQ LUT is
  content-locked and passes focused Debug/Release vectors with 0.000523759 p99 random
  encoded error. It is not uploaded yet, so runtime cost remains zero.
- 2026-07-22: M1.6d accepted independent HDR transport math. Debug/Release vectors
  cover ST.2084 absolute nits, Rec.2020-to-Rec.709, 80-nit scRGB scaling, negative
  wide-gamut components, and 203-nit UI white. Runtime remains unchanged. Evidence
  split the first scRGB swapchain/output integration into M1.6e.
- 2026-07-22: M1.6e/f accepted live scRGB and HDR10. HDR10 composes UI into a
  graph-owned FP16 Rec.2020 target before its sole ST.2084 pass and applies optional
  P3-D65/1000-nit metadata. All required 4K fixtures and HDR resize runs are
  validation-clean. Five-run medians put output transforms at 0.049152 ms, scRGB
  output/UI at 0.266240 ms, and HDR10 output/UI/encode at 0.205824 ms. Exact scene
  captures are transport-independent; target-window -4/+4 EV captures prove UI
  independence. Evidence: `M1.6-hdr-transports-acceptance.md`.
- 2026-07-22: M1.7 accepted M1. Debug and Release pass 15/15 tests. Worst transport
  requested peak is scRGB at 886.662 MiB, below M0. The manual path remains a
  deprecated diagnostic rollback with M2 as its removal target. Criterion-level
  evidence and hashes are `M1-acceptance-report-2026-07-22.md`.

## Completion report

Complete. The authoritative report is
`docs/milestones/M1-acceptance-report-2026-07-22.md`. A fresh lead does not need this
conversation to resume at M2 planning.
