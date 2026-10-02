# M5 Execution Plan: Raster Lighting, Shadows, Probes, and Baking Foundation

- Roadmap milestone: M5
- Status: In progress; owner approved all recommended decisions on 2026-08-08;
  M5.0 through M5.6 and M5.8 through M5.10 are complete; M5.7 final repeated
  timing remains open
- Lead: this milestone-lead task; one integration owner for light semantics,
  clustered assignment, shadow/probe ownership, cooked lighting products, and final
  acceptance
- Last updated: 2026-08-13
- Dependencies: accepted M0 through M4. M4 is an effective dependency even though
  the older M5 roadmap entry names only M1/M2 and M3 asset integration.
- Relevant ADRs: ADR-0001 through accepted ADR-0011. ADR-0009
  supersedes the M5.6 single-directional-owner limit; ADR-0010 owns the long-term
  Sky, contact-hardening, virtual-shadow, translucent-shadow, and AO direction;
  ADR-0011 owns baked-lighting product and scene assignment.
- Performance contract: `docs/performance/FRAME_BUDGET.md`
- Accepted comparison: `docs/milestones/M4-acceptance-report-2026-08-03.md`
  and `docs/performance/M4.10-production-cutover-2026-08-03.md`
- Source baseline: `30252593f8fdd2de5dffbb8da31bb570ff49a7c0` on
  `Render-Refactor-for-Modularity`, with the intentionally dirty accepted M0-M4
  worktree preserved (313 status entries at planning time)
- Approval gate: satisfied on 2026-08-08. The owner approved every recommendation
  under **Decisions requested from the owner**. Plan creation changed documentation
  only; M5.0 began after that explicit approval.

## Objective and user-visible outcome

M5 replaces the fixed demonstration light and raw environment reflection with a
physically coherent raster-lighting foundation. Authored directional, point, and
spot lights will illuminate both standard deferred and complex-forward materials;
standard diffuse and specular image-based lighting will use cooked irradiance,
GGX-prefiltered radiance, and a BRDF integration product; directional, spot, and
point lights will have bounded, cacheable shadow-map paths; and environment and
local reflection probes will use stable scene and asset identities.

When M5 is accepted, an artist can create lights through normal scene commands,
author explicit physical units, assign or capture an environment/probe asset, save,
cook, reopen, and obtain the same validated lighting intent without a source path,
runtime JSON/image parse, Vulkan pointer, or hardcoded light. Standard surfaces and
an equivalent standard base routed through complex forward will agree under direct
light and IBL within frozen thresholds. Missing products, exhausted cluster or
shadow capacity, nonresident probes, and unsupported area lights will have safe,
deterministic, visible outcomes.

M5 also freezes renderer-independent manifests and runtime publication interfaces
for future lightmaps, irradiance probes/volumes, and related baked products. It does
not implement M10's production GI authoring, streaming, or dynamic-object solution.
Visible emissive radiance remains correct and scene-linear; emissive surfaces do not
implicitly illuminate other geometry without a baked, GI, or ray-tracing product.

## Planning audit

### Method and worktree

The milestone lead read the complete required planning/context documents, all six
accepted ADRs, the M4 execution plan and acceptance evidence, and the M2 closure and
production-cutover records relevant to lighting. The current source, shaders, tests,
RHI, render graph, Vulkan executor/backend, scene registries/codecs, cooker/DDC,
editor drawers/transactions, benchmark fixtures, profiling, and CMake test inventory
were reinspected. The existing Debug and Release build trees each enumerate the
accepted 60 CTest targets. Tests and renderer benchmarks are deliberately rerun in
M5.0 after approval so the first captured M5 artifact has a frozen protocol and
worktree manifest; planning did not relabel old results as a new run.

The worktree contains the accepted M0-M4 implementation as extensive uncommitted and
untracked work. Those changes are user-owned. M5 will add only scoped changes, will
never clean or regenerate unrelated files, and will record overlapping frozen files
before each schema/build/shader cut.

### Pre-M5 implementation renderer and lighting audit

| Area | Current source fact | M5 consequence |
|---|---|---|
| Scene lights | At M5 start, `LightComponent` v1 stored type, RGB, ambiguous `intensity`, range, radius, cones, and `castsShadows`. It was source/cooked round-tripped as `iridium.component.light` / `LGT1`, shown by a transaction-owned editor drawer, and counted by the profiler. | M5.1 has now replaced the persistent contract with v2 while retaining ID/section stability. No renderer consumes it until M5.2 extraction. |
| Direct light | Deferred and complex-forward GLSL independently construct `normalize(vec3(1))` and `vec3(3)` as their only light. | This is the hardcoded demonstration light. Both consumers must move to shared light/cluster records and shared evaluation includes. |
| Environment | The shaders sample one equirectangular `sampler2D` for background and an unfiltered reflected direction, add a fixed diffuse factor of `1.5`, and convert linear Rec.709 to AP1 in the shader. | There is no diffuse irradiance, roughness mip hierarchy, BRDF integration term, probe selection, or physical environment scale. |
| Environment loading | Benchmark mode creates a raw float constant texture. Normal editor startup calls `stbi_loadf` on a hardcoded HDRI source path and uploads `RGBA32F`. | This is a legacy diagnostic exception to the source-free cooked runtime direction. M5.5 must remove it from production and use GUID/DDC products. |
| Deferred lighting | One fullscreen graphics pass reads the canonical M2 R cache and depth. `IRenderBackend::submitLightingPass` accepts only camera matrices/position. | The interface has no lighting scene input. It must accept a backend-neutral frame lighting packet without exposing Vulkan layout. |
| Complex forward | The same Vulkan scene descriptor set is bound at set 3, but the shader repeats the fixed light/environment code. It does share M2 BSDF and normal includes. | Descriptor ownership can be extended coherently, but light/IBL evaluation must be factored into shared includes and records. |
| Render graph | The backend-neutral compiler models buffers, images, layers, mips, storage access, and compute queue class. The production declaration lives in `VulkanProductionRenderGraph`. | M5 can reuse graph dependency/lifetime logic, but should introduce backend-neutral lighting graph descriptors instead of embedding semantic contracts in Vulkan code. |
| Vulkan graph execution | Buffers and storage usages can be allocated, but graph buffers are not publicly retrievable by logical name. Layered or mipmapped images are rejected with the old “M1.2 shadow allocator” 2D-only diagnostic. All execution uses the graphics queue. | Cluster buffers need executor access; cubemaps, shadow arrays, and mip chains need an explicit executor/resource extension. M5 does not add async-compute scheduling. |
| Texture RHI/runtime | `TextureDesc` supports 2D storage and mip levels plus BC6H; `TextureUsageClass::Environment` only changes memory attribution. The allocator creates a 2D image/view with one array layer. | Cubemap, array, compare-sampler, subresource-view, and depth-format contracts are missing. |
| Texture cooking | M3 texture schema 1 cooks deterministic 2D mip chains and BC6H HDR products. The importer rejects cubemaps, arrays, and non-2D source shapes. | M5 should compose a versioned environment/cubemap product from M3 artifacts rather than silently treating a 2D texture as a cube. |
| Shadows | No directional, spot, or point shadow resource, pass, caster queue, shader sampler, allocation policy, or cache exists. “Shadow executor” comments refer to M1 graph resource shadow mode, not shadow maps. | Shadow foundations are new vertical slices and require measured RHI prerequisites before raster passes. |
| Probes/baking | No probe component, probe registry entry, cubemap capture, probe GPU record, baked-lighting component, or baked product exists. | Stable components/products must be added through M4 registries and M3 DDC, not renderer-side ad hoc state. |
| Transforms | Light position/orientation can be derived from the entity `Transform` hierarchy. Current `worldMatrix` includes scale; there is no explicit derived world rotation. | Extraction must define one unit as one metre, local `-Z` as light forward, and derive an orthonormal hierarchy rotation that ignores scale/mirroring. |
| Profiling | Existing CPU/GPU ranges cover GBuffer, fixed deferred lighting, forward, transparency, output, and UI. `light.scene` exists; `changed.lights` is unavailable. Memory has only broad environment/render-graph buckets. GPU range capacity is 32. | M5 needs distinct extraction/upload/cluster/shadow/IBL/probe ranges, counters, and memory categories; range/counter capacity must be raised and tested. |
| Fixtures | M0 has material, emissive, legacy-light, geometry, transparency, and temporal fixtures. The benchmark harness creates model entities and a constant environment; it does not load an authored cooked lighting scene. | M5.0 must add cooked scene fixtures and forced deferred/forward comparators before production behavior changes. |

### M4 contract revalidation

The prompt's M4 assumptions match current source:

- entities are 64-bit generational handles and persistent UUID identity is separate;
- new/editor-created entities go through scene actions/command/factory services and
  receive Relationship components;
- Transform hierarchy state is persistent while children/depth/world matrices are
  derived;
- `iridium.component.light` is stable ID `LGT1`, source/cooked version 1;
- source schema 1, unknown preservation, swap-on-success loading, atomic save,
  source-free cooked runtime scenes, frozen manifests, and transaction-owned editor
  mutation are implemented;
- mesh/material references are GUID/subasset-GUID based and residency does not alter
  authoring state;
- runtime component headers are free of ImGui/editor behavior.

M5 must deliberately change the M4 frozen registry/CookKey/artifact contract when
the light version and new probe/baked components land. `M4ProductionCutoverTests`
must remain a historical guard, but its fixture/contract will be superseded by a
named M5 contract rather than updated blindly until hashes pass.

### Conflicts and stale descriptions

- The older ROADMAP M5 dependency line omits accepted M4. M5 now depends on M4's
  scene identity, registries, cooked runtime, and transaction boundaries.
- `PROJECT_CONTEXT.md` still contains an old “important unresolved choices” bullet
  about scene format details even though its later M4 section records those choices
  as accepted. M5 treats M4 source/acceptance as authoritative.
- `LightComponent` comments call intensity “Lumens/Lux” and `Area` a rectangular
  soft-shadow light. Neither statement is implemented or rigorous. Area lighting is
  not accepted by M5 unless a later measured slice explicitly adds it; this plan
  recommends a deterministic unsupported diagnostic.
- The normal editor environment path parses an HDR source file at runtime by path.
  This does not meet the M3/M4 production asset boundary and is removed during M5.5
  cutover; it is not preserved as a runtime fallback.
- Render-graph descriptors can express layers/mips but the Vulkan graph allocator
  rejects them. M5 must close that implementation gap before claiming graph-owned
  shadow/probe resources.
- The complex-forward shader hardcodes camera near/far values for its retained M6
  depth transport. M5 cluster depth parameters must come from the active camera;
  M5 will not broaden this into the M6 refraction rewrite.

