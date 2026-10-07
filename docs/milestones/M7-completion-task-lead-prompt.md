# Prompt for the M7 Completion Lead — M7.9 through M7.12

Written by the director session on 2026-10-07. Paste everything below the rule into
a fresh Claude Code session opened on the Iridium Engine repository (Opus, high
reasoning effort recommended). Run it locally: the work needs MSVC, the Vulkan
SDK, the RTX 4090, and the local asset library, none of which exist in a cloud
session.

---

You are the milestone lead for **Iridium Engine M7 completion: slices M7.9–M7.12**
of "GPU Scene and Indirect Visibility". One lead owns all four slices, starting with
the owner's performance cases. Own the work from audit through implementation,
verification, the M7 acceptance report, and the M7-to-M9c hand-off. Continue slice
to slice without waiting for prompts, except at the **owner checkpoint** described
below and for genuine blockers, product-intent decisions, or new third-party
libraries.

## Product context

Iridium is a C++20/Vulkan engine whose primary goal is visual fidelity at the level
of UE5, Frostbite, Anvil, and Northlight. The reference machine is an RTX 4090,
i9-14900K, 64 GB DDR5-6000, and a 4K HDR display.

The raster target is **144 FPS at native 3840x2160 without ray tracing (6.94 ms)**.
Budgets are in `docs/performance/FRAME_BUDGET.md`: geometry and surface 1.20 ms,
shadows 1.00, direct lighting 0.90, transparency 0.70, GI/AO 1.20, TAA 0.40, post
0.50, UI 0.40, margin 0.64. Efficiency work exists to buy fidelity; never trade away
meaningful silhouette, material, shadow, transparency, HDR, or temporal quality.

Where things stand:

- **M0–M6 accepted.**
- **M7.0–M7.7 accepted** (2026-08/09):
  - persistent generation-safe GPU scene (ADR-0014);
  - geometry arenas;
  - device frustum compaction with indexed indirect-count submission;
  - LOD and Hi-Z, both workload-selectable and default-off;
  - independent device-built shadow and probe commands.
- **M7.8 Virtual Shadow Maps:** paused at a checkpoint, default-off. It resumes after
  M8 and is **not** part of this milestone.
- **M7R architecture consolidation accepted** (2026-10-04):
  - module libraries and a qualification harness;
  - graph-driven execution with feature owners (ADR-0016);
  - synchronization2, dynamic rendering, VMA, transient aliasing, deferred deletion,
    pipeline cache, and a transfer queue;
  - the enkiTS task system (ADR-0015) with parallel change-driven extraction;
  - zero steady-frame allocations.
- **M9 temporal and post accepted** (2026-10-06):
  - native TAA, motion vectors, auto-exposure, and bloom, all now **on by default**;
  - per-view History with reset policy, and history that survives compatible
    rebuilds (ADR-0017);
  - the revealage reactive mask and motion-aware transparent coverage.
  - The heaviest timing route is 3.95 ms GPU with everything on.

## Read before changing code

1. `AGENTS.md`: rules, git and branch policy, the local asset library, the library
   policy, and evidence tiers.
2. `docs/milestones/M9-to-M7.9-handoff.md`. This is your primary brief: contracts,
   tooling, open items, and section 6 "Notes for M7.9–M7.12".
3. The `ROADMAP.md` M7 section, including the **owner-observed performance cases
   (2026-10-07)**, and the Program schedule.
4. `docs/milestones/M7-gpu-scene-indirect-visibility.md`: the header, the
   optimization candidate register, the M7.9–M7.12 slice definitions, the
   risks, and the decision log. Skim the M7.5–M7.7 history only as needed.
5. `docs/performance/FRAME_BUDGET.md` (evidence tiers, M7R CPU baseline, M9
   admission) and `docs/performance/M7-performance-fidelity-contract.md`
   (candidate gates and experiment template).
