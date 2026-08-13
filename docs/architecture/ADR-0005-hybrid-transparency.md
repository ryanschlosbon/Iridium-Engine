# ADR-0005: Classified Hybrid Transparency

- Status: Accepted direction; algorithms and tiers to validate in M6
- Date: 2026-07-17
- Last updated: 2026-07-22
- Owners: Renderer, material compiler, and asset cooker

## Context

No single real-time transparency technique is best for ordinary alpha surfaces, thin glass, nested hero glass, particles, hair-like coverage, and rough refraction. Global primitive sorting fails for intersecting or nested geometry. Weighted blended OIT is fast and stable but approximate, especially for strong opacity/color layering and refraction. Full per-pixel linked lists or unbounded peeling can be expensive at 4K. The current two-bucket behavior is not true two-layer rendering, and import-time merge-by-material destroys useful spatial boundaries.

## Decision

Classify transparent materials and geometry into explicit paths:

- `AlphaClip`: opaque depth/visibility path when a bounded opacity evaluation can
  determine coverage; otherwise an explicitly classified forward path.
- `SortedSurface`: back-to-front per-primitive or per-draw sorting for simple surfaces.
- `ThinGlass`: specialized forward transmission/reflection using front/back or thickness information.
- `LayeredGlass`: bounded depth peeling or equivalent accurate layers for nested/hero glass.
- `WeightedOIT`: stable approximate accumulation for particles and high-overdraw surfaces that do not require accurate refractive ordering.

The material compiler selects a safe default and permits an author override. The cooker preserves transparent primitive bounds and does not merge disconnected surfaces merely because they share a material.

At runtime, work is restricted by object bounds, tiles, depth ranges, and material class. Layered paths use a small default layer count, early termination, adaptive escalation for marked hero materials, and explicit overflow diagnostics/fallback. Rough refraction samples a linear-HDR scene-color pyramid. All transparent paths compose before final tone mapping.

Once M5 establishes clustered lighting, opaque-complex and transparent forward paths
consume the same cluster/light records as deferred or visibility-resolved standard
surfaces. Transparency does not create a second light-assignment system. Visibility
buffering does not replace the ordered or multi-layer visibility required by these
transparent classes.

## Consequences

- Visual fidelity is spent where complexity is present instead of charging every transparent pixel equally.
- Draw sorting uses per-primitive depth intervals and deterministic priority rules rather than entity origins.
- Thin closed meshes need validated winding, thickness/front-back information, or a documented approximation.
- Normal, roughness, absorption, IOR/F0, and emissive behavior use the same material conventions as opaque/complex forward shading.

## M2 implementation note

M2 separates opaque complex closures from transparent transport. Opaque clearcoat,
sheen, anisotropy, and iridescence use a depth-writing `forward-opaque` pass.
Coverage blend and transmissive closures alone enter the retained bounded two-layer
transport. This is an interface and correctness baseline, not M6's production
ordering, OIT, peeling, nested-glass, or refraction implementation.
- M0/M6 reference scenes and GPU timings determine layer counts and quality tiers.
- Ray-traced transmission may later be another classified path, not a replacement for all raster transparency.

## Rejected alternatives

- One global sort: cannot resolve cycles or per-pixel nesting.
- Weighted OIT for everything: attractive performance and stability, but insufficient for accurate hero glass and refraction.
- Unbounded peeling or linked lists for everything: excessive and unpredictable memory/bandwidth at 4K.
- A fixed two-bucket scheme: not a general representation of two physical layers and depends on fragile submission order.
