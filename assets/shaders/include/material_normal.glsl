#ifndef IRIDIUM_MATERIAL_NORMAL_GLSL
#define IRIDIUM_MATERIAL_NORMAL_GLSL

const float MATERIAL_NORMAL_EPSILON = 1.0e-12;

struct MaterialTangentFrame {
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
};

vec3 materialFallbackTangent(vec3 unitNormal) {
    vec3 axis = abs(unitNormal.z) < 0.999
        ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return normalize(cross(axis, unitNormal));
}

MaterialTangentFrame materialBuildTangentFrame(vec3 geometricNormal,
    vec3 tangent, float handedness, bool doubleSided, bool frontFacing) {
    MaterialTangentFrame frame;
    frame.normal = normalize(geometricNormal);
    if (doubleSided && !frontFacing) frame.normal = -frame.normal;
    tangent -= frame.normal * dot(frame.normal, tangent);
    frame.tangent = any(isnan(tangent)) || any(isinf(tangent)) ||
        dot(tangent, tangent) <= MATERIAL_NORMAL_EPSILON
        ? materialFallbackTangent(frame.normal) : normalize(tangent);
    float orientation = handedness < 0.0 ? -1.0 : 1.0;
    frame.bitangent = normalize(cross(frame.normal, frame.tangent)) * orientation;
    return frame;
}

vec3 materialApplyTangentNormal(MaterialTangentFrame frame,
    vec3 encodedNormal, float normalScale) {
    vec3 tangentNormal = vec3(
        (encodedNormal.x * 2.0 - 1.0) * normalScale,
        (encodedNormal.y * 2.0 - 1.0) * normalScale,
        encodedNormal.z * 2.0 - 1.0);
    return normalize(frame.tangent * tangentNormal.x +
        frame.bitangent * tangentNormal.y + frame.normal * tangentNormal.z);
}

vec3 materialReconstructEncodedNormalZ(vec3 encodedNormal) {
    vec2 xy = encodedNormal.xy * 2.0 - 1.0;
    float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    return vec3(encodedNormal.xy, z * 0.5 + 0.5);
}

vec2 materialOctahedralWrap(vec2 value) {
    return (1.0 - abs(value.yx)) * mix(vec2(-1.0), vec2(1.0),
        step(vec2(0.0), value));
}

vec2 materialEncodeOctahedralNormal(vec3 normal) {
    vec3 folded = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    return folded.z >= 0.0 ? folded.xy : materialOctahedralWrap(folded.xy);
}

vec3 materialDecodeOctahedralNormal(vec2 encoded) {
    vec3 normal = vec3(encoded,
        1.0 - abs(encoded.x) - abs(encoded.y));
    float fold = clamp(-normal.z, 0.0, 1.0);
    normal.x += normal.x >= 0.0 ? -fold : fold;
    normal.y += normal.y >= 0.0 ? -fold : fold;
    return normalize(normal);
}

#endif
