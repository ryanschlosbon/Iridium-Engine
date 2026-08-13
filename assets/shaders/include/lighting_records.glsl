#ifndef IRIDIUM_LIGHTING_RECORDS_GLSL
#define IRIDIUM_LIGHTING_RECORDS_GLSL

// std430 ABI mirror of Iridium::PackedGpuLight. Four vec4 fields freeze the
// offsets at 0, 16, 32, and 48 bytes and the stride at 64 bytes.
struct PackedGpuLight {
    vec4 positionRange;
    // World-space local +Z emission direction and spot outer-cone cosine.
    vec4 directionOuterCos;
    vec4 colorIntensity;
    vec4 shapeMetadata;
};

const uint IRIDIUM_LIGHT_TYPE_DIRECTIONAL = 0u;
const uint IRIDIUM_LIGHT_TYPE_POINT = 1u;
const uint IRIDIUM_LIGHT_TYPE_SPOT = 2u;
const uint IRIDIUM_LIGHT_CASTS_SHADOWS_BIT = 1u << 2u;
const uint IRIDIUM_INVALID_SHADOW_DATA_SLOT = 0xffffffffu;

#endif
