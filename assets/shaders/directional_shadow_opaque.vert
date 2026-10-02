#version 450

layout(location = 0) in vec3 inPosition;

layout(std140, set = 0, binding = 0) uniform DirectionalShadowData {
    mat4 worldToShadowClip[8];
    vec4 splitFar[2];
    uvec4 metadata[2];
    vec4 texelWorldSize[2];
    vec4 depthSpanMeters[2];
    vec4 filterParameters[2];
    uvec4 filterMetadata[2];
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
}
