#version 450

#define IRIDIUM_INDEXED_MATERIAL_TEXTURES 1
#define IRIDIUM_OPAQUE_FORWARD 1
// M9.8e: SortedSurface materials draw with this program in the transparent
// pass; their reactive coverage is motion-aware. Forward-opaque's velocity
// variant writes opaque alpha instead.
#if !defined(IRIDIUM_WRITE_VELOCITY)
#define IRIDIUM_TRANSPARENT_REACTIVE 1
#endif
#include "include/complex_material_body.glsl"
