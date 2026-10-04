# M7R R3 design — shared culler, graph-driven execution, backend decomposition

This is the implementation design for slice R3 of
`docs/milestones/M7R-architecture-consolidation.md`. It was drafted by a read-only
design pass against `2aa6ca6`, before R2.9, and accepted by the lead on 2026-10-02.

Line numbers refer to that commit; re-verify before editing. R2.9 removes the 8
capture/oracle methods from `IRenderBackend`, `IVulkanLegacyQualificationRequests`,
and the backend forwarders (~70 lines). R2.9 also moves oracle configuration out of
`RenderBackendConfig`. Decisions and evidence go in the plan's log.

## 1. Current state at 2aa6ca6

### Culler duplication after lane B

| Piece | Directional | Spot | Point | Probe | Opaque |
|---|---|---|---|---|---|
| prepare | 3502-3812 | 4203-4497 | 4771-5077 | 5366-5591 + per-face 5593-5633 | 6021-6465 |
| create pipeline | 7076-7144 (owns the shared 3-binding layout) | 7175-7226 | 7228-7279 | 7281-7333 | 7954-8037 (6 bindings) |
| bind buffers | 7146-7173 | 7393-7420 | 7335-7362 | 7364-7391 | 8039-8082 |
| create buffers | 7627-7708 | 7710-7789 | 7791-7870 | 7872-7952 | 8382-8514 |
| collect | table-driven 7420-7625 | (same) | (same) | (same) | 8084-8380 |

What is still duplicated:

- **Bind and buffer creation:** `bind*` and `create*Buffers` are identical modulo names. The only difference is command capacity: directional uses `primitive × layerCount`, uncapped; the others use `min(primitive × MaxWork, LocalShadowIndirectMaximumCommandCount)`.
- **Pipeline creation:** `create*Pipeline` differs only in shader, set-0 layout, and push words (9/9/10/10).
- **Prepare:** spot and point differ only in work enumeration and push word 9. Directional differs in cascade-mask enumeration. Probe has no membership cache, forces direct mode from the G-buffer only, and dispatches per face interleaved with draws.
- **Repeated five times:** the `GpuSceneIndirectPolicy` reject ladder, the resident-LOD-prefix walk, the host→compute and compute→indirect barriers, and oracle expectation emission inside the draw loops.
- **Recording order:**
  - Shadow compaction is recorded after `beginPass("shadow.X")`.
  - Opaque compaction is recorded before `beginPass("gbuffer")`.
  - Probe compaction is recorded inside the undeclared probe capture.

### Executor

**API surface:**
- `beginPass` and `skipPass` match by string against a sequential cursor.
- `transitionImage`, `imageResource`, and `bufferResource` use linear `find_if`.
- `bindExternalBuffer` binds external buffers.

**Barrier behavior:**
- One sync1 barrier per resource.
- A same-access barrier on a physical slot is skipped, except for storage writes.
- External buffers re-barrier after writes.
- Imported images without a physical slot are silently skipped.

**Call sites:**

| Call | Sites |
|---|---|
| `beginPass` | 32: 27 in the backend, 5 in clustered lighting |
| `skipPass` | 17 |
| out-of-plan `transitionImage` | 2 (scene capture) |
| `imageResource` | 22 rebuild-time, in `VulkanFrameTargets` |

### Manual barriers

- **Shadow/probe/opaque compaction:** host→compute, then compute→indirect.
- **VSM readback:** transfer→host.
- **Retained views.**
- **Inside clustered lighting:** compute→compute.
- **Depth-pyramid history:** begin, per-mip, ready, readback.
- **Transparency pyramid:** per-mip.
- **Probe capture:** prefilter, readback, host.
- **Probe clustering:** compute→fragment.
- **VSM internal compute ordering.**
- **Upload context.**
- **Qualification-owned readbacks.**

### Undeclared GPU work

- Probe capture (5635-6021).
- All five compaction dispatches, including occlusion queries.
- Probe clustering (9411-9421).
- Depth-pyramid history images.
- The scene-capture bracket (11279/11288).

