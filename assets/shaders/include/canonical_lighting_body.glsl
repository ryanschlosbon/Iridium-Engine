#extension GL_EXT_nonuniform_qualifier : require

#include "include/scene_color.glsl"
#include "include/material_bsdf.glsl"
#define IRIDIUM_LIGHTING_SET 0
#include "include/clustered_light_access.glsl"
#include "include/environment_ibl.glsl"
#include "include/directional_shadow.glsl"
#include "include/spot_shadow.glsl"
#include "include/point_shadow.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D gDepth;
layout(set = 0, binding = 1) uniform sampler2D gNormalF90;
layout(set = 0, binding = 2) uniform sampler2D gDiffuseAo;
layout(set = 0, binding = 6) uniform sampler2D gEmissive;
layout(set = 0, binding = 7) uniform sampler2D gF0Roughness;
layout(set = 0, binding = 8) uniform usampler2D gMaterialFlags;
layout(push_constant) uniform PushConstants {
    vec3 viewPos;
    mat4 invView;
    mat4 invProj;
    ivec4 debugView;
} push;

vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewSpace = push.invProj * clipSpace;
    viewSpace /= viewSpace.w;
    return (push.invView * viewSpace).xyz;
}

vec3 idColor(uint id) {
    uint hash = id * 1664525u + 1013904223u;
    return vec3(float((hash >> 0u) & 255u), float((hash >> 8u) & 255u),
        float((hash >> 16u) & 255u)) / 255.0;
}

uint debugClusterIndex(float rawDepth) {
    vec4 viewPosition = push.invProj * vec4(
        fragTexCoord * 2.0 - 1.0, rawDepth, 1.0);
    float positiveDepth = max(iridiumClusterDepth.x,
        -viewPosition.z / viewPosition.w);
    uint slice = min(iridiumClusterLimits.x - 1u,
        uint(log(clamp(positiveDepth, iridiumClusterDepth.x,
            iridiumClusterDepth.y) / iridiumClusterDepth.x) *
            iridiumClusterDepth.z));
    uvec2 tile = min(uvec2(gl_FragCoord.xy) / iridiumClusterInput.zw,
        iridiumClusterGrid.zw - uvec2(1u));
    return (slice * iridiumClusterGrid.w + tile.y) *
        iridiumClusterGrid.z + tile.x;
}

vec3 clusterOccupancyColor(float occupancy) {
    float value = clamp(occupancy, 0.0, 1.0);
    return value < 0.5
        ? mix(vec3(0.02, 0.08, 0.35), vec3(0.0, 0.9, 0.7), value * 2.0)
        : mix(vec3(0.0, 0.9, 0.7), vec3(1.0, 0.05, 0.0),
            (value - 0.5) * 2.0);
}

