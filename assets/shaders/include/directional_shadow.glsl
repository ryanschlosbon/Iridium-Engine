#ifndef IRIDIUM_DIRECTIONAL_SHADOW_GLSL
#define IRIDIUM_DIRECTIONAL_SHADOW_GLSL

#include "include/shadow_filter.glsl"

#ifndef IRIDIUM_LIGHTING_SET
#error IRIDIUM_LIGHTING_SET must name the shared scene descriptor set
#endif

layout(set = IRIDIUM_LIGHTING_SET, binding = 20)
    uniform sampler2DArray iridiumDirectionalShadowMap;
layout(std140, set = IRIDIUM_LIGHTING_SET, binding = 21) uniform
    IridiumDirectionalShadowData {
    mat4 iridiumWorldToShadowClip[8];
    vec4 iridiumShadowSplitFar[2];
    uvec4 iridiumShadowMetadata[2];
    vec4 iridiumShadowTexelWorldSize[2];
    vec4 iridiumShadowDepthSpanMeters[2];
    vec4 iridiumShadowFilterParameters[2];
    uvec4 iridiumShadowFilterMetadata[2];
    vec4 iridiumShadowBiasParameters;
};

float iridiumDirectionalShadowBilinearCompare(uint layer, vec2 uv,
    float referenceDepth) {
    ivec2 size = textureSize(iridiumDirectionalShadowMap, 0).xy;
    vec2 pixel = uv * vec2(size) - vec2(0.5);
    ivec2 base = ivec2(floor(pixel));
    vec2 blend = fract(pixel);
    ivec2 maximumPixel = size - ivec2(1);
    ivec2 p00 = clamp(base, ivec2(0), maximumPixel);
    ivec2 p10 = clamp(base + ivec2(1, 0), ivec2(0), maximumPixel);
    ivec2 p01 = clamp(base + ivec2(0, 1), ivec2(0), maximumPixel);
    ivec2 p11 = clamp(base + ivec2(1, 1), ivec2(0), maximumPixel);
    float v00 = iridiumShadowCompare(referenceDepth, texelFetch(
        iridiumDirectionalShadowMap, ivec3(p00, int(layer)), 0).r);
    float v10 = iridiumShadowCompare(referenceDepth, texelFetch(
        iridiumDirectionalShadowMap, ivec3(p10, int(layer)), 0).r);
    float v01 = iridiumShadowCompare(referenceDepth, texelFetch(
        iridiumDirectionalShadowMap, ivec3(p01, int(layer)), 0).r);
    float v11 = iridiumShadowCompare(referenceDepth, texelFetch(
        iridiumDirectionalShadowMap, ivec3(p11, int(layer)), 0).r);
    return mix(mix(v00, v10, blend.x), mix(v01, v11, blend.x), blend.y);
}

float iridiumDirectionalShadowHardFilter(uint layer, vec2 uv,
    vec2 texel, float referenceDepth) {
    const float tentWeights[3] = float[3](1.0, 2.0, 1.0);
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visibility += tentWeights[x + 1] * tentWeights[y + 1] *
                iridiumDirectionalShadowBilinearCompare(layer,
                    uv + vec2(x, y) * texel, referenceDepth);
    return visibility / 16.0;
}

uint iridiumDirectionalShadowOwner(uint lightSlot) {
    for (uint shadowIndex = 0u; shadowIndex < 2u; ++shadowIndex)
        if (iridiumShadowMetadata[shadowIndex].w != 0u &&
            iridiumShadowMetadata[shadowIndex].x == lightSlot)
            return shadowIndex;
    return 2u;
}

uint iridiumDirectionalShadowCascade(uint shadowIndex, float viewDepth) {
    if (viewDepth <= iridiumShadowSplitFar[shadowIndex].x) return 0u;
    if (viewDepth <= iridiumShadowSplitFar[shadowIndex].y) return 1u;
    if (viewDepth <= iridiumShadowSplitFar[shadowIndex].z) return 2u;
    return 3u;
}

vec3 iridiumDirectionalShadowCascadeDebug(float viewDepth) {
    uint shadowIndex = iridiumShadowMetadata[0].w != 0u ? 0u : 1u;
    uint cascade = iridiumDirectionalShadowCascade(shadowIndex, viewDepth);
    if (iridiumShadowMetadata[shadowIndex].w == 0u ||
        (iridiumShadowMetadata[shadowIndex].y & (1u << cascade)) == 0u)
        return vec3(0.0);
    const vec3 colors[4] = vec3[4](vec3(1.0, 0.08, 0.04),
        vec3(0.04, 1.0, 0.08), vec3(0.05, 0.2, 1.0),
        vec3(1.0, 0.85, 0.05));
    if (cascade == 3u ||
        (iridiumShadowMetadata[shadowIndex].y &
            (1u << (cascade + 1u))) == 0u)
        return colors[cascade];
    float splitNear = cascade == 0u ? 0.0 :
        iridiumShadowSplitFar[shadowIndex][cascade - 1u];
    float blendWidth = (iridiumShadowSplitFar[shadowIndex][cascade] -
        splitNear) * 0.1;
    float blend = smoothstep(iridiumShadowSplitFar[shadowIndex][cascade] -
        blendWidth, iridiumShadowSplitFar[shadowIndex][cascade], viewDepth);
    return mix(colors[cascade], colors[cascade + 1u], blend);
}

