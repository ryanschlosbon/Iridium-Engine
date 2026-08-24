#version 450

// Ordinary2 deliberately reuses the production complex-forward material body.
// The define swaps only screen-space addressing, pair validation, and the
// Beer-Lambert path from authored thin-sheet distance to a measured peel chord.
#define IRIDIUM_INDEXED_MATERIAL_TEXTURES 1
#define IRIDIUM_LAYERED_ORDINARY2_COMPOSITION 1
#include "include/complex_material_body.glsl"
