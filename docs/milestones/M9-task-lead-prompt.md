# Prompt for the M9 Milestone Lead — Temporal Rendering and Core Post

Written by the director session on 2026-10-04. Paste everything below the rule into
a fresh Claude Code session opened on the Iridium Engine repository (Opus, high
reasoning effort recommended).

---

You are the milestone lead for **Iridium Engine M9 — Temporal Rendering and Core
Post-Processing**. Own it from audit through implementation, verification, the
completion report, and the hand-off to the next lead (M7.9). Continue slice to
slice without waiting for prompts. Stop and ask the owner only for a genuine blocker,
a product-intent decision, or a new third-party library.

## Product context

Iridium is a C++20/Vulkan engine whose primary goal is visual fidelity at the level
of UE5, Frostbite, Anvil, and Northlight. The reference machine is an RTX 4090,
i9-14900K, 64 GB DDR5-6000, and a 4K HDR display.

The raster target is **144 FPS at native 3840x2160 without ray tracing (6.94 ms)**.
Native temporal AA is the raster answer; sub-native reconstruction belongs to the
RT tier.

Your budgets (`docs/performance/FRAME_BUDGET.md`):
- **0.40 ms** for native TAA.
- **0.50 ms** for post, bloom, exposure, and output, of which the current output
  transform already uses about 0.04–0.27 ms depending on transport.

Current native-4K GPU medians after M7R: T-F1 1.129 ms, T-F7 1.942 ms, T-F5 2.997 ms,
T-F6 3.669 ms.

The engine has **no anti-aliasing, motion vectors, bloom, or auto-exposure today**.
This milestone is the first large visible fidelity step since M6. Aim for the image
quality of a modern AAA TAA (stable, sharp, ghost-free in motion, HDR-correct), not a
minimal TAA.

## Read before changing code

1. `AGENTS.md`: rules, git and content policy, library policy, evidence tiers.
2. `docs/milestones/M7R-to-M9-handoff.md`. This is your primary technical brief:
   exact attach points, gaps, frame model, tooling, and invariants. Line numbers are
   for `d2ffdb1`; some have shifted.
3. `docs/milestones/M7R-architecture-consolidation.md`: the completion report and
   decision log only.
4. The `ROADMAP.md` M9 section, including the **director decisions of
   2026-10-04**.
5. `docs/performance/FRAME_BUDGET.md`, including the evidence tiers.
6. ADR-0002 (scene-linear HDR, single output transform, auto-exposure deferred to
   M9), ADR-0006, ADR-0012, ADR-0013, ADR-0014, ADR-0015 (task system), and ADR-0016
   (graph execution, History, as-implemented notes).
7. `PLANS.md` for the plan structure.

Then reinspect the source and `git status`. The source is authoritative over this
prompt and the hand-off; record any corrections.

## Director decisions already made (do not reopen without evidence)

1. **Branching.** PR #7 (M7R) merges into `Render-Refactor-for-Modularity`. Branch
   `m9-temporal` from the updated `Render-Refactor-for-Modularity`. If PR #7 is
   somehow not yet merged when you start, branch from `m7r-consolidation` and
   rebase after the merge. Keep one PR open for M9, based on
   `Render-Refactor-for-Modularity`.
2. **F6 +1.8% GPU watch item** (VMA placement: refraction pyramids +0.038 ms,
   clustered lighting +0.022 ms, bisected to R4b.2). Carry it as a watch item, not a
   pre-slice.
   - Run every timing pair on a verified-quiet machine and record the machine state.
   - Bisect any delta by pass GPU ranges before attributing it to TAA or bloom.
   - Re-measure T-F6 at acceptance. Apply a placement policy (dedicated or aligned
     allocations for the refraction pyramids and cluster buffers, behind
     `VulkanResourceAllocator`) only if a placement-attributed regression above 1%
     remains, measured with the feature-tier protocol.
