# ADR-0003: Persistent GPU Scene Before Mesh Shaders

- Status: Accepted direction; implementation details belong to M3, M7, and M8
- Date: 2026-07-17
- Owners: Renderer, geometry cooker, and RHI

## Context

Current per-submesh draw packets repeat transforms and other data, causing CPU preparation and submission work to scale with draws. Mesh shaders alone do not solve scene updates, material indirection, visibility, LOD, shadow submission, or ray-tracing instance construction. A future renderer needs one durable scene representation that can feed classic indexed rasterization, mesh shaders, shadow passes, and acceleration structures.

## Decision

Build a persistent GPU scene with stable indexed records for instances, geometry, materials, transforms, bounds, and visibility metadata. CPU work primarily uploads compact changes. Compute visibility produces compacted instance/draw data and indirect commands.

Implement indexed indirect-count rendering first as the portable high-performance path. Add mesh shaders as a second emission path consuming the same scene and visibility architecture.

At import/cook time, preserve geometry data needed for:

- classic vertex/index rendering;
- LOD and spatial primitive bounds;
- meshlet generation with bounds and normal cones;
- transparent primitive boundaries;
- future BLAS construction and rebuild policy.

Mesh-shader use is capability- and workload-selected. It is not mandatory for small or unsuitable draws.

## Consequences

- CPU draw packets become update or work-request records rather than owners of duplicated render state.
- Current and previous transforms live in stable indexed data for motion vectors and temporal techniques.
- Material and geometry identifiers must remain stable across the asset/runtime boundary.
- Culling diagnostics and a classic CPU/indexed fallback remain available.
- RHI abstractions describe indirect and mesh-shader capabilities without exposing Vulkan implementation details to scene code.

## Rejected alternatives

- Add mesh shaders directly to the current draw loop: duplicates architecture and leaves CPU scaling problems intact.
- Replace indexed rendering entirely: discards a useful fallback and may regress workloads that do not benefit from mesh shaders.
- Make ray tracing own a separate scene database: creates synchronization and material/identity divergence.
