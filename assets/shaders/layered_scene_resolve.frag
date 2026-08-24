#version 450

layout(set = 1, binding = 0) uniform sampler2D layeredLocalColor;
layout(set = 1, binding = 1) uniform usampler2D layeredEntryIdentity;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint padding0;
    uint padding1;
    uint padding2;
} push;

int iridiumLayeredUnpackSigned16(uint value) {
    return int((value & 0xffffu) ^ 0x8000u) - 0x8000;
}

void main() {
    bool mirrored = push.padding1 != 0u;
    if (gl_FrontFacing == mirrored)
        discard;

    ivec2 viewportOffset = ivec2(
        iridiumLayeredUnpackSigned16(push.padding2),
        iridiumLayeredUnpackSigned16(push.padding2 >> 16u));
    ivec2 atlasPixel = ivec2(gl_FragCoord.xy) - viewportOffset;
    ivec2 atlasExtent = textureSize(layeredLocalColor, 0);
    if (any(lessThan(atlasPixel, ivec2(0))) ||
        any(greaterThanEqual(atlasPixel, atlasExtent)))
        discard;

    uint entryIdentity = texelFetch(layeredEntryIdentity,
        atlasPixel, 0).r;
    uint expectedEntryIdentity = push.materialIndex + 1u;
    uint workMask = push.padding0 != 0u ? 0x00001fffu : 0x7fffffffu;
    if ((entryIdentity & workMask) != expectedEntryIdentity)
        discard;

    vec4 localColor = texelFetch(layeredLocalColor, atlasPixel, 0);
    if (!(localColor.a > 0.0) || any(isnan(localColor)) ||
        any(isinf(localColor)))
        discard;
    outColor = localColor;
}