### Imported resources

| Resource | Current handling |
|---|---|
| swapchain | Its render passes handle UNDEFINED→PRESENT. |
| shadow maps | Render passes go READ_ONLY→READ_ONLY. |
| cluster readback | Never bound. |
| VSM working set | Bound. |
| Probe staging, history, retained views, indirect/count, cluster, GPU-scene tables | Not declared at all. |

### Backend clusters

Line ranges in the 11,635-line backend:

| Cluster | Lines |
|---|---|
| Helpers | 1-260 |
| init | 261-711 |
| Extension | 711-781 |
| Environment/LUT | 781-976 |
| cleanup | 976-1346 |
| Counters | 1346-1695 |
| Samplers/material descriptors | 1695-1818 |
| Graph rebuild/OIT/topology | 1818-2060 |
| Swapchain/transport/resize | 2060-2407 |
| Capabilities | 2407-2563 |
| Topology preparation | 2563-2672 |
| Resources | 2672-3115 |
| beginFrame | 3115-3253 |
| Caster resolve | 3253-3502 |
| Directional | 3502-4203 |
| Spot | 4203-4771 |
| Point | 4771-5366 |
| Probe capture | 5366-6021 |
| Opaque | 6021-6751 |
| Buffers | 6751-7076 |
| Culler infrastructure | 7076-7954 |
| Opaque culler | 7954-8516 |
| Probe resources | 8578-9020 |
| GPU-scene prepare/publish | 9020-9183 |
| Lights/cluster params | 9183-9367 |
| Lighting | 9394-9493 |
| Layered transparency | 9493-10497 |
| `submitForwardQueues` | 10497-11264 |
| Capture/forwarders | 11264-11374 |
| Output/UI/end | 11374-11537 |
| Editor/ImGui | 11537-11635 |

## 2. R3a — `VulkanIndirectViewCuller`

New files:
- `VulkanIndirectCullerShared.{h,cpp}`: the shared 3-binding layout, `createCompactPipeline`, `evaluateIndirectPolicy`, `residentLodPrefix`, and `VulkanIndirectBufferSet`.
- `VulkanIndirectViewCuller.{h,cpp}`.
- `VulkanOpaqueIndirectCuller.{h,cpp}`: a sibling of the view culler built on the shared core, not a subclass.

```cpp
enum class IndirectViewKind : uint8_t { DirectionalShadow, SpotShadow, PointShadow, ReflectionProbe };
enum class CompactionPlacement : uint8_t { BatchedAtPrepare, PerWorkItem };
struct IndirectWorkItem { uint32_t slotWord; uint32_t extraWord; };   // push[5], push[9]
struct IndirectViewKindConfig {
  IndirectViewKind kind; VulkanIndirectOracleView oracleView;
  const char* shader; VkDescriptorSetLayout set0Layout; uint32_t pushWords;
  uint32_t consumerMask; bool membershipCache; bool forceDirectIncludesShadowFlag;
  uint32_t workPerPrimitive; bool capCommandsAtLocalMaximum; uint32_t maximumWorkCount;
  CompactionPlacement placement; const char* gpuRangeName;
  void (*enumerateWork)(const void* packets, std::span<uint32_t> workIndices, IndirectWorkSink&);
};
class VulkanIndirectViewCuller {
public:
  void init(const VulkanCullerServices&, const IndirectViewKindConfig&);
  void resize(uint32_t primitiveCapacity);          // same drain/collect order as today (R4c target)
  bool plan(const IndirectViewInputs&, const void* packets, uint32_t frame);
  void recordCompaction(VkCommandBuffer, uint32_t frame, VkDescriptorSet set0);
  void recordWorkItem(VkCommandBuffer, uint32_t frame, VkDescriptorSet set0, uint32_t work, IndirectWorkItem);
  void recordDraws(VkCommandBuffer, uint32_t frame, uint32_t workIndex, const IndirectDrawBinder&);
  void emitExpectations(IVulkanIndirectOracle&, uint32_t frame, uint32_t countRegion, CasterMaskView, LodMetricFn);
  void collect(uint32_t frame, CpuProfiler*, IVulkanIndirectOracle*);
};
```

