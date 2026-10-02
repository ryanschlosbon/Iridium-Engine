#ifndef IRIDIUM_REFLECTION_PROBE_RECORDS_GLSL
#define IRIDIUM_REFLECTION_PROBE_RECORDS_GLSL

// std430 ABI mirror of Iridium::PackedGpuReflectionProbe. The stride is 112
// bytes, with fields beginning at byte offsets 0, 64, 80, and 96.
struct PackedGpuReflectionProbe {
    mat4 worldToProbe;
    vec4 influence;
    vec4 positionIntensity;
    uvec4 metadata;
};

const uint IRIDIUM_PROBE_BOX_SHAPE_BIT = 1u << 0u;
const uint IRIDIUM_PROBE_BOX_PROJECTION_BIT = 1u << 1u;
const uint IRIDIUM_INVALID_ENVIRONMENT_TABLE_SLOT = 0xffffffffu;

#endif
