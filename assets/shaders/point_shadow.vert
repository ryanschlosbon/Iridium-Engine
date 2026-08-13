#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec2 fragTexCoord1;

struct PointShadowEntry {
    mat4 worldToShadowClip[6];
    vec4 lightPositionFar;
    uvec4 metadata;
    vec4 depthBias;
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
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
}