3. **Capture point.**
   - The existing scene-linear `scene` capture stays **before** TAA: the
     single-frame radiance domain.
   - A **TAA-off and jitter-off route is mandatory** and must reproduce the M7R
     frozen set (`tools/m7r`, `captures/r0` envelopes) byte-for-byte. Use it for
     every refactor-tier slice.
   - TAA-on adds a **new post-TAA scene-linear capture domain**, taken before bloom
     and exposure, plus the final output. These fixtures use deterministic per-view
     jitter sequences and measured envelopes (feature tier).
   - Record the jitter index and offset in capture metadata. Never edit the frozen R0
     entries.
4. **Scope split.**
   - M9 delivers: native-resolution motion vectors, jitter, TAA, reactive handling,
     bloom, auto-exposure, generic reprojection/history utilities, and a
     vendor-neutral super-resolution **input contract**.
   - **M9b** gets DLSS/FSR/XeSS integration and dynamic resolution; it is scheduled
     before M11.
   - M10 gets the stochastic-shadow and GTAO denoiser consumers.
   - Design the inputs (jitter, motion, depth, exposure, reactive) so M9b plugs in
     without reworking M9.

## First work: close the hand-off gaps (before any image-changing work)

These are prerequisites. Each is refactor-tier unless noted: the TAA-off route stays
byte-identical, validation stays clean, and steady allocation stays at zero.

- **G1 — View-keyed history.** Supply the view's `ViewHistoryContext` to the
  executor before the first graph pass (gap 1). Unify or explicitly convert the rhi
  and graph `ViewHistoryContext` types, which have different identity defaults.
- **G2 — Per-view history and reset policy.**
  - Give graph History a per-view dimension (gap 4). Follow the Hi-Z per-retained-
    view precedent, so the editor's dual views and asset previews stop invalidating
    each other.
  - Add a **per-pair reset policy**: TAA resets on cuts, while adapted exposure
    normally survives cuts.
  - Own cut and teleport detection. Today only the benchmark camera bumps
    `resetRevision`, and the editor sends 0.
  - Amend ADR-0016's item 5 note, or write a superseding ADR if the contract changes.
- **G3 — Settle publication** (gap 2). The frame after a transform stops, publish
  previous = current, using a bounded "moved last frame" list. Change-driven
  extraction does not re-observe stopped entities. Keep
  `--qualification-extraction-verifier` passing, and use `GpuSceneInstanceHistoryReset`
  for no-history motion. Report any upload-byte increase.
- **G4 — Previous matrices for direct-packet draws** (gap 3): forward-opaque,
  selection, direct fallback, and transparent draws. Velocity must be derivable for
  every opaque writer of depth.
- **G5 — Jitter and view ABI.**
  - Add jitter plus the previous unjittered view-projection to
    `ViewTransportRecord` and the view UBO. Move the ABI `static_assert`s and the
    `ShaderAbiContract` tests together.
  - Apply jitter **only in raster vertex stages**. Never apply it to culling,
    clustering, Hi-Z (`projectionRevision_`), shadow, probe, or the R5c.4e CPU frustum
    classification.
  - Derive the sequence index from a per-view frame counter, never from wall time.
    `finalizeView` does not receive `applicationFrameIndex` today.
  - With zero jitter, output must be byte-identical.
- **G6 — Evidence tooling.**
  - A five-fresh-process feature-admission runner. `Run-TimingPair.ps1` runs only
    four processes.
  - A post-TAA capture domain.
  - Engine-authored temporal fixtures. No third-party content: build them from
    procedural geometry. Cover:
    - thin geometry and high-frequency detail (fences, wires, alpha-mask foliage
      cards);
    - disocclusion from a moving occluder;
    - fast camera pan and cut;
    - a moving emissive object and moving transparent glass;
    - high-frequency specular on curved surfaces;
    - a static-camera stability case;
    - an HDR extreme (small super-bright highlights).
  - Metrics:
    - **Ground truth:** a high-sample jittered accumulation of a static scene
      (for example 64–256 frames) serves as a supersampled reference for error.
    - **Static-camera temporal stability:** frame-to-frame variance or flicker.
    - **Ghosting:** trail energy behind moving objects.
    - **Disocclusion recovery:** time to converge.
- **G7 — Probe-promotion race.** Before any probe route is admitted under TAA, add
  a warm-up past publication or a qualification-only finalize drain. Publication
  timing must not enter history nondeterministically.
