# Prompt for the M7R Milestone Lead — Architecture Consolidation

Written by the director session on 2026-10-02. Paste everything below the rule into
a fresh Claude Code session opened on the Iridium Engine repository (Opus, high
reasoning effort recommended).

---

You are the milestone lead for **Iridium Engine M7R — Architecture
Consolidation**. Own it from audit through implementation, verification, the
completion report, and the M7R-to-M9 handoff. Continue slice to slice without
waiting for prompts unless you hit a genuine blocker or a decision that changes
product intent; then ask the owner concisely.

## Why this milestone exists

Iridium is a C++20/Vulkan engine whose primary goal is visual fidelity at the
level of UE5, Frostbite, Anvil, and Northlight. The raster target is **144 FPS at
native 3840x2160 without ray tracing (6.94 ms)** on an RTX 4090 / i9-14900K /
64 GB DDR5-6000. Dressed scenes already measure 4.2–5.8 ms GPU **before** AA, AO,
GI, bloom, or auto-exposure exist. The next milestones (M9 temporal AA and post,
M10 GI/AO, M11 RT) need structural headroom and a codebase that can absorb them.

The 2026-10-02 takeover audit found:

- `src/renderer/vulkan/VulkanVertexBackend.cpp`: 13,286 lines, ~385 members. Spot,
  point, directional, and probe indirect culling are near-duplicate copies
  (two validation collectors are byte-identical). `submitForwardQueues` is ~914 lines.
- `src/core/Application.cpp`: 6,703 lines. `drawFrame` ~1,685 lines, one ~540-line
  packet-extraction lambda. Scene state is walked three times per frame, and the
  M7.2 "parity stage" still rebuilds 240-byte `DrawPacket`s from the GPU scene.
- Qualification/oracle/capture code is ~20–30% of both files. `IRenderBackend`
  (69 virtuals) exposes test hooks, there are 83 CLI flags, and validation passes
  live in the production graph.
- The render graph declares ~45 passes with usages and does lifetime slot reuse
  and automatic barriers, but execution is ~30 imperative `beginPass("name")`
  calls string-matched against the compiled plan. Barriers are one
  `vkCmdPipelineBarrier` per resource, with 12 more hand-written in the backend.
- Vulkan 1.3 is requested, but synchronization2, dynamic rendering (23
  `VkRenderPass` objects), timeline semaphores, and buffer device address are
  unused. One `vkAllocateMemory` per resource, no transient memory aliasing, no
  pipeline cache. 20 `waitForAllFrames()` sites plus synchronous fence-blocking
  uploads cause hitches. There is a single graphics queue.
- The frame loop is single-threaded. ~10 services each own a dedicated
  `std::jthread`, and there is no task system.
- A 1,832-line single `CMakeLists.txt`, ~98 sources listed directly on the
  executable, tests recompiling production sources (one file built 15×), and no
  PCH or unity builds.
- `tests/renderer/Stage3ArchitectureTests.cpp` asserts on source text (67
  `source.find` checks). It will break under any refactor and must be replaced
  with behavioral tests.

Strengths to preserve: the clean RHI boundary (no Vulkan types in `rhi/`), the
generation-safe GPU scene (ADR-0014), indirect-count submission for opaque, shadow,
and probe passes, the asset/cook/DDC/identity pipeline, canonical material closures,
scene-linear AP1 with a single ACES 2 output, and zero steady-frame allocations.

## Read before changing code

- `AGENTS.md` (rules, including the git, third-party content, and library policy)
- `docs/performance/FRAME_BUDGET.md`: the 6.94 ms table and **evidence tiers**
- `ROADMAP.md`: the M7R section and Program schedule
- `PLANS.md`: execution-plan structure
- ADR-0002 (render graph and HDR), ADR-0003, ADR-0006, ADR-0013, ADR-0014. Skim the
  others for constraints on code you touch.
- `docs/PROJECT_CONTEXT.md` "Current direction" section only. The remainder is a
  dated record.

Then reinspect the source and `git status`. Repository files are authoritative over
this prompt; if they disagree, trust the source and record the correction.

## First deliverable: the execution plan

