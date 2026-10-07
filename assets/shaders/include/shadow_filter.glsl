#ifndef IRIDIUM_SHADOW_FILTER_GLSL
#define IRIDIUM_SHADOW_FILTER_GLSL

const float IRIDIUM_SHADOW_GOLDEN_ANGLE = 2.39996322972865332;

// The receiver contract shared by directional, spot and point shadows (ADR-0010
// decision 4): the geometric normal offsets the receiver, and the world-position
// derivatives give the receiver plane each filter tap is compared against.
struct IridiumShadowReceiver {
    vec3 worldPosition;
    vec3 shadingNormal;
    vec3 geometricNormal;
    vec3 worldPositionDx;
    vec3 worldPositionDy;
};

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

// Geometric-normal receiver offset in shadow texels: front-facing only, fading
// with geometric N.L so normal incidence and back faces are not moved.
float iridiumShadowNormalOffsetScale(vec3 geometricNormal,
    vec3 surfaceToLight, float normalOffsetTexels) {
    float geometricNoL = clamp(dot(geometricNormal, surfaceToLight), 0.0, 1.0);
    return geometricNoL > 0.0
        ? normalOffsetTexels *
            sqrt(max(1.0 - geometricNoL * geometricNoL, 0.0))
        : 0.0;
}

// Receiver-plane depth gradient over shadow-map UV from the UV and depth
// footprints of the screen-space derivatives; degenerate input gives zero.
vec2 iridiumShadowReceiverPlaneGradient(vec2 uvDx, vec2 uvDy,
    float depthDx, float depthDy) {
    float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) < 1.0e-12 || isnan(determinant) || isinf(determinant))
        return vec2(0.0);
    vec2 gradient = vec2(
        (depthDx * uvDy.y - depthDy * uvDx.y) / determinant,
        (depthDy * uvDx.x - depthDx * uvDy.x) / determinant);
    return any(isnan(gradient)) || any(isinf(gradient))
        ? vec2(0.0) : gradient;
}

float iridiumShadowReceiverPlaneReference(float referenceDepth,
    vec2 depthGradient, vec2 uvOffset, float maximumCorrection) {
    return referenceDepth + clamp(dot(depthGradient, uvOffset),
        -maximumCorrection, maximumCorrection);
}

#endif
