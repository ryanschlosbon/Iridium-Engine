#ifndef IRIDIUM_PACKED_MATERIAL_GLSL
#define IRIDIUM_PACKED_MATERIAL_GLSL

const uint MATERIAL_SCHEMA_VERSION = 2u;
const uint MATERIAL_TEXTURE_BASE_COLOR = 0u;
const uint MATERIAL_TEXTURE_METALLIC_ROUGHNESS = 1u;
const uint MATERIAL_TEXTURE_NORMAL = 2u;
const uint MATERIAL_TEXTURE_OCCLUSION = 3u;
const uint MATERIAL_TEXTURE_EMISSIVE = 4u;

struct PackedComplexLobe {
    uint type;
    uint textureMask;
    float parameters[6];
};

struct PackedMaterial {
    uint schemaVersion;
    uint closureClass;
    uint workflow;
    uint featureFlags;
    uint textureMask;
    uint alphaMode;
    uint doubleSided;
    uint complexLobeCount;
    vec4 baseColorFactor;
    vec4 metallicRoughnessIorSpecular;
    vec4 specularColorNormalScale;
    vec4 diffuseFactor;
    vec4 specularGlossinessFactorGloss;
    vec4 emissiveFactorStrength;
    vec4 surfaceParameters;
    PackedComplexLobe complexLobes[8];
    uvec4 textureUses[21];
    uint textureIndices[21];
    uint reserved[3];
};

vec2 packedMaterialUv(PackedMaterial material, uint semantic,
    vec2 uv0, vec2 uv1) {
    uvec4 use = material.textureUses[semantic];
    uint texCoord = (use.w >> 16u) & 0xffu;
    vec2 uv = texCoord == 1u ? uv1 : uv0;
    vec2 offset = unpackHalf2x16(use.x);
    vec2 scale = unpackHalf2x16(use.y);
    vec2 rotationScalar = unpackHalf2x16(use.z);
    float c = cos(rotationScalar.x);
    float s = sin(rotationScalar.x);
    vec2 scaled = uv * scale;
    return offset + mat2(c, s, -s, c) * scaled;
}

float packedMaterialScalar(PackedMaterial material, uint semantic) {
    return unpackHalf2x16(material.textureUses[semantic].z).y;
}

uint packedMaterialSamplerIndex(PackedMaterial material, uint semantic) {
    return material.textureUses[semantic].w & 0xffffu;
}

bool packedMaterialHasTexture(PackedMaterial material, uint semantic) {
    return (material.textureMask & (1u << semantic)) != 0u;
}

bool packedMaterialReconstructNormalZ(
    PackedMaterial material, uint semantic) {
    return (material.reserved[0] &
        (1u << semantic)) != 0u;
}

#endif