float iridiumSampleDirectionalShadowCascade(uint shadowIndex, uint cascade,
    vec3 worldPosition, vec3 surfaceNormal, vec3 surfaceToLight) {
    uint layer = iridiumShadowMetadata[shadowIndex].z + cascade;
    vec4 clip = iridiumWorldToShadowClip[layer] *
        vec4(worldPosition, 1.0);
    vec3 coordinate = clip.xyz / clip.w;
    vec2 uv = coordinate.xy * 0.5 + 0.5;
    if (coordinate.z <= 0.0 || coordinate.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;
    float noL = clamp(dot(surfaceNormal, surfaceToLight), 0.0, 1.0);
    float texelWorldSize =
        iridiumShadowTexelWorldSize[shadowIndex][cascade];
    float depthSpan = iridiumShadowDepthSpanMeters[shadowIndex][cascade];
    float receiverWorldBias = texelWorldSize *
        iridiumShadowBiasParameters.z * mix(1.0, 2.0, 1.0 - noL);
    float receiverBias = receiverWorldBias / max(depthSpan, 0.000001);
    float referenceDepth = coordinate.z - receiverBias;
    vec2 texel = 1.0 / vec2(textureSize(
        iridiumDirectionalShadowMap, 0).xy);
    uvec4 filterMetadata = iridiumShadowFilterMetadata[shadowIndex];
    vec4 filterParameters = iridiumShadowFilterParameters[shadowIndex];
    if (filterMetadata.z != 0u && filterMetadata.x != 0u &&
        filterParameters.x > 0.0) {
        float maximumRadius = filterParameters.y;
        float searchRadius = clamp(filterParameters.x * coordinate.z *
            depthSpan / max(texelWorldSize, 0.000001), 0.0, maximumRadius);
        float rotation = iridiumShadowRotation(gl_FragCoord.xy);
        float blockerDepth = 0.0;
        uint blockerCount = 0u;
        for (uint sampleIndex = 0u; sampleIndex < 32u; ++sampleIndex) {
            if (sampleIndex >= filterMetadata.x) break;
            vec2 offset = iridiumShadowDiskSample(sampleIndex,
                filterMetadata.x, rotation) * searchRadius * texel;
            float storedDepth = texture(iridiumDirectionalShadowMap,
                vec3(clamp(uv + offset, vec2(0.0), vec2(1.0)),
                    float(layer))).r;
            if (storedDepth < referenceDepth) {
                blockerDepth += storedDepth;
                ++blockerCount;
            }
        }
        if (blockerCount == 0u) return 1.0;
        blockerDepth /= float(blockerCount);
        float penumbraRadius = clamp((referenceDepth - blockerDepth) *
            depthSpan * filterParameters.x /
            max(texelWorldSize, 0.000001), 0.0, maximumRadius);
        if (penumbraRadius <= 1.0)
            return iridiumDirectionalShadowHardFilter(layer, uv, texel,
                referenceDepth);
        float visibility = 0.0;
        uint filterSamples = max(filterMetadata.y, 1u);
        for (uint sampleIndex = 0u; sampleIndex < 64u; ++sampleIndex) {
            if (sampleIndex >= filterSamples) break;
            vec2 offset = iridiumShadowDiskSample(sampleIndex,
                filterSamples, rotation) * penumbraRadius * texel;
            float storedDepth = texture(iridiumDirectionalShadowMap,
                vec3(clamp(uv + offset, vec2(0.0), vec2(1.0)),
                    float(layer))).r;
            visibility += iridiumShadowCompare(referenceDepth, storedDepth);
        }
        return visibility / float(filterSamples);
    }

    return iridiumDirectionalShadowHardFilter(layer, uv, texel,
        referenceDepth);
}

float iridiumDirectionalShadowVisibility(uint lightSlot, vec3 worldPosition,
    vec3 surfaceNormal, vec3 surfaceToLight, float viewDepth) {
    uint shadowIndex = iridiumDirectionalShadowOwner(lightSlot);
    if (shadowIndex == 2u) return 1.0;
    uint cascade = iridiumDirectionalShadowCascade(shadowIndex, viewDepth);
    if ((iridiumShadowMetadata[shadowIndex].y & (1u << cascade)) == 0u)
        return 1.0;
    float visibility = iridiumSampleDirectionalShadowCascade(shadowIndex,
        cascade, worldPosition, surfaceNormal, surfaceToLight);
    if (cascade == 3u ||
        (iridiumShadowMetadata[shadowIndex].y &
            (1u << (cascade + 1u))) == 0u)
        return visibility;
    float splitNear = cascade == 0u ? 0.0 :
        iridiumShadowSplitFar[shadowIndex][cascade - 1u];
    float blendWidth = (iridiumShadowSplitFar[shadowIndex][cascade] -
        splitNear) * 0.1;
    float blend = smoothstep(iridiumShadowSplitFar[shadowIndex][cascade] -
        blendWidth, iridiumShadowSplitFar[shadowIndex][cascade], viewDepth);
    if (blend <= 0.0) return visibility;
    float nextVisibility = iridiumSampleDirectionalShadowCascade(shadowIndex,
        cascade + 1u, worldPosition, surfaceNormal, surfaceToLight);
    return mix(visibility, nextVisibility, blend);
}

#endif
