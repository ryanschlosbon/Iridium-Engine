# ADR-0013: Runtime Display-Transport Switching

- Status: Accepted; implemented as post-M6 editor hardening
- Date: 2026-08-27
- Supersedes: ADR-0002 only where it requires an application restart to change
  SDR, scRGB, or HDR10 transport

## Context

ADR-0002 correctly requires the scene to remain linear and scene-referred until one
final output transform. It also kept display-transport selection at startup because
changing the Vulkan surface format/color space can change the output target, HDR10 UI
composition topology, final encode pass, render passes, pipelines, framebuffers, and
descriptors. That restriction makes HDR difficult to discover and calibrate in the
editor even though Windows desktop HDR does not require exclusive fullscreen.

## Decision

The RHI exposes a frame-boundary display-transport change. The Vulkan backend creates
and validates the replacement swapchain first, waits for active GPU frames once, and
rebuilds only presentation-dependent output resources: the output pass, optional
HDR10 encode pass, UI render pass/pipeline, render-graph display targets,
framebuffers, and their descriptors. Scene entities and resident geometry, materials,
textures, environments, and editor documents remain intact.

The requested preference and effective transport are distinct. `Auto` resolves in
this order:

1. Windows extended-linear scRGB;
2. HDR10 Rec.2100/PQ;
3. SDR sRGB.

Explicit unsupported HDR requests retain the established SDR fallback and diagnostic.
The editor disables concrete modes not exposed by the current surface, always permits
Auto, and shows requested/effective status. Auto is reevaluated on every later
swapchain recreation. HDR10 metadata remains conditional on `VK_EXT_hdr_metadata`.

The application binds the ACES output LUT for the **effective** transport after the
cutover. SDR uses the Rec.709/100-nit LUT; scRGB and HDR10 use the common P3-D65
1000-nit LUT. No scene pass sees encoded display values, and UI remains composed in
the transport-specific display-linear path before the sole PQ encode when applicable.

Transport changes are intentionally not a steady-frame operation. They incur a
visible, bounded frame-boundary stall and transient presentation-resource churn, like
a conventional game display-mode change. No scene asset is reimported or reuploaded.

## Consequences

- Artists can select Auto, SDR, scRGB, or HDR10 under **Window > Project Settings**
  without restarting Iridium or reopening the scene.
- Moving to another display takes effect when the swapchain is next recreated; Auto
  does not add periodic format polling to steady frames.
- Validation can exercise scRGB, HDR10, then SDR in one process with
  `--validate-output-transport-switch`.
- The startup CLI remains useful for automation and now also accepts
  `--output-transport auto`.

