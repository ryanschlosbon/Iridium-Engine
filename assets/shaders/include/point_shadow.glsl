#ifndef IRIDIUM_POINT_SHADOW_GLSL
#define IRIDIUM_POINT_SHADOW_GLSL

#include "include/shadow_filter.glsl"

#ifndef IRIDIUM_LIGHTING_SET
#error IRIDIUM_LIGHTING_SET must name the shared scene descriptor set
#endif

struct IridiumPointShadowEntry {
    mat4 worldToShadowClip[6];
    vec4 lightPositionFar;
    uvec4 metadata;
    vec4 depthBias;
    vec4 filterParameters;
    uvec4 filterMetadata;
};

layout(set = IRIDIUM_LIGHTING_SET, binding = 24)
    uniform samplerCubeArray iridiumPointShadow256;
layout(set = IRIDIUM_LIGHTING_SET, binding = 25)
    uniform samplerCubeArray iridiumPointShadow512;
layout(set = IRIDIUM_LIGHTING_SET, binding = 26)
    uniform samplerCubeArray iridiumPointShadow1024;
layout(std140, set = IRIDIUM_LIGHTING_SET, binding = 27) uniform
    IridiumPointShadowData {
    IridiumPointShadowEntry iridiumPointShadowEntries[56];
    uvec4 iridiumPointShadowMetadata;
};

float iridiumPointShadowDepth(uint tier, vec3 direction, uint cubeIndex) {
    vec4 coordinate = vec4(direction, float(cubeIndex));
    float storedDepth = 1.0;
    if (tier == 0u)
        storedDepth = texture(iridiumPointShadow256, coordinate).r;
    else if (tier == 1u)
        storedDepth = texture(iridiumPointShadow512, coordinate).r;
    else
        storedDepth = texture(iridiumPointShadow1024, coordinate).r;
    return storedDepth;
}

float iridiumPointShadowHardFilter(uint tier, uint cubeIndex,
    vec3 direction, vec3 tangent, vec3 bitangent, float angularTexel,
    float referenceDepth, uint sampleCount) {
    // A fixed low-discrepancy disk is temporally stable. Per-pixel rotation is
    // useful for broad PCSS penumbrae, but makes a sub-texel hard edge sparkle.
    float rotation = 0.0;
    float visibility = 0.0;
    uint samples = clamp(sampleCount, 16u, 64u);
    for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
        if (sampleIndex >= samples) break;
        vec2 disk = iridiumShadowDiskSample(sampleIndex, samples, rotation) *
            1.5 * angularTexel;
        vec3 sampleDirection = normalize(direction + tangent * disk.x +
            bitangent * disk.y);
        visibility += iridiumShadowCompare(referenceDepth,
            iridiumPointShadowDepth(tier, sampleDirection, cubeIndex));
    }
    return visibility / float(samples);
}

