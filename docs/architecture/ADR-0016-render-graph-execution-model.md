# ADR-0016: Render-Graph Execution Model

- Status: Accepted 2026-10-02 (M7R R3b). Every production call site addresses the
  graph by id, string APIs are removed, imported images and compaction/probe/history
  work are declared, and synchronization validation reports zero hazards on the
  frozen set. Callback registration by feature owners proceeds in R3c.
- Date: 2026-10-02
- Owners: Renderer, RHI, and Vulkan backend
- Refined by: ADR-0017 (history survives compatible rebuilds; accepted 2026-10-06).
- Refines: ADR-0002. Its scene-linear HDR and output-transform decisions are unchanged.

## Context

ADR-0002 requires a backend-neutral render graph whose Vulkan executor owns
barriers, layouts, queue synchronization, transient aliasing, and history/external
resource integration. The M7R takeover audit (2026-10-02) found that only part of
that existed.

| Area | State before R3 |
|---|---|
| Declaration | Passes were declared, compiled in declaration order, and reused physical slots for lifetime-disjoint transients with exactly equal descriptors. |
| Execution | About 35 imperative `beginPass("name")` calls were string-matched against a sequential cursor. |
| Barriers | One synchronization-1 `vkCmdPipelineBarrier` per resource transition; about 30 more barriers were hand-written in pass code. |
| Queues | `QueueClass` was declared but ignored. Everything recorded on one graphics command buffer. |
| History | The `History` lifetime existed but had no users. Real history (depth pyramid, LOD) was CPU-owned in the backend. |
| Imported images | The swapchain and shadow maps had no graph transitions; their render passes owned the layouts. |
| Undeclared GPU work | Shadow/GPU-scene compaction, probe clustering, probe capture, and depth-pyramid history. |
| Aliasing | Memory aliasing did not exist. Each physical slot was a separate allocation. |

M9 (TAA, motion vectors, bloom, exposure), M10 and M11 need history resources,
declared dependencies, and an executor that can grow to async queues. This record
describes the execution model that replaces the imperative path.

## Decision

1. **Declaration stays backend-neutral.** `RenderGraphBuilder` declares passes,
   resources, usages, load/store operations, and history pairs. Compilation
   produces index-addressed passes (`PassId`) and resources (`GraphResourceId`).
   Names are diagnostics, resolved once per rebuild through O(1) maps.

2. **Execution is a Vulkan-side callback table.** Feature owners register
   `VulkanPassCallbacks { owner, active, execute, gpuRange, placement }` per
   compiled pass. These are plain function pointers, so recording a frame
   allocates nothing.
   - The executor runs registered passes in compiled order. `active == false`
     means the pass is skipped, with no barriers and no GPU range.
   - During migration, imperative `beginPass(PassId)` first drains earlier
     registered passes, and throws if it would skip an unregistered pass.
     `finishFrameExecution` drains the rest.
   - The string forms remain only until every call site is converted.

3. **Synchronization uses synchronization2, one dependency per pass.** Each pass
   issues at most one `vkCmdPipelineBarrier2` with fixed-capacity arrays sized at
   rebuild.
   - The first mapping is *equivalence-first*: synchronization-1 stage and access
     bits are cast to their identical *2 values, a TOP_OF_PIPE source becomes
     `NONE`, and layouts are unchanged.
   - The same-access rules are kept: physical slots re-barrier only on storage
     writes; external buffers re-barrier after writes.
   - Two usages of one physical resource in one pass collapse to a single barrier.
   - A synchronization-1 fallback records the same dependencies on devices
     without the feature.
   - Narrowing stage and access masks is a separate, measured step, checked with
     synchronization validation (`--validation-sync`).

   *As implemented (R4a):* passes on dynamic rendering also re-barrier a physical
   slot that they write again as an attachment with the same access (load-op
   ordering), because no render-pass subpass dependency supplies that ordering any
   more.

