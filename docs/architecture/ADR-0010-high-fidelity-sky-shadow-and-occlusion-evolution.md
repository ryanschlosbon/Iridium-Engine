# ADR-0010: High-Fidelity Sky, Shadow, and Occlusion Evolution

- Status: Accepted
- Date: 2026-08-11
- Last updated: 2026-09-30 (M7.8 exact full-view depth accumulation and GPU compaction qualification)
- Extends: ADR-0007, ADR-0008, and ADR-0009; it does not weaken their shared
  light-assignment, per-light visibility, cache-validity, or safe-fallback rules
- Owners: Renderer, material system, scene system, RHI, Vulkan backend, asset
  pipeline, editor, and profiling

## Context

Iridium targets native or temporally reconstructed 4K above 100 FPS on high-end
PCs. The accepted M5 shadow maps are a correct, cacheable raster foundation, but a
fixed-resolution PCF map is not the final fidelity target. The production renderer
also needs physically driven penumbrae, dense independent shadow casters, high detail
over large view ranges, transmission through colored translucent media, contact
detail, and a scalable ambient-occlusion stack.

The same problem applies to the sky. M5.5 has a cooked environment product, but the
scene needs a stable artist-facing owner rather than a hardcoded environment. HDRI,
authored skybox, and simulated atmosphere have different data and implementation
requirements and must not be flattened into one ambiguous setting group.

Published engine details are incomplete, especially for proprietary Frostbite,
Anvil, Snowdrop, Northlight, and RE Engine versions. This decision therefore adopts
publicly documented production patterns and measurable requirements; it does not
claim parity with undisclosed implementations.

## Decision

1. `iridium.component.sky` is the stable scene owner with three explicit modes:
   `Skybox`, `Hdri`, and `Simulated`. Each mode owns a separate settings structure.
   M5 implements HDRI assignment, cooking, thumbnail drag/drop, background
   visibility/intensity, lighting intensity, rotation, lighting participation,
   priority, deterministic selection, and safe black/neutral fallback. Skybox and
   Simulated retain distinct persisted authoring contracts until their render paths
   land.
2. Shadow visibility remains per light. No quality feature may collapse independent
   directional, spot, point, or future area-light visibility into one global mask.
   Storage and update budgets can omit a lower-ranked shadow owner explicitly, but
   one owner's visibility never substitutes for another's.
3. M5's conventional cascades, spot atlas, and tiered point cubes remain the robust
   fallback and reference path. Project quality profiles own resolution, owner and
   update budgets, filtering, cache latency, contact-shadow policy, memory limits,
   and directional-shadow coverage distance. Light components own enable, source
   size, quality override, and priority. Directional coverage is not inferred from
   the camera far plane and is not normally an arbitrary per-light physical setting;
   the engine derives stable cascade splits from project/profile policy and reports
   world units per texel. Bounded camera- or volume-specific overrides may be added
   later without exposing manual per-cascade planes.
4. Contact-hardening raster shadows use physical light extent: directional source
   angle and local source radius/shape drive blocker search and penumbra width. The
   production candidate is a stochastic PCSS/SMRT-style filter with a clean
   point-like hard-shadow limit, bounded sampling, receiver-plane/bias handling, and
   temporal stability supplied by M9. Wide-kernel moment/variance methods may be
   measured, but are not the default because light leaking and precision failure are
   unacceptable without evidence. M7.7 hardens the spatial conventional path before
   virtual-shadow comparison: PCSS filter taps reconstruct bilinear depth
   comparisons instead of averaging nearest binary texels, while blocker search may
   continue to read raw depth. Bias combines the existing world-units-per-texel
   scale with receiver-plane depth-gradient bias and a bounded geometric-normal
   world offset; simply increasing global constants is not an acceptable acne fix.
   Geometric normals may be reconstructed from world-position derivatives when this
   avoids expanding the canonical GBuffer. Blue-noise sequences, reprojection,
   temporal accumulation, and denoising remain M9 work.
   Shadow-caster geometry LOD is selected independently for each shadow consumer,
   never inherited from the main camera: directional cascades use measured world
   units per texel, point cubes use a face-invariant radial bound, and spotlights
   evaluate conservative projected error in the authored light clip transform and
   atlas-tile resolution. Only resident, buffer-compatible cooked chain prefixes
   are eligible; invalid projections or chains retain LOD0. A shared project/profile
   texel-error policy may control these paths while per-light resolution and quality
   naturally change the selected detail. LOD0 remains the default until matched 4K
   visual and warmed performance admission succeeds.