No accepted ADR must be superseded for the recommended architecture. M5 implements
ADR-0001/0002/0004/0006 and preserves ADR-0003/0005 boundaries. New ADRs document
lasting M5 choices rather than rewriting an accepted record.

## Invariants

- Lighting, IBL, emissive, probes, shadows, and baked contributions are evaluated in
  scene-linear ACEScg/AP1. Exposure/output transform, gamut mapping, display
  encoding, and HDR transport happen exactly once at the M1 output boundary.
- The M2 canonical closure—not authored metallic/roughness—is the deferred lighting
  input. Shared BSDF/normal/light/attenuation/cone/shadow/IBL functions serve
  deferred and forward consumers.
- One coherent clustered assignment product supplies global and clustered local
  lights to deferred/material-resolve and complex/transparent forward paths. No
  second forward-only light scheduler or list is allowed.
- Runtime components contain authoring/runtime data only. They never store Vulkan
  descriptors, device addresses, GPU slots, renderer pointers, source paths, ImGui,
  SQLite, importers, or native dialogs.
- Persistent light/probe/baked ownership uses scene UUIDs and asset/subasset GUIDs.
  GPU indices, atlas slots, cube layers, and descriptor indices are transient.
- Position and orientation come from Transform/hierarchy. M5 does not duplicate a
  persistent world transform in light or probe data. Local `-Z` is the authored
  forward axis; world orientation ignores scale and mirrored-transform sign.
- One world unit is one metre for light range, source radius, probe extent, shadow
  projection, and baking manifests. Existing glTF metre semantics are retained.
- Range never replaces inverse-square attenuation. It is an explicit smooth culling
  window with diagnostics when authored too small for the light's contribution.
- Cluster, shadow, and probe overflow never performs out-of-bounds access, depends
  on GPU race order for visible selection, silently loses work, or flickers.
- Every pass declares graph reads/writes/stages/lifetimes. M5 does not add a parallel
  manual scheduler or hidden tone mapping inside lighting passes.
- The Vulkan backend owns descriptor layouts, image formats, compare samplers,
  barriers/layouts, subgroup/workgroup selection, device limits, and actual memory.
  RHI/scene/asset contracts remain backend neutral.
- Cooked lighting products use deterministic M3 CookKeys, receipts, DDC, provenance,
  corruption validation, and GUID dependencies. Runtime never parses JSON, glTF,
  HDR/EXR/DDS, or other source formats.
- Asset publication/residency and cache refresh do not dirty a scene. Explicit
  authoring assignment/capture/bake commands are transactional and undoable.
- The M2 R cache remains the production surface cache and image oracle. M5 does not
  implement the M7 GPU scene/visibility buffer or M8 mesh shaders.
- Existing corrected winding, positive-scale Alfa Romeo, Reverse Winding override,
  gizmo scale preservation, Entity 0 Relationship, hierarchy/model drops, model
  creation, and Entity 0 deletion behavior remain regression gates.

## Scope and non-goals

### Scope

- deterministic lighting fixtures, CPU reference equations, capture comparators,
  and M4 baseline replay;
- explicit photometric light schema v2, migration diagnostics, editor conversions,
  source/cooked round trips, and frozen-contract supersession;
- backend-neutral 64-byte GPU light records, stable UUID-to-slot extraction,
  changed-light uploads, capacity/diagnostics, and M7-compatible ownership;
- GPU-built clustered assignment shared by deferred and forward shading, debug
  visualization, occupancy/overflow diagnostics, and deterministic fallback;
- directional, point, and spot direct lighting with shared M2 BSDF conventions;
- deterministic environment/cubemap cooking, diffuse irradiance, GGX prefilter,
  F0/F90 BRDF integration, AP1 conversion, and neutral missing-product fallbacks;
- stabilized directional cascades and bounded spot/point shadow-map foundations with
  caching, priorities, update budgets, and graph-owned resources;
- stable global environment and local reflection-probe components, capture/cook,
  selection/blending, box correction, residency, and transactions;
- renderer-independent baked-lighting-set manifests/publication interfaces for
  lightmaps, irradiance probes/volumes/grids, and optional visibility/occlusion;
- complete CPU/GPU/memory/upload/allocation diagnostics and M5 production cutover.

### Non-goals

- production area lights, line/disk/rect emitters, LTC tables, raster soft area
  shadows, or RT shadows. `LightType::Area` is explicitly unsupported in M5.
- emissive-to-scene illumination without an explicit baked/GI/RT product.
- M6 transparency sorting, peeling, OIT, layered glass, advanced refraction,
  absorption transport, or scalable transparent layers.
- M7 persistent GPU scene, indirect visibility, progressive texture publication,
  or visibility-buffer material resolve.
- M8 mesh shaders, M9 temporal reconstruction/automatic exposure, M10 production GI
  authoring/streaming/dynamic-object GI, or M11 ray tracing/path tracing.
- async-compute queue ownership. M5 compute passes run on the current graphics queue;
  queue specialization requires later scheduling evidence.
- sample-car material, light, exposure, winding, scale, or shader exceptions.

## Design and data flow

```text
SceneWorld (UUID + Transform + Light/Probe/Baked components)
  -> backend-neutral extraction and validation
  -> stable UUID <-> transient light/probe slot maps
  -> changed record ranges + frame lighting constants
  -> RHI lighting packet
       -> Vulkan fence-safe uploads
       -> graph-declared cluster count/scan/fill/finalize
       -> one global + per-cluster light assignment product
       -> deferred and complex-forward direct/IBL/shadow sampling

GUID environment/probe/baked recipes
  -> source importer / deterministic cooker / M3 CookKey + DDC
  -> validated AP1 runtime products
  -> residency publication + semantic fallback
  -> global/local probe records and shader sampling

Stable scene UUIDs + asset revisions + transform revisions
  -> shadow/probe invalidation
  -> deterministic priority and update budget
  -> graph-owned directional arrays / spot atlas / point cube arrays
```

### Physical light and color convention

M5.1 introduces `iridium.component.light` source/cooked version 2 while retaining its
stable component ID and `LGT1` section ID. Recommended v2 persistent fields are:

```text
type                       : Directional | Point | Spot | legacy Area
colorLinearRec709          : nonnegative linear Rec.709/D65 RGB chromaticity
illuminanceLux             : directional incident illuminance
luminousIntensityCandela   : point/spot on-axis luminous intensity
rangeMeters                : local-light smooth culling boundary
sourceRadiusMeters         : finite-source near-field clamp; no soft-shadow claim
innerConeDegrees           : spot full-intensity half-angle
outerConeDegrees           : spot zero-intensity half-angle
castsShadows               : authoring intent
shadowQuality              : Low | Medium | High | Ultra
priority                   : signed deterministic overflow/update priority
```

The editor may display point lumens as `4*pi*cd`. A spot light with smoothstep cone
profile uses the exact effective solid angle
`2*pi*(1 - (cos(inner)+cos(outer))/2)` so lumens-to-on-axis-candela conversion and
CPU/shader fixtures agree. The component persists canonical lux/candela, not the
editor display-unit toggle.

Authored light color is displayed through an sRGB picker but stored as linear
Rec.709. Extraction converts it to AP1, rejects nonfinite/negative input, and
normalizes AP1 chromaticity to unit photopic/ACES Y before multiplying by the scalar
physical intensity. A zero-luminance color produces an off light plus a diagnostic.
This makes the scalar lux/candela independent of hue.

Version-1 migration is deterministic and visible:

- preserve the old numeric RGB triple as `colorLinearRec709` and emit
  `light.v1_color_assumed_linear_rec709`;
- copy `intensity` numerically into lux for Directional or candela for Point/Spot,
  initialize the inactive field to the v2 default, and emit
  `light.v1_intensity_unit_adopted`;
- preserve range/radius/cones/shadow intent exactly;
- retain Area as a readable legacy enum with an unsupported diagnostic. Strict cook
  fails an enabled Area light rather than silently approximating it.

The migration framework currently cannot return warnings from a component migration;
M5.1 extends it to ordered structured diagnostics. Existing source files are never
rewritten merely by runtime publication. A successful explicit save writes v2.

Direct local attenuation is inverse-square in metres with a finite-source distance
floor based on `sourceRadiusMeters` and a smooth range window
`saturate(1-(d/range)^4)^2`; range is not substituted for distance. The spot angular
term is smoothstep in cosine space between outer and inner cones. Directional lights
have no distance attenuation. Numerical epsilons, zero distance, zero radius, equal
cones, and extreme ranges are frozen in CPU/GLSL parity vectors.

GPU records retain physical lux/candela. A frame-global
`photometricToSceneScale` converts illuminance/luminance-equivalent values before
FP16 output. This preserves explicit units while keeping typical sun values inside
FP16. The recommended initial calibration candidate is `1e-4`; M5.1 freezes the
actual default only after grey-card/sun/indoor fixtures prove FP16 headroom and a
documented relationship to the existing final-boundary `manualExposureEv`:
`scene = physical * photometricToSceneScale`, then the M1 output boundary applies
`2^manualExposureEv` exactly once. M9 may add automatic exposure without changing
light units.

Environment source recipes declare primaries and a radiance scale in cd/m2. The
default imported HDR policy is linear Rec.709/D65 plus an explicit scale; AP1/Rec.2020
inputs require metadata. Cubemap captures and baked products are already AP1 and are
never converted a second time. Emissive follows the accepted M2 glTF sRGB-to-linear-
Rec.709-to-AP1 path and remains relative scene radiance; a future physical emissive
authoring UI must be separately versioned.

### Backend-neutral packed GPU light record

M5.2 freezes a standard-layout, `alignas(16)`, 64-byte record shared with shaders:

```text
float4 positionRange       // world metres xyz; smooth range metres
float4 directionOuterCos   // normalized surface-to-light direction; cos outer
float4 colorIntensity      // unit-Y AP1 chromaticity; physical lux or candela
float4 shapeMetadata       // radius, inverse cone delta, bitcast type/flags,
                           // bitcast shadow-data slot or invalid
```

Directional records ignore position/range/radius and store the surface-to-light
direction. Point records ignore cone data. Shadow transforms/atlas rectangles live
in a separate backend-neutral shadow-data table referenced by transient slot.
Vulkan descriptor/address details never enter this record.

The extraction system owns a UUID-keyed table and an inverse slot map. Initial slots
are assigned in UUID order; later valid owners retain slots until removal/world
replacement. Full UUIDs remain CPU authority and diagnostic provenance; an optional
32-bit debug tag may be stored in a separate provenance buffer rather than replacing
identity. Record equality is fieldwise/canonical, never raw padding comparison.