6. ADR-0003, ADR-0006, ADR-0012, ADR-0014, ADR-0015, ADR-0016, and ADR-0017. Read
   the others where you touch their code.
7. The `docs/PROJECT_CONTEXT.md` sections "Current direction" and "Local asset
   library". `PLANS.md` gives the plan structure.

Then reinspect the source and `git status`. The source is authoritative over this
prompt; record any corrections.

## Plan, branch, and owner checkpoint

- **Branch** `m7-completion` from `main`. `main` is the integration branch; open one
  PR into `main` and keep it updated.
- **The local `project.settings.json` diff** on `main` (bloom intensity and levels) is
  the owner's working state. Don't commit it unless the owner says so; the same goes
  for `imgui.ini`.
- **Write `docs/milestones/M7-completion.md`** following `PLANS.md`. It is the
  focused execution plan for M7.9–M7.12. Link it from the M7 plan's header, and
  supersede the old slice text where your audit corrects it rather than duplicating
  the 1,400-line plan.
- Get the owner's approval of the plan before the first implementation slice.
- **Owner checkpoint after phase 1** (the performance cases plus the M7.10 fixes
  they motivate):
  - stop and report the fixtures, attribution, fixes, and before/after numbers;
  - give the owner time to verify by eye in the editor;
  - continue to M7.9 only after the owner replies.

## Phase 1 (first work): the owner's performance cases

Capture each as a native-4K Release fixture before optimizing. Report GPU pass
times, non-waiting CPU stages, presentation waits separately, requested, visible,
and recorded work, and VRAM. Never use title-bar FPS.

1. **Many objects.** Frame rate falls steeply as model instances are added.
   - **Measured lead:** sorted and compatibility transparency is **not frustum-
     culled**. With the Porsche fully out of view, all of its transparent packets
     were still requested, sorted (26,910 ambiguous sort intervals for one car), and
     recorded.
   - Make transparent culling, sorting, and per-packet CPU work scale with
     *visible* transparent work.
   - Then measure what else scales with instance count: extraction, per-instance
     publication, opaque compaction, command recording (parallel recording was
     deferred from M7R), shadow casters, and probes.
   - The fixture uses multiple Porsche/Alfa-class instances from the local asset
     library, plus the existing 256-instance stress fixture.
2. **Camera close to glass.** Cost rises sharply as glass covers the screen.
   Candidates to measure:
   - per-pixel complex-forward glass shading;
   - refraction-pyramid builds and sampling;
   - layered-tier atlas capacity and composition;
   - overdraw from nested interfaces;
   - the transparent motion and reactive work M9.8e added.
3. **A very bright point light.** Cost rises with intensity. Candidates to measure:
   - an influence radius derived from intensity, which means more clusters and more
     shaded pixels per light;
   - point-shadow cube faces and caster counts inside that radius;
   - any per-light work that an authored or perceptual cutoff should bound instead.

   Any cutoff must be physically defensible. Lights must not visibly clip, and
   ADR-0007's inverse-square photometric model stays. Prefer a perceptual threshold
   in exposed luminance, plus an explicit authored range.

Fixes from these cases are M7.10 work. Each is either a behavior-preserving
refactor (refactor tier: byte-identical TAA-off frozen set plus one matched timing
pair) or an image-changing change (feature tier). Classify each one explicitly.

## Phase 2: remaining slices (refine order in your plan)

- **M7.10 — CPU, cluster, compiler, and scheduling.**
  - Before any other backend growth, free headroom in
    `src/renderer/vulkan/VulkanVertexBackend.cpp` (**2,496 of its 2,500-line cap**)
    by moving code into feature owners.
  - Reprofile the clustered-light assignment on current workloads. It must stay one
    shared product for deferred and forward (ADR-0006/0007).
  - Cache per-view derived math.
  - Evaluate parallel command recording, which is the largest remaining H-stress
    CPU cost.
  - Revisit the R5c.4e CPU frustum classification, which must use unjittered
    matrices.
  - Benchmark-gated candidates from the M7 register: DirectXMath, GLM SIMD, or SoA
    kernels behind an internal boundary; LTCG/PGO and an AVX2 tier; a conditional
    depth prepass; async compute (timeline-gated). Classify each as production,
    workload-selectable, experimental, or rejected, with evidence.