5. M7 owns the measured production successor for large-world shadow detail: sparse
   virtual shadow maps with page tables, GPU page marking/culling, physical-page
   pools, cache invalidation/age, directional clip levels, and local-light pages.
   This is distinct from Variance Shadow Maps despite the shared acronym “VSM.” M8
   feeds the same pages from meshlet caster submission. Conventional maps remain a
   capability/debug fallback until the virtual path wins matched 4K quality,
   performance, and memory tests. Conventional and virtual methods are selectable by
   project/quality policy. They are not rendered redundantly over the same coverage
   merely to combine them; any future hybrid must define mutually exclusive regions
   or a measured complementary role.
6. Screen-space contact shadows are a targeted complement, never primary shadow
   ownership. They may restore sub-map-resolution contact and selected non-shadow-
   map detail, but must expose off-screen/depth-discontinuity limits and use M9
   temporal rejection. They are disabled where virtual or ray-traced visibility
   already provides equivalent detail without benefit.
7. M6 defines spectral/RGB transmittance, absorption, thickness, and coverage for
   transparent material closures. M10 adds a bounded raster transmittance-shadow
   path for colored glass and suitable participating media, with separate opaque,
   alpha-clip, and RGB optical-depth products. M11 adds ray-traced transmission and
   area-shadow alternatives for hero content. Caustics are a separate feature and
   are not implied by colored shadows.
8. Authored material AO remains a material-scale input, not a substitute for scene
   occlusion. M10 adds a high-quality GTAO-class horizon solution with depth/normal
   pyramids, bent normals where justified, temporal/spatial denoising, multi-bounce
   compensation, and physically bounded diffuse and specular occlusion. FidelityFX
   CACAO is a required Vulkan-capable comparison/fallback candidate. M11 adds RTAO
   and lets the renderer select or combine screen, probe/distance-field, and ray
   visibility without double-darkening.
9. Quality is explicit and user configurable. Project-wide Low/Medium/High/Ultra/
   Cinematic profiles choose shadow representation, resolution/page pool, rays,
   samples, denoising, maximum owners, cache/update budgets, contact detail, colored
   transmittance, AO method/resolution, and memory cap. Per-light and per-volume
   overrides are bounded by the project policy. High/Ultra target the reference
   RTX 4090/5090 class first; lower tiers preserve semantics rather than silently
   changing light or material intent.
10. Every new path must serve deferred/material-resolve and complex-forward consumers
    through the same light, BSDF, cluster, visibility, and environment contracts.
    A forward material route is not permission to lose normal maps, metallic/specular
    response, clearcoat, or environment reflections.

## Consequences

- High fidelity is budgeted rather than artificially capped at one caster. More
  shadowed lights consume raster work, page residency, filtering, and bandwidth, so
  selection, caching, virtual residency, and temporal reuse remain visible policies.
- Physically large emitters transition continuously from sharp contact to wider
  penumbrae. “Hard” and “soft” are endpoints of one source-size model, not unrelated
  rendering modes.
- Virtual pages reduce the cost of uniformly allocating maximum resolution, but page
  invalidation and many lights affecting the same pixels can still dominate. The
  profiler must expose requested/rendered/cached pages and shadowed lights per pixel.
- Conventional cascade quality is governed by projected texel density, not by a
  quality label alone. The profiler exposes configured coverage distance, per-
  cascade near/far intervals, world units per texel, projected texel size, filter
  radius/sample counts, and active bias terms. The Cinematic profile must remain
  reachable when selected and must not be silently capped to Ultra.
- Colored translucent shadows require M6 material semantics and additional colored
  visibility storage or rays; multiplying an opaque depth shadow by base color is
  rejected as physically and compositionally insufficient.
- AO remains indirect-visibility modulation. It must not darken direct light or
  stack authored, screen-space, probe, and ray occlusion without an explicit
  composition rule.
- Sky settings are component data independent of ImGui and Vulkan. Asset cooking and
  runtime publication remain GUID/DDC based and source-free.

## Primary research basis

Implementation evidence for the accepted M7.7 conventional path is recorded in
`docs/performance/M7.7-directional-shadow-density-and-filter-reconstruction-2026-09-12.md`
and
`docs/performance/M7.7-directional-shadow-receiver-bias-2026-09-12.md`, followed by
`docs/performance/M7.7-directional-shadow-caster-visibility-2026-09-12.md` and
`docs/performance/M7.7-gpu-scene-shadow-submission-2026-09-13.md`, then
`docs/performance/M7.7-directional-shadow-device-commands-2026-09-13.md` and
`docs/performance/M7.7-local-shadow-device-commands-2026-09-13.md`, followed by
`docs/performance/M7.7-shadow-command-oracle-gating-2026-09-13.md` and
`docs/performance/M7.7-independent-probe-visibility-2026-09-13.md`, followed by
`docs/performance/M7.7-directional-shadow-lod-2026-09-14.md`,
`docs/performance/M7.7-point-shadow-radial-lod-2026-09-14.md`, and
`docs/performance/M7.7-spot-shadow-projected-lod-2026-09-14.md`. Reflection capture
applies the same shared-record/independent-visibility rule; these checkpoints refine
the accepted architecture without superseding it.