**Opaque extension:**
- `GpuSceneIndirectPlan` and DrawPacket-run binning.
- The 6-binding set, including the history sampler.
- Cull and fallback pipelines.
- 11-word push constants.
- Occlusion query and results.
- `mainOpaqueLodHistory_`.
- Pending validation.

**Ownership:** each culler owns:
- its pipeline, layout and per-frame sets;
- its buffers;
- scratch storage reserved in `resize`, so steady frames make no allocations;
- its validation slots.

**Identity proof, two layers:**

1. **Device-free unit test.** The culler records through a `VulkanCullerCommands` function-pointer table (bind pipeline/sets, push, dispatch, barrier) that defaults to `vkCmd*`. This is a Vulkan-side seam, not an RHI hook. The test asserts the exact ordered dispatch log and the host-written candidate/count bytes per kind, on a fake scene with two vertex buffers, alpha/double-sided mixes and LOD chains.
2. **Frozen-set digest.** A qualification-only flag, `--qualification-indirect-stream-digest`, hashes per view, slot and frame after the fence:
   - the dispatch log;
   - the candidate and count bytes;
   - the command regions, sorted by `firstInstance` within each `(work, bin)`, because GPU append order is nondeterministic.

   The baseline is recorded at R3a.0 on F1, F3, F5-hetero, F5-point, F6-probecap, F7-hiz and F7-lod. Every R3a step must match it exactly.

## 3. R3b — callback executor and synchronization2

### Index addressing

- `PassId{order}` and `GraphResourceId{logical}` are resolved per rebuild through a transparent-hash `unordered_map<string_view, uint32_t>` over strings the compiled graph owns.
- `beginPass(cmd, PassId)`, `image(frame, id)` and `buffer(frame, id)` are primary. The string overloads forward to them and keep the cursor check.
- Features cache a `ProductionPassIds` struct in `onGraphRebuilt`.

### Callbacks

- The setup side stays in the backend-neutral builder. In R3c it is split per owner as `declare(RenderGraphBuilder&, Inputs, Handles&)`.
- The execute side is a Vulkan table indexed by compiled order. It uses function pointers, which avoids `std::function` and steady-frame allocation:

```cpp
struct VulkanPassCallbacks { void* owner;
  bool (*active)(void*, const VulkanFrameRecordContext&);   // false => skipPass
  void (*execute)(void*, VulkanPassContext&);
  const char* gpuRange; GpuRangePlacement placement; };      // BeforeBarriers | AfterBarriers
```

- `GpuRangePlacement` reproduces today's mixed placement: some ranges wrap the barriers, others start after them. `RangeGroup{name, firstPass, lastPass}` covers ranges that span several passes, such as `gpu.lighting.cluster`.
- **Coexistence:** `beginPass(cmd, id)` first drains every earlier registered callback in compiled order. It throws if that would skip an unregistered pass. `finishFrameExecution` drains the rest.
- **Rollback:** unregister the pass and restore its imperative call.

### One `VkDependencyInfo` per pass

- **Batching:** fixed arrays of `VkImageMemoryBarrier2` and `VkBufferMemoryBarrier2`, sized at rebuild. Each pass issues one `vkCmdPipelineBarrier2` when it has at least one barrier.
- **Equivalence-first mapping:**
  - Set `stage2 = (VkPipelineStageFlags2)sync1Stages` and `access2 = (VkAccessFlags2)sync1Access`; the legacy bit values are identical. Add a `static_assert` per bit used.
  - Layouts are unchanged; keep `accessInfoForAspect`.
  - A TOP_OF_PIPE source is equivalent to NONE.
  - Each sync1 call carried exactly one barrier, so batching barriers on different resources changes no dependency.
