# Iridium Engine Agent Guidance

## Project direction

Iridium is a high-end, future-facing C++20/Vulkan engine. The reference system is an RTX 4090, Core i9-14900K, 64 GB DDR5-6000, a fast NVMe SSD, and a 4K HDR display. Visual fidelity is the primary goal, at the level of Unreal Engine 5, Frostbite, Anvil, and Northlight. Performance targets (owner decision, 2026-10-02):

- **Raster (no ray tracing): 144 FPS at native 3840x2160** in fully dressed gameplay scenes, a 6.94 ms base-render budget. Native temporal AA is expected; sub-native reconstruction is not the raster answer.
- **With hybrid ray tracing:** temporal reconstruction (DLSS-class or native TAAU) may render below output resolution; the displayed target stays 144 FPS.
- Frame generation never counts toward these targets.

Planned capabilities include wide-gamut HDR, GPU-driven rendering, mesh shaders, temporal reconstruction, and hybrid ray tracing. Architecture must leave headroom for heavy techniques; efficiency work exists to buy fidelity.

Do not optimize primarily for low-end hardware. Do not use the high-end target as justification for waste that produces no measurable fidelity or engineering benefit.

## Authoritative project context

Read these before roadmap work:

- `docs/PROJECT_CONTEXT.md`
- `ROADMAP.md`
- `PLANS.md`
- relevant records under `docs/architecture/`
- `docs/performance/FRAME_BUDGET.md` (budget table and evidence tiers)
- the active milestone's execution plan and lead prompt under `docs/milestones/`

Accepted architecture records are authoritative. If evidence requires changing one, propose a new superseding ADR instead of silently contradicting it.

## Working rules

- Reinspect the current source before acting; documentation describes direction and may lag implementation.
- Preserve unrelated and pre-existing worktree changes. Never run `git reset --hard`, `git clean`, or bulk checkouts over others' work.
- Keep the engine buildable after each implementation slice, and commit each accepted slice (with the owner's standing approval) with a message naming the milestone/slice. Do not let accepted work accumulate uncommitted.
- **Never commit or push third-party content** (models, textures, HDRIs, scenes, fonts, audio) unless the owner explicitly approves a specific asset and license. Such content is local-test only. `assets/` is allowlisted in `.gitignore`; keep it that way and check `git status` before every commit.
- Local third-party content lives in the local asset root named by the gitignored `iridium.local.json` (or `IRIDIUM_LOCAL_ASSET_ROOT`), never inside the repository or a worktree. Lanes read it in place: a worktree falls back to the main checkout's `iridium.local.json`. Do not copy, hard-link or junction assets into checkouts. See "Local asset library" in `docs/PROJECT_CONTEXT.md`.
- Third-party code libraries are allowed, but before adopting one, explain to the owner what it does, how it works, its license, and why it benefits the project. Record the decision in the milestone plan.
- Use the evidence tier in `docs/performance/FRAME_BUDGET.md`: behavior-preserving refactors need byte-identical captures plus one matched timing pair; feature admission needs the full five-process protocol.
- Keep qualification/oracle/test scaffolding out of production code paths where practical (harness, observer, or separate target); do not add new test hooks to core runtime interfaces.
- Use the RHI boundary for backend-neutral contracts. Keep Vulkan details in the Vulkan backend unless a capability genuinely belongs in the RHI.
- Keep scene lighting and transparency in linear scene-referred HDR until the final output transform.
- Share BSDF functions between deferred, forward, and future ray-tracing paths.
- Treat the M2 reference/production GBuffer as a measured canonical surface cache,
  not a permanent requirement that geometry always emit a full GBuffer. Keep material,
  geometry, and RHI contracts compatible with the accepted future visibility-buffer
  path in ADR-0006.
- When clustered lighting arrives in M5, use one light-assignment representation for
  deferred/material-resolve and complex-forward consumers.
- Keep runtime component data independent of ImGui, native file dialogs, and editor-only behavior.
- Do not merge disconnected transparent surfaces merely because they share a material.
- Prefer stable asset/component/entity identities over paths, RTTI names, or transient ECS indices.
- Parallel agents should default to read-heavy or disjoint work. Do not allow overlapping write-heavy changes in the same checkout.

## Build and verification

Windows debug configuration:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug --output-on-failure
```

Use the corresponding `x64-release` preset for performance measurement. Vulkan SDK, MSVC, Ninja, and `glslc` must be available.

A renderer change is not complete solely because it compiles. In proportion to risk, also validate:

- relevant automated architecture/unit tests;
- Vulkan validation output;
- representative reference scenes and screenshots;
- GPU timestamps and CPU frame-stage timings;
- transient and persistent VRAM changes;
- behavior in both common and feature-heavy material paths.

## Completion standard

For a roadmap slice, report:

- changed behavior and architecture;
- files and interfaces affected;
- verification performed and results;
- visual/performance comparison against baseline;
- remaining risks or deliberately deferred work;
- whether any ADR or roadmap status changed.
