# ADR-0012: Versioned Transparency Transport and Bounded Execution

- Status: Accepted; M6 implements the decision in sequential slices
- Date: 2026-08-13
- Accepted: 2026-08-13
- Last updated: 2026-08-14
- Owners: Renderer, material compiler, asset cooker, RHI, and editor
- Refines: ADR-0005; does not supersede its classified-hybrid direction

## Context

ADR-0005 selects classified hybrid transparency, but the accepted direction does
not by itself define a versioned source policy, deterministic class resolution,
metric thickness, bounded quality tiers, stable execution fallbacks, or the schema
boundaries needed by the cooker and GPU material table. The retained M2 path infers
one transparent queue from alpha/transmission and executes a two-bucket full-screen
copy/depth/forward bridge. Expanding that queue enum would conflate material intent,
coverage, and backend scheduling and would not survive future visibility or ray
paths.

M6 also requires predictable native-4K memory and time. Unbounded per-pixel lists,
unrestricted peeling, weighted OIT for refractive glass, and implicit fallback are
incompatible with that requirement.

## Decision

### Versioned policy and independent semantics

Iridium uses backend-neutral `TransparencyPolicyV1`, keyed in versioned asset
metadata by stable material or primitive subasset GUID. The policy stores:

- Auto or an explicit `AlphaClip`, `SortedSurface`, `ThinGlass`, `LayeredGlass`, or
  `WeightedOIT` request;
- `Ordinary2`, `Hero4`, or `Cinematic8` bounded quality;
- signed author priority;
- nonnegative effective thin-sheet thickness in metres.

Coverage remains opaque/mask/blend and is not a transparency class. Execution phase
is derived from resolved class, closure, topology, capability, and active comparison
mode. Source data does not contain Vulkan state or ImGui state.

Schema-1 model settings migrate deterministically to Auto, Ordinary2, priority zero,
zero thin thickness, and `LegacyTwoBucket` execution. Unknown policy schemas, enum
values, fields, invalid GUIDs, and invalid numeric values produce stable diagnostics
and normalized safe values. Normalized policy bytes participate in CookKeys.

### Resolution and fallback

Auto resolves non-transmissive mask to AlphaClip, non-transmissive blend to
SortedSurface, thin or diffuse transmission to ThinGlass, and validated positive
closed volume to LayeredGlass. Until topology is validated, positive volume remains
a diagnosed ThinGlass-safe result. Auto never selects WeightedOIT.

Explicit incompatible choices retain the request and expose a fallback flag:

- transmissive/volume SortedSurface falls back to ThinGlass;
- LayeredGlass without validated closed topology falls back to ThinGlass;
- refractive, volume, or dispersive WeightedOIT falls back to SortedSurface;
- incompatible AlphaClip falls back through Auto.

Fallback never rewrites serialized author intent silently. Requested and resolved
classes, quality, flags, priority, and thickness remain inspectable at source,
compiled, cooked, runtime, and packed-GPU boundaries.

### Schema and ABI policy

M6.1 advances compiled material, compiled-material product, model importer/settings,
cooked-model, and packed-GPU schemas. The packed material remains 832 bytes. Its
12-byte tail stores a packed requested/resolved class, quality and flags word, signed
priority, and float thin thickness. Normal-Z reconstruction moves to a dedicated
feature bit. Shader and CPU schema constants advance together; unsupported products
are rejected and recooked rather than guessed.

Stable primitive policies are preserved independently of material policy in cooked
and runtime primitive records. M6.2 uses that record for deterministic work routing;
M6.1 deliberately keeps pixels on the named LegacyTwoBucket bridge.

### Bounded execution

Subsequent M6 slices implement:

- depth-interval SortedSurface work with stable persistent ties;
- ThinGlass using shared BSDF/probe/cluster records and metric transport;
- one conditional scene-linear AP1 color pyramid and conservative depth pyramid;
- LayeredGlass as bounded interface peeling in packed islands at 2/4/8 interfaces,
  with explicit area caps, early termination, and a deterministic residual tail;
- FP16 weighted blended OIT only for explicitly approximate non-refractive work.

All composition remains scene-linear before the single output transform. Main depth
is read-only for transparent work. Transparent surfaces remain excluded from M6
scene-probe capture. Graph resources and descriptors remain frame-context owned and
fence retired.

`LegacyTwoBucket` is a named comparison mode through qualification. Classified
execution becomes the production default only after its routing and visual gates
pass; unsupported per-class work uses the explicit local fallbacks above.

## Consequences

- Material truth survives changes in raster execution, visibility buffering, Vulkan
  implementation, and future ray tracing.
- Every policy change deterministically changes compiled identity and CookKey.
- Old cooked products are intentionally rejected and recooked.
- Primitive overrides can differ from a shared material without cloning material
  identity or mutating source glTF.
- Hero cost is explicit and bounded; it cannot become an Auto default.
- Diagnostics can distinguish author request, resolved class, fallback reason,
  coverage, quality, priority, and metric thickness.
- M6.1 changes schemas and inspection only; pixel equivalence is maintained by the
  LegacyTwoBucket execution bridge.

## Rejected alternatives

- Expand `RenderQueue` into source-facing transparency classes: conflates intent and
  scheduling and leaks current raster organization into assets.
- Store policy only in editor widgets or Vulkan pipeline state: headless cook,
  runtime publication, and future backends would disagree.
- Mutate glTF extensions for engine policy: fragile under reimport and lacks stable
  primitive override identity.
- Auto-select WeightedOIT: approximate ordering is an author-visible fidelity choice.
- Treat requested LayeredGlass as valid before topology proof: manufactures volume
  semantics and hides unsafe content.
- Preserve old binary schemas by interpreting spare bytes without version changes:
  makes cache/product compatibility unverifiable.
- Use unbounded PPLL or global peeling: native-4K memory, atomics, overflow, and tail
  cost are not predictably bounded.

## Required evidence

- deterministic source/settings migration and normalized CookKeys;
- source/compiled/product/model/runtime/GPU round trips for every class and field;
- explicit unknown/incompatible/topology fallback diagnostics;
- LegacyTwoBucket scene-linear and final-output pixel equality during M6.1;
- per-slice validation, timestamps, requested/committed memory, fragment/layer/
  overflow counters, and zero incremental steady-frame allocations;
- matched native-4K ordinary, hero, and OIT qualification before final cutover.
