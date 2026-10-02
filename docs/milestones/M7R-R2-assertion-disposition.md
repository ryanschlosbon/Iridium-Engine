# M7R R2.1 — source-text assertion disposition

Steps R2.1b/R2.1c replace every test that opened files under `src/`,
`assets/shaders/*.glsl|vert|frag|comp` or `CMakeLists.txt` for text matching.
Line ranges refer to the files at `6b000ad`. Approximate check counts are given
per group (about 445 removed in total).

## Replacement tests

| Test (label) | What it checks |
|---|---|
| `ShaderAbiContractTests` (unit) | SPIR-V member offsets, extents and array strides against `offsetof`/`sizeof` for every record that C++ and GLSL share; compute LocalSize; push-block layouts; vertex-input locations; one consistent lighting descriptor set across the deferred (set 0) and forward (set 3) consumers, including descriptor kinds and image dimensions. |
| `ShaderOwnershipTests` (unit) | Include ownership from the glslc depfiles, and linked-function ownership from the SPIR-V function table (glslang drops uncalled functions). Covers the shared BSDF, one clustered-light representation, shared shadow/IBL code, the material-body tier permutations, AP1 inputs, and the single output transform. |
| `ProductionGraphContractTests` (unit) | The compiled `buildVulkanProductionRenderGraph`: pass order, read/write wiring and load ops for the Ordinary2, deep-tier and WeightedOIT paths; `TransferSource` only on readback/capture hooks; scene-linear AP1 until the single output transform; one cluster product read by every lit consumer. |
| `VulkanPipelineContractTests` (vulkan) | Headless creation of the production layered-capture, local-composition, scene-resolve, WeightedOIT, transparency-pyramid, shadow (directional/spot/point, all variants), probe-capture, cluster, probe-cluster and depth-pyramid pipelines. Requires zero validation errors. |
| `VulkanShaderParityTests` (vulkan) | GPU/CPU parity: `scene_color.glsl` vs `SceneColor.h`; `direct_lighting.glsl` on extractor-packed lights vs `LightingReference.h`; `material_bsdf.glsl` (standard and canonical entry points) vs `StandardMaterialShading.h`; the four production `*_compact.comp` vs the CPU LOD selectors (`selectGpuSceneDensity/LodGeometry`, `selectGpuSceneRadialLodGeometry`), including consumer-mask, disabled-instance, excluded-instance and malformed-chain cases. |

Support code is in `tests/support/`: `SpirvInspector`, `ShaderDepfile`,
`GraphQuery`, `HeadlessVulkanDevice`, `HeadlessComputeKernel` and `TestHarness`.
The parity kernels are in `tests/shaders/`.

## Disposition

### Stage3ArchitectureTests (15 cases)

**Group 1 — device indirect/LOD contracts**

| Case (lines) | What it asserted | Disposition |
|---|---|---|
| `testReflectionProbeDeviceCommandContract` (561–599, 12) | Shader text: excluded instance, probe consumer bit, `atomicAdd`, `selectedGeometry`, face-resolution scale, `maximumLod`. | Replaced: `VulkanShaderParityTests` (reflection_probe_capture_compact parity) and `ShaderAbiContractTests` "GPU-scene compaction ABI" / "shadow and probe table ABI". |
| | Backend text: `vkCmdDrawIndexedIndirectCount`, counter names, oracle error string. | Deleted: backend call text. Covered by F5 captures and oracle runs, per the design. |
| `testDirectionalShadowDeviceLodContract` (601–628, 7) | Shader text: selector, texel world size, `maximumLod`, error threshold. | Replaced: `VulkanShaderParityTests` (directional_shadow_compact parity) and the compaction ABI. |
| | Backend selector call, counter name, error string. | Deleted: backend call text. |
| `testPointShadowDeviceLodContract` (630–658, 7) | Shader text: selector, light position, face-resolution scale. | Replaced: point_shadow_compact parity and the compaction ABI. |
| | Backend text. | Deleted: backend call text. |
| `testSpotShadowDeviceLodContract` (660–689, 7) | Shader text: selector, `worldToShadowClip * world`, tile size. | Replaced: spot_shadow_compact parity and the compaction ABI. |
| | Backend text. | Deleted: backend call text. |

**Group 2 — layered Vulkan objects**