Capacity starts at 256 records, grows geometrically at a fence-safe frame boundary,
and is capped by the smaller of 65,536, RHI storage-buffer range, and configured
quality policy. Eight global directional lights are supported; additional
directionals participate in the same deterministic priority/fallback policy. The
CPU still scans the dense light pool in M5, but only changed/new/removed record
ranges upload. Snapshot/revision storage is reserved and allocation-free in steady
frames. M7 may replace the scan with changed-component streams without changing the
record or consumers.

World position is Transform translation. World direction is local `-Z` transformed
by a derived hierarchy quaternion/orthonormal basis that ignores scale. Singular,
nonfinite, missing-Transform, missing-identity, unsupported-Area, or invalid physical
lights are omitted with stable UUID/property diagnostics and counters.

### Shared clustered assignment

The accepted production configuration is 32x32 screen tiles, 24 logarithmic
view-depth slices between the active camera near/far planes, and a single product:

```text
ClusteredLightingProduct
  frame parameters and active-light count
  global directional light indices (max 8)
  cluster header buffer: uint offset, uint count
  compact uint local-light index buffer
  overflow/fallback header and deterministic top-light list
  optional provenance/occupancy counters
```

At 3840x2160 this is `120*68*24 = 195,840` clusters and 18.991 MiB per frame
context including headers, count/cursor scratch, scan scratch, indirect arguments,
diagnostics, and the 4,194,304-reference capacity. M5.3 measured 16/32 tiles and
24/32 slices on 64/512/4096-light scenes. At 512 lights, 32x32x24 costs 0.267 ms
versus 0.787 ms for 16x16x24, saves 31.9% cluster memory, and raises the conservative
consumer-loop proxy only 3.8%. The selected-default verification is 0.241 ms median /
0.259 ms p95.

Construction uses graph-declared compute passes on the graphics queue:

1. clear counts/diagnostics;
2. light-centric conservative cluster intersection and atomic count;
3. prefix-scan counts, compute bounded offsets, and detect per-cluster/global limits;
4. fill only validated ranges;
5. sort each used cluster by transient light slot and finalize headers/counters.

Below overflow, sorting removes atomic scheduling order from summation and captures.
The normal per-cluster limit candidate is 256 and the global reference candidate is
4,194,304. If either is exceeded, the whole product for that frame selects a CPU-
prepared deterministic fallback list of at most 64 lights ordered by authored
priority, shadow intent, conservative camera-space contribution, then UUID. Both
deferred and forward use that same list. The frame records an overflow code, requested
counts, and dropped count and enables a visible debug overlay. It never consumes a
partially race-selected list. Empty-light and sky-only frames produce valid zero-count
buffers and no stale references.

Cluster bounds use Vulkan 0..1 depth and view-space positive distance derived from
the active projection; no shader hardcodes near/far. Directional lights stay global.
Local-light sphere/cone bounds are conservative. Extreme-range locals remain locals;
their reference cost and overflow are visible rather than silently promoted into a
second global scheduler.

### Direct lighting and shared shader conventions

M5.4 introduces descriptor-free shared GLSL includes with matching CPU functions
for light decode, attenuation, cone, canonical BRDF application, and shadow hooks.
Deferred reconstructs the M2 canonical closure. Complex forward evaluates its
standard base and supported explicit lobes, but obtains lights from the exact same
cluster/global records. Unlit remains unlit. AO affects indirect IBL only, not direct
light. Emissive is added once.

The canonical convention is `V` surface-to-camera, `L` surface-to-light, world-space
shading normal oriented by the accepted M2 two-sided rules, and geometric normal
available where a shadow bias needs it. Backfacing one-sided light evaluation is
zero. Invalid half vectors, zero distance, grazing denominators, roughness floor,
and FP16 range are explicitly guarded. No material- or car-specific correction is
allowed.

### Cooked environment and complete standard IBL

M5.5 adds an `iridium.environment` cooked artifact (schema 1) whose provenance names
the source texture/cubemap GUID, input primaries/radiance scale, orientation,
convolution implementation/sample sequence, product resolutions/formats, tool
versions, and dependencies. Recommended High products are:

- AP1 radiance cubemap, 512x512 faces with a complete mip chain;
- diffuse irradiance cubemap, 32x32 faces;
- GGX-prefiltered specular cubemap, 256x256 or 512x512 faces with one roughness level
  per mip according to the frozen `roughness = mip/(mipCount-1)` convention;
- 256x256 two-channel BRDF integration LUT storing the Schlick split-sum scale/bias
  so `F0*scale + F90*bias` preserves scalar F90.

Resolution and BC6H-versus-FP16 choices are cook-quality policy and are accepted
only after image/error/timing/size evidence. Float32 accumulation with a pinned
low-discrepancy sequence is the reference. Compression follows deterministic M3
codec/version dependencies. Negative/nonfinite radiance is rejected unless a future
signed format policy is explicitly added.

Runtime RHI gains cube/array topology, mip/layer subresource views, compare samplers,
and required depth/color formats. The Vulkan allocator creates cube-compatible 2D
arrays and typed views; the graph executor stops rejecting valid declared layers/
mips and exposes logical buffers/images to passes. Capability reporting and format
limits are queried. Missing/not-yet-resident products bind neutral black irradiance,
black prefilter, and a correct constant BRDF fallback; the sky can use a separately
resident radiance cube. No stale descriptor is sampled.

Deferred and forward call one IBL include: diffuse irradiance times canonical diffuse
reflectance, prefiltered GGX radiance times the F0/F90 split sum, documented energy
compensation, and AO on indirect terms. Clearcoat and iridescence may reuse the GGX
product with their own roughness/Fresnel. Isotropic standard-base parity is the M5
acceptance gate. Sheen and anisotropic environment integration must either pass
dedicated reference evidence in M5.5 or retain an explicit named approximation;
M5 will not claim them as exact standard IBL.

### Directional shadow foundation

M5.6 recommends one prioritized shadowed directional light with four stabilized
cascades in a dedicated array. Other directional lights remain lit but unshadowed
with a visible capacity diagnostic. Candidate High settings are 2048x2048 per
cascade, practical/log split lambda 0.7, texel-snapped orthographic bounds, guard
bands, and a fixed 5x5 tent PCF compare filter. D32 is the reference format; D16 may
become production only if acne, detachment, quantization, bandwidth, and memory
comparisons pass.

Cascade selection/blending, receiver convention, raster slope bias, world-normal
bias, and per-cascade texel scale are shared with forward sampling. The camera and
light orientation determine cascade invalidation; sub-texel camera movement within
the snapped volume does not rerender. Graph declarations own the cascade depth array,
caster reads, and lighting reads. Vulkan owns render passes, compare samplers,
layouts, and depth bias.

### Spot and point shadow foundation

M5.7 uses a deterministic spot atlas and point cube-array pools:

- spots allocate power-of-two 512/1024/2048 tiles from a 4096 or 8192 square atlas;
- points allocate stable six-layer cube slots in separate 256/512/1024 pools so one
  high-resolution light does not waste every light's storage;
- resources allocate on demand. D32 is the reference; D16 is a measured production
  candidate.

Requests sort by quality, authored priority, conservative contribution, then owner
UUID. Existing compatible allocations remain stable. Eviction uses the same order
and records owner, requested/assigned tier, age, and reason. Spot projection uses the
outer cone; point faces use a frozen orientation/edge convention and seam-aware PCF.
Atlas guards prevent cross-tile filtering. The default High capacity candidates are
16 spot 1024-equivalents and 16 point lights at 512, but device memory and the 1.5 ms
shadow budget decide accepted limits.

Shadow cache keys include owner UUID, light parameters/world transform, quality,
projection, caster entity UUID/world-transform revision, mesh artifact revision,
material alpha/two-sided shadow state, and relevant pipeline version. A moved receiver
does not invalidate a scene-space shadow map. A moved caster invalidates intersecting
maps. A moved light invalidates all of its views; an old projection is never sampled
against a new light. Directional camera movement invalidates only changed cascades.

Updates are budgeted by rendered texels/faces and measured GPU time. New/moved-light
maps that miss the budget are temporarily unshadowed. Projection-compatible dirty-
caster maps may retain a bounded stale map with age diagnostics; the accepted stale
limit is selected from motion fixtures, not hidden. Alpha-clipped opaque casters use
the existing indexed material opacity contract. General transparent shadowing and
production temporal soft area shadows are not part of the M5.6/5.7 foundation.
M5.9 adds their backend-neutral quality contracts, a measured raster contact-
hardening slice, and the handoff to M6/M7/M9/M10/M11.

### Environment and reflection probes

M5.8 adds two stable component types through all M4 registries:

- `iridium.component.sky` is the global sky owner with separate `Skybox`, `Hdri`,
  and `Simulated` settings structures. HDRI owns a cooked environment-product asset
  GUID, independent lighting/background intensity, rotation, camera visibility,
  lighting participation, and priority. Skybox reserves cubemap/intensity/rotation/
  visibility settings. Simulated reserves atmosphere/ozone/turbidity/ground/sun/
  aerial-perspective settings without claiming its M10 render path is implemented;
- `iridium.component.reflection_probe` owns sphere/box influence shape, extent,
  blend distance, priority, capture settings/update mode, parallax-correction policy,
  and a stable cooked environment-product GUID. Position/orientation come from
  Transform.

At most one enabled HDRI Sky is selected by priority then stable entity order. The
source component persists GUID intent while asynchronous DDC cooking and runtime
publication remain transient. HDRI assets are first-class `iridium.environment`
assets, own `.hdr` import, show cooked thumbnails, and use typed drag/drop in the
Inspector. Missing/failed products preserve the authored GUID, diagnose the failure,
and render safe black/neutral lighting. Runtime sampling applies rotation and the
background/lighting controls through one shared deferred/complex-forward scene
contract. Skybox rendering and the physically based Simulated atmosphere move to
their named later slices without changing the component's mode ownership.

Local probes affect
specular reflections; M10 owns spatial diffuse GI. Probe candidates use the same
screen/depth cluster grid but separate probe headers/indices in the coherent lighting
product. Each cluster retains at most four probes sorted by priority, containment,
smallest influence volume, then UUID. Shading blends the top two normalized influence
weights and falls back to global. Box probes use bounded box projection; spheres use
directional lookup without claiming box correction. Missing/nonresident probes carry
zero weight without changing serialized intent.

Capture uses six deterministic faces and the production opaque/complex lighting path
with probe self-contribution disabled. Static captures cook through the same
environment product pipeline. On-demand capture budgets one configurable face or
equivalent texel budget per frame, records age/work, and publishes only a complete
validated cube. Partial cubes are never visible. Provenance includes scene asset GUID,
probe entity UUID, canonical scene hash, capture settings, dependency artifact hashes,
and shader/cooker versions. The editor's Capture/Bake/Assign operations are explicit
transactions; asynchronous completion/residency is not.

