# ADR-0010: High-Fidelity Sky, Shadow, and Occlusion Evolution

- Status: Accepted
- Date: 2026-08-11
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
   update budgets, filtering, cache latency, contact-shadow policy, and memory limits;
   Light components own enable, source size, quality override, and priority.
4. Contact-hardening raster shadows use physical light extent: directional source
   angle and local source radius/shape drive blocker search and penumbra width. The
   production candidate is a stochastic PCSS/SMRT-style filter with a clean
   point-like hard-shadow limit, bounded sampling, receiver-plane/bias handling, and
   temporal stability supplied by M9. Wide-kernel moment/variance methods may be
   measured, but are not the default because light leaking and precision failure are
   unacceptable without evidence.
5. M7 owns the measured production successor for large-world shadow detail: sparse
   virtual shadow maps with page tables, GPU page marking/culling, physical-page
   pools, cache invalidation/age, directional clip levels, and local-light pages.
   This is distinct from Variance Shadow Maps despite the shared acronym “VSM.” M8
   feeds the same pages from meshlet caster submission. Conventional maps remain a
   capability/debug fallback until the virtual path wins matched 4K quality,
   performance, and memory tests.
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
- Colored translucent shadows require M6 material semantics and additional colored
  visibility storage or rays; multiplying an opaque depth shadow by base color is
  rejected as physically and compositionally insufficient.
- AO remains indirect-visibility modulation. It must not darken direct light or
  stack authored, screen-space, probe, and ray occlusion without an explicit
  composition rule.
- Sky settings are component data independent of ImGui and Vulkan. Asset cooking and
  runtime publication remain GUID/DDC based and source-free.

## Primary research basis

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
- Opposing and overlapping multi-light shadow fixtures with independent visibility.
- Thin geometry, foliage alpha masks, moving casters, cascade/page boundaries,
  disocclusion, and high-frequency normal/material fixtures.
- Stained glass and participating-media fixtures separating direct transmission,
  indirect lighting, and unsupported caustics.
- AO ground truth against reference rays, including halo, over-darkening, thin-
  object, off-screen, motion, and specular-occlusion cases.
- 4K GPU/CPU percentiles, light/page/sample counts, cache behavior, persistent and
  transient VRAM, and validation-clean Debug/Release runs on the target tier.