- **Collapse rule:** two usages of one physical resource in a pass become one barrier, from the first "before" state to the last "after" state. A contract test asserts this never happens in current topologies.
- **Same-access rules stay verbatim.**
- **Narrowing** stage masks is a later, separate step, checked with sync validation.
- **Equivalence test:**
  - Inject a `VulkanBarrierSink` and use `FakeResourceFactory`.
  - Script four frames with realistic skips for every production topology.
  - Compare the legacy sync1 emission, kept for exactly one commit, against the batched sync2 emission, as ordered per-pass sets.

### Imported images

`bindExternalImage(frameOrGlobal, id, VulkanImageResource&, Access current, policy)` with three policies:

| Policy | Behavior | Used for |
|---|---|---|
| `ExecutorOwned` | The executor emits barriers and writes the state back. | Default |
| `RenderPassManaged{initial, final}` | No barrier; asserts the tracked layout equals `initial`, then sets `final`. | Swapchain (UNDEFINED→PRESENT) and shadow maps (READ_ONLY→READ_ONLY), until R4a |
| `OwnerManaged` | The pass is declared; its barriers stay inside it. | Probe staging, probe indirect buffers |

- The swapchain is bound per frame after acquire.
- Global bindings persist state across frame slots; depth-pyramid history and History need this.
- Variable-capacity imported buffers are declared with a minimum size plus a `variableSize` flag. Barriers then use the bound size, so capacity growth does not change the topology hash.

### History lifetime

- **Graph:** `createHistory(name, desc)` returns `{previous, current}`. The two linked logical resources get two non-reusable physical slots outside the per-frame pool.
- **Executor:**
  - Keeps a per-pair parity that flips when the writer pass actually begins.
  - `previous` maps to `pair[parity^1]`, with global access state.
  - Validity is tracked in the extended `HistoryValidityTracker`, keyed by `{identity, resetRevision, extent, format, topologyHash}`.
  - `beginFrameExecution(frame, const ViewHistoryContext&)` invalidates on key change, and `historyValid(id)` is queryable.
  - An invalid `previous` transitions from UNDEFINED.
- **Tests:**
  - the pair is distinct from transients;
  - `previous(N+1) == current(N)`;
  - validity across the first frame, a written frame, a reset, a rebuild, a resize, and a skipped writer;
  - state carries across slots;
  - zero allocations.
- **Production:** declares no History until M9; a contract test asserts this.

### Declaring undeclared work (output-neutral)

- **Ordering:** the Kahn sort picks the lowest declaration index first. Inserting passes at matching declaration positions therefore keeps the existing order as a subsequence. Physical slots are unchanged because the new resources are imported. Both properties are tested.
- **New passes:**

| New pass | Declaration |
|---|---|
| `shadow.{directional,spot,point}.compact` (Compute) | Imported per-slot command and count buffers: `StorageReadWrite`, then `IndirectRead`. Compute→indirect becomes an executor barrier; host barriers stay in the pass. |
| `gpu-scene.opaque.compact` | Declared before `gbuffer`. |
| `lighting.probe-cluster` | Imported buffers; readers declared through `readProbeClusterProduct`. |
| `probe.capture` | Declared after `shadow.point`; reads the shadow maps; staging is OwnerManaged. |
| Depth-pyramid history | ExecutorOwned global import. Begin and ready barriers move to the executor; mip barriers stay in the pass. Done last, because the ready-barrier position moves. |
| `scene-color-capture-hook` (Transfer) | Declared through a `VulkanGraphHooks` flag. The return to SampledRead happens at the output-transform begin. The usage mask already includes `TransferSource`, so allocations are unchanged. `transitionImage` is then deleted. |

## 4. R3c — feature owners

**Shared context, `VulkanFeatureContext`:**
- Vulkan objects and services: `VkContext`, device, allocator, `DescriptorAllocator`, scheduler, executor, frame targets, mesh layouts, pipeline library.
- `VulkanResourceRegistry`: vaults and samplers.
- `VulkanGpuSceneState`: buffers, sets, mirrors, published counts.
- `CpuProfiler*`, a `VulkanFrameCounters&` sink, and `VulkanExtensionHooks&`.

