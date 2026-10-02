#ifndef IRIDIUM_CLUSTERED_LIGHTING_GLSL
#define IRIDIUM_CLUSTERED_LIGHTING_GLSL

#include "include/lighting_records.glsl"

struct ClusterLightHeader {
    uint offset;
    uint count;
};

layout(std430, set = 0, binding = 0) readonly buffer LightRecords {
    PackedGpuLight lights[];
};
layout(std430, set = 0, binding = 1) readonly buffer ActiveLightSlots {
    uint activeLightSlots[];
};
layout(std430, set = 0, binding = 2) readonly buffer FallbackCandidates {
    uint fallbackCandidateSlots[];
};
layout(std140, set = 0, binding = 3) uniform ClusterParameters {
    mat4 clusterView;
    mat4 clusterProjection;
    uvec4 clusterGrid;   // width, height, tiles-x, tiles-y
    vec4 clusterDepth;   // near, far, slices/log(far/near), reserved
    uvec4 clusterLimits; // slices, per-cluster, references, directionals
    uvec4 clusterInput;  // active lights, fallback count, tile width, tile height
};
layout(std430, set = 0, binding = 4) buffer GlobalLightSlots {
    uint globalLightSlots[];
};
layout(std430, set = 0, binding = 5) buffer ClusterHeaders {
    ClusterLightHeader clusterHeaders[];
};
layout(std430, set = 0, binding = 6) buffer ClusterIndices {
    uint clusterLightSlots[];
};
layout(std430, set = 0, binding = 7) buffer FallbackLightSlots {
    uint fallbackLightSlots[];
};
layout(std430, set = 0, binding = 8) buffer ClusterDiagnostics {
    uint clusterDiagnostics[];
};
layout(std430, set = 0, binding = 9) buffer ClusterCounts {
    uint clusterCounts[];
};
layout(std430, set = 0, binding = 10) buffer ClusterCursors {
    uint clusterCursors[];
};
layout(std430, set = 0, binding = 11) buffer ClusterScanScratch {
    uint clusterScanScratch[];
};
layout(std430, set = 0, binding = 12) buffer ClusterIndirectArgs {
    uint clusterIndirectArgs[];
};

const uint IRIDIUM_CLUSTER_OVERFLOW_DIRECTIONAL = 1u;
const uint IRIDIUM_CLUSTER_OVERFLOW_PER_CLUSTER = 2u;
const uint IRIDIUM_CLUSTER_OVERFLOW_REFERENCES = 3u;
const uint IRIDIUM_INVALID_LIGHT_SLOT = 0xffffffffu;

const uint IRIDIUM_DIAGNOSTIC_ACTIVE = 0u;
const uint IRIDIUM_DIAGNOSTIC_DIRECTIONAL = 1u;
const uint IRIDIUM_DIAGNOSTIC_LOCAL = 2u;
const uint IRIDIUM_DIAGNOSTIC_OVERFLOW = 3u;
const uint IRIDIUM_DIAGNOSTIC_REQUESTED = 4u;
const uint IRIDIUM_DIAGNOSTIC_PUBLISHED = 5u;
const uint IRIDIUM_DIAGNOSTIC_CLUSTERS_USED = 6u;
const uint IRIDIUM_DIAGNOSTIC_MAX_OCCUPANCY = 7u;
const uint IRIDIUM_DIAGNOSTIC_FALLBACK = 8u;
const uint IRIDIUM_DIAGNOSTIC_DROPPED = 9u;

uint iridiumClusterCount() {
    return clusterGrid.z * clusterGrid.w * clusterLimits.x;
}

uint iridiumLightType(PackedGpuLight light) {
    return floatBitsToUint(light.shapeMetadata.z) & 3u;
}

uint iridiumDepthSlice(float positiveDepth) {
    float clampedDepth = clamp(positiveDepth, clusterDepth.x, clusterDepth.y);
    return min(clusterLimits.x - 1u,
        uint(log(clampedDepth / clusterDepth.x) * clusterDepth.z));
}

struct IridiumClusterBounds {
    uvec3 minimum;
    uvec3 maximum;
    bool valid;
};

uint iridiumTile(float ndc, uint pixels, uint tileCount, uint tileSize) {
    float pixel = clamp(ndc * 0.5 + 0.5, 0.0, 1.0) * float(pixels);
    return min(tileCount - 1u, uint(pixel) / tileSize);
}

