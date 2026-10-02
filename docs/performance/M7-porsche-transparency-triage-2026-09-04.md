# Porsche transparency correction and remaining fidelity cases

Status: partial correction, not visual acceptance. M7.6 remains active.

## Owner update (2026-09-09)

The owner reports fixing the 1975 911/930 black headlights in Blender and with the
engine material panel. Treat the earlier black-lens issue as owner-resolved, not
an outstanding engine correction. Bulbs behind that headlight are still missing;
their cause is unconfirmed. Inspect current bulb geometry/material visibility,
resolved lens class and mixed-class composition before attributing it to layer
capacity. The separate Carrera cover/bulb ordering and side-angle reflection cases
remain unverified. Do not overwrite the owner's corrected source/material edits.

Project Settings now exposes an explicit session-only Layered Glass interface
quality override (authored, 2, 4, 8). It is an execution diagnostic, not a universal
transparent-object count and not a demonstrated fix for missing bulbs. It cannot
change Thin Glass/Sorted Surface ordering or manufacture valid closed topology.

## Source material corrections

The locally installed, gitignored model sources were edited, not their original
download copies. Both material-12 transmission closures omit metallicFactor;
SourceMaterial correctly defaults it to 1. The complex forward shader weights
transmission by `transmission * (1 - metallic)`. Raising glass quality cannot
repair fully metallic authoring.

| Source | Material | Change |
| --- | --- | --- |
| `assets/1975_porsche_911_930t/1975_porsche_911_930t.gltf` | `930_lights_refraction`, index 12 | Explicit metallicFactor 0; baseColorFactor RGB black to white, alpha remains 0.25 |
| `assets/models/porsche_911_carrera4s/porsche_911_carrera4s.gltf` | `lights`, index 12 | Explicit metallicFactor 0; white base color and alpha 0.25 unchanged |

These are neutral dielectric lens corrections. Roughness, normal textures,
transmission factor 1, doubleSided, identities, and all per-primitive transparency
policies were preserved. Other metallic/roughness-textured lamp materials were
not blanket-converted. For future authoring, set a glass lens's metallic factor
to zero in the source material, use a neutral/light transmission tint, export,
then reimport/reload the asset. Already loaded runtime material instances do not
change merely because the source JSON changed. The edits are local assets, so
this table also records how to reproduce them after replacing downloaded assets.

MaterialCompiler's existing metallic-suppression warning now explains the glTF
default and the corrective setting. A new regression verifies the omitted factor
stays 1, emits that warning, and an explicitly authored zero removes the warning
and changes the material hash. Do not silently rewrite importer defaults for all
transmissive materials.

## Remaining renderer work (do not close on these material edits)

VulkanVertexBackend records `transparent.sorted.forward` before thin-glass
`transparent.compatibility.forward`. The refraction pyramid is captured before
those surfaces. A transmission-1 thin-glass fragment replaces its destination
with a sample derived from that earlier pyramid. An inner lens can consequently
erase an already drawn outer cover's reflection. Sorting within each queue does
not establish cross-class per-pixel ordering. Higher Layer priority selects
bounded layered-atlas admission; it is not an author-controlled front/back order.

Carrera's `glass` material (11) and `lights` (12) are separate closures. The
inside-out reflection seen at grazing angles remains unverified: double-sided
triangle/back-interface ordering is a candidate, not a confirmed normal-import
defect. No speculative normal inversion or global backface discard was applied.

Next fidelity slice must reproduce both lamps close-up with a structured HDR
environment, front/side sweeps and a bright reflected source. Establish a shared
depth-aware composition contract for overlapping sorted surfaces and thin glass,
including scene color availability and bounded fallbacks; do not merely reverse
the two passes globally. Add nested/intersecting mixed-class regression fixtures,
and verify two-sided geometry separately before changing culling policy. Retain
ADR-0012's topology and transport requirements; supersede it if the chosen
composition contract changes accepted architecture.

## Verification

- Both corrected assets cook successfully with Release importer 8 into
  `out/benchmarks/m7-transparency-porsche/ddc`.
- Carrera cook key: `62cf686cf92cfecb41c2097d4d322f64ec6511381c613b22c50715fa453882f5`.
- 930 cook key: `894d9de0f8adcc25cb6ac25121d1c0ead88119e14ac657ace757d0e20ee6f819`.
- Cooked Carrera material inspection confirms metallic 0 and thin-glass routing.
- Debug/Release engine builds pass. MaterialCompilerTests and DepthPyramidTests
  pass in both configurations (2/2 each).
- Additive `assets/m7-porsche-transparency-manifest.v1.json` launches the Carrera
  cooked product, 30 warmup + 5 measured frames at 1280x720, validation enabled.
  Scene-linear and final-SDR captures exit 0 without reported validation errors.
  Final SDR image was inspected after lossless TGA-to-PNG conversion. It is a
  broad oblique, constant-environment smoke check, not the owner's close-up HDR
  reference; it cannot establish repair of the grazing reflection or cover order.
- No matched before/after GPU, VRAM or image-quality admission was performed.
  No renderer shader, ABI, render graph, or runtime Hi-Z behavior changed here.

No ADR status changed. M7.6 remains open; the material/import regression is an
additional fidelity task, not evidence that GPU Hi-Z is implemented.
