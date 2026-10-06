# ADR-0017: History Survives Compatible Graph Rebuilds

- Status: Proposed 2026-10-06 (M9.8d). Implemented and tested on `m9-temporal`;
  awaiting the owner's acceptance.
- Date: 2026-10-06
- Owners: Renderer (render graph), Vulkan backend
- Refines: ADR-0016 decision 5 ("History is a graph lifetime"). The rule that
  validity "is lost after a key change, rebuild, resize, or skipped writer" now
  excludes compatible rebuilds. Everything else in ADR-0016 is unchanged.

## Context

The production graph is rebuilt between frames whenever its topology changes:

- transparency residency (refraction pyramids, layered-glass atlases, WeightedOIT),
  enabled immediately when transparency needs it and released after 120 frames
  without it;
- turning bloom on or off, or changing its level count;
- the exposure mode and the anti-aliasing mode;
- an output-transport switch and a resize.

Under ADR-0016 every rebuild discarded all history. With M9's temporal features that
became visible. Auto-exposure adapts instantly when its history is invalid, so the
image jumped to the new exposure in one frame. TAA restarted from a single aliased
frame. A rebuild caused by transparency entering or leaving the view, or by an
unrelated settings change, has nothing to do with the content of a TAA or exposure
history.

## Decision

1. **Compatible pairs are kept.** A device-idle rebuild keeps a History pair when the
   new plan declares a pair with:
   - the same name;
   - the same reset policy;
   - slot descriptors that would create identical resources (type, image or buffer
     descriptor, usage mask).

   The pair keeps, for every view set:
   - its physical resources;
   - its parity;
   - its tracked access;
   - its validity state.

   Its key moves to the new plan's topology hash. A pair written on its view's
   previous turn therefore stays valid.
2. **Everything else is unchanged.** Pairs whose descriptor changed (a resize, a
   format change) start fresh, as do pairs the new plan does not declare (TAA or
   auto-exposure turned off and on again). Retained history that no plan adopts is
   destroyed. Key changes, cuts (for `OnCut` pairs) and skipped writers invalidate as
   before.
3. **Mechanism.**
   - Before a device-idle rebuild, the backend calls
     `VulkanRenderGraphExecutor::retainHistoryForRebuild()`. This happens in
     `releaseFrameTargets`, which every rebuild path uses.
   - Retention keeps:
     - the history resources;
     - the access and parity tables;
     - the `HistoryValidityTracker`;
     - the last view;
     - a factory that outlives `cleanupAfterDeviceIdle`.
   - The next compile adopts compatible pairs, using
     `HistoryValidityTracker::adoptPair` for their state, and destroys the rest.
   - Shutdown calls `discardRetainedHistory()`.
   - A rebuild that does not go through retention (the direct `rebuild` while a
     plan is bound) still retires all history, as before.
4. **Contract tests.** `VulkanRenderGraphExecutionTests`, "History survives a
   compatible rebuild (ADR-0017)", covers:
   - adoption across a topology change, in both view sets, with the production
     begin order;
   - a resize starting fresh;
   - repeated retention;
   - leak-free discard.

## Consequences

- When transparency enters or leaves the view, and when bloom, exposure-mode or
  output-transport settings change, auto-exposure and TAA continue as if nothing
  had happened.
- A retained pair may hold content from the previous plan's writer. That is
  correct only if the writer's meaning did not change between plans. The pair name
  identifies the writer's meaning. A writer that changes its output's meaning must
  rename its pair or change its descriptor.
- No extra memory: retained history replaces history that would otherwise be
  created again. During the rebuild the old and new pool resources do not coexist,
  because cleanup runs before the new plan is created.