| Case (lines) | What it asserted | Disposition |
|---|---|---|
| `testOrdinary2ProjectionAndRequestCollection` (1677–1748, 25) | Backend `submitForwardQueues`/capture-recording text: residency observe, collect/prepare/record calls, entry-before-exit capture, viewport/scissor/flags, counter names. | Order and wiring replaced: `ProductionGraphContractTests` "Ordinary2 composition wiring". Recording calls and counter names deleted (implementation; covered by `--validate-ordinary2-capture`). The behavioral collector/plan/allocation checks (1561–1675) are kept. |
| `testOrdinary2VulkanLocalCompositionObjectContract` (1842–1938, 33) | Pass source: format, clear, 5 set layouts, image infos, SPIR-V name, cull mode, push size. | Replaced: `VulkanPipelineContractTests` (layered local composition) and `ShaderAbiContractTests` (`CanonicalPushConstants` == `sizeof(CanonicalMeshPushConstants)`). |
| | Graph source: resource/pass names, write/read calls. | Replaced: `ProductionGraphContractTests` "Ordinary2 composition wiring". |
| | Resolve pass: blend factors, `depthWriteEnable`, SPIR-V. | Replaced in part: scene-resolve pipeline creation, and graph `compose-hook` (`DepthAttachmentRead`, `scene.color` loaded). Blend/cull state is not observable after creation and is deleted (F4 captures cover it). |
| | Resolve shader expressions and backend record-function text. | Deleted: expression and implementation text. |
| `testOrdinary2VulkanCaptureObjectContract` (2066–2301, 98) | Capture pass: formats, layouts, descriptor types, layout count, push `sizeof`, cull, tier descriptors. | Replaced: layered-capture pipeline creation and the `LayeredInterfaceCapturePushConstants` ABI. |
| | Frame targets: resource names, framebuffers, tier acquire/destroy, OIT targets. | Resource existence and formats are already in `VulkanRenderGraphExecutorTests` (conditional topology). Framebuffer and destroy helpers deleted (implementation). |
| | WeightedOIT pass: formats, blend, depth write/compare, cull, SPIR-V, views. | Replaced: WeightedOIT pipeline creation (instanced vertex input validated) and `ProductionGraphContractTests` "WeightedOIT accumulate/resolve wiring" (read-only depth, `Clear`/`Load`). Fixed-function state deleted. |
| | OIT shader text: `#if` guards, refraction define, resolve expressions, instanced inputs. | Replaced: permutation check (OIT and opaque forward do not link refraction transport). Expressions deleted. |
| | Backend prewarm/rebuild/clear ordering, graph config, readback hook, OIT batching, counters; `RenderInstanceBatchComponent.h` and `Application.cpp` text. | Deleted: implementation text. Covered by F4 captures and `--validate-weighted-oit-resize`. `DrawPacket` instance defaults stay in `testWeightedOitBoundedReferenceContract`. |

**Group 3 — layered shaders**

| Case (lines) | What it asserted | Disposition |
|---|---|---|
| `testOrdinary2MaterialAwareCaptureShaderContract` (1752–1794, 17) | `layered_interface_capture.frag` expressions: uint output, packed material, alpha mode, depth/orientation tests, viewport unpacking. | Replaced: push ABI; capture pipeline creation (uint output vs R32_UINT attachment validated); ownership (`packed_material.glsl` compiled, `packedMaterialHasTexture` linked). Expressions deleted (`--validate-ordinary2-capture` reads back the results). |
| `testOrdinary2MeasuredChordMaterialShaderContract` (1796–1840, 16) | Wrapper defines/include, set 4 bindings, pairing and chord expressions, IBL and transport calls. | Replaced: `ShaderOwnershipTests` "material body ownership and permutations" (Ordinary2 links its pairing function and the deep tier does not); local-composition pipeline creation validates set 4. Expressions deleted. |
| `testDeepLayeredLocalCompositionContract` (1940–2064, 48) | Deep/residual wrappers and body text. | Replaced: ownership permutations (deep pair search, residual entry). |
| | Pass text. | Replaced: deep and residual pipeline creation. |
| | Graph `tier.identity[0]` read. | Replaced: `ProductionGraphContractTests` "deep-tier capture and composition wiring". |
| | Backend recording/readback/counter text; scheduler occlusion-query text. | Deleted: implementation and telemetry text. `--validate-deep-layered-capture` covers it. |

**Group 4 — topology/lifecycle**

| Case (lines) | What it asserted | Disposition |
|---|---|---|
| `testContentDrivenFrameTopologyPrewarmContract` (2352–2445, 39) | `Application.cpp` order: `initRenderer` < `prepareFrameTopology` < scene start < `mainLoop`; requirement-field text; backend `prepareFrameTopology` internals and pass-name tables. | Deleted. No public API exposes the phase order yet; the R2.6 harness phase-order unit test replaces it. The behavioral model-requirement checks (2304–2350) are kept. |
| `testSwapchainRebuildRetiresClusterDescriptors` (531–559, 8) | `recreateSwapchain` call order: clear descriptors < rebuild graph < bind cluster buffers. | Deleted: backend call order. Covered by the resize validators (`--validate-ordinary2-resize`, `--validate-weighted-oit-resize`, `--validate-depth-pyramid-resize`) in the R2.0 sweep. Vulkan-label process tests are planned with R2.5. |

**Group 5 — debug view and editor text**

