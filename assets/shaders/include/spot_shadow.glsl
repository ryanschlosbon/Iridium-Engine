#ifndef IRIDIUM_SPOT_SHADOW_GLSL
#define IRIDIUM_SPOT_SHADOW_GLSL

#include "include/shadow_filter.glsl"

#ifndef IRIDIUM_LIGHTING_SET
#error IRIDIUM_LIGHTING_SET must name the shared scene descriptor set
#endif

struct IridiumSpotShadowEntry {
    mat4 worldToShadowClip;
    vec4 atlasScaleBias;
    uvec4 metadata;
    vec4 biasParameters;
    vec4 projectionParameters;
    uvec4 filterMetadata;
};

layout(set = IRIDIUM_LIGHTING_SET, binding = 22)
    uniform sampler2D iridiumSpotShadowAtlas;
layout(std140, set = IRIDIUM_LIGHTING_SET, binding = 23) uniform
    IridiumSpotShadowData {
    IridiumSpotShadowEntry iridiumSpotShadowEntries[256];
    uvec4 iridiumSpotShadowMetadata;
};

float iridiumSpotShadowBilinearCompare(vec2 uv, vec2 minimumUv,
    vec2 maximumUv, vec2 receiverUv, float referenceDepth,
    vec2 depthGradient, float maximumPlaneCorrection) {
    ivec2 size = ivec2(iridiumSpotShadowMetadata.x);
    vec2 pixel = uv * vec2(size) - vec2(0.5);
    ivec2 base = ivec2(floor(pixel));
    vec2 blend = fract(pixel);
    ivec2 minimumPixel = ivec2(floor(minimumUv * vec2(size)));
    ivec2 maximumPixel = ivec2(floor(maximumUv * vec2(size)));
    ivec2 p00 = clamp(base, minimumPixel, maximumPixel);
    ivec2 p10 = clamp(base + ivec2(1, 0), minimumPixel, maximumPixel);
    ivec2 p01 = clamp(base + ivec2(0, 1), minimumPixel, maximumPixel);
    ivec2 p11 = clamp(base + ivec2(1, 1), minimumPixel, maximumPixel);
    vec2 inverseSize = 1.0 / vec2(size);
    float v00 = iridiumShadowCompare(iridiumShadowReceiverPlaneReference(
        referenceDepth, depthGradient,
        (vec2(p00) + vec2(0.5)) * inverseSize - receiverUv,
        maximumPlaneCorrection), texelFetch(iridiumSpotShadowAtlas, p00, 0).r);
    float v10 = iridiumShadowCompare(iridiumShadowReceiverPlaneReference(
        referenceDepth, depthGradient,
        (vec2(p10) + vec2(0.5)) * inverseSize - receiverUv,
        maximumPlaneCorrection), texelFetch(iridiumSpotShadowAtlas, p10, 0).r);
    float v01 = iridiumShadowCompare(iridiumShadowReceiverPlaneReference(
        referenceDepth, depthGradient,
        (vec2(p01) + vec2(0.5)) * inverseSize - receiverUv,
        maximumPlaneCorrection), texelFetch(iridiumSpotShadowAtlas, p01, 0).r);
    float v11 = iridiumShadowCompare(iridiumShadowReceiverPlaneReference(
        referenceDepth, depthGradient,
        (vec2(p11) + vec2(0.5)) * inverseSize - receiverUv,
        maximumPlaneCorrection), texelFetch(iridiumSpotShadowAtlas, p11, 0).r);
    return mix(mix(v00, v10, blend.x), mix(v01, v11, blend.x), blend.y);
}

float iridiumSpotShadowHardFilter(vec2 atlasUv, vec2 minimumUv,
    vec2 maximumUv, vec2 texel, float referenceDepth, vec2 depthGradient,
    float maximumPlaneCorrection) {
    const float tentWeights[3] = float[3](1.0, 2.0, 1.0);
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visibility += tentWeights[x + 1] * tentWeights[y + 1] *
                iridiumSpotShadowBilinearCompare(
                    atlasUv + vec2(x, y) * texel, minimumUv, maximumUv,
                    atlasUv, referenceDepth, depthGradient,
                    maximumPlaneCorrection);
    return visibility / 16.0;
}

