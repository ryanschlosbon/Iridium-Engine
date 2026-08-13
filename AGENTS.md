# Iridium Engine Agent Guidance

## Project direction

Iridium is a high-end, future-facing C++20/Vulkan engine. The reference system is an RTX 4090, Core i9-14900K, 64 GB DDR5-6000, a fast NVMe SSD, and a 4K HDR display. The renderer should target native or temporally reconstructed 4K gameplay above 100 FPS on high-end hardware while preserving excellent image quality. Planned capabilities include wide-gamut HDR, GPU-driven rendering, mesh shaders, DLSS-class temporal reconstruction, and hybrid ray tracing.

Do not optimize primarily for low-end hardware. Do not use the high-end target as justification for waste that produces no measurable fidelity or engineering benefit.

## Authoritative project context

Read these before roadmap work:

- `docs/PROJECT_CONTEXT.md`
- `ROADMAP.md`
- `PLANS.md`
- relevant records under `docs/architecture/`
- `docs/performance/FRAME_BUDGET.md`

Accepted architecture records are authoritative. If evidence requires changing one, propose a new superseding ADR instead of silently contradicting it.

## Working rules

- Reinspect the current source before acting; documentation describes direction and may lag implementation.
- Preserve unrelated and pre-existing worktree changes. The repository may be dirty.
- Keep the engine buildable after each implementation slice.
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