float iridiumPointShadowVisibility(uint lightSlot, PackedGpuLight lightRecord,
    vec3 worldPosition, vec3 surfaceNormal, vec3 surfaceToLight) {
    if ((floatBitsToUint(lightRecord.shapeMetadata.z) &
        IRIDIUM_LIGHT_CASTS_SHADOWS_BIT) == 0u)
        return 1.0;
    uint slot = floatBitsToUint(lightRecord.shapeMetadata.w);
    if (slot == IRIDIUM_INVALID_SHADOW_DATA_SLOT || slot >= 56u)
        return 1.0;
    IridiumPointShadowEntry entry = iridiumPointShadowEntries[slot];
    if (entry.metadata.y == 0u || entry.metadata.x != lightSlot ||
        entry.metadata.z >= 3u)
        return 1.0;
    vec3 lightToReceiver = worldPosition - entry.lightPositionFar.xyz;
    float receiverDistance = length(lightToReceiver);
    if (receiverDistance <= 0.00001 ||
        receiverDistance >= entry.lightPositionFar.w)
        return 1.0;
    vec3 direction = lightToReceiver / receiverDistance;
    float majorDistance = max(abs(lightToReceiver.x),
        max(abs(lightToReceiver.y), abs(lightToReceiver.z)));
    vec3 upReference = abs(direction.y) < 0.99
        ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 tangent = normalize(cross(direction, upReference));
    vec3 bitangent = cross(tangent, direction);
    float resolution = entry.metadata.z == 0u ? 256.0 :
        entry.metadata.z == 1u ? 512.0 : 1024.0;
    float angularTexel = 2.0 / resolution;
    float noL = clamp(dot(surfaceNormal, surfaceToLight), 0.0, 1.0);
    float worldUnitsPerTexel = receiverDistance * angularTexel;
    float receiverWorldBias = worldUnitsPerTexel * entry.depthBias.w *
        mix(1.0, 2.0, 1.0 - noL);
    float biasedMajorDistance = max(majorDistance - receiverWorldBias,
        0.00001);
    float receiverBias = entry.depthBias.y / biasedMajorDistance -
        entry.depthBias.y / max(majorDistance, 0.00001);
    float referenceDepth = entry.depthBias.x -
        entry.depthBias.y / max(majorDistance, 0.00001) -
        max(receiverBias, 0.0);
    if (entry.filterMetadata.z != 0u && entry.filterMetadata.x != 0u &&
        entry.filterParameters.x > 0.0) {
        float texelsPerRadian = resolution * 0.5;
        float maximumRadius = entry.filterParameters.y;
        float searchRadius = clamp(entry.filterParameters.x /
            receiverDistance * texelsPerRadian, 0.0, maximumRadius);
        float rotation = iridiumShadowRotation(gl_FragCoord.xy);
        float blockerDistance = 0.0;
        uint blockerCount = 0u;
        for (uint sampleIndex = 0u; sampleIndex < 32u; ++sampleIndex) {
            if (sampleIndex >= entry.filterMetadata.x) break;
            vec2 disk = iridiumShadowDiskSample(sampleIndex,
                entry.filterMetadata.x, rotation) * searchRadius *
                angularTexel;
            vec3 sampleDirection = normalize(direction + tangent * disk.x +
                bitangent * disk.y);
            float storedDepth = iridiumPointShadowDepth(entry.metadata.z,
                sampleDirection, entry.metadata.w);
            if (storedDepth < referenceDepth) {
                float blockerMajor = entry.depthBias.y /
                    max(entry.depthBias.x - storedDepth, 0.000001);
                blockerDistance += blockerMajor * receiverDistance /
                    max(majorDistance, 0.000001);
                ++blockerCount;
            }
        }
        if (blockerCount == 0u) return 1.0;
        blockerDistance /= float(blockerCount);
        float penumbraAngle = entry.filterParameters.x *
            max(receiverDistance - blockerDistance, 0.0) /
            max(blockerDistance * receiverDistance, 0.000001);
        float penumbraRadius = clamp(penumbraAngle * texelsPerRadian,
            0.0, maximumRadius);
        if (penumbraRadius <= 1.0)
            return iridiumPointShadowHardFilter(entry.metadata.z,
                entry.metadata.w, direction, tangent, bitangent,
                angularTexel, referenceDepth, entry.filterMetadata.y);
        float visibility = 0.0;
        uint filterSamples = max(entry.filterMetadata.y, 1u);
        for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
            if (sampleIndex >= filterSamples) break;
            vec2 disk = iridiumShadowDiskSample(sampleIndex,
                filterSamples, rotation) * penumbraRadius * angularTexel;
            vec3 sampleDirection = normalize(direction + tangent * disk.x +
                bitangent * disk.y);
            float storedDepth = iridiumPointShadowDepth(entry.metadata.z,
                sampleDirection, entry.metadata.w);
            visibility += iridiumShadowCompare(referenceDepth, storedDepth);
        }
        return visibility / float(filterSamples);
    }

    return iridiumPointShadowHardFilter(entry.metadata.z, entry.metadata.w,
        direction, tangent, bitangent, angularTexel, referenceDepth,
        entry.filterMetadata.y);
}

#endif
