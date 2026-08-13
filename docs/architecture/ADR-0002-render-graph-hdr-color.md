# ADR-0002: Render Graph with Scene-Linear HDR and Explicit Output Transforms

- Status: Accepted and implemented by M1
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

The M1 owner decision selects linear ACEScg/AP1 as the internal scene working gamut.
Standard Windows SDR remains the default through an explicit Rec.709/sRGB output
profile. Output profiles carry primaries, white point, transfer, and luminance so
Display-P3, cinema DCI-P3, Rec.2020/HDR10, and other targets do not require changes to
scene shading.

The production operator is the configurable ACES 2 Output Transform. Its rendering
and display-encoding portions remain separable so display-linear UI is composed at
paper white before final encoding. The current fitted ACES curve remains a selectable
compatibility operator, not an implicit shader step. Display-referred values must not
leak back into lighting.

## Consequences

- Transparent/refraction passes sample linear HDR scene resources, not tone-mapped swapchain images.
- Emissive values may exceed display white and participate consistently in exposure and bloom.
- Render-graph resources distinguish transient, persistent, history, imported, and exported lifetimes.
- The RHI exposes capabilities and operations; Vulkan-specific barrier and swapchain details remain in the backend.
- Automated captures must record output mode and should retain a scene-linear reference when practical.

## M0 evidence constraining M1

The accepted M0 baseline records the current reference device and output path as
native 3840x2160, `VK_FORMAT_B8G8R8A8_SRGB`, sRGB nonlinear color space, Mailbox,
and no reconstruction. The current legacy final target is display-referred and the
transparent path samples and tone-maps that target again.

At 4K with three swapchain images, each `RGBA16F` GBuffer category requests
199,065,600 bytes. Each legacy 4-byte scene color/copy or depth category requests
99,532,800 bytes. Total static-fixture scope is about 949.220 MiB requested and
956.253 MiB engine-committed. M1 should allocate graph-owned raster intermediates by
frame-in-flight rather than swapchain-image count and reuse compatible physical
images across nonoverlapping lifetimes. This makes an `RGBA16F` scene color and copy
feasible without accepting avoidable per-swapchain duplication.

M0 also establishes bit-exact 4K final captures, a six-step opaque-emissive range,
nested transparency, exact pass timings, and a separate memory category baseline.
M1 uses those as before/after evidence but must add a distinct high-precision
scene-linear capture; it must not relabel the M0 8-bit artifacts.

The owner resolved the remaining M1 color choices on 2026-07-19: ACEScg/AP1 working
space; configurable ACES 2 production output with the fitted ACES compatibility
option; scRGB before HDR10/PQ; user-adjustable defaults of 203-nit paper white and
1,000-nit peak; and manual EV exposure in M1 with automatic adaptation deferred to
M9. The owner granted M1 execution-plan approval on 2026-07-19.

## M1 implementation evidence

M1 execution was approved on 2026-07-19. M1.1 accepted the backend-neutral graph
compiler and allocation-free compiled-plan cache. M1.2 accepted an opt-in Vulkan
shadow executor without changing presented rendering. Source comparison corrected
the maximum legacy declaration to include separate scene-color copies for the
background and foreground transparent layers. The resulting nine-pass declaration
compiles to eight logical resources, seven engine-owned physical slots per frame
context, one imported swapchain resource, and 25 Vulkan barrier intents.

M1.3 accepted opt-in graph execution with graph-owned offscreen targets across two
fenced frame contexts. The selectable legacy path remains the fallback until M1.7.
The accepted executor checks pass order, owns offscreen transitions, tracks physical
slot state across dynamic skipped passes, and integrates capture transitions. The
imported swapchain's presentation layouts remain a Vulkan render-pass contract.

All eight 4K M0 comparisons are bit-exact and Debug Vulkan validation is clean.
Live committed engine memory is 668,470,976 bytes, 334,233,600 bytes (33.33%) below
the former three-swapchain-image target ownership, with no shadow allocation in graph
mode. Three-run Release medians show no CPU or GPU regression. The detailed evidence
is `docs/milestones/M1.3-graph-executed-sdr-parity.md`. This implements the graph
ownership portion of this ADR.

