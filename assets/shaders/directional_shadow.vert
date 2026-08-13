#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec2 fragTexCoord1;

layout(std140, set = 0, binding = 0) uniform DirectionalShadowData {
    mat4 worldToShadowClip[8];
    vec4 splitFar[2];
    uvec4 metadata[2];
    vec4 texelWorldSize[2];
    vec4 biasParameters;
} shadowData;

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint cascadeIndex;
    uint padding1;
    uint padding2;
} push;

void main() {
    gl_Position = shadowData.worldToShadowClip[push.cascadeIndex] *
        push.renderMatrix * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
}
