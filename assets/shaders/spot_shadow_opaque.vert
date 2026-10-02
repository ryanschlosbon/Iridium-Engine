#version 450

layout(location = 0) in vec3 inPosition;

struct SpotShadowEntry {
    mat4 worldToShadowClip;
    vec4 atlasScaleBias;
    uvec4 metadata;
    vec4 biasParameters;
    vec4 projectionParameters;
    uvec4 filterMetadata;
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
}
