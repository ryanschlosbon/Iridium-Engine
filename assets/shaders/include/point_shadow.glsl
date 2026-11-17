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

// The 2x2 texels a linear filter at this direction would read, in the gather
// order (i0,j1), (i1,j1), (i1,j0), (i0,j0). Gather ignores the nearest
// filter of the shared sampler, and cube edges and corners are seamless.
vec4 iridiumPointShadowGather(uint tier, vec3 direction, uint cubeIndex) {
    vec4 coordinate = vec4(direction, float(cubeIndex));
    vec4 storedDepth = vec4(1.0);
    if (tier == 0u)
        storedDepth = textureGather(iridiumPointShadow256, coordinate);
    else if (tier == 1u)
        storedDepth = textureGather(iridiumPointShadow512, coordinate);
    else
        storedDepth = textureGather(iridiumPointShadow1024, coordinate);
    return storedDepth;
}

// The receiver plane in stored projective depth. Over a planar receiver
// 1 / major-axis distance is linear in a direction scaled to unit major-axis
// component (1 / major = dot(n, c) / dot(n, receiver - light)), so the plane's
// stored depth along any direction is dot(depthPerUnitMajor, unitMajor(c))
// up to the constant A, exactly, on whichever cube face c selects. Each
// comparison is against the receiver's biased reference corrected by the
// plane's depth there relative to the receiver's own (centerDepth), bounded
// to the configured world-texel allowance.
struct IridiumPointShadowReceiverPlane {
    vec3 depthPerUnitMajor;
    float centerDepth;
    float reference;
    float maximumCorrection;
};

vec3 iridiumPointShadowUnitMajor(vec3 direction) {
    vec3 magnitude = abs(direction);
    return direction /
        max(magnitude.x, max(magnitude.y, magnitude.z));
}

float iridiumPointShadowPlaneReference(
    IridiumPointShadowReceiverPlane receiver, float planeDepth) {
    return receiver.reference + clamp(planeDepth - receiver.centerDepth,
        -receiver.maximumCorrection, receiver.maximumCorrection);
}

float iridiumPointShadowReference(IridiumPointShadowReceiverPlane receiver,
    vec3 sampleDirection) {
    return iridiumPointShadowPlaneReference(receiver, dot(
        receiver.depthPerUnitMajor,
        iridiumPointShadowUnitMajor(sampleDirection)));
}

// The cube face a direction selects (the Vulkan cube face table:
// sc = dot(sAxis, r), tc = dot(tAxis, r), ma = dot(majorAxis, r), face
// coordinates sc / ma and tc / ma in [-1, 1]) and the receiver plane's stored
// depth step per texel along its s and t axes: the plane's depth is affine in
// face coordinates (dot(depthPerUnitMajor, r / ma)).
struct IridiumPointShadowCubeFace {
    vec3 majorAxis;
    vec3 sAxis;
    vec3 tAxis;
    vec2 texelGradient;
};

IridiumPointShadowCubeFace iridiumPointShadowCubeFace(vec3 direction,
    vec3 depthPerUnitMajor, float angularTexel) {
    vec3 magnitude = abs(direction);
    IridiumPointShadowCubeFace face;
    if (magnitude.z >= magnitude.x && magnitude.z >= magnitude.y) {
        float side = direction.z < 0.0 ? -1.0 : 1.0;
        face.majorAxis = vec3(0.0, 0.0, side);
        face.sAxis = vec3(side, 0.0, 0.0);
        face.tAxis = vec3(0.0, -1.0, 0.0);
    } else if (magnitude.y >= magnitude.x) {
        float side = direction.y < 0.0 ? -1.0 : 1.0;
        face.majorAxis = vec3(0.0, side, 0.0);
        face.sAxis = vec3(1.0, 0.0, 0.0);
        face.tAxis = vec3(0.0, 0.0, side);
    } else {
        float side = direction.x < 0.0 ? -1.0 : 1.0;
        face.majorAxis = vec3(side, 0.0, 0.0);
        face.sAxis = vec3(0.0, 0.0, -side);
        face.tAxis = vec3(0.0, -1.0, 0.0);
    }
    face.texelGradient = vec2(dot(depthPerUnitMajor, face.sAxis),
        dot(depthPerUnitMajor, face.tAxis)) * angularTexel;
    return face;
}

