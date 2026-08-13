#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec2 fragTexCoord1;

struct SpotShadowEntry {
    mat4 worldToShadowClip;
    vec4 atlasScaleBias;
    uvec4 metadata;
    vec4 biasParameters;
};

layout(std140, set = 0, binding = 0) uniform SpotShadowData {
    SpotShadowEntry entries[256];
    uvec4 metadata;
} shadowData;

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint shadowDataSlot;
    uint padding1;
    uint padding2;
} push;

void main() {
    gl_Position = shadowData.entries[push.shadowDataSlot].worldToShadowClip *
        push.renderMatrix * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
}