M7.8's first implementation checkpoint freezes the backend-neutral sparse page
identity, separate static/dynamic validity, deterministic bounded allocation and
eviction, pending-raster publication, and safe missing-page fallback contract. See
`docs/performance/M7.8-virtual-shadow-residency-contract-2026-09-29.md`.
That evidence now also records the default-off Vulkan physical-pool/page-table
allocation checkpoint and its explicit memory cost. ABI v2 adds signed world-page
coordinates, snapped clip origins, and a deterministic directional receiver-marking
oracle so routine camera scrolling preserves cache identity; sampling remains
disabled. The matching raw Vulkan compute projection and deterministic bounded
reference compactor are hardware-qualified against that oracle. Scalable parallel
compaction, page raster, and sampling remain separate gated work.
The first forward-progress-safe parallel sort/reduce/sort candidate preserves the
contract but fails the gate at roughly 845 ms for 65,536 unique marks on the RTX
4090. It is retained as qualification evidence only; this ADR does not admit global
bitonic ordering as the production implementation.
The subsequent hierarchical shared-memory/merge candidate preserves the same exact
contract and reduces the full 65,536-mark compute median to about 0.146 ms on the
RTX 4090. It is admitted as the implementation candidate. The default-off Vulkan
owner now provides two independent aligned device-local working sets for the two
frames in flight (17,042,496 bytes total). The same owner now creates immutable
per-frame descriptors and both pipelines. Their shared direct compute chain passes
exact populated/empty reuse under synchronization validation and measures about
0.147 ms Release for 65,536 receivers. Runtime promotion still requires live
depth-driven receiver/clip publication and integrated frame evidence. See
`docs/performance/M7.8-persistent-mark-compact-chain-2026-09-30.md`.
The next checkpoint adds exact sampled-depth reconstruction for explicit bounded
pixel regions and hardware-qualifies its direct chain. Capacity limits must not
silently turn into spatial decimation: this region checkpoint did not yet provide
full-view page-demand accumulation, and forward/transparency receivers require
explicit coverage or conventional fallback. See
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


- Epic, [Virtual Shadow Maps](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine): sparse high-resolution pages, caching,
  and ray-sampled contact-hardening filters.
- Epic, [Contact Shadows](https://dev.epicgames.com/documentation/en-us/unreal-engine/contact-shadows-in-unreal-engine): per-light screen-space contact rays and
  their screen/depth limitations.
- Epic, [Hardware Ray Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-ray-tracing-in-unreal-engine): higher-accuracy contact-hardening
  shadow alternatives.
- NVIDIA, [Summed-Area Variance Shadow Maps](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps): PCSS blocker/penumbra stages and the
  cost/leak tradeoffs of wide filtering.
- Activision, [Practical Realtime Strategies for Accurate Indirect Occlusion](https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf):
  the GTAO reference used for the M10 horizon-based AO candidate.
- AMD GPUOpen, [FidelityFX CACAO](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/combined-adaptive-compute-ambient-occlusion/): Vulkan-capable adaptive AO,
  quality tiers, downsampled operation, and edge-aware filtering.
- EA Frostbite, [Physically Based Sky, Atmosphere and Cloud Rendering](https://www.ea.com/news/physically-based-sky-atmosphere-and-cloud-rendering): a production basis for
  the later Simulated sky/atmosphere/cloud work.
- EA Frostbite, [Moving Frostbite to Physically Based Rendering](https://www.ea.com/news/moving-frostbite-to-pb): coherent material, lighting, and cinematic authoring
  rather than effect-specific shading exceptions.
- Epic, [Using Colored Translucent Shadows](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-colored-translucent-shadows-in-unreal-engine): RGB transmission as a
  concrete content requirement and evidence that implementation limitations must be
  stated rather than hidden.

## Verification required by the roadmap

- Matched hard-to-soft blocker-distance sweeps for directional and local lights.
- Close-up penumbra ramps that reveal binary-texel stepping or stipple, plus grazing
  receivers that distinguish acne from peter-panning and detached contacts.
- Opposing and overlapping multi-light shadow fixtures with independent visibility.
- Thin geometry, foliage alpha masks, moving casters, cascade/page boundaries,
  camera motion, disocclusion, and high-frequency normal/material fixtures.
- Stained glass and participating-media fixtures separating direct transmission,
  indirect lighting, and unsupported caustics.
- AO ground truth against reference rays, including halo, over-darkening, thin-
  object, off-screen, motion, and specular-occlusion cases.
- 4K GPU/CPU percentiles, light/page/sample counts, cascade coverage and world-units-
  per-texel diagnostics, active bias terms, cache behavior, persistent and transient
  VRAM, and validation-clean Debug/Release runs on the target tier.