M1.4 implements the scene-linear portion: source material/environment boundaries
transform linear Rec.709/sRGB primaries into ACEScg/AP1; FP16 scene color carries
opaque, emissive, environment, refraction, and transparency without display mapping;
and PFM captures that domain before output. The fitted legacy curve moved to a single
explicit compatibility-output pass. A disabled bloom hook exposes the same scene
input without steady cost. The eleven-pass/nine-resource graph still uses six
physical slots and 668,470,976 bytes committed through lifetime reuse. Detailed
validation, range, image, timing, and memory evidence is
`docs/milestones/M1.4-scene-linear-hdr-acceptance.md`.

M1.5 implements the SDR output portion. Bounded manual EV is applied only at the
single final boundary. The default production path is the pinned Academy ACES 2
100-nit Rec.709/sRGB transform, baked deterministically to a 128-cubed log-shaper
LUT with tetrahedral sampling. Legacy fitted and identity operators remain explicit
choices. Scene PFM and final-SDR TGA captures identify their domain and exact
transform independently. ImGui vertex colors are decoded from authored sRGB to
display-linear and composed after output; texture sampling retains Vulkan format
decode. Matched Nsight captures prove exact exposure-independent UI codes and one
output pass before UI. The complete evidence is
`docs/milestones/M1.5-final-sdr-acceptance.md`.

M1.6 completes the transport portion. Exact Vulkan negotiation selects mandatory
SDR, FP16 extended-linear scRGB, or 10-bit Rec.2100/PQ HDR10. The common pinned
Academy P3-D65 1000-nit transform feeds both HDR transports. scRGB applies the
Windows 80-nit scale; HDR10 uses a graph-owned FP16 Rec.2020 UI-composition image and
a sole final ST.2084 pass, so alpha blending never occurs in PQ code space. Optional
HDR metadata is applied only to HDR10 and reports unknown MaxCLL/MaxFALL as zero.
Scene captures are byte-identical across transports and final artifacts serialize
exact primaries, transfer, paper white, and peak. M1 was accepted on 2026-07-22; full
evidence is `docs/milestones/M1-acceptance-report-2026-07-22.md`.

M2.6a exposes the existing output contract in **Window > Project Settings**. Manual
EV, HDR UI/reference white, and peak luminance update live through backend-neutral
values; the Vulkan backend updates ImGui's display-linear scale and HDR10 metadata.
UI remains after scene output, receives no scene exposure, and is encoded only by the
transport's final encoding step. Transport selection still requires startup because
it changes the swapchain format/color space and graph topology. Evidence is
`docs/milestones/M2.6a-editor-output-and-transform-ux.md`.

M2 acceptance keeps transport selection at startup because changing SDR/scRGB/HDR10
changes swapchain format and dependent render-pass/pipeline state. Exclusive
fullscreen is not required; scRGB is the recommended Windows HDR editor transport.
Selection outlines are composited at the final output boundary after scene shading
and before color-managed UI, so selected materials remain scene-linear and
unchanged. The no-selection path performs no mask-neighborhood sampling.

Windows desktop HDR state does not silently override Iridium's requested transport.
SDR remains the safe default; scRGB is the preferred Windows desktop HDR transport
and HDR10 remains the explicit Rec.2100/PQ option. Neither requires exclusive
fullscreen. Because the swapchain format/color space and HDR10 graph topology differ,
transport changes require renderer restart; paper-white and peak values remain
visible in SDR but are clearly labeled inactive there.

## Rejected alternatives

- Continue manual pass sequencing: initially smaller, but fragile as temporal, async, RT, and SDK dependencies grow.
- Tone-map before transparency: prevents physically coherent composition and causes repeated or mismatched output transforms.
- Treat HDR as a swapchain-format toggle: omits color-space and luminance management.