### High-fidelity raster shadow and occlusion handoff

M5.9 freezes the cross-milestone contracts in ADR-0010 and implements the first
measured contact-hardening raster slice. Directional source angle and local source
radius/shape control blocker search and penumbra width with a clean point-source
hard-shadow limit. The reference candidate is bounded stochastic PCSS/SMRT-style
sampling; fixed 5x5 PCF remains the stable fallback until temporal accumulation is
available in M9. Screen-space contact shadows are an optional diagnosed complement
for sub-map-resolution detail, never an owner or off-screen visibility solution.

Project profiles own representation, resolution/page and owner memory budgets,
samples/rays, cache/update latency, contact detail, translucent-shadow policy, and
AO method/resolution. Light components keep enable, source size, quality override,
and priority. M7 owns sparse virtual-shadow pages and GPU residency/culling, M8 owns
meshlet caster submission, M9 owns temporal filtering, M6 supplies transparent
closure transmittance/thickness, M10 owns raster RGB transmittance shadows and GTAO,
and M11 owns RT area shadows/RTAO/transmission. Variance or exponential moment maps
are research candidates only; they must beat the leak/precision behavior of the
reference path. AO composition is specified now so authored AO, GTAO/CACAO, probe
occlusion, specular occlusion, and RTAO cannot multiply into unbounded darkness.

### Baked-lighting interfaces

M5.10 adds `iridium.component.baked_lighting_set` with a stable asset GUID and a
versioned `iridium.baked-lighting-set` artifact. The renderer-independent manifest
contains typed optional sections:

```text
LightmapBinding
  scene entity UUID, model primitive/subasset GUID, UV set,
  page product GUID/subasset, scale/bias, encoding/version

IrradianceProbeVolume
  owning volume entity UUID, grid transform/dimensions/spacing,
  coefficient encoding, data product GUID/subasset

BakedVisibilityOrOcclusion
  owning entity/volume UUID, semantic/encoding, data product GUID/subasset
```

The artifact also records producer/version, target/quality, scene asset GUID and
canonical hash, sorted light/mesh/material/environment dependencies, sample/settings
hashes, and invalidation reason. Runtime publication exposes typed immutable views
resolved from GUIDs; components never store texture slots or renderer pointers.
Missing data returns neutral black indirect/no occlusion and a diagnostic. M5 tests
identity, versioning, determinism, invalidation, residency, and fallback, but does not
implement charting, a production lightmapper, probe-volume solver, streaming, or
dynamic-object GI.

### Diagnostics and profiling contract

M5 adds at least these scopes/counters, with unavailable work reported honestly:

- CPU: `lighting.extract`, `lighting.pack`, `lighting.upload.prepare`,
  `shadow.invalidate`, `shadow.schedule`, `probe.select`, `probe.capture.schedule`;
- GPU: cluster clear/count/scan/fill/finalize, deferred direct+IBL,
  forward opaque direct+IBL, transparent-forward lighting contribution, each
  directional cascade, spot atlas updates, point faces, probe capture/convolution;
- counts: total/valid/invalid/changed/uploaded lights, upload bytes/ranges,
  directional/local counts, cluster count/references/occupancy median/p95/p99/max,
  overflow cause/fallback/dropped lights, shadow requests/allocations/evictions/cache
  hits/misses/invalidations/updates/stale ages, probe candidates/blends/fallbacks/
  captures/cooks/residency;
- memory: light buffers, cluster persistent/transient, directional shadows, spot
  atlas, point cubes, environment/IBL, probes, baked products, staging/upload;
- provenance: entity UUID, component/property, asset/subasset GUID, transient slot,
  cluster, shadow allocation, probe selection, and material/closure identity.

Profiler fixed capacities are raised only with tests proving no steady allocation or
dropped values. Debug views include cluster occupancy/overflow, light bounds/types,
shadow cascade/atlas allocation/cache age, probe index/weights/influence, and direct/
diffuse-IBL/specular-IBL contribution isolation.

## Vertical slices

All slices are `Planned` until owner approval. After approval, only M5.0 becomes
`In Progress`; the lead updates this file before advancing any later slice. Each
slice keeps the engine buildable and records fallback behavior and durable evidence.

### M5.0 - Audit freeze, deterministic fixtures, reference equations, baseline

- Status: Complete; started and completed 2026-08-08 after owner approval.
- Preconditions: owner approves this plan and named open choices.
- Affected systems: test/reference libraries, benchmark/capture manifests, fixture
  scenes/assets, documentation, profiling schema; no production lighting change.
- Work: freeze current hardcoded-light/environment shader behavior and M4 contract;
  add analytic CPU references for light units/attenuation/cones/BSDF/IBL edge cases;
  add authored cooked scenes for sun/sky, point/spot, cone boundary, 64/512/4096
  local lights, cluster overflow, shadows, probes, emissive, standard deferred,
  forced equivalent forward, complex lab, and sample car.
- Tests: current 60 plus deterministic fixture/camera/content hashes, expected
  counts, CPU equations, NaN/Inf/zero/extreme inputs, source-free scene load, and
  capture-domain metadata.
- Measurements: prescribed Debug/Release builds/tests; five-run current 4K M4 car,
  material lab, complex lab, emissive, and legacy-light baselines; pass times,
  memory, upload, allocation, scene-linear/final images, validation.
- Fallback: fixture/reference additions only; production output remains exact M4.
- Completion: every later visual/performance decision has a fixed input and the
  current fixed light/raw environment are captured as defects, not references for
  physical correctness.
- Evidence: `docs/performance/M5.0-lighting-baseline-2026-08-08.md` and
  `docs/performance/data/M5.0-lighting-baseline-2026-08-08.json`; 62/62 Debug and
  62/62 Release CTests; 250,000 measured 4K frames with zero drops; validation-clean
  scene-linear/final captures for all five fixtures; exact M4 final-SDR preservation.
- Carry-forward: retained allocation counters report one 8-byte allocation at
  median/p99 versus M4's accepted zero. M5.0 changed no production source. Diagnose
  this discrepancy and add full-run counter aggregation before claiming the final
  zero-allocation gate; do not attribute it to lighting without evidence.

### M5.1 - Photometric light v2, migration, editor, and round trips

- Status: Complete 2026-08-08. Depends on M5.0.
- Affected systems: Light component, runtime/source registries, migration diagnostics,
  source/cooked codecs, scene cooker/CookKey, editor descriptors/drawer/transactions,
  frozen M4/M5 fixtures and tests.
- Work: implement the v2 convention above, explicit v1 migration warnings, editor
  lux/candela/lumen conversions, AP1 normalization reference, invalid/Area policy,
  and chosen photometric-to-scene calibration.
- Tests: v1/v0-to-v2 migration, source/cooked round trips, unknown preservation,
  no implicit save, undo/redo/multi-edit/type changes, unit/cone conversions,
  invalid values, registry manifest and deliberate frozen artifact supersession.
- Measurements: migration/cook size/time deltas and editor transaction costs; no
  renderer image change yet.
- Fallback: v1 sources remain readable through migration; failed conversion retains
  the active document. Derived cooked v1 scenes rebuild, never runtime-parse source.
- Completion: units and color semantics are explicit in source, runtime, editor,
  cooked data, CPU references, diagnostics, and durable docs.
- Result: Light source/cooked version 2 is production, with stable ID/`LGT1`,
  explicit linear Rec.709/D65 and lux/candela/metre/degree fields, ordered v1 and
  v0 migration warnings, strict Area rejection, sRGB/lumen editor presentation,
  `1e-4` physical-to-scene calibration, AP1 unit-Y reference math, deterministic
  cook/load, and compiler/CookKey feature version 2. Historical M4/M5.0 hashes are
  retained beside named M5.1 supersession blocks. Debug and Release pass 63/63
  tests. Release characterization and artifact deltas are recorded in
  `docs/performance/M5.1-light-component-v2-2026-08-08.md`. Renderer output, GPU
  time, and VRAM remain unchanged because extraction starts in M5.2.

### M5.2 - GPU light records, extraction, upload, and RHI diagnostics

- Status: Accepted 2026-08-09; depends on M5.1. Evidence:
  `docs/performance/M5.2-gpu-light-records-2026-08-09.md`.
- Affected systems: backend-neutral lighting types/extractor, Transform derived
  orientation, RHI frame input/capabilities, Vulkan buffers/descriptors/uploads,
  profiler, unit tests.
- Work: freeze the 64-byte ABI, UUID-slot map, validation, capacity growth, changed
  record uploads, removal/tombstone behavior, full-world swap, and provenance.
- Tests: layout/alignment/GLSL offsets, stable slots, hierarchy position/orientation,
  nonuniform/negative scale, add/change/remove/world swap, capacity/exhaustion,
  invalid lights, per-frame fence synchronization, zero-light path.
- Measurements: 1/256/4096/65,536-light extraction, pack/upload bytes/ranges,
  changed 0/1/10/100%, CPU median/p95/p99, steady allocations, persistent memory.
- Fallback: valid zero-light buffer and environment/emissive-only scene; extraction
  failure never leaves stale records visible.
- Completion: authored lights reach validated GPU records with changed-data upload
  and no Vulkan state in scene/runtime contracts.

### M5.3 - Shared clustered assignment, overflow, and visualization

- Status: Accepted 2026-08-09; depends on M5.2. Full evidence:
  `docs/performance/M5.3-shared-clustered-assignment-2026-08-09.md`.
- Affected systems: graph buffer access/declarations, compute pipelines/shaders,
  shared descriptor contract, cluster builder/finalizer, debug views, profiler.
- Work: implement and bake off 16/32 tiles and 24/32 log slices, coherent global
  and local lists, deterministic sorted lists, capacity detection, whole-product
  fallback, and occupancy/provenance views.
- Tests: CPU cluster/reference intersection, projection/depth conventions, empty,
  behind-camera, near-plane, extreme range, resize, 64/512/4096 lights, per-cluster
  and global overflow, repeated-run byte/capture determinism, OOB guards.
- Measurements: build passes, references, occupancy, buffer/transient bytes, upload,
  direct-consumer loop proxy at 4K, resize and 4K/1440p comparisons.
- Fallback: deterministic top-64 list shared by all consumers with visible overflow;
  zero lights yields empty valid lists.
- Completion: one safe measured product exists and no forward-only list/scheduler is
  introduced.

### M5.4 - Clustered direct-light deferred/forward parity

- Status: Accepted 2026-08-09; depends on accepted M5.3. Full evidence:
  `docs/performance/M5.4-clustered-direct-lighting-2026-08-09.md`.
