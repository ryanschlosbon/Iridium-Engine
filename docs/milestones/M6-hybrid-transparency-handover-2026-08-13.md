# M5 to M6 Hybrid Transparency Handover — 2026-08-13

Status: durable lead handover after M5 acceptance and M5.12/M5.13 corrective
hardening. This is starting context, not an M6 execution plan or acceptance claim.

## Read order and source of truth

Before planning M6, read current source plus:

1. `docs/PROJECT_CONTEXT.md`, `ROADMAP.md`, `PLANS.md`, and
   `docs/performance/FRAME_BUDGET.md`;
2. ADR-0001, ADR-0002, ADR-0005, ADR-0006, ADR-0007, ADR-0009, and ADR-0010;
3. `docs/milestones/M2.6-complex-forward-closures.md`;
4. `docs/milestones/M5-acceptance-report-2026-08-13.md`;
5. `docs/performance/M5.11-production-qualification-2026-08-13.md`;
6. the post-acceptance M5.12 reflection and M5.13 shadow hardening records.

The numerical shadow implementation in current source and M5.13 is newer than the
original M5.6 constants written into ADR-0008. ADR-0008's stable ownership, cache
validity, no-stale-publication, and visibility-one fallback rules still apply;
ADR-0009 supersedes its one-owner decision. Do not restore the old 2048/PCF/bias
constants while working near forward transparency.

## Renderer contracts M6 inherits

- All lighting, environment, transparency, emissive, probe, and refraction work
  remains scene-linear ACEScg/AP1 until the single final output transform. M6 must
  sample HDR scene resources, never a tone-mapped swapchain image.
- The M2 packed material record and shared normal/BSDF libraries are the material
  truth. M6 may extend versioned transport/classification data, but must not create
  a private glass-only material model or duplicate GGX/Fresnel conventions.
- One M5 clustered assignment supplies deferred, opaque-complex forward, and
  transparent forward consumers. M6 must not create a second transparent-light
  list. The current transparent complex shader already consumes clustered direct
  lights, per-light scalar shadow visibility, cooked global/local-probe IBL, normal
  maps, and the shared BSDF.
- Shadow visibility is independent per light and multiplies only that light's
  radiance before accumulation. A combined global mask is invalid, including for
  transparent receivers.
- Local `+Z` is now the authored emission direction for spot and directional
  lights, matching the gizmo. Some older M5 planning text says local `-Z`; that is
  stale. Persisted Light v2 data did not change, but scenes authored around the old
  bug may need an intentional 180-degree rotation.
- Stable scene UUIDs, primitive/material GUIDs, cooked-only runtime assets, graph
  resource ownership, and resize-safe descriptor retirement are mandatory. M6
  resources and material modes belong in backend-neutral contracts; Vulkan and
  editor UI remain implementations/consumers of those contracts.

## What M5 changed in or around glass

M5 did not implement production transparency, but it did materially improve the
input lighting and the existing compatibility path:

- Complex and transparent forward shading was connected to the shared clustered
  lights, independent directional/spot/point shadows, cooked environment IBL, and
  clustered reflection probes. Reflective glass and complex car materials do not
  need a separate forward-only lighting system.
- The retained transmission shader now uses
  `outputAlpha = max(alpha, transmission)` after it has already composited
  transmitted scene color and Fresnel reflection. This prevents a low base-color
  alpha from blending the result away a second time and restores normal/Fresnel
  detail on etched headlamp lenses. `StandardMaterialShadingTests` freezes this
  interim behavior. M6 may replace the compositing model, but must preserve the
  visual result deliberately rather than accidentally reverting it.
- M5 material diagnostics now accurately say that clustered lighting exists while
  production ordering/refraction/volume transport remains M6 work.
- M5 did not add bloom. Apparent glow or soft reflection features are not a bloom
  pass, and M6 should not build assumptions around one.

## The current glass path is a bridge, not the M6 design

The present Vulkan route is intentionally bounded legacy behavior:

1. the frontend sorts transparent packets back-to-front using one distance from
   the model/entity origin;
2. every packet except the last becomes the background bucket and the final packet
   becomes the foreground bucket;
3. each nonempty bucket copies the complete FP16 lit scene;
4. a depth-only, cull-none glass pass writes the nearest face into one D32 image;
5. the forward shader samples that scene copy and glass depth, then alpha blends
   into the lit scene.

This is not two physical layers, general peeling, correct nesting, or a thickness
solution. Specific liabilities to remove or supersede:

- `Application.cpp` computes the same origin distance for every submesh in a model.
  `DrawPacket` now carries a world-space primitive bounding sphere, and cooked
  `SubMesh` retains local bounds plus `primitiveGuid`, but the transparent sort does
  not use a depth interval and the packet does not retain the primitive GUID.
  M6 needs a stable transparent work identity/key, per-primitive bounds, explicit
  author priority, deterministic ties, and camera-space near/far intervals.
- `submitForwardQueues` still implements “all but last” versus “last” buckets. Do
  not rename these to layered glass; replace them with the classified ADR-0005
  paths and explicit overflow/fallback behavior.
- `GlassDepthPipeline` disables culling but stores only the nearest depth. It does
  not preserve paired front/back surfaces, evaluate material coverage in that pass,
  or prove valid winding/closed geometry. It cannot measure closed-shell thickness.
- `complex_material_body.glsl::linearizeDepth` hardcodes camera near/far to
  0.1/100 m. The shader then adds an arbitrary 0.02 m thickness, applies empirical
  refraction scales, clamps off-screen UVs, and perturbs UVs directly by normal and
  roughness. M6 must use active-camera parameters, explicitly defined thin-shell or
  volume thickness, a linear-HDR scene-color pyramid for rough refraction, and a
  diagnosed off-screen fallback.
- The current scene copies are full resolution and repeated per legacy bucket.
  Treat them as baseline cost, not architecture to multiply by the new layer count.
  At native 4K, one RGBA16F image is about 63.3 MiB and a full mip chain is about
  84.4 MiB before alignment. Use graph lifetime/aliasing evidence and tile/bounds
  restriction rather than unbounded full-frame duplication.
- Transparent forward currently shares the main depth attachment read-only while
  opaque complex forward writes it first. Preserve that ordering relationship or
  explicitly replace it; do not let transparent work invalidate opaque visibility.

## Material and classification starting point

The source/compiler/runtime already preserve the data M6 needs:

- alpha mode, cutoff, coverage, and transmission texture/factor;
- IOR/specular color;
- volume thickness, attenuation distance, attenuation color, and thickness texture;
- dispersion amount;
- diffuse-transmission factor/color and textures;
- normal, tangent, roughness, metallic, F0/F90, emissive, and complex lobe identity.

Active transmission currently selects `ComplexForward`; alpha-blend without complex
lobes selects `StandardForward`. Runtime `RenderQueue` still has only Opaque,
ForwardOpaque, and Transparent. M6 should add the ADR-0005 semantic classes
(`AlphaClip`, `SortedSurface`, `ThinGlass`, `LayeredGlass`, `WeightedOIT`) as
versioned backend-neutral material/primitive policy, with a safe compiler default
and author override. Do not encode those semantics only as Vulkan pipelines or UI
enums. Any packed material/product ABI change needs an explicit version, CookKey
invalidation, migration/diagnostics, and stable unknown/failure behavior.

Geometry classification matters as much as material classification. Thin glass
needs a documented shell approximation or reliable front/back data. Volume glass
needs validated closed geometry or explicit thickness. Disconnected transparent
surfaces must never be merged merely because they share a material.

## Shadows and transparent transmission

M5.13 changed conventional shadow behavior in ways relevant to glass:

- directional and spot hard/contact edges use compare-then-bilinear reconstruction
  with a compact tent; point cubes use a stable dense disk over nearest raw depth;
- PCSS softness comes from physical source angle/radius and retains bounded blocker
  and filter samples;
- receiver bias is derived from the world-space shadow-texel footprint instead of
  fixed normalized depth or fixed centimeters;
- normal-mapped shading normals no longer move the receiver's shadow lookup. This
  closed the detached-shadow failure and is an important precedent for M6: do not
  use a high-frequency shading normal to translate a depth/thickness receiver.

Transparent surfaces can receive current scalar opaque-shadow visibility through
their direct-light evaluation. They do not generally cast transparent or colored
shadows. Alpha-mask casters are supported; general transmissive casters are not.

Per ADR-0010, M6 owns the material semantics for RGB/spectral transmittance,
absorption, thickness, and coverage. M10 owns the bounded raster transmittance-
shadow product; M11 owns ray-traced transmission alternatives. M6 should therefore
produce an unambiguous future input contract and fixtures, but must not fake colored
shadows by tinting the scalar opaque map with base color. Caustics are separate and
are not implied by colored transmission.

Point cube maps remain the weakest conventional case at cube-face grazing angles.
Their bias is now distance/resolution-scaled rather than a fixed centimeter, but a
future geometric-normal receiver offset still needs dressed-scene evidence. Do not
paper over glass artifacts by increasing one global bias; keep shading normals,
geometric normals, transport thickness, and shadow bias as separate quantities.

## Environment and reflection integration

- New HDRI imports default to a 1024-face Ultra GGX reflection product. Existing
  products keep their authored settings until explicitly upgraded/reimported.