- **M7.9 — Fine-grained cooking and progressive residency.**
  - Per-primitive child CookKeys for geometry and LOD, and texture keys decoupled
    from geometry settings. Today a geometry-only setting recooks all 76 embedded
    textures.
  - A bounded deterministic cook job graph on the M7R task system (`Background`
    priority) with cancellation and editor priority.
  - Progressive, budgeted, visible-demand publication with last-known-good and
    semantic or coarser fallbacks, so oversized models arrive without a one-shot
    upload hitch.
  - **M9 contract 3:** streaming and LOD swaps must not invent motion. Previous
    transforms survive content swaps of the same instance; check with TF-teleport
    and TF-disocclude motion evaluation.
  - Prove the M7 acceptance edit cases: material-only, one primitive, one texture,
    unchanged, superseded edit, and oversized model.
- **M7.11 — Indexed visibility/material-resolve experiment.**
  - Standard opaque surfaces only. The payload must work for indexed and later M8
    mesh-shader emitters, with derivative-correct attribute reconstruction.
  - It resolves into the M2 canonical surface cache and reuses existing lighting.
  - The resolve writes **velocity** and **scene-colour alpha 1** (M9 contracts 1–2).
    Its output must stay bit-compatible with the TAA-off identity route at refactor
    tier, or justify a feature-tier change.
  - Compare the complete chain against packed deferred: images, timing, bandwidth,
    memory, and material coherence.
  - Conventional packed deferred remains the fallback. Promote only on a
    representative-scene win.
- **M7.12 — Production qualification and hand-off.**
  - Run the roadmap M7 acceptance gate: CPU preparation and submission scale with
    changed data and visible batches; a fixed native-4K three-high-fidelity-asset
    scene covers static, moving, off-screen, occluded, and LOD cases with
    presentation wait separated; recook and residency behavior; the visibility-path
    decision.
  - Add TAA-on fixtures (the `scene-resolved` domain) with measured envelopes to the
    qualification sweep.
  - Re-run the owner performance cases as before/after evidence.
  - Write the M7 acceptance report. It accepts M7 with **M7.8 explicitly carried to
    its post-M8 resumption**, not silently dropped.
  - Write `docs/milestones/M7-to-M9c-handoff.md`. M9c (camera and post-process
    stack) is next on the schedule.

## Invariants (summarised; the M9 hand-off contracts and ADRs are authoritative)

- **Identity.** Scene UUID and asset/subasset GUIDs remain the persistent identity;
  GPU slots, offsets, and indices are transient (ADR-0014). Material-only edits never
  recook geometry.
- **Visibility.** Main, shadow, probe, and selection visibility stay independent;
  main-camera culling never removes a shadow caster or reflection participant.
  Uncertain culling or residency fails visible or uses an explicit valid fallback.
  Transparent culling is per view and conservative over each transparent
  primitive's bounds.
- **M9 contracts.** Scene-colour alpha is revealage, and every opaque writer outputs
  1. Every opaque writer of scene colour also writes velocity. Never feed jittered
  matrices to culling, Hi-Z, clustering, LOD, shadows, probes, or layered atlas
  rects. Temporal state goes through `createHistory` with the right reset policy.
- **Shading and transparency.** One clustered-light representation; shared BSDFs;
  scene-linear AP1 until one output transform. M6 classified transparency keeps its
  routing and ordering semantics; culling must not reorder visible transparent work.