- Affected systems: shared CPU/GLSL light evaluation, deferred shader/pipeline,
  complex-forward shader/layout, graph reads, material/direct-light debug views.
- Work: replace both fixed lights, evaluate directional/point/spot records, preserve
  unlit/emissive behavior, and remove duplicate direct-light code.
- Tests: CPU/GLSL vectors, inverse-square/range/cone/radius boundaries, F0/F90,
  roughness, normals/two-sided, equivalent standard deferred/forward, many-light
  accumulation and overflow fallback.
- Captures: analytic fixtures, material/complex lab, emissive, many lights, car;
  scene-linear direct contribution and final SDR/HDR smoke.
- Measurements: deferred and forward direct times, cluster consumption, CPU submit,
  allocations, image deltas, validation.
- Fallback: no-light/environment/emissive path. The hardcoded light remains only in
  a captured M5.0 oracle and is deleted from production at slice completion.
- Completion: scene lights are the sole production direct-light source and matched
  standard surfaces meet parity thresholds.

### M5.5 - Cubemap cooking and complete standard IBL

- Status: Accepted 2026-08-09; depends on accepted M5.4 and M3 texture/DDC
  contracts. Full evidence:
  `docs/performance/M5.5-cooked-environment-ibl-2026-08-09.md`.
- Affected systems: environment recipes/import/cook/DDC, texture/cube RHI, graph
  layered/mip resources, Vulkan allocation/views/upload, shared IBL CPU/GLSL,
  environment runtime publication, fixtures.
- Work: implement deterministic AP1 cubemap/irradiance/prefilter/BRDF products,
  roughness/orientation conventions, physical scale, residency/fallback, and remove
  the runtime source-HDRI path from production.
- Tests: independent process/schedule byte identity, CookKey/provenance/corruption,
  face orientation/seams/mips, AP1 transforms, F0/F90 split sum, Monte Carlo reference
  error, missing/nonresident/hot-reload, deferred/forward parity.
- Captures: constant/gradient/bright-source environments, rough dielectric/conductor
  grids, complex coat, sample car, sun/sky; scene-linear and final output.
- Measurements: cook time/artifact size, upload/staging, persistent/transient memory,
  deferred/forward IBL time and A/B image quality.
- Fallback: semantic neutral products and optional sky radiance; never raw source
  parse or stale descriptor.
- Completion: diffuse irradiance, prefiltered GGX specular, and F0/F90 BRDF
  integration are production and shared.

### M5.6 - Stabilized directional shadow foundation

- Status: Complete and accepted 2026-08-09; depends on M5.4 and layered/depth RHI
  from accepted M5.5.
- Affected systems: shadow data records, caster extraction, graph passes/resources,
  directional pipeline, cache/invalidation/scheduler, samplers/shaders/debug/profiler.
- Work: four-cascade primary sun, stabilization, split/blend/bias/filter policy,
  cache keys, camera/caster invalidation, update budget, and capacity fallback.
- Tests: cascade math/snapping, ownership, moved camera/light/caster/receiver,
  alpha/two-sided casters, allocation/failure, bias/filter CPU vectors.
- Captures/measurements: acne, panning, cascade seams, shimmering, thin/contact cases,
  cached and full-refresh timing, spike/memory, final validation.
- Fallback: prioritized unsupported directional lights remain unshadowed; an invalid
  or moved unscheduled map is never sampled with a new projection.
- Completion: one production-quality directional foundation meets stability and
  budget gates with explicit limits.
- Accepted result: one UUID/priority-selected sun owns a persistent imported D32
  2048x2048 four-layer array. Stabilized practical splits, 10% cascade blends,
  texel-scaled normal/raster/receiver bias, and shared deferred/forward 5x5 compare
  PCF are validation-clean. Static frames update zero layers; moving casters update
  all four at 0.0171 ms median / 0.0177 ms p95. The array costs exactly 64 MiB.
  Opaque, alpha-mask, and one/two-sided caster paths are exercised. Debug/Release
  pass 66/66. ADR-0008 is accepted; evidence is
  `docs/performance/M5.6-directional-shadows-2026-08-09.md`.

### M5.7 - Spot/point shadows, allocation, caching, and budgets

- Status: Complete on 2026-08-13. CPU allocation/projection/cache groundwork,
  spot-atlas and tiered point-cube GPU slices, complex-forward parity, and the
  prescribed five-process on/off timing distribution are accepted.
- Affected systems: spot atlas, point cube pools, local shadow matrices/records,
  caster culling, deterministic allocator/eviction, graph passes, sampling/debug.
- Work: quality tiers, guards/seams, stable UUID allocation, cache invalidation,
  scheduling/age/fallback, and local shadow priority.
- Tests: atlas packing/reuse/eviction, cube face orientation/seams, moved dependencies,
  range/cone projection, capacity/exhaustion, repeated-run determinism.
- Captures/measurements: local acne/panning/leak/seam/motion scenes, cache hit/miss,
  cold update spike, per-tier memory and 4K direct-sampling cost.
- Fallback: deterministic unshadowed light or bounded compatible stale map with age
  diagnostic according to accepted policy.
- Accepted result: 20 current Release processes cover 200,000 measured native-4K
  frames with zero frame/GPU-range drops. Two cached spot owners add 0.081248 ms
  median; two cached point owners add 0.406368 ms. Evidence is
  `docs/performance/M5.7-forward-parity-and-baseline-2026-08-11.md`.
- Completion: spot and point foundations are safe, budgeted, cacheable, and measured.
- Current groundwork: deterministic shared-light request ranking; stable guarded
  power-of-two spot-atlas allocation; separate stable 256/512/1024 point pools;
  frozen spot outer-cone and six-face Vulkan projections; and a rendered-texel
  scheduler that permits only bounded two-frame caster-compatible stale history.
  New/allocation/light/projection/pipeline changes that miss budget are unshadowed.
  `LocalShadowTests` raises the Debug suite to 67/67.
- Current spot GPU slice: persistent graph-declared 2048/4096/8192 D32 atlas;
  guarded tile clears and inner-tile rasterization; alpha-mask/double-sided caster
  variants; constant-time packed-light data-slot publication; and one shared 5x5
  comparison sampler for deferred and complex forward. A two-owner validation
  fixture visibly composes independent colored spot shadows. Its cold update records
  six draws, including two alpha-mask draws, in 0.016640 ms; cached frames record no
  spot draws. Conservative transformed-sphere frustum culling now rejects unrelated
  casters from updated tiles and exposes exact tested/culled counters; missing legacy
  bounds fall back to visible. The default atlas is exactly 64 MiB.
- Current point GPU slice: three persistent D32 cube arrays provide stable
  256/512/1024 tiers with default capacities 32/16/8. Whole-cube scheduling updates
  six faces atomically, conservatively culls each face, and publishes constant-time
  packed-light slots. Deferred and complex forward call the same seam-safe 5x5
  manual depth comparison. A two-owner validation fixture shows independent orange
  and blue projections overlapping on one receiver. The cold 512-tier update draws
  21 caster-face pairs in 0.031744 ms; static frames are complete cache hits. Point
  storage is exactly 336 MiB and combined default local-shadow storage is 400 MiB.
- Complex-forward parity checkpoint: dedicated opaque clearcoat fixtures retain
  identical receiver/caster geometry while selecting the complex-forward queue.
  The filtered visibility comparisons pass at 0.999987 mean luma SSIM for spot
  and 0.999947 for point, with 0.0105% and 0.0204% changed pixels above one code.
  The visibility debug view now reports all contributing shadow types and no longer
  exits before cascade/visibility diagnostics in complex forward. One complete
  10,000-frame 4K on/off pair per light type plus a second spot-on process are
  retained; the remaining pairs are not yet complete.
- Directional correction during M5.7: two independent four-cascade owners now share
  one eight-layer D32 array. Deferred and complex-forward resolve visibility per
  light slot, so opposing lights can cast over the same receiver without suppressing
  either contribution. Project Settings owns live owner/update/stabilization policy;
  startup policy owns 512/1024/2048/4096 allocation. A validation-enabled four-light
  stress run selects two and reports two explicit omissions; the 1024 run reserves
  exactly 32 MiB. ADR-0009 supersedes ADR-0008's single-owner capacity only.

### M5.8 - Environment/reflection probes and cubemap capture

- Status: Complete; accepted 2026-08-11. HDRI Sky, stable local-probe persistence,
  deterministic extraction/selection, Vulkan clustered sampling, six-face AP1 scene
  capture, GPU GGX filtering, complete-only fence-safe publication, project quality
  controls, and persistent `.irprobe` baking are implemented and qualified. Depends
  on M5.5 and M5.3; consumes the M5.7 shadowed capture path.
- Affected systems: new components/registries/codecs/editor transactions, probe
  extraction/assignment, capture scheduler, environment cooker/runtime, shading.
- Work: three-mode Sky ownership with distinct settings; cooked HDRI import,
  thumbnail drag/drop, source/cooked persistence, asynchronous DDC/runtime
  publication, shared background/IBL controls, stable priority selection and safe
  fallback; then local influence/priority/blending, box correction, capture/cook/
  publication, invalidation, residency and diagnostics.
- Tests: source/cooked round trips, transactions, selection ordering/weights,
  sphere/box bounds, parallax math, self-exclusion, partial capture, dependency
  invalidation, missing/nonresident, deterministic publication.
- Captures/measurements: colored-room probes, overlapping volumes, boundaries,
  box correction, moving camera, capture cost/budget, blending/sampling cost/memory.
- Fallback: global environment then black; partial or stale-incompatible captures are
  never sampled.
- Current HDRI result: `iridium.component.sky` persists Skybox/HDRI/Simulated mode-
  specific records. Only HDRI renders. `.hdr` is owned by the first-class
  `iridium.environment.hdri` importer, the Asset Browser provides environment
  thumbnails and typed drag/drop, and the runtime selects/cooks/publishes the active
  GUID without source parsing. Lighting/background intensity, Y rotation, camera
  visibility, lighting participation, and priority are exposed. Skybox and Simulated
  identify their future settings and clearly report that their rendering is pending.