**Per-frame context, `VulkanFrameRecordContext`:** `{cmd, frame, imageIndex, collectCounters, sceneExtent}`.

**Owner interface, `IVulkanFeature`:** `create`, `onGraphRebuilt(ProductionPassIds)` (re-queries images, which keeps R4b aliasing safe), `registerPasses`, `onFrameSlotRetired`, `destroy`.

**Migration order:**

| # | Owner(s) | Passes |
|---|---|---|
| 0 | `VulkanFrameTelemetry`, `VulkanResourceRegistry`, `VulkanGpuSceneState` | none |
| 1 | `VulkanClusterLightingFeature` | `lighting.probe-cluster`, `lighting.cluster.*` |
| 2 | `VulkanOutputFeature` | bloom-hook, output-transform, hdr10-encode-present |
| 3 | `VulkanWeightedOitFeature` | `transparent.oit.*` |
| 4 | `VulkanHookPasses` | Validation, final-capture and scene-capture hooks |
| 5 | `VulkanShadowFeature` (directional + VSM), `VulkanLocalShadowFeature` | `shadow.*` |
| 6 | `VulkanReflectionProbeFeature` | `probe.capture` |
| 7 | `VulkanOpaqueFeature` (opaque culler, depth pyramid/history) | compact, gbuffer, pyramid |
| 8 | `VulkanDeferredLightingFeature` | lighting |
| 9 | `VulkanForwardFeature`, `VulkanLayeredTransparencyFeature` | The remaining passes |

Each step moves code, registers its callbacks, and deletes the imperative call. It is then verified with the frozen set, the indirect digest, and sync validation.

**Final `IRenderBackend` (~30 methods):**
- `init(RenderBackendInitInfo)`, `cleanup`.
- Capabilities, runtime info, extent, resize, swapchain, transport.
- Resources: geometry, arena, texture, material, environment, LUT.
- `prepareFrameTopology`.
- `prepareCapacities(RenderCapacityRequest)`, which merges the GPU-scene, lighting and probe prepare calls.
- Probe owners, probe settings, `finalizeReflectionProbeCaptures`.
- `beginFrame`, `submitFrame(const RenderFrame&)`, `endFrame`.
- Queries: shadow caster revisions and `frameTelemetry()`.

**`RenderFrame`** holds non-owning spans that are valid until `submitFrame` returns:
- view transport and history, camera, matrices, near/far planes;
- output settings, debug view, grid overlay;
- GPU-scene tables;
- the three shadow `{submission, packets}` pairs;
- probe casters, schedule and lights;
- opaque, forward, sorted, compatibility and selection queues, the wireframe flag, and instance transforms;
- lighting and probe frame packets.

**`IEditorRenderBridge`** lives in a new `iridium_vulkan_imgui` library (`src/renderer/vulkan_imgui/`, linking iridium_vulkan, imgui and glfw); `iridium_vulkan` drops imgui.
- **Interface:** `beginUI`, scene/glass-depth/editor texture IDs, `prepareRetainedViews`, retained-view texture IDs.
- **As a backend extension:**
  - UI recording becomes a contributor inside the backend-owned UI pass, which still clears and presents without an editor.
  - Retained views become a `FinalCaptureHook` consumer.
  - It handles the target-rebuild and texture-retire events that drive today's ImGui re-registration.
- The grid overlay stays an RHI POD in `RenderFrame`.

## 5. Byte-identity risks and verification

