#version 450

#extension GL_EXT_nonuniform_qualifier : require
#include "include/packed_material.glsl"

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord0;
layout(location = 2) in vec2 fragTexCoord1;

layout(std430, set = 1, binding = 0) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
layout(set = 1, binding = 1) uniform texture2D materialTextureViews[];
layout(set = 2, binding = 0) uniform sampler materialSamplers[];

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint cascadeIndex;
    uint padding1;
    uint padding2;
} push;

void main() {
    PackedMaterial material = materials[push.materialIndex];
    if (material.schemaVersion != MATERIAL_SCHEMA_VERSION) discard;
    float alpha = material.baseColorFactor.a * fragColor.a;
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_BASE_COLOR)) {
        uint semantic = MATERIAL_TEXTURE_BASE_COLOR;
        uint viewIndex = material.textureIndices[semantic];
        uint samplerIndex = packedMaterialSamplerIndex(material, semantic);
        vec2 uv = packedMaterialUv(material, semantic,
            fragTexCoord0, fragTexCoord1);
        alpha *= texture(sampler2D(
            materialTextureViews[nonuniformEXT(viewIndex)],
            materialSamplers[nonuniformEXT(samplerIndex)]), uv).a;
    }
    if (alpha < material.surfaceParameters.y) discard;
}
