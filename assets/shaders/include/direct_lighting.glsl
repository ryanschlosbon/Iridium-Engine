#ifndef IRIDIUM_DIRECT_LIGHTING_GLSL
#define IRIDIUM_DIRECT_LIGHTING_GLSL

#include "include/lighting_records.glsl"

const float IRIDIUM_PHOTOMETRIC_TO_SCENE_SCALE = 1.0e-4;
const float IRIDIUM_LIGHT_DISTANCE_EPSILON = 1.0e-8;

struct IridiumDirectLightSample {
    vec3 direction;
    vec3 radiance;
};

uint iridiumPackedLightType(PackedGpuLight light) {
    return floatBitsToUint(light.shapeMetadata.z) & 3u;
}

float iridiumSmoothRangeWindow(float distanceToLight, float range) {
    if (!(range > 0.0) || distanceToLight >= range) return 0.0;
    float ratioSquared = distanceToLight * distanceToLight / (range * range);
    float base = clamp(1.0 - ratioSquared * ratioSquared, 0.0, 1.0);
    return base * base;
}

float iridiumSpotCone(PackedGpuLight light, vec3 surfaceToLight) {
    float cosineTheta = dot(-surfaceToLight,
        normalize(light.directionOuterCos.xyz));
    float outerCosine = light.directionOuterCos.w;
    float inverseDelta = light.shapeMetadata.y;
    if (!(inverseDelta > 0.0))
        return cosineTheta >= outerCosine ? 1.0 : 0.0;
    float value = clamp((cosineTheta - outerCosine) * inverseDelta,
        0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

IridiumDirectLightSample iridiumEvaluateDirectLight(PackedGpuLight light,
    vec3 worldPosition, vec3 surfaceNormal) {
    IridiumDirectLightSample result;
    result.direction = vec3(0.0, 0.0, 1.0);
    result.radiance = vec3(0.0);
    uint type = iridiumPackedLightType(light);
    if (type == IRIDIUM_LIGHT_TYPE_DIRECTIONAL) {
        result.direction = -normalize(light.directionOuterCos.xyz);
        result.radiance = light.colorIntensity.rgb *
            light.colorIntensity.w * IRIDIUM_PHOTOMETRIC_TO_SCENE_SCALE;
        return result;
    }

    vec3 toLight = light.positionRange.xyz - worldPosition;
    float distanceSquared = dot(toLight, toLight);
    float distanceToLight = sqrt(max(distanceSquared, 0.0));
    result.direction = distanceSquared > IRIDIUM_LIGHT_DISTANCE_EPSILON
        ? toLight * inversesqrt(distanceSquared)
        : normalize(surfaceNormal);
    float radiusSquared = light.shapeMetadata.x * light.shapeMetadata.x;
    float attenuation = iridiumSmoothRangeWindow(
        distanceToLight, light.positionRange.w) /
        max(max(distanceSquared, radiusSquared),
            IRIDIUM_LIGHT_DISTANCE_EPSILON);
    if (type == IRIDIUM_LIGHT_TYPE_SPOT)
        attenuation *= iridiumSpotCone(light, result.direction);
    result.radiance = light.colorIntensity.rgb * light.colorIntensity.w *
        IRIDIUM_PHOTOMETRIC_TO_SCENE_SCALE * attenuation;
    return result;
}

#endif