| Step | Risk | Verification |
|---|---|---|
| R3a | Dispatch order, push words, cache-reset order, drain/collect order | Digest, unit log, frozen set |
| R3b.1-2 | Barrier set or masks, Undefined/Present, in-pass collapse | Equivalence test, frozen set, zero sync-validation delta |
| Pass declaration | Order, slots, timestamp placement, topology-dependent counters | Subsequence and slot-golden tests, sweep diff with logged expected changes |
| Imported images | Layout mismatch against render-pass `initialLayout` | Policy assertions, validation |
| Owners | Clear values, `firstSet`/binding order, GPU range names | Frozen set, timing pair, range-name test |
| Depth history move | Ready-barrier position | F7-hiz identity, oracle sweep |

**Enabling synchronization validation:**
- **In code:** in `VkContext::createInstance`, when validation and `config.synchronizationValidation` are both on, chain `VkValidationFeaturesEXT{SYNCHRONIZATION_VALIDATION}`, the same as `HeadlessVulkanDevice`. `VkLayerSettingsCreateInfoEXT{"validate_sync", true}` is the non-deprecated alternative.
- **Tooling:**
  - Add a renderer CLI flag `--validation-sync`.
  - Count `SYNC-HAZARD-*` separately.
  - Add `Run-FrozenCaptures.ps1 -SyncValidation`.
- **Without code changes:** set `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` with `--validation`.
- **Baseline:** record existing hazards at the R2 head first.

## 6. Ordered steps

| Track | Steps |
|---|---|
| **R3.0** (serial) | Enable `synchronization2` (`VkPhysicalDeviceVulkan13Features`, also in `HeadlessVulkanDevice`); sync-validation plumbing and baseline; indirect-digest baseline. |
| **Lane A** (worktree: backend culler regions plus new files) | R3a.1 shared helpers → R3a.2 directional → R3a.3 spot/point → R3a.4 probe → R3a.5 single expectation emission and generic collect → R3a.6 opaque extension → R3a.7 tests. |
| **Lane B** (parallel: executor, graph, tests) | R3b.1 IDs, name map, barrier sink (still sync1) → R3b.2 sync2 batching and equivalence test → R3b.3 callback registry, drain, range placement → R3b.10 History, with tests. |
| **Serial after both lanes** | R3b.4 call sites to IDs → R3b.5 scene-capture hook pass, delete `transitionImage` → R3b.6 `bindExternalImage` and policies → R3b.7 compaction and probe-cluster passes → R3b.8 `probe.capture` → R3b.9 depth history → R3b.11 ADR-0016. |
| **R3c** (serial integration; owner files may be drafted in worktrees) | R3c.0 context, telemetry, registry, GPU-scene state → R3c.1-4 leaf owners → R3c.5-6 shadows and probes → R3c.7-9 opaque, lighting, forward/transparency → R3c.10 editor bridge → R3c.11 `RenderFrame`/`submitFrame` → R3c.12 completion: backend under 2,500 lines, sync validation clean, frozen set and timing pair. |

**R4 compatibility:**
- Usages already carry load/store ops, for dynamic rendering.
- Owners re-query images on rebuild, for VMA aliasing.
- Culler `resize` is the single deferred-deletion site.
- One pipeline-creation helper, for a pipeline cache.
- `QueueClass` and the per-pass dependency info leave room for queue-family transfers.

## 7. ADR-0016 outline (refines ADR-0002)

1. **Context:** ADR-0002's claims compared with the pre-R3 reality. Queue sync, history, imported-image transitions and aliasing were not implemented.
2. **Model:** neutral declaration, a Vulkan execute table, and transitional imperative coexistence.
3. **Synchronization:** sync2 with one dependency per pass, the equivalence mapping, re-barrier rules, and the narrowing policy.
4. **Imported resources:** the binding API, the three policies, and state persistence.
5. **History:** pairs, parity, validity keys, and resets.
6. **Barriers allowed inside passes:** mips, scans, VSM, prefilter.
7. **Queues:** graphics only until R4d.
8. **GPU-range placement rules.**
9. **Zero-allocation rule.**
10. **Rejected alternatives:** `std::function` callbacks, RHI-level callbacks, full subresource tracking now.
11. **Verification:** the equivalence, contract, sync-validation and frozen-set tests.