- Current local-probe contract: sphere and oriented-box volumes retain distinct
  extents, interior blend distance, priority, intensity, capture/update/parallax
  policy, 128-through-4096 capture resolution, clip range, and cooked environment
  identity. The component is addable/removable through editor command transactions,
  accepts typed HDRI thumbnail drag/drop, and round-trips through canonical source
  and cooked runtime scenes. Extraction orders owners by stable UUID, removes scale
  from oriented influence transforms, diagnoses invalid owners/transforms, and gates
  residency without changing authored intent. Selection rejects disabled, invalid,
  missing, and nonresident products; orders deterministically by priority, influence,
  smallest volume, and stable UUID; bounds candidates to four; blends the top two
  within local coverage while the residual weight smoothly returns to the global
  environment; and provides robust box-projected reflection direction math.
  Publication maps stable UUID owners to reusable
  transient slots, emits a std430-compatible 112-byte record with abstract indexed-
  environment references, coalesces changed upload ranges, and reports residency,
  environment-resolution, and capacity omissions. A backend-neutral CPU oracle
  conservatively assigns at most four probes per existing 32x32x24 cluster with
  deterministic priority/influence/volume/UUID ordering and safe global-environment
  fallback on truncation. The production Vulkan path now uploads those records
  incrementally, dispatches one bounded compute assignment over the existing light-
  cluster grid, and publishes separate probe headers/indices plus a 64-entry cube
  table filled with a neutral fallback for unused entries. Deferred and complex-
  forward materials consume the same per-pixel top-two local-specular selection,
  including box projection and smooth global blending. Local diffuse remains owned
  by the global irradiance product. Six capture cameras now exactly match the cooked
  +X/-X/+Y/-Y/+Z/-Z convention. A stable-UUID scheduler prioritizes and budgets face
  work, distinguishes Baked/OnDemand/Realtime invalidation, retains last-known-good
  sampling during refresh, isolates incomplete faces, and requires an explicit
  complete-product publication acknowledgment. The Vulkan backend now allocates a
  cube-compatible RGBA16F raw-radiance target, six-layer D32 depth target, and full-
  mip RGBA16F storage/sampled prefilter target for each active capture. Per-face
  render views and per-mip six-layer compute views are owned by stable owner/ticket
  identity. Staging products remain physically separate from last-known-good
  published cubes until explicit fence-safe promotion; abandon preserves the old
  product. Device cube limits, 64 owners, four captures in flight, a 3 GiB logical
  staging budget, and a separate 4 GiB published-product budget are guarded without
  silent tier downgrade. Production capture now renders opaque and opaque-complex
  scene radiance with direct lights, raster shadows, optional global sky, owner
  exclusion, and recursive-local-probe suppression; dispatches a configurable
  Hammersley GGX filter; and publishes the completed cube into the shared indexed
  environment table. Baked mode performs a layer-major readback, exact cube-texel
  SH9 diffuse convolution, complete environment-product serialization, UUIDv7
  metadata retention, scene/shader CookKey provenance, atomic replacement, and
  Asset Browser refresh. Capture Now/Bake Capture increments transient explicit
  revisions after editor transactions rather than being discarded with authored
  state.
- Checkpoint evidence: `docs/performance/M5.8-hdri-sky-2026-08-11.md`.
  Probe-contract evidence:
  `docs/performance/M5.8-reflection-probe-selection-2026-08-11.md`.
  Final capture/qualification evidence:
  `docs/performance/M5.8-reflection-probe-capture-2026-08-11.md`.
- Completion: stable global and local specular probes work through cooked products
  with deterministic blending and no scene dirtying from residency.

### M5.9 - Contact-hardening shadows and future visibility/AO contracts

- Status: Complete on 2026-08-13. The retained path is bounded spatial PCSS over
  conventional cached maps with a deterministic fixed 5x5 PCF fallback. Its final
  stochastic temporal reconstruction remains explicitly assigned to M9.
- Affected systems: light source-size semantics, backend-neutral visibility quality
  policy, shared shadow sampling, project profiles, debug/profiling, M6/M7/M9/M10/M11
  interfaces.
- Work: implement blocker search and source-size-driven penumbra estimation with a
  point-source hard limit; bake off bounded PCSS/SMRT-style spatial sampling against
  fixed PCF and moment-filter candidates; expose resolution, owner/update, sample,
  contact, cache, and memory controls; freeze virtual-page, RGB transmittance, GTAO/
  CACAO, bent-normal/specular-occlusion, temporal, and RT handoffs from ADR-0010.
- Tests: hard-to-soft continuity, blocker-distance/source-size sweeps, independent
  overlapping lights, cascade/atlas/cube boundaries, thin and alpha-mask casters,
  receiver bias, missing capacity, deterministic fallback, deferred/forward parity.
- Captures/measurements: contact/penumbra reference scenes at 4K, moving light/caster/
  camera, samples versus error/noise, cache hit/miss, lights per pixel, GPU/CPU and
  persistent/transient VRAM for every retained tier.
- Fallback: accepted fixed 5x5 PCF and conventional maps. Screen-space detail is
  optional and never replaces off-screen/per-light visibility.
- Completion: Iridium has a physically meaningful hard/soft raster path and no
  later virtual, translucent, AO, temporal, or RT feature needs to fork light,
  material, project-policy, or visibility ownership.
- Evidence: `docs/performance/M5.9-contact-hardening-shadows-2026-08-13.md`.

### M5.10 - Baked-lighting data contracts and future-GI compatibility

- Status: Complete on 2026-08-13; depends on M5.8 and M4/M3 contracts.
- Affected systems: baked-set component, artifact manifest/cooker/DDC/runtime
  publication, neutral renderer interface, editor assignment/diagnostics, tests.
- Work: implement typed lightmap/probe-volume/visibility sections, provenance,
  identity associations, invalidation, deterministic serialization, and fallback.
- Tests: UUID/GUID association, moved/renamed assets, version/corruption/unknown
  sections, CookKey dependency changes, source-free runtime boundary, residency and
  transaction behavior.
- Measurements: artifact/manifest size, validation/load/publication time/memory;
  no production GI quality/performance claim.
- Fallback: optional sections resolve to neutral contribution; bad products fail
  publication without changing authored GUID intent.
- Completion: M10 can add solvers/streaming without changing stable scene/component/
  asset ownership or renderer-neutral runtime interfaces.
- Evidence: `docs/performance/M5.10-baked-lighting-contracts-2026-08-13.md`.

### M5.11 - Production cutover, qualification, and acceptance

- Status: Complete on 2026-08-13; depends on M5.0-M5.10.
- Affected systems: remove obsolete fixed light/raw HDRI/runtime paths, freeze M5
  source/cooked/product contracts, documentation, roadmap/context, acceptance report.
- Work: delete demo light/environment multipliers and temporary adapters; finalize
  ADRs, quality defaults, limits, diagnostics, fixtures, and production hashes.
- Automated gate: prescribed Debug/Release builds and all tests; CPU references,
  schema migrations/round trips, CookKey determinism, cluster overflow, direct/IBL
  parity, shadow cache/budget, probe selection/capture/residency, baked interfaces.
- Visual/runtime gate: every M5 fixture, M0-M4 preservation set, material/complex lab,
  emissive, many lights, shadows, probes, sample car, SDR/scRGB/HDR10 smoke, fixed
  scene-linear/final captures, Vulkan validation and normal cleanup.
- Performance/memory gate: five-run 4K sample car and dressed lighting scene; full
  CPU/GPU percentiles and pass breakdown; requested/committed persistent/transient
  memory; upload/staging; allocations; cold/cache-hit shadow/probe paths.
- Fallback: last accepted product versions/source-control reversal. Production never
  falls back to the hardcoded light or runtime source image parse.
- Completion: dated M5 acceptance report, this plan's completion report, accepted
  ADRs, performance baseline, and ROADMAP/PROJECT_CONTEXT update only after every gate
  genuinely passes.
- Evidence: `docs/performance/M5.11-production-qualification-2026-08-13.md` and
  `docs/milestones/M5-acceptance-report-2026-08-13.md`.

## Delegation and integration

The milestone lead works sequentially by default. No subagent is authorized during
the plan/approval phase. After interfaces are frozen, bounded read-heavy or disjoint
work may be delegated for reference math vectors, cooker determinism/corruption
fixtures, image analysis, or isolated editor tests. One owner at a time controls the
light/probe component schemas, runtime/source registry composition, packed light and
cluster headers, shared shader includes, production graph, central CMake shader/test
lists, descriptor layouts, and final cutover.

Every delegated result is reinspected against current source and this plan. No two
write-heavy tasks overlap shared renderer headers, registries, shader interfaces, or
build files. The lead performs integration, both configurations, visual validation,
measurements, and durable plan updates before advancing the next slice.

## Verification and acceptance strategy

### Prescribed builds and automation

Every implementation slice runs:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug --output-on-failure