- **G8 — Deterministic opaque tie-break.** This is feature tier, because images
  change by 1–2 depth-tie pixels on F3/F7-lod. Make draw order deterministic before
  TAA, because accumulated ties become visible shimmer. Record new reference hashes
  as a new feature set and keep R0 untouched.

## Main work after the gaps (refine order in your plan)

- **M9.1 — Motion vectors.**
  - Add a velocity target appended after `gbuffer.material-flags`, keeping
    attachment indices 0–4 stable. `VulkanPipelineMaxColorTargets`, the 3-or-5
    target check, and the opaque target list must change together.
  - Add a velocity write in forward-opaque.
  - Transparent targets stay single-colour; transparency feeds the reactive mask
    instead.
  - Velocity derives from GPU-scene current/previous records so later visibility-
    resolve and M8 mesh-shader emitters produce identical vectors (ADR-0006).
  - Choose the format and precision with evidence (for example `RG16F` in pixels or
    NDC).
  - Add a debug view and a CPU oracle comparison for analytic motion. Final output
    is unchanged, so this is refactor tier for images, but report GPU cost and
    bandwidth.
- **M9.2 — Native TAA (DLAA-class at 1:1).**
  - Insert it after the last `scene.color` writer (OIT resolve or compatibility
    forward) and before `bloom-hook`, as a new feature owner (`IVulkanFeature`)
    with declared passes and resources. Do not add it to `VulkanVertexBackend`.
  - TAA writes a new resolved resource that is a History pair. Decide and document
    what `final-capture-hook`, bloom, exposure, and `output-transform` read.
  - Starting points, each validated by evidence:
    - a low-discrepancy jitter sequence;
    - closest-depth dilated motion fetch;
    - Catmull-Rom or bicubic history sampling;
    - variance or YCoCg neighbourhood clipping;
    - HDR-aware weighting, since AP1 scene-linear has an extreme range and the
      previous frame's exposure is needed;
    - disocclusion rejection by depth and velocity;
    - anti-flicker;
    - an optional, measured sharpening step.
  - Candidates, each needing evidence: a negative texture LOD bias; specular-
    aliasing reduction (roughness or normal-variance filtering) as a complement
    to TAA, not instead of it.
  - Selection outline and grid overlays read unjittered inputs so they don't
    shimmer.
  - The TAA-off route remains a supported option. TAA becomes the production default
    only after feature-tier admission.
- **M9.3 — Reactive and transparency handling.** A reactive or transparency mask
  from M6 transparent passes, plus emissive and particle (WeightedOIT) behaviour.
  Ghost-free moving glass. Account for `transparent.compatibility.forward` writing
  fallback glass depth.
- **M9.4 — Bloom.**
  - A physically based, energy-conserving downsample/upsample chain (dual-filter
    style). Use no hard threshold by default; any threshold or knee is an explicit
    option.
  - Transient aliased graph images replace the inactive `bloom-hook`.
  - Scene-linear AP1 before the single output transform, read through a new
    output-set binding.
- **M9.5 — Auto-exposure.**
  - A log-luminance histogram in compute after TAA, with percentile clipping,
    separate up/down adaptation rates, EV100 limits, metering policy, and manual EV
    as compensation.
  - Adapted luminance as a History buffer pair. `output-transform` reads exposure
    through a descriptor.
  - Exposure survives cuts by policy, and the editor and asset-preview views adapt
    independently.
  - The project/profile setting owns parameters; persisted components hold no ImGui
    state.
- **M9.6 — History invalidation and robustness.** Resize, transport switches
  (ADR-0013 rebuilds the graph, so one unconverged frame is expected and must not
  flash), zero extent, cuts, teleports, new instances, LOD changes (LOD stays
  default-off and workload-selectable), and editor view switching.
- **M9.7 — Qualification and hand-off.** Five-process native-4K admission for TAA,
  bloom, and exposure; the frozen-set TAA-off byte identity; validation and
  synchronization validation; the shipping preset; the editor smoke test; and the
  F6 re-measure. Write a completion report and an M9-to-M7.9 hand-off, including
  what M9b needs.

