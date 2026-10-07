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

// The receiver as every cube tap sees it: the plane through the (normal-offset)
// receiver, its distance from the light, the bounded plane-correction allowance,
// and the constant receiver bias, all in world units.
struct IridiumPointShadowReceiverPlane {
    vec3 normal;
    float offset;
    float distance;
    float maximumCorrection;
    float bias;
    float depthA;
    float depthB;
    float resolution;
};

// Snaps a cube direction to the centre of the texel nearest sampling selects:
// the major-axis component becomes +-1 and both face coordinates move to the
// centre of their texel, where the caster's depth was rasterized.
vec3 iridiumPointShadowTexelCenter(vec3 direction, float resolution) {
    vec3 magnitude = abs(direction);
    float major = max(magnitude.x, max(magnitude.y, magnitude.z));
    vec3 face = direction / major;
    vec3 center = (min(floor((face * 0.5 + 0.5) * resolution),
        resolution - 1.0) + 0.5) * (2.0 / resolution) - 1.0;
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
        center.x = face.x;
    else if (magnitude.y >= magnitude.z)
        center.y = face.y;
    else
        center.z = face.z;
    return center;
}

// Stored-depth reference for one texel centre (major-axis component +-1): the
// receiver plane's major-axis distance along that texel's ray, clamped to the
// correction allowance around the receiver distance. A ray parallel to the
// plane or meeting it behind the light takes the far bound; a degenerate plane
// has no allowance and reduces to the receiver distance.
float iridiumPointShadowReference(IridiumPointShadowReceiverPlane receiver,
    vec3 texelDirection) {
    float directionLength = length(texelDirection);
    float sphereMajor = receiver.distance / directionLength;
    float correction = receiver.maximumCorrection / directionLength;
    float planeMajor = receiver.offset /
        dot(receiver.normal, texelDirection);
    if (!(planeMajor > 0.0) || isinf(planeMajor))
        planeMajor = sphereMajor + correction;
    float major = clamp(planeMajor, sphereMajor - correction,
        sphereMajor + correction);
    return receiver.depthA -
        receiver.depthB / max(major - receiver.bias, 0.00001);
}

float iridiumPointShadowHardFilter(IridiumPointShadowReceiverPlane receiver,
    uint tier, uint cubeIndex, vec3 direction, vec3 tangent, vec3 bitangent,
    float angularTexel, uint sampleCount) {
    // A fixed low-discrepancy disk is temporally stable. Per-pixel rotation is
    // useful for broad PCSS penumbrae, but makes a sub-texel hard edge sparkle.
    float rotation = 0.0;
    float visibility = 0.0;
    uint samples = clamp(sampleCount, 16u, 64u);
    for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
        if (sampleIndex >= samples) break;
        vec2 disk = iridiumShadowDiskSample(sampleIndex, samples, rotation) *
            1.5 * angularTexel;
        vec3 texelDirection = iridiumPointShadowTexelCenter(direction +
            tangent * disk.x + bitangent * disk.y, receiver.resolution);
        visibility += iridiumShadowCompare(
            iridiumPointShadowReference(receiver, texelDirection),
            iridiumPointShadowDepth(tier, texelDirection, cubeIndex));
    }
    return visibility / float(samples);
}

float iridiumPointShadowVisibility(uint lightSlot, PackedGpuLight lightRecord,
    IridiumShadowReceiver receiver, vec3 surfaceToLight) {
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
    float resolution = entry.metadata.z == 0u ? 256.0 :
        entry.metadata.z == 1u ? 512.0 : 1024.0;
    float angularTexel = 2.0 / resolution;
    // The geometric-normal offset is sized by the receiver's own texel
    // footprint (ADR-0010 decision 4; the directional contract).
    float normalOffsetScale = iridiumShadowNormalOffsetScale(
        receiver.geometricNormal, surfaceToLight, entry.depthBias.z);
    vec3 offsetWorldPosition = receiver.worldPosition +
        receiver.geometricNormal * (length(receiver.worldPosition -
            entry.lightPositionFar.xyz) * angularTexel * normalOffsetScale);
    vec3 lightToReceiver = offsetWorldPosition - entry.lightPositionFar.xyz;
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
    float noL = clamp(dot(receiver.shadingNormal, surfaceToLight), 0.0, 1.0);
    float worldUnitsPerTexel = receiverDistance * angularTexel;
    // Every tap compares against the receiver plane along its own texel's ray,
    // bounded to the configured world-texel allowance; derivatives that do not
    // span a plane give no correction.
    vec3 planeNormal = cross(receiver.worldPositionDx,
        receiver.worldPositionDy);
    float planeLengthSquared = dot(planeNormal, planeNormal);
    bool planeValid = planeLengthSquared > 1.0e-24 &&
        !isinf(planeLengthSquared) && !isnan(planeLengthSquared);
    IridiumPointShadowReceiverPlane plane;
    plane.normal = planeValid
        ? planeNormal * inversesqrt(planeLengthSquared) : vec3(0.0);
    plane.offset = dot(plane.normal, lightToReceiver);
    plane.distance = receiverDistance;
    plane.maximumCorrection = planeValid
        ? entry.filterParameters.z * worldUnitsPerTexel : 0.0;
    plane.bias = worldUnitsPerTexel * entry.depthBias.w *
        mix(1.0, 2.0, 1.0 - noL);
    plane.depthA = entry.depthBias.x;
    plane.depthB = entry.depthBias.y;
    plane.resolution = resolution;
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
            vec3 texelDirection = iridiumPointShadowTexelCenter(direction +
                tangent * disk.x + bitangent * disk.y, resolution);
            float storedDepth = iridiumPointShadowDepth(entry.metadata.z,
                texelDirection, entry.metadata.w);
            if (storedDepth < iridiumPointShadowReference(plane,
                    texelDirection)) {
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
            return iridiumPointShadowHardFilter(plane, entry.metadata.z,
                entry.metadata.w, direction, tangent, bitangent,
                angularTexel, entry.filterMetadata.y);
        float visibility = 0.0;
        uint filterSamples = max(entry.filterMetadata.y, 1u);
        for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
            if (sampleIndex >= filterSamples) break;
            vec2 disk = iridiumShadowDiskSample(sampleIndex,
                filterSamples, rotation) * penumbraRadius * angularTexel;
            vec3 texelDirection = iridiumPointShadowTexelCenter(direction +
                tangent * disk.x + bitangent * disk.y, resolution);
            float storedDepth = iridiumPointShadowDepth(entry.metadata.z,
                texelDirection, entry.metadata.w);
            visibility += iridiumShadowCompare(
                iridiumPointShadowReference(plane, texelDirection),
                storedDepth);
        }
        return visibility / float(filterSamples);
    }

    return iridiumPointShadowHardFilter(plane, entry.metadata.z,
        entry.metadata.w, direction, tangent, bitangent, angularTexel,
        entry.filterMetadata.y);
}

#endif
