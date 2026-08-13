# Lighting authoring workflows

## Light gizmos

Select a light entity to display its authored shape in the Scene Viewport. The
transform's local +Z axis is the light direction; transform scale does not alter
the photometric light.

- Directional lights show a disk and direction arrow.
- Point lights show their range as a wire sphere and their physical source radius
  as a smaller ring.
- Spot lights show the outer cone in amber, the inner cone in pale yellow, their
  full range, and a direction arrow. The inner cone receives full intensity; the
  falloff transition ends at the outer cone.

The gizmos are editor overlays and do not add geometry or rendering cost to the
runtime scene.

## Reflection probes

Reflection probes provide local, filtered environment reflections. Their cyan
selection gizmo is the influence boundary. The paler inner boundary marks the
start of the blend region.

1. Put the probe near the visual center of the region it represents.
2. Use a box for rooms and corridors, or a sphere for an open/local region.
3. Size the influence so it overlaps neighboring probes slightly. Use Blend
   distance to make the handoff gradual.
4. For authored lighting, drag an HDRI Environment asset into the Environment
   slot. To capture the scene, leave that slot empty, choose On demand, and click
   Capture Now after static geometry and lighting are ready.
5. Use Box projection with a box probe to keep reflections attached to interior
   walls. Priority is a tie-breaker, not a substitute for good bounds.
6. Choose 512 for general work, 1024 for important rooms, and 2048 only for hero
   regions after checking capture time and GPU memory. Realtime capture is the
   most expensive update mode.

An assigned environment intentionally disables scene capture. Clear the asset to
enable Capture Now or Bake Capture. Baked mode writes a reusable environment;
re-bake when nearby static geometry or lighting changes.

## Baked lighting sets

A Baked Lighting Set is the scene-level owner for one validated
`iridium.baked-lighting` product. The product can contain surface lightmaps,
probe-volume irradiance, and baked visibility, which can be toggled independently.

Drag a cooked Baked Lighting asset into the component, leave one authoritative
set enabled for a region, and use Diffuse intensity and Specular intensity only
for deliberate artistic balancing. Missing or invalid products contribute neutral
lighting and retain their authored asset identity for repair.

M5 establishes the component, asset, validation, and publication contract. The
full GI bake solver and final runtime GI consumers are later roadmap work, so
adding this component by itself does not currently calculate indirect lighting.

## Troubleshooting

- A probe has no effect: ensure it is enabled, the shaded point is inside its
  influence, intensity is nonzero, and the assigned/captured environment reports
  no publication diagnostic.
- Reflections slide across room walls: use a box probe and Box projection, then
  align its oriented bounds to the room.
- A scene capture button is disabled: clear the assigned Environment asset first.
- A baked set has no visible result: confirm the product contains the enabled
  payload kind; until the later GI runtime milestone, the M5 contract alone is
  intentionally neutral.
