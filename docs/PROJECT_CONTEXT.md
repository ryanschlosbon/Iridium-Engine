# Iridium Engine Project Context

## Why this document exists

This is the compact handoff for new lead tasks. It records facts found during the post-RHI-refactor architecture review, accepted direction, and unresolved choices. It is not a substitute for reading current source, the roadmap, milestone plans, and ADRs.

## Product intent

Iridium is intended to be a visually ambitious, high-end engine rather than a lowest-common-denominator renderer. The reference PC is an RTX 4090, Core i9-14900K, 64 GB DDR5-6000, a 2 TB Samsung 990 Pro, and a 4K HDR display. The target is more than 100 FPS in fully dressed active gameplay scenes, with native rendering or high-quality temporal reconstruction chosen intentionally.

The engine should eventually support wide-gamut HDR, GPU-driven submission, mesh shaders, DLSS-class reconstruction, hybrid ray tracing, and a reference path tracer. Before those become dependencies, it needs a complete raster foundation: lights, shadow maps, image-based lighting, probes/cubemaps, baking, and credible non-RT GI.

## Current implementation facts from the architecture review

These facts must be revalidated as the refactor continues.

### Rendering and draw data

- The application currently builds draw packets per submesh. A full model matrix is repeated in each packet; the observed packet was approximately 112 bytes before container and alignment overhead.
- Transparent sorting uses a distance derived from the entity origin rather than a per-transparent-primitive bound or depth interval.
- The Vulkan transparency path treats the last packet as foreground and the remainder as background. This is not general depth peeling and cannot correctly model arbitrary nesting.
- Import-time merge-by-material can join disconnected transparent surfaces and discard the spatial boundaries required for useful sorting, culling, or layer classification.
- The deferred GBuffer currently uses three `RGBA16F` targets, or 24 bytes per pixel before depth and other surfaces. That is a good high-quality reference layout, but bandwidth and cache pressure must be measured rather than inferred from VRAM capacity.
- Observed draw push constants were approximately 120 bytes. This is close enough to common guaranteed limits that future additions should move to indexed GPU data rather than growing the block.

### Materials and the sample car

- The material asset contains albedo, normal, metallic/roughness, emissive, and transmission data. The importer currently handles emissive-strength and transmission extensions, but the reviewed path did not fully parse clearcoat or specular extensions.
- The shaders do consume per-material base color, metallic, and roughness factors; the car is not simply rendered with one universal hardcoded metallic/roughness value.
- In the inspected sample car, 25 of 87 materials omitted `metallicFactor`, which means the glTF default of `1.0`; 61 explicitly specified `0.0`. A red paint material omitted metallic, used roughness around `0.523`, and declared clearcoat. A glass material used transmission `1.0` and also omitted metallic. If clearcoat is ignored and omitted values are accepted without diagnostics, the result can look implausibly dark or metallic even while technically following individual glTF defaults.
- The current lighting is a major confounding factor: it includes demonstration/hardcoded behavior and the environment path does not yet implement a complete irradiance plus prefiltered-specular IBL solution with a BRDF integration term.
- Scene color is tone-mapped before the current glass work, and glass applies tone mapping again. The observed scene-color resource also follows a swapchain-like format. Transparency and emissive therefore are not consistently composed in one linear HDR scene.

### ECS, editor, assets, and scenes

- Component pools use dense vectors with an `unordered_map` sparse lookup. This can be adequate for iteration, but entity-to-component lookup, allocation behavior, stable identity, and query construction should be measured before redesign.
- Components expose inspector behavior such as `DrawInspector`/`OnInspector`, pulling ImGui and editor concerns into runtime data types.
- Mesh component/editor behavior includes asset selection or file-dialog responsibilities. Those belong in editor drawers and asset-browser services.
- Scene serialization is manual and centrally coordinated. The reviewed path lacks a durable top-level schema version, per-component versions, stable entity UUIDs, a component serializer registry, migrations, and robust unknown-component preservation.
- The asset manager largely uses paths as identity and combines source import, cooking, caching, and GPU upload responsibilities. This makes deterministic reimport, dependencies, metadata, headless cooking, and background work difficult.
- Model import currently appears in the scene hierarchy. The intended workflow is an asset browser plus a menu-level import action; hierarchy and entity inspectors should assign already registered assets.

## Accepted architecture

- Preserve rich source materials, but compile ordinary single-scattering surface materials to a canonical runtime closure: diffuse albedo, specular F0, perceptual roughness, normal, AO, and emissive.
- Converting glTF specular/glossiness to that closure can be physically faithful for a single dielectric/conductor microfacet model. Conversion is lossy when the source represents multiple lobes, coatings, volume transmission, sheen, anisotropy, or other closures; classify those into a forward complex-material path.
- Supporting F0 directly avoids treating metallic as the only possible specular control. Authoring workflows may remain metallic/roughness, specular/glossiness, or extension-based; runtime storage should follow the evaluated closure.
- Keep a high-precision reference GBuffer first. Introduce a production-packed layout only after image-difference, frame-time, and bandwidth evidence. VRAM capacity does not remove bandwidth, cache, ROP, or synchronization costs.
- Share BSDF code and material data semantics across deferred, forward, and future ray-tracing paths.
- Use hybrid transparency classification. Default inexpensive paths should handle ordinary surfaces; hero/nested/refractive glass can request bounded extra layers or a complex forward path.
- Establish a render graph and linear HDR/color-managed pipeline before adding more major lighting features.
- Build stable asset, scene, and component identities before a persistent GPU scene depends on them.
- Add indexed indirect GPU-driven rendering before mesh shaders. Mesh shaders should reuse the same GPU scene, visibility data, and cooked meshlets rather than create a separate renderer.

## Important unresolved choices

- Exact production GBuffer formats and whether material/model IDs share a target.
- The standard closure's supported lobe set and the precise classification threshold for forward complex materials.
- Bindless descriptor strategy and RHI capability contract.
- Render-graph ownership model, scheduling granularity, and transient allocator.
- HDR output APIs and the engine's internal working color space. Linear Rec.2020 or an ACES-oriented scene space are candidates; display conversion must remain explicit.
- Asset metadata representation and cooked binary container format.
- Scene source format details, registry API, and migration policy limits.
- Transparency algorithms and quality tiers after representative captures and timing data exist.
- Native TAA/reconstruction baseline and external SDK abstraction.

## Immediate next step

Execute M0, not a renderer rewrite. Establish reference scenes, material diagnostics, repeatable captures, CPU/GPU timings, and memory/counter baselines. Then audit and approve M1's render-graph/HDR design using evidence from those tools.

The first milestone plan is `docs/milestones/M0-reference-and-profiling.md`.