4. **Imported resources are bound explicitly** with a synchronization policy:
   - `ExecutorOwned`: the executor emits barriers and tracks state.
   - `RenderPassManaged{initial, final}`: the swapchain and shadow maps, until
     dynamic rendering (R4a). The executor asserts the tracked layout and adopts
     `final`.
   - `OwnerManaged`: the pass owns internal barriers, for staging sets with
     variable membership.

   Bindings are per frame slot, or global when state must persist across slots.
   Imported buffers whose capacity grows are `variableSize`; their barriers use
   the bound size, so capacity growth never changes the topology hash.

   *As implemented (R4a):* `RenderPassManaged` and its `initial`/`final` fields
   are removed (R4a.9b), and no production pass uses a `VkRenderPass` or
   framebuffer (R4a.final). The swapchain is `ExecutorOwned` with
   `discardOnFirstUse()`, bound per frame after acquire with `current = Present`:
   its writer transitions it from `UNDEFINED`, and `finishFrameExecution` emits
   the frame-end Present export. The shadow maps are `ExecutorOwned` global
   imports bound `SampledRead`; before each drawing pass the executor moves the
   whole image to the depth-attachment layout with its contents kept, and the
   next reader moves it back. The policies are now `ExecutorOwned` (optionally discarding on first
   use) and `OwnerManaged`.

5. **History is a graph lifetime.**
   - `createHistory(name, desc)` yields `{previous, current}` sharing a pair of two
     non-reusable physical slots outside the per-frame pool.
   - Parity flips when the writer pass actually begins. `previous` is the slot the
     last writer wrote.
   - Validity is keyed by `{view identity, ViewHistoryContext.resetRevision,
     extent, format, topology hash}`. It is lost after a key change, rebuild,
     resize, or skipped writer.
   - An invalid `previous` transitions from `UNDEFINED`, and consumers query
     `historyValid`.
   - Production declares no history until M9.

   *As implemented (M7R, recorded at R6 preparation 2026-10-04):* the validity
   tracker keys pairs as described, but production does not yet supply a view.
   The backend calls the one-argument `beginFrameExecution(frame)`, so every frame
   uses the default `{identity 0, resetRevision 0}`, and `RenderFrame::history`
   reaches only the Hi-Z owner. The key is also one per graph, not per view: the
   editor's alternating scene and asset views, and any camera cut, would
   invalidate every pair. The Hi-Z depth pyramid is an imported per-view image,
   not graph History. No production pass declares history, so nothing is wrong
   today. M9 must pass the view into `beginFrameExecution`, give history a
   per-view key (or per-view pairs), and unify the two `ViewHistoryContext`
   types (`Mesh.h`, identity 1 by default; `RenderGraph.h`, identity 0) before
   TAA history lands. See `docs/milestones/M7R-to-M9-handoff.md`.

   *As implemented (M9 G1–G2, 2026-10-05):* this completes the item; it does not
   change the contract.
   - **One type.** `RenderGraph::ViewHistoryContext` (`renderer/graph/ViewHistory.h`)
     is the only definition; rhi re-exports it. Identity 0 means "no view", and
     nothing is valid under it. The extractor always sets an explicit identity:
     scene view 1, asset preview `sessionSerial + 2`.
   - **View supplied before the first pass.** `submitFrame` calls
     `beginViewExecution(frame.history)` before any pass, which re-keys validity
     for the frame's view.
   - **Per-view sets.** History keeps `HistoryViewSetCount = 2` physical sets per
     pair, selected by `ViewHistoryContext::historySet` (0 = scene view, 1 = asset
     preview). Set 0 is created with the plan. Later sets are created the first
     time a view selects them, and every set retires with the plan.
   - **Validity per view.** Each set has its own parity, tracked access and
     validity state. A pair is valid iff its writer ran on **that set's previous
     turn** under an identical key. That turn is the last frame that rendered the
     view, not necessarily the previous frame, because the editor renders one view
     per frame and never gives the background view two turns in a row.
   - **Reset policy.** `createHistory(name, desc, HistoryReset)`:
     - `OnCut` (the default) keys on `resetRevision`;
     - `SurviveCut` (adapted exposure) does not, but still follows identity,
       extent, format and topology.

     The policy is hashed into the topology only when it is not the default.
   - **Cuts.** `resetRevision` is owned per retained view by
     `ViewMotionTracker` (`renderer/rhi/ViewMotion.h`), fed by the extractor. It
     advances on:
     - a requested revision (benchmark cut, preview framing);
     - an explicit cut;
     - a projection-kind or extent change;
     - a translation or rotation discontinuity (10 m or 45° in one turn).

     A field-of-view change is reprojectable, not a cut.

6. **Barriers allowed inside passes:** mip chains, scan/compaction steps inside
   one logical pass, VSM-internal compute ordering, probe prefiltering, and
   qualification readbacks. Every other dependency is declared.