- Smooth materials sample the sharpest available prefiltered mip and roughness
  selects progressively filtered mips. Do not add per-material duplicate cubes or a
  raw-HDRI-only glass reflection path.
- Global environment and local reflection probes use the same roughness-aware IBL
  contract in deferred and forward shading. Retain box correction, bounded probe
  blending, complete-only publication, and safe fallback.
- Scene-captured probes currently do not claim general transparent/refractive
  capture. M6 must define recursion/exclusion policy explicitly if this changes;
  blindly capturing refractive glass into a probe can feed stale or recursive
  screen-space transport back into itself.
- The M5.12 Ultra product adds exactly 108 MiB over the accepted 256-face product
  in its matched checkpoint. M6 memory reports must identify the exact environment
  recipe rather than attributing that residency to transparency.

## Resize, graph, and descriptor warning

M5 fixed a resize/maximize crash caused by graph-resource and clustered descriptor
lifetimes. Every new scene pyramid, peel target, OIT accumulation target, depth
layer, tile list, and history must be graph-declared and must recreate/rebind views
when the render extent changes. Old views are retired with the owning frame context;
do not cache graph-owned image views in a longer-lived glass object. Required tests
include repeated resize, maximize, restore, minimize/zero extent, HDR transport
change, and capture insertion while each transparency class is active.

## Performance baselines and first measurement

- The ordinary transparency/refraction budget is 1.0 ms at native 4K on the RTX
  4090 reference. Marked hero glass may have a separately reported bounded budget;
  it is not permission for unbounded per-pixel storage or layer count.
- Accepted dressed M5 is 4.238 ms GPU median of medians and 4.521 ms worst p99, but
  M5.12/M5.13 are post-acceptance changes. Establish a fresh matched post-hardening
  no-M6 baseline before judging M6 deltas.
- The accepted dressed complex-forward median is 1.249 ms and cluster assignment is
  1.930 ms. Separate existing forward lighting/cluster work from new M6 ordering,
  copy/pyramid, peel, OIT, and refraction costs.
- M5.13 high-end shadow defaults reserve about 1,104 MiB (512 MiB directional plus
  592 MiB local), substantially above M5.11's accepted 528 MiB. Report current
  shadow settings and do not use the older total when budgeting M6 targets.
- M5 retains 39 C++ allocations/5,288 requested bytes per dressed frame. New M6
  queues/tile/layer lists should use persistent frame-context scratch and move the
  renderer back toward M4's zero steady-allocation standard.

## Recommended M6 qualification matrix

At minimum, freeze matched scene-linear and final captures for:

- Alfa Romeo windows and etched headlamp glass, including normal/roughness detail,
  Fresnel, HDRI/probe reflections, and strong side lighting;
- sorted disconnected transparent primitives and cyclic/intersecting surfaces;
- thin closed glass with valid and invalid winding/thickness inputs;
- two nested shells, then scalable hero layers with explicit overflow/fallback;
- roughness and IOR sweeps against the HDR scene pyramid;
- colored absorption over known metric thickness and an M10-ready stained-glass
  transmittance contract, with caustics explicitly absent;
- weighted-OIT particles/high overdraw versus a reference order, without using OIT
  for hero refractive glass;
- off-screen refraction, screen edges, depth discontinuities, camera cuts, motion,
  and objects entering/leaving the frame;
- overlapping directional/spot/point shadows on transparent receivers;
- global HDRI plus overlapping sphere/box reflection probes;
- SDR, scRGB, HDR10, scene-linear capture, selection overlay, and all resize states.

Report class counts, bounds/tile coverage, fragment invocations, layers requested/
stored/rejected, early termination, overflow/fallback, pyramid/copy costs, forward
lighting cost, persistent/transient VRAM, allocations, and validation output.

## Suggested first slices

1. Freeze a post-M5.13 baseline and M6 fixtures before changing rendering.
2. Add versioned transparent classification plus stable per-primitive bounds/depth
   keys and diagnostics, retaining current rendering as the comparison path.
3. Replace entity-origin sorting with deterministic `SortedSurface` work.
4. Implement `ThinGlass` with active-camera data and explicit thickness semantics.
5. Add one graph-owned linear-HDR scene pyramid and defined off-screen fallback.
6. Add bounded `LayeredGlass` with a two-layer baseline, tile restriction, early
   termination, quality tiers, and visible overflow fallback.
7. Add weighted OIT only for its classified workloads.
8. Run the complete car/nesting/HDR/resize/performance gate before cutover.

No accepted ADR needs to be weakened for this direction. If evidence changes the
classification or bounded-layer decision, write a superseding ADR rather than
silently replacing ADR-0005.