IridiumClusterBounds iridiumLocalLightBounds(PackedGpuLight light) {
    IridiumClusterBounds result;
    result.minimum = uvec3(0u);
    result.maximum = uvec3(0u);
    result.valid = false;
    uint type = iridiumLightType(light);
    vec3 center = light.positionRange.xyz;
    float radius = light.positionRange.w;
    if (!(radius > 0.0) || isnan(radius) || isinf(radius)) return result;
    if (type == IRIDIUM_LIGHT_TYPE_SPOT) {
        float halfRange = radius * 0.5;
        float outerCos = clamp(light.directionOuterCos.w, 0.0, 1.0);
        if (outerCos <= 1.0e-4) {
            result.maximum = uvec3(clusterGrid.z - 1u, clusterGrid.w - 1u,
                clusterLimits.x - 1u);
            result.valid = true;
            return result;
        }
        float coneRadius = radius * sqrt(max(0.0, 1.0 - outerCos * outerCos)) /
            outerCos;
        center += light.directionOuterCos.xyz * halfRange;
        radius = length(vec2(halfRange, coneRadius)) +
            max(0.0, light.shapeMetadata.x);
    }
    vec3 viewCenter = (clusterView * vec4(center, 1.0)).xyz;
    float positiveDepth = -viewCenter.z;
    float minimumDepth = max(clusterDepth.x, positiveDepth - radius);
    float maximumDepth = min(clusterDepth.y, positiveDepth + radius);
    if (maximumDepth < clusterDepth.x || minimumDepth > clusterDepth.y ||
        minimumDepth > maximumDepth) return result;
    result.minimum.z = iridiumDepthSlice(minimumDepth);
    result.maximum.z = iridiumDepthSlice(maximumDepth);
    if (positiveDepth - radius <= clusterDepth.x) {
        result.maximum.xy = clusterGrid.zw - uvec2(1u);
        result.valid = true;
        return result;
    }
    float inverseDepth = 1.0 / positiveDepth;
    float centerX = clusterProjection[0][0] * viewCenter.x * inverseDepth;
    float centerY = clusterProjection[1][1] * viewCenter.y * inverseDepth;
    float conservativeDepth = max(clusterDepth.x, positiveDepth - radius);
    float radiusX = abs(clusterProjection[0][0]) * radius / conservativeDepth;
    float radiusY = abs(clusterProjection[1][1]) * radius / conservativeDepth;
    vec2 minimumNdc = vec2(centerX - radiusX, centerY - radiusY);
    vec2 maximumNdc = vec2(centerX + radiusX, centerY + radiusY);
    if (maximumNdc.x < -1.0 || minimumNdc.x > 1.0 ||
        maximumNdc.y < -1.0 || minimumNdc.y > 1.0) return result;
    result.minimum.x = iridiumTile(minimumNdc.x, clusterGrid.x,
        clusterGrid.z, clusterInput.z);
    result.maximum.x = iridiumTile(maximumNdc.x, clusterGrid.x,
        clusterGrid.z, clusterInput.z);
    result.minimum.y = iridiumTile(minimumNdc.y, clusterGrid.y,
        clusterGrid.w, clusterInput.w);
    result.maximum.y = iridiumTile(maximumNdc.y, clusterGrid.y,
        clusterGrid.w, clusterInput.w);
    result.valid = true;
    return result;
}

uint iridiumClusterIndex(uvec3 coordinate) {
    return (coordinate.z * clusterGrid.w + coordinate.y) * clusterGrid.z +
        coordinate.x;
}

void iridiumSaturatingIncrementCount(uint index) {
    uint oldValue = atomicAdd(clusterCounts[index], 0u);
    for (;;) {
        if (oldValue >= clusterLimits.y + 1u) {
            atomicMax(clusterDiagnostics[IRIDIUM_DIAGNOSTIC_OVERFLOW],
                IRIDIUM_CLUSTER_OVERFLOW_PER_CLUSTER);
            return;
        }
        uint newValue = min(oldValue + 1u, clusterLimits.y + 1u);
        uint observed = atomicCompSwap(clusterCounts[index], oldValue, newValue);
        if (observed == oldValue) {
            if (newValue > clusterLimits.y) {
                atomicMax(clusterDiagnostics[IRIDIUM_DIAGNOSTIC_OVERFLOW],
                    IRIDIUM_CLUSTER_OVERFLOW_PER_CLUSTER);
            }
            return;
        }
        oldValue = observed;
    }
}

bool iridiumReserveReferences(uint references) {
    uint maximum = clusterLimits.z;
    uint oldValue = atomicAdd(
        clusterDiagnostics[IRIDIUM_DIAGNOSTIC_REQUESTED], 0u);
    for (;;) {
        if (oldValue > maximum) return false;
        uint remaining = maximum + 1u - oldValue;
        uint newValue = references >= remaining
            ? maximum + 1u : oldValue + references;
        uint observed = atomicCompSwap(
            clusterDiagnostics[IRIDIUM_DIAGNOSTIC_REQUESTED],
            oldValue, newValue);
        if (observed == oldValue) {
            if (newValue > maximum) {
                atomicMax(clusterDiagnostics[IRIDIUM_DIAGNOSTIC_OVERFLOW],
                    IRIDIUM_CLUSTER_OVERFLOW_REFERENCES);
                return false;
            }
            return true;
        }
        oldValue = observed;
    }
}

#endif
