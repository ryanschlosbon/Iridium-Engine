#version 450

#define IRIDIUM_INDEXED_MATERIAL_TEXTURES 1
// M9.8e: sorted and compatibility transparency write motion-aware reactive
// coverage (dual-source alpha) for the TAA reactive mask.
#define IRIDIUM_TRANSPARENT_REACTIVE 1
#include "include/complex_material_body.glsl"