7. **Queues.** Graphics only until R4d adds a transfer queue with timeline
   semaphores. `QueueClass` and per-pass dependency info leave room for
   queue-family ownership transfers and later async compute, which is gated on
   timeline evidence.

   *As implemented (R4d):* graph passes still run on the graphics queue only;
   uploads moved off the frame's critical path. `VkContext` selects an upload
   family (a dedicated transfer family with (1,1,1) image granularity, else a
   graphics-free compute family, else graphics; family 1, dedicated transfer, on
   the reference RTX 4090) and enables timeline semaphores. Uploads stage through
   one persistently mapped 64 MiB ring (dedicated staging above a quarter of it, or
   when the open batch fills it) whose batches retire by upload-timeline value.
   Uploads into fresh resources record on the transfer queue as copy plus a release
   that carries the final layout; the matching acquires are recorded at the start
   of the next frame command buffer, before any pass. Other uploads and layout-only
   transitions stay on the graphics queue. `beginFrame` submits both upload lanes
   without a CPU wait. The frame submission (`vkQueueSubmit2`, with a
   synchronization1 fallback) waits on the acquired image at
   COLOR_ATTACHMENT_OUTPUT and on the upload timelines at ALL_COMMANDS. It signals
   the present semaphore and a graphics timeline equal to the frame serial, which
   replaces the frame fences and feeds the deletion queue. A retire floor keeps
   a resource an outstanding upload writes alive until the frame that waits on that
   upload. Blocking flushes (init, frame-target rebuilds, registry error paths,
   cleanup) stay bounded. `--upload-queue graphics` keeps the asynchronous path on
   the graphics queue; `--upload-queue legacy-blocking` is the pre-R4d path. Async
   compute remains a later candidate.

8. **GPU ranges.** Each pass places its range before or after its barriers,
   preserving the profile semantics. A `RangeGroup` spans several passes.

9. **Aliasing (R4b)** builds on this model. Owners re-query images after every
   rebuild, and transient first use is always a discard.

   *As implemented (R4b):* the backend-neutral planner (`planTransientAliasing`)
   places every alias-eligible transient image in an `AliasHeap` from compiled
   lifetimes and device memory requirements. History, imports and resources marked
   `excludeFromAliasing` are never aliased. A resource is eligible only when its first
   use writes it whole; storage first writes declare that with
   `declareWholeResourceWrite`. The Vulkan executor allocates one VMA block per heap
   per frame slot and binds images with `vmaCreateAliasingImage2`. At frame begin
   it resets aliased slots to `Undefined`. A first use transitions from `UNDEFINED`,
   with the source scope set to the union of the accesses its alias predecessors
   actually made this frame. Reading or loading an aliased image before its first
   writer ran that frame throws. Aliasing is on by default; `--render-graph-aliasing
   off` remains until M7R R6, and `--qualification-alias-poison` fills the heaps with
   NaN at frame start. At native 4K it saves 341.6 MB across the two frame slots.
   Synchronization validation does not detect a missing predecessor scope, so the
   executor's barrier-sink tests and the poison captures are the guard.

## Consequences

- New features declare passes and resources; they do not edit a central sequence
  of imperative calls.
- M9 history resources (TAA color, velocity, exposure luminance) have a defined
  lifetime and invalidation contract.
- Synchronization changes are testable: barrier sinks, equivalence tests against
  the legacy emission, device tests under synchronization validation, and frozen
  captures.
- The executor rejects an inconsistent graph state at runtime (skipping
  unregistered passes, binding imported images twice, mismatched layouts)
  instead of producing undefined GPU behavior.

## Rejected alternatives

- **`std::function` callbacks:** allocation and indirection on the frame path
  for no gain over function pointers with an owner pointer.
- **RHI-level callbacks:** would leak Vulkan command recording into the
  backend-neutral contract.
- **Full subresource-level tracking now:** no current pass needs it.
  Mip/layer-granular passes keep their internal barriers until a measured need
  appears.

## Verification

- An equivalence test of legacy synchronization-1 emission against batched
  synchronization2 across all production topologies.
- Device tests under synchronization validation.
- Contract tests: no history in production, no duplicate usage within a pass,
  declaration-order subsequence when passes are added.
- The frozen capture set, the qualification sweep, and the matched timing pair
  defined in `docs/milestones/M7R-architecture-consolidation.md`.
