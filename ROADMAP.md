# Iridium Engine Development Roadmap

## Purpose

This is the durable program-level roadmap for the engine after the RHI refactor. It records milestone order, dependencies, acceptance gates, and work intentionally deferred. Detailed execution plans follow `PLANS.md` and live under `docs/milestones/`.

Status values: `Proposed`, `Ready`, `In Progress`, `Blocked`, `Accepted`.

## Product and performance goals

- High-fidelity, future-facing Vulkan renderer for high-end PCs.
- 4K HDR gameplay at more than 100 FPS on the reference system, using native rendering, DLAA, or high-quality temporal reconstruction as appropriate.
- Linear scene-referred HDR lighting and transparency with SDR, scRGB, HDR10, and useful wide-gamut output paths.
- A complete high-quality raster path before ray tracing becomes required for baseline lighting.
- Scalable CPU/GPU architecture: GPU scene, indirect visibility, mesh shaders, and later ray tracing.
- Modular assets, scenes, ECS, editor, and serialization suitable for an expanding engine rather than one demonstration model.

Frame generation is not counted as meeting the engine's simulation or base-rendering frame-time target.

## Accepted architectural direction

The ADRs contain the full rationale. The current direction is:

1. Use a render graph for pass dependencies, resource state, transient allocation, history, and external SDK integration.
2. Keep opaque lighting, emissive, transparency, bloom, and temporal effects in linear HDR. Tone-map and gamut-map once at output.
3. Preserve rich source material workflows. Compile ordinary single-closure materials to a canonical deferred closure containing diffuse albedo, F0, and roughness.
4. Render genuinely multi-lobe, transmissive, or otherwise uncommon closures through a forward complex-material path using the same BSDF library.
5. Use stable GUID-based source assets, metadata sidecars, dependency tracking, and a derived-data cache. Separate import, cooking, and GPU upload.
6. Use a versioned JSON editor scene format and a cooked binary runtime format. Register component serializers by stable component IDs.
7. Move component drawers and editor interaction out of ECS component types.
8. Build a persistent GPU scene and indexed indirect path before adding a mesh-shader emission path.
9. Preserve meshlet, bounds, material, and triangle data needed by classic raster, mesh shaders, and BLAS construction.
10. Use classified transparency paths rather than one universal sorting or OIT algorithm.

## Milestones

### M0 - Reference scenes and profiling foundation

Status: `Ready`

Establish trustworthy baselines before changing renderer architecture.

Deliverables:

- CPU frame-stage timers and Vulkan GPU timestamps.
- Per-frame draw, triangle, material, pipeline, transparent-overdraw, and VRAM statistics.
- Reference scenes for material spheres, the sample car, transparent nesting, emissive range, lighting, and large-scene CPU stress.
- Deterministic screenshot capture and reference-image comparison workflow.
- Debug views for base color/diffuse, F0 or metallic, roughness, normals, emissive, depth, material ID, and motion vectors as they become available.
- Recorded baseline on the reference PC at 4K in Debug and Release where meaningful.

Acceptance gate: `docs/milestones/M0-reference-and-profiling.md` is satisfied and baseline results are recorded.

### M1 - Render graph, linear HDR, and color management

Status: `Proposed`

Dependencies: M0.

Deliverables:

- Backend-neutral render-graph declarations with Vulkan execution and barriers.
- Transient, persistent, and history resource classes.
- FP16 or equivalently suitable linear scene-color target.
- Opaque and transparent composition before tone mapping.
- Exposure, bloom integration point, final tone mapping, and gamut mapping.
- SDR output plus negotiated scRGB/HDR10 paths and HDR metadata where supported.
- Display/paper-white/peak-nits configuration and color-managed UI composition.

Acceptance gate: opaque, emissive, sky, and glass are composed in one linear HDR pipeline; SDR and at least one HDR path validate on target hardware.

### M2 - Material compiler and canonical shading closure

Status: `Proposed`

Dependencies: M0, M1.

Deliverables:

- `SourceMaterial`, compiled material, material instance, and packed GPU material separation.
- Rich glTF material parsing with support policy and diagnostics for relevant Khronos extensions.
- Canonical standard closure: diffuse albedo, F0, perceptual roughness, normal, AO, emissive, and model/feature flags.
- Shared BSDF library used by deferred and forward rendering.
- Reference and production GBuffer layouts with image-difference validation.
- Clear classification between standard deferred and complex forward closures.
- Material inspector/debug reporting showing source values, defaults, compiled values, textures, color spaces, and warnings.

Acceptance gate: glTF metallic/roughness and specular/gloss reference assets produce consistent, explainable results; the sample car's source/default/extension behavior is visible and validated.

### M3 - Asset registry, cooker, and browser

Status: `Proposed`

Dependencies: M0; coordinate with M2 and M6 data needs.

Deliverables:

- Stable asset GUIDs and source-controlled metadata sidecars.
- Importer registry, importer versions, settings hashing, dependency graph, and derived-data cache.
- Separation of source parsing, CPU cooking, runtime blobs, and scheduled GPU upload.
- Model and texture import settings, reimport, validation, thumbnails, search, filtering, and drag/drop.
- Asset browser as the primary import/assignment workflow; remove import responsibility from the scene hierarchy.
- Cooked geometry retains spatial primitive boundaries, transparent-surface bounds, optional meshlets, and RT-compatible source data.

Acceptance gate: assets can be imported, reimported, referenced by GUID, assigned from the browser, and rebuilt deterministically from source plus metadata.

### M4 - Scene schema, ECS identity, and editor separation

Status: `Proposed`

Dependencies: M3 for asset references.

Deliverables:

