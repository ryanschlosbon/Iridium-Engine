#ifndef IRIDIUM_CLUSTERED_LIGHT_ACCESS_GLSL
#define IRIDIUM_CLUSTERED_LIGHT_ACCESS_GLSL

#ifndef IRIDIUM_LIGHTING_SET
#error IRIDIUM_LIGHTING_SET must name the shared scene descriptor set
#endif

#include "include/direct_lighting.glsl"

struct IridiumClusterLightHeader {
    uint offset;
    uint count;
};

layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 9) readonly buffer
    IridiumLightRecordBuffer {
    PackedGpuLight iridiumLights[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 10) readonly buffer
    IridiumGlobalLightBuffer {
    uint iridiumGlobalLightSlots[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 11) readonly buffer
    IridiumClusterHeaderBuffer {
    IridiumClusterLightHeader iridiumClusterHeaders[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 12) readonly buffer
    IridiumClusterIndexBuffer {
    uint iridiumClusterLightSlots[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 13) readonly buffer
    IridiumFallbackLightBuffer {
    uint iridiumFallbackLightSlots[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 14) readonly buffer
    IridiumClusterDiagnosticBuffer {
    uint iridiumClusterDiagnostics[];
};
layout(std140, set = IRIDIUM_LIGHTING_SET, binding = 15) uniform
    IridiumClusterParameterBuffer {
    mat4 iridiumClusterView;
    mat4 iridiumClusterProjection;
    uvec4 iridiumClusterGrid;
    vec4 iridiumClusterDepth;
    uvec4 iridiumClusterLimits;
    uvec4 iridiumClusterInput;
    vec4 iridiumEnvironmentSettings;
};

const uint IRIDIUM_CLUSTER_DIAGNOSTIC_DIRECTIONAL = 1u;
const uint IRIDIUM_CLUSTER_DIAGNOSTIC_OVERFLOW = 3u;
const uint IRIDIUM_CLUSTER_DIAGNOSTIC_FALLBACK = 8u;

struct IridiumDirectLightRange {
    uint cluster;
    uint globalCount;
    uint localCount;
    uint fallbackCount;
    bool fallback;
};

uint iridiumShadingClusterIndex(vec3 worldPosition, uvec2 pixel) {
    float positiveDepth = clamp(
        -(iridiumClusterView * vec4(worldPosition, 1.0)).z,
        iridiumClusterDepth.x, iridiumClusterDepth.y);
    uint slice = min(iridiumClusterLimits.x - 1u,
        uint(log(positiveDepth / iridiumClusterDepth.x) *
            iridiumClusterDepth.z));
    uvec2 tile = min(pixel / iridiumClusterInput.zw,
        iridiumClusterGrid.zw - uvec2(1u));
    return (slice * iridiumClusterGrid.w + tile.y) *
        iridiumClusterGrid.z + tile.x;
}

IridiumDirectLightRange iridiumDirectLightRange(
    vec3 worldPosition, uvec2 pixel) {
    IridiumDirectLightRange range;
    range.cluster = iridiumShadingClusterIndex(worldPosition, pixel);
    range.fallback = iridiumClusterDiagnostics[
        IRIDIUM_CLUSTER_DIAGNOSTIC_OVERFLOW] != 0u;
    range.globalCount = range.fallback ? 0u : min(
        iridiumClusterDiagnostics[IRIDIUM_CLUSTER_DIAGNOSTIC_DIRECTIONAL],
        iridiumClusterLimits.w);
    range.localCount = range.fallback ? 0u :
        iridiumClusterHeaders[range.cluster].count;
    range.fallbackCount = range.fallback ? min(
        iridiumClusterDiagnostics[IRIDIUM_CLUSTER_DIAGNOSTIC_FALLBACK],
        iridiumClusterInput.y) : 0u;
    return range;
}

uint iridiumDirectLightCount(IridiumDirectLightRange range) {
    return range.fallback ? range.fallbackCount :
        range.globalCount + range.localCount;
}

uint iridiumDirectLightSlot(IridiumDirectLightRange range, uint index) {
    if (range.fallback) return iridiumFallbackLightSlots[index];
    if (index < range.globalCount) return iridiumGlobalLightSlots[index];
    IridiumClusterLightHeader header = iridiumClusterHeaders[range.cluster];
    return iridiumClusterLightSlots[
        header.offset + index - range.globalCount];
}

IridiumDirectLightSample iridiumEvaluateDirectLightSlot(uint slot,
    vec3 worldPosition, vec3 surfaceNormal) {
    return iridiumEvaluateDirectLight(iridiumLights[slot], worldPosition,
        surfaceNormal);
}

#endif