- **Graph and frame.** Declared passes and resources; barriers from usages; no
  render passes, `waitForAllFrames`, blocking uploads, or `std::function` frame
  callbacks; zero steady-frame allocations on the timing routes
  (`--qualification-allocation-trace`); ADR-0015 rules for frame-critical tasks.
- **Fallbacks.** LOD and Hi-Z stay workload-selectable unless their own gates pass.
  Conventional shadows and direct submission remain available. VSM stays
  default-off and must keep compiling and passing its tests.
- **Layering.** Qualification stays in `iridium_qualification`, with no test hooks
  in core interfaces; the shipping preset stays clean. Put new code in feature
  owners; respect the 2,500-line guideline.
- Keep ADR-0013 live transport switching, ADR-0017 history across rebuilds, and
  resize/zero-extent safety.

## Evidence

- Always build through `tools/m7r/Build.ps1`.
- **Refactor tier:** the TAA-off, Manual-exposure, bloom-off route (pinned by
  `Get-M7REngineBaseArgs`) reproduces the frozen set (`out/m7r/captures/m9-g8`,
  F4-woit within its envelope) byte-for-byte. Also: Debug and Release tests,
  synchronization validation, and one matched native-4K timing pair.
- **Feature tier:** `tools/m9/Run-FeatureAdmission.ps1 -RequireQuiet` (five
  processes per side), per-fixture envelopes, the temporal motion evaluation where
  motion or streaming is involved, memory, and counters.
- **Machine state.** Timing needs a quiet machine. The owner sometimes runs games;
  **ask the owner before starting admission or timing runs**, and record the machine
  state with every pair.
- **Disk.** C: fills quickly. 4K 64-phase references take about 6.4 GB each
  (transient), and a lane worktree with builds takes about 25 GB. Delete build
  directories and transient references when a lane ends. Ask the owner before
  removing any worktree.
- **Worktree safety.** Before any recursive delete, scan for junctions and symlinks.
  A junction delete once wiped the local asset library.
- Inspect real images, stills, and motion before accepting hashes
  (`--require-capture-signal`). The owner judges fidelity by eye.

## Libraries, delegation, git, and content

- New third-party libraries require an explanation to the owner first: what the
  library does, how it works, its license, and why it helps. Likely candidates:
  meshoptimizer for cooking and meshlet prep, or a PGO toolchain change.
- You are the integration owner. Use subagents for read-only audits, fixtures,
  metric tooling, cooker children, and isolated kernels. Each parallel writer gets
  its own worktree, built under the disk rules. Never have two concurrent writers
  on the graph, executor, GPU-scene/RHI headers, common shaders, model schemas, or
  CMake.
- Keep durable state in the plan and its decision log. Commit each accepted slice
  with a message naming it, and push. Run `git status` before every commit: no
  third-party content, no `imgui.ini`, and no owner `project.settings.json` changes.
- Local third-party content stays in the local asset root (`iridium.local.json`).
  Never copy, link, or junction it into a checkout. Manifests record content SHAs
  only.

## Out of scope

M9c camera and post-process volumes, grading, motion blur, DoF, and lens effects;
M8 meshlets and mesh shaders; resuming M7.8 VSM; M10 GI, AO, and area lights; M9b
DLSS/FSR/XeSS; M11 RT; the M9 TAA open items (sub-pixel wires, shadow-ghost trails,
the WeightedOIT halo, motion sharpening); the material editor follow-ups; and the
Porsche mixed-class glass ordering defect, unless a performance case proves it is
the same code path, in which case raise it with the owner first.

## Completion report

Follow AGENTS.md. Include:
- performance-case before/after numbers;
- every candidate's classification with evidence;
- cook, residency, and recook results;
- the visibility-buffer decision;
- memory and VRAM;
- images;
- ADR changes;
- remaining risks.

Update `ROADMAP.md` (M7 `Accepted` with M7.8 carried, M9c `Ready`),
`FRAME_BUDGET.md`, and the "Current direction" section of
`docs/PROJECT_CONTEXT.md`.

---
