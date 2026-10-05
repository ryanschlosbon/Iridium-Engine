# M7R R4 design: Vulkan modernization

This is the implementation design for slice R4 of
`docs/milestones/M7R-architecture-consolidation.md`. It was drafted by a read-only
design pass against `bd89494` and accepted by the lead on 2026-10-03. Line numbers
refer to that commit; re-verify before editing. Decisions and evidence go in the
plan's log.

## Findings that shape R4

1. **Same-access attachment writes are ordered only by render passes.** The
   executor skips same-access barriers (`VulkanRenderGraphExecutor.cpp:1005-1009`).
   `scene.color` is written as a colour attachment by lighting, forward-opaque,
   sorted, compatibility, the compose hooks and the OIT resolve, one after another.
   Today only each render pass's EXTERNAL→0 dependency orders those writes. Under
   dynamic rendering, a same-access `ColorAttachment` or `DepthAttachmentWrite`
   usage must emit a memory-only image barrier.
2. **Swapchain acquire chaining and the frame-end Present transition need new
   executor code.** `finishFrameExecution` (`:1263`) never emits export
   transitions. A swapchain bound with current=Undefined would give a NONE source
   scope, which does not chain with the acquire semaphore's COLOR_ATTACHMENT_OUTPUT
   wait.
3. **Latent use-after-free in `VulkanFrameScheduler::defer` (`.cpp:410-415`).** It
   queues on `frames_[currentFrame_]` after `endFrame` has already advanced
   `currentFrame_`. Items deferred between frames can therefore be freed after the
   older slot's fence while the just-submitted frame may still use them. Callers:
   `VulkanResourceRegistry.cpp:84, 217, 367`. R4c's deletion queue fixes it.
4. **The OIT accumulation pass stores to a read-only depth attachment.**
   `VulkanWeightedOitPass.cpp:~126-131` uses LOAD/STORE on read-only depth; under
   dynamic rendering it must be STORE_OP_NONE.
5. **Editor depth sampling is undeclared.** The editor samples `targets.depth` in
   the UI pass (`VulkanImGuiEditorBridge.cpp:170`), but the graph declares no read
   there. R4b must declare it before aliasing is enabled.

## 1. Inventory (bd89494)

### Render passes

There are 14 creation sites; each has one subpass and stencil DONT_CARE.

