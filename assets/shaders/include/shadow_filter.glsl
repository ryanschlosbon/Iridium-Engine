#ifndef IRIDIUM_SHADOW_FILTER_GLSL
#define IRIDIUM_SHADOW_FILTER_GLSL

const float IRIDIUM_SHADOW_GOLDEN_ANGLE = 2.39996322972865332;

float iridiumShadowHash(vec2 value) {
    vec3 state = fract(vec3(value.xyx) * 0.1031);
    state += dot(state, state.yzx + 33.33);
    return fract((state.x + state.y) * state.z);
}

vec2 iridiumShadowDiskSample(uint sampleIndex, uint sampleCount,
    float rotation) {
    float index = float(sampleIndex) + 0.5;
    float radius = sqrt(index / max(float(sampleCount), 1.0));
    float angle = index * IRIDIUM_SHADOW_GOLDEN_ANGLE + rotation;
    return vec2(cos(angle), sin(angle)) * radius;
}

float iridiumShadowRotation(vec2 stableCoordinate) {
    return iridiumShadowHash(floor(stableCoordinate)) * 6.28318530717958648;
}

float iridiumShadowCompare(float referenceDepth, float storedDepth) {
    return referenceDepth <= storedDepth ? 1.0 : 0.0;
}

#endif