float iridiumSpotShadowVisibility(uint lightSlot, PackedGpuLight lightRecord,
    IridiumShadowReceiver receiver, vec3 surfaceToLight) {
    if ((floatBitsToUint(lightRecord.shapeMetadata.z) &
        IRIDIUM_LIGHT_CASTS_SHADOWS_BIT) == 0u)
        return 1.0;
    uint slot = floatBitsToUint(lightRecord.shapeMetadata.w);
    if (slot == IRIDIUM_INVALID_SHADOW_DATA_SLOT || slot >= 256u)
        return 1.0;
    IridiumSpotShadowEntry entry = iridiumSpotShadowEntries[slot];
    if (entry.metadata.y == 0u || entry.metadata.x != lightSlot)
        return 1.0;
    float outerCos = max(lightRecord.directionOuterCos.w, 0.0001);
    float tangentHalfFov = sqrt(max(1.0 - outerCos * outerCos, 0.0)) /
        outerCos;
    float innerResolution = entry.atlasScaleBias.x *
        float(iridiumSpotShadowMetadata.x);
    float texelsPerRadian = innerResolution /
        max(2.0 * tangentHalfFov, 0.0001);
    // The geometric-normal offset is sized by the receiver's own texel
    // footprint (ADR-0010 decision 4; the directional contract).
    vec4 receiverClip = entry.worldToShadowClip *
        vec4(receiver.worldPosition, 1.0);
    float normalOffsetScale = iridiumShadowNormalOffsetScale(
        receiver.geometricNormal, surfaceToLight, entry.biasParameters.z);
    vec3 offsetWorldPosition = receiver.worldPosition +
        receiver.geometricNormal * (max(receiverClip.w, 0.0001) /
            texelsPerRadian * normalOffsetScale);
    vec4 clip = entry.worldToShadowClip * vec4(offsetWorldPosition, 1.0);
    vec3 coordinate = clip.xyz / clip.w;
    vec2 localUv = coordinate.xy * 0.5 + 0.5;
    if (coordinate.z <= 0.0 || coordinate.z >= 1.0 ||
        any(lessThan(localUv, vec2(0.0))) ||
        any(greaterThan(localUv, vec2(1.0))))
        return 1.0;
    vec2 atlasUv = localUv * entry.atlasScaleBias.xy +
        entry.atlasScaleBias.zw;
    vec2 texel = 1.0 / vec2(iridiumSpotShadowMetadata.x);
    vec2 minimumUv = entry.atlasScaleBias.zw + texel * 0.5;
    vec2 maximumUv = entry.atlasScaleBias.zw + entry.atlasScaleBias.xy -
        texel * 0.5;
    float receiverDistance = max(clip.w, 0.0001);
    float worldUnitsPerTexel = 2.0 * tangentHalfFov * receiverDistance /
        max(innerResolution, 1.0);
    float nearPlane = entry.projectionParameters.x;
    float farPlane = entry.projectionParameters.y;
    float projectionA = farPlane / max(farPlane - nearPlane, 0.0001);
    float projectionB = farPlane * nearPlane /
        max(farPlane - nearPlane, 0.0001);
    float noL = clamp(dot(receiver.shadingNormal, surfaceToLight), 0.0, 1.0);
    float receiverWorldBias = worldUnitsPerTexel * entry.biasParameters.x *
        mix(1.0, 2.0, 1.0 - noL);
    float biasedDistance = max(receiverDistance - receiverWorldBias,
        nearPlane);
    float receiverBias = projectionB / biasedDistance -
        projectionB / receiverDistance;
    float referenceDepth = coordinate.z - max(receiverBias, 0.0);
    // Perspective depth is affine in shadow UV over a planar receiver, so the
    // directional receiver-plane gradient applies; the world-texel allowance
    // converts to depth through the projection's local slope B / w^2.
    vec4 clipDx = entry.worldToShadowClip * vec4(receiver.worldPositionDx, 0.0);
    vec4 clipDy = entry.worldToShadowClip * vec4(receiver.worldPositionDy, 0.0);
    vec3 projectedDx = (clipDx.xyz - coordinate * clipDx.w) / clip.w;
    vec3 projectedDy = (clipDy.xyz - coordinate * clipDy.w) / clip.w;
    vec2 depthGradient = iridiumShadowReceiverPlaneGradient(
        projectedDx.xy * 0.5 * entry.atlasScaleBias.xy,
        projectedDy.xy * 0.5 * entry.atlasScaleBias.xy,
        projectedDx.z, projectedDy.z);
    float maximumPlaneCorrection = entry.biasParameters.y *
        worldUnitsPerTexel * projectionB /
        (receiverDistance * receiverDistance);
    if (entry.filterMetadata.z != 0u && entry.filterMetadata.x != 0u &&
        entry.projectionParameters.z > 0.0) {
        float maximumRadius = float(entry.filterMetadata.w) / 256.0;
        float searchRadius = clamp(entry.projectionParameters.z /
            max(receiverDistance, 0.0001) * texelsPerRadian,
            0.0, maximumRadius);
        float rotation = iridiumShadowRotation(gl_FragCoord.xy);
        float blockerDistance = 0.0;
        uint blockerCount = 0u;
        for (uint sampleIndex = 0u; sampleIndex < 32u; ++sampleIndex) {
            if (sampleIndex >= entry.filterMetadata.x) break;
            vec2 offset = iridiumShadowDiskSample(sampleIndex,
                entry.filterMetadata.x, rotation) * searchRadius * texel;
            vec2 sampleUv = clamp(atlasUv + offset, minimumUv, maximumUv);
            float storedDepth = texture(iridiumSpotShadowAtlas, sampleUv).r;
            if (storedDepth < iridiumShadowReceiverPlaneReference(
                    referenceDepth, depthGradient, sampleUv - atlasUv,
                    maximumPlaneCorrection)) {
                blockerDistance += projectionB /
                    max(projectionA - storedDepth, 0.000001);
                ++blockerCount;
            }
        }
        if (blockerCount == 0u) return 1.0;
        blockerDistance /= float(blockerCount);
        float penumbraAngle = entry.projectionParameters.z *
            max(receiverDistance - blockerDistance, 0.0) /
            max(blockerDistance * receiverDistance, 0.000001);
        float penumbraRadius = clamp(penumbraAngle * texelsPerRadian,
            0.0, maximumRadius);
        if (penumbraRadius <= 1.0)
            return iridiumSpotShadowHardFilter(atlasUv, minimumUv,
                maximumUv, texel, referenceDepth, depthGradient,
                maximumPlaneCorrection);
        float visibility = 0.0;
        uint filterSamples = max(entry.filterMetadata.y, 1u);
        for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
            if (sampleIndex >= filterSamples) break;
            vec2 offset = iridiumShadowDiskSample(sampleIndex,
                filterSamples, rotation) * penumbraRadius * texel;
            vec2 sampleUv = clamp(atlasUv + offset, minimumUv, maximumUv);
            float storedDepth = texture(iridiumSpotShadowAtlas, sampleUv).r;
            visibility += iridiumShadowCompare(
                iridiumShadowReceiverPlaneReference(referenceDepth,
                    depthGradient, sampleUv - atlasUv,
                    maximumPlaneCorrection), storedDepth);
        }
        return visibility / float(filterSamples);
    }

    return iridiumSpotShadowHardFilter(atlasUv, minimumUv, maximumUv,
        texel, referenceDepth, depthGradient, maximumPlaneCorrection);
}

#endif