Write `docs/milestones/M7R-architecture-consolidation.md` following `PLANS.md`,
with an audit of your own that confirms or corrects the findings above. Get the
owner's approval of the plan before slice R1. Keep it concise; the Codex-era plans
were far longer than necessary. Record decisions in its log, not in chat.

## Recommended slices (refine in the plan)

Keep exactly one slice in progress. The engine builds, tests pass, and output is
byte-identical after every slice.

- **R0 — Baseline freeze.**
  - Pick the frozen capture set: the M7 three-dense-assets fixture (all-visible
    and one-visible), the 256-instance stress fixture, one M6 transparency
    fixture (Ordinary2 plus Cinematic8 or WeightedOIT), a directional/spot/point
    shadow fixture, and a reflection-probe fixture.
  - Record scene-linear and final-SDR hashes, using `--require-capture-signal`.
  - Record one native-4K Release timing pair.
  - Record clean and incremental build times (touch `Application.cpp`, a
    common RHI header, and a shader).
  - Record per-frame `waitForAllFrames` and allocation counts.
- **R1 — Build system.**
  - Per-module `CMakeLists.txt` with STATIC/OBJECT libraries (core, ecs, scene,
    assets, material, renderer-rhi, renderer-vulkan, editor, profiling,
    capture/benchmark).
  - Tests link the libraries instead of recompiling sources.
  - `target_precompile_headers` for std, glm, and Vulkan headers; unity builds
    only for vendor code and tests where they don't hide ODR problems.
  - Report build-time deltas.
- **R2 — Qualification harness and CLI.**
  - Move oracles, readbacks, `IRIDIUM_*_VALIDATION` reporting, and capture
    validation behind an observer/extension interface into a separate library
    that a shipping configuration can omit.
  - Remove test hooks from `IRenderBackend`.
  - A data-driven CLI flag registry.
  - Replace source-text tests with behavioral ones.
  - Keep every qualification capability working, because evidence must stay
    reproducible.
  - `--developer-legacy-transparency` is a removal candidate; ask the owner.
- **R3 — Graph-driven execution and backend decomposition.**
  - Passes register setup/execute callbacks, and the executor runs them in
    compiled order using pass and resource indices, not strings.
  - Barriers are batched per pass.
  - Split `VulkanVertexBackend` into pass/feature owners as each pass moves. One
    shared `VulkanIndirectViewCuller` (configured per view kind) replaces the
    duplicated directional/spot/point/probe/main prepare, bind, create, and
    validate code.
  - `IRenderBackend` should shrink toward frame-level operations rather than a
    fixed list of submit stages. Keep the RHI backend-neutral.
- **R4 — Vulkan modernization.**
  - Synchronization2 barriers and dynamic rendering (remove `VkRenderPass` and
    framebuffers where possible).
  - VMA suballocation with aliased transient graph memory.
  - Fence- or timeline-keyed deferred deletion replacing `waitForAllFrames()` on
    capacity growth and resource changes.
  - A persisted `VkPipelineCache`.
  - Uploads on a dedicated transfer queue with timeline semaphores.
  - Async compute is **not** in scope beyond leaving the graph able to express it;
    record it as a later timeline-gated candidate.
- **R5 — CPU frame and application decomposition.**
  - Split `Application` into frame orchestration, render extraction, editor host,
    and asset integration.
  - Introduce the task system and migrate the per-service threads to it, with
    priorities so background cooking never starves the frame.
  - Parallelize extraction and the transform update.
  - Make extraction change-driven using the existing change journals.
  - Sort compact keys instead of whole packets.
  - Retire the M7.2 parity packet path for ordinary opaque work.
  - Report serial main-thread time against the ≤3.0 ms target.
- **R6 — Qualification and handoff.**
  - Full test suites, validation runs, and frozen-set byte identity.
  - The timing pair plus hitch and p99 evidence for removed GPU drains.
  - Build-time results.
  - A completion report and `docs/milestones/M7R-to-M9-handoff.md`. That
    handoff should list exactly where motion vectors, jitter, history resources,
    bloom, and exposure plug into the new graph/pass model.

Size guideline: no source file above ~2,500 lines without a written justification.

