# ADR-0002: Render Graph with Scene-Linear HDR and Explicit Output Transforms

- Status: Accepted direction; implementation design belongs to M1
- Date: 2026-07-17
- Owners: Renderer, RHI, and platform output

## Context

The reviewed renderer tone-maps opaque scene color before glass and tone-maps glass again. That makes transmission, reflection, emissive bloom, and exposure dependent on pass order rather than radiometric scene values. Future temporal reconstruction, ray tracing, denoisers, capture tools, and external SDKs also require explicit resource lifetimes and states.

HDR support is more than an FP16 render target. It requires a defined scene working space, exposure, tone mapping, gamut mapping, display transfer function, paper white, peak luminance, swapchain/color-space negotiation, metadata where relevant, and a UI composition policy.

## Decision

Introduce a backend-neutral render graph whose passes declare read/write usage and whose Vulkan executor owns barriers, layouts, queue synchronization, transient aliasing, and history/external resource integration.

Opaque lighting, sky, emissive, transparency, volumetrics, and bloom inputs remain in one scene-linear HDR pipeline. Tone mapping and gamut mapping occur once in the final output transform. UI is composed through a documented color-managed path.

The engine will support:

- SDR output;
- an HDR development path such as scRGB where supported;
- HDR10/PQ output with wide-gamut mapping and platform metadata where supported.

The internal working gamut and exact output operators will be selected during M1 with test imagery and target-display measurements. Display-referred values must not leak back into lighting.

## Consequences

- Transparent/refraction passes sample linear HDR scene resources, not tone-mapped swapchain images.
- Emissive values may exceed display white and participate consistently in exposure and bloom.
- Render-graph resources distinguish transient, persistent, history, imported, and exported lifetimes.
- The RHI exposes capabilities and operations; Vulkan-specific barrier and swapchain details remain in the backend.
- Automated captures must record output mode and should retain a scene-linear reference when practical.

## Rejected alternatives

- Continue manual pass sequencing: initially smaller, but fragile as temporal, async, RT, and SDK dependencies grow.
- Tone-map before transparency: prevents physically coherent composition and causes repeated or mismatched output transforms.
- Treat HDR as a swapchain-format toggle: omits color-space and luminance management.
