#version 450

layout(location = 0) in vec3 inPosition;

struct PointShadowEntry {
    mat4 worldToShadowClip[6];
    vec4 lightPositionFar;
    uvec4 metadata;
    vec4 depthBias;
    vec4 filterParameters;
    uvec4 filterMetadata;
};
layout(std140, set = 0, binding = 0) uniform PointShadowData {
    PointShadowEntry entries[56];
    uvec4 metadata;
} shadowData;
layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint shadowFaceSlot;
    uint padding1;
    uint padding2;
} push;

void main() {
    uint entry = push.shadowFaceSlot / 6u;
    uint face = push.shadowFaceSlot % 6u;
    gl_Position = shadowData.entries[entry].worldToShadowClip[face] *
        push.renderMatrix * vec4(inPosition, 1.0);
}
