#ifndef IRIDIUM_ENVIRONMENT_IBL_GLSL
#define IRIDIUM_ENVIRONMENT_IBL_GLSL

#ifndef IRIDIUM_LIGHTING_SET
#error IRIDIUM_LIGHTING_SET must name the scene-lighting descriptor set
#endif

layout(set = IRIDIUM_LIGHTING_SET, binding = 16) uniform samplerCube
    iridiumEnvironmentIrradiance;
layout(set = IRIDIUM_LIGHTING_SET, binding = 17) uniform samplerCube
    iridiumEnvironmentPrefiltered;
layout(set = IRIDIUM_LIGHTING_SET, binding = 18) uniform sampler2D
    iridiumEnvironmentBrdfLut;
layout(set = IRIDIUM_LIGHTING_SET, binding = 19) uniform samplerCube
    iridiumEnvironmentRadiance;

#include "include/reflection_probe_records.glsl"

struct IridiumReflectionProbeClusterHeader {
    uint offset;
    uint count;
};

layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 28) readonly buffer
    IridiumReflectionProbeRecordBuffer {
    PackedGpuReflectionProbe iridiumReflectionProbes[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 29) readonly buffer
    IridiumReflectionProbeHeaderBuffer {
    IridiumReflectionProbeClusterHeader iridiumReflectionProbeHeaders[];
};
layout(std430, set = IRIDIUM_LIGHTING_SET, binding = 30) readonly buffer
    IridiumReflectionProbeIndexBuffer {
    uint iridiumReflectionProbeSlots[];
};
layout(set = IRIDIUM_LIGHTING_SET, binding = 31) uniform samplerCube
    iridiumReflectionProbePrefiltered[64];

vec3 iridiumRotateEnvironmentDirection(vec3 direction) {
    float sineYaw = sin(iridiumEnvironmentSettings.z);
    float cosineYaw = cos(iridiumEnvironmentSettings.z);
    return vec3(cosineYaw * direction.x - sineYaw * direction.z,
        direction.y,
        sineYaw * direction.x + cosineYaw * direction.z);
}

vec3 iridiumEnvironmentSpecular(vec3 f0, vec3 f90,
    float perceptualRoughness, vec3 normal, vec3 view) {
    float noV = clamp(dot(normal, view), 0.0, 1.0);
    float levelCount = float(textureQueryLevels(
        iridiumEnvironmentPrefiltered));
    float lod = clamp(perceptualRoughness, 0.0, 1.0) *
        max(levelCount - 1.0, 0.0);
    vec3 prefiltered = textureLod(iridiumEnvironmentPrefiltered,
        iridiumRotateEnvironmentDirection(reflect(-view, normal)), lod).rgb;
    vec2 integrated = texture(iridiumEnvironmentBrdfLut,
        vec2(noV, clamp(perceptualRoughness, 0.0, 1.0))).rg;
    // Product v1 is the measured single-scatter F0/F90 split sum. It applies
    // no unvalidated multi-scatter energy compensation.
    int flags = int(iridiumEnvironmentSettings.w + 0.5);
    float lightingScale = (flags & 2) != 0
        ? iridiumEnvironmentSettings.x : 0.0;
    return prefiltered * (f0 * integrated.x + f90 * integrated.y) *
        lightingScale;
}

float iridiumReflectionProbeInfluence(PackedGpuReflectionProbe probe,
    vec3 worldPosition) {
    vec3 local = (probe.worldToProbe * vec4(worldPosition, 1.0)).xyz;
    float boundary;
    if ((probe.metadata.x & IRIDIUM_PROBE_BOX_SHAPE_BIT) != 0u) {
        vec3 margin = probe.influence.xyz - abs(local);
        boundary = min(margin.x, min(margin.y, margin.z));
    }
    else boundary = probe.influence.x - length(local);
    if (!(boundary >= 0.0)) return 0.0;
    return probe.influence.w > 0.0
        ? clamp(boundary / probe.influence.w, 0.0, 1.0) : 1.0;
}

bool iridiumReflectionProbePreferred(PackedGpuReflectionProbe candidate,
    float candidateInfluence, PackedGpuReflectionProbe current,
    float currentInfluence) {
    int candidatePriority = int(candidate.metadata.z);
    int currentPriority = int(current.metadata.z);
    if (candidatePriority != currentPriority)
        return candidatePriority > currentPriority;
    if (candidateInfluence != currentInfluence)
        return candidateInfluence > currentInfluence;
    return candidate.metadata.w < current.metadata.w;
}

vec3 iridiumReflectionProbeDirection(PackedGpuReflectionProbe probe,
    vec3 worldPosition, vec3 reflectionDirection) {
    vec3 fallback = normalize(reflectionDirection);
    if ((probe.metadata.x & IRIDIUM_PROBE_BOX_SHAPE_BIT) == 0u ||
        (probe.metadata.x & IRIDIUM_PROBE_BOX_PROJECTION_BIT) == 0u)
        return fallback;
    vec3 localOrigin = (probe.worldToProbe *
        vec4(worldPosition, 1.0)).xyz;
    vec3 localDirection = mat3(probe.worldToProbe) * fallback;
    if (any(greaterThan(abs(localOrigin), probe.influence.xyz)))
        return fallback;
    float hitDistance = 3.402823e38;
    for (uint axis = 0u; axis < 3u; ++axis) {
        if (abs(localDirection[axis]) <= 1.0e-6) continue;
        float boundary = localDirection[axis] > 0.0
            ? probe.influence[axis] : -probe.influence[axis];
        float distance = (boundary - localOrigin[axis]) /
            localDirection[axis];
        if (distance >= 0.0) hitDistance = min(hitDistance, distance);
    }
    if (hitDistance == 3.402823e38) return fallback;
    vec3 localHit = localOrigin + localDirection * hitDistance;
    if (dot(localHit, localHit) <= 1.0e-12) return fallback;
    return normalize(transpose(mat3(probe.worldToProbe)) *
        normalize(localHit));
}

struct IridiumReflectionProbeSpecular {
    vec3 value;
    float coverage;
};

IridiumReflectionProbeSpecular iridiumEvaluateReflectionProbeSpecular(
    vec3 f0, vec3 f90, float perceptualRoughness, vec3 normal, vec3 view,
    vec3 worldPosition, uvec2 pixel) {
    IridiumReflectionProbeSpecular result;
    result.value = vec3(0.0);
    result.coverage = 0.0;
    uint cluster = iridiumShadingClusterIndex(worldPosition, pixel);
    IridiumReflectionProbeClusterHeader header =
        iridiumReflectionProbeHeaders[cluster];
    uint selectedSlots[2];
    float selectedInfluences[2];
    uint selectedCount = 0u;
    for (uint candidate = 0u; candidate < min(header.count, 4u);
        ++candidate) {
        uint slot = iridiumReflectionProbeSlots[header.offset + candidate];
        PackedGpuReflectionProbe probe = iridiumReflectionProbes[slot];
        float influence = iridiumReflectionProbeInfluence(probe,
            worldPosition);
        if (!(influence > 0.0)) continue;
        uint position = selectedCount;
        for (uint index = 0u; index < selectedCount; ++index) {
            if (iridiumReflectionProbePreferred(probe, influence,
                    iridiumReflectionProbes[selectedSlots[index]],
                    selectedInfluences[index])) {
                position = index;
                break;
            }
        }
        if (position >= 2u) continue;
        uint newCount = min(selectedCount + 1u, 2u);
        for (uint index = newCount - 1u; index > position; --index) {
            selectedSlots[index] = selectedSlots[index - 1u];
            selectedInfluences[index] = selectedInfluences[index - 1u];
        }
        selectedSlots[position] = slot;
        selectedInfluences[position] = influence;
        selectedCount = newCount;
    }
    if (selectedCount == 0u) return result;
    float influenceSum = selectedInfluences[0];
    if (selectedCount > 1u) influenceSum += selectedInfluences[1];
    result.coverage = clamp(selectedInfluences[0], 0.0, 1.0);
    float noV = clamp(dot(normal, view), 0.0, 1.0);
    vec2 integrated = texture(iridiumEnvironmentBrdfLut,
        vec2(noV, clamp(perceptualRoughness, 0.0, 1.0))).rg;
    vec3 reflected = reflect(-view, normal);
    for (uint index = 0u; index < selectedCount; ++index) {
        PackedGpuReflectionProbe probe =
            iridiumReflectionProbes[selectedSlots[index]];
        uint environment = probe.metadata.y;
        float levelCount = float(textureQueryLevels(
            iridiumReflectionProbePrefiltered[nonuniformEXT(environment)]));
        float lod = clamp(perceptualRoughness, 0.0, 1.0) *
            max(levelCount - 1.0, 0.0);
        vec3 direction = iridiumReflectionProbeDirection(probe,
            worldPosition, reflected);
        vec3 prefiltered = textureLod(
            iridiumReflectionProbePrefiltered[nonuniformEXT(environment)],
            direction, lod).rgb;
        float weight = selectedInfluences[index] / influenceSum *
            result.coverage;
        result.value += prefiltered *
            (f0 * integrated.x + f90 * integrated.y) *
            probe.positionIntensity.w * weight;
    }
    return result;
}

vec3 iridiumSceneSpecular(vec3 f0, vec3 f90,
    float perceptualRoughness, vec3 normal, vec3 view,
    vec3 worldPosition, uvec2 pixel) {
    IridiumReflectionProbeSpecular local =
        iridiumEvaluateReflectionProbeSpecular(f0, f90,
            perceptualRoughness, normal, view, worldPosition, pixel);
    return local.value + iridiumEnvironmentSpecular(f0, f90,
        perceptualRoughness, normal, view) * (1.0 - local.coverage);
}

vec3 iridiumEnvironmentDiffuse(vec3 diffuseAlbedo, vec3 f0, vec3 f90,
    vec3 normal, vec3 view) {
    float noV = clamp(dot(normal, view), 0.0, 1.0);
    vec3 fresnel = materialFresnelSchlick(f0, f90, noV);
    vec3 irradiance = texture(iridiumEnvironmentIrradiance,
        iridiumRotateEnvironmentDirection(normal)).rgb;
    int flags = int(iridiumEnvironmentSettings.w + 0.5);
    float lightingScale = (flags & 2) != 0
        ? iridiumEnvironmentSettings.x : 0.0;
    return irradiance * (vec3(1.0) - fresnel) * diffuseAlbedo /
        MATERIAL_PI * lightingScale;
}

vec3 iridiumEvaluateCanonicalIbl(vec3 diffuseAlbedo, vec3 f0, vec3 f90,
    float perceptualRoughness, vec3 normal, vec3 view, float ao,
    vec3 worldPosition, uvec2 pixel) {
    return (iridiumEnvironmentDiffuse(diffuseAlbedo, f0, f90, normal, view) +
        iridiumSceneSpecular(f0, f90, perceptualRoughness, normal, view,
            worldPosition, pixel)) *
        clamp(ao, 0.0, 1.0);
}

vec3 iridiumEvaluateStandardIbl(vec3 baseColor, vec3 f0, vec3 f90,
    float metallic, float perceptualRoughness, vec3 normal, vec3 view,
    float ao, vec3 worldPosition, uvec2 pixel) {
    vec3 diffuseAlbedo = baseColor * (1.0 - clamp(metallic, 0.0, 1.0));
    return iridiumEvaluateCanonicalIbl(diffuseAlbedo, f0, f90,
        perceptualRoughness, normal, view, ao, worldPosition, pixel);
}

vec3 iridiumSampleEnvironmentSky(vec3 direction) {
    int flags = int(iridiumEnvironmentSettings.w + 0.5);
    if ((flags & 1) == 0) return vec3(0.0);
    return texture(iridiumEnvironmentRadiance,
        iridiumRotateEnvironmentDirection(direction)).rgb *
        iridiumEnvironmentSettings.y;
}

vec3 iridiumSampleTransmissionEnvironment(vec3 worldPosition,
    vec3 direction, float perceptualRoughness, uvec2 pixel) {
    uint cluster = iridiumShadingClusterIndex(worldPosition, pixel);
    IridiumReflectionProbeClusterHeader header =
        iridiumReflectionProbeHeaders[cluster];
    uint selectedSlots[2];
    float selectedInfluences[2];
    uint selectedCount = 0u;
    for (uint candidate = 0u; candidate < min(header.count, 4u);
        ++candidate) {
        uint slot = iridiumReflectionProbeSlots[header.offset + candidate];
        PackedGpuReflectionProbe probe = iridiumReflectionProbes[slot];
        float influence = iridiumReflectionProbeInfluence(probe,
            worldPosition);
        if (!(influence > 0.0)) continue;
        uint position = selectedCount;
        for (uint index = 0u; index < selectedCount; ++index) {
            if (iridiumReflectionProbePreferred(probe, influence,
                    iridiumReflectionProbes[selectedSlots[index]],
                    selectedInfluences[index])) {
                position = index;
                break;
            }
        }
        if (position >= 2u) continue;
        uint newCount = min(selectedCount + 1u, 2u);
        for (uint index = newCount - 1u; index > position; --index) {
            selectedSlots[index] = selectedSlots[index - 1u];
            selectedInfluences[index] = selectedInfluences[index - 1u];
        }
        selectedSlots[position] = slot;
        selectedInfluences[position] = influence;
        selectedCount = newCount;
    }

    float localCoverage = selectedCount == 0u ? 0.0 :
        clamp(selectedInfluences[0], 0.0, 1.0);
    float influenceSum = selectedCount == 0u ? 1.0 :
        selectedInfluences[0] +
        (selectedCount > 1u ? selectedInfluences[1] : 0.0);
    vec3 local = vec3(0.0);
    for (uint index = 0u; index < selectedCount; ++index) {
        PackedGpuReflectionProbe probe =
            iridiumReflectionProbes[selectedSlots[index]];
        uint environment = probe.metadata.y;
        float levelCount = float(textureQueryLevels(
            iridiumReflectionProbePrefiltered[nonuniformEXT(environment)]));
        float lod = clamp(perceptualRoughness, 0.0, 1.0) *
            max(levelCount - 1.0, 0.0);
        vec3 sampleDirection = iridiumReflectionProbeDirection(probe,
            worldPosition, direction);
        local += textureLod(
            iridiumReflectionProbePrefiltered[nonuniformEXT(environment)],
            sampleDirection, lod).rgb * probe.positionIntensity.w *
            selectedInfluences[index] / influenceSum * localCoverage;
    }

    int flags = int(iridiumEnvironmentSettings.w + 0.5);
    vec3 global = vec3(0.0);
    if ((flags & 1) != 0) {
        float levelCount = float(textureQueryLevels(
            iridiumEnvironmentRadiance));
        float lod = clamp(perceptualRoughness, 0.0, 1.0) *
            max(levelCount - 1.0, 0.0);
        global = textureLod(iridiumEnvironmentRadiance,
            iridiumRotateEnvironmentDirection(direction), lod).rgb *
            iridiumEnvironmentSettings.y;
    }
    return local + global * (1.0 - localCoverage);
}

#endif