| Pass | Site | Attachments: load/store, initial→final | Dependencies | Clear values |
|---|---|---|---|---|
| gbuffer | `VkRenderPass.cpp:17-90` | 5× colour CLEAR/STORE COLOR→COLOR; D32 CLEAR/STORE DS_ATT→DS_ATT | E→0 TOP→COLOR_OUT\|EFT\|LFT; 0→E COLOR_OUT\|LFT→FRAG | (0,0,0,1)×2, (0,0,0,0) emissive, (0,0,0,1), uint 0, depth 1.0 |
| forward / transparent | `VkForwardRenderPass.cpp:16-95` | colour LOAD/STORE; depth LOAD, STORE (writable) or NONE (read-only), DS_ATT or DS_RO | E→0 COLOR_OUT\|TRANSFER\|FRAG → COLOR_OUT\|EFT\|LFT | none |
| ui | `VkUIRenderPass.cpp:5-48` | swapchain or RGBA16F CLEAR/STORE, UNDEFINED→PRESENT (or COLOR for HDR10 composition) | E→0 COLOR_OUT (src access 0)→COLOR_OUT | (0,0,0,1) |
| lighting | `VulkanDeferredLightingFeature.cpp:25-66` | RGBA16F CLEAR/STORE COLOR→COLOR | E→0 COLOR_OUT\|FRAG→COLOR_OUT | (0,0,0,1) |
| output | `VulkanOutputPass.cpp:58-93` | CLEAR/STORE COLOR→COLOR | E→0 FRAG\|COLOR_OUT→COLOR_OUT | (0,0,0,1) |
| hdr10 encode | `VulkanHdrEncodePass.cpp:30-56` | swapchain CLEAR/STORE UNDEFINED→PRESENT | E→0 FRAG→COLOR_OUT; the source does not cover the acquire wait | (0,0,0,1) |
| layered capture | `VulkanLayeredInterfaceCapturePass.cpp:37-91` | R32UI + D32 CLEAR/STORE UNDEFINED→COLOR/DS_ATT | E→0 TOP→writes | uint 0, depth 1.0 |
| layered local composition | `VulkanLayeredLocalCompositionPass.cpp:38-75` | RGBA16F CLEAR/STORE UNDEFINED→COLOR | FRAG\|COLOR_OUT↔ | (0,0,0,0) |
| OIT accumulation | `VulkanWeightedOitPass.cpp:~105-171` | RGBA16F + R16F CLEAR/STORE; D32 LOAD/**STORE** DS_RO | COLOR_OUT\|EFT↔ | (0,0,0,0), (1,0,0,0) |
| OIT resolve | `VulkanWeightedOitPass.cpp:174-206` | scene LOAD/STORE COLOR | FRAG\|COLOR_OUT↔ | none |
| directional shadow | `VulkanDirectionalShadowMap.cpp:72-110`; per-layer framebuffers `:112-123` | D32 CLEAR/STORE DS_RO→(DS_ATT)→DS_RO per layer | E→0 FRAG→EFT\|LFT BY_REGION; 0→E LFT→FRAG | depth 1.0 |
| spot atlas | `VulkanSpotShadowAtlas.cpp:56-102`; framebuffer `:104-113` | LOAD/STORE DS_RO↔; one instance per tile + `vkCmdClearAttachments` | E→0 FRAG→EFT\|LFT (DS_R\|DS_W, fixed in R3b.6); 0→E | `beginTile :250-272` |
| point pools | `VulkanPointShadowPools.cpp:56-101`; per-face framebuffers `:126-140` | LOAD/STORE DS_RO↔ per face + full-face clear | as spot | `beginFace :303-333` |
| probe capture | `VulkanReflectionProbeCapturePass.cpp:~45-105`; per-face framebuffers `ReflectionProbeCaptureTargets.cpp:158-167` | RGBA16F CLEAR/STORE UNDEFINED→SHADER_RO; D32 CLEAR/DONT_CARE UNDEFINED→DS_ATT | E→0 FRAG\|COMPUTE→COLOR_OUT\|EFT; 0→E COLOR_OUT→COMPUTE | (0,0,0,1), 1.0 |

Framebuffers:
- **Created in `VulkanFrameTargets.cpp`:**
  - `:281-302` — gbuffer, lighting, forward, transparent, OIT×2, output;
  - layered — `:340-361, 374, 403, 424`;
  - uiComposition — `:455`;
  - ui per swapchain image — `:477`.
- **Destroyed:** `:505-589`.
- **Render-pass handles:** passed through `VulkanTargetRenderPasses` (`VulkanFrameTargets.h:24-35`).

### Pipelines

Every pipeline passes cache = VK_NULL_HANDLE.

**Graphics:**
- `VkGraphicsPipeline.cpp:202-207`
- `VkLightingPipeline.cpp:165-169`
- `VulkanPipelineLibrary.cpp:319-323` (targets at `VulkanVertexBackend.cpp:358-363`)
- shadows: `Directional :424-428`, `Spot :398-402`, `Point :460-464`
- `ReflectionProbeCapturePass :322-325`
- `HdrEncodePass :130-131`
- `OutputPass :186-187`
- layered: `Capture :245-249`, `LocalComposition :227-231`, `SceneResolve :152-156`
- OIT: `WeightedOitPass :291-295, 363-367`
- ImGui: `VulkanImGuiEditorBridge.cpp:138, 189` (render pass at `VulkanVertexBackend.cpp:1422`)

**Compute:**
- `ClusteredLightingPipeline :105`
- `DepthPyramid :42, 70, 108`
- `IndirectCullerShared :381`
- `LayeredInterfaceCapturePass :163`
- `ReflectionProbeCapturePass :207`, `ReflectionProbePipeline :70`
- `TransparencyPyramid :94`
- VSM: `DepthReceiver :53`, `FullView :56`, `Marking :80`

**Tests:** `VulkanPipelineContractTests.cpp:131` builds render passes; `Stage3ArchitectureTests.cpp:62` checks `depthStoreOperation`.

### Memory

**Allocation and budget:**
- `vkAllocateMemory` is called only at `VulkanResourceAllocator.cpp:136` (buffers) and `:211` (images).
- Entry points: `createBuffer :108`, `createImage2D :165`, `destroy`, `write`, `reclassify`, and `memorySnapshot :300`. The snapshot already queries `VK_EXT_memory_budget` (`:310-325`).
- There are 56 call sites.
- `findMemoryType :95` takes the first matching type.
- Categories: `profiling/MemoryProfile.h:9-30`.

**Graph memory:**
- Allocated through `VulkanAllocatorGraphResourceFactory::create` (`Executor.cpp:294-329`) in category RenderGraphTransient.
- Pools are per frame slot (FramesInFlight = 2).
- A rebuild retires resources per slot (`:360-393`); they are destroyed at `onFrameFenceCompleted :395`.
- Slot reuse is exact-descriptor (`RenderGraph.cpp:572-626`; `compatible() :87`).

**ImGui:** its own allocations (`imgui_impl_vulkan.cpp:491, 762, 823`) are editor-only and out of scope.

### GPU drains

**Capacity growth** (replaced in R4c):
- `GpuSceneState.cpp:89`
- `OpaqueIndirectCuller.cpp:194`
- `IndirectViewCuller.cpp:346`
- `VulkanVertexBackend.cpp:710` (`bindGraphImportedBuffers`, from `:1806` and probe `:307`)
- `ClusterLightingFeature.cpp:245`
- `ResourceRegistry.cpp:520`
- `ReflectionProbeFeature.cpp:203`

**Probe changes** (replaced in R4c):
- `ReflectionProbeFeature.cpp:295` (owner removed)
- `:312` (`finalizeCaptures`)
- `:413` (environment table)

**Kept as bounded stalls:**
- `VulkanVertexBackend.cpp:495` (LUT)
- `:851` (transparency topology)
- `:944` resize `vkDeviceWaitIdle` (ADR-0013)
- `:1070` scene extent
- `:533-535` shutdown
- `VulkanImGuiEditorBridge.cpp:265` (retained views)
- qualification capture collection (`VulkanQualificationExtension.cpp:391, 501, 679, 789`)

**Upload flushes:**
- Each flush is a submit plus `vkWaitForFences(UINT64_MAX)` (`VulkanUploadContext.cpp:167-256`, wait `:228`).
- Call sites: `VulkanVertexBackend.cpp:383` (init), `:534` (cleanup), `:1450` (`createFrameTargets`), `:1488` (`beginFrame`), and the registry error paths at `:315, 326`.

### Queues

- **Families:** `VkContext::findQueueFamilies` (`VkContext.cpp:339-375`) finds graphics and present only. There is one queue per unique family (`:382-397`, retrieved at `:523-524`).
- **Features:** of the 1.2/1.3 features (`:420-468`), only `synchronization2` is enabled. `dynamicRendering` and `timelineSemaphore` are off.
- **Uploads:** each upload makes its own staging `vkAllocateMemory` (`UploadContext.cpp:280, 321`) and runs on the graphics queue.
- **Frame submit:** `vkQueueSubmit` (sync1) plus a fence (`VulkanFrameScheduler.cpp:347-367`).

## 2. R4a: dynamic rendering

### Graph API (`RenderGraph.h`)

- Add `StoreOp::None`.
- Add a bit-exact clear value:
  ```
  ClearValue { std::array<uint32_t,4> colorBits; float depth=1; uint32_t stencil=0;
               static color(f,f,f,f) /*bit_cast*/; colorUint(...); depthStencil(f,u) }
  ```
- Add `write(pass, res, Access, LoadOp, StoreOp, ClearValue)`. The clear value is stored in `UsageRecord` and `CompiledUsage`, and hashed only when `loadOp==Clear` (a one-time topology-hash change, which is logged).
- `DepthAttachmentRead` implies LOAD + NONE.
- The production graph supplies the exact values from the table above.

### Executor

**Rendering plan:** at rebuild, each pass gets a fixed-size `VulkanPassRenderingPlan`:
- colour attachments in usage-declaration order (this matches the existing framebuffer order);
- depth;
- load/store ops, clear values and layouts.

`VulkanPassContext::beginRendering(const VulkanRenderingOverrides&)` / `endRendering()` start and end the pass. Overrides cover:
- render area;
- per-layer views (shadow cascades, point faces);
- a LOAD + clear-attachments override.

Owner-managed probe staging calls `vkCmdBeginRendering` directly.

**Barrier changes:**
- **Same-access attachment re-barrier (finding 1):** for `ColorAttachment` and `DepthAttachmentWrite`, a same-access usage emits a memory-only image barrier with an unchanged layout (COLOR_OUT write → COLOR_OUT read/write, and the depth equivalent). It applies only to passes flagged as migrated, so each step's barrier diff stays local. The R3b equivalence-test expectations are updated per migrated pass.
- **`ExternalSyncPolicy::discardOnFirstUse`:** the old layout becomes UNDEFINED, while the source scope keeps the tracked access (the History pattern).
- **Frame-end exports:** `finishFrameExecution` emits them with `passOrderIndex == passCount` (swapchain → Present).
- **Retire `RenderPassManaged`** once the swapchain and shadow maps are ExecutorOwned.

### Per-pass mapping

Content-preserving layouts are unchanged, and so are clear values.

1. **gbuffer, lighting, output, layered capture/local composition, OIT accumulation, ui-compose.** The executor's pre-pass barrier performs the layout change, and declared transitions cover the old E→0 dependencies. Use the declared load and clear ops. For layered passes, CLEAR on a preserved layout equals the old UNDEFINED-initial behaviour.
2. **forward-opaque, sorted, compatibility, layered compose hooks and scene resolve, OIT resolve.** These need the attachment re-barrier for `scene.color`, and for depth between forward-opaque and compatibility.
   - Read-only depth uses `DEPTH_STENCIL_READ_ONLY_OPTIMAL` with store NONE.
   - OIT accumulation depth store becomes NONE (finding 4).
3. **shadow.directional.** ExecutorOwned, global, current=SampledRead.
   - The executor transitions the whole array DS_RO → DepthAttachmentWrite, preserving contents, since unrendered layers stay valid.
   - Rendering is begun per layer view with CLEAR 1.0.
   - The next reader transitions it back.
   - Never derive UNDEFINED from LoadOp::Clear on an import.
4. **shadow.spot.** Same whole-image transition with LOAD, but **one rendering instance for the whole pass**, with clear-attachments and draws per tile. Separate instances on one subresource would race through LOAD without a barrier.
5. **shadow.point.** Transition all three pools; begin rendering per face layer (distinct subresources); keep LOAD + clear-attachments.
6. **probe.capture** (owner-managed). Per face:
   - before: raw layer UNDEFINED→COLOR (src FRAG\|COMPUTE read) and depth UNDEFINED→DS_ATT;
   - after: COLOR→SHADER_RO (COLOR_OUT write → COMPUTE read).

   This is permitted by ADR-0016 rule 6.
7. **ui-present / hdr10-encode.**
   - Swapchain bound per frame (`VulkanVertexBackend.cpp:1507`), ExecutorOwned, current=Present, `discardOnFirstUse`.
   - In sync2, Present's BOTTOM_OF_PIPE first scope equals ALL_COMMANDS, which chains with the acquire wait.
   - Old layout UNDEFINED → COLOR, then the frame-end export → PRESENT_SRC.

### Pipelines

Each graphics site uses `renderPass = VK_NULL_HANDLE` and chains a `VkPipelineRenderingCreateInfo`:

| Pipelines | Colour formats | Depth |
|---|---|---|
| gbuffer | `vulkanGBufferFormats` (5) | D32 |
| forward, transparent, scene resolve | RGBA16F | D32 |
| lighting, local composition, OIT resolve | RGBA16F | none |
| OIT accumulation | RGBA16F + R16F | D32 |
| layered capture | R32_UINT | D32 |
| output | output format | none |
| ui | swapchain format, or RGBA16F for HDR10 composition | none |
| hdr10 encode | swapchain format | none |
| shadows | none | D32 |
| probe capture | RGBA16F | D32 |

Related changes:
- `VulkanPipelineTarget` becomes `{formats, layout}`.
- **ImGui:** `UseDynamicRendering=true`, `ApiVersion=1.3`, and `PipelineInfoMain.PipelineRenderingCreateInfo` with the UI format; the same in `onPresentationChanged`. `VulkanEditorUiPresentation.renderPass` becomes `colorFormat`.
- **Device:** enable `dynamicRendering` in `VkContext.cpp:463-467` and in `HeadlessVulkanDevice`.

### Order and tests

**Order:**
1. R4a.0: graph, executor, features.
2. Migrations, in order:
   1. output
   2. lighting
   3. layered capture/compose
   4. OIT
   5. gbuffer (with the library gbuffer target and `VkGraphicsPipeline`)
   6. forward/sorted/compatibility + scene resolve (library forward and transparent targets together)
   7. shadows (policy switch)
   8. probe capture
   9. UI/HDR10/swapchain/ImGui
3. R4a.final deletes `VkRenderPass*`, `VkForwardRenderPass*`, `VkUIRenderPass*`, all framebuffer fields and `VulkanTargetRenderPasses`.

**Tests:**
- graph clear/store-op compilation and hashing;
- executor fake-sink tests (rendering plan, attachment re-barrier, discard flag, frame-end exports);
- `VulkanPipelineContractTests` ported to rendering formats;
- `VulkanBackendFrameTests` under synchronization validation;
- per step: frozen set, digest and GPU ranges.

## 3. R4b: VMA and transient aliasing

### Library

- **Fetch:** `GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator`, pinned to the exact tag **v3.3.0** (confirm it is the latest release at adoption; fallback v3.2.1). Use `GIT_SHALLOW`, `SYSTEM`, and disable samples, docs and install.
- **Implementation TU:** `src/renderer/vulkan/VulkanMemoryAllocatorImpl.cpp` with `VMA_IMPLEMENTATION`, `VMA_STATIC_VULKAN_FUNCTIONS 1`, `VMA_DYNAMIC_VULKAN_FUNCTIONS 0`, `VMA_VULKAN_VERSION 1003000`, under warning push 0.
- The library was explained to and allowed by the owner (lead prompt). Record the pin in the plan's library table.

### Allocator

- `VulkanResourceAllocator` owns a `VmaAllocator` (API 1.3; `EXT_MEMORY_BUDGET` when available).
- The resource structs gain `VmaAllocation` and an offset. `memory` keeps `VmaAllocationInfo.deviceMemory`, so `isValid` and the fake cullers keep working.
- **Step 1 keeps memory types identical:** `requiredFlags` stay the same and `memoryTypeBits` is restricted to the type the old `findMemoryType` picks, so there is no silent ReBAR change. ReBAR for dynamic buffers is a separate, measured change.
- **Categories stay engine-side:** `recordAllocation(category, requested, info.size, type, heap)`, with names for VMA stats.
- **Dedicated allocations:** non-aliased images of 16 MiB or more (shadow maps and pools, history, probe cubes) and the alias heaps get `DEDICATED_MEMORY_BIT`. VMA also honours prefers/requires-dedicated.
- **Budget:** `memorySnapshot` uses `vmaGetHeapBudgets`, and `vmaSetCurrentFrameIndex` is called in `beginFrame`.

### Aliasing planner (`renderer/graph`, backend-neutral)

```
planTransientAliasing(const CompiledGraph&,
    span<const TransientMemoryRequirement{size, alignment, typeMask}>, AliasingOptions)
  -> AliasPlan{heaps{size, typeMask}, placements[slot]{heap, offset},
               aliasPredecessors (CSR), requestedBytes, committedBytes}