cmake --preset x64-release
cmake --build out/build/x64-release
ctest --test-dir out/build/x64-release --output-on-failure
```

Performance decisions use Release on the reference i9-14900K/RTX 4090, native
3840x2160 base rendering, fixed camera/content/output/quality, validation off, 500
warm-up and 10,000 measured frames, five independent processes. Validation/capture
runs are separate. Results record driver, clocks/power state, shader/DDC cache state,
output transport, scene/camera/product hashes, and dirty-worktree manifest. Median,
p95, and p99 are retained for full runs, not only a rolling window.

### Accepted M4 baseline

The comparison baseline is:

| Metric | M4 accepted value |
|---|---:|
| CPU frame median | 2.8883 ms |
| GPU frame median | 1.5662 ms |
| CPU p95 / p99 | 16.1270 / 16.6309 ms |
| GPU p95 / p99 | 3.0614 / 4.5779 ms |
| requested live / peak | 897.558 / 972.257 MiB |
| committed live / peak | 967.567 / 1,042.265 MiB |
| steady C++ allocation median / p99 | 0 / 0 |
| final SDR versus M3.7 | byte-identical |
| Vulkan validation | clean |

M5.0 repeats this protocol before using it as a live comparator. M5 reports deltas
for the same car/camera plus absolute results for the new dressed lighting fixture;
it does not attribute changed material coverage or desktop scheduling to lighting
without matched counters and images.

### Reference and image thresholds

M5.0 freezes exact tolerances from reference vectors before shaders change. Initial
recommended gates are:

- CPU float equations versus high-precision reference: absolute `1e-5` and relative
  `1e-4`, with explicit zero/denormal/invalid cases;
- CPU versus GPU analytic probe samples: relative `5e-4` before FP16 storage;
- deferred versus equivalent forward direct lighting, excluding one-pixel raster
  boundaries: p99 relative scene-linear luminance error <= 0.5%, maximum <= 2%;
- deferred versus equivalent forward standard IBL: p99 <= 1%, maximum <= 3%;
- final matched-image SSIM >= 0.9999 with changed-pixel/error limits recorded per
  fixture rather than substituting SSIM for scene-linear correctness;
- environment cooker versus high-sample reference: per-face seam and irradiance/
  prefilter error thresholds frozen separately for uncompressed and chosen compressed
  quality tiers;
- shadow contact, acne, leak, cascade seam, and temporal motion fixtures use measured
  world/pixel masks. Bias may not hide acne by detaching contacts; sub-texel stabilized
  camera motion must not change fully lit/shadowed interior pixels outside a frozen
  edge band;
- probe CPU selection/weight/parallax vectors are exact within float tolerance;
  repeated captures/cooks and overflow selection must be byte-identical.

If FP16 raster-order or hardware filtering makes a threshold unrealistic, the slice
records evidence and obtains owner approval before changing it. It is never relaxed
only because a test failed.

### Performance and memory gates

- Base frame remains below 10 ms GPU in the dressed M5 fixture at 4K.
- Direct lighting plus standard IBL targets <= 1.4 ms median. Cluster build and both
  deferred/forward consumption are reported separately and summed.
- Scheduled shadow rendering targets <= 1.5 ms median; cache-hit and cold/full
  refresh paths are separate. Update spikes, p95, and p99 remain visible.
- Probe/reflection sampling and scheduled capture work are reported against the 1.5
  ms provisional non-RT GI/probe budget. If probe sampling is fused into lighting,
  matched A/B runs attribute its incremental cost.
- CPU simulation plus render preparation remains below the provisional 4 ms target
  on representative content. Light extraction/upload preparation must scale with
  total scan plus changed uploads and be allocation-free at steady state.
- Cluster candidate memory must match declared arithmetic and graph aliasing. Shadow,
  IBL, probe, baked, upload, and staging memory are separate categories. No feature
  is accepted because VRAM capacity alone hides redundant bandwidth or allocations.
- Sample-car steady C++ allocation median and p99 remain zero. Upload bytes/ranges
  distinguish first publication from unchanged frames.

### Required acceptance matrix

- zero, one, and many directional/point/spot lights;
- attenuation distances, cone boundaries, range window, radius singularity guards;
- standard dielectric/conductor/F0/F90/roughness/normal/two-sided surfaces;
- complex opaque base plus coat/sheen/anisotropy/iridescence diagnostics;
- visible emissive with black-neighbor proof that no implicit GI was added;
- 64/512/4096 local lights and deliberate overflow;
- sun/sky and all IBL roughness/F0/F90 cases;
- directional cascades, spot atlas, point cube seams, moved light/caster/receiver;
- global environment, sphere/box probes, overlap/blend/boundary/correction,
  missing/resident/reimport/invalidation/capture;
- baked-set version/identity/dependency/fallback without claiming a GI solver;
- source/cooked migration/round trips and frozen contract changes;
- material lab, complex lab, sample car, winding/mirror/positive-scale, editor
  create/delete/drop/gizmo/Entity 0 regressions;
- scene-linear, final SDR, scRGB, HDR10, output-transform uniqueness, validation and
  normal cleanup.

## Risks, fallback, and rollback

- **Ambiguous v1 lights are silently relabeled:** use v2 field names, ordered
  migration warnings, numeric preservation, and explicit save. Never infer old
  lumens/lux from comments.
- **Photometric values overflow/darken FP16:** keep physical values in records,
  apply one documented frame-global scale, measure sun/indoor/highlight charts, and
  reject nonfinite output. Do not hide it with car exposure.
- **Color normalization changes hue/energy:** freeze Rec.709-to-AP1 matrices and
  unit-Y normalization in CPU/GPU vectors; reject zero/negative/nonfinite colors.
- **Cluster atomics create flicker or unsafe overflow:** count/scan bounded ranges,
  sort valid lists, and switch the whole product to a CPU deterministic fallback on
  overflow. Never shade a partially race-selected list.
- **16x16 cluster headers/bandwidth cost more than they save:** bake off 32x32 and
  depth counts. The selected dimensions are quality-policy data, not an identity
  contract; retain the faster measured candidate.
- **GPU light ABI blocks M7:** keep stable 64-byte semantics, UUID ownership outside
  the record, transient slots explicit, and extraction replaceable by change streams.
- **Graph extension becomes a Vulkan rewrite:** add only layered/mip/subresource and
  logical-buffer access required by declared RHI contracts; keep execution on the
  current graphics queue.
- **IBL cooker is nondeterministic or compressed error hides energy:** pinned sample
  sequences/tool dependencies, independent-process byte tests, float reference
  products, and separate compressed thresholds.
- **Environment orientation/seams differ across paths:** freeze face basis, handedness,
  texel-center, longitude seam, mip and capture conventions with colored-face tests.
- **Shadow bias trades acne for floating objects:** measure both masks and contact
  offsets; centralize bias/filter conventions and retain reference quality.
- **Shadow memory/update spikes:** on-demand tier pools, stable allocation, cache,
  texel/time budgets, and visible unshadowed/stale fallback. No surprise full refresh.
- **Stale cache uses wrong ownership/projection:** key by UUID and revisions;
  projection-incompatible maps are never sampled.
- **Probe blending pops or double-counts diffuse GI:** deterministic top-two weights,
  influence blend bands, local specular only, global fallback, fixed moving-camera
  captures. M10 owns spatial diffuse GI.
- **Partial capture leaks into production:** publish complete validated cubes only;
  retain the last compatible product or global/black fallback.
- **Baking foundation expands into M10:** implement typed manifests, identity,
  provenance, publication, and neutral fallback only. No charting/solver/streaming.
- **Frozen M4 tests are blindly regenerated:** create a named M5 superseding contract,
  document every schema/manifest/CookKey/hash delta, and keep historical evidence.
- **Dirty worktree overlap loses accepted work:** inspect status/diff before each
  slice, patch narrowly, never clean/reset, and record intentional overlaps.
- **No hardcoded light leaves old scenes black:** migration retains authored lights;
  zero-light scenes intentionally render emissive/environment only. Fixtures and
  editor creation make the outcome obvious; production does not resurrect a demo sun.

## Decisions requested from the owner

Approval outcome: on 2026-08-08 the owner approved all eleven recommended choices
without modification. They are now the implementation authority for M5 unless new
slice evidence requires an explicit amendment.

1. **Physical v2 light schema and migration — recommend approval.** Persist separate
   directional lux and local candela fields, linear Rec.709 color, metres, smooth
   range window, explicit source radius, quality/priority, and warning-producing
   numeric v1 adoption. Alternative: keep a type-dependent `intensity` field; it is
   smaller but makes type changes and units less explicit. Alternative: retain a
   `LegacyUnitless` mode; it preserves ambiguity indefinitely and weakens acceptance.
2. **Photometric scene calibration — recommend physical records plus one measured
   global scale, initial candidate `1e-4`.** Alternative: pre-scale records during
   extraction; smaller shader state but loses direct record-unit readability.
   Alternative: store arbitrary relative radiance; rejected because authored units
   would not connect rigorously to exposure.
3. **GPU light record — recommend the 64-byte four-`float4` record and separate
   shadow/provenance tables.** Alternative: 80/96 bytes with UUID/debug fields;
   simpler inspection but unnecessary upload/cache cost and tempts GPU identity
   authority. Alternative: aggressively pack halves; premature precision risk.
4. **Cluster configuration — accepted 32x32x24 logarithmic after the measured
   bakeoff.** Relative to 16x16x24 it reduces 512-light build time 66.0% and memory
   31.9% for a 3.8% conservative consumer-loop proxy increase. Thirty-two slices are
   slower and produce more references. Linear depth slices remain rejected because
   they waste distant/near precision.
5. **Overflow — recommend whole-product deterministic top-64 fallback plus visible
   diagnostics.** Alternative per-cluster fixed arrays are simple but can consume
   hundreds of MiB at 4K. Alternative atomic truncation is unsafe for deterministic
   fidelity and rejected.
6. **IBL cooking — recommend versioned AP1 radiance/irradiance/GGX/BRDF products with
   float reference and measured BC6H production tiers.** Alternative runtime GPU
   convolution shortens cook time but complicates determinism, startup, and residency.
   Alternative raw equirect lookup is the current incomplete path and rejected.
7. **Directional shadows — accepted four-cascade stabilized conventional owners,
   fixed tent PCF fallback, D32 reference and configurable resolution.** ADR-0009
   has since raised the M5 capacity to two independent owners. ADR-0010 assigns
   contact hardening to M5.9 and measured sparse virtual pages/clip levels to M7;
   variance/moment filtering remains an evidence-gated alternative because of leak
   and precision risks.
8. **Local shadows — recommend spot atlas plus tiered point cube arrays, stable UUID
   allocation, on-demand memory, and budgeted cache.** A single atlas for all six
   point faces simplifies allocation but raises seam/guard complexity. Dedicated map
   per light simplifies ownership but scales descriptors/memory poorly.
9. **Probes — recommend one global environment plus local specular sphere/box probes,
   top-two blending, maximum four candidates, and box projection.** Local diffuse
   probe GI remains M10. Blending every overlapping probe is smoother in pathological
   layouts but divergent/unbounded and not recommended.
10. **Baked interface — recommend a scene-referenced baked-lighting-set manifest with
    per-entity/primitive/volume stable associations.** Per-component renderer texture
    slots are simpler but violate identity/residency boundaries.
11. **ADRs — accepted ADR-0007 with M5.1, ADR-0008 with M5.6, ADR-0009 with the
    M5.7 multi-light correction, and ADR-0010 with the owner's 2026-08-11 fidelity
    direction.** ADR-0009 supersedes only ADR-0008's single-owner capacity; the
    cache-validity rules remain authoritative.
12. **Sky and future visibility — accepted a distinct three-mode Sky component,
    HDRI-first implementation, physically sized hard/soft shadows, sparse virtual
    shadow evolution, RGB translucent visibility, GTAO/CACAO and RTAO scalability,
    and high-end-first project profiles.** Later features must use shared light,
    material, cluster, visibility, and environment contracts.

## Decision log

- 2026-08-08: Read the complete required M4/M5 context, planning format, performance
  contract, M4 plan/acceptance/cutover, ADR-0001/0002/0006, and relevant accepted
  ADR-0003/0004/0005 and M2 closure/cutover evidence before writing this plan.
- 2026-08-08: Confirmed the current Debug and Release build trees enumerate 60 tests;
  no new run is claimed in the planning phase.
- 2026-08-08: Confirmed source baseline and intentionally dirty accepted worktree;
  no unrelated file is cleaned, reverted, committed, or regenerated.
- 2026-08-08: Confirmed Light v1 is fully persisted/transactional but renderer-unused.
  Both deferred and complex-forward independently use the fixed light and raw
  environment approximation.
- 2026-08-08: Confirmed there are no shadow/probe/baked components or resources,
  runtime cubemap products, or graph support for layered/mip image allocation.
- 2026-08-08: Selected explicit v2 migration warnings over silent reinterpretation
  or permanent legacy-unit shading.
- 2026-08-08: Selected one clustered product with sorted normal lists and a whole-
  product deterministic overflow fallback; rejected independent forward lists and
  race-order truncation.
- 2026-08-08: Kept M6/M7/M10/M11 boundaries explicit and treated standard IBL,
  raster shadow foundations, local specular probes, and baked interfaces as M5.
- 2026-08-08: Plan written; implementation is paused for owner approval.
- 2026-08-08: Owner approved every recommended decision; M5.0 became the only active
  slice.
- 2026-08-08: M5.0 completed with frozen CPU equations, deterministic fixture and
  capture manifests, a source/canonical/cooked three-light v1 contract, source-free
  load evidence, and Debug/Release 62-test passes.
- 2026-08-08: Five fixtures completed five independent 4K 10,000-frame processes
  each (250,000 frames, zero drops, source import zero). Validation captures were
  clean and the matched sample-car final SDR remained byte-identical to M4.
- 2026-08-08: Recorded a pre-existing allocation-counter discrepancy (retained
  median/p99 1 call/8 bytes versus M4 zero) as a mandatory investigation/profiler
  coverage item. No production change was made to mask it.
- 2026-08-08: Began and completed M5.1. Light source/cooked version 2, structured
  migration warnings, physical/color reference math, editor unit presentation,
  strict legacy-Area policy, cooker versioning, and named frozen-contract
  supersession landed. Accepted ADR-0007. Debug and Release pass 63/63 tests.
- 2026-08-08: Characterized 1,000-light v1 migration at 26.190 ms median and
  current-v2 read at 21.357 ms; v2 stage/cook is 8.556 ms median. The three-light
  fixture grows 32 bytes. The stable 10,000-target editor transaction rerun is
  0.1072 ms median with the accepted one 120-byte apply allocation and
  allocation-free undo/redo.
- 2026-08-09: Accepted M5.2 with backend-neutral 64-byte light records, UUID-owned
  extraction, revision/range uploads, geometrically grown per-frame Vulkan storage,
  a valid zero-light path, capability/memory telemetry, and 64/64 Debug/Release
  tests. The matched 4K sample-car output remains byte-identical to M5.0.
- 2026-08-09: M5.3 rejected its first serial and under-parallel GPU construction
  attempts after measured 4K costs of 5.6–40.5 ms. Workgroup-parallel count/fill and
  compacted subgroup/shared sorting reduce the accepted 512-light product to
  0.241 ms median / 0.259 ms p95.
- 2026-08-09: Selected 32x32x24 logarithmic clusters over 16x16 and 32-slice
  candidates. The selection saves 31.9% cluster memory and 66.0% build time at 512
  lights for a 3.8% conservative consumer-loop proxy increase.
- 2026-08-09: Accepted M5.3 with one graph product and shared deferred/forward
  descriptor contract, exact counters/readback, deterministically sorted lists,
  whole-product UUID-stable top-64 overflow, occupancy/overflow views, clean normal
  and overflow validation, byte-identical M5.2 final SDR, and 65/65 Debug/Release
  tests. No ADR changed; M5.4 is next.
- 2026-08-09: Accepted M5.4. Descriptor-free physical direct-light equations now
  serve deferred and complex forward through one cluster access contract; all fixed
  production lights are removed. Direct-only 4K standard parity passes at one SDR
  code and 0.999996 mean luma SSIM. Debug/Release pass 65/65, 512-light normal and
  4,096-light fallback validation are clean, and no ADR changed. M5.5 is next.
- 2026-08-09: Accepted M5.5. A deterministic `iridium.environment` product now
  carries AP1 radiance, exact source-texel SH9 irradiance, GGX prefilter, and the
  F0/F90 BRDF LUT through layered/cube RHI and Vulkan contracts. Production no
  longer parses a source HDR; neutral fallback and atomic cooked hot publication
  are validation-clean. Standard deferred/forward 4K IBL luma differs by at most
  0.000001188, complete IBL costs 0.110 / 0.119 ms in deferred, and the resident
  High product adds 20.297 MiB requested. Debug/Release pass 65/65. No ADR changed;
  M5.6 is next.
- 2026-08-09: Began M5.6 with a backend-neutral directional selector, four-cascade
  practical/log split and texel-stabilization math, Vulkan 0-1 shadow transforms,
  strict stale-projection cache/update-budget scheduling, and compare-sampler RHI
  semantics. The new analytic suite brings Debug to 66/66; Vulkan shadow image,
  caster, sampling, and measurement work remains in progress.
- 2026-08-09: Accepted M5.6. One prioritized stable-UUID sun now renders four
  stabilized 2048 D32 cascades into a persistent imported graph resource. Deferred
  and complex forward share 10% cascade blending, 5x5 tent compare PCF, and measured
  bias policy. Static frames publish four cache hits and no pass; moving casters
  refresh all four in 0.0171 / 0.0177 ms. The 64 MiB array, alpha-mask and culling
  variants, capture metadata/debug views, stale-map fallback, and Vulkan validation
  are verified. Debug/Release pass 66/66. ADR-0008 is accepted; M5.7 is next.
- 2026-08-09: Began M5.7 with priority/quality/contribution/UUID-ranked local
  requests, a stable guarded spot atlas, tiered stable point-cube pools, outer-cone
  spot and frozen six-face Vulkan projection math, and rendered-texel cache
  scheduling. Only caster-compatible history can remain stale, bounded to two
  diagnosed frames; incompatible missed updates are unshadowed. Debug passes 67/67
  and the focused Release suite passes. Vulkan vertical slices remain in progress.
- 2026-08-11: Fixed swapchain recreation after clustered lighting by retiring and
  rebinding graph-buffer descriptors; a real resize/maximize/restore/resize sequence
  survives. Added two-owner directional storage and per-light visibility composition,
  project/CLI shadow policy, Project Settings controls, and non-default 1024 Vulkan
  validation. Bloom remains an intentionally skipped graph hook, not an implemented
  effect. ADR-0009 records the multi-light correction; checkpoint evidence is
  `docs/performance/M5.7-multi-directional-resize-2026-08-11.md`.
- 2026-08-11: Landed the first spot-shadow GPU vertical slice: persistent guarded
  atlas, graph pass/resource, stable cached updates, per-light packed-record lookup,
  shared deferred/forward sampling, alpha-mask caster coverage, project/CLI policy,
  Vulkan validation, and a two-owner visual fixture. Evidence is
  `docs/performance/M5.7-spot-shadow-atlas-2026-08-11.md`; point cubes remain.
- 2026-08-11: Added conservative per-submesh spot caster culling using transformed
  world-space spheres and Vulkan clip planes. Unknown legacy bounds remain visible;
  exact tested/culled counters and CPU coverage make the optimization auditable.
- 2026-08-11: Landed tiered point-cube GPU storage, whole-cube cache scheduling,
  six-face raster/culling, per-light packed lookup, and shared seam-safe deferred/
  forward sampling. The two-light validation fixture visibly composes overlapping
  colored shadows; Debug/Release pass 67/67. Evidence is
  `docs/performance/M5.7-point-shadow-pools-2026-08-11.md`.
- 2026-08-11: Accepted ADR-0010 after primary-source research into sparse virtual
  shadow maps, contact-hardening filtering, screen-space contact limitations,
  GTAO/CACAO, colored transmission, RT visibility, and production physical sky.
  Expanded M5/M6-M11 handoffs for high-end-first shadow, AO, atmosphere, and quality
  scalability without asserting undocumented proprietary-engine internals.
- 2026-08-11: Began M5.8 with the stable `iridium.component.sky` three-mode schema.
  HDRI is the implemented mode: `.hdr` now imports as `iridium.environment`, cooks
  through DDC, produces Asset Browser thumbnails, supports typed Inspector drop/
  clear, persists source/cooked GUID intent, and controls shared background/IBL
  intensity, rotation, visibility, lighting participation, and priority. Skybox and
  Simulated keep distinct future settings and explicit pending-render diagnostics.
- 2026-08-11: Extended M5.8 through the renderer publication boundary. Local probes
  now resolve cooked environment residency without dirtying authored state, publish
  stable backend-neutral records and incremental update ranges, and expose a bounded
  four-candidate clustered-assignment oracle shared with the existing light grid.
  Debug and Release build and pass 68/68 tests. Vulkan upload, indexed local-cubemap
  binding, deferred/forward sampling, captures, and GPU measurements remain open.
- 2026-08-11: Completed M5.8. Vulkan now renders and filters six-face scene captures,
  keeps partial work private, promotes complete cubes into the shared local-
  environment table, and supports atomic reusable `.irprobe` baking with SH9
  irradiance and source/CookKey provenance. Debug/Release pass 68/68; targeted baked
  readback and final Release 4K runs are validation-clean. A 512 capture plus filter
  costs 1.234144 ms GPU; steady one-probe overhead is 0.008448 ms median GPU and
  16.03125 MiB committed live VRAM. Final evidence is
  `docs/performance/M5.8-reflection-probe-capture-2026-08-11.md`.

## Completion report

Complete and accepted on 2026-08-13. M5.0-M5.11 establish frozen reference
fixtures; physical Light v2 migration; UUID-owned GPU records; one bounded 32x32x24
cluster product; shared deferred/complex-forward physical direct lighting; cooked
AP1 irradiance, GGX prefilter, and BRDF IBL; independent cached directional, spot,
and point visibility; fixed PCF and physical-source PCSS; three-mode Sky ownership
with production HDRI; clustered local reflection probes with complete-only capture
and `.irprobe` baking; and typed future-GI baked-lighting products.

Debug and Release pass 69/69. Vulkan validation is clean for opposing local shadow
owners, the cooked-HDRI dressed car, SDR/scRGB/HDR10, and normal/final captures.
M5.7's final 20-process on/off gate covers 200,000 native-4K measured frames: two
spots add 0.081248 ms median and two point cubes add 0.406368 ms. The final five-run
dressed car covers 50,000 frames with zero frame/counter drops and measures
4.238432 ms GPU median of medians, 4.484928 ms worst p95, and 4.520544 ms worst p99.
Requested live/peak memory is 1,492.982/1,587.977 MiB and committed live/peak is
1,563.056/1,658.052 MiB, including explicit 128 MiB directional and 400 MiB local
shadow reservations.

Final audit increased bounded profiler counter capacity to retain all M5 counters.
The truthful car result is a constant 39 C++ calls / 5,288 requested bytes per
retained steady frame; this is a documented allocation-efficiency carry-forward,
not hidden or mislabeled as M4's zero-allocation result. Bloom remains an explicitly
skipped zero-cost hook. M6 owns transparent/colored transmission, M10 owns AO,
atmosphere/clouds and GI solvers, and M11 owns hybrid RT. Accepted ADR-0007 through
ADR-0011 own the lasting boundaries. Final evidence is the dated acceptance report
and M5.11 qualification record. M5 is `Accepted`.