| Case (lines) | What it asserted | Disposition |
|---|---|---|
| `testArtistFacingTransparencyPolicyControls` (2588–2623, 18) | `AssetBrowserPanel.cpp` UI copy and erase/tooltip calls. | Deleted: UI copy. The default-policy checks are kept (renamed `testClassifiedTransparencyIsTheDefaultPolicy`). |
| `testTransparencyDeepDebugViewContract` (2642–2722, 25) | Shader debug expressions; backend debug packing, pass-name tables, scratch reuse; `TransformSystem`/`LocalShadow`/`ReflectionProbeCapture` include and allocation text; viewport UI labels; resolve debug colour. | Deleted: shader expressions, implementation and UI copy. Steady-frame allocation freedom is the R0 timing-route invariant. The `RenderDebugView` round trip is kept. |

**Group 6 — position-only shadow fetch**

| Case (lines) | What it asserted | Disposition |
|---|---|---|
| `testOpaqueShadowPositionOnlyFetchContract` (2726–2760, 6) | Six opaque shadow vertex shaders declare only `location 0 vec3`; the implementation selects the `_opaque_vert.spv` variant with one attribute. | Replaced: `ShaderAbiContractTests` "vertex input contracts" (exactly one float3 input at the position location; masked variants fetch more) and shadow pipeline creation for every variant (validation requires provided attributes to cover consumed inputs). The exact attribute count of a created pipeline is not observable and that assertion is deleted. |

### Other files

| File (lines) | What it asserted | Disposition |
|---|---|---|
| `SceneColorTests` `testShaderColorBoundary` (121–187, 37) | G-buffer/forward convert to AP1; lighting does not; no `ACESFilm`; `output.frag` leaves AP1; shared matrix constants. | Replaced: `ShaderOwnershipTests` "scene-colour boundary and single output transform" and `VulkanShaderParityTests` scene_color parity. |
| | Frame pass order: scene capture < output < final capture < UI. | Replaced: `ProductionGraphContractTests` "single output transform". |
| | `output.frag` selection/grid expressions; UI-shader decode text; `Application`/backend/settings-panel text. | Deleted: expressions, implementation and UI copy. The UI shader still must link `decodeSrgb`. |
| `LightingReferenceTests` `testSharedDirectLightingContract` (145–217, 49) | Deferred/forward include clustered access, shadows and IBL, and call the shared functions. | Replaced: ownership tests (one clustered-light representation; shared shadow and environment). |
| | Bindings 16–19 and 22–27, `samplerCubeArray`, no `sampler2D hdriMap`. | Replaced: ABI "lighting descriptor representation" (kinds and Cube/2D/array dimensions). |
| | The photometric constant and the no-`layout` library. | Replaced: direct-lighting parity (the kernel includes the file standalone). |
| | `min(shadowVisibility…)`, debug-view numbers, split-sum expression, absence of legacy hard-coded light vectors. | Deleted: expression text. CPU split-sum behavior stays in `testBsdfAndIblEdges`. |
| `StandardMaterialShadingTests` `testRasterShadersUseSharedConventions` (321–357, 28) | BSDF/normal/transport/complex includes; shared BRDF names; no private `DistributionGGX`/`FresnelSchlick`. | Replaced: ownership "shared BSDF ownership" (also rejects legacy private names in every module) and "material body ownership"; BSDF parity. |
| | Binding 21. | Replaced: ABI descriptor representation. |
| | Schema/lobe/alpha/transport expressions. | Deleted: expression text. The packed-material ABI covers the record. |
| | `sizeof`/`offsetof` checks. | Kept (renamed `testShaderSharedRecordSizes`). |
| `M4ProductionCutoverTests` `retiredProductionPathsStayRemoved` (138–151, 4) | Top-level `CMakeLists.txt` lacks `SceneSerializer`; `InspectorPanel`/`SceneHierarchyPanel` text. | Deleted: build-script and editor source text. The retired-file absence checks and the canonical-bytes checks are kept. |
| `ReflectionProbeTests` `gpuRecordAbiAndShaderMirrorAreFrozen` (101–118, 6) | GLSL member order; cluster `local_size_x = 64`; `probeClusterHeaders` present. | Replaced: ABI "reflection-probe record ABI" (offsets, stride, parameter block, LocalSize, set 0 binding 3 storage buffer). |
| | The `maximumPerCluster` identifier. | Deleted: local identifier. |
| `LightExtractorTests` `abiAndInitialUuidOrderingAreFrozen` (70–79, 2) | GLSL member order of `PackedGpuLight`. | Replaced: ABI "packed light record ABI" (cluster builder, deferred and forward consumers). |

## Remaining file reads in tests

After this change, no test opens engine source or GLSL for text:

- `tests/` reads compiled `.spv` files, glslc `.d` depfiles, and fixture or manifest JSON/glTF.
- `M5FixtureContractTests` and `M6FixtureContractTests` compare manifest strings that name shader paths; they do not open the shaders.