// Bilinear reconstruction of four depth comparisons (the directional and spot
// reconstruction, for cube maps) on the cube face the tap direction selects
// (selected per tap: hoisting the receiver's face out of the filter loops
// measured slower). Each texel is compared against
// the receiver plane at its own centre: the plane is evaluated at the tap and
// stepped by the per-texel face gradient. The gather is taken at the
// footprint's shared corner, which selects the same face and 2x2 texels as the
// weights without depending on the hardware's sub-texel rounding; a footprint
// straddling a face edge is gathered a quarter texel inside the face, and the
// hardware's seamless filtering supplies the texels across the edge (at a cube
// corner, the implementation's corner texel). Across an edge the plane is
// extrapolated in this face's projection, which only errs towards lit by up
// to half a texel of depth (the neighbouring face's major distance is larger).
float iridiumPointShadowBilinearCompare(
    IridiumPointShadowReceiverPlane receiver, uint tier, uint cubeIndex,
    vec3 sampleDirection, float resolution, float angularTexel) {
    IridiumPointShadowCubeFace face = iridiumPointShadowCubeFace(
        sampleDirection, receiver.depthPerUnitMajor, angularTexel);
    vec2 coordinate = vec2(dot(face.sAxis, sampleDirection),
        dot(face.tAxis, sampleDirection));
    float inverseMajor = 1.0 / dot(face.majorAxis, sampleDirection);
    vec2 pixel = (coordinate * inverseMajor * 0.5 + 0.5) * resolution -
        vec2(0.5);
    vec2 base = floor(pixel);
    vec2 blend = pixel - base;
    float cornerLimit = 1.0 - 0.25 * angularTexel;
    vec2 corner = clamp((base + vec2(1.0)) * angularTexel - vec2(1.0),
        vec2(-cornerLimit), vec2(cornerLimit));
    // Gather order: (i0,j1), (i1,j1), (i1,j0), (i0,j0).
    vec4 storedDepth = iridiumPointShadowGather(tier, face.majorAxis +
        face.sAxis * corner.x + face.tAxis * corner.y, cubeIndex);
    float planeDepth00 = dot(receiver.depthPerUnitMajor, sampleDirection) *
        inverseMajor - dot(face.texelGradient, blend);
    vec4 references = receiver.reference + clamp(planeDepth00 +
        vec4(0.0, face.texelGradient.x, face.texelGradient.y,
            face.texelGradient.x + face.texelGradient.y) -
        receiver.centerDepth, -receiver.maximumCorrection,
        receiver.maximumCorrection);
    // iridiumShadowCompare per texel: (0,0), (1,0), (0,1), (1,1).
    vec4 visible = vec4(lessThanEqual(references, storedDepth.wzxy));
    return mix(mix(visible.x, visible.y, blend.x),
        mix(visible.z, visible.w, blend.x), blend.y);
}

// The directional and spot hard filter: a fixed 3x3 tent of bilinear
// comparisons one texel apart, here in the receiver's tangent frame. It is
// temporally stable (no per-pixel rotation, which makes a sub-texel hard edge
// sparkle) and matches the width of the former 1.5-texel nearest-texel disk.
float iridiumPointShadowHardFilter(IridiumPointShadowReceiverPlane receiver,
    uint tier, uint cubeIndex, vec3 direction, vec3 tangent, vec3 bitangent,
    float resolution, float angularTexel) {
    const float tentWeights[3] = float[3](1.0, 2.0, 1.0);
    vec3 tangentStep = tangent * angularTexel;
    vec3 bitangentStep = bitangent * angularTexel;
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            // Cube sampling needs no normalized direction.
            visibility += tentWeights[x + 1] * tentWeights[y + 1] *
                iridiumPointShadowBilinearCompare(receiver, tier,
                    cubeIndex, direction + tangentStep * float(x) +
                        bitangentStep * float(y), resolution, angularTexel);
    return visibility / 16.0;
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
    float receiverWorldBias = worldUnitsPerTexel * entry.depthBias.w *
        mix(1.0, 2.0, 1.0 - noL);
    IridiumPointShadowReceiverPlane plane;
    plane.reference = entry.depthBias.x - entry.depthBias.y /
        max(majorDistance - receiverWorldBias, 0.00001);
    // Every comparison is against the receiver plane at its own direction,
    // bounded to the configured world-texel allowance (converted to depth
    // through the projection's local slope B / major^2). Derivatives that do
    // not span a plane, or a plane through the light, give no correction.
    vec3 planeNormal = cross(receiver.worldPositionDx,
        receiver.worldPositionDy);
    float planeLengthSquared = dot(planeNormal, planeNormal);
    planeNormal *= inversesqrt(max(planeLengthSquared, 1.0e-30));
    vec3 depthPerUnitMajor = planeNormal *
        (-entry.depthBias.y / dot(planeNormal, lightToReceiver));
    bool planeValid = planeLengthSquared > 1.0e-24 &&
        !isinf(planeLengthSquared) && !any(isinf(depthPerUnitMajor)) &&
        !any(isnan(depthPerUnitMajor));
    plane.depthPerUnitMajor = planeValid ? depthPerUnitMajor : vec3(0.0);
    plane.centerDepth = dot(plane.depthPerUnitMajor,
        iridiumPointShadowUnitMajor(direction));
    float safeMajorDistance = max(majorDistance, 0.00001);
    plane.maximumCorrection = planeValid
        ? entry.depthBias.y * entry.filterParameters.z * worldUnitsPerTexel /
            (safeMajorDistance * receiverDistance)
        : 0.0;
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
            vec3 sampleDirection = direction + tangent * disk.x +
                bitangent * disk.y;
            float storedDepth = iridiumPointShadowDepth(entry.metadata.z,
                sampleDirection, entry.metadata.w);
            if (storedDepth < iridiumPointShadowReference(plane,
                    sampleDirection)) {
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
                entry.metadata.w, direction, tangent, bitangent, resolution,
                angularTexel);
        float visibility = 0.0;
        uint filterSamples = max(entry.filterMetadata.y, 1u);
        for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
            if (sampleIndex >= filterSamples) break;
            vec2 disk = iridiumShadowDiskSample(sampleIndex,
                filterSamples, rotation) * penumbraRadius * angularTexel;
            visibility += iridiumPointShadowBilinearCompare(plane,
                entry.metadata.z, entry.metadata.w,
                direction + tangent * disk.x + bitangent * disk.y,
                resolution, angularTexel);
        }
        return visibility / float(filterSamples);
    }

    return iridiumPointShadowHardFilter(plane, entry.metadata.z,
        entry.metadata.w, direction, tangent, bitangent, resolution,
        angularTexel);
}

#endif
