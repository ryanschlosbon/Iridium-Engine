# ADR-0016: Render-Graph Execution Model

- Status: Accepted 2026-10-02 (M7R R3b). Every production call site addresses the
  graph by id, string APIs are removed, imported images and compaction/probe/history
  work are declared, and synchronization validation reports zero hazards on the
  frozen set. Callback registration by feature owners proceeds in R3c.
- Date: 2026-10-02
- Owners: Renderer, RHI, and Vulkan backend
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

6. **Barriers allowed inside passes:** mip chains, scan/compaction steps inside
   one logical pass, VSM-internal compute ordering, probe prefiltering, and
   qualification readbacks. Every other dependency is declared.

7. **Queues.** Graphics only until R4d adds a transfer queue with timeline
   semaphores. `QueueClass` and per-pass dependency info leave room for
   queue-family ownership transfers and later async compute, which is gated on
   timeline evidence.

8. **GPU ranges.** Each pass places its range before or after its barriers,
   preserving the profile semantics. A `RangeGroup` spans several passes.

9. **Aliasing (R4b)** builds on this model. Owners re-query images after every
   rebuild, and transient first use is always a discard.

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