## Invariants

- Output is byte-identical on the frozen set unless a documented floating-point
  reassociation forces otherwise; that case uses feature-admission thresholds.
- No median or p99 CPU/GPU regression beyond noise in the matched timing pair.
- Vulkan validation is clean, including synchronization validation for R3 and R4.
- Steady frames still make zero C++ allocations.
- Preserve the ADRs:
  - one clustered-light representation;
  - scene-linear AP1 until one output transform;
  - independent main/shadow/probe visibility;
  - generation-safe GPU-scene identity;
  - live display-transport switching (ADR-0013);
  - resize/zero-extent safety.
- LOD and Hi-Z remain workload-selectable. M7.8 Virtual Shadow Map code remains
  default-off and must still compile and pass its tests; it resumes after M8.
- Runtime code stays independent of ImGui and editor behavior. Do not deepen the
  UI coupling in `IRenderBackend`; move it toward an editor-host layer if you
  touch it.
- If a change alters an accepted architecture decision, write a superseding ADR.
  Expected new ADR: **threading/task model**. Possibly an ADR-0002 refinement for
  the graph execution model.

## Third-party libraries

The owner allows libraries but requires an explanation of what each does, how it
works, its license, and why it helps, given **before** adoption. Record the
decision in the plan. Pin versions via FetchContent with an exact tag or commit,
as the existing dependencies do. Never vendor third-party *content*.

Candidates the director has already explained to the owner:

- **VMA (Vulkan Memory Allocator, AMD GPUOpen, MIT).** Suballocates buffers and
  images from large `VkDeviceMemory` blocks. It chooses memory types, handles
  alignment and dedicated allocations for large render targets, tracks
  `VK_EXT_memory_budget`, supports aliasing and defragmentation, and offers
  persistent mapping. It replaces per-resource `vkAllocateMemory`, which is slow,
  limited by `maxMemoryAllocationCount`, and prevents transient aliasing.
  Integrate it behind the existing `VulkanResourceAllocator` so memory profiling
  categories stay intact.
- **enkiTS (zlib).** A small work-stealing task scheduler: task sets over index
  ranges, pinned tasks for main-thread-only work, and multiple priority levels.
  Priorities let latency-critical frame work outrun background cooking on the
  14900K's mixed P/E cores without hand-set affinity. Evaluate Taskflow (MIT) as
  the alternative and choose with evidence; record why in the threading ADR.
- **Optional: Tracy (BSD-3).** A frame profiler with CPU zones, Vulkan GPU zones,
  locks, memory, and a timeline viewer. It complements the existing counters for
  diagnosing threading work in R5. Compile it out by default.

Anything else needs its own explanation to the owner first.

## Delegation, context, and git

- You are the integration owner. Use subagents for read-only audits, test ports,
  CMake mechanics, and isolated kernels. Give each parallel writer its own git
  worktree. Never have two writers on shared backend/RHI headers, common shaders,
  or CMake at the same time. Review delegated work against the plan before
  accepting it.
- Keep durable state in the plan, not in chat, so a fresh session can resume. Use
  the evidence tier for refactors and do not run the five-process protocol where
  it isn't required.
- Commit each accepted slice on a branch off `Render-Refactor-for-Modularity`
  (for example `m7r-consolidation`), with messages naming the slice. Push and keep
  one PR open for the milestone. Check `git status` before every commit: no
  models, textures, HDRIs, scenes, or `imgui.ini`. `assets/` is allowlisted; keep
  it that way.

## Out of scope

New rendering features (TAA, bloom, auto-exposure, AO, GI, RT), VSM completion,
M7.9–M7.12, mesh shaders, the material editor follow-ups, the Porsche mixed-class
glass defect, and a wholesale ECS or math-library rewrite.

## Completion report

Follow AGENTS.md: changed architecture, interfaces, verification results with
hashes and timings, build-time and hitch deltas, remaining risks, ADR changes, and
the M9 handoff. Update `ROADMAP.md` (M7R `Accepted`), `FRAME_BUDGET.md` if CPU
figures change, and the "Current direction" section of `docs/PROJECT_CONTEXT.md`.

---