void main() {
    float rawDepth = texture(gDepth, fragTexCoord).r;
    vec4 normalF90 = texture(gNormalF90, fragTexCoord);
    vec4 diffuseAo = texture(gDiffuseAo, fragTexCoord);
    vec4 f0Roughness = texture(gF0Roughness, fragTexCoord);
    vec3 emissive = max(texture(gEmissive, fragTexCoord).rgb, vec3(0.0));
    uint materialFlags = texelFetch(gMaterialFlags, ivec2(gl_FragCoord.xy), 0).r;

#if defined(IRIDIUM_PACKED_GBUFFER)
    vec2 octNormal = normalF90.xy;
    vec3 decodedNormal = vec3(octNormal,
        1.0 - abs(octNormal.x) - abs(octNormal.y));
    if (decodedNormal.z < 0.0) {
        decodedNormal.xy = (1.0 - abs(decodedNormal.yx)) * sign(decodedNormal.xy);
    }
    vec3 N = normalize(decodedNormal);
#else
    vec3 N = normalize(normalF90.xyz);
#endif
    vec3 diffuse = diffuseAo.rgb;
    float ao = clamp(diffuseAo.a, 0.0, 1.0);
    vec3 f0 = f0Roughness.rgb;
    float roughness = clamp(f0Roughness.a, 0.0, 1.0);
#if defined(IRIDIUM_PACKED_GBUFFER)
    // The frozen Q/C proposal has no F90 channel. Keeping the experiment honest
    // makes non-unit specular weight a measurable rejection case.
    vec3 f90 = vec3(1.0);
#else
    vec3 f90 = vec3(clamp(normalF90.a, 0.0, 1.0));
#endif

    if (push.debugView.x != 0 && push.debugView.x != 15 &&
        push.debugView.x != 16 && push.debugView.x != 17) {
        if (push.debugView.x == 1) outColor = vec4(diffuse, 1.0);
        else if (push.debugView.x == 2) outColor = vec4(N * 0.5 + 0.5, 1.0);
        else if (push.debugView.x == 3) outColor = vec4(vec3(roughness), 1.0);
        else if (push.debugView.x == 4) outColor = vec4(0.0, 0.0, 0.0, 1.0);
        else if (push.debugView.x == 5) outColor = vec4(emissive, 1.0);
        else if (push.debugView.x == 7) outColor = vec4(vec3(ao), 1.0);
        else if (push.debugView.x == 8) outColor = vec4(f0, 1.0);
        else if (push.debugView.x == 9) outColor = vec4(f90, 1.0);
        else if (push.debugView.x == 10)
            outColor = vec4(idColor(materialFlags & 0x000fffffu), 1.0);
        else if (push.debugView.x == 11)
            outColor = vec4(idColor((materialFlags >> 20u) & 0x3ffu), 1.0);
        else if (push.debugView.x == 12)
            outColor = vec4(idColor(materialFlags >> 30u), 1.0);
        else if (push.debugView.x == 13) {
            uint count = iridiumClusterHeaders[debugClusterIndex(rawDepth)].count;
            outColor = vec4(clusterOccupancyColor(
                float(count) / float(iridiumClusterLimits.y)), 1.0);
        }
        else if (push.debugView.x == 14) {
            bool overflow = iridiumClusterDiagnostics[3] != 0u;
            outColor = vec4(overflow ? vec3(1.0, 0.0, 0.8) :
                vec3(0.0, 0.15, 0.0), 1.0);
        }
        else outColor = vec4(vec3(rawDepth), 1.0);
        return;
    }

    vec3 fragPos = ReconstructWorldPos(fragTexCoord, rawDepth);
    if (rawDepth == 1.0) {
        if (push.debugView.x >= 15 && push.debugView.x <= 17) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        vec2 ndc = fragTexCoord * 2.0 - 1.0;
        vec4 eye = push.invProj * vec4(ndc, 1.0, 1.0);
        vec3 viewDir = normalize((push.invView * vec4(eye.xy, -1.0, 0.0)).xyz);
        outColor = vec4(iridiumSampleEnvironmentSky(viewDir), 1.0);
        return;
    }

    vec3 V = normalize(push.viewPos - fragPos);
    float viewDepth = dot(fragPos - push.viewPos,
        -normalize(push.invView[2].xyz));
    if (push.debugView.x == 16) {
        outColor = vec4(iridiumDirectionalShadowCascadeDebug(viewDepth), 1.0);
        return;
    }
    vec3 direct = vec3(0.0);
    float shadowVisibility = 1.0;
    IridiumDirectLightRange lightRange = iridiumDirectLightRange(
        fragPos, uvec2(gl_FragCoord.xy));
    uint directLightCount = iridiumDirectLightCount(lightRange);
    for (uint index = 0u; index < directLightCount; ++index) {
        uint lightSlot = iridiumDirectLightSlot(lightRange, index);
        IridiumDirectLightSample light = iridiumEvaluateDirectLightSlot(
            lightSlot, fragPos, N);
        float noL = max(dot(N, light.direction), 0.0);
        if (noL > 0.0) {
            float visibility = iridiumDirectionalShadowVisibility(lightSlot,
                fragPos, N, light.direction, viewDepth);
            PackedGpuLight lightRecord = iridiumLights[lightSlot];
            if ((floatBitsToUint(lightRecord.shapeMetadata.z) & 3u) ==
                IRIDIUM_LIGHT_TYPE_SPOT)
                visibility *= iridiumSpotShadowVisibility(lightSlot,
                    lightRecord, fragPos, N, light.direction);
            else if ((floatBitsToUint(lightRecord.shapeMetadata.z) & 3u) ==
                IRIDIUM_LIGHT_TYPE_POINT)
                visibility *= iridiumPointShadowVisibility(lightSlot,
                    lightRecord, fragPos, N, light.direction);
            shadowVisibility = min(shadowVisibility, visibility);
            direct += materialEvaluateCanonicalBrdf(diffuse, f0, f90,
                roughness, N, V, light.direction) * light.radiance * noL *
                visibility;
        }
    }
    if (push.debugView.x == 17) {
        outColor = vec4(vec3(shadowVisibility), 1.0);
        return;
    }
    if (push.debugView.x == 15) {
        outColor = vec4(max(direct, vec3(0.0)), 1.0);
        return;
    }
    vec3 ambient = iridiumEvaluateCanonicalIbl(diffuse, f0, f90,
        roughness, N, V, ao, fragPos, uvec2(gl_FragCoord.xy));
    outColor = vec4(ambient + direct + emissive, 1.0);
}