## Invariants (summarised; the hand-off section 10 is authoritative)

- Keep one output transform and scene-linear AP1 until then. Bloom, TAA, and
  exposure operate on scene-linear data, and `output.display` keeps one writer.
- One clustered-light representation; shared BSDFs.
- Do not jitter visibility, culling, clustering, Hi-Z, shadow, or probe projections.
  Main, shadow, probe, and selection visibility stay independent.
- All GPU work is declared graph passes and resources. Barriers come from usages
  (one `vkCmdPipelineBarrier2` per pass, plus ADR-0016 item 6 cases only). History
  is never aliased. No transient is read before its writer.
- No render passes, `std::function` frame callbacks, `waitForAllFrames`, or blocking
  uploads. Use the deletion queue for resize and growth.
- Steady-frame C++ allocations stay at zero on the timing routes; check with
  `--qualification-allocation-trace`. Per-frame CPU work follows ADR-0015:
  frame-critical or inline, per-index outputs, and no order-deciding atomics.
- Qualification stays in `iridium_qualification` with no test hooks in core
  interfaces, and the shipping preset stays clean.
- Put new code in new feature owners. `VulkanVertexBackend.cpp` (2,334 lines) and
  `VulkanRenderGraphExecutor.cpp` (2,038) are near the 2,500-line guideline.
- Keep ADR-0013 live transport switching, ADR-0014 identity, and resize/zero-extent
  safety.
- LOD, Hi-Z, and VSM stay default-off. VSM must keep compiling and passing its tests.
  Note that `shadow.virtual.depth-mark` reads jittered depth.
- Always build through `tools/m7r/Build.ps1`. Machine state affects GPU timing, so
  record it with every pair.

## Evidence

- **Refactor tier** (G1–G7, M9.1 images): byte-identical frozen captures on the
  TAA-off route, Debug and Release tests (113 today; the shipping preset has 106),
  synchronization validation, and one matched native-4K timing pair.
- **Feature tier** (G8, TAA, reactive handling, bloom, exposure, and any change of
  default): five fresh native-4K Release processes per route, median/p95/p99,
  per-fixture envelopes, temporal metrics, memory, and counters.
- Inspect the actual images, both stills and motion sequences, before accepting
  hashes. Use `--require-capture-signal`. Judge ghosting, shimmer, softness, and
  highlight stability by eye as well as by metric. The owner prioritizes fidelity.

## Libraries, delegation, git, and content

- No new library is expected. If you want one (for example a reference TAA or a
  denoiser), first explain to the owner what it does, how it works, its license,
  and why it helps. Tracy remains deferred unless you justify it.
- You are the integration owner. Use subagents for read-only audits, fixture and
  metric tooling, and isolated shaders. Each parallel writer gets its own worktree.
  Never have two concurrent writers on the graph, executor, view ABI, common shaders,
  or CMake.
- Keep durable state in `docs/milestones/M9-temporal-and-post.md`. Write it first
  following `PLANS.md`, and get the owner's approval before the first
  implementation slice. Record decisions in its log.
- Commit each accepted slice with a message naming it, push, and keep the M9 PR
  updated. Check `git status` before every commit: never commit third-party content
  or `imgui.ini`.

## Out of scope

DLSS/FSR/XeSS integration and dynamic resolution (M9b), motion blur and depth of
field (a later post-processing slice, which the ROADMAP must name explicitly if you
recommend it), GTAO and GI denoising consumers (M10), RT (M11), VSM completion and
M7.9–M7.12, skinned motion (M13; keep the velocity contract ready for it), the
material editor follow-ups, the Porsche mixed-class glass defect, async compute,
and parallel command recording.

## Completion report

Follow AGENTS.md. Include before and after images and motion evidence, TAA,
bloom, and exposure costs against the 0.40 and 0.50 ms rows, memory, the F6
re-measure, and ADR changes (at least the ADR-0016 history notes, plus any new
ADR). Update `ROADMAP.md` (M9 `Accepted`, M9b defined), `FRAME_BUDGET.md`, and the
"Current direction" section of `docs/PROJECT_CONTEXT.md`.

---