- Stable scene entity UUIDs distinct from runtime generational entity handles.
- Versioned top-level schema and per-component versions.
- Explicit component serializer/migration registry and multi-phase reference fixup.
- Deterministic JSON source scenes, unknown-component preservation, atomic saves, and cooked binary scenes.
- Cache-friendly sparse component lookup and query/view improvements based on measured needs.
- Editor-only component drawer registry with undo-aware property editing.
- Runtime components no longer depend on ImGui or native file dialogs.

Acceptance gate: component addition does not require modifying a central scene serializer, old fixtures migrate, unknown data survives an editor round trip, and runtime ECS builds without editor UI dependencies.

### M5 - Raster lighting, shadows, probes, and baking foundation

Status: `Proposed`

Dependencies: M1, M2; asset integration from M3 as available.

Deliverables:

- ECS lights uploaded to GPU light buffers.
- Tiled or clustered light assignment for deferred and forward paths.
- Physically consistent direct lighting and image-based lighting with irradiance, prefiltered specular environment, and BRDF integration.
- Directional, spot, and point shadow-map foundations with an explicit quality/caching policy.
- Reflection/environment probe system and cubemap capture/cooking.
- Interfaces for lightmaps, irradiance volumes/probes, and other non-RT baked GI data.

Acceptance gate: no hardcoded demonstration light is required; deferred and forward materials agree under direct lights and IBL; shadow and probe costs are measured at 4K.

### M6 - Hybrid transparency

Status: `Proposed`

Dependencies: M1, M2, M3, M5.

Deliverables:

- Per-transparent-primitive bounds and deterministic depth/priority keys.
- `AlphaClip`, `SortedSurface`, `ThinGlass`, `LayeredGlass`, and weighted-OIT material modes.
- Correct front/back or thickness data for thin closed glass.
- Two-layer baseline and scalable additional peels for nested hero glass.
- Rough refraction using the HDR scene-color pyramid and defined off-screen fallbacks.
- Tile/bounds restriction, early termination, and quality-tier controls.

Acceptance gate: the sample car windows and headlights render predictably, normal/roughness detail remains visible, nested reference scenes are stable, and costs fit the transparency budget.

### M7 - GPU scene and indirect visibility

Status: `Proposed`

Dependencies: stable M2/M3/M4 identities and M0 profiling.

Deliverables:

- Persistent GPU buffers for instances, current/previous transforms, geometry, materials, bounds, and visibility metadata.
- Compact CPU update streams rather than repeated full draw packets.
- Compute frustum/LOD culling, visibility compaction, and indexed indirect-count submission.
- Hi-Z occlusion path with temporal conservatism and diagnostics.
- Shared visible-instance representation for raster, shadows, and future RT updates.

Acceptance gate: CPU render preparation and submission scale primarily with changed data rather than total draws, with a classic indexed fallback retained.

### M8 - Meshlet cooker and mesh-shader path

Status: `Proposed`

Dependencies: M3, M7.

Deliverables:

- Offline meshlet construction with tunable vendor-neutral limits, local indices, bounds, and normal cones.
- Mesh-shader capability detection and pipeline path using `VK_EXT_mesh_shader`.
- Meshlet culling and indirect mesh-task dispatch consuming the M7 visibility architecture.
- Benchmarks against indexed indirect rendering; select the faster path per workload/capability.

Acceptance gate: mesh shaders produce matching images and a measured benefit on appropriate high-geometry scenes without becoming a mandatory regression for small meshes.

### M9 - Temporal rendering and reconstruction

Status: `Proposed`

Dependencies: M1, M2, current/previous data from M7.

Deliverables:

- Stable jitter, current/previous matrices, skinned motion, depth, exposure, reactive data, and history invalidation.
- Native TAA/DLAA-quality reference path.
- Vendor-neutral super-resolution interface and Vulkan SDK/plugin requirement negotiation.
- DLSS integration first, with room for FSR/XeSS providers.
- Dynamic-resolution policy and objective ghosting/disocclusion tests.

Acceptance gate: high-quality reconstruction is stable in motion, transparencies provide appropriate reactive behavior, and displayed versus base-render frame rates are reported separately.

### M10 - Non-ray-traced GI production paths

Status: `Proposed`

Dependencies: M3, M5, M9 as appropriate.

Deliverables:

- Production-ready selection of baked lightmaps, irradiance probes/volumes, screen-space techniques, and probe updates.
- Streaming, invalidation, and authoring workflows.
- Quality/performance comparisons and fallbacks for dynamic objects.

Acceptance gate: fully dressed scenes have a credible non-RT indirect-lighting solution within the 4K frame budget.

### M11 - Hybrid ray tracing and reference path tracing

Status: `Proposed`

Dependencies: M2, M3, M5, M7, M9; baseline raster and non-RT solutions accepted.

Deliverables:

- RHI and Vulkan acceleration-structure/resource support.
- BLAS cooking/build policy and TLAS update system using stable GPU-scene instances.
- Progressive hybrid effects: shadows, AO, reflections, GI, then transmission as justified.
- Denoiser integration using correct motion/depth/normal/material demodulation inputs.
- Reference/photo-mode path tracer for visual validation.

Acceptance gate: RT features are optional capabilities, share material/light/scene data with raster, and have measured quality and performance fallbacks.

## Program controls

- Do not start a milestone before its dependencies and acceptance criteria are understood.
- Every milestone begins with a checked-in execution plan following `PLANS.md`.
- Architectural changes require an ADR or explicit update to an existing proposed ADR.
- Every milestone ends with a review, verification report, and updated baseline.
- New features discovered during implementation enter the roadmap explicitly; they must not disappear into chat-only follow-up notes.

