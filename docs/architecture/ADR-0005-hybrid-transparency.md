# ADR-0005: Classified Hybrid Transparency

- Status: Accepted direction; algorithms and tiers to validate in M6
- Date: 2026-07-17
- Owners: Renderer, material compiler, and asset cooker

## Context

No single real-time transparency technique is best for ordinary alpha surfaces, thin glass, nested hero glass, particles, hair-like coverage, and rough refraction. Global primitive sorting fails for intersecting or nested geometry. Weighted blended OIT is fast and stable but approximate, especially for strong opacity/color layering and refraction. Full per-pixel linked lists or unbounded peeling can be expensive at 4K. The current two-bucket behavior is not true two-layer rendering, and import-time merge-by-material destroys useful spatial boundaries.

## Decision

Classify transparent materials and geometry into explicit paths:

- `AlphaClip`: opaque/deferred depth path with coverage testing where appropriate.
- `SortedSurface`: back-to-front per-primitive or per-draw sorting for simple surfaces.
- `ThinGlass`: specialized forward transmission/reflection using front/back or thickness information.
- `LayeredGlass`: bounded depth peeling or equivalent accurate layers for nested/hero glass.
- `WeightedOIT`: stable approximate accumulation for particles and high-overdraw surfaces that do not require accurate refractive ordering.

The material compiler selects a safe default and permits an author override. The cooker preserves transparent primitive bounds and does not merge disconnected surfaces merely because they share a material.

At runtime, work is restricted by object bounds, tiles, depth ranges, and material class. Layered paths use a small default layer count, early termination, adaptive escalation for marked hero materials, and explicit overflow diagnostics/fallback. Rough refraction samples a linear-HDR scene-color pyramid. All transparent paths compose before final tone mapping.

## Consequences

- Visual fidelity is spent where complexity is present instead of charging every transparent pixel equally.
- Draw sorting uses per-primitive depth intervals and deterministic priority rules rather than entity origins.
- Thin closed meshes need validated winding, thickness/front-back information, or a documented approximation.
- Normal, roughness, absorption, IOR/F0, and emissive behavior use the same material conventions as opaque/complex forward shading.
- M0/M6 reference scenes and GPU timings determine layer counts and quality tiers.
- Ray-traced transmission may later be another classified path, not a replacement for all raster transparency.

## Rejected alternatives

- One global sort: cannot resolve cycles or per-pixel nesting.
- Weighted OIT for everything: attractive performance and stability, but insufficient for accurate hero glass and refraction.
- Unbounded peeling or linked lists for everything: excessive and unpredictable memory/bandwidth at 4K.
- A fixed two-bucket scheme: not a general representation of two physical layers and depends on fragile submission order.