```

- **Slots:** with aliasing on, every eligible transient logical resource gets its own physical slot and image, bypassing exact-descriptor reuse. With aliasing off, slot reuse is unchanged.
- **Eligibility:** the resource must be all of the following:
  - transient, not exported, not History, not imported;
  - an image (buffers stay dedicated in R4b);
  - first used either by a Clear write, or by a whole-resource DontCare storage write (asserted).
- **Interference:** inclusive [firstUse, lastUse] intervals in compiled order.
- **Placement:** greedy, sorted by size descending, then firstUse, then logical index (deterministic). Each resource takes the lowest aligned offset among 0 and the ends of placed interfering resources, grouped by typeMask intersection.
- **Requirements:** `vkGetDeviceImageMemoryRequirements` (core 1.3), so planning happens before any image exists.

### Executor

- **Heaps:** one alias heap per frame slot (`vmaAllocateMemory` dedicated, plus `vmaCreateAliasingImage2(heap, offset, …)`).
- **Frame start:** `beginFrameExecution` resets `frameAccess_` of aliased slots to Undefined.
- **First use:** old layout UNDEFINED, with the source scope equal to the union of the final accesses of every overlapping resource (static, computed at rebuild).
- **Never aliased:** History, imports and ExecutorOwned bindings.
- **No owner changes:** owners already re-query images per rebuild (ADR-0016 rule 9).

### Correctness

- **Editor depth (finding 5):** declare the editor depth sample as a ui-pass read when a bridge is attached (`VulkanGraphHooks`).
- **`--qualification-alias-poison`** (qualification-only): fill each alias heap with a NaN pattern at frame start. Captures must stay byte-identical.
- **Fresh VMA suballocations:** also poisoned, to catch any reliance on driver-zeroed memory. WDDM zeroes fresh `vkAllocateMemory`; reused VMA blocks are not zeroed.

### Controls and reporting

- **Switch:** `--render-graph-aliasing on|off`, kept until R6.
- **Counters:** transient requested/committed bytes, persistent bytes, `alias_heap_count`, efficiency.

### Expected 4K savings

Default SDR graph, CanonicalReference, refraction pyramids on.

**Inputs:**
- Requested is about 564 MB per slot; peak live is about 398 MB at lighting.
- Per-slot resources:
  - normal / albedo / f0-roughness: 66.4 MB each
  - flags: 33.2 MB
  - emissive: 66.4 MB
  - depth: 33.2 MB
  - scene: 66.4 MB
  - refraction pyramids: 88.5 + 44.2 MB
  - output: 33.2 MB

**Savings:**
- Aliasing: about 166 MB per slot, roughly **332 MB total (29%)**.
- With WeightedOIT, the 83 MB per slot of OIT targets also fits into dead g-buffer space (about 498 MB total).
- Sharing one heap across both frame slots would save about 398 MB more but serializes frame overlap. It is a later candidate that needs timing evidence.

### Tests

- **Planner:** no overlap among interfering resources, determinism, alignment, typeMask grouping, ineligibility, peak bound, and aliasing-off reproducing the golden slots.
- **Executor fakes:** UNDEFINED transitions with the predecessor scope; zero steady allocations.
- **Device:** aliased images at offsets under synchronization validation, plus the poison mode.
- **Frozen set:** with aliasing on and off.

## 4. R4c: deferred deletion, pipeline cache, hitch harness

### Deletion queue

- **Structure:** `VulkanDeletionQueue {retireValue, kind, payload}`. Kinds: buffer, image, view, sampler, pipeline/layout, descriptor set, and a function-pointer callback (no `std::function`). Capacity is reserved.
- **Retire and collect:** `retireValue = frameOpen ? lastSubmittedSerial+1 : lastSubmittedSerial`. Items are collected in `beginFrame` once `completedSerial ≥ key` (the graphics timeline after R4d).
- **Use-after-free fix:** `scheduler.defer` and its three registry users move to the queue, which fixes finding 3.

### Capacity growth by slot-retirement swap

- **New buffers:** created immediately and parked as pending per slot.
- **Slot not in flight** (startup, or fence known signalled): swap now; startup is identical to today.
- **Slot in flight:** swap at that slot's retirement in `beginFrame`, after its collect and `onFrameSlotRetired`:
  - destroy the slot's old buffers;
  - rewrite the slot's descriptor sets;
  - rebind the slot's imports.
- **Sites:**
  - `GpuSceneState:89`
  - `OpaqueIndirectCuller:194`
  - `IndirectViewCuller:346`
  - `bindGraphImportedBuffers:710` (per slot; the wait and fence-completed loop are deleted)
  - `ClusterLighting:245`
  - `Registry:520` (`bindMaterialBuffer` per slot)
  - `ReflectionProbe:203`
- **Shared buffers** (the opaque culler's single LOD-history buffer) go to the deletion queue.

### Probe changes

- **`:295`:** removed capture targets go to the deletion queue; environment descriptors are rewritten per slot.
- **`:413`:** environment-table changes are rewritten per slot.
- **`:312` `finalizeCaptures`:** promotes only captures whose recording serial has completed, without blocking. F6-probecap promotion may shift by up to two frames; the 12 warm-up frames should absorb that. Otherwise keep a qualification-only drain.

### Pipeline cache

- **Service:** `VulkanPipelineCache` in `VulkanFeatureContext`, passed to every pipeline site and to ImGui (`initInfo.PipelineCache`).
- **Location:**
  - path: `%LOCALAPPDATA%/Iridium/PipelineCache/<vendor>-<device>.ircache`, via `platform/UserCacheDirectory`;
  - CLI: `--pipeline-cache <path|off>`;
  - harness runs pass a path under `out/m7r`.
- **File header:** magic, version, vendorID, deviceID, driverVersion, driverID, `pipelineCacheUUID`, payload size, FNV-64. `VkPipelineCacheHeaderVersionOne` is also validated.
- **Mismatch:** discard and recreate.
- **Limits and saving:** 512 MiB load cap. Saved atomically (temp file, then rename) at cleanup, before device destruction.
- **Measurement:** cold vs warm `backendNanoseconds`.

### Hitch scenario (R4c.0, harness tooling)

- **Flag:** `--qualification-scripted-changes <json>`, applied by `QualificationHarness` at `FrameBeginPhase::PreSceneUpdate`.
- **Events:**
  - add instances (256→512→1024): GPU-scene and culler growth, plus the import rebind;
  - add lights beyond `kInitialGpuLightCapacity=256`;
  - add materials;
  - remove, add or change probes and the environment on F6-probecap.
- **Schedule:** 500 warm-up frames, then an event every 500–1000 frames over 10,000 frames.
- **Tools:** `tools/m7r/Run-HitchScenario.ps1` and `Analyze-Hitches.py` (profile JSONL). They report:
  - hitches (> 2× median);
  - p99;
  - per-event maximum over 4 frames;
  - `cpu.renderer.drain_all_frames` / `upload_wait` counts.
- **Runs:** A,B,B,A, three per side.
- **Baseline (lead decision, 2026-10-03):** measured at the last pre-R4c commit with the harness. The R0 worktree predates `IFrameObserver`, and the drain sites are unchanged since R0, which the drain counters confirm.

## 5. R4d: transfer queue and timeline semaphores

### Queue selection (`VkContext`)

- **Selection order:**
  1. a family with TRANSFER but neither GRAPHICS nor COMPUTE, and `minImageTransferGranularity == (1,1,1)`;
  2. otherwise a compute family without graphics;
  3. otherwise graphics.
- **New API:** `getTransferQueue/Family()` and `hasTimelineSemaphore()`.
- **Features:** enable `Vulkan12Features.timelineSemaphore` (also in the headless device).
- **Switch:** `--upload-queue auto|graphics|legacy-blocking`.

### Upload path

**Staging ring:**
- a VMA persistent mapped ring, 64 MiB by default, category UploadStaging;
- regions are keyed by the upload timeline value;
- oversized uploads get dedicated staging retired the same way;
- when the ring is full, the CPU waits and the stall is counted;
- no per-upload allocations.

**Fresh destinations (state Undefined):** recorded on the transfer queue as copy then release:
- the release barrier is `{COPY/TRANSFER_WRITE → NONE, srcQF=T, dstQF=G}`;
- images go UNDEFINED→TRANSFER_DST, and the release carries the final layout.

**Graphics acquire:**
- recorded in the frame command buffer right after `scheduler.beginFrame`, before the first pass;
- identical queue families and layouts, src NONE, dst stage and access from the final state.

**Other cases:** non-fresh destinations stay on graphics. A same-family fallback needs no ownership transfer.

**Frame submission:**
- `beginFrame` (`:1488`) becomes `submitAsync`: signal uploadTimeline=V, with no CPU wait.
- `endFrame` uses `vkQueueSubmit2`:
  - waits: `imageAvailable`@COLOR_OUT and `uploadTimeline≥V`@ALL_COMMANDS (narrow later);
  - signals: `renderFinished` and `graphicsTimeline=serial`, which feeds the deletion queue.

**Unchanged:** blocking flushes at init, `createFrameTargets`, cleanup and error paths stay as graphics submit plus fence (bounded). Async compute remains a later candidate.

### Tests

- Headless round-trips on the transfer queue and the graphics fallback.
- Ring wrap and the oversized path.
- Synchronization validation, which checks the ownership transfers.
- The frozen set.
- Upload-wait telemetry at 0.
- A hitch run with mid-run asset loads.

## 6. Sequencing, verification and risks

### Serial order

R4a.0 → R4a migrations (integrated in the order above) → R4a.final → R4b.4–6 (executor aliasing, editor declaration, enable) → R4c.1–3 → R4d.1–4.

### Parallel lanes

| Lane | Scope | Constraint |
|---|---|---|
| R4a A | Shadows and probe | The integrator applies the `bindGraphImportedImages` policy hunk |
| R4a B | Layered and OIT | |
| R4a C | Gbuffer, lighting, forward, pipeline library | |
| R4a D | Output, HDR10, UI, ImGui, swapchain | |
| R4b.1–2 | VMA: cmake, allocator, budget | Alongside R4a |
| R4b.3 | Aliasing planner + tests (`renderer/graph`) | After R4a.0 |
| R4c.0 | Hitch harness + tools | Any time |
| R4c.4 | Pipeline cache | After R4a.final (same pipeline sites) |

The R4a lanes keep the framebuffer fields until R4a.final.

### Every step

- **Builds and tests:** Debug and Release builds plus ctest.
- **Images:** the frozen set is byte-identical, or within the F3/F7-lod/F4-woit envelopes.
- **Digest:** identical to `r3a0` and `-ext`.
- **Sync:** `-SyncValidation` reports 0 hazards.
- **Sweep:** 36/36, with expected deltas logged (topology hash after clear values, barrier counts, committed memory under VMA).
- **Timing:** a T-F1/T-F7 pair with 0 steady allocations.
- **Per slice:**
  - R4b: VRAM per category.
  - R4c/R4d: hitches and p99 against the baseline.
  - Also re-measure the R3 GPU watch item (+0.57% on F7) after R4b.

### Risks

- **Pipeline codegen:** dynamic rendering versus render-pass pipelines. Per-pass migration isolates any difference.
- **Hidden synchronization:** synchronization that came from global subpass dependencies would show up as sync hazards. The fix is to declare the resource.
- **Whole-array shadow transitions:** possible GPU cost. Watch `gpu.shadow.point`; the fallback is per-layer OwnerManaged.
- **Swapchain acquire chaining:** sync validation only partly covers presentation semaphores, so it needs careful review.
- **VMA type and zero-init drift:** guarded by the exact type mask and poison mode.
- **Aliasing uninitialized reads:** guarded by poison, the switch, and the editor depth declaration.
- **Probe finalize timing:** F6-probecap could shift, as noted above.
- **Pipeline-cache corruption:** guarded by the header and hash checks.
- **Transfer granularity and queue-family-ownership bugs:** guarded by the runtime switch and sync validation.
- **ADR-0016:** add "as implemented (R4)" notes for items 4, 7 and 9.
